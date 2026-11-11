#include "balboa_gs.h"

#ifdef USE_ARDUINO
#include <Arduino.h>
#endif

#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifndef IRAM_ATTR
#define IRAM_ATTR ICACHE_RAM_ATTR
#endif

namespace esphome {
namespace balboa_gs {

static const char *const TAG = "balboa_gs";

static const uint32_t NEW_CYCLE_US = 5000;       // gap that marks the start of a new clock burst
static const uint8_t DISPLAY_LAST_BIT = 38;      // display data is pulses 0..38
static const uint8_t TOTAL_BITS = 41;            // pulses 39..41 carry the button data
static const uint32_t TEMP_PRESS_GAP_MS = 500;   // time between simulated temperature presses
static const uint32_t LIGHT_SETTLE_MS = 3000;    // time for the panel to report a light press before re-syncing

// Mode handling. Economy / Sleep put "Ecn" / "5Lp" on the display regularly; Standard shows no mode
// text (only "5td" briefly when switching into it). A mode change is Temp+ then Mode.
static const uint32_t MODE_TEXT_WINDOW_MS = 30000;  // no mode text for this long = Standard
static const uint32_t MODE_READY_QUIET_MS = 4000;   // panel must not have shown the SET temp for this long before Temp+
static const uint32_t MODE_READY_TIMEOUT_MS = 30000;
static const uint32_t MODE_PRESS_GAP_MS = 1000;     // Temp+ -> Mode (manual: too-fast presses may not register)
static const uint32_t MODE_OBSERVE_MS = 4000;       // after Mode, read the display this long
static const uint32_t MODE_SETTLE_MS = 32000;       // no mode text after Mode: let passive detection decide
static const uint8_t MODE_MAX_ATTEMPTS = 3;

// Pump speed: the pump button cycles Off -> Low -> High -> Off; the panel only reports on/off.
static const uint32_t PUMP_PRESS_GAP_MS = 1000;
static const uint32_t PUMP_SETTLE_MS = 2500;
static const uint32_t PUMP2_PRESS_GAP_MS = 2500;  // EXPERIMENTAL: time for the panel to report pump 2
static const uint8_t PUMP2_MAX_PRESSES = 3;
static const uint32_t FILTER_PRESS_GAP_MS = 1000;   // Temp+ -> Pump 1, same gap as the mode change
static const uint32_t FILTER_SET_SHOW_MS = 3000;    // Temp+ must bring up the flashing set temperature within this
static const uint32_t FILTER_OBSERVE_MS = 6000;     // after Pump 1, wait this long for an F setting
static const uint32_t FILTER_EXIT_OBSERVE_MS = 5000;
static const uint32_t FILTER_TEST_GAP_MS = 1500;    // test run: time between Temp+ presses inside the menu
static const uint8_t FILTER_OPTIONS = 5;            // F2 F4 F6 F8 FC: 5 presses = back where it started
static const uint8_t FILTER_SET_MAX_PRESSES = 8;    // never more than this many Temp+ in the menu
static const uint32_t FILTER_SHOW_WAIT_MS = 4500;   // a setting must be readable within this after a press
static const char *const FILTER_OPTION_NAMES[FILTER_OPTIONS] = {"F2", "F4", "F6", "F8", "FC"};  // as the panel shows them
static const char *const FILTER_OPTION_LABELS[FILTER_OPTIONS] = {"2 hours", "4 hours", "6 hours", "8 hours", "Continuous"};
static const int FILTER_OPTION_HOURS[FILTER_OPTIONS] = {2, 4, 6, 8, 24};     // after a change, don't re-sync the list from the panel yet
static const uint8_t PUMP_OFF_MAX_PRESSES = 2;   // the tub can run Low itself and refuse Off; never cycle on to High

// Heater decode (bit 35). While heat is wanted but the heater isn't running, the panel blinks the heat
// indicator, so the bit toggles. Seen on this tub during frost protection, checked against supply power:
// blinking on-spells up to ~9 s and off-gaps up to ~12 s with the heater drawing nothing, while real
// heating is a steady on. So: on for 10 s without a change = Heating, off for 15 s = Off, anything
// in between = Pending. Real heating is reported 10 s late; the heater going off is reported at once.
static const uint32_t HEATER_ON_STEADY_MS = 10000;
static const uint32_t HEATER_OFF_STEADY_MS = 15000;

// 3-bit codes sent on pulses 39, 40, 41 (MSB first), indexed by ButtonId
static const uint8_t BUTTON_CODES[7] = {0b001, 0b100, 0b101, 0b011, 0b110, 0b010, 0b111};

// ---------------------------------------------------------------------------------------------
// ISR shared state
// ---------------------------------------------------------------------------------------------
uint8_t BalboaGS::isr_clock_pin_ = 0;
uint8_t BalboaGS::isr_data_pin_ = 0;
uint8_t BalboaGS::isr_button_pin_ = 0;
uint8_t BalboaGS::buffer_[40];
volatile uint8_t BalboaGS::pending_mask_ = 0;
volatile bool BalboaGS::buffer_ready_ = false;
volatile bool BalboaGS::overflow_ = false;
volatile uint32_t BalboaGS::last_edge_us_ = 0;
volatile uint8_t BalboaGS::bit_counter_ = 0;
volatile uint8_t BalboaGS::data_index_ = 0;
volatile uint8_t BalboaGS::active_button_ = 0xFF;
volatile bool BalboaGS::skip_frame_ = false;

// Same logic as the original library's clockPinInterrupt(), with two small changes:
//  * the 3-bit button code is latched once at pulse 39 (no mixed codes if a flag changes mid-frame)
//  * only the button that was actually sent is cleared, so a second queued press is not lost
void IRAM_ATTR BalboaGS::clock_isr() {
  // Everything here must run from IRAM: no Arduino/SDK calls that live in flash (detachInterrupt was one,
  // and crashed the ESP when a frame ended while settings were being written to flash).
  // GPIO is read and written through the registers directly.
  const uint32_t now = micros();
  if ((now - last_edge_us_) >= NEW_CYCLE_US) {  // new cycle detected
    data_index_ = 0;
    bit_counter_ = 0;
    active_button_ = 0xFF;
    // The previous frame is still waiting to be decoded: skip this one whole (never store half a frame)
    skip_frame_ = buffer_ready_;
  }
  last_edge_us_ = now;
  if (skip_frame_) return;

  if (((GPI >> isr_clock_pin_) & 1) == 0) {
    GPOC = (1u << isr_button_pin_);  // falling edge: button line low
    return;
  }

  // ---- rising edge ----
  const uint8_t bit = bit_counter_;

  // Write button data if requested
  if (bit >= 39 && bit <= TOTAL_BITS) {
    if (bit == 39) {
      active_button_ = 0xFF;
      const uint8_t mask = pending_mask_;
      for (uint8_t i = 0; i < 7; i++) {
        if (mask & (1 << i)) {
          active_button_ = i;
          break;
        }
      }
    }
    if (active_button_ != 0xFF) {
      if ((BUTTON_CODES[active_button_] >> (TOTAL_BITS - bit)) & 1) {
        GPOS = (1u << isr_button_pin_);
      } else {
        GPOC = (1u << isr_button_pin_);
      }
      if (bit == TOTAL_BITS) {
        pending_mask_ = pending_mask_ & (uint8_t) ~(1 << active_button_);
        active_button_ = 0xFF;
      }
    }
  }

  // Read display data
  if (bit <= DISPLAY_LAST_BIT) {
    const uint8_t idx = data_index_;
    if (idx < sizeof(buffer_)) {
      buffer_[idx] = (GPI >> isr_data_pin_) & 1;
      data_index_ = idx + 1;
    }
  } else if (bit == TOTAL_BITS) {  // total cycle has passed
    buffer_ready_ = true;          // further pulses are skipped until loop() has decoded it
  } else if (bit > TOTAL_BITS) {  // more pulses than a frame should have
    overflow_ = true;
  }

  if (bit < 255) {
    bit_counter_ = bit + 1;
  }
}

// ---------------------------------------------------------------------------------------------
// 7-segment lookup (same table as the original library)
// ---------------------------------------------------------------------------------------------
static char lookup_char(uint8_t p) {
  switch (p) {
    case 0b0000000: return ' ';
    case 0b1111110: return '0';
    case 0b0110000: return '1';
    case 0b1101101: return '2';
    case 0b1111001: return '3';
    case 0b0110011: return '4';
    case 0b1011011: return '5';
    case 0b1011111: return '6';
    case 0b1110000: return '7';
    case 0b1111111: return '8';
    case 0b1110011: return '9';
    case 0b1110111: return 'A';
    case 0b1001110: return 'C';
    case 0b1001111: return 'E';
    case 0b1011110: return 'G';
    case 0b0110111: return 'H';
    case 0b0111100: return 'J';
    case 0b0001110: return 'L';
    case 0b1010100: return 'M';
    case 0b1110110: return 'N';
    case 0b1101011: return 'Q';
    case 0b1100110: return 'R';
    case 0b0111110: return 'U';
    case 0b0101010: return 'W';
    case 0b1111101: return 'a';
    case 0b0011111: return 'b';
    case 0b0001101: return 'c';
    case 0b0111101: return 'd';
    case 0b1101111: return 'e';
    case 0b1000111: return 'f';
    case 0b0010111: return 'h';
    case 0b0000100: return 'i';
    case 0b0000001: return 'j';
    case 0b1010111: return 'k';
    case 0b0000110: return 'l';
    case 0b0010100: return 'm';
    case 0b0010101: return 'n';
    case 0b0011101: return 'o';
    case 0b1100111: return 'p';
    case 0b0000101: return 'r';
    case 0b0001111: return 't';
    case 0b0011100: return 'u';
    case 0b0111011: return 'y';
    default: return '-';
  }
}

static inline bool is_digit_char(char c) { return c >= '0' && c <= '9'; }

// ---------------------------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------------------------
void BalboaGS::setup() {
  isr_clock_pin_ = this->clock_pin_;
  isr_data_pin_ = this->data_pin_;
  isr_button_pin_ = this->button_pin_;

  pinMode(this->clock_pin_, INPUT);
  pinMode(this->data_pin_, INPUT);
  pinMode(this->button_pin_, OUTPUT);
  digitalWrite(this->button_pin_, LOW);

  pending_mask_ = 0;
  buffer_ready_ = false;
  bit_counter_ = 0;
  data_index_ = 0;
  active_button_ = 0xFF;

  if (this->climate_ != nullptr) {
    this->climate_->mode = climate::CLIMATE_MODE_HEAT;  // the controller is always in "heat"
  }

  // Last water temperature from flash. In Sleep/Economy the panel only shows the water
  // temperature while the pump runs, so without this a restart leaves it unknown for hours.
  this->water_pref_ = global_preferences->make_preference<float>(fnv1_hash("balboa_gs_water_c"));
  float stored = NAN;
  if (this->water_pref_.load(&stored) && plausible_(stored)) {
    this->water_c_ = stored;
    this->saved_water_c_ = stored;
    this->have_valid_water_ = true;
    ESP_LOGI(TAG, "Restored last water temperature %.1f C from flash", stored);
  }

  // Filter cycle hours, last read from the panel
  this->filter_pref_ = global_preferences->make_preference<uint8_t>(fnv1_hash("balboa_gs_filter_h"));
  uint8_t fh = 0;
  if (this->filter_pref_.load(&fh) && (fh == 2 || fh == 4 || fh == 6 || fh == 8 || fh == 24)) {
    this->filter_hours_ = fh;
  }

  if (this->power_sensor_ != nullptr) {
    this->power_sensor_->add_on_state_callback([this](float w) { this->on_power_(w); });
  }

  attachInterrupt(this->clock_pin_, BalboaGS::clock_isr, CHANGE);
  this->log_action_("Booted");
}

void BalboaGS::dump_config() {
  ESP_LOGCONFIG(TAG, "Balboa GS510SZ spa controller:");
  ESP_LOGCONFIG(TAG, "  Clock pin: GPIO%u", this->clock_pin_);
  ESP_LOGCONFIG(TAG, "  Display data pin: GPIO%u", this->data_pin_);
  ESP_LOGCONFIG(TAG, "  Button data pin: GPIO%u", this->button_pin_);
  ESP_LOGCONFIG(TAG, "  Target range: %.1f - %.1f C", this->min_target_, this->max_target_);
  ESP_LOGCONFIG(TAG, "  Learn target on boot: %s", this->learn_on_boot_ ? "yes" : "no");
}

void BalboaGS::loop() {
  if (buffer_ready_) {
    this->decode_frame_();
    buffer_ready_ = false;  // the interrupt stays attached; it resumes at the next whole frame
  }

  this->detect_mode_();
  this->process_mode_change_();
  this->process_filter_read_();
  this->process_pump_();
  this->process_pump2_();
  this->process_temp_presses_();
  this->learn_target_();
  this->publish_states_();
  this->publish_last_action_();
}

void BalboaGS::stop_interface_() {
  this->interface_stopped_ = true;
  detachInterrupt(digitalPinToInterrupt(this->clock_pin_));
  digitalWrite(this->button_pin_, LOW);
  pending_mask_ = 0;
}

// ---------------------------------------------------------------------------------------------
// Frame decoding
// ---------------------------------------------------------------------------------------------
void BalboaGS::decode_frame_() {
  uint8_t seg[4] = {0, 0, 0, 0};
  for (uint8_t x = 0; x <= 27; x++) {
    seg[x / 7] = (uint8_t)((seg[x / 7] << 1) | (buffer_[x] == 1 ? 1 : 0));
  }

  this->display_button_ = buffer_[28] == 1;
  const bool heat_bit = buffer_[35] == 1;
  if (!this->heater_bit_seen_) {
    // First frame: treat the bit as already steady, so a boot doesn't show Pending for 10-15 s
    this->heater_bit_change_ms_ = millis() - HEATER_OFF_STEADY_MS;
    this->heater_bit_seen_ = true;
  } else if (heat_bit != this->heater_on_) {
    this->heater_bit_change_ms_ = millis();
  }
  this->heater_on_ = heat_bit;
  this->pump1_on_ = buffer_[36] == 1;
  if (this->pump1_on_) this->last_pump_on_ms_ = millis();
  this->pump2_on_ = buffer_[37] == 1;
  if (this->pump2_on_) this->last_pump2_on_ms_ = millis();
  this->light_on_ = buffer_[38] == 1;

  // Raw status bits 29..38, for working out the undocumented ones (mode, pump speed)
  uint16_t bits = 0;
  for (uint8_t b = 29; b <= 38; b++) {
    if (buffer_[b] == 1) bits |= (uint16_t) (1u << (b - 29));
  }
  this->status_bits_ = bits;

  const char c0 = lookup_char(seg[0]);
  const char c1 = lookup_char(seg[1]);
  const char c2 = lookup_char(seg[2]);
  const char c3 = lookup_char(seg[3]);

  if (is_digit_char(c0) && is_digit_char(c1) && is_digit_char(c2) && c3 == 'C') {
    // A temperature is shown, e.g. "37.5C"
    const float t = 10.0f * (c0 - '0') + (c1 - '0') + 0.1f * (c2 - '0');
    snprintf(this->text_, sizeof(this->text_), "%c%c.%c%c", c0, c1, c2, c3);

    if (this->display_button_) {
      // The panel shows the SET temperature while a temperature button is active
      if (plausible_(t)) {
        this->target_c_ = t;
        this->target_known_ = true;
      }
    } else if (plausible_(t)) {
      this->water_c_ = t;
      this->have_valid_water_ = true;
      this->water_live_ = true;
      this->last_valid_water_ms_ = millis();
      // Only write when the reading changes (0.5 C steps), to spare the flash
      if (!(std::fabs(t - this->saved_water_c_) < 0.05f)) {
        this->water_pref_.save(&t);
        this->saved_water_c_ = t;
      }
    }
  } else {
    // Something else is shown (dashes, error codes, mode names...)
    snprintf(this->text_, sizeof(this->text_), "%c%c%c%c", c0, c1, c2, c3);
  }

  const uint32_t now = millis();
  if (!this->have_frame_) this->first_frame_ms_ = now;
  if (this->display_button_) this->last_set_display_ms_ = now;
  if (strstr(this->text_, "5td") != nullptr) this->mode_text_ms_[MODE_STANDARD] = now;
  if (strstr(this->text_, "Ecn") != nullptr) this->mode_text_ms_[MODE_ECONOMY] = now;
  if (strstr(this->text_, "5Lp") != nullptr) this->mode_text_ms_[MODE_SLEEP] = now;

  this->have_frame_ = true;
}

// ---------------------------------------------------------------------------------------------
// Presses
// ---------------------------------------------------------------------------------------------
void BalboaGS::queue_press_(ButtonId id) {
  InterruptLock lock;
  pending_mask_ = pending_mask_ | (uint8_t)(1 << id);
}

void BalboaGS::command_button(uint8_t action) {
  switch (action) {
    case ACTION_MODE: this->queue_press_(BTN_MODE); this->log_action_("Mode pressed"); break;
    case ACTION_TEMP_UP: this->queue_press_(BTN_TEMP_UP); this->log_action_("Temperature Up pressed"); break;
    case ACTION_TEMP_DOWN: this->queue_press_(BTN_TEMP_DOWN); this->log_action_("Temperature Down pressed"); break;
    case ACTION_LIGHT: this->queue_press_(BTN_LIGHT); this->log_action_("Light pressed"); break;
    case ACTION_PUMP: this->queue_press_(BTN_PUMP1); this->log_action_("Pump pressed"); break;
    case ACTION_READ_FILTER: this->command_read_filter(false); break;
    case ACTION_PUMP2: this->queue_press_(BTN_PUMP2); this->log_action_("Pump 2 pressed (experimental)"); break;
    case ACTION_PUMP3: this->queue_press_(BTN_PUMP3); this->log_action_("Pump 3 pressed (experimental)"); break;
    case ACTION_TEST_FILTER: this->command_read_filter(true); break;
    case ACTION_STOP:
      this->stop_interface_();
      this->log_action_("Stop command: panel interface stopped (reboot to resume)");
      break;
  }
}

// Light entity command: press Light only if the panel is in the other state.
void BalboaGS::command_light(bool on) {
  if (!this->have_frame_ || this->interface_stopped_) {
    // Nothing read from the panel yet (e.g. a restored state at boot): don't press,
    // sync_light_() will show the real state once the panel has been read.
    return;
  }
  if (on == this->light_on_) {
    this->log_action_("Light -> %s (already %s, no press)", on ? "on" : "off", on ? "on" : "off");
    return;
  }
  if (pending_mask_ & (1 << BTN_LIGHT)) return;  // a light press is already queued

  this->queue_press_(BTN_LIGHT);
  this->light_settle_until_ = millis() + LIGHT_SETTLE_MS;
  this->log_action_("Light -> %s (pressed)", on ? "on" : "off");
}

// Keep the light entity matching the panel (physical button, 4 h auto-off, or a press that didn't take).
void BalboaGS::sync_light_() {
  if (this->light_output_ == nullptr) return;
  light::LightState *state = this->light_output_->get_state();
  if (state == nullptr) return;
  if (!elapsed_(this->light_settle_until_)) return;
  if (state->current_values.is_on() == this->light_on_) return;

  // write_state() runs again from perform(); it sees the panel already matches and does not press
  this->log_action_("Light re-synced to %s (panel)", this->light_on_ ? "on" : "off");
  auto call = state->make_call();
  call.set_state(this->light_on_);
  call.set_transition_length(0);
  call.perform();
}

// Same idea as the original library's updateTemperature(): the first press only wakes the panel's
// set-temperature display, every press after that moves the target by 0.5 C.
void BalboaGS::command_target_temperature(float target) {
  if (!this->target_known_) {
    this->log_action_("Target %.1f C ignored: panel target not known yet", target);
    return;
  }
  if (this->mode_change_active_() || this->filter_read_active_()) {
    this->log_action_("Target %.1f C ignored: mode change or filter read in progress", target);
    return;
  }
  if (target < this->min_target_) target = this->min_target_;
  if (target > this->max_target_) target = this->max_target_;
  target = roundf(target * 2.0f) / 2.0f;

  const float diff = target - this->target_c_;
  if (fabsf(diff) < 0.25f) return;

  this->temp_dir_ = diff < 0 ? BTN_TEMP_DOWN : BTN_TEMP_UP;
  this->temp_presses_ = 1 + (int) (fabsf(diff) * 2.0f + 0.5f);
  this->temp_next_press_ms_ = 0;
  this->temp_hold_until_ = millis() + 4000;
  this->log_action_("Target %.1f C requested (%d presses)", target, this->temp_presses_);
}

void BalboaGS::process_temp_presses_() {
  if (this->temp_presses_ <= 0) return;
  if (!elapsed_(this->temp_next_press_ms_)) return;
  if (this->any_press_pending_()) return;

  this->queue_press_(this->temp_dir_);
  this->temp_presses_--;
  this->temp_next_press_ms_ = millis() + TEMP_PRESS_GAP_MS;
  this->temp_hold_until_ = millis() + 4000;
}

// The original library did this too: until the set temperature is known, press "Temp Up" once so the
// panel displays it. Rate limited so it never double-presses.
void BalboaGS::learn_target_() {
  if (!this->learn_on_boot_ || this->target_known_ || this->interface_stopped_) return;
  if (this->mode_change_active_() || this->filter_read_active_()) return;
  if (!this->have_frame_ || millis() < 5000) return;
  if (!elapsed_(this->learn_next_ms_) || this->any_press_pending_()) return;

  this->queue_press_(BTN_TEMP_UP);
  this->learn_next_ms_ = millis() + 10000;
  this->log_action_("Reading target temperature (Temp Up pressed)");
}

// ---------------------------------------------------------------------------------------------
// Filter setting (read only for now)
// Manual: Temp+ (set temperature flashes), Pump 1 within the window shows the setting (F2, F4, F6,
// F8, FC / Filc), Temp+/- steps through them, Pump 1 again stores it and exits. Reading = open the
// menu, note what is shown, press Pump 1 to exit without stepping, so the setting is unchanged.
// ---------------------------------------------------------------------------------------------
// 2/4/6/8 hours, 24 for continuous, -1 if the text isn't a filter setting
int BalboaGS::parse_filter_text_(const char *text) {
  const char *f = strchr(text, 'f');  // the 7-segment F decodes as 'f'
  if (f == nullptr) f = strchr(text, 'F');
  if (f == nullptr) return -1;
  const char *p = f + 1;
  while (*p == ' ') p++;
  // The panel spells it "FIL2": the I decodes as '1' (or 'i'), then L
  if (*p == '1' || *p == 'i' || *p == 'I') {
    p++;
    if (*p == 'L' || *p == 'l') p++;
    else return -1;
  }
  switch (*p) {
    case '2': return 2;
    case '4': return 4;
    case '6': return 6;
    case '8': return 8;
    case 'C': case 'c': return 24;
    default: return -1;
  }
}

int BalboaGS::filter_index_(int hours) {
  for (int i = 0; i < FILTER_OPTIONS; i++) {
    if (FILTER_OPTION_HOURS[i] == hours) return i;
  }
  return -1;
}

void BalboaGS::command_read_filter(bool test, int target_hours) {
  if (this->interface_stopped_) {
    this->log_action_("Filter read ignored: interface stopped");
    return;
  }
  if (this->filter_read_active_() || this->mode_change_active_() || this->pump_target_ >= 0) {
    this->log_action_("Filter read ignored: another change is in progress");
    this->lp_filter_hours_ = -1;  // put the list back to the real setting
    return;
  }
  this->filter_step_ = FR_WAIT_READY;
  this->filter_step_ms_ = millis();
  this->filter_found_ = -1;
  this->filter_seen_[0] = '\0';
  this->filter_last_text_[0] = '\0';
  this->filter_test_ = test;
  this->filter_test_left_ = 0;
  this->filter_target_ = target_hours;
  this->filter_set_presses_ = 0;
  this->filter_shown_ = -1;
  if (target_hours > 0) {
    this->log_action_("Filter -> %s requested (now %s)", FILTER_OPTION_NAMES[filter_index_(target_hours)],
                      this->filter_hours_ ? FILTER_OPTION_NAMES[filter_index_(this->filter_hours_)] : "unknown");
  } else {
    this->log_action_(test ? "Filter menu test requested" : "Filter read requested");
  }
}

// Keep every distinct display text seen during the read (shown in Last Action and the log)
void BalboaGS::note_filter_text_() {
  if (strcmp(this->text_, this->filter_last_text_) == 0) return;
  snprintf(this->filter_last_text_, sizeof(this->filter_last_text_), "%s", this->text_);
  ESP_LOGI(TAG, "Filter read: display '%s'", this->text_);
  const size_t len = strlen(this->filter_seen_);
  if (len + strlen(this->text_) + 3 < sizeof(this->filter_seen_)) {
    snprintf(this->filter_seen_ + len, sizeof(this->filter_seen_) - len, "[%s]", this->text_);
  }
}

void BalboaGS::finish_filter_read_(bool ok, const char *why) {
  if (this->filter_test_ && ok) {
    ESP_LOGI(TAG, "Filter test display sequence: %s", this->filter_seen_);
  }
  if (ok && this->filter_found_ > 0) {
    if (this->filter_hours_ != this->filter_found_) {
      this->filter_hours_ = (uint8_t) this->filter_found_;
      this->filter_pref_.save(&this->filter_hours_);
    }
    const char *name = FILTER_OPTION_NAMES[filter_index_(this->filter_found_)];
    if (this->filter_target_ > 0) {
      this->log_action_("Filter -> %s %s (%u presses)", name, this->filter_found_ == this->filter_target_ ? "done" : "NOT reached, stored",
                        (unsigned) this->filter_set_presses_);
    } else {
      this->log_action_("Filter read: %s", name);
    }
    ESP_LOGI(TAG, "Filter display sequence: %s", this->filter_seen_);
  } else {
    this->log_action_("Filter read failed: %s. Display %s", why, this->filter_seen_);
  }
  this->filter_step_ = FR_IDLE;
  this->filter_target_ = -1;
  this->lp_filter_hours_ = -1;  // re-publish the list with the real setting
}

void BalboaGS::process_filter_read_() {
  if (this->filter_step_ == FR_IDLE) return;
  const uint32_t now = millis();
  if (this->filter_step_ != FR_WAIT_READY) {
    this->note_filter_text_();
    const int shown = parse_filter_text_(this->text_);
    if (shown > 0) {
      this->filter_shown_ = shown;
      this->filter_shown_ms_ = now;
    }
  }

  switch (this->filter_step_) {
    case FR_WAIT_READY: {
      // Same rule as the mode change: never send Temp+ while the set temperature is showing
      const bool ready = this->have_frame_ && !this->any_press_pending_() && this->temp_presses_ == 0 &&
                         elapsed_(this->temp_hold_until_) && !this->display_button_ &&
                         (now - this->last_set_display_ms_) >= MODE_READY_QUIET_MS;
      if (!ready) {
        if (now - this->filter_step_ms_ > MODE_READY_TIMEOUT_MS) this->finish_filter_read_(false, "panel busy");
        return;
      }
      this->filter_pump_before_ = this->pump1_on_;
      this->queue_press_(BTN_TEMP_UP);
      this->filter_step_ = FR_TEMP_SENT;
      this->filter_step_ms_ = now;
      this->log_action_("Filter read: Temp+ pressed");
      return;
    }

    case FR_TEMP_SENT: {
      if (this->any_press_pending_()) return;
      if (now - this->filter_step_ms_ < FILTER_PRESS_GAP_MS) return;
      // Only press Pump 1 if Temp+ really opened the flashing set temperature (else it just toggles the pump)
      const bool set_showing = (int32_t) (this->last_set_display_ms_ - this->filter_step_ms_) >= 0;
      if (!set_showing) {
        if (now - this->filter_step_ms_ > FILTER_SET_SHOW_MS) {
          this->finish_filter_read_(false, "set temperature never showed after Temp+, Pump 1 not pressed");
        }
        return;
      }
      this->queue_press_(BTN_PUMP1);
      this->filter_step_ = FR_PUMP_SENT;
      this->filter_step_ms_ = now;
      return;
    }

    case FR_PUMP_SENT: {
      if (this->any_press_pending_()) return;
      if (this->filter_test_) {
        const int seen = parse_filter_text_(this->text_);
        if (seen > 0 && this->filter_found_ < 0) this->filter_found_ = seen;
        if (now - this->filter_step_ms_ < FILTER_TEST_GAP_MS) return;
        if (this->pump1_on_ != this->filter_pump_before_) {
          this->finish_filter_read_(false, "menu didn't open, Pump 1 toggled the pump. No Temp+ sent");
          return;
        }
        if (this->showing_temperature_()) {
          this->finish_filter_read_(false, "display shows a temperature, menu not open. No Temp+ sent");
          return;
        }
        this->filter_test_left_ = FILTER_OPTIONS;
        this->filter_step_ = FR_TEST_STEPPING;
        this->filter_step_ms_ = now - FILTER_TEST_GAP_MS;  // first Temp+ straight away
        return;
      }
      const int found = parse_filter_text_(this->text_);
      if (found > 0) {
        this->filter_found_ = found;
        if (this->filter_target_ > 0 && found != this->filter_target_) {
          this->filter_step_ = FR_SET_STEPPING;
          this->filter_step_ms_ = now - FILTER_TEST_GAP_MS;  // first Temp+ straight away
          return;
        }
        this->queue_press_(BTN_PUMP1);  // store (unchanged) and exit
        this->filter_step_ = FR_EXIT_SENT;
        this->filter_step_ms_ = now;
        return;
      }
      if (now - this->filter_step_ms_ < FILTER_OBSERVE_MS) return;
      if (this->pump1_on_ != this->filter_pump_before_) {
        // The pump changed instead: the menu never opened. Don't press again.
        this->finish_filter_read_(false, "menu didn't open, Pump 1 toggled the pump");
        return;
      }
      // Pump unchanged, so the press went somewhere: assume the menu is open and exit it
      this->log_action_("Filter read: no F setting recognised, pressing Pump 1 to exit");
      this->queue_press_(BTN_PUMP1);
      this->filter_step_ = FR_EXIT_SENT;
      this->filter_step_ms_ = now;
      return;
    }

    case FR_TEST_STEPPING: {
      if (this->any_press_pending_()) return;
      if (now - this->filter_step_ms_ < FILTER_TEST_GAP_MS) return;
      // Every press is guarded: if the menu has closed, Temp+ would change the set temperature
      if (this->showing_temperature_() || this->pump1_on_ != this->filter_pump_before_) {
        this->finish_filter_read_(false, "menu closed during the test, stopped (setting may have moved, check panel)");
        return;
      }
      const size_t len = strlen(this->filter_seen_);
      if (this->filter_test_left_ > 0) {
        const unsigned n = FILTER_OPTIONS - this->filter_test_left_ + 1;
        if (len + 4 < sizeof(this->filter_seen_)) snprintf(this->filter_seen_ + len, sizeof(this->filter_seen_) - len, " +%u", n);
        ESP_LOGI(TAG, "Filter test: Temp+ %u of %u", n, (unsigned) FILTER_OPTIONS);
        this->filter_last_text_[0] = '\0';  // record the display again after each press
        this->queue_press_(BTN_TEMP_UP);
        this->filter_test_left_--;
        this->filter_step_ms_ = now;
        return;
      }
      if (len + 4 < sizeof(this->filter_seen_)) snprintf(this->filter_seen_ + len, sizeof(this->filter_seen_) - len, " P1");
      this->queue_press_(BTN_PUMP1);  // store (back at the starting setting) and exit
      this->filter_step_ = FR_EXIT_SENT;
      this->filter_step_ms_ = now;
      return;
    }

    case FR_SET_STEPPING: {
      // Closed loop: press Temp+ only after the display has shown the result of the last press
      if (this->any_press_pending_()) return;
      if (now - this->filter_step_ms_ < FILTER_TEST_GAP_MS) return;
      if (this->showing_temperature_() || this->pump1_on_ != this->filter_pump_before_) {
        this->finish_filter_read_(false, "menu closed while stepping, stopped (check the panel)");
        return;
      }
      const bool fresh = (int32_t) (this->filter_shown_ms_ - (this->filter_step_ms_ + 300)) >= 0 ||
                         this->filter_set_presses_ == 0;
      if (!fresh) {
        if (now - this->filter_step_ms_ > FILTER_SHOW_WAIT_MS) {
          this->finish_filter_read_(false, "setting not readable after Temp+, stopped (check the panel)");
        }
        return;
      }
      if (this->filter_shown_ == this->filter_target_) {
        this->filter_found_ = this->filter_shown_;
        this->queue_press_(BTN_PUMP1);  // store the new setting and exit
        this->filter_step_ = FR_EXIT_SENT;
        this->filter_step_ms_ = now;
        return;
      }
      if (this->filter_set_presses_ >= FILTER_SET_MAX_PRESSES) {
        // Leave it where it is: Pump 1 stores what is shown, which is reported back
        this->filter_found_ = this->filter_shown_;
        this->log_action_("Filter: target not reached after %u presses, storing what is shown",
                          (unsigned) this->filter_set_presses_);
        this->queue_press_(BTN_PUMP1);
        this->filter_step_ = FR_EXIT_SENT;
        this->filter_step_ms_ = now;
        return;
      }
      this->queue_press_(BTN_TEMP_UP);
      this->filter_set_presses_++;
      this->filter_step_ms_ = now;
      return;
    }

    case FR_EXIT_SENT:
      if (this->any_press_pending_()) return;
      if (now - this->filter_step_ms_ < FILTER_EXIT_OBSERVE_MS) return;
      if (this->pump1_on_ != this->filter_pump_before_) {
        this->finish_filter_read_(false, "pump state changed during the read, check the panel");
      } else if (this->filter_found_ > 0) {
        this->finish_filter_read_(true, "");
      } else {
        this->finish_filter_read_(false, "no F setting recognised");
      }
      return;

    default:
      this->filter_step_ = FR_IDLE;
      return;
  }
}

// ---------------------------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------------------------
const char *BalboaGS::mode_name(SpaMode m) {
  switch (m) {
    case MODE_STANDARD: return "Standard";
    case MODE_ECONOMY: return "Economy";
    case MODE_SLEEP: return "Sleep";
    default: return "Unknown";
  }
}

// Newest mode text shown on the display at or after `since` (MODE_UNKNOWN if none)
SpaMode BalboaGS::mode_seen_since_(uint32_t since) const {
  SpaMode seen = MODE_UNKNOWN;
  uint32_t newest = 0;
  for (uint8_t m = MODE_STANDARD; m <= MODE_SLEEP; m++) {
    const uint32_t t = this->mode_text_ms_[m];
    if (t != 0 && (int32_t) (t - since) >= 0 && (seen == MODE_UNKNOWN || (int32_t) (t - newest) >= 0)) {
      seen = (SpaMode) m;
      newest = t;
    }
  }
  return seen;
}

// Passive: the newest mode text in the last 30 s wins; no mode text for 30 s means Standard.
void BalboaGS::detect_mode_() {
  if (!this->have_frame_) return;
  const uint32_t now = millis();
  const uint32_t since = now > MODE_TEXT_WINDOW_MS ? now - MODE_TEXT_WINDOW_MS : 1;
  const SpaMode seen = this->mode_seen_since_(since);
  if (seen != MODE_UNKNOWN) {
    this->mode_ = seen;
  } else if (now - this->first_frame_ms_ >= MODE_TEXT_WINDOW_MS) {
    this->mode_ = MODE_STANDARD;
  }
}

void BalboaGS::command_mode(SpaMode target) {
  if (target == MODE_UNKNOWN) return;
  if (this->interface_stopped_) {
    this->log_action_("Mode -> %s ignored: interface stopped", mode_name(target));
    return;
  }
  if (this->filter_read_active_()) {
    this->log_action_("Mode -> %s ignored: filter read in progress", mode_name(target));
    this->lp_mode_ = MODE_UNKNOWN;
    return;
  }
  if (!this->mode_change_active_() && target == this->mode_) {
    this->log_action_("Mode -> %s (already %s, no press)", mode_name(target), mode_name(target));
    return;
  }
  this->mode_target_ = target;
  this->mode_attempts_ = 0;
  this->mode_step_ = MS_WAIT_READY;
  this->mode_step_ms_ = millis();
  this->log_action_("Mode -> %s requested (now %s)", mode_name(target), mode_name(this->mode_));
}

void BalboaGS::finish_mode_change_(bool ok) {
  if (ok) {
    this->log_action_("Mode -> %s done (%u cycles)", mode_name(this->mode_target_), (unsigned) this->mode_attempts_);
  } else {
    this->log_action_("Mode -> %s gave up after %u cycles, panel shows %s", mode_name(this->mode_target_),
                      (unsigned) this->mode_attempts_, mode_name(this->mode_));
  }
  this->mode_step_ = MS_IDLE;
  this->lp_mode_ = MODE_UNKNOWN;  // re-publish the real mode
}

// One cycle = Temp+ (only shows the set temperature) then Mode (changes the mode), then read the display.
void BalboaGS::process_mode_change_() {
  if (this->mode_step_ == MS_IDLE) return;
  const uint32_t now = millis();

  switch (this->mode_step_) {
    case MS_WAIT_READY: {
      if (this->mode_attempts_ > 0 && this->mode_ == this->mode_target_) {
        this->finish_mode_change_(true);
        return;
      }
      if (this->mode_attempts_ >= MODE_MAX_ATTEMPTS) {
        this->finish_mode_change_(false);
        return;
      }
      // Never send Temp+ while the panel is showing the set temperature: a second press would change it
      const bool ready = this->have_frame_ && !this->any_press_pending_() && this->temp_presses_ == 0 &&
                         elapsed_(this->temp_hold_until_) && !this->display_button_ &&
                         (now - this->last_set_display_ms_) >= MODE_READY_QUIET_MS;
      if (!ready) {
        if (now - this->mode_step_ms_ > MODE_READY_TIMEOUT_MS) {
          this->log_action_("Mode -> %s gave up: panel busy", mode_name(this->mode_target_));
          this->mode_step_ = MS_IDLE;
          this->lp_mode_ = MODE_UNKNOWN;
        }
        return;
      }
      this->mode_attempts_++;
      this->queue_press_(BTN_TEMP_UP);
      this->mode_step_ = MS_TEMP_SENT;
      this->mode_step_ms_ = now;
      this->log_action_("Mode -> %s: cycle %u (Temp+ then Mode)", mode_name(this->mode_target_),
                        (unsigned) this->mode_attempts_);
      return;
    }

    case MS_TEMP_SENT:
      if (this->any_press_pending_()) return;  // Temp+ not sent yet
      if (now - this->mode_step_ms_ < MODE_PRESS_GAP_MS) return;
      this->queue_press_(BTN_MODE);
      this->mode_step_ = MS_MODE_SENT;
      this->mode_step_ms_ = now;
      return;

    case MS_MODE_SENT: {
      if (this->any_press_pending_()) return;  // Mode not sent yet
      if (now - this->mode_step_ms_ < MODE_OBSERVE_MS) return;
      const SpaMode result = this->mode_seen_since_(this->mode_step_ms_);
      if (result != MODE_UNKNOWN) {
        this->mode_ = result;
        if (result == this->mode_target_) {
          this->finish_mode_change_(true);
        } else {
          this->mode_step_ = MS_WAIT_READY;  // not there yet, go round again
          this->mode_step_ms_ = now;
        }
      } else {
        this->mode_step_ = MS_SETTLE;  // no mode text: let passive detection decide
        this->mode_step_ms_ = now;
      }
      return;
    }

    case MS_SETTLE:
      if (now - this->mode_step_ms_ < MODE_SETTLE_MS) return;
      if (this->mode_ == this->mode_target_) {
        this->finish_mode_change_(true);
      } else {
        this->mode_step_ = MS_WAIT_READY;
        this->mode_step_ms_ = now;
      }
      return;

    default:
      this->mode_step_ = MS_IDLE;
      return;
  }
}

// ---------------------------------------------------------------------------------------------
// Pump speed (tracked)
// ---------------------------------------------------------------------------------------------
const char *BalboaGS::pump_name(PumpSpeed p) {
  switch (p) {
    case PUMP_LOW: return "Low";
    case PUMP_HIGH: return "High";
    default: return "Off";
  }
}

// Anchor the guess to what the panel reports: off is off; on from off means Low (also when the tub starts it).
void BalboaGS::sync_pump_assumed_() {
  if (this->pump_is_off_()) {
    this->pump_assumed_ = PUMP_OFF;
  } else if (this->pump1_on_ && this->pump_assumed_ == PUMP_OFF) {
    this->pump_assumed_ = PUMP_LOW;
  }
}

void BalboaGS::command_pump(PumpSpeed target) {
  if (this->interface_stopped_) {
    this->log_action_("Pump -> %s ignored: interface stopped", pump_name(target));
    return;
  }
  this->sync_pump_assumed_();
  if (this->pump_target_ < 0 && target == this->pump_assumed_) {
    this->log_action_("Pump -> %s (already %s, no press)", pump_name(target), pump_name(target));
    return;
  }
  this->pump_target_ = target;
  this->pump_attempts_ = 0;
  this->pump_next_press_ms_ = 0;
  this->pump_presses_left_ = target == PUMP_OFF ? 0 : (uint8_t) ((target - this->pump_assumed_ + 3) % 3);
  if (target == PUMP_OFF) {
    this->log_action_("Pump -> Off requested (now %s)", pump_name(this->pump_assumed_));
  } else {
    this->log_action_("Pump -> %s requested (now %s, %u presses)", pump_name(target), pump_name(this->pump_assumed_),
                      (unsigned) this->pump_presses_left_);
  }
}

void BalboaGS::finish_pump_(PumpSpeed result, bool ok) {
  this->pump_target_ = -1;
  if (ok && result != PUMP_OFF && !this->pump1_on_) {
    this->pump_assumed_ = PUMP_OFF;
    this->log_action_("Pump -> %s: pump not running after %u presses", pump_name(result), (unsigned) this->pump_attempts_);
  } else if (ok) {
    this->pump_assumed_ = result;
    this->log_action_("Pump -> %s done (%u presses)", pump_name(result), (unsigned) this->pump_attempts_);
  } else {
    this->pump_assumed_ = result;
    this->log_action_("Pump -> Off: still running after %u presses (tub is running it), shown as %s",
                      (unsigned) this->pump_attempts_, pump_name(result));
  }
  this->pump_settle_until_ = millis() + PUMP_SETTLE_MS;
  this->lp_pump_speed_ = -1;  // re-publish
}

void BalboaGS::process_pump_() {
  if (this->pump_target_ < 0) return;
  if (this->mode_change_active_() || this->filter_read_active_()) return;  // don't interleave with Temp+ / Mode
  if (!elapsed_(this->pump_next_press_ms_)) return;
  if (this->any_press_pending_()) return;

  if (this->pump_target_ == PUMP_OFF) {
    // Closed loop: press until the panel says off, but never more than twice
    if (this->pump_is_off_()) {
      this->finish_pump_(PUMP_OFF, true);
      return;
    }
    if (this->pump_attempts_ >= PUMP_OFF_MAX_PRESSES) {
      this->finish_pump_(PUMP_LOW, false);
      return;
    }
    // give the panel time to report off before deciding another press is needed
    if (this->pump_attempts_ > 0 && !this->pump1_on_) return;
  } else {
    if (this->pump_presses_left_ == 0) {
      this->finish_pump_((PumpSpeed) this->pump_target_, true);
      return;
    }
    this->pump_presses_left_--;
  }

  this->queue_press_(BTN_PUMP1);
  this->pump_attempts_++;
  this->pump_next_press_ms_ = millis() + PUMP_PRESS_GAP_MS;
}

// Power check on the tracked pump speed. Measured on this tub: Low ~390 W, High ~1950 W (the heater is
// locked out on High, 13 A tub), Low + heater ~2270 W. So between the two thresholds = High,
// below = Low, above = Low with the heater on. The panel's pump bit stays the source for on/off.
// Two readings in a row must agree before the tracked speed is corrected (heater switching mid-sample).
void BalboaGS::on_power_(float watts) {
  if (std::isnan(watts) || !this->have_frame_) return;

  // What the power alone says, for the "Pump Speed (Power)" diagnostic. Published only when it changes.
  if (this->power_check_sensor_ != nullptr) {
    int8_t cls = 0;
    if (watts >= 100.0f) {
      if (watts < this->pump_high_min_w_) cls = 1;
      else if (watts <= this->pump_high_max_w_) cls = 2;
      else cls = 3;
    }
    if (cls != this->lp_power_class_) {
      static const char *const NAMES[4] = {"Off", "Low", "High", "Low + heater"};
      char s[40];
      snprintf(s, sizeof(s), "%s (%.0f W)", NAMES[cls], watts);
      this->power_check_sensor_->publish_state(std::string(s));
      this->lp_power_class_ = cls;
    }
  }

  int8_t speed = -1;
  if (this->pump1_on_ && watts >= 100.0f) {  // pump bit on and the supply agrees something is running
    speed = (watts >= this->pump_high_min_w_ && watts <= this->pump_high_max_w_) ? PUMP_HIGH : PUMP_LOW;
  }
  if (speed < 0) {
    this->power_speed_ = -1;
    this->power_agree_count_ = 0;
    return;
  }
  if (speed == this->power_speed_) {
    if (this->power_agree_count_ < 255) this->power_agree_count_++;
  } else {
    this->power_speed_ = speed;
    this->power_agree_count_ = 1;
  }
  // Leave our own presses alone while they settle
  if (this->pump_target_ >= 0 || !elapsed_(this->pump_settle_until_)) return;
  // The power bands assume one pump: with a second pump configured they overlap, so don't correct
  if (this->pump2_configured()) return;
  if (this->power_agree_count_ >= 2 && (PumpSpeed) speed != this->pump_assumed_) {
    this->log_action_("Pump is %s (power %.0f W, was tracked as %s)", pump_name((PumpSpeed) speed), watts,
                      pump_name(this->pump_assumed_));
    this->pump_assumed_ = (PumpSpeed) speed;
  }
}

// ---------------------------------------------------------------------------------------------
// Last action
// ---------------------------------------------------------------------------------------------
// The counter makes a repeated identical action show up as a new state in HA.
void BalboaGS::log_action_(const char *fmt, ...) {
  char msg[190];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  this->last_action_count_++;
  snprintf(this->last_action_, sizeof(this->last_action_), "#%u %s", (unsigned) this->last_action_count_, msg);
  this->last_action_dirty_ = true;
  ESP_LOGI(TAG, "%s", this->last_action_);
}

void BalboaGS::publish_last_action_() {
  if (!this->last_action_dirty_ || this->last_action_sensor_ == nullptr) return;
  this->last_action_sensor_->publish_state(std::string(this->last_action_));
  this->last_action_dirty_ = false;
}

// ---------------------------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------------------------
static void pub_float(sensor::Sensor *s, float &cache, float v) {
  if (s == nullptr) return;
  if (std::isnan(cache) || cache != v) {
    s->publish_state(v);
    cache = v;
  }
}

static void pub_bool(binary_sensor::BinarySensor *s, int8_t &cache, bool v) {
  if (s == nullptr) return;
  if (cache != (int8_t) v) {
    s->publish_state(v);
    cache = (int8_t) v;
  }
}

const char *BalboaGS::heater_status_name(HeaterStatus s) {
  switch (s) {
    case HEATER_HEATING: return "Heating";
    case HEATER_PENDING: return "Pending";
    default: return "Off";
  }
}

// Classify bit 35 by how long it has held its current value (see HEATER_ON_STEADY_MS)
void BalboaGS::update_heater_status_() {
  const uint32_t steady_ms = millis() - this->heater_bit_change_ms_;
  HeaterStatus s = HEATER_PENDING;
  if (this->heater_on_ && steady_ms >= HEATER_ON_STEADY_MS) {
    s = HEATER_HEATING;
  } else if (!this->heater_on_ && steady_ms >= HEATER_OFF_STEADY_MS) {
    s = HEATER_OFF;
  }
  if (s != this->heater_status_) {
    ESP_LOGI(TAG, "Heater: %s -> %s", heater_status_name(this->heater_status_), heater_status_name(s));
    this->heater_status_ = s;
  }
}

void BalboaGS::publish_states_() {
  if (!this->have_frame_) return;

  this->update_heater_status_();

  if (this->filter_select_ != nullptr && !this->filter_read_active_() && this->lp_filter_hours_ != this->filter_hours_) {
    const int idx = filter_index_(this->filter_hours_);
    if (idx >= 0) this->filter_select_->publish_state(std::string(FILTER_OPTION_LABELS[idx]));
    this->lp_filter_hours_ = this->filter_hours_;
  }

  this->sync_light_();

  if (this->pump_select_ != nullptr && this->pump_target_ < 0 && elapsed_(this->pump_settle_until_)) {
    const PumpSpeed before = this->pump_assumed_;
    this->sync_pump_assumed_();
    if (this->lp_pump_speed_ >= 0 && before != this->pump_assumed_) {
      this->log_action_("Pump is %s (panel)", pump_name(this->pump_assumed_));
    }
    if ((int8_t) this->pump_assumed_ != this->lp_pump_speed_) {
      this->pump_select_->publish_state(pump_name(this->pump_assumed_));
      this->lp_pump_speed_ = (int8_t) this->pump_assumed_;
    }
  }

  if (this->mode_select_ != nullptr && !this->mode_change_active_() && this->mode_ != MODE_UNKNOWN &&
      this->mode_ != this->lp_mode_) {
    if (this->lp_mode_ != MODE_UNKNOWN) this->log_action_("Mode is %s (panel)", mode_name(this->mode_));
    this->mode_select_->publish_state(mode_name(this->mode_));
    this->lp_mode_ = this->mode_;
  }

  if (this->display_sensor_ != nullptr && strcmp(this->lp_display_, this->text_) != 0) {
    this->display_sensor_->publish_state(std::string(this->text_));
    snprintf(this->lp_display_, sizeof(this->lp_display_), "%s", this->text_);
  }
  pub_bool(this->heater_sensor_, this->lp_heater_, this->heater_status_ == HEATER_HEATING);
  if (this->heater_status_sensor_ != nullptr && (int8_t) this->heater_status_ != this->lp_heater_status_) {
    this->heater_status_sensor_->publish_state(std::string(heater_status_name(this->heater_status_)));
    this->lp_heater_status_ = (int8_t) this->heater_status_;
  }
  pub_bool(this->pump_sensor_, this->lp_pump_, this->pump1_on_);
  pub_bool(this->pump2_sensor_, this->lp_pump2_, this->pump2_is_on_());
  // Keep the Pump 2 switch matching the panel (panel button, or a press that didn't take)
  if (this->pump2_switch_ != nullptr && this->pump2_target_ < 0 && elapsed_(this->pump2_next_press_ms_)) {
    const bool on = this->pump2_is_on_();
    if ((int8_t) on != this->lp_pump2_switch_) {
      this->pump2_switch_->publish_state(on);
      this->lp_pump2_switch_ = (int8_t) on;
    }
  }
  pub_bool(this->lights_sensor_, this->lp_light_, this->light_on_);

  // Hold back the target while we are stepping it, so HA does not see 37.5 -> 38 -> 38.5
  const bool target_hold = (this->temp_presses_ > 0) || !elapsed_(this->temp_hold_until_);
  const bool water_ok = this->have_valid_water_;
  const bool target_ok = this->target_known_ && !target_hold;

  if (water_ok) pub_float(this->water_sensor_, this->lp_water_, this->water_c_);

  // Seconds since the panel last showed a valid water temperature, in 10 s steps (keeps history tidy)
  // A value restored from flash has no known age, so this stays unknown until a live reading
  if (this->water_age_sensor_ != nullptr && this->water_live_) {
    const int32_t age = (int32_t) (((millis() - this->last_valid_water_ms_) / 10000UL) * 10UL);
    if (age != this->lp_water_age_) {
      this->water_age_sensor_->publish_state(age);
      this->lp_water_age_ = age;
    }
  }

  // Raw bits: only published when one changes
  // Bit 31 is excluded: it follows the display (flips every few seconds as text and temperature alternate)
  const uint16_t status_masked = this->status_bits_ & (uint16_t) ~(1u << (31 - 29));
  if (this->status_bits_sensor_ != nullptr && (int32_t) status_masked != this->lp_status_bits_) {
    const uint16_t v = status_masked;
    // (not called bit(): Arduino defines a bit() macro)
    auto panel_bit_value = [v](uint8_t panel_bit) { return (int) ((v >> (panel_bit - 29)) & 1); };
    char buf[96];
    snprintf(buf, sizeof(buf), "b29=%d b30=%d b32=%d b33=%d b34=%d | heat35=%d p1_36=%d p2_37=%d light38=%d",
             panel_bit_value(29), panel_bit_value(30), panel_bit_value(32), panel_bit_value(33), panel_bit_value(34), panel_bit_value(35), panel_bit_value(36), panel_bit_value(37), panel_bit_value(38));
    this->status_bits_sensor_->publish_state(std::string(buf));
    this->lp_status_bits_ = v;
  }
  if (target_ok) pub_float(this->target_sensor_, this->lp_target_, this->target_c_);

  if (this->climate_ != nullptr) {
    const float cur = water_ok ? this->water_c_ : this->climate_->current_temperature;
    const float tgt = target_ok ? this->target_c_ : this->climate_->target_temperature;
    const auto action =
        this->heater_status_ == HEATER_HEATING ? climate::CLIMATE_ACTION_HEATING : climate::CLIMATE_ACTION_IDLE;
    auto differs = [](float a, float b) { return std::isnan(a) != std::isnan(b) || (!std::isnan(a) && a != b); };
    if (this->climate_->mode != climate::CLIMATE_MODE_HEAT || this->climate_->action != action ||
        differs(cur, this->climate_->current_temperature) || differs(tgt, this->climate_->target_temperature)) {
      this->climate_->mode = climate::CLIMATE_MODE_HEAT;
      this->climate_->action = action;
      this->climate_->current_temperature = cur;
      this->climate_->target_temperature = tgt;
      this->climate_->publish_state();
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Entity glue
// ---------------------------------------------------------------------------------------------
void BalboaButton::press_action() { this->parent_->command_button(this->action_); }

light::LightTraits BalboaLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::ON_OFF});
  return traits;
}

void BalboaModeSelect::control(const std::string &value) {
  SpaMode target = MODE_UNKNOWN;
  if (value == "Standard") target = MODE_STANDARD;
  else if (value == "Economy") target = MODE_ECONOMY;
  else if (value == "Sleep") target = MODE_SLEEP;
  this->parent_->command_mode(target);
  this->publish_state(value);  // optimistic; the real mode is re-published when the change finishes
}

// ---------------------------------------------------------------------------------------------
// Pump 2 (EXPERIMENTAL, untested): press Pump 2 until bit 37 matches, at most 3 presses
// ---------------------------------------------------------------------------------------------
void BalboaGS::command_pump2(bool on) {
  if (this->interface_stopped_) {
    this->log_action_("Pump 2 -> %s ignored: interface stopped", on ? "on" : "off");
    return;
  }
  if (this->pump2_target_ < 0 && on == this->pump2_is_on_()) {
    this->log_action_("Pump 2 -> %s (already %s, no press)", on ? "on" : "off", on ? "on" : "off");
    this->lp_pump2_switch_ = -1;
    return;
  }
  this->pump2_target_ = on ? 1 : 0;
  this->pump2_presses_ = 0;
  this->pump2_next_press_ms_ = 0;
  this->log_action_("Pump 2 -> %s requested (experimental)", on ? "on" : "off");
}

void BalboaGS::process_pump2_() {
  if (this->pump2_target_ < 0) return;
  if (this->mode_change_active_() || this->filter_read_active_() || this->pump_target_ >= 0) return;
  if (!elapsed_(this->pump2_next_press_ms_) || this->any_press_pending_()) return;

  if (this->pump2_is_on_() == (this->pump2_target_ == 1)) {
    this->log_action_("Pump 2 -> %s done (%u presses)", this->pump2_target_ ? "on" : "off",
                      (unsigned) this->pump2_presses_);
    this->pump2_target_ = -1;
    this->lp_pump2_switch_ = -1;  // re-publish the real state
    return;
  }
  if (this->pump2_presses_ >= PUMP2_MAX_PRESSES) {
    this->log_action_("Pump 2 -> %s gave up after %u presses: bit 37 didn't follow (check status_bits)",
                      this->pump2_target_ ? "on" : "off", (unsigned) this->pump2_presses_);
    this->pump2_target_ = -1;
    this->lp_pump2_switch_ = -1;
    return;
  }
  this->queue_press_(BTN_PUMP2);
  this->pump2_presses_++;
  this->pump2_next_press_ms_ = millis() + PUMP2_PRESS_GAP_MS;
}

void BalboaPump2Switch::write_state(bool state) { this->parent_->command_pump2(state); }

void BalboaFilterSelect::control(const std::string &value) {
  int hours = -1;
  for (int i = 0; i < FILTER_OPTIONS; i++) {
    if (value == FILTER_OPTION_LABELS[i]) hours = FILTER_OPTION_HOURS[i];
  }
  if (hours < 0) return;
  this->publish_state(value);  // optimistic; the real setting is re-published when the change finishes
  // Always opens the menu, even if the stored value already matches: a stale value gets corrected,
  // and if the panel already shows the target it just exits without stepping
  this->parent_->command_read_filter(false, hours);
}

void BalboaPumpSelect::control(const std::string &value) {
  PumpSpeed target = PUMP_OFF;
  if (value == "Low") target = PUMP_LOW;
  else if (value == "High") target = PUMP_HIGH;
  this->parent_->command_pump(target);
  this->publish_state(value);  // optimistic; re-synced when the presses finish
}

void BalboaLight::write_state(light::LightState *state) {
  this->state_ = state;
  bool on;
  state->current_values_as_binary(&on);
  this->parent_->command_light(on);
}

climate::ClimateTraits BalboaClimate::traits() {
  auto traits = climate::ClimateTraits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_ACTION);  // Heating / Idle from the decoded heater status
  traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);
  traits.set_visual_min_temperature(this->parent_->get_min_target());
  traits.set_visual_max_temperature(this->parent_->get_max_target());
  traits.set_visual_target_temperature_step(0.5f);
  traits.set_visual_current_temperature_step(0.1f);
  return traits;
}

void BalboaClimate::control(const climate::ClimateCall &call) {
  // Home Assistant may replay a restored target at boot, before the panel has told us its own
  if (!this->parent_->target_known()) return;

  auto t = call.get_target_temperature();
  if (t.has_value()) {
    float v = *t;
    if (v < this->parent_->get_min_target()) v = this->parent_->get_min_target();
    if (v > this->parent_->get_max_target()) v = this->parent_->get_max_target();
    this->parent_->command_target_temperature(v);
    this->target_temperature = v;  // optimistic; the real value is re-published after the presses finish
  }
  this->mode = climate::CLIMATE_MODE_HEAT;
  this->publish_state();
}

}  // namespace balboa_gs
}  // namespace esphome

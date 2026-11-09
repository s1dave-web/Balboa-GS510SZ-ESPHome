#pragma once

// ESPHome external component for Balboa GS510SZ / GS501SZ spa controllers (VL700S-style panel).
// Protocol decoding and button injection ported from https://github.com/MagnusPer/Balboa-GS510SZ
// Feature set matches the proven Arduino v1.5 sketch: sensors, binary sensors, buttons, thermostat.

#include "esphome/core/defines.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <cmath>
#include <string>

namespace esphome {
namespace balboa_gs {

// Order = priority when several presses are queued (lowest first), index into the code table
enum ButtonId : uint8_t {
  BTN_MODE = 0,
  BTN_TEMP_UP = 1,
  BTN_TEMP_DOWN = 2,
  BTN_LIGHT = 3,
  BTN_PUMP1 = 4,
  BTN_PUMP2 = 5,
  BTN_PUMP3 = 6,
};

// Spa operating modes (index = option position in the Mode select)
enum SpaMode : uint8_t {
  MODE_UNKNOWN = 0,
  MODE_STANDARD = 1,
  MODE_ECONOMY = 2,
  MODE_SLEEP = 3,
};

// Pump speeds (index = option position in the Pump Speed select). The pump button cycles Off -> Low -> High -> Off.
enum PumpSpeed : int8_t {
  PUMP_OFF = 0,
  PUMP_LOW = 1,
  PUMP_HIGH = 2,
};

// Actions for BalboaButton
enum ButtonAction : uint8_t {
  ACTION_MODE = 0,
  ACTION_TEMP_UP = 1,
  ACTION_TEMP_DOWN = 2,
  ACTION_LIGHT = 3,
  ACTION_PUMP = 4,
  ACTION_STOP = 5,
  ACTION_READ_FILTER = 6,
  ACTION_TEST_FILTER = 7,
  ACTION_PUMP2 = 8,  // EXPERIMENTAL, untested: tubs with a second / third pump button
  ACTION_PUMP3 = 9,
};

class BalboaGS;

class BalboaButton : public button::Button, public Parented<BalboaGS> {
 public:
  void set_action(uint8_t action) { this->action_ = action; }

 protected:
  void press_action() override;
  uint8_t action_{ACTION_MODE};
};

class BalboaClimate : public climate::Climate, public Parented<BalboaGS> {
 public:
  climate::ClimateTraits traits() override;

 protected:
  void control(const climate::ClimateCall &call) override;
};

// Spa light as an on/off light entity. The panel's light bit is the source of truth:
// a command only presses Light when the panel is in the other state.
class BalboaLight : public light::LightOutput, public Parented<BalboaGS> {
 public:
  light::LightTraits get_traits() override;
  void write_state(light::LightState *state) override;
  light::LightState *get_state() const { return this->state_; }

 protected:
  light::LightState *state_{nullptr};
};

// Mode list: shows the panel's mode (read passively from the display) and changes it with Temp+ then Mode.
class BalboaModeSelect : public select::Select, public Parented<BalboaGS> {
 protected:
  void control(const std::string &value) override;
};

// Pump speed list. The panel only reports pump on/off, so Low vs High is tracked from the presses sent.
class BalboaPumpSelect : public select::Select, public Parented<BalboaGS> {
 protected:
  void control(const std::string &value) override;
};

// Filter setting list (F2 / F4 / F6 / F8 / FC), read from and set through the panel's filter menu
// EXPERIMENTAL (untested, no 2-pump tub available): Pump 2 on/off. Assumes a single-speed pump 2 whose
// state the panel reports in bit 37; presses Pump 2 until the panel agrees, like the light.
class BalboaPump2Switch : public switch_::Switch, public Parented<BalboaGS> {
 protected:
  void write_state(bool state) override;
};

class BalboaFilterSelect : public select::Select, public Parented<BalboaGS> {
 protected:
  void control(const std::string &value) override;
};

class BalboaGS : public Component {
 public:
  // ---- configuration ----
  void set_clock_pin(InternalGPIOPin *pin) { this->clock_pin_ = pin->get_pin(); }
  void set_data_pin(InternalGPIOPin *pin) { this->data_pin_ = pin->get_pin(); }
  void set_button_pin(InternalGPIOPin *pin) { this->button_pin_ = pin->get_pin(); }
  void set_min_target_temperature(float t) { this->min_target_ = t; }
  void set_max_target_temperature(float t) { this->max_target_ = t; }
  void set_learn_target_on_boot(bool v) { this->learn_on_boot_ = v; }
  // Optional: the tub's supply power (e.g. a smart plug in HA), used to check the tracked pump speed
  void set_power_sensor(sensor::Sensor *s) { this->power_sensor_ = s; }
  void set_power_check_text_sensor(text_sensor::TextSensor *s) { this->power_check_sensor_ = s; }
  void set_pump_high_power(float min_w, float max_w) {
    this->pump_high_min_w_ = min_w;
    this->pump_high_max_w_ = max_w;
  }

  // ---- entities ----
  void set_climate(climate::Climate *c) { this->climate_ = c; }
  void set_water_temperature_sensor(sensor::Sensor *s) { this->water_sensor_ = s; }
  void set_target_temperature_sensor(sensor::Sensor *s) { this->target_sensor_ = s; }
  void set_heater_binary_sensor(binary_sensor::BinarySensor *s) { this->heater_sensor_ = s; }
  void set_pump_binary_sensor(binary_sensor::BinarySensor *s) { this->pump_sensor_ = s; }
  void set_lights_binary_sensor(binary_sensor::BinarySensor *s) { this->lights_sensor_ = s; }
  void set_display_text_sensor(text_sensor::TextSensor *s) { this->display_sensor_ = s; }
  void set_light_output(BalboaLight *l) { this->light_output_ = l; }
  void set_last_action_text_sensor(text_sensor::TextSensor *s) { this->last_action_sensor_ = s; }
  void set_mode_select(BalboaModeSelect *s) { this->mode_select_ = s; }
  void set_pump_select(BalboaPumpSelect *s) { this->pump_select_ = s; }
  // EXPERIMENTAL pump 2 / 3 (untested)
  void set_pump2_binary_sensor(binary_sensor::BinarySensor *s) { this->pump2_sensor_ = s; }
  void set_pump2_switch(BalboaPump2Switch *s) { this->pump2_switch_ = s; }
  void command_pump2(bool on);
  bool pump2_configured() const { return this->pump2_sensor_ != nullptr || this->pump2_switch_ != nullptr; }
  void set_water_age_sensor(sensor::Sensor *s) { this->water_age_sensor_ = s; }
  void set_status_bits_text_sensor(text_sensor::TextSensor *s) { this->status_bits_sensor_ = s; }
  void set_filter_select(BalboaFilterSelect *s) { this->filter_select_ = s; }

  // ---- Component ----
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // ---- commands from the entities ----
  void command_target_temperature(float target);
  void command_button(uint8_t action);
  void command_light(bool on);
  void command_mode(SpaMode target);
  void command_pump(PumpSpeed target);
  static const char *pump_name(PumpSpeed p);
  static const char *mode_name(SpaMode m);
  void command_read_filter(bool test = false, int target_hours = -1);
  static int filter_index_(int hours);  // F2..FC -> 0..4, -1 if unknown
  // Hours per filter cycle as last read from the panel: 2/4/6/8, 24 = continuous, 0 = not read yet
  int filter_hours() const { return this->filter_hours_; }

  float get_min_target() const { return this->min_target_; }
  float get_max_target() const { return this->max_target_; }
  bool target_known() const { return this->target_known_; }

  // Clock edge interrupt. Public so it can be exercised by the host-side tests.
  static void clock_isr();

 protected:
  static bool elapsed_(uint32_t until) { return (int32_t)(millis() - until) >= 0; }
  static bool plausible_(float t) { return t > 5.0f && t < 50.0f; }
  void queue_press_(ButtonId id);
  bool any_press_pending_() const { return pending_mask_ != 0; }
  void stop_interface_();

  void decode_frame_();
  void publish_states_();
  void process_temp_presses_();
  void learn_target_();
  void sync_light_();
  // Record what the ESP just did; published to the Last Action sensor from loop()
  void log_action_(const char *fmt, ...);
  void publish_last_action_();
  void detect_mode_();
  void process_mode_change_();
  void finish_mode_change_(bool ok);
  SpaMode mode_seen_since_(uint32_t since) const;
  bool mode_change_active_() const { return this->mode_step_ != MS_IDLE; }
  void process_pump_();
  void process_pump2_();
  bool pump2_is_on_() const { return this->pump2_on_ || (millis() - this->last_pump2_on_ms_) < 1500; }
  void sync_pump_assumed_();
  void finish_pump_(PumpSpeed result, bool ok);
  void on_power_(float watts);
  void process_filter_read_();
  void finish_filter_read_(bool ok, const char *why);
  void note_filter_text_();
  static int parse_filter_text_(const char *text);
  bool filter_read_active_() const { return this->filter_step_ != FR_IDLE; }

  // ---- configuration ----
  uint8_t clock_pin_{0}, data_pin_{0}, button_pin_{0};
  float min_target_{26.0f}, max_target_{40.0f};
  bool learn_on_boot_{true};
  sensor::Sensor *power_sensor_{nullptr};
  float pump_high_min_w_{1000.0f}, pump_high_max_w_{2100.0f};
  int8_t power_speed_{-1};          // speed the last power reading points to (-1 = no opinion)
  uint8_t power_agree_count_{0};    // consecutive readings pointing to power_speed_
  text_sensor::TextSensor *power_check_sensor_{nullptr};
  int8_t lp_power_class_{-1};       // last published power class (0 off, 1 Low, 2 High, 3 Low + heater)

  // ---- entities ----
  climate::Climate *climate_{nullptr};
  sensor::Sensor *water_sensor_{nullptr}, *target_sensor_{nullptr};
  binary_sensor::BinarySensor *heater_sensor_{nullptr}, *pump_sensor_{nullptr}, *lights_sensor_{nullptr};
  text_sensor::TextSensor *display_sensor_{nullptr};
  BalboaLight *light_output_{nullptr};
  text_sensor::TextSensor *last_action_sensor_{nullptr};
  BalboaModeSelect *mode_select_{nullptr};
  BalboaPumpSelect *pump_select_{nullptr};
  sensor::Sensor *water_age_sensor_{nullptr};
  text_sensor::TextSensor *status_bits_sensor_{nullptr};
  BalboaFilterSelect *filter_select_{nullptr};

  // ---- decoded panel state (main loop only) ----
  bool have_frame_{false};
  bool display_button_{false};  // panel is showing the SET temperature
  bool heater_on_{false}, pump1_on_{false}, light_on_{false};
  char text_[8]{" "};
  float water_c_{NAN}, target_c_{NAN};
  bool target_known_{false};
  bool have_valid_water_{false};
  uint32_t last_valid_water_ms_{0};   // when the panel last showed a valid water temperature
  bool water_live_{false};            // true once the panel has shown a temperature since boot
  float saved_water_c_{NAN};          // last value written to flash
  ESPPreferenceObject water_pref_;    // last water temperature, kept across restarts
  uint16_t status_bits_{0};           // raw panel bits 29..38 (bit 0 = panel bit 29)
  bool interface_stopped_{false};

  // ---- temperature change in progress ----
  int temp_presses_{0};
  ButtonId temp_dir_{BTN_TEMP_UP};
  uint32_t temp_next_press_ms_{0};
  uint32_t temp_hold_until_{0};
  uint32_t learn_next_ms_{0};
  char last_action_[200]{""};
  uint32_t last_action_count_{0};
  bool last_action_dirty_{false};
  uint32_t light_settle_until_{0};

  // ---- mode ----
  enum ModeStep : uint8_t { MS_IDLE, MS_WAIT_READY, MS_TEMP_SENT, MS_MODE_SENT, MS_SETTLE };
  SpaMode mode_{MODE_UNKNOWN};             // current mode as read from the display
  SpaMode lp_mode_{MODE_UNKNOWN};          // last published
  uint32_t mode_text_ms_[4]{0, 0, 0, 0};   // when each mode's text (5td / Ecn / 5Lp) was last on the display
  uint32_t first_frame_ms_{0};
  uint32_t last_set_display_ms_{0};        // last time the panel was showing the SET temperature
  ModeStep mode_step_{MS_IDLE};
  SpaMode mode_target_{MODE_UNKNOWN};
  uint8_t mode_attempts_{0};
  uint32_t mode_step_ms_{0};

  // ---- pump speed (tracked) ----
  PumpSpeed pump_assumed_{PUMP_OFF};  // best guess of the current speed
  int8_t pump_target_{-1};            // requested speed, -1 = nothing in progress
  uint8_t pump_presses_left_{0};
  uint8_t pump_attempts_{0};
  uint32_t pump_next_press_ms_{0};
  uint32_t pump_settle_until_{0};
  int8_t lp_pump_speed_{-1};
  uint32_t last_pump_on_ms_{0};  // last frame with the pump bit on (off must last 1.5 s to count)

  // ---- pump 2 (EXPERIMENTAL) ----
  binary_sensor::BinarySensor *pump2_sensor_{nullptr};
  BalboaPump2Switch *pump2_switch_{nullptr};
  bool pump2_on_{false};            // panel bit 37
  uint32_t last_pump2_on_ms_{0};
  int8_t pump2_target_{-1};         // -1 = nothing in progress
  uint8_t pump2_presses_{0};
  uint32_t pump2_next_press_ms_{0};
  int8_t lp_pump2_{-1}, lp_pump2_switch_{-1};
  bool pump_is_off_() const { return !this->pump1_on_ && (millis() - this->last_pump_on_ms_) >= 1500; }  // after a light command, don't sync the light entity from the panel yet

  // ---- filter setting read (Temp+, Pump 1 opens the filter menu, Pump 1 again exits) ----
  enum FilterStep : uint8_t { FR_IDLE, FR_WAIT_READY, FR_TEMP_SENT, FR_PUMP_SENT, FR_TEST_STEPPING, FR_SET_STEPPING, FR_EXIT_SENT };
  int filter_target_{-1};            // hours wanted (2/4/6/8/24), -1 = read only
  uint8_t filter_set_presses_{0};    // Temp+ presses sent inside the menu
  int filter_shown_{-1};             // setting on the display now (parsed every frame during a read)
  uint32_t filter_shown_ms_{0};      // when filter_shown_ was last seen
  bool filter_test_{false};          // test run: step Temp+ round all 5 settings (back to the start) before exiting
  uint8_t filter_test_left_{0};
  bool showing_temperature_() const { return strchr(this->text_, '.') != nullptr && strchr(this->text_, 'C') != nullptr; }
  FilterStep filter_step_{FR_IDLE};
  uint32_t filter_step_ms_{0};
  bool filter_pump_before_{false};   // pump bit before the menu press (a change means the menu didn't open)
  int filter_found_{-1};             // setting seen on the display during this read
  char filter_seen_[150]{""};         // every display text seen during the read, for the log
  char filter_last_text_[8]{""};
  uint8_t filter_hours_{0};
  int16_t lp_filter_hours_{-1};
  ESPPreferenceObject filter_pref_;

  // ---- last published values (avoid repeating identical states) ----
  float lp_water_{NAN}, lp_target_{NAN};
  int8_t lp_heater_{-1}, lp_pump_{-1}, lp_light_{-1};
  int32_t lp_water_age_{-1};
  int32_t lp_status_bits_{-1};
  char lp_display_[8]{"\x01"};

  // ---- ISR shared state ----
  static uint8_t isr_clock_pin_, isr_data_pin_, isr_button_pin_;
  static uint8_t buffer_[40];
  static volatile uint8_t pending_mask_;
  static volatile bool buffer_ready_;
  static volatile bool overflow_;
  static volatile uint32_t last_edge_us_;
  static volatile uint8_t bit_counter_;
  static volatile uint8_t data_index_;
  static volatile uint8_t active_button_;
  static volatile bool skip_frame_;
};

}  // namespace balboa_gs
}  // namespace esphome

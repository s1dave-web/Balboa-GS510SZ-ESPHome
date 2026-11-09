# Balboa GS510SZ – ESPHome

An [ESPHome](https://esphome.io) external component for hot tubs with a **Balboa GS510SZ** controller and a
**VL700S / VL701S** top-side panel. A Wemos D1 mini (ESP8266) sits on the panel cable, reads the display
and presses the buttons, and gives Home Assistant a proper thermostat, light, mode, pump speed and filter
cycle control.

Built on the protocol work in [MagnusPer/Balboa-GS510SZ](https://github.com/MagnusPer/Balboa-GS510SZ)
and its single-pump adaptation in [s1dave-web/Balboa-GS510SZ](https://github.com/s1dave-web/Balboa-GS510SZ)
(see [Credits](#credits)). Tested on a StrongSpa G2 (GS510SZ + VL701S, 920 L, 13 A supply).

> **Status:** working and in daily use on one tub. Other GS-series controllers and panels may differ —
> the raw `display` and `status_bits` sensors are there to help you check yours.

## What you get in Home Assistant

| Entity | Type | Notes |
|---|---|---|
| Temperature Control | climate | Heat only. Action shows **Heating / Idle** from the panel's heater bit. Setting a target presses Warm/Cool for you. |
| Water Temperature / Target Temperature | sensor | Read from the display. The last water temperature is kept in flash, so a restart doesn't blank it. |
| Lights | light | On/off. Only presses Light when the panel is in the other state; re-syncs if someone uses the panel. |
| Mode | select | Standard / Economy / Sleep. Read from the display, changed with Warm then Mode (see below). |
| Pump Speed | select | Off / Low / High. Tracked from presses, corrected from a power sensor if you have one. |
| Filter Cycle Setting | select | 2 / 4 / 6 / 8 hours / Continuous. Read and set through the panel's filter menu. |
| Heater, Pump | binary sensor | Straight from the panel bits. |
| Filter Cycle, Next Filter Cycle | binary sensor, timestamp | Predicted from the spa's power-on time (see below). |
| Time to Heat | sensor (h) | From water volume, temperature difference and heater power (measured if available). |
| Effective Target | sensor | The set temperature, or set − 11 °C in Sleep mode. |
| Last Action | text (diagnostic) | What the ESP last did and why, e.g. `#12 Mode -> Sleep done (1 cycles)`. |
| Pump Speed (Power), Heater Power | diagnostic | What the supply power says about the pump, and the measured heater power. |
| Water Temperature Age, Boot Reason, Spa Powered On, WiFi Signal, Uptime | diagnostic | |
| Read Filter Setting, Stop Command, Reset ESP | button (diagnostic) | |

## Hardware

### Panel

Written for the **VL701S**: the single-pump version of the VL700S, with five buttons — **Jets**, **Mode**,
**Light**, **Warm** and **Cool** — around the 4-character display. One Jets button drives a two-speed pump
(Off → Low → High → Off) and there is no separate circulation pump. The original library targets the
two-pump VL700S; the bus and button codes are the same, only Pump 2 / Pump 3 go unused here.

### Interface

- Wemos D1 mini (ESP8266), powered from the panel cable's 5 V.
- The OR-gate interface from MagnusPer's project between the D1 mini and the panel's data lines — see the
  [wiring diagram and PCB](https://github.com/MagnusPer/Balboa-GS510SZ#wiring). The OR gates stop the
  ESP and the panel driving the button line against each other.

| RJ45 pin | Signal | D1 mini |
|---|---|---|
| 3 | Button data (via OR gate) | D8 / GPIO15 |
| 4 | GND | GND |
| 5 | Display data (via OR gate) | D2 / GPIO4 |
| 6 | Clock | D1 / GPIO5 |
| 7 | 5 V | 5V |

**Use `board: esp01_1m`** (and GPIO numbers rather than D-pin names). A `d1_mini` (4 MB) build did not boot
on this board with ESPHome 2026.10's native ESP8266 toolchain.

## Installing

1. Copy [`example/spa.yaml`](example/spa.yaml) into your ESPHome folder. It pulls the component straight
   from this repo:
   ```yaml
   external_components:
     - source: github://s1dave-web/Balboa-GS510SZ-ESPHome@main
       components: [balboa_gs]
   ```
2. Add `wifi_ssid`, `wifi_password`, `ap_password` and `api_key` to your `secrets.yaml`.
3. Set the substitutions at the top for your tub (below), then install.
4. After the first boot, press **Read Filter Setting** once so the filter schedule knows your cycle length.

### Configuration (substitutions in `spa.yaml`)

| Substitution | Example | Used for |
|---|---|---|
| `water_litres` | `920` | Time to Heat. Use the water you actually fill to, not the brochure capacity (see below). |
| `heater_watts` | `1865` | Time to Heat, until the heater has been measured from `power_entity` |
| `heater_efficiency` | `1.0` | Share of heater power that reaches the water. Lower it if real heating is slower than the estimate. |
| `power_entity` | `sensor.hot_tub_plug_power` | Optional HA power sensor on the tub's supply (smart plug, CT clamp) |
| `pump_high_min_w` / `pump_high_max_w` | `1000` / `2100` | Power band that means pump High (see below) |
| `pump_low_watts` | `385` | Pump Low draw, subtracted to measure the heater |
| `sleep_drop` | `11.0` | How far below the set temperature Sleep mode lets the water fall |
| `filter_first_delay_s` | `360` | First filter cycle after power-on (6 min on this spa) |

### Sizing `water_litres`

Brochure capacities are usually brim-full: StrongSpa lists every
[G-2 model](https://strongspas.com/product/the-g-2-collection/) at 300 US gal / 1135 L. What matters for
Time to Heat is the water actually heated. On this tub a clean heating run went from 11.5 °C to 14.5 °C in
1 h 44 min while the heater delivered about 3.0 kWh (supply power minus the pump), which is
3.0 kWh × 3.6 MJ/kWh ÷ (4.184 kJ/kg·°C × 3.0 °C) ≈ 865 L. Cover heat loss pulls the true figure down a
little and the pump's own heat pushes it up, so 920 L is used. To check yours: note the water temperature
and the heater's kWh over a couple of hours of heating on Low with the cover on, and use the same sum.

**No power sensor?** Remove `power_sensor:` from the `balboa_gs:` block, the `homeassistant` sensor
(`tub_power`) and the `Heater Power` sensor. Pump speed then comes from press tracking alone and Time to
Heat uses `heater_watts`.

## How it works

### Panel protocol (GS510SZ ↔ VL700S/VL701S)

The controller clocks the panel with **42 pulses per frame, one frame every ~20 ms**. Display data is
read on pulses 0–38; the panel (or the ESP) drives the button line on pulses 39–41.

| Bits | Meaning |
|---|---|
| 0–27 | Four 7-segment characters (7 bits each) |
| 28 | Set temperature is being shown (Warm/Cool pressed) |
| 29, 30, 32, 33, 34 | Always 0 here |
| 31 | Follows the display contents (not the mode, despite older notes) |
| 35 | Heater on (flashes while heat is wanted but waiting) |
| 36 | Pump 1 on — **the same for Low and High**, there is no speed bit |
| 37 | Pump 2 |
| 38 | Light |

Button codes on pulses 39–41: Mode `001`, Warm `100`, Cool `101`, Light `011`, Pump 1 `110`,
Pump 2 `010`, Pump 3 `111`.

The interrupt handler only touches GPIO registers and RAM. An earlier version called `detachInterrupt()`
from the interrupt; that code lives in flash and crashed the ESP8266 (exception 0) whenever a frame ended
while settings were being written to flash.

### Mode (Standard / Economy / Sleep)

- Display text: ` 5Lp` = Sleep, `Ecn` = Economy, ` 5td` = Standard. In Sleep and Economy the mode text
  shows while idle; Standard shows no text, so **30 s without mode text = Standard**.
- To change mode the panel wants **Warm, then Mode while the set temperature is flashing**. A second Warm in
  that window would change the set temperature instead, so the ESP waits until the panel is quiet
  (no set temperature shown for 4 s) before starting, then checks the display after each attempt (max 3).
- Sleep only heats during filter cycles and lets the water drop 11 °C below the set temperature; Economy
  only heats during filter cycles.

### Pump speed

The panel only reports pump on/off. The ESP tracks speed from its own presses (Pump 1 cycles
Off → Low → High → Off) and, with `power_entity`, checks it against the tub's supply power. Two readings in
a row must agree before it corrects the tracked speed. Measured on this tub:

| State | Power |
|---|---|
| Idle | 5 W |
| Pump Low | ~385 W |
| Pump High (heater locked out on a 13 A tub) | ~1920 W |
| Pump Low + heater | ~2250 W |

So below `pump_high_min_w` = Low, between the thresholds = High, above `pump_high_max_w` = Low with the
heater on. When the heater runs on Low, the heater's own power (total − pump Low) is averaged and used for
Time to Heat.

### Filter cycles

- **Setting:** Warm (set temperature flashes), then Pump 1 opens the filter menu, which shows `FIL2`
  (decodes as `f1L2`). Each Warm press steps `FIL2 → FIL4 → FIL6 → FIL8 → FILC → FIL2`; Pump 1 stores and
  exits. The ESP reads the menu after every press, stops if a temperature appears or the pump changes,
  and only presses Pump 1 once the display shows the wanted setting.
- **Schedule:** per the manual, the first cycle starts 6 minutes after the spa is powered on and the next
  12 hours after that. The D1 mini is powered by the controller, so a real power-on boot (reset reason
  *Power on*) means the spa was just powered on; that time is kept in flash. OTA updates and restarts
  don't move it. *Next Filter Cycle* and *Filter Cycle* are predictions from that time and the cycle
  length.

## Known limits

- One pump (Pump 1) with Low/High. Pump 2 / 3 aren't exposed (the Pump 2 bit only shows in `status_bits`).
- Celsius displays only.
- Filter schedule assumes the 6 min / 12 h rule from the StrongSpa manual; it is unknown whether changing
  the filter setting restarts the controller's cycle clock.
- Tubs are often at the edge of Wi-Fi range. The example delays the fallback hotspot to 10 minutes; AP mode
  plus scanning while reconnecting crashed the ESP8266 here.

## Credits

- [MagnusPer/Balboa-GS510SZ](https://github.com/MagnusPer/Balboa-GS510SZ) — the original reverse
  engineering of the GS510SZ panel bus, the 7-segment table, the button codes, the OR-gate interface and
  PCB. This component's frame decoding and button injection are based on that library.
- [s1dave-web/Balboa-GS510SZ](https://github.com/s1dave-web/Balboa-GS510SZ) — the single-pump (VL701S)
  Arduino version this ESPHome component was converted from.
- [NickB1/OpenSpa](https://github.com/NickB1/OpenSpa) and
  [The Internet of Spas](https://www.olivierhill.ca/archives/72-The-Internet-of-Spas.html), referenced by
  the original project.

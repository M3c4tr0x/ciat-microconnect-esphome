**English** · [Français](emulator.fr.md)

# The ESPHome emulator

`esphome/ciat-emulator.yaml` turns an ESP32-C3 into a **virtual Micro-Connect terminal**. It answers the heat pump
exactly like the original terminal, and exposes the heat pump to Home Assistant as thermostats and sensors.

It is "half an emulator": it speaks to the bus like the terminal, but it does not expose virtual keys to the user.
A navigator turns high-level requests ("eco preset", "setpoint 21.5", "read P40") into key presses and **checks every
result on the heat pump's display** before going on.

## 1. Install

1. Build the interface and **bench-test the transmit stage** ([hardware](hardware.md), `esphome/ciat-bench.yaml`).
2. Copy the files of `esphome/` into your ESPHome configuration folder:
   `ciat-emulator.yaml`, `ciat_bus.h`, `ciat_emul.h`, `ciat_link.h`, `ciat_nav.h`, `ciat_flash.h`.
3. Add to your `secrets.yaml`: `wifi_ssid`, `wifi_password`, `ciat_api_key` (32 bytes in base64, e.g.
   `openssl rand -base64 32`), `ciat_ota_password`.
4. In `substitutions`, set `room_sensor` to the Home Assistant entity of a room thermometer. It replaces the
   terminal's own sensor (the heat pump uses it for room compensation).
5. **First run, listen only**: keep `tx_compiled: "false"` and the original terminal connected. Check in Home
   Assistant that "Bus active" is on, that "Valid frames per minute" is ~1,300 and "Rejected bytes per minute" ~0,
   and that "Shadow differences per minute" stays at 0 (the emulator's 12.1 would match the terminal's).
6. **Replace the terminal**: set `tx_compiled: "true"`, flash, unplug the original terminal, turn on "Bus
   transmission". The heat pump keeps its state; the thermostats appear within seconds.

Tested with ESPHome 2026.11 (ESP-IDF framework) on an ESP32-C3 Super Mini.

## 2. What runs where

| Layer | File | Role |
|---|---|---|
| Bus task | `ciat_link.h` | FreeRTOS task: reads the UART, answers each request **10.5 ms** after its start, checks the echo. Independent of the ESPHome loop and of the network |
| Frames | `ciat_emul.h` | builds the 14.1 (clock) and 12.1 (state) answers |
| Decoder | `ciat_bus.h` | splits the byte stream into frames (header + length + XOR + content), decodes them for Home Assistant |
| Navigator | `ciat_nav.h` | drives the parameter menu and the setpoint with key presses, reads every result back |
| Backup | `ciat_flash.h` | keeps the surveyed settings in flash |
| Glue | `ciat-emulator.yaml` | ESPHome entities, thermostats, scans, safeguards |

The pure-logic parts (`ciat_emul.h`, `ciat_nav.h`) are tested on a PC: `tools/test_ciat_emul.cpp` (real frames) and
`tools/test_ciat_nav.cpp` (a simulator of the heat pump's menu, 51 cases). The bus task (`ciat_link.h`) is tested
the same way with stand-ins for the ESP-IDF functions: `tools/test_ciat_link.cpp` (echo checks and inhibitions).

## 3. Thermostats

### Heating (`climate.<name>_heating`)

| Thermostat | Heat pump |
|---|---|
| mode heat / off | dial on heating / off |
| preset comfort / eco | dial on heating + programme slot sent in b1 |
| preset away | dial on frost protection |
| target temperature | adjusted outside the menu by + and −, 15 to 30 °C by 0.5 |
| current temperature | the room temperature sent to the heat pump |
| action heating / idle | compressor bit |

- The state shown always comes from the heat pump (`optimistic: false`): Home Assistant never shows something the
  heat pump has not displayed.
- Changing the target temperature **changes the heat pump's own comfort or eco setpoint**, durably (it is the
  setpoint of the current slot).
- **Cooling is not supported**: the dial positions cooling and cooling programme were never observed (protocol §9).
- The weekly programme of the original terminal no longer exists: **the slot comes from Home Assistant**. Any
  scheduler that sets the preset works (see [Home Assistant](home-assistant.md)).

### Pool (`climate.<name>_pool`)

- Heat = dial on pool; off = dial on off (only if the dial was on pool).
- Target temperature = pool setpoint (P23), 20 to 35 °C by 0.5: the range of the CIAT sheet; both ends were accepted
  by the heat pump ✅.
- Current temperature = P50 (read in the menu scans, only with the dial on pool).
- The heat pump then heats only while its **pool contact** is closed (wired by the installer, typically through the
  pool pump).

### How requests are applied

- Requests are queued and applied when the navigator is free. A request **interrupts a measurement scan** (clean
  exit from the menu, ~3 s), never a parameter write.
- A request that changes nothing (dial already in place) is ignored, so a scheduler can repeat it freely.
- The setpoint is reached by + and − presses of **3 frames** each (the heat pump ignores shorter presses outside the
  menu), read back one by one, then confirmed for ~2 s. One attempt only; an error is published in "Navigator last
  error".

## 4. Measurements and settings

- **Measurement scan**, every "Measurement scan period" (5 min by default; 1 to 2 min works well):
  - at **user level**: P40-P42, P44, P45, P48 (plus P50 with the dial on pool), the measurements visible at that
    level. ~17 s per scan (~3 s to enter the menu);
  - at **installer level**, every 10 min instead: the same plus P43, P46 and P47, which are only shown there. ~28 s
    (~8 s to enter the installer level);
  - each value is the median of 3 cycles (a one-cycle glitch is rejected); P50, which jumps at every cycle, is
    averaged over 9.
  - Values only change in Home Assistant at each scan: curves show steps of one scan period.
- **Settings scan** (P00-P36, P49, P60-P63, P80-P99): at boot and once a day. Results in "Settings P00 to P29" and
  "Settings P30 to P99", kept in flash.
- Scans run only with "Automatic scan" on, transmission active, and the dial on a position that allows the menu
  (cooling, off, frost protection, heating, pool).
- The heat pump **keeps regulating while the menu is open**; a slot change arriving during a scan is applied when
  the scan leaves the menu.
- Passive survey: with the original terminal in listen-only mode, values viewed by hand on its menu are published too.

### Thermal power and energy

- **"Thermal power"** (kW) = 1.163 × Q × (P41 − P40), computed every 10 s, where Q is the **"Water flow rate"**
  (m³/h, configuration) that **you enter**: the heat pump does not report the flow. 0 disables it.
- 0 while the compressor is off, and while the supply is colder than the return (defrost: the heat taken back from
  the water is ignored, so the total is slightly over-estimated). Unknown if P40/P41 have not been read for 3 min.
- **"Thermal energy"** (kWh, `total_increasing`, kept in flash): its integral, for Home Assistant's long-term
  statistics (the raw history is purged after a few days).
- Accuracy: P40 and P41 are read to the tenth, so ±0.1 K on a 3-5 K difference (±2-3 %), plus the error on Q. During
  the first 2-3 minutes of a compressor cycle, the difference is still building up: the power shown is lower, which
  is real (the water and the exchanger warm up first).
- Finding Q: a flow meter (a clamp-on ultrasonic one needs no plumbing), the circulator curve, or the
  [Home Assistant package](home-assistant.md#6-heat-output-and-cop) that estimates it from the datasheet COP.

## 5. Safety and robustness

| Situation | Behaviour |
|---|---|
| ESP32 off, rebooting or crashed | TX optocoupler off: **bus free**. The heat pump keeps running on its last state |
| Reboot | the "Bus transmission" choice is kept in flash; after 20 s of listening, transmission resumes unless another terminal answers or the bus is silent |
| Home Assistant or Wi-Fi lost | no reboot (`reboot_timeout: 0s`); answers continue with the last dial, slot and room temperature (kept in flash) |
| Another terminal on the bus | 3 foreign frames within 10 s → transmission inhibited |
| Collision (echo different) | inhibited at once |
| Echo missing | tolerated if the heat pump answered our 12.1 with its 12.2 (the frame went through, only our receiver missed it), up to 2 in a row; otherwise (no 12.2, 3rd in a row, or clock frame) inhibited at once |
| Heat pump stops answering our frames | 20 cycles without a 12.2 → inhibited |
| Inhibition | automatic resume after 30 s if no other terminal is seen, **at most 3 times per hour**; then it stays off |
| Answer too late (> 16 ms) | dropped, the heat pump moves on |
| Menu left open (reboot during a scan) | left by a long OK 3 s later |

⚠️ After a power cut, the heat pump **does not start until a terminal answers**. Keeping the last room temperature in
flash makes sure the ESP32 always can, even without Home Assistant.

Remaining limits without Home Assistant: no scheduler (preset frozen), no clock after a reboot (no 14.1; effect on
the heat pump ❓), no thermostats.

## 6. Rescue mode

"Rescue mode" (configuration) unlocks the low-level commands for 30 minutes: + / − / OK keys, menu entry and exit,
writing a parameter (whitelist only, value read back), manual scans, navigator reset. Without it, these buttons do
nothing and say so in "Navigator last error". Use it to change a setting the thermostats do not cover, or to recover
a blocked navigator.

To reset a **permanent fault** (protocol §8.2: dial on off, OK held 3 s): turn the heating thermostat off, then press
"Long OK (leave the menu)" (OK held ~4 s) in rescue mode. 🟡 Never tried: no fault occurred on the reference
installation.

## 7. Diagnostics

| Entity | Use |
|---|---|
| Emulator state | `transmission active`, `inhibited: <reason>`, `ready, transmission off`, `(room temperature frozen …)` |
| Last bus anomaly | each foreign frame, wrong or missing echo: time, bytes, delay after the request, whether our echo was expected, frame sent |
| Wrong echoes / Tolerated missed echoes / Dropped answers / Answers sent per minute | health of the transmit side (~530 answers per minute) |
| Navigator state, last error, last scan, last write | what the navigator is doing and how it ended |
| Raw heat-pump / terminal frame, Display, Dial, Keys | live decoding |
| Last key press | each press: keys, number of frames, heat-pump reaction and delay |
| New heat-pump frame / Single-cycle heat-pump frame | frames never seen since boot, and frames lasting one cycle only |
| Last reboot reason, Uptime | power cut vs crash |

A Home Assistant automation on "Last bus anomaly" gives a notification with the full diagnostic at each incident
(see `examples/`).

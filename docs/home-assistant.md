**English** · [Français](home-assistant.fr.md)

# Home Assistant integration

The emulator appears in Home Assistant as an ESPHome device ("CIAT heat pump" by default) with two thermostats,
the heat pump's measurements and diagnostics. Examples in [`examples/home-assistant/`](../examples/home-assistant/).

Entity IDs below assume the default names; check yours in Settings → Devices.

## 1. Thermostats

- `climate.ciat_heat_pump_heating`: heat / off, presets **comfort / eco / away**, target 15-30 °C by 0.5.
- `climate.ciat_heat_pump_pool`: heat / off (dial on pool / off), target 20-35 °C by 0.5.

Their state always comes from the heat pump. A change takes a few seconds: the navigator may first leave the
parameter menu (~3 s), and the setpoint is reached press by press (~0.5 s each) then confirmed (~2 s). While it
adjusts, the card shows the value climbing step by step.

Only one of the two can heat at a time: the pool is a position of the same dial. Turning the pool on puts the heating
thermostat to off, and the other way round.

## 2. Weekly schedule

The original terminal held the weekly programme; with the emulator, **Home Assistant must set the slot**. Any
scheduler that sets the `comfort` / `eco` preset of `climate.ciat_heat_pump_heating` works. A request that changes
nothing is ignored by the firmware, so a scheduler may repeat it.

Recommended: the **`ha-chauffage`** custom integration (HACS, by armellarcier). Unlike Home Assistant's native
schedule helper, its card lets **non-admin users** (a wall tablet, for instance) edit the week, with half-hour slots.

Settings that work with this emulator:

| Option | Value | Why |
|---|---|---|
| Use the thermostats' presets | **on** | otherwise each slot sends a temperature, i.e. + and − presses on the heat pump |
| Eco preset | the thermostat's `eco` | |
| Comfort preset | the thermostat's `comfort` | |
| Frost protection preset | the thermostat's **`away`** | otherwise it would send 7 or 15 °C |
| Boost preset | the thermostat's `comfort` | the heat pump has no boost |
| Non-admin access level | Programmer | lets a tablet user edit the weeks |
| Override duration | e.g. 120 min, or until the next slot | |

Behaviour to know:

- It applies a slot at its start, at Home Assistant start-up and when the weeks are saved; it does not resend in a
  loop, so scans are not interrupted for nothing.
- **Any change made elsewhere becomes an override** (preset, setpoint or mode) that lasts until its end, then the
  schedule takes over again. Turning the thermostat off is therefore temporary: to stop heating durably, pause the
  integration's "Control" switch.
- 🟡 When a setpoint override ends, it restores the preset, not the temperature: the changed setpoint stays in the heat
  pump (it is a durable change of its comfort or eco setpoint).
- ⚠️ **Summer**: if the schedule stays active, its next slot puts the dial back on heating and stops pool heating.
  Pause it for the summer, or link the pause to the pool thermostat with an automation.

## 3. Notifications

[`automation-bus-notifications.yaml`](../examples/home-assistant/automation-bus-notifications.yaml) sends a
notification:

- at each **bus anomaly**, with the full diagnostic (frame, delay, expected echo or not, frame sent), even when
  transmission resumes by itself;
- when **transmission stays off for more than 1 min** (automatic resumes exhausted, or turned off by hand).

It never turns transmission back on: the firmware does, at most 3 times per hour on purpose. An automation that
forces it back on would defeat that limit.

## 4. Dashboard

[`dashboard-view.yaml`](../examples/home-assistant/dashboard-view.yaml) is a "sections" view with:

- the two thermostats;
- the measurements P40-P48 and their 24 h curves;
- the emulator state, last anomaly and navigator results;
- the settings read from the heat pump;
- the low-level commands, shown only while **Rescue mode** is on;
- optionally, the `ha-chauffage` weekly schedule card.

On a wall tablet, a single climate tile (`target-temperature`, `climate-preset-modes`) plus the schedule card in a
pop-up is enough for daily use.

## 5. Room temperature

The firmware sends the Home Assistant sensor set in `room_sensor` to the heat pump. Choose a thermometer in the main
room: the heat pump uses it for room compensation (P04). The last valid value is kept in flash and used without time
limit if the sensor or Home Assistant disappears; "Emulator state" then says "room temperature frozen".

Do not use the device's own "Terminal room temperature": once the original terminal is unplugged, it only reads back
what the ESP32 sends.

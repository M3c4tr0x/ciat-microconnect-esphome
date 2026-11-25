**English** · [Français](README.fr.md)

# CIAT Micro-Connect bus ↔ ESPHome

Reverse engineering of the two-wire bus between the **CIAT Micro-Connect** room terminal and the
**CIAT Aqualis 2** heat pump, and an **ESP32 (ESPHome) that replaces the terminal** so the heat pump can be
monitored and controlled from **Home Assistant**.

> ⚠️ **Safety first — read before connecting anything.**
> The same two wires carry both power and data, and the bus return is **tied to earth inside the heat pump**.
> **Never connect the ground of an earthed device** (oscilloscope, PC, mains charger) **to a bus wire**: on
> 15 Nov 2026 this put the heat pump into fault, and the outdoor unit had to be power-cycled to recover.
> The ESP32 must stay **galvanically isolated** from the bus (optocouplers on receive and transmit).
> **Never power the ESP32 from the bus.** The original terminal draws only **~1.5 mA** on average (1 to 5 mA
> ripple, measured with a shunt resistor in the bus wire); an ESP32 needs ~80 mA with 300 mA Wi-Fi peaks. Use a
> separate USB supply.
> You work on your own equipment at your own risk; see the disclaimer below.

## What works

| | Status |
|---|---|
| Bus protocol: UART 9600 8N1, XOR checksum, ~220 ms polling cycle, every frame decoded | ✅ |
| Full parameter map P00–P99 (settings and live measurements) | ✅ |
| Passive listening through an HCPL-0501 optocoupler, both directions | ✅ in service since Nov 2026 |
| Isolated transmit stage, answering the heat pump in place of the terminal | ✅ in service since 23 Nov 2026, terminal unplugged |
| Home Assistant `climate` entities: heating (comfort / eco / away presets, setpoint) and pool | ✅ validated on the heat pump |
| Automatic reading of measurements (water temperatures, outdoor, refrigerant…) and settings through the terminal menu | ✅ |
| Weekly schedule from Home Assistant (replaces the terminal's own programme) | ✅ |
| Writing parameters from Home Assistant | 🟡 low-level "rescue mode" only |
| Cooling, back-up heaters / boiler, hot water, fault codes, load shedding | ❓ never observed on the reference installation ([protocol §9](docs/protocol.md#9-not-observed-yet)) |

Every finding in the documentation is tagged ✅ confirmed, 🟡 hypothesis or ❓ unknown, with the evidence.

## The protocol in short

- UART 9600 baud, 8N1. The last byte of every frame is the **XOR** of the previous ones.
- Open-collector bus: the heat pump and the terminal both pull the line low to transmit, so a single
  input on the bus voltage sees both directions.
- The heat pump is the master and repeats this cycle every ~220 ms:

| Frame | Direction | Bytes | Content |
|---|---|---|---|
| Q14 | heat pump → terminal | `14 14` | "clock" request |
| 14.1 | terminal → heat pump | `EE dd HH MM xx` | day, hour, minutes (BCD) |
| Q12 | heat pump → terminal | `12 12` | "status" request |
| 12.1 | terminal → heat pump | `AC b1 b2 TT Fd 12 xx` | requested mode, dial and keys, room temperature (BCD) |
| 12.2 | heat pump → terminal | `40 p1 p2 d1 d2 d3 d4 xx` | pictograms, compressor, the four 7-segment digits |

- The **terminal** owns the clock, the weekly programme, the room sensor and the raw state of keys and dial.
- The **heat pump** owns the setpoints, the P00–P99 parameters (including the heating curve) and whatever
  its four digits display.

## Documentation

- [Protocol](docs/protocol.md): physical layer, frames, register map, menu behaviour, parameters.
- [Hardware](docs/hardware.md): the isolated ESP32 interface (receive and transmit), bench test, wiring.
- [Emulator](docs/emulator.md): install, how it works, thermostats, safety, diagnostics.
- [Home Assistant](docs/home-assistant.md): thermostats, weekly schedule, notifications, dashboard.

## Quick start

1. Read the safety warning above and the [hardware](docs/hardware.md) page; build the interface.
2. Bench-test the transmit stage with `esphome/ciat-bench.yaml`.
3. Flash `esphome/ciat-emulator.yaml` in listen-only mode, with the original terminal still connected, and check the
   decoding in Home Assistant.
4. Switch to transmission, unplug the terminal, and drive the heat pump from Home Assistant
   ([emulator](docs/emulator.md)).

## Repository layout

```
docs/                  protocol, hardware, emulator, Home Assistant (English + French)
hardware/              interface schematic (SVG), photos of the original terminal
esphome/               ESPHome firmware: emulator (also listen-only) and bench test
tools/                 PC tests of the firmware logic, raw-capture decoder
examples/              Home Assistant dashboard view, notification automation, heat output package
captures/              six raw oscilloscope captures and the decoded table of the whole campaign
```

Run the tests: `g++ -std=c++17 -I esphome tools/test_ciat_nav.cpp -o /tmp/tn && /tmp/tn` (same for
`test_ciat_emul.cpp`; the bus-task test also needs the ESP-IDF stand-ins: add `-I tools/stub_idf` for
`test_ciat_link.cpp`).

## Disclaimer

This is an independent project, **not affiliated with or endorsed by CIAT**. CIAT, Aqualis and Micro-Connect
are trademarks of their respective owners and are used here only to identify the hardware. The information
comes from observing one installation; other firmware versions may behave differently. Nothing here is
guaranteed: modifying your heating system may void its warranty and is done entirely at your own risk.

## License

- Code (firmware, tools, tests): [MIT](LICENSE).
- Documentation, schematics and photos: [CC BY-SA 4.0](LICENSE-docs.md).

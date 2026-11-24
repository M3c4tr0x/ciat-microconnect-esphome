**English** · [Français](protocol.fr.md)

# CIAT Micro-Connect bus protocol

Specification of the two-wire bus between the **CIAT Micro-Connect** room terminal and the **CIAT Aqualis 2**
heat pump (controller board "Micro-Connect"). It was established by reverse engineering in November 2026:
234 oscilloscope captures, then weeks of continuous logging by an ESP32 and, from 23 Nov 2026, by an ESP32
that replaces the terminal.

Status tags used throughout:

- ✅ **confirmed**, by many captures or by the emulator working on the heat pump;
- 🟡 **hypothesis**, consistent with the data but not proven;
- ❓ **unknown**.

Reference installation: controller board version **P80 = 9.00**, terminal version **P81 = 1.02**. Other
versions may differ. The same hardware is reportedly sold under the Atlantic and De Dietrich brands (🟡).

## 1. Physical layer

| Item | Value | Status |
|---|---|---|
| Wiring | 2 wires, carrying **both power and data**, on connector J2 terminals 1-2 of the controller board ("12 V control terminal") | ✅ |
| Polarity | the terminal has a bridge rectifier (MB10S) at its input: **polarity does not matter** | ✅ |
| Idle voltage | ~11.2 V with the terminal connected, 11.6 V with no load | ✅ |
| Heat pump → terminal | the heat pump pulls the bus to **~0 V** during 0 bits | ✅ |
| Terminal → heat pump | the terminal pulls the bus to **~0.5 V** during its 0 bits (open collector) | ✅ |
| Format | **UART 9600 baud, 8 data bits, no parity, 1 stop bit (8N1), LSB first** | ✅ |
| Terminal consumption | ~1.5 mA on average, 1 to 5 mA ripple (22 Ω shunt) | ✅ |

Because both sides transmit by pulling the same line low, **a single receiver on the bus voltage sees both
directions** ✅. The terminal keeps running on its capacitors while the line is low (a diode separates the bus
from its supply reservoir).

> ⚠️ **The bus is not floating.** Its return is tied to earth inside the heat pump, at low impedance in DC.
> Connecting the ground of an earthed oscilloscope to a bus wire **stopped the communication and put the heat
> pump into fault**; only cutting the power of the outdoor unit brought it back. Any interface must be
> **galvanically isolated** (see [hardware](hardware.md)).

## 2. Frames and checksum

- No length field. The frame type is given by its position in the cycle; the first byte is constant.
- ✅ **The last byte is the XOR of all previous bytes** (17,604 of 17,604 frames decoded during the review,
  the only exceptions being frames cut at the edge of a capture).
- The heat pump's requests are 2 bytes: the command, then its XOR, which is the same byte (`14 14`, `12 12`).

## 3. Polling cycle ✅

The heat pump is the master. It repeats this cycle forever:

| # | Direction | Frame | Start → start of the next frame |
|---|---|---|---|
| Q14 | heat pump → terminal | `14 14` | 10.2 to 21.5 ms (median 10.75) |
| 14.1 | terminal → heat pump | `EE dd HH MM xx` (5 bytes) | 24.7 to 48.7 ms |
| Q12 | heat pump → terminal | `12 12` | 10.6 to 22.1 ms (median 11.1) |
| 12.1 | terminal → heat pump | `AC b1 b2 TT Fd 12 xx` (7 bytes) | 13.6 to 18.5 ms |
| 12.2 | heat pump → terminal | `40 p1 p2 d1 d2 d3 d4 xx` (8 bytes) | 140 to 164 ms |

- One cycle lasts 207 to 245 ms, median **~222 ms** (~270 cycles per minute). One byte lasts 1.04 ms.
- The terminal answers **10.2 to 11.2 ms** after the start of the request; ~1.5 % of its answers arrive between
  12 and 22 ms. ✅ **The heat pump accepts an answer at least up to ~22 ms** after the start of the request
  (continuous measurement with edge time-stamping). Beyond that: ❓.
- **No answer** (terminal unplugged): the heat pump moves to the next request after 43 to 55 ms, does not send
  12.2, and keeps polling (`14 14`, `12 12`) every ~106 ms ✅.
- **No other frame exists** ✅: checked on all captures, including start-up, the whole parameter menu, the
  programme editor and every mode.

## 4. Register map

Bits from b7 (left) to b0.

**14.1 — terminal → heat pump: clock** `EE dd HH MM xx`

| Byte | Content | Status |
|---|---|---|
| 0 | `EE`, constant | ❓ meaning |
| 1 | day of week, **1 = Monday … 7 = Sunday** | ✅ |
| 2 | hour, **BCD** (`22` = 22 h) | ✅ |
| 3 | minutes, **BCD** | ✅ |
| 4 | XOR of bytes 0-3 | ✅ |

The clock belongs to the terminal (backed up by a supercapacitor). The heat pump never sets it.

**12.1 — terminal → heat pump: terminal state** `AC b1 b2 TT Fd 12 xx`

| Byte | b7 | b6-b5 | b4 | b3 | b2-b0 |
|---|---|---|---|---|---|
| 1 `b1` mode | 0 | **schedule slot** ✅: `01` comfort, `10` eco, `11` no request | 1 | 1 | **copy of p1 bits 2-0 from the previous cycle** ✅ |

| Byte | Content | Status |
|---|---|---|
| 0 | `AC`, constant | ❓ meaning |
| 2 `b2` | bits 6-3: **dial position** (1 clock, 2 cooling programme, 3 cooling, 4 off, 5 frost protection, 6 heating, 7 heating programme, 8 pool); bit 2 **OK**, bit 1 **−**, bit 0 **+** (1 = pressed) | ✅ |
| 3 `TT` | room temperature, integer part, **BCD** (`23` = 23 °C) | ✅ 13.1 to 31.0 °C; below 10 °C not checked |
| 4 `Fd` | high nibble always `F` (🟡 sign or validity?); low nibble = **tenths** of the room temperature | ✅ tenths; ❓ `F` |
| 5 | `12`, constant; 🟡 terminal version in BCD (P81 = 1.02) | 🟡 |
| 6 | XOR of bytes 0-5 | ✅ |

Common `b1` values ✅: `7E` first frame after power-up (once), `3D` programme / comfort slot,
`5D` programme / eco slot, `78` manual, off, frost protection, pool, cooling, `79` clock,
`7A` cooling programme editor, `7B` heating programme editor.

- The slot field (bits 6-5) differs from `11` only when the mode code is 5 (programme). There, **the terminal
  tells the heat pump which slot of its weekly programme is current**; the heat pump answers with the matching
  comfort or eco bit in p1 within the same cycle ✅.
- **Bits 2-0 of b1 equal bits 2-0 of p1 from the previous cycle** ✅ (3,306 consecutive cycles, no exception).
  The heat pump computes the mode code from the dial and its own state; the terminal echoes it.
- `b2` example: dial on heating, no key = `0 110 000` = `30`; heating + OK = `34`.
- The terminal sends the **raw** state of keys and dial. **The heat pump interprets presses** (setpoint, menu,
  long presses) ✅.

**12.2 — heat pump → terminal: display** `40 p1 p2 d1 d2 d3 d4 xx`

| Byte | b7 | b6 | b5 | b4 | b3 | b2-b0 |
|---|---|---|---|---|---|---|
| 1 `p1` | hourglass, blinking ✅ | **eco** ✅ | **comfort** ✅ | ❓ | **backlight** ✅ | **mode code** ✅ (copied into b1) |
| 2 `p2` | ❓ | **compressor running** ✅ | ❓ | ❓ | ❓ | bit 2 **pool, blinking** ✅; bit 1 **parameter menu** ✅; bit 0 **installer level** ✅ |

| Byte | Content | Status |
|---|---|---|
| 0 | `40`, constant | ❓ meaning |
| 3-6 | the **four digits**, 7-segment code (§5) | ✅ |
| 7 | XOR of bytes 0-6 | ✅ |

- **Mode code** (p1 bits 2-0): 0 manual or other, 1 clock, 2 cooling programme, 3 heating programme, 5 programme,
  6 start-up. ✅ copy direction; 🟡 exact meaning of each code.
- **Backlight** (0x08): set by the heat pump when a key is pressed or the dial moves, cleared ~4 s later ✅.
- **Hourglass** (0x80): blinks in test mode (P30 = 1), as the manual says ✅.
- **Compressor** (p2 0x40): follows the real compressor to within a second or two ✅.
- **Pool** (p2 0x04): blinks (~0.9 s on, 0.9 s off) while waiting for the pool contact ✅.
- **Menu** (p2): `02` user level, `03` installer level ✅.
- p2 bits 0x08, 0x10, 0x20 and 0x80 were never seen. The manual lists pictograms for electric back-up, boiler,
  test mode, load shedding and defrost ❓. **A defrost does not change p2** ✅ (one defrost observed).

## 5. Seven-segment code ✅

bit 0 = segment a (top), bit 1 = b, bit 2 = c, bit 3 = d, bit 4 = e, bit 5 = f, bit 6 = g (middle),
**bit 7 = decimal point** (seen only on d3: a fixed point between the 2nd and 3rd digit).

| Code | Char | Code | Char | Code | Char |
|---|---|---|---|---|---|
| `3F` | 0 | `6D` | 5 | `40` | - |
| `06` | 1 | `7D` | 6 | `58` | c (°C) |
| `5B` | 2 | `07` | 7 | `73` | P |
| `4F` | 3 | `7F` | 8 | `00` | blank |
| `66` | 4 | `6F` | 9 | `5E` | d (🟡 fault codes, never seen) |

Examples: `06 7F ED 58` = "18.5c"; `00 73 4F 06` = "P31"; `40 40 40 40` = "----"; `00 06 BF 5B` = "1.02".

## 6. Who does what ✅

| Terminal | Heat pump |
|---|---|
| clock (day, hour, minute), with backup | comfort, eco, frost protection and pool setpoints |
| weekly heating and cooling programmes, and their display | parameters P00 to P99, including the **heating curve** (P10-P19) |
| current programme slot, comfort or eco (b1) | water setpoint (P42) from the outdoor temperature and room compensation P04 |
| room temperature measurement | compressor control on the **return water temperature**, defrost, faults |
| dial position, raw key state | parameter menu, key timing, **everything the four digits show** |

**The weekly programme lives only in the terminal** and is never transmitted ✅. A replacement terminal must
provide the current slot itself (here: Home Assistant).

## 7. Behaviour

### 7.1 Start-up and connection

- After power-up the terminal is silent for ~1 s, then sends one 12.1 with **b1 = `7E`** ✅.
- The heat pump answers `7E` with a **segment test**: `40 F8 FF FF FF FF 7F C7` ("`.8.8.88`", every
  pictogram on), then normal frames ✅.
- Until it receives a slot, the heat pump shows **eco** in programme mode; it switches to comfort as soon as the
  terminal sends `3D` ✅.
- **Without a terminal, the heat pump keeps running** with its last operating state: circulator on, compressor
  starting and stopping on its own ✅. But **after a power cut, nothing starts until a terminal answers**:
  0 W until the first exchange, which is enough to start the circulator ✅.

### 7.2 Dial

- When the dial moves, **p1 follows after ~5 cycles (~1.1 s)** and b1 copies p1 one cycle later ✅.
- The terminal samples the dial slowly: positions passed through quickly are never sent ✅.
- Displays: frost protection `10.0c`, off `----`, pool = pool setpoint (P23), with the blinking pool bit ✅.

### 7.3 Keys outside the menu (setpoint)

- **+ and − change the setpoint of the current mode by 0.5 °C**: in programme mode, the comfort or eco setpoint
  itself, durably ✅. Heating range **15.0 to 30.0 °C** (saturates at the limits) ✅; the pool setpoint accepts
  at least 20 to 35 °C ✅ (limits not explored further).
- ✅ **A key must be held for at least 3 cycles** (~460 ms) outside the menu: the heat pump reacts during the
  3rd frame. On 2 frames there is no reaction at all (10 presses out of 10). Held longer, the value auto-repeats.
- **OK** toggles between programme, forced comfort and forced eco (b1 becomes `78` in forced modes). It acts
  even when the screen is off ✅.
- + and − do not leave programme mode ✅.

### 7.4 Parameter menu

- **Entry**: + and OK held together. **User level** opens after ~2.95 s (p2 `02`); **installer level** after
  ~5.1 s more, i.e. ~8 s from the start of the press (p2 `03`) ✅. The installer level is the only one showing
  every parameter: P00, P06-P09, P20-P21, P24-P30, P43, P46, P47 and P49, among others, are hidden at user level ✅.
- In the menu, **one frame is enough** for a key, and the heat pump reacts **in the same cycle** (10-20 ms) ✅.
  Held from the 4th frame on, keys auto-repeat (4, 3, 3, 2, 2 cycles, then one step per cycle) ✅.
- **Navigation**: + / − change the parameter number; numbers that do not exist or are hidden are skipped; **the
  list wraps around** (P00 ↔ P99 at installer level, P05 ↔ P81 at user level) ✅.
- **Resume rule**: the menu reopens on the parameter **after the last one viewed** (taking the level into
  account) ✅. A power cut of the heat pump resets it to P05: the position is kept in RAM ✅.
- **OK** on a number shows its value; + / − change it (step 1 or 0.1, **saturating** at its limits, no
  wrap-around) ✅; **OK returns to the number and validates the value** ✅.
- **Exit**: OK held; the menu closes ~12-13 cycles (~2.7-2.9 s) after the start of the press ✅. If a value is
  shown, the held OK first returns to the number, **which validates the value** ✅.
- The heat pump **keeps regulating while the menu is open** (compressor starts and stops, a full defrost seen
  during 5 h of menu sessions) ✅. A schedule slot change that arrives while the menu is open is applied on exit ✅ (one case).
- Measured values are shown **raw**: P50 changes at almost every cycle; average several cycles ✅.

### 7.5 Programme editing (terminal only)

The weekly programme is edited inside the terminal (dial on a programme position). The bus only shows the raw
keys and b1 `7A` / `7B`; **the heat pump does not react to any of these presses** ✅. The editor always opens
on Monday; each short OK moves to the next day; a long OK moves to the next day and copies the day left (no copy
from Sunday to Monday) ✅.

### 7.6 Unexplained observations ❓

- A few **single-cycle 12.2 frames** with a setpoint or value +0.1 (e.g. 22.1c between two 22.0c), valid XOR.
- **`AE` frames** from the original terminal (`AC` with bit 0x02 set in the header and the checksum),
  irregularly (minutes to hours apart, ~1-2 h on average), ignored by the heat pump. **None** since an ESP32 replaced the terminal: 🟡 a quirk of the
  original terminal, not bus noise.

## 8. Parameters

Source: the CIAT Habitat commissioning sheet "Liste des Paramètres Aqualis 2" (2 pages, not redistributed here),
read on the original. **UT** = radiators / fan coils or mixed with underfloor (P03 = 0); **PCR** = underfloor heating
and cooling (P03 = 1). 👤 = shown at user level according to the sheet (on the reference installation the bus
disagrees for P00, P05, P43 and P60, see below). "Read" = live value, not adjustable.

| P | Meaning | Values / range | Factory |
|---|---|---|---|
| P00 👤 | machine version | 1 = Aqualis, 2 = Aqualis 2 | 2 |
| P01 | glycol | 0 = pure water, 1 = glycol water | 0 |
| P02 | digital input J2 terminals 3-5 | 0 = not used, 1 = forces heating / cooling modes, 2 = remote frost protection | 0 |
| P03 | heating emitters | 0 = radiators / fan coils or mixed with underfloor (UT), 1 = underfloor heating and cooling (PCR) | 1 |
| P04 | room compensation coefficient | 0 to 5 | 2 |
| P05 | low-noise operation | 0 = standard, 1 = low noise in cooling, 2 = in heating, 3 = in cooling and heating | 2 |
| P06 | back-up and load shedding | 0 = heat pump only (heat pump shedding possible); 1 = heat pump + electric back-up (back-up shedding); 2 = heat pump + electric back-up (heat pump and back-up shedding); 3 = heat pump **or** boiler (heat pump shedding); 4 = heat pump + boiler (heat pump and boiler shedding); 5 = heat pump + boiler (heat pump shedding) | 0 |
| P07 | outdoor temperature enabling back-up | P09 to 24 °C (shown only with electric back-up, P06 = 1 or 2) | 5 °C |
| P08 | compressor enable | 0 = not allowed to run, 1 = allowed | 1 |
| P09 | low operating limit in heating (air) | −15 to 24 °C | −15 °C |
| P10 👤 | comfort water setpoint, cooling | UT: 10 to 28 °C (P01 = 0), −6 to 28 °C (P01 = 1); PCR: 20 to 25 °C (P22 = 0 or 1), 18 to 25 °C (P22 = 2 or 3) | UT 12 °C / PCR 23 °C |
| P11 👤 | eco water setpoint, cooling | same as P10 | UT 12 °C / PCR 23 °C |
| P12 👤 | outdoor temperature, start of cooling curve | UT: 10 to 40 °C; PCR: not applicable | UT 25 °C |
| P13 👤 | outdoor temperature, end of cooling curve | UT: P12 + 5 to 45 °C; PCR: not applicable | UT 35 °C |
| P14 👤 | max setpoint at end of cooling curve | UT: 10 to 28 °C (P01 = 0), −6 to 28 °C (P01 = 1); PCR: not applicable | UT 15 °C |
| P15 👤 | comfort water setpoint, heating | 20 °C to P19 | UT 30 °C / PCR 20 °C |
| P16 👤 | eco water setpoint, heating | 20 °C to P19 | UT 30 °C / PCR 20 °C |
| P17 👤 | outdoor temperature, start of heating curve | UT: −10 to 30 °C; PCR: 17 to 25 °C | 20 °C |
| P18 👤 | outdoor temperature, end of heating curve | UT: −15 °C to P17 − 5; PCR: −15 to 8 °C | −7 °C |
| P19 👤 | max setpoint at end of heating curve | UT: 20 to 49 °C (P06 = 0), 20 to 55 °C (P06 = 1 or 2), 20 to 70 °C (P06 = 3, 4 or 5); PCR: 20 to 40 °C | UT 40 °C / PCR 35 °C |
| P20 | stage differential | 0.5 to 5 °C | 2 °C |
| P21 | inter-stage differential (shown only with back-up) | 0.5 to 10 °C | 2 °C |
| P22 👤 | pool and hygrometric sensor options | 0 = none, 1 = pool, 2 = hygrometric sensor, 3 = pool + hygrometric sensor | 0 |
| P23 👤 | pool water setpoint | 20 to 35 °C | 28 °C |
| P24 | self-adaptive frosting time | 0 = no, 1 = yes | 0 |
| P25 | coil temperature starting the defrost timer | −5 to 0 °C | −2 °C |
| P26 | minimum return water to allow defrost | UT: 25 °C (P01 = 0), 10 °C (P01 = 1); PCR: 20 °C (P01 = 0), 5 °C (P01 = 1) | 20 °C |
| P27 | coil temperature ending defrost | 10 to 45 °C | 35 °C |
| P28 | frosting time | 10 to 90 min | 30 min |
| P29 | compressor stop at the end of defrost | 0 = no stop, 1 = 3-min stop (the manual adds 2 = 1-min stop, default 2) | 0 |
| P30 | test mode (safety delays shortened, cancelled after 1 h, blinking hourglass) | 0 = no, 1 = yes | 0 |
| P31 👤 | frost-protection setpoint (air) | 8 to 15 °C | 10 °C |
| P32 | max refrigerant temperature change at the exchanger (anti-freeze) | 0 to 5 °C | 1 °C |
| P33 | max water outlet temperature change (anti-freeze) | 0 to 1 °C | 0.3 °C |
| P34 | circulator control (shown with a boiler) | 0 = stops while the boiler runs, 1 = runs continuously | 1 |
| P36 | cooling allowed | 0 = cooling forbidden, 1 = cooling allowed | 1 |
| P40 👤 | **read** return water temperature | | |
| P41 👤 | **read** flow water temperature | | |
| P42 👤 | **read** computed water setpoint (`----` when not computed, e.g. frost protection) | | |
| P43 👤 | **read** corrected stage differential | | |
| P44 👤 | **read** refrigerant temperature, outdoor coil | | |
| P45 👤 | **read** refrigerant temperature, exchanger | | |
| P46 | **read** anti-short-cycle timer: countdown shown as **minutes.seconds** (`2.52` = 2 min 52 s), from 3:00 when the compressor stops ✅; values above 3:00 just after a start (4:31, 29 s after one) ❓ | | |
| P47 | **read** remaining frosting time (min) | | |
| P48 👤 | **read** outdoor temperature | | |
| P49 | **read** fault reset counter | | |
| P50 👤 | **read** pool temperature | | |
| P60 | DHW production | 0 = heating / cooling output, 1 = DHW tank heated once a day, 2 = twice a day | 0 |
| P61 | DHW cycle 1 start time (hidden when P60 = 0) | 00:00 to 23:30 | 01:30 |
| P62 | DHW cycle 2 start time | P61 + 6 h to 23:30 | 12:30 |
| P63 | max DHW time of the heat pump when heating is selected | 0:30 to 2:00 | 2:00 |
| P80 👤 | **read** controller board version | | |
| P81 👤 | **read** terminal version | | |
| P93 | evaporator anti-freeze limit on flow water | fixed 3 °C with pure water (P01 = 0); −10 to +3 °C with glycol (P01 = 1) | +3 °C (P01 = 0) / 0 °C (P01 = 1) |
| P94 | exchanger anti-freeze limit on refrigerant | not adjustable | P93 − 5 °C |
| P95 | high return water limit at the exchanger, cooling | not adjustable | 40 °C |
| P96 | high return water limit at the exchanger, heating | not adjustable | 10 °C (P01 = 0) / 5 °C (P01 = 1) |
| P98 | refrigerant | 1 = R410A | 1 |
| P99 | cooling-only / reversible version | 0 = cooling only, 1 = reversible | depends on the model |

Numbers that do not exist: P35, P37-P39, P51-P59, P64-P79, P82-P92, P97 ✅. Parameters hidden by the configuration
(✅ on the bus): P07 and P21 without electric back-up, P34 without a boiler, P61-P63 when P60 = 0.

Bus vs sheet for the user level, on the reference installation (board P80 = 9.00): P05 and P60 are shown at user level
although the sheet gives them no icon; P00 and P43 are not, although it does. The bus is the reference for that board.

### 8.1 Later controller boards (P80 = 22.00)

A later CIAT sheet, "Paramétrage et liste code défauts — µconnect 2 et µconnect 2+ ou 2x, version P80 = V22.00"
(plan 3989622, index 06, 11/2018; [copy hosted by an installer](https://www.lemble-pac.fr/wp-content/uploads/2025/02/aqualis-2-parametre-et-defaut.pdf)), lists **more parameters**. None of them
exists on the reference board (P80 = 9.00): the menu skips P51-P59, P64-P79 and P82-P92 ✅. They depend on the board
version.

| P | Meaning (V22.00) | Values / factory |
|---|---|---|
| P53 | count of d1.4 and d1.4P faults | read, 0 to 9999 |
| P54 | count of d1.2 faults (temporary or permanent) | read |
| P55 / P56 | compressor starts / starts ÷ 10,000 | read |
| P57 / P58 | compressor running hours / hours ÷ 10,000 | read |
| P59 | thermodynamic defrost cycles | read |
| P70 / P71 | min / max fan voltage in defrost (0-10 V output, EC fan) | by model (P82) |
| P73, P74, P76, P77 | fan voltages: low noise / DHW / pool, heating-cooling max and min, cooling start | by model |
| P82 | heat-pump model | 20H, 28H, 35t, 35H, 35Ht, 50t, 50H, 50Ht, 65t, 65Ht, 75t, 75Ht |
| P83 | minimum compressor off time (regulation stop) | 6 to 15 min (single-phase, default 6) or 3 to 15 min (three-phase, default 3) |
| P84 | fan type | AC (triac) or EC (0-10 V) |

Values that **differ from the older sheet** on V22.00 (🟡 not checked on a board of this version):

- P02: 0 = local terminal, or DHW if P60 = 3; 1 = heating / cooling digital inputs; 2 = terminal with absence
  management by frost protection.
- P03: adds 2 = UTR + PCR and 3 = 2 × PCR.
- P19: factory 49 °C (UTR) with P00 = 2.
- P25 / P27: factory −2 °C / 35 °C with R410A (P98 = 1), 0 °C / 40 °C with R407C (P98 = 0).
- P29: minimum compressor stop during defrost cycles, 1 = 3 min (single-phase starter), 2 = 1 min; factory 1.
- P30: 0 = no, 1 = test mode with 1-min minimum stop (three-phase), 2 = with 3-min stop (single-phase).
- P34: adds 2 = economy mode with stop while the boiler runs, 3 = economy mode with circulator stop if P48 > P17 + 2.
- P60: adds 3 = DHW driven by an external contact.
- P98: 0 = R407C, 1 = R410A.

### 8.2 Fault codes

From the same V22.00 sheet ✅ (document); **how they appear on the bus was never observed** ❓. They are shown on the
four digits (`d` = `5E` in 7-segment code, then digits, e.g. "d1.4", "d4.6"); the decoder accepts these codes.

- **Temporary** fault: degraded operation, **blinking** display, automatic reset when conditions are back to normal.
- **Permanent** fault: the unit stops (back-up allowed if configured), **steady** display. Reset: **dial on off, OK
  held for 3 s**.
- In DHW mode, a fault is shown with an extra `S` (displayed like a 5), e.g. "d3. 5", until reset with OK in off
  mode or a new DHW cycle.

| Code | Component | Cause | Level | Result | Reset |
|---|---|---|---|---|---|
| (none) | terminal link | no frame from the terminal | temporary | runs on the last requested mode | automatic when the dialogue is back ✅ (observed) |
| d1.1 | compressor / water flow | three-phase compressor rotation reversed; test mode active (single-phase, P30 = 1); water flow too high (P41 − P40) | temporary, permanent after 5 in 24 h | heat pump off (back-up allowed) | 5 min / manual |
| d1.2 | refrigerant circuit | high pressure while running | temporary, permanent after 5 in 24 h | heat pump off (back-up allowed) | 6 min / manual |
| d1.4 | single-phase soft starter | supply voltage < 202 V (CIAT starter) or < 190 V (RSBS2332A); contact J3 (3-4) closed | temporary | heat pump off; restart delayed 6 min, 30 min, then 1 h within 24 h | automatic |
| d1.4P | soft starter RSBS2332A | incomplete ramp, over-current > 80 A / 1 s | permanent | heat pump off | power off the unit |
| d1.5 | underfloor limit thermostat | floor inlet water > 60 °C | permanent | heat pump off, no back-up | thermostat reset, then manual |
| d2 | 4-way valve | valve stuck; test mode active (single-phase) | temporary / permanent | heat pump off (back-up allowed) | 5 min / manual |
| d3 | circulator | no or low water flow, air, pump failure (differential pressure switch) | temporary / permanent | heat pump off (boiler back-up if P34 = 0) | delay / manual |
| d4.1 | outdoor temperature sensor | open or short-circuited | temporary in cooling, permanent in heating | heat pump off | automatic in cooling / manual in heating |
| d4.2 | water inlet sensor | open or short-circuited | permanent | heat pump off | manual |
| d4.3 | water outlet sensor | open or short-circuited | permanent | heat pump off (back-up allowed) | manual |
| d4.4 | coil refrigerant sensor | open or short-circuited | permanent | heat pump off (back-up allowed) | manual |
| d4.5 | exchanger refrigerant sensor | open or short-circuited | permanent | heat pump off | manual |
| d4.6 | **terminal temperature sensor** | open or short-circuited | temporary | runs on the water setpoint, without room compensation | automatic |
| d4.7 | pool temperature sensor | open or short-circuited | temporary | heat pump off | automatic |
| d5 | operating limits | water ≥ P95 (cooling) or ≤ P96 (heating), outdoor ≥ 43 °C (R407C) or < P09 (heating); return water < P26 preventing defrost (permanent) | temporary / permanent | heat pump off (back-up allowed) | automatic / manual |
| d6 | exchanger frost protection | outlet water probe | temporary / permanent | heat pump off (back-up allowed) | until P41 > P93 + 6 K / manual |
| d7 | exchanger frost protection | outlet water and refrigerant probes, cooling only | temporary / permanent | heat pump off | 15 min / manual |
| d8 | exchanger frost protection | outlet water and refrigerant, cooling and during defrost | permanent | heat pump off | manual |
| d9.1 | terminal EEPROM | dialogue with the terminal | temporary | last saved settings, no weekly programme | automatic or power off |
| d9.2 | controller EEPROM, or new software version | self-diagnosis | temporary | last saved settings, no weekly programme | automatic or power off |

Sensors are 10 kΩ at 25 °C NTCs. d4.6 concerns the **terminal's** room sensor: 🟡 how the heat pump detects it (an
out-of-range value, or the `F` nibble of Fd, §4) is not known; an emulator should always send a plausible room
temperature.

## 9. Not observed yet

Everything above comes from one installation, which **never went through some modes**. Their frames are unknown or
only guessed; a terminal replacement must be checked before relying on them.

| Mode or display | Why it was not seen | What is known |
|---|---|---|
| **Cooling** and cooling programme (dial positions 2 and 3) | cooling forbidden on this installation (P36 = 0) | display `----`; cooling setpoints, pictograms and the cooling programme slot ❓ |
| **Electric back-up / boiler** running | back-up disabled during the campaign (P06 = 0), then boiler (P06 = 3) never triggered while logging | probable p2 pictograms 0x08 / 0x10 / 0x20 / 0x80 ❓ |
| **Domestic hot water** | no DHW option (P60 = 0): P61-P63 hidden | ❓ |
| **Load shedding** (`€` pictogram) | no load-shedding contact wired (🟡) | ❓ |
| **Fault codes** `dX.Y` | no fault occurred while logging | list and meaning known from CIAT (§8.2); frames on the bus ❓ (blinking for a temporary fault: probably an alternating display) |
| **Defrost** pictogram | one defrost observed: **nothing changed on the bus** | 🟡 the heat pump does not report defrost to the terminal |
| Room temperature **below 10 °C** or negative | never reached | the `F` nibble of Fd ❓; the emulator clamps to 10.0 °C |
| Remote frost-protection / heat-cool forcing input (P02) | input not used | ❓ |
| Hygrostat option (P22 = 2) | pool option used instead | ❓ |
| Test mode (P30 = 1) | seen ~16 s only | blinking hourglass (p1 0x80) ✅; effects on regulation ❓ |

If you own an installation in one of these cases, captures of the bus (with the emulator in listen-only mode, its
"New heat-pump frame" and "Last rejected bytes" sensors) would fill these gaps.

## 10. Open questions

| # | Question | Status |
|---|---|---|
| O2 | p2 pictograms never seen (back-up, boiler, load shedding, fault) | ❓ long-term logging |
| O7 | how a `dX.Y` fault code appears on the bus | ❓ codes known (§8.2), frames never seen |
| O10 | meaning of the constant bytes `EE`, `AC`, `12`, `40` | ❓ 🟡 `12` = terminal version |
| O11 | exact cause of the heat-pump fault caused by an earthed probe | ❓ |
| O12 | the `F` nibble of Fd; negative room temperatures | 🟡 |
| — | heat-pump tolerance for late answers beyond ~22 ms | ❓ |
| — | what the heat pump applies after a power cut followed by a long time without terminal | ❓ |

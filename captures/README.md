**English** · [Français](README.fr.md)

# Bus captures

Raw recordings of the CIAT bus made with an oscilloscope (Siglent SDS1104X-E, 2.5 MS/s) on the reference
installation, with the original terminal connected.

## Raw waveforms (`*.npz`)

Six representative captures, ~4 MB each (NumPy `savez_compressed`):

| File | Content |
|---|---|
| `F01_1.npz` | terminal power-up: first 12.1 with b1 = `7E`, then normal cycles |
| `C04_1.npz` | one + press outside the menu: setpoint 15.0c → 15.5c |
| `Z05_1.npz` | dial moved from off to frost protection: `----`, then `10.0c` ~1.1 s later |
| `J05_1.npz` | installer menu: P80 opened (controller version `9.00`) |
| `K03_1.npz` | OK pressed once in programme mode: forced comfort / eco |
| `R05_1.npz` | P50 shown live in the menu: the value changes at every cycle |

Each file holds `c2` (voltage across the bus, both directions), `c1` (a probe on the original terminal's transmit
transistor: terminal frames only) and `meta` (JSON: test, action, scope settings, `t0` = time of the first sample).

Decode one: `python tools/decode_capture.py captures/F01_1.npz` (needs `numpy`). A frame cut at the very start of a
recording shows as "bad XOR".

⚠️ These captures were made with the oscilloscope ground on the terminal's own "−" after its bridge rectifier. It was
tolerated, but **never put an earthed ground on a bus wire** (see [protocol §1](../docs/protocol.md#1-physical-layer)).

## Decoded table (`frames.csv`)

One row per capture of the whole campaign (234 captures): the first 14.1, 12.1 and 12.2 frames, their decoded fields,
and the note taken at the time (`note_fr`, in French).

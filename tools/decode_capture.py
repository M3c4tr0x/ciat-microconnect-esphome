"""Decode a raw oscilloscope capture of the CIAT bus (captures/*.npz) into frames.

Each .npz holds:
  c2   : bus voltage between the two wires (V), sampled at meta["chans"]["2"]["sara"] samples/s;
  c1   : a probe on the original terminal's transmit transistor (only reacts to the terminal's frames);
  meta : JSON string (test id, action performed, scope settings).
Both directions are visible on c2: the heat pump pulls the bus to ~0 V, the terminal to ~0.5 V (see docs/protocol.md).

Usage: python tools/decode_capture.py captures/F01_1.npz [--raw]
Needs Python 3 with numpy.
"""
import json
import sys

import numpy as np

BAUD = 9600
GAP_S = 0.003          # silence that separates two frames
SEG = {0x3F: '0', 0x06: '1', 0x5B: '2', 0x4F: '3', 0x66: '4', 0x6D: '5', 0x7D: '6', 0x07: '7',
       0x7F: '8', 0x6F: '9', 0x40: '-', 0x58: 'c', 0x00: ' ', 0x73: 'P', 0x5E: 'd'}
DIAL = {1: 'clock', 2: 'cooling programme', 3: 'cooling', 4: 'off', 5: 'frost protection', 6: 'heating',
        7: 'heating programme', 8: 'pool'}


def uart_bytes(v, rate, threshold=5.0):
    """8N1 UART decoding of a voltage trace: low (< threshold) = 0 bit. Returns [(time_s, byte)]."""
    low = v < threshold
    bit = rate / BAUD
    out, i, n = [], 1, len(low)
    while i < n:
        # next falling edge (idle high -> start bit low)
        j = np.argmax(low[i:] & ~low[i - 1:-1]) if i < n else 0
        if not low[i + j] or (low[i + j - 1] if i + j > 0 else True):
            break
        s = i + j
        mid = s + bit / 2
        if int(mid + 9 * bit) >= n:
            break
        if not low[int(mid)]:            # false start
            i = s + 1
            continue
        b = 0
        for k in range(8):
            if not low[int(mid + (k + 1) * bit)]:
                b |= 1 << k
        stop_ok = not low[int(mid + 9 * bit)]
        if stop_ok:
            out.append((s / rate, b))
        i = int(mid + 9 * bit)
    return out


def frames(bs):
    """Groups bytes separated by less than GAP_S."""
    out, cur, last = [], [], None
    for t, b in bs:
        if last is not None and t - last > GAP_S + 10 / BAUD:
            out.append(cur)
            cur = []
        cur.append((t, b))
        last = t
    if cur:
        out.append(cur)
    return out


def describe(f):
    """Human-readable decoding of one frame (list of bytes)."""
    x = 0
    for b in f[:-1]:
        x ^= b
    ok = len(f) > 1 and x == f[-1]
    h = f[0]
    if h in (0x14, 0x12) and len(f) == 2:
        return f"request {h:02X}"
    if not ok:
        return "?? bad XOR or unknown frame"
    if h == 0xEE and len(f) == 5:
        return f"14.1 clock: day {f[1]} {f[2]:02X}:{f[3]:02X}"
    if h in (0xAC, 0xAE) and len(f) == 7:
        keys = ''.join(n for n, m in (('+', 1), ('-', 2), ('OK', 4)) if f[2] & m) or 'none'
        return (f"12.1 terminal: b1 {f[1]:02X}, dial {DIAL.get(f[2] >> 3, '?')}, keys {keys}, "
                f"room {f[3]:02X}.{f[4] & 0x0F} °C" + (" (AE variant)" if h == 0xAE else ""))
    if h == 0x40 and len(f) == 8:
        lcd = ''.join(('.' if d & 0x80 else '') + SEG.get(d & 0x7F, '?') for d in f[3:7])
        flags = [n for n, m in (('eco', 0x40), ('comfort', 0x20), ('backlight', 0x08)) if f[1] & m]
        flags += [n for n, m in (('compressor', 0x40), ('pool', 0x04), ('menu', 0x02)) if f[2] & m]
        return f"12.2 heat pump: display [{lcd}] p1 {f[1]:02X} p2 {f[2]:02X} {' '.join(flags)}"
    return "valid XOR, unknown frame"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    d = np.load(sys.argv[1], allow_pickle=False)
    meta = json.loads(str(d['meta']))
    ch = meta['chans']['2']
    rate, t0 = ch['sara'], ch.get('t0', 0.0)
    print(f"{sys.argv[1]}: {meta['test'].get('id')} — {meta['test'].get('action', '')}")
    fs = frames(uart_bytes(d['c2'], rate))
    for f in fs:
        bs = [b for _, b in f]
        line = f"{f[0][0] + t0:9.4f} s  {' '.join(f'{b:02X}' for b in bs):24s}"
        print(line if '--raw' in sys.argv else f"{line} {describe(bs)}")
    print(f"{len(fs)} frames")


if __name__ == '__main__':
    main()

// CIAT Micro-Connect <-> Aqualis 2 bus decoder (listening side).
// Protocol: see docs/protocol.md. UART 9600 8N1, last byte = XOR of the previous ones.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ciat {

// Frame length from its header byte (0 = unknown header).
inline int frame_len(uint8_t h) {
  switch (h) {
    case 0x14: case 0x12: return 2;  // heat-pump requests: repeated byte
    case 0xEE: return 5;             // 14.1 terminal: EE day HH MM xor
    case 0xAC: return 7;             // 12.1 terminal: AC b1 b2 TT Fd 12 xor
    case 0xAE: return 7;             // 12.1 variant (bit 0x02 of the header), sent now and then by the original terminal
    case 0x40: return 8;             // 12.2 heat pump: 40 p1 p2 d1 d2 d3 d4 xor
    default: return 0;
  }
}

inline bool bcd_ok(uint8_t v) { return (v >> 4) <= 9 && (v & 0x0F) <= 9; }
inline int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

// Content check on top of the XOR: avoids false positives while resynchronising
// (1 chance in 256 per attempt with the XOR alone). Constraints observed on ~3,500 frames of each type.
inline bool content_ok(const uint8_t *f) {
  switch (f[0]) {
    case 0xEE:  // day 1..7, hour and minutes in valid BCD
      return f[1] >= 1 && f[1] <= 7 && bcd_ok(f[2]) && bcd(f[2]) < 24 && bcd_ok(f[3]) && bcd(f[3]) < 60;
    case 0xAC:
    case 0xAE:  // byte 5 = 12, b1 bits 3-4 set, b2 bit 7 clear; the high nibble of Fd is not checked (unknown below 10 °C)
      return f[5] == 0x12 && (f[1] & 0x18) == 0x18 && !(f[2] & 0x80);
    case 0x40: {  // 12.2: the 4 digits must be known 7-segment codes (bit 7 = point, ignored)
      static const uint8_t ok[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
                                   0x40, 0x58, 0x73, 0x5E, 0x00};
      for (int i = 3; i < 7; i++) {
        bool found = false;
        for (uint8_t c : ok) found |= (f[i] & 0x7F) == c;
        if (!found) return false;
      }
      return true;  // p1/p2 free: the segment test at start-up sets them to FF/F8
    }
    default:
      return true;
  }
}

// Splits the byte stream into frames, by header + length + XOR + content.
// Resynchronises by itself: a byte that does not start a valid frame is dropped.
struct Parser {
  uint8_t buf[32];
  size_t n = 0;
  uint32_t ok = 0, bad = 0;
  // Consecutive rejected bytes: accumulated until the next valid frame, then made available in rej_out
  // ("Last rejected bytes" diagnostic). Beyond 32 bytes only the count goes on.
  uint8_t rej[32], rej_out[32];
  size_t nrej = 0, nrej_out = 0;
  uint32_t total_rej = 0, total_rej_out = 0;
  bool rej_ready = false;
  // Last valid frame of each type (12.1, 12.2, 14.1), and its copy taken at the start of the rejected block:
  // used to show which bits differ.
  uint8_t ref[3][8] = {}, ref_rej[3][8] = {};
  bool has_ref[3] = {}, has_ref_rej[3] = {};
  static int idx(uint8_t h) { return h == 0xAC ? 0 : h == 0x40 ? 1 : h == 0xEE ? 2 : -1; }

  void drop(size_t k) {
    memmove(buf, buf + k, n - k);
    n -= k;
  }
  void reject(size_t k) {
    if (nrej == 0 && total_rej == 0) {
      memcpy(ref_rej, ref, sizeof ref);
      memcpy(has_ref_rej, has_ref, sizeof has_ref);
    }
    for (size_t i = 0; i < k; i++)
      if (nrej < sizeof(rej)) rej[nrej++] = buf[i];
    total_rej += k;
    bad++;
    drop(k);
  }

  // Returns true when a complete, valid frame is available in out[0..len).
  bool feed(uint8_t b, uint8_t *out, size_t &len) {
    if (n >= sizeof(buf)) reject(n);
    buf[n++] = b;
    while (n > 0) {
      int L = frame_len(buf[0]);
      if (L == 0) { reject(1); continue; }
      if ((int) n < L) return false;
      bool good;
      if (L == 2) {
        good = buf[1] == buf[0];
      } else {
        uint8_t x = 0;
        for (int i = 0; i < L - 1; i++) x ^= buf[i];
        good = x == buf[L - 1] && content_ok(buf);
      }
      if (good) {
        memcpy(out, buf, L);
        len = L;
        int k = idx(buf[0]);
        if (k >= 0) { memcpy(ref[k], buf, L); has_ref[k] = true; }
        drop(L);
        ok++;
        if (total_rej) {
          memcpy(rej_out, rej, nrej);
          nrej_out = nrej;
          total_rej_out = total_rej;
          rej_ready = true;
          nrej = 0;
          total_rej = 0;
        }
        return true;
      }
      reject(1);
    }
    return false;
  }
};

// Single parser instance. It is not declared in ESPHome "globals": since ESPHome 2026.x, globals are
// declared before this file is included.
inline Parser &parser() {
  static Parser p;
  return p;
}

// 7-segment display: bit0 = a ... bit6 = g, bit7 = point before the digit.
inline char seg(uint8_t v) {
  switch (v & 0x7F) {
    case 0x3F: return '0'; case 0x06: return '1'; case 0x5B: return '2'; case 0x4F: return '3';
    case 0x66: return '4'; case 0x6D: return '5'; case 0x7D: return '6'; case 0x07: return '7';
    case 0x7F: return '8'; case 0x6F: return '9'; case 0x40: return '-'; case 0x58: return 'c';
    case 0x73: return 'P'; case 0x5E: return 'd'; case 0x00: return ' ';
    default: return '?';
  }
}

inline std::string lcd(const uint8_t *d) {
  std::string s;
  for (int i = 0; i < 4; i++) {
    if (d[i] & 0x80) s += '.';
    s += seg(d[i]);
  }
  return s;
}

// Numeric value of the display ("18.5c" -> 18.5). False for "----", "P31", etc.
inline bool lcd_number(const std::string &s, float &v) {
  std::string t;
  bool digit = false;
  for (char c : s) {
    if (c >= '0' && c <= '9') { t += c; digit = true; }
    else if (c == '.' || c == '-') t += c;
    else if (c != ' ' && c != 'c') return false;
  }
  if (!digit) return false;
  v = atof(t.c_str());
  return true;
}

inline const char *dial_name(uint8_t b2) {
  static const char *m[] = {"?", "clock", "cooling programme", "cooling", "off",
                            "frost protection", "heating", "heating programme", "pool"};
  uint8_t k = (b2 >> 3) & 0x0F;
  return k <= 8 ? m[k] : "?";
}

inline std::string keys_text(uint8_t b2) {
  std::string s;
  if (b2 & 0x01) s += "+ ";
  if (b2 & 0x02) s += "- ";
  if (b2 & 0x04) s += "OK ";
  if (s.empty()) return "none";
  s.pop_back();
  return s;
}

inline const char *mode_name(uint8_t b1) {
  switch (b1) {
    case 0x3D: return "programme comfort";
    case 0x5D: return "programme eco";
    case 0x78: return "manual / other";
    case 0x79: return "clock setting";
    case 0x7E: return "start-up";  // first frame after the terminal powers up
    case 0x7A: return "cooling programme editor";
    case 0x7B: return "heating programme editor";
    default: return "unknown";
  }
}

inline std::string clock_text(const uint8_t *f) {
  static const char *d[] = {"?", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  char t[16];
  snprintf(t, sizeof t, "%s %02d:%02d", f[1] <= 7 ? d[f[1]] : "?", bcd(f[2]), bcd(f[3]));
  return t;
}

inline std::string hex(const uint8_t *f, size_t L) {
  std::string s;
  char t[4];
  for (size_t i = 0; i < L; i++) {
    snprintf(t, sizeof t, i ? " %02X" : "%02X", f[i]);
    s += t;
  }
  return s;
}

// Text of the "Last rejected bytes" sensor: byte count and bytes in hex, then
// - [valid XOR: content refused] if the block starts with a known-header frame whose XOR is right;
// - "≈ 12.x: oN ^MM …": comparison with the closest last valid frame (at most 6 bits apart), showing for each
//   byte the bits that differ, and whether the block's XOR is right.
inline std::string reject_text(const Parser &p) {
  const uint8_t *r = p.rej_out;
  size_t n = p.nrej_out;
  char t[48];
  snprintf(t, sizeof t, "%u byte%s: ", (unsigned) p.total_rej_out, p.total_rej_out > 1 ? "s" : "");
  std::string s = t + hex(r, n);
  if (p.total_rej_out > n) s += " …";
  int L = n ? frame_len(r[0]) : 0;
  if (L > 2 && (int) n >= L) {
    uint8_t x = 0;
    for (int i = 0; i < L - 1; i++) x ^= r[i];
    if (x == r[L - 1]) s += " [valid XOR: content refused]";
  }
  static const uint8_t hdr[3] = {0xAC, 0x40, 0xEE};
  static const char *name[3] = {"12.1", "12.2", "14.1"};
  int best = -1, best_bits = 99;
  for (int k = 0; k < 3; k++) {
    int Lk = frame_len(hdr[k]);
    if (!p.has_ref_rej[k] || (int) n < Lk) continue;
    int bits = 0;
    for (int i = 0; i < Lk; i++) bits += __builtin_popcount(r[i] ^ p.ref_rej[k][i]);
    if (bits < best_bits) { best_bits = bits; best = k; }
  }
  if (best >= 0 && best_bits <= 6) {
    int Lk = frame_len(hdr[best]);
    s += std::string(" | ≈ ") + name[best] + ":";
    for (int i = 0; i < Lk; i++) {
      uint8_t d = r[i] ^ p.ref_rej[best][i];
      if (d) { snprintf(t, sizeof t, " o%d ^%02X", i, d); s += t; }
    }
    uint8_t x = 0;
    for (int i = 0; i < Lk - 1; i++) x ^= r[i];
    s += x == r[Lk - 1] ? ", valid XOR" : ", wrong XOR";
  }
  return s;
}

// Passive tracking of a key press: how many 12.1 frames carry the key, and did the heat pump react?
// Reaction = change of p1 (backlight 0x08 ignored, but p1 published raw) or of the display, compared with the last
// 12.2 received before the press. The report is given WAIT heat-pump frames after release (the heat pump reacts
// ~0.45 s after the press outside the menu), or at once if a new press arrives first.
struct Press {
  static const int WAIT = 4;   // ~0.9 s
  bool active = false, released = false, reacted = false, has_hp = false, has_before = false;
  uint8_t keys = 0, p1_now = 0, p1_before = 0, p1_after = 0;
  uint32_t cycles = 0, t0 = 0, t_react = 0;
  int wait = 0;
  std::string disp_now, disp_before, disp_after;

  std::string report() const {
    char t[48];
    snprintf(t, sizeof t, " on %u 12.1 frame%s", (unsigned) cycles, cycles > 1 ? "s" : "");
    std::string s = keys_text(keys) + t;
    if (!has_before) return s + " | heat pump state before unknown";
    if (!reacted) {
      snprintf(t, sizeof t, "%02X", p1_before);
      return s + " | no reaction (p1 " + t + ", " + disp_before + ")";
    }
    snprintf(t, sizeof t, " | reaction after %u ms: p1 %02X→%02X, ", (unsigned) t_react, p1_before, p1_after);
    return s + t + disp_before + "→" + disp_after;
  }

  // Call for each 12.1 (AC). Returns true and fills out when a previous press ends.
  bool terminal(uint8_t b2, uint32_t now, std::string &out) {
    uint8_t k = b2 & 0x07;
    bool done = false;
    if (k && active && released) { out = report(); done = true; active = false; }  // new press before the report
    if (k && !active) {
      active = true; released = false; reacted = false;
      keys = k; cycles = 1; t0 = now;
      p1_before = p1_now; disp_before = disp_now; has_before = has_hp;
    } else if (k && !released) {
      keys |= k; cycles++;
    } else if (!k && active && !released) {
      released = true; wait = WAIT;
    }
    return done;
  }

  // Call for each 12.2 (40).
  bool heat_pump(const uint8_t *f, const std::string &disp, uint32_t now, std::string &out) {
    uint8_t p1 = f[1];  // raw (for display); compared without the backlight bit
    if (active && !reacted && has_before && ((p1 ^ p1_before) & ~0x08 || disp != disp_before)) {
      reacted = true; t_react = now - t0; p1_after = p1; disp_after = disp;
    }
    p1_now = p1; disp_now = disp; has_hp = true;
    if (active && released && --wait <= 0) { out = report(); active = false; return true; }
    return false;
  }
};

inline Press &press() {
  static Press p;
  return p;
}

// Watch of the heat pump's 12.2 frames, for unknown phases and isolated frames:
//   - single-cycle frame: present for one cycle only, different from the one before and the one after;
//   - new frame: never seen since the ESP started (table of MAX_SEEN frames, then no more detection).
// Comparisons ignore the XOR and the pool blinking bit (p2 0x04, toggling every ~0.9 s).
// Values shown in the menu: a P40-P50 measurement is sent as is at every cycle and wobbles by a tenth
// (12.0c 12.1c 12.0c). This is not an anomaly, so:
//   - single-cycle: not reported if the three cycles are in the menu with the same p1 and p2 and the middle one
//     shows a value (not a "Pnn" number); a change of p1 or p2 is still reported;
//   - new: the digits of a menu value are ignored, only the p1/p2 pair counts (otherwise the 256-frame table
//     fills up in a morning of readings, and nothing is detected afterwards).
struct HpWatch {
  static const int MAX_SEEN = 256;
  uint8_t seen[MAX_SEEN][7];
  int n_seen = 0;
  uint8_t prev[8], cur[8];
  bool has_prev = false, has_cur = false;
  uint32_t cycles_cur = 0, n_single = 0, n_wobble = 0;  // wobble: single-cycle frames ignored (menu)

  static bool same(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 7; i++)
      if ((a[i] ^ b[i]) & (i == 2 ? 0xFB : 0xFF)) return false;
    return true;
  }
  static bool menu(const uint8_t *f) { return f[2] & 0x02; }
  static bool number_shown(const uint8_t *f) {  // "Pnn": one digit is P (0x73, decimal point ignored)
    for (int i = 3; i < 7; i++)
      if ((f[i] & 0x7F) == 0x73) return true;
    return false;
  }
  static bool menu_value(const uint8_t *f) { return menu(f) && !number_shown(f); }
  static bool same_p1_p2(const uint8_t *a, const uint8_t *b) {
    return a[1] == b[1] && ((a[2] ^ b[2]) & 0xFB) == 0;
  }
  // Key in the table of seen frames: digits cleared for a menu value.
  static void key(const uint8_t *f, uint8_t *k) {
    memcpy(k, f, 7);
    if (menu_value(f)) memset(k + 3, 0, 4);
  }
  bool full() const { return n_seen >= MAX_SEEN; }

  // Call for each valid 12.2. Returns 1 if the frame is new (text in new_text), 2 if the previous frame lasted a
  // single cycle (text in single_text), 3 if both.
  int frame(const uint8_t *f, std::string &new_text, std::string &single_text) {
    int r = 0;
    uint8_t k[7];
    key(f, k);
    bool known = false;
    for (int i = 0; i < n_seen && !known; i++) known = same(seen[i], k);
    if (!known && !full()) {
      memcpy(seen[n_seen++], k, 7);
      new_text = hex(f, 8) + " [" + lcd(f + 3) + "]" + (has_cur ? " | before " + hex(cur, 8) : std::string());
      r |= 1;
    }
    if (has_cur && same(cur, f)) {
      cycles_cur++;
      return r;
    }
    bool wobble = has_prev && has_cur && cycles_cur == 1 && menu(prev) && menu(f) && menu_value(cur) &&
                  same_p1_p2(prev, cur) && same_p1_p2(cur, f);
    if (wobble) {
      n_wobble++;
    } else if (has_prev && has_cur && cycles_cur == 1) {
      n_single++;
      single_text = hex(prev, 8) + " → " + hex(cur, 8) + " [" + lcd(cur + 3) + "] (1 cycle) → " + hex(f, 8);
      if (same(prev, f)) single_text += " (back to the previous frame)";
      r |= 2;
    }
    if (has_cur) { memcpy(prev, cur, 8); has_prev = true; }
    memcpy(cur, f, 8);
    has_cur = true;
    cycles_cur = 1;
    return r;
  }
};

inline HpWatch &hp_watch() {
  static HpWatch w;
  return w;
}

}  // namespace ciat

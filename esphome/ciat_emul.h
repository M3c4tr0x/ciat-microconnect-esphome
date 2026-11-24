// Micro-Connect terminal emulation: builds the 14.1 and 12.1 answers.
// Pure logic, no ESP dependency: tested on a PC by tools/test_ciat_emul.cpp.
// Protocol: docs/protocol.md §4. Tags in brackets: ✅ confirmed, 🟡 hypothesis.
#pragma once
#include <cmath>
#include <cstdint>

namespace ciat {
namespace emu {

// What Home Assistant asks of the virtual terminal.
struct Command {
  uint8_t dial = 6;        // 1 clock, 2 cooling programme, 3 cooling, 4 off, 5 frost protection, 6 heating,
                           // 7 heating programme, 8 pool [✅]
  uint8_t slot = 2;        // programme slot when the heat pump is in programme mode: 1 comfort, 2 eco [✅ 3D / 5D]
  float room_temp = NAN;   // °C; NAN = unknown: no 12.1 answer
  bool clock_ok = false;   // false until the time is known: no 14.1 answer
  uint8_t day = 1;         // 1 = Monday … 7 = Sunday [✅]
  uint8_t hour = 0, minute = 0;
};

inline uint8_t to_bcd(int v) { return (uint8_t) (((v / 10) << 4) | (v % 10)); }

inline uint8_t frame_xor(const uint8_t *f, int n) {
  uint8_t x = 0;
  for (int i = 0; i < n; i++) x ^= f[i];
  return x;
}

// Accepted room temperature range. Checked from 13.1 to 31.0 °C; below 10 °C the high nibble of Fd (always F) is
// unknown, hence the 10.0 °C floor [🟡]. Above 39.9 °C the BCD tens would still be valid but were never seen.
static const float ROOM_MIN = 10.0f, ROOM_MAX = 39.9f;

// Room temperature -> bytes TT (BCD, integer part) and Fd (F + tenths) [✅ format; 🟡 meaning of the F].
inline bool room_temp_bytes(float t, uint8_t &tt, uint8_t &fd) {
  if (std::isnan(t)) return false;
  if (t < ROOM_MIN) t = ROOM_MIN;
  if (t > ROOM_MAX) t = ROOM_MAX;
  int d = (int) lroundf(t * 10.0f);
  tt = to_bcd(d / 10);
  fd = (uint8_t) (0xF0 | (d % 10));
  return true;
}

// 14.1: EE dd HH MM xx [✅]
inline bool frame_141(const Command &c, uint8_t out[5]) {
  if (!c.clock_ok || c.day < 1 || c.day > 7 || c.hour > 23 || c.minute > 59) return false;
  out[0] = 0xEE;
  out[1] = c.day;
  out[2] = to_bcd(c.hour);
  out[3] = to_bcd(c.minute);
  out[4] = frame_xor(out, 4);
  return true;
}

// Mode code to copy into b1: bits 2-0 of p1 in the previous 12.2 [✅ 3,306 cycles, no exception],
// or 6 (start-up, b1 = 7E) as long as no 12.2 was received [✅ 7E after power-up; 🟡 reason].
inline uint8_t mode_code(bool has_p1, uint8_t p1) { return has_p1 ? (p1 & 0x07) : 6; }

// b1 = 0 | slot (bits 6-5) | 1 1 | code (bits 2-0).
// Slot: 01 comfort / 10 eco only when the code is 5 (programme), otherwise 11 [✅ on every value seen].
inline uint8_t byte_b1(uint8_t code, uint8_t slot) {
  uint8_t s = (code == 5) ? (slot == 1 ? 0x20 : 0x40) : 0x60;
  return (uint8_t) (s | 0x18 | (code & 0x07));
}

// b2 = 0 | dial (bits 6-3) | OK | − | + [✅]
inline uint8_t byte_b2(uint8_t dial, uint8_t keys) {
  return (uint8_t) (((dial & 0x0F) << 3) | (keys & 0x07));
}

// 12.1: AC b1 b2 TT Fd 12 xx. False if the command is incomplete (unknown room temperature, dial out of range).
inline bool frame_121(const Command &c, uint8_t code, uint8_t keys, uint8_t out[7]) {
  if (c.dial < 1 || c.dial > 8) return false;
  uint8_t tt, fd;
  if (!room_temp_bytes(c.room_temp, tt, fd)) return false;
  out[0] = 0xAC;
  out[1] = byte_b1(code, c.slot);
  out[2] = byte_b2(c.dial, keys);
  out[3] = tt;
  out[4] = fd;
  out[5] = 0x12;  // constant [✅ 3,526 frames]; 🟡 terminal version
  out[6] = frame_xor(out, 6);
  return true;
}

// "Shadow" mode: checks the parts of the real terminal's 12.1 that the emulator must reproduce exactly,
// regardless of the user's choices (dial, slot, keys, room temperature). Returns a bit mask of differences:
// 1 = b1 bits 2-0 differ from the expected code, 2 = b1 bits 4-3 not 11 or bit 7 set,
// 4 = slot inconsistent with the code, 8 = byte 5 not 12, 16 = high nibble of Fd not F, 32 = wrong XOR.
inline uint8_t shadow_diffs(const uint8_t f[7], uint8_t expected_code) {
  uint8_t e = 0;
  if ((f[1] & 0x07) != (expected_code & 0x07)) e |= 1;
  if ((f[1] & 0x98) != 0x18) e |= 2;
  uint8_t s = f[1] & 0x60;
  if ((f[1] & 0x07) == 5 ? (s != 0x20 && s != 0x40) : (s != 0x60)) e |= 4;
  if (f[5] != 0x12) e |= 8;
  if ((f[4] & 0xF0) != 0xF0) e |= 16;
  if (frame_xor(f, 6) != f[6]) e |= 32;
  return e;
}

}  // namespace emu
}  // namespace ciat

// PC test of esphome/ciat_emul.h (and of the heat-pump frame watch in ciat_bus.h) against real frames logged by the
// ESP32 and listed in docs/protocol.md.
// Build and run: g++ -std=c++17 -Wall -I esphome tools/test_ciat_emul.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstring>
#include "ciat_bus.h"
#include "ciat_emul.h"

using namespace ciat::emu;
static int failures = 0;

static void check(const char *name, const uint8_t *got, const uint8_t *expected, int n) {
  bool ok = memcmp(got, expected, n) == 0;
  printf("%s %-44s", ok ? "OK  " : "FAIL", name);
  for (int i = 0; i < n; i++) printf(" %02X", got[i]);
  if (!ok) {
    printf("   expected");
    for (int i = 0; i < n; i++) printf(" %02X", expected[i]);
    failures++;
  }
  printf("\n");
}

// Rebuilds a real 12.1 from its decoded fields and compares byte by byte.
static void case_121(const char *name, uint8_t dial, uint8_t slot, float room, bool has_p1, uint8_t p1, uint8_t keys,
                     const uint8_t expected[7]) {
  Command c;
  c.dial = dial;
  c.slot = slot;
  c.room_temp = room;
  uint8_t f[7];
  if (!frame_121(c, mode_code(has_p1, p1), keys, f)) { printf("FAIL %s: frame refused\n", name); failures++; return; }
  check(name, f, expected, 7);
  uint8_t e = shadow_diffs(expected, mode_code(has_p1, p1));
  if (e) { printf("FAIL %s: shadow differences %02X on a real frame\n", name, e); failures++; }
}

int main() {
  // Real frames. p1 = the one of the previous cycle's 12.2.
  { const uint8_t a[7] = {0xAC, 0x78, 0x40, 0x22, 0xF6, 0x12, 0x52};  // pool, heat pump p1 00, 22.6 °C
    case_121("pool 22.6 °C, p1 00", 8, 2, 22.6f, true, 0x00, 0, a); }
  { const uint8_t a[7] = {0xAC, 0x5D, 0x40, 0x22, 0xF7, 0x12, 0x76};  // 1st pool cycle, p1 0D, eco slot
    case_121("pool, p1 0D (programme), eco", 8, 2, 22.7f, true, 0x0D, 0, a); }
  { const uint8_t a[7] = {0xAC, 0x7B, 0x38, 0x22, 0xF6, 0x12, 0x29};  // heating programme, p1 4B
    case_121("heating programme, p1 4B", 7, 2, 22.6f, true, 0x4B, 0, a); }
  { const uint8_t a[7] = {0xAC, 0x78, 0x28, 0x22, 0xF7, 0x12, 0x3B};  // frost protection, p1 00
    case_121("frost protection 22.7 °C, p1 00", 5, 2, 22.7f, true, 0x00, 0, a); }
  { const uint8_t a[7] = {0xAC, 0x7E, 0x30, 0x22, 0xF3, 0x12, 0x21};  // start-up, no 12.2 received yet
    case_121("start-up (7E), heating 22.3 °C", 6, 2, 22.3f, false, 0, 0, a); }
  { const uint8_t a[7] = {0xAC, 0x7E, 0x08, 0x20, 0xF4, 0x12, 0x1C};  // start-up, dial on clock
    case_121("start-up (7E), clock 20.4 °C", 1, 2, 20.4f, false, 0, 0, a); }
  { const uint8_t a[7] = {0xAC, 0x78, 0x28, 0x23, 0xF0, 0x12, 0x3D};  // frost protection 23.0 °C
    case_121("frost protection 23.0 °C", 5, 2, 23.0f, true, 0x00, 0, a); }

  // Programme comfort / eco and OK key (protocol.md §4)
  { Command c; c.dial = 6; c.room_temp = 21.0f;
    uint8_t f[7];
    c.slot = 1; frame_121(c, 5, 0, f);
    printf("%s b1 programme comfort = %02X (expected 3D)\n", f[1] == 0x3D ? "OK  " : "FAIL", f[1]); failures += f[1] != 0x3D;
    c.slot = 2; frame_121(c, 5, 0, f);
    printf("%s b1 programme eco     = %02X (expected 5D)\n", f[1] == 0x5D ? "OK  " : "FAIL", f[1]); failures += f[1] != 0x5D;
    frame_121(c, 5, 0x04, f);
    printf("%s b2 heating + OK      = %02X (expected 34)\n", f[2] == 0x34 ? "OK  " : "FAIL", f[2]); failures += f[2] != 0x34;
    frame_121(c, 1, 0, f);
    printf("%s b1 clock code        = %02X (expected 79)\n", f[1] == 0x79 ? "OK  " : "FAIL", f[1]); failures += f[1] != 0x79; }

  // Room temperature: rounding, floor and unknown
  { uint8_t tt, fd;
    room_temp_bytes(19.96f, tt, fd);
    printf("%s 19.96 °C -> %02X %02X (expected 20 F0)\n", tt == 0x20 && fd == 0xF0 ? "OK  " : "FAIL", tt, fd); failures += !(tt == 0x20 && fd == 0xF0);
    room_temp_bytes(4.2f, tt, fd);
    printf("%s 4.2 °C -> %02X %02X (floor 10.0: expected 10 F0)\n", tt == 0x10 && fd == 0xF0 ? "OK  " : "FAIL", tt, fd); failures += !(tt == 0x10 && fd == 0xF0);
    Command c; uint8_t f[7];
    bool r = frame_121(c, 0, 0, f);
    printf("%s unknown room temperature -> no 12.1\n", !r ? "OK  " : "FAIL"); failures += r; }

  // 14.1: Wednesday 23:06 -> EE 03 23 06 xx
  { Command c; c.clock_ok = true; c.day = 3; c.hour = 23; c.minute = 6;
    uint8_t f[5];
    frame_141(c, f);
    const uint8_t a[5] = {0xEE, 0x03, 0x23, 0x06, (uint8_t) (0xEE ^ 0x03 ^ 0x23 ^ 0x06)};
    check("14.1 Wed 23:06", f, a, 5);
    c.clock_ok = false;
    bool r = frame_141(c, f);
    printf("%s unknown time -> no 14.1\n", !r ? "OK  " : "FAIL"); failures += r; }

  // Shadow: a 12.1 whose b1 does not copy p1 must be reported
  { const uint8_t f[7] = {0xAC, 0x78, 0x30, 0x22, 0xF6, 0x12, (uint8_t) (0xAC ^ 0x78 ^ 0x30 ^ 0x22 ^ 0xF6 ^ 0x12)};
    uint8_t e = shadow_diffs(f, 5);
    printf("%s shadow: code 0 instead of 5 -> difference %02X (expected 01)\n", e == 1 ? "OK  " : "FAIL", e); failures += e != 1; }

  // Heat-pump frame watch: isolated frame (one cycle of 22.1c in the middle of 22.6c)
  { ciat::HpWatch v;
    uint8_t a[8] = {0x40, 0x45, 0x40, 0x5B, 0x5B, 0xFD, 0x58, 0};  // display 22.6c, XOR set below
    uint8_t b[8] = {0x40, 0x25, 0x40, 0x5B, 0x5B, 0x86, 0x58, 0};
    a[7] = frame_xor(a, 7); b[7] = frame_xor(b, 7);
    uint8_t a_bl[8]; memcpy(a_bl, a, 8); a_bl[2] |= 0x04; a_bl[7] = frame_xor(a_bl, 7);  // pool blinking
    std::string nw, single;
    int r1 = v.frame(a, nw, single);
    int r2 = v.frame(a_bl, nw, single);
    int r3 = v.frame(a, nw, single);
    int r4 = v.frame(b, nw, single);
    int r5 = v.frame(a, nw, single);
    bool ok = r1 == 1 && r2 == 0 && r3 == 0 && r4 == 1 && r5 == 2 && v.n_single == 1 && v.n_seen == 2 &&
              single.find("back to the previous frame") != std::string::npos;
    printf("%s watch: new, blinking ignored, single-cycle frame (r = %d %d %d %d %d)\n", ok ? "OK  " : "FAIL", r1, r2, r3, r4, r5);
    printf("      %s\n", single.c_str());
    failures += !ok;
    // A lasting change (two cycles or more) is not single-cycle
    v.frame(b, nw, single); v.frame(b, nw, single);
    int r6 = v.frame(a, nw, single);
    printf("%s watch: change over 2 cycles not reported (r = %d)\n", r6 == 0 ? "OK  " : "FAIL", r6); failures += r6 != 0; }

  // Watch, real frames. Replays prev, cur (1 cycle), next, doubling prev so that it lasts several cycles, and checks
  // whether cur is reported as single-cycle.
  { auto replay = [](ciat::HpWatch &v, const uint8_t *p, const uint8_t *c, const uint8_t *s) {
      std::string nw, single;
      v.frame(p, nw, single); v.frame(p, nw, single); v.frame(c, nw, single);
      return (v.frame(s, nw, single) & 2) != 0;
    };
    struct Case { const char *name; uint8_t p[8], c[8], s[8]; bool expected; };
    const Case cases[] = {
      {"P41 12.0c 12.1c 12.0c (wobble, ignored)",
       {0x40, 0x00, 0x02, 0x06, 0x5B, 0xBF, 0x58, 0xF8}, {0x40, 0x00, 0x02, 0x06, 0x5B, 0x86, 0x58, 0xC1},
       {0x40, 0x00, 0x02, 0x06, 0x5B, 0xBF, 0x58, 0xF8}, false},
      {"menu + compressor 27.6c 27.7c (wobble, ignored)",
       {0x40, 0x00, 0x42, 0x5B, 0x07, 0xFD, 0x58, 0xFB}, {0x40, 0x00, 0x42, 0x5B, 0x07, 0x87, 0x58, 0x81},
       {0x40, 0x00, 0x42, 0x5B, 0x07, 0xFD, 0x58, 0xFB}, false},
      {"P48, p1 bit 0x08 missing for 1 cycle (reported)",
       {0x40, 0x08, 0x42, 0x00, 0x73, 0x66, 0x7F, 0x60}, {0x40, 0x00, 0x42, 0x00, 0x73, 0x66, 0x7F, 0x68},
       {0x40, 0x08, 0x42, 0x00, 0x07, 0xFD, 0x58, 0xA8}, true},
      {"eco 18.5c 18.6c 18.5c outside the menu (reported)",
       {0x40, 0x45, 0x00, 0x06, 0x7F, 0xED, 0x58, 0xC9}, {0x40, 0x45, 0x00, 0x06, 0x7F, 0xFD, 0x58, 0xD9},
       {0x40, 0x45, 0x00, 0x06, 0x7F, 0xED, 0x58, 0xC9}, true},
      {"leaving the menu, eco for 1 cycle (reported)",
       {0x40, 0x00, 0x02, 0x5B, 0x7F, 0xDB, 0x58, 0xE5}, {0x40, 0x45, 0x00, 0x06, 0x7F, 0xED, 0x58, 0xC9},
       {0x40, 0x25, 0x00, 0x5B, 0x5B, 0xBF, 0x58, 0x82}, true},
    };
    for (const Case &k : cases) {
      ciat::HpWatch v;
      bool rep = replay(v, k.p, k.c, k.s);
      printf("%s watch: %s\n", rep == k.expected ? "OK  " : "FAIL", k.name); failures += rep != k.expected;
    }
    // Table of seen frames: menu values no longer fill it (a single key per p1/p2 pair)
    ciat::HpWatch v;
    std::string nw, single;
    int news = 0;
    uint8_t f[8] = {0x40, 0x00, 0x02, 0x06, 0x5B, 0x00, 0x58, 0};
    const uint8_t digits[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    for (uint8_t d : digits) { f[5] = d | 0x80; f[7] = frame_xor(f, 7); news += v.frame(f, nw, single) & 1; }
    uint8_t num[8] = {0x40, 0x00, 0x02, 0x00, 0x73, 0x66, 0x06, 0}; num[7] = frame_xor(num, 7);  // "P41"
    news += v.frame(num, nw, single) & 1;
    bool ok = news == 2 && v.n_seen == 2;
    printf("%s watch: 10 menu values = 1 frame seen, + 1 number (new = %d, seen = %d)\n",
           ok ? "OK  " : "FAIL", news, v.n_seen);
    failures += !ok; }

  printf("\n%d failure(s)\n", failures);
  return failures != 0;
}

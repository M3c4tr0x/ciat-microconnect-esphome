// PC test of the navigator (esphome/ciat_nav.h) against a simulator of the heat pump's menu.
// The simulator reproduces the behaviour logged on the bus (docs/protocol.md §7.3 and §7.4):
//   - + and OK held: user level at 13 frames (~2.95 s), installer level at 36 (~5.1 s later);
//   - the menu reopens on the 1st visible parameter after the last one viewed, at the entry level; the list wraps;
//   - visibility by level and by configuration (P07, P21, P61-P63 hidden here);
//   - one press = one step; key held: auto-repeat at 4, 3, 3, 2, 2 frames, then at every frame;
//   - value: saturates at its limits; OK goes back to the number and validates; OK held 13 frames: leaves the menu
//     (validates);
//   - reaction in the 12.2 of the same cycle;
//   - outside the menu: + and − change the setpoint by 0.5 °C if held for at least 3 frames, limits 15-30 °C;
//     1st press with the screen off: wake-up only (🟡 one case); OK outside the menu counted (must never happen).
// Build and run: g++ -std=c++17 -Wall -I esphome tools/test_ciat_nav.cpp -o /tmp/tn && /tmp/tn
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "ciat_nav.h"

using namespace ciat;
using namespace ciat::nav;
static int failures = 0;
#define CHECK(cond, ...)                           \
  do {                                             \
    bool _ok = (cond);                             \
    printf("%s ", _ok ? "OK  " : "FAIL");          \
    printf(__VA_ARGS__);                           \
    printf("\n");                                  \
    if (!_ok) failures++;                          \
  } while (0)

// ------------------------------------------------------------------------------------------------ simulator

struct Param {
  float v, min, max, step;
  int dec;          // 0: integer "   3"; 1: temperature "28.3c"; -1: "----"
  bool user;        // visible at user level
  bool read_only;   // read only (P40-P50, P80, P81)
  bool noisy = false;  // changes at every cycle (P50)
};

static uint8_t seg7(char c) {
  switch (c) {
    case '0': return 0x3F; case '1': return 0x06; case '2': return 0x5B; case '3': return 0x4F; case '4': return 0x66;
    case '5': return 0x6D; case '6': return 0x7D; case '7': return 0x07; case '8': return 0x7F; case '9': return 0x6F;
    case '-': return 0x40; case 'c': return 0x58; case 'P': return 0x73; default: return 0x00;
  }
}

struct HpSim {
  std::map<int, Param> par;
  bool menu = false, inst = false, value = false, deaf = false;
  int cur = 5, last = 4;        // last viewed: 4 after a power cut (the menu reopens on P05)
  float edit = 0;               // value being edited
  int held = 0, opened_at = -1; // frames the key has been held; frame at which the menu opened during this press
  uint8_t prev = 0;
  int idle = 99, n = 0;
  float setpoint = 22.0f, smin = 15.0f, smax = 30.0f;
  int setpoint_delay = 1;                    // cycles between the 3rd frame held and the setpoint change
  std::vector<std::pair<int, int>> pending;  // (remaining cycles, direction) of pending setpoint steps
  int single_at = -1;                        // cycle at which the display shows setpoint + 0.1 (single-cycle frame)
  int ok_out_of_menu = 0, pm_out_of_menu = 0;
  bool off_at_start = false;
  // checks
  int pm_on_value = 0, changes = 0;
  std::vector<std::string> change_log;

  HpSim() {
    // values of a real installation, P06 = 3; limits: CIAT sheet and bus readings
    auto R = [&](int p, float v, float mn, float mx, float st, int dec, bool us) { par[p] = Param{v, mn, mx, st, dec, us, false}; };
    auto L = [&](int p, float v, int dec, bool us) { par[p] = Param{v, v, v, 0, dec, us, true}; };
    R(0, 2, 1, 2, 1, 0, false); R(1, 1, 0, 1, 1, 0, false); R(2, 0, 0, 2, 1, 0, false); R(3, 0, 0, 1, 1, 0, false);
    R(4, 3.5f, 0, 5, 0.5f, 1, false); R(5, 0, 0, 3, 1, 0, true); R(6, 3, 0, 5, 1, 0, false);
    R(8, 1, 0, 1, 1, 0, false); R(9, -9, -15, 24, 1, 1, false);
    R(10, 12, -6, 28, 0.5f, 1, true); R(11, 12, -6, 28, 0.5f, 1, true); R(12, 25, 10, 40, 0.5f, 1, true);
    R(13, 35, 15, 45, 0.5f, 1, true); R(14, 15, -6, 28, 0.5f, 1, true); R(15, 27, 20, 70, 0.5f, 1, true);
    R(16, 20, 20, 70, 0.5f, 1, true); R(17, 20, -10, 30, 0.5f, 1, true); R(18, -8, -15, 25, 0.5f, 1, true);
    R(19, 44.5f, 20, 70, 0.5f, 1, true); R(20, 2, 0.5f, 5, 0.5f, 1, false);
    R(22, 1, 0, 3, 1, 0, true); R(23, 31.5f, 20, 32, 0.5f, 1, true);  // upper limit lowered to 32 for the test
    R(24, 0, 0, 1, 1, 0, false); R(25, -2, -5, 0, 0.5f, 1, false); R(26, 10, 5, 45, 0.5f, 1, false);
    R(27, 30, 10, 45, 0.5f, 1, false); R(28, 35, 10, 90, 1, 0, false); R(29, 1, 0, 2, 1, 0, false);
    R(30, 0, 0, 1, 1, 0, false); R(31, 10, 8, 15, 0.5f, 1, true); R(32, 1, 0, 5, 0.1f, 1, false);
    R(33, 0.3f, 0, 1, 0.1f, 1, false); R(34, 1, 0, 1, 1, 0, false); R(36, 0, 0, 1, 1, 0, false);
    L(40, 21.3f, 1, true); L(41, 25.9f, 1, true); L(42, 0, -1, true); L(43, 2.0f, 1, false); L(44, 8.9f, 1, true);
    L(45, 13.2f, 1, true); L(46, 0, 0, false); L(47, 23, 0, false); L(48, 9.2f, 1, true); L(49, 7, 0, false);
    L(50, 18.3f, 1, true); par[50].noisy = true;
    R(60, 0, 0, 3, 1, 0, true); L(80, 9, 0, true); L(81, 1, 0, true);
    R(93, 0, -10, 3, 0.5f, 1, false); R(94, -5, -5, -5, 0, 1, false); R(95, 40, 40, 40, 0, 1, false);
    R(96, 5, 5, 5, 0, 1, false); R(98, 1, 1, 1, 0, 0, false); R(99, 1, 1, 1, 0, 0, false);
  }

  bool visible(int p) const { auto it = par.find(p); return it != par.end() && (inst || it->second.user); }
  int neighbour(int p, int dir) const {
    for (int i = 0; i < 100; i++) {
      p = (p + dir + 100) % 100;
      if (visible(p)) return p;
    }
    return p;
  }
  void leave() {
    if (value) validate();
    menu = inst = value = false;
    last = cur;
  }
  void validate() {
    Param &q = par[cur];
    if (!q.read_only && q.v != edit) {
      changes++;
      char t[64];
      snprintf(t, sizeof t, "P%02d %.1f -> %.1f", cur, q.v, edit);
      change_log.push_back(t);
      q.v = edit;
    }
    value = false;
  }
  static bool repeats(int t) {  // frames held at which the key acts
    static const int r[] = {1, 5, 8, 11, 13, 15};
    for (int x : r)
      if (t == x) return true;
    return t > 15;
  }

  void key(uint8_t k) {
    if (k == 0 || deaf) { held = 0; prev = k; if (k == 0) idle++; return; }
    held = (k == prev) ? held + 1 : 1;
    if (k != prev) opened_at = -1;
    prev = k;
    bool screen_off = idle >= 18;
    idle = 0;
    if (!menu) {
      if (k == OK) ok_out_of_menu++;
      // Outside the menu the key is only taken on the 3rd frame held (3 frames → one step after ~470 ms;
      // 2 frames → no reaction). Screen off at the start of the press: wake-up only.
      if (k == PLUS || k == MINUS) {
        if (held == 1) { pm_out_of_menu++; off_at_start = screen_off; }
        if ((held == 3 || (held > 3 && repeats(held))) && !off_at_start)
          pending.push_back({setpoint_delay, k == PLUS ? 1 : -1});
      }
      if (k == (PLUS | OK) && held == 13) {
        menu = true;
        inst = false;
        cur = neighbour(last, +1);
        opened_at = held;
      }
      return;
    }
    if (k == (PLUS | OK)) {
      if (!inst && held == (opened_at > 0 ? 36 : 20)) inst = true;
      return;
    }
    if (k == OK) {
      if (held == 1) {
        if (value) validate();
        else { value = true; edit = par[cur].v; }
      }
      if (held == 13) leave();
      return;
    }
    if ((k == PLUS || k == MINUS) && repeats(held)) {
      int dir = k == PLUS ? 1 : -1;
      if (!value) { cur = neighbour(cur, dir); return; }
      pm_on_value++;
      Param &q = par[cur];
      if (q.read_only) return;
      float x = edit + dir * q.step;
      if (x > q.max + 1e-4f || x < q.min - 1e-4f) return;  // saturation
      edit = std::round(x * 10) / 10;
    }
  }

  std::string text() {
    char t[16];
    if (!menu) { snprintf(t, sizeof t, "%4.1fc", setpoint + (n == single_at ? 0.1f : 0.0f)); return t; }
    if (!value) { snprintf(t, sizeof t, " P%02d", cur); return t; }
    const Param &q = par[cur];
    float x = q.read_only ? q.v : edit;
    if (q.noisy) x += (n % 3 - 1) * 0.1f;
    if (q.dec < 0) return "----";
    if (q.dec == 0) { snprintf(t, sizeof t, "%4d", (int) lroundf(x)); return t; }
    snprintf(t, sizeof t, "%4.1fc", x);
    return t;
  }

  // One 12.1 frame carrying the keys k, then the heat pump's 12.2.
  void frame(uint8_t k, uint8_t f[8]) {
    n++;
    key(k);
    for (auto &a : pending)
      if (--a.first == 0) {
        float x = setpoint + a.second * 0.5f;
        if (x <= smax + 1e-4f && x >= smin - 1e-4f) setpoint = x;
      }
    pending.erase(std::remove_if(pending.begin(), pending.end(), [](const std::pair<int, int> &a) { return a.first <= 0; }),
                  pending.end());
    std::string s = text();
    uint8_t d[4] = {0, 0, 0, 0};
    int j = 0;
    bool point = false;
    for (char c : s) {
      if (c == '.') { point = true; continue; }
      if (j < 4) { d[j] = seg7(c) | (point ? 0x80 : 0); j++; }
      point = false;
    }
    bool backlight = idle < 18;
    f[0] = 0x40;
    f[1] = menu ? (backlight ? 0x08 : 0x00) : (0x25 | (backlight ? 0x08 : 0));
    f[2] = menu ? (inst ? 0x03 : 0x02) : 0x00;
    memcpy(f + 3, d, 4);
    f[7] = 0;
    for (int i = 0; i < 7; i++) f[7] ^= f[i];
  }
};

// ------------------------------------------------------------------------------------------------ closed loop

struct Bench {
  HpSim hp;
  Navigator nav;
  Survey sur;
  uint8_t k = 0;
  int left = 0;
  int key_frames = 0;
  std::vector<Event> events;

  void one_cycle() {
    uint8_t keys = 0;
    if (left > 0) { keys = k; left--; }
    if (keys) key_frames++;
    uint8_t f[8];
    hp.frame(keys, f);
    View v = view(f);
    sur.observe(v);
    nav.cycle(v);
    Keys a;
    if (nav.press_to_send(a)) { k = a.keys; left = a.frames; }
    Event e;
    while (nav.take_event(e)) events.push_back(e);
  }
  int run(int max = 4000) {
    int c = 0;
    one_cycle();  // a first 12.2, as on the bus
    while (nav.busy() && c < max) { one_cycle(); c++; }
    for (int i = 0; i < 5; i++) one_cycle();  // a few cycles after the end: no key must be sent
    return c;
  }
  int count(Event::Type t) const { int n = 0; for (auto &e : events) n += e.type == t; return n; }
  const Event *find(Event::Type t, int p) const {
    for (auto &e : events) if (e.type == t && e.p == p) return &e;
    return nullptr;
  }
  void errors() const {
    for (auto &e : events)
      if (e.type == Event::ERROR) printf("      error: %s\n", e.msg.c_str());
  }
};

static const std::vector<int> FAST_LIST = fast_list(false);

int main() {
  // 1. Fast list, menu reopened after P09 (on P10)
  {
    Bench b;
    b.hp.last = 9;
    std::string err;
    b.nav.scan(FAST_LIST, err);
    int c = b.run();
    b.errors();
    CHECK(b.count(Event::VALUE) == 9 && b.count(Event::ERROR) == 0 && b.count(Event::SCAN_END) == 1,
          "fast list: 9 values, no error (%d cycles = %.0f s)", c, c * 0.222);
    bool right = true;
    for (int p : FAST_LIST) {
      const Event *e = b.find(Event::VALUE, p);
      const Param &q = b.hp.par[p];
      if (!e) { right = false; continue; }
      if (q.dec < 0) right &= e->reading.txt == "----" && std::isnan(e->reading.val);
      else right &= equal(e->reading.val, q.v);
    }
    CHECK(right, "fast list: values equal to the heat pump's (P42 \"----\" non-numeric)");
    CHECK(!b.hp.menu && b.hp.changes == 0 && b.hp.pm_on_value == 0,
          "fast list: menu closed, no change, no + or − on a value");
    CHECK(b.nav.state() == Navigator::IDLE && b.left == 0, "fast list: navigator idle, no key pending");
    CHECK(c * 0.222 < 85, "fast list, worst case (menu reopened on P10, 26 steps to P40): duration < 85 s");
  }

  // 1 bis. Steady state: the previous scan ended on P48, the menu reopens after it (P50 at user level)
  {
    Bench b;
    b.hp.last = 48;
    std::string err;
    b.nav.scan(FAST_LIST, err);
    int c = b.run();
    b.errors();
    CHECK(b.count(Event::VALUE) == 9 && b.count(Event::ERROR) == 0 && c * 0.222 < 55,
          "fast list in steady state: 9 values in %d cycles = %.0f s (< 55 s)", c, c * 0.222);
    // The next scan starts from the right side (descending order, from the last one viewed)
    b.events.clear();
    b.nav.scan(FAST_LIST, err);
    int c2 = b.run();
    CHECK(b.count(Event::VALUE) == 9 && b.count(Event::ERROR) == 0, "next scan: 9 values in %.0f s", c2 * 0.222);
  }

  // 1 ter. Scan at user level: entry ~3 s, 6 measurements, never the installer level.
  for (int last : {48, 45, 9}) {
    Bench b;
    b.hp.last = last;
    std::string err;
    bool r = b.nav.scan(user_list(false), err, false);
    bool never_inst = true;
    int c = 0;
    b.one_cycle();
    while (b.nav.busy() && c < 4000) { b.one_cycle(); c++; never_inst &= !b.hp.inst; }
    for (int i = 0; i < 5; i++) b.one_cycle();
    b.errors();
    bool right = true;
    for (int p : user_list(false)) {
      const Event *e = b.find(Event::VALUE, p);
      const Param &q = b.hp.par[p];
      right &= e && (q.dec < 0 ? std::isnan(e->reading.val) : equal(e->reading.val, q.v));
    }
    CHECK(r && right && never_inst && b.count(Event::VALUE) == 6 && b.count(Event::ERROR) == 0 &&
              b.count(Event::ABSENT) == 0 && !b.hp.menu && b.hp.changes == 0,
          "user-level scan (menu reopened after P%02d): 6 right values, never the installer level, %d cycles = %.0f s",
          last, c, c * 0.222);
  }
  {
    Bench b;  // pool: P50 visible at user level, average over 9 cycles
    b.hp.last = 48;
    std::string err;
    b.nav.scan(user_list(true), err, false);
    int c = b.run();
    const Event *e = b.find(Event::VALUE, 50);
    CHECK(e && std::fabs(e->reading.val - 18.3f) < 0.11f && b.count(Event::VALUE) == 7 && b.count(Event::ERROR) == 0,
          "user-level scan, pool: 7 values, P50 = %s (%d cycles = %.0f s)", e ? e->reading.txt.c_str() : "?", c, c * 0.222);
  }
  {
    Bench b;  // P43 asked at user level: absent, no error
    b.hp.last = 41;
    std::string err;
    b.nav.scan({42, 43, 44}, err, false);
    b.run();
    CHECK(b.find(Event::ABSENT, 43) && b.count(Event::VALUE) == 2 && b.count(Event::ERROR) == 0,
          "user-level scan: P43 (installer level only) reported absent, without error");
  }

  // 2. Slow list: absent parameters (P07, P21 with P06 = 3; P35 does not exist; P61-P63 with P60 = 0)
  {
    Bench b;
    b.hp.last = 50;
    std::string err;
    b.nav.scan(slow_list(), err);
    int c = b.run();
    b.errors();
    int absent = b.count(Event::ABSENT);
    bool right_absent = b.find(Event::ABSENT, 7) && b.find(Event::ABSENT, 21) && b.find(Event::ABSENT, 35) &&
                        b.find(Event::ABSENT, 61) && b.find(Event::ABSENT, 63);
    CHECK(absent == 6 && right_absent && b.count(Event::ERROR) == 0,
          "slow list: 6 absent (P07, P21, P35, P61-P63), no error");
    int expected = (int) slow_list().size() - 6;
    CHECK(b.count(Event::VALUE) == expected, "slow list: %d values read out of %d expected (%d cycles = %.0f s)",
          b.count(Event::VALUE), expected, c, c * 0.222);
    CHECK(!b.hp.menu && b.hp.changes == 0 && b.hp.pm_on_value == 0, "slow list: no change, menu closed");
    const Event *e = b.find(Event::VALUE, 9);
    CHECK(e && equal(e->reading.val, -9), "slow list: P09 = %s", e ? e->reading.txt.c_str() : "?");
  }

  // 3. Write: P04 3.5 -> 3.0 (step 0.5)
  {
    Bench b;
    b.hp.last = 33;
    std::string err;
    bool r = b.nav.write(4, 3.0f, err);
    int c = b.run();
    b.errors();
    const Event *e = b.find(Event::WRITTEN, 4);
    CHECK(r && e && equal(e->reading.val, 3.0f) && equal(e->old, 3.5f) && equal(b.hp.par[4].v, 3.0f),
          "write P04 3.5 -> 3.0: read back %s (%d cycles = %.0f s)", e ? e->reading.txt.c_str() : "?", c, c * 0.222);
    CHECK(b.hp.changes == 1 && !b.hp.menu && b.count(Event::ERROR) == 0, "write P04: a single change, menu closed");
  }

  // 4. Value beyond the heat pump's limit: the heat pump saturates at 32, 34 requested -> error, original value back
  {
    Bench b;
    b.hp.last = 9;
    std::string err;
    b.nav.write(23, 34.0f, err);
    b.run();
    b.errors();
    CHECK(b.count(Event::ERROR) >= 1 && !b.find(Event::WRITTEN, 23) && equal(b.hp.par[23].v, 31.5f) && !b.hp.menu,
          "limit: P23 stays at %.1f, error published, menu closed", b.hp.par[23].v);
    CHECK(b.nav.state() == Navigator::IDLE, "limit: navigator idle (not blocked)");
  }

  // 5. Value impossible with the step: P20 2.0 -> 2.2 (step 0.5)
  {
    Bench b;
    std::string err;
    b.nav.write(20, 2.2f, err);
    b.run();
    b.errors();
    CHECK(b.count(Event::ERROR) >= 1 && equal(b.hp.par[20].v, 2.0f) && !b.hp.menu,
          "step: P20 stays at %.1f, error published, menu closed", b.hp.par[20].v);
  }

  // 6. Whitelist
  {
    Navigator n;
    std::string e1, e2, e3, e4;
    bool r1 = n.write(30, 1, e1), r2 = n.write(33, 0.5f, e2), r3 = n.write(40, 20, e3), r4 = n.write(6, 7, e4);
    CHECK(!r1 && !r2 && !r3 && !r4, "whitelist: P30, P33, P40 and P06 = 7 refused");
    printf("      %s | %s | %s | %s\n", e1.c_str(), e2.c_str(), e3.c_str(), e4.c_str());
  }

  // 7. Parameter hidden by the configuration: P07 (P06 = 3)
  {
    Bench b;
    b.hp.last = 5;
    std::string err;
    b.nav.write(7, 5, err);
    b.run();
    b.errors();
    CHECK(b.find(Event::ABSENT, 7) && b.count(Event::ERROR) >= 1 && b.hp.changes == 0 && !b.hp.menu,
          "P07 hidden: absent, error, no change, menu closed");
  }

  // 8. The heat pump does not receive our presses (transmission without effect): entry fails, nothing else
  {
    Bench b;
    b.hp.deaf = true;
    std::string err;
    b.nav.scan(FAST_LIST, err);
    int c = b.run();
    b.errors();
    CHECK(b.count(Event::ERROR) == 1 && b.nav.state() == Navigator::IDLE && b.left == 0,
          "deaf heat pump: one error, back to idle after %d cycles", c);
  }

  // 9. Menu already open at the start (ESP rebooted in the middle of the menu): closed, then read normally
  {
    Bench b;
    b.hp.menu = true;
    b.hp.cur = 12;
    std::string err;
    b.nav.scan({40, 41}, err);
    b.run();
    b.errors();
    CHECK(b.count(Event::VALUE) == 2 && b.count(Event::ERROR) == 0 && !b.hp.menu,
          "menu already open: closed, then 2 values read");
  }

  // 10. Busy: a 2nd command during a scan is refused
  {
    Bench b;
    std::string err;
    b.nav.scan(FAST_LIST, err);
    b.one_cycle();
    bool r = b.nav.write(4, 3, err);
    CHECK(!r, "busy: write refused during a scan (%s)", err.c_str());
    b.run();
  }

  // 11. Interruption (silent bus or transmission off) in the middle of navigation: no more keys
  {
    Bench b;
    std::string err;
    b.nav.scan(FAST_LIST, err);
    for (int i = 0; i < 60; i++) b.one_cycle();
    b.nav.interrupt("transmission off");
    for (int i = 0; i < 40; i++) b.one_cycle();
    CHECK(b.nav.state() == Navigator::IDLE && b.left == 0 && b.count(Event::ERROR) == 1,
          "interruption: idle, no key pending, one error");
  }

  // 12. Passive survey: a user opens the menu and views P31 (no navigator)
  {
    Bench b;
    b.hp.last = 23;  // reopens on P31 at user level
    auto hold = [&](uint8_t k, int n) { b.k = k; b.left = n; for (int i = 0; i < n + 2; i++) b.one_cycle(); };
    hold(PLUS | OK, 16);
    hold(OK, 1);
    hold(OK, 1);
    hold(OK, 16);
    CHECK(b.sur.tab[31].ok && equal(b.sur.tab[31].val, 10.0f) && !b.hp.menu, "passive survey: P31 = %s", b.sur.tab[31].txt.c_str());
  }

  auto view_txt = [](const char *txt, bool menu) {
    View v;
    v.menu = menu;
    v.txt = txt;
    float x;
    if (lcd_number(v.txt, x)) v.val = x;
    v.screen = !menu ? Screen::OUT_OF_MENU : (param_number(v.txt) >= 0 ? Screen::NUMBER : Screen::VALUE);
    v.num = param_number(v.txt);
    return v;
  };

  // 13. Passive survey of a measurement that jumps at every cycle: real P50 values (54 s on screen, 18.0 to 18.8).
  //     The average is published, not the instantaneous value.
  {
    static const float raw[] = {18.5, 18.0, 18.6, 18.4, 18.5, 18.8, 18.4, 18.5, 18.7, 18.2, 18.6, 18.4, 18.3, 18.7,
                                18.3, 18.5, 18.8, 18.2, 18.5, 18.4, 18.2, 18.5, 18.1, 18.5, 18.6, 18.3, 18.6, 18.3,
                                18.6, 18.7, 18.2, 18.7, 18.4, 18.7, 18.2, 18.5, 18.3, 18.6, 18.2, 18.4, 18.5, 18.3};
    Survey r;
    std::vector<std::string> pubs, displays;
    auto obs = [&](const View &v) {
      int q = r.observe(v);
      if (q >= 0) pubs.push_back(r.tab[q].txt);
      std::string a = r.filter_active() ? r.shown() : v.txt;
      if (displays.empty() || displays.back() != a) displays.push_back(a);
    };
    obs(view_txt(" P50", true));
    int n = 0;
    for (int k = 0; k < 6; k++)  // 6 × 42 = 252 cycles ≈ 56 s
      for (float x : raw) {
        char t[8];
        snprintf(t, sizeof t, "%.1fc", x);
        obs(view_txt(t, true));
        n++;
      }
    obs(view_txt(" P50", true));
    obs(view_txt("26.0c", false));
    std::string list;
    for (auto &x : pubs) list += x + " ";
    CHECK(pubs.size() >= 1 && pubs.size() <= 3 && r.tab[50].ok && std::fabs(r.tab[50].val - 18.45f) < 0.11f,
          "passive survey P50: %zu publication(s) (%s) for %d cycles, value %.1f (true average 18.45)", pubs.size(),
          list.c_str(), n, r.tab[50].val);
    CHECK(displays.size() <= 6, "filtered display during P50: %zu changes instead of ~%d", displays.size(), n);
    CHECK(!r.changed, "a measurement does not trigger a flash backup");
  }

  // 14. Setting viewed then changed by hand: published on opening and on closing, backup due
  {
    Survey r;
    int pub = 0;
    for (const char *s : {" P49", "   7", " P49", " P06", "   3", "   3", "   4", " P06"}) pub += r.observe(view_txt(s, true)) >= 0;
    CHECK(pub == 3 && r.tab[49].txt == "   7" && r.tab[6].txt == "   4" && r.changed,
          "passive survey of settings: 3 publications (P49, P06 opened, P06 changed), P06 = %s", r.tab[6].txt.c_str());
  }

  // 15. Flash backup: round trip, measurements excluded, other version refused
  {
    Survey r;
    Reading l;
    l.ok = true;
    l.txt = " 3.5"; l.val = 3.5f; r.set(4, l);
    l.txt = "10.0c"; l.val = 10.0f; r.set(31, l);
    l.txt = "45.2c"; l.val = 45.2f; r.set(40, l);  // measurement
    Backup s;
    to_backup(r, s);
    Survey r2;
    int n = from_backup(r2, s);
    std::string t1 = settings_text(r2, false), t2 = settings_text(r2, true);
    CHECK(n == 2 && !r2.tab[40].ok && t1 == "P04=3.5 " && t2 == "P31=10.0 " && !r2.changed,
          "backup: %d settings restored, \"%s\" \"%s\", P40 not restored", n, t1.c_str(), t2.c_str());
    s.version = 99;
    Survey r3;
    CHECK(from_backup(r3, s) == -1 && !r3.tab[4].ok, "backup of another version ignored");
    Survey r4;
    r4.set(4, r2.tab[4]);
    CHECK(r4.changed, "new setting: backup due");
  }

  // 16-22. Setpoint outside the menu
  auto adjust = [](Bench &b, float target, std::string &err) {
    for (int i = 0; i < 3; i++) b.one_cycle();
    bool r = b.nav.setpoint(target, err);
    int c = r ? b.run(600) : 0;
    b.errors();
    return c;
  };
  {
    Bench b;
    b.hp.idle = 0;  // screen on
    std::string err;
    int c = adjust(b, 23.5f, err);
    const Event *e = b.find(Event::SETPOINT, -1);
    CHECK(e && equal(e->reading.val, 23.5f) && equal(e->old, 22.0f) && equal(b.hp.setpoint, 23.5f) &&
              b.hp.pm_out_of_menu == 3 && b.hp.ok_out_of_menu == 0 && !b.hp.menu && b.count(Event::ERROR) == 0,
          "setpoint 22.0 → 23.5 screen on: %d presses, no OK, %d cycles (%.1f s)", b.hp.pm_out_of_menu, c, c * 0.222);
  }
  {
    Bench b;  // screen off (idle = 99): the 1st − only wakes it up
    std::string err;
    adjust(b, 21.0f, err);
    CHECK(b.find(Event::SETPOINT, -1) && equal(b.hp.setpoint, 21.0f) && b.hp.pm_out_of_menu == 3 &&
              b.hp.ok_out_of_menu == 0 && b.count(Event::ERROR) == 0,
          "setpoint 22.0 → 21.0 screen off: wake-up + 2 steps (%d presses), no OK", b.hp.pm_out_of_menu);
  }
  {
    Bench b;
    b.hp.smax = 25.0f;  // heat-pump limit lower than the target
    std::string err;
    adjust(b, 26.0f, err);
    bool limit = false;
    for (auto &e : b.events) limit |= e.type == Event::ERROR && e.msg.find("stayed at 25.0") != std::string::npos;
    CHECK(limit && equal(b.hp.setpoint, 25.0f) && b.nav.state() == Navigator::IDLE && b.hp.ok_out_of_menu == 0 && b.left == 0,
          "setpoint beyond the heat pump's limit: abort \"stayed at 25.0\", setpoint left at %.1f, no OK", b.hp.setpoint);
  }
  {
    Bench b;
    b.hp.deaf = true;  // the heat pump does not receive the presses
    std::string err;
    adjust(b, 23.0f, err);
    bool none = false;
    for (auto &e : b.events) none |= e.type == Event::ERROR && e.msg.find("had no effect") != std::string::npos;
    CHECK(none && equal(b.hp.setpoint, 22.0f) && b.nav.state() == Navigator::IDLE && b.left == 0,
          "presses not received: abort \"had no effect\", idle, no key pending");
  }
  {
    Bench b;
    b.hp.idle = 0;
    b.hp.single_at = 7;  // single-cycle frame at 22.1c during the adjustment
    std::string err;
    adjust(b, 24.0f, err);
    CHECK(b.find(Event::SETPOINT, -1) && equal(b.hp.setpoint, 24.0f) && b.hp.pm_out_of_menu == 4,
          "single-cycle frame during the adjustment: ignored, 4 presses, setpoint %.1f", b.hp.setpoint);
  }
  {
    Bench b;
    b.hp.idle = 0;
    b.hp.setpoint_delay = 7;  // late reaction (1.5 s): the loop may overshoot, then must correct
    std::string err;
    adjust(b, 23.0f, err);
    CHECK(b.find(Event::SETPOINT, -1) && equal(b.hp.setpoint, 23.0f) && b.hp.ok_out_of_menu == 0,
          "late reaction of the heat pump: final setpoint %.1f (%d presses)", b.hp.setpoint, b.hp.pm_out_of_menu);
  }
  {
    Bench b;
    std::string e1, e2, e3, e4;
    for (int i = 0; i < 3; i++) b.one_cycle();
    bool r1 = b.nav.setpoint(22.3f, e1), r2 = b.nav.setpoint(31.0f, e2), r3 = b.nav.setpoint(14.5f, e3);
    std::string es;
    b.nav.scan(FAST_LIST, es);
    bool r4 = b.nav.setpoint(23.0f, e4);
    CHECK(!r1 && !r2 && !r3 && !r4 && b.hp.pm_out_of_menu == 0, "refused: 22.3 (step), 31 and 14.5 (limits), navigator busy (%s)",
          e4.c_str());
    b.run();
    std::string e5;
    b.k = PLUS | OK; b.left = 20;  // menu opened by hand
    for (int i = 0; i < 16; i++) b.one_cycle();
    bool r5 = b.nav.setpoint(23.0f, e5);
    CHECK(!r5 && b.hp.menu, "refused with the menu open: %s", e5.c_str());
  }

  // 22 bis. Pool setpoint (limits 20-35): 31.5 → 33.0; 36 refused; with the heating limits, 33 would be refused.
  {
    Bench b;
    b.hp.idle = 0;
    b.hp.setpoint = 31.5f;
    b.hp.smin = 20.0f;
    b.hp.smax = 35.0f;
    std::string e1, e2, e3;
    for (int i = 0; i < 3; i++) b.one_cycle();
    bool r36 = b.nav.setpoint(36.0f, Navigator::POOL_SETPOINT_MIN, Navigator::POOL_SETPOINT_MAX, e1);
    bool rheat = b.nav.setpoint(33.0f, e2);
    bool r = b.nav.setpoint(33.0f, Navigator::POOL_SETPOINT_MIN, Navigator::POOL_SETPOINT_MAX, e3);
    b.run(600);
    CHECK(!r36 && !rheat && r && equal(b.hp.setpoint, 33.0f) && b.find(Event::SETPOINT, -1) && b.hp.ok_out_of_menu == 0,
          "pool setpoint 31.5 → 33.0 (limits 20-35), 36 refused, heating limits refuse 33");
  }

  // 22 ter. Menu left open (ESP32 rebooted in the middle of a scan, on P18): long OK exit, nothing changed.
  for (bool on_value : {false, true}) {
    Bench b;
    b.hp.menu = true;
    b.hp.inst = true;
    b.hp.cur = 18;
    b.hp.value = on_value;
    if (on_value) b.hp.edit = b.hp.par[18].v;
    for (int i = 0; i < 3; i++) b.one_cycle();
    std::string e;
    bool r = b.nav.exit_menu(e);
    int c = 0;
    while (b.nav.busy() && c < 100) { b.one_cycle(); c++; }
    for (int i = 0; i < 3; i++) b.one_cycle();
    std::string e2;
    bool r2 = b.nav.exit_menu(e2);  // outside the menu: nothing to do
    CHECK(r && !r2 && !b.hp.menu && b.hp.changes == 0 && b.count(Event::ERROR) == 0 &&
              b.count(Event::SCAN_END) == 0 && b.nav.state() == Navigator::IDLE,
          "menu left open on P18%s: closed in %d cycles (%.1f s), nothing changed, no error or scan end",
          on_value ? " (value shown)" : "", c, c * 0.222);
  }

  // 23-26. Scan stopped by the thermostat at various moments: menu closed, nothing changed, no error,
  //        no + or − on a value, then the setpoint can be adjusted.
  for (int a : {5, 45, 70, 120}) {
    Bench b;
    b.hp.last = 39;
    b.hp.idle = 0;
    std::string err;
    b.one_cycle();
    b.nav.scan(slow_list(), err);
    for (int i = 0; i < a; i++) b.one_cycle();
    std::string st = b.nav.state_text();
    bool r = b.nav.stop_scan();
    int c = 0;
    while (b.nav.busy() && c < 200) { b.one_cycle(); c++; }
    for (int i = 0; i < 5; i++) b.one_cycle();
    bool interr = b.nav.take_interruption();
    std::string e2;
    bool rs = b.nav.setpoint(23.0f, e2);
    b.run(600);
    CHECK(r && interr && !b.hp.menu && b.hp.changes == 0 && b.hp.pm_on_value == 0 && b.count(Event::ERROR) == 0 &&
              b.count(Event::SCAN_END) == 0 && rs && equal(b.hp.setpoint, 23.0f) && b.hp.ok_out_of_menu == 0,
          "scan stopped after %d cycles (%s): menu closed in %d cycles (%.1f s), nothing changed, setpoint adjusted afterwards",
          a, st.c_str(), c, c * 0.222);
  }
  {
    Bench b;
    std::string err;
    b.one_cycle();
    b.nav.write(4, 3.0f, err);
    for (int i = 0; i < 45; i++) b.one_cycle();
    bool refused = !b.nav.stop_scan() && b.nav.busy();
    b.run();
    CHECK(refused && equal(b.hp.par[4].v, 3.0f), "no stop during a write (refused, write carried through)");
  }

  printf("\n%d failure(s)\n", failures);
  return failures != 0;
}

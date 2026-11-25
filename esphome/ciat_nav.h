// Parameter-menu navigator: reads and writes heat-pump parameters in place of a user.
// Pure logic, no ESP dependency: tested on a PC against a menu simulator (tools/test_ciat_nav.cpp).
// Menu behaviour: docs/protocol.md §7.3 and §7.4; design: docs/emulator.md.
//
// Principle: the navigator only does two things.
//   - It observes every 12.2 from the heat pump (display, menu / installer flags). One 12.2 = one cycle (~222 ms).
//   - It requests key presses (keys + number of 12.1 frames), which the ESPHome loop passes on to Link::press().
// It never assumes the result of a press: it reads it back in the following 12.2 frames, and aborts if it does not
// see what it expects within the allotted time.
//
// Safety rules:
//   - never + or − while a value is displayed, except during a requested write;
//   - short presses: a key held for 4 frames or more auto-repeats;
//   - writes limited to a whitelist, value read back after validation;
//   - a long OK cancels nothing (it validates the displayed value): to abort, the original value is put back;
//   - every wait is bounded; on failure: back to the number, exit from the menu, error published; a write is never
//     retried automatically.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "ciat_bus.h"

namespace ciat {
namespace nav {

static const uint8_t PLUS = 0x01, MINUS = 0x02, OK = 0x04;

// ---------------------------------------------------------------------------------------------------------------
// Reading the display

enum class Screen : uint8_t { OUT_OF_MENU, NUMBER, VALUE };

struct View {
  Screen screen = Screen::OUT_OF_MENU;
  int num = -1;            // number shown (NUMBER)
  std::string txt;         // display text, as decoded ("28.3c", "   3", "----")
  float val = NAN;         // numeric value, if the display shows one
  bool menu = false, installer = false, backlight = false;

  bool same_display(const View &o) const {
    return screen == o.screen && num == o.num && txt == o.txt && menu == o.menu && installer == o.installer;
  }
};

// "P40" on 4 positions (space, P, tens, units) -> 40; -1 otherwise.
inline int param_number(const std::string &t) {
  size_t i = 0;
  while (i < t.size() && t[i] == ' ') i++;
  if (i + 3 != t.size() || t[i] != 'P' || t[i + 1] < '0' || t[i + 1] > '9' || t[i + 2] < '0' || t[i + 2] > '9')
    return -1;
  return (t[i + 1] - '0') * 10 + (t[i + 2] - '0');
}

inline View view(const uint8_t *f) {  // f = complete 12.2 (40 p1 p2 d1..d4 xor)
  View v;
  v.txt = lcd(f + 3);
  v.menu = f[2] & 0x02;
  v.installer = (f[2] & 0x03) == 0x03;
  v.backlight = f[1] & 0x08;
  float x;
  if (lcd_number(v.txt, x)) v.val = x;
  if (!v.menu) v.screen = Screen::OUT_OF_MENU;
  else if ((v.num = param_number(v.txt)) >= 0) v.screen = Screen::NUMBER;
  else v.screen = Screen::VALUE;
  return v;
}

// ---------------------------------------------------------------------------------------------------------------
// Parameters

struct Reading {
  bool ok = false;
  std::string txt;
  float val = NAN;  // NAN if the value is not numeric ("----")
};

// Allowed writes: ranges from the CIAT sheet "Liste des paramètres Aqualis 2".
// Excluded: installation settings (P00-P03, P22, P93-P99), compressor enable (P08), test mode (P30),
// anti-freeze (P32, P33: "must never be changed"), DHW hours (P61-P63: display format unknown),
// read-only values (P40-P59, P80, P81).
struct Range { int p; float min, max; };
static const Range WHITELIST[] = {
    {4, 0, 5},     {5, 0, 3},     {6, 0, 5},     {7, -15, 24},  {9, -15, 24},  {10, -6, 28}, {11, -6, 28},
    {12, 10, 40},  {13, 15, 45},  {14, -6, 28},  {15, 20, 70},  {16, 20, 70},  {17, -10, 30}, {18, -15, 25},
    {19, 20, 70},  {20, 0.5f, 5}, {21, 0.5f, 10}, {23, 20, 35}, {24, 0, 1},    {25, -5, 0},  {26, 5, 45},
    {27, 10, 45},  {28, 10, 90},  {29, 0, 2},    {31, 8, 15},   {34, 0, 1},    {36, 0, 1},   {60, 0, 3},
};

inline bool write_allowed(int p, float v, std::string &err) {
  for (const Range &r : WHITELIST)
    if (r.p == p) {
      if (std::isnan(v) || v < r.min - 1e-3f || v > r.max + 1e-3f) {
        char t[64];
        snprintf(t, sizeof t, "P%02d: %.1f outside the range %.1f to %.1f", p, v, r.min, r.max);
        err = t;
        return false;
      }
      return true;
    }
  char t[48];
  snprintf(t, sizeof t, "P%02d is not in the whitelist", p);
  err = t;
  return false;
}

inline bool equal(float a, float b) { return std::fabs(a - b) < 0.05f; }

// Dial positions that allow the menu (manual): cooling, off, frost protection, heating, pool.
inline bool dial_allows_menu(uint8_t m) { return m == 3 || m == 4 || m == 5 || m == 6 || m == 8; }

// Scan lists. Fast: the measurements; P50 only with the dial on pool.
// Slow: every setting, plus P49 (fault resets) and the versions; absent ones (P07, P21, P35, P61-P63 depending on
// the configuration) are reported without an error.
inline std::vector<int> fast_list(bool pool) {
  std::vector<int> l = {40, 41, 42, 43, 44, 45, 46, 47, 48};
  if (pool) l.push_back(50);
  return l;
}
// Frequent scan at user level (entry ~3 s instead of ~8 s): the measurements visible at that level (P40-P42, P44,
// P45, P48, P50 ✅). P43, P46 and P47 are only shown at installer level: they stay in fast_list(), read at installer
// level less often.
inline std::vector<int> user_list(bool pool) {
  std::vector<int> l = {40, 41, 42, 44, 45, 48};
  if (pool) l.push_back(50);
  return l;
}
inline std::vector<int> slow_list() {
  std::vector<int> l;
  for (int p = 0; p <= 36; p++) l.push_back(p);
  for (int p : {49, 60, 61, 62, 63, 80, 81, 93, 94, 95, 96, 98, 99}) l.push_back(p);
  return l;
}

// ---------------------------------------------------------------------------------------------------------------
// Measurements: the heat pump shows the sensor's raw value, which jumps from one cycle to the next (P50: 18.0 to
// 18.8 around 18.4, no trend, over 200 cycles). An average is published, not an instantaneous value.

// P46 (anti-short-cycle) is not a measurement: a countdown shown as minutes.seconds ("2.52" = 2 min 52 s, checked
// against the compressor stops). No average, no rounding to the tenth: median, like a setting.
inline bool is_measurement(int p) { return (p >= 40 && p <= 45) || p == 47 || p == 48 || p == 50; }  // P49: counter
inline bool has_sensor(int p) { return p >= 40 && p <= 50; }  // published as HA sensors, not in the "Settings" texts

// Average rounded to the tenth, like the display.
inline float mean_tenth(double sum, int n) { return std::round(sum / n * 10.0) / 10.0f; }

// Display text for a filtered value, in the format of the original text (width, final "c").
inline std::string text_like(const std::string &model, float v) {
  bool c = !model.empty() && model.back() == 'c';
  char t[16];
  snprintf(t, sizeof t, "%.1f%s", v, c ? "c" : "");
  std::string r = t;
  while (r.size() < model.size()) r.insert(r.begin(), ' ');
  return r;
}

// ---------------------------------------------------------------------------------------------------------------
// Passive survey: values seen on the display when someone browses the menu (the real terminal in shadow mode, or
// the navigator). A number then a value = the value of that parameter.
//   - Setting: published when the value opens, then when it closes if it changed in between.
//   - Numeric measurement (P40-P50): running average by blocks. First publication after FIRST_BLOCK cycles (~2 s),
//     then one per BLOCK cycles (~10 s) if the average changed, and on closing the average of the current block if
//     it is long enough. Meanwhile, shown() gives the filtered text for the published display.

struct Survey {
  static const int FIRST_BLOCK = 9;  // ~2 s
  static const int BLOCK = 45;       // ~10 s

  Reading tab[100];
  int last = -1;          // last number shown
  bool open = false;      // a value is shown
  std::string published;  // text published for the open value
  bool changed = false;   // a setting changed since the last flash backup

  // Records a reading (survey or navigator); marks the backup as due if a setting changed.
  void set(int p, const Reading &l) {
    if (p < 0 || p > 99) return;
    Reading &t = tab[p];
    if (!has_sensor(p) && (!t.ok || t.txt != l.txt)) changed = true;
    t = l;
  }

  // A numeric measurement is open: the published display must show shown() rather than the raw text.
  bool filter_active() const { return open && filter_; }
  const std::string &shown() const { return shown_; }

  // Returns the number of the parameter to publish (tab[p]), -1 otherwise.
  int observe(const View &v) {
    if (v.screen == Screen::VALUE) {
      if (last < 0 || last > 99) return -1;
      if (!open) {
        open = true;
        filter_ = is_measurement(last) && !std::isnan(v.val);
        sum_ = 0; n_ = 0; first_ = true;
        shown_ = v.txt;  // until the first average
        model_ = v.txt;
        if (!filter_) { set(last, read_(v)); published = v.txt; return last; }
      }
      if (filter_) {
        if (std::isnan(v.val)) {  // the measurement becomes non-numeric ("----"): published as is
          filter_ = false;
          set(last, read_(v));
          published = v.txt;
          return last;
        }
        sum_ += v.val; n_++;
        if (n_ >= (first_ ? FIRST_BLOCK : BLOCK)) return end_block_(false);
        return -1;
      }
      set(last, read_(v));
      return -1;
    }
    int r = -1;
    if (open && last >= 0) {
      if (filter_) r = end_block_(true);
      else if (tab[last].txt != published) r = last;  // closing: value changed
    }
    open = false;
    filter_ = false;
    last = v.screen == Screen::NUMBER ? v.num : -1;
    return r;
  }

 private:
  bool filter_ = false, first_ = true;
  double sum_ = 0;
  int n_ = 0;
  std::string shown_, model_;

  static Reading read_(const View &v) {
    Reading l;
    l.ok = true;
    l.txt = v.txt;
    l.val = v.val;
    return l;
  }
  // End of an averaging block: publishes if the average changed. On closing, a block that is too short only counts
  // if nothing was published yet for this viewing.
  int end_block_(bool closing) {
    int p = last;
    if (n_ == 0 || (closing && n_ < FIRST_BLOCK && !first_)) return -1;
    float m = mean_tenth(sum_, n_);
    sum_ = 0; n_ = 0;
    bool first = first_;
    first_ = false;
    shown_ = text_like(model_, m);
    Reading &t = tab[p];
    if (!first && t.ok && equal(t.val, m)) return -1;
    Reading l;
    l.ok = true;
    l.val = m;
    l.txt = shown_;
    set(p, l);
    published = l.txt;
    return p;
  }
};

// "P04=3.5 P05=0 …" texts of the settings (excluding measurements P40-P50), in two halves: an HA state is limited
// to 255 characters. upper = false: P00 to P29; true: P30 to P99.
inline std::string settings_text(const Survey &r, bool upper) {
  std::string a;
  for (int q = upper ? 30 : 0; q < (upper ? 100 : 30); q++) {
    if (has_sensor(q)) continue;
    const Reading &x = r.tab[q];
    if (!x.ok) continue;
    std::string v = x.txt;
    size_t d = v.find_first_not_of(' ');
    v = d == std::string::npos ? std::string() : v.substr(d);
    if (!v.empty() && v.back() == 'c') v.pop_back();
    while (!v.empty() && v.back() == ' ') v.pop_back();  // "3.5 " (P04: no unit)
    char t[24];
    snprintf(t, sizeof t, "P%02d=%s ", q, v.c_str());
    a += t;
  }
  return a;
}

// Flash backup of the settings (flat structure, copied as is by ESPHome preferences).
// Measurements and P49 (HA sensors, P40-P50) are not saved: restored at boot they would be stale.
struct Backup {
  static const uint16_t VERSION = 1;
  uint16_t version = 0;
  uint8_t ok[100] = {0};
  char txt[100][8] = {{0}};
  float val[100] = {0};
};

inline void to_backup(const Survey &r, Backup &s) {
  s = Backup();
  s.version = Backup::VERSION;
  for (int p = 0; p < 100; p++) {
    const Reading &l = r.tab[p];
    if (has_sensor(p) || !l.ok) continue;
    s.ok[p] = 1;
    snprintf(s.txt[p], sizeof s.txt[p], "%s", l.txt.c_str());
    s.val[p] = l.val;
  }
}

// Restores the saved settings into an empty survey. Returns the number of settings restored, -1 if the backup is
// of another version.
inline int from_backup(Survey &r, const Backup &s) {
  if (s.version != Backup::VERSION) return -1;
  int n = 0;
  for (int p = 0; p < 100; p++) {
    if (has_sensor(p) || !s.ok[p] || r.tab[p].ok) continue;
    char t[9];
    memcpy(t, s.txt[p], 8);
    t[8] = 0;
    r.tab[p].ok = true;
    r.tab[p].txt = t;
    r.tab[p].val = s.val[p];
    n++;
  }
  return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Navigator

struct Event {
  // SETPOINT: room setpoint adjusted outside the menu; reading = value read back, old = starting value.
  enum Type : uint8_t { VALUE, ABSENT, SCAN_END, WRITTEN, ERROR, SETPOINT } type;
  explicit Event(Type t = ERROR) : type(t) {}
  int p = -1;
  Reading reading;   // VALUE; WRITTEN: value read back
  float old = NAN;   // WRITTEN
  std::string msg;
};

struct Keys {
  uint8_t keys = 0;
  uint16_t frames = 0;  // 0 with keys = 0: release at once
};

class Navigator {
 public:
  // Delays, in cycles (one 12.2 ≈ 222 ms). Measurements: docs/protocol.md §7.3 and §7.4.
  // Short press in the menu: 1 frame (the heat pump reacts in the same cycle). 2 frames were used at first; 1 after
  // 5 scans without a missed step (~110 parameters).
  static const int PRESS_FRAMES = 1;
  static const int REACTION_WAIT = 4;      // after release (in the menu the reaction comes in the same cycle)
  static const int ENTRY_FRAMES = 45;      // + and OK: user level at ~13 frames, installer level at ~36
  static const int USER_LEVEL_MAX = 20;    // no menu after 20 cycles: the heat pump does not receive our presses
  static const int EXIT_FRAMES = 18;       // long OK: exit ~12-13 frames after the start of the press
  static const int EXIT_MAX = 30;
  static const int READINGS = 3;           // setting: value read over 3 cycles (median)
  static const int READINGS_MEASURE = 3;   // measurement (P40-P48): median over 3 cycles (~0.7 s); a one-cycle
                                           // glitch is still rejected
  static const int READINGS_P50 = 9;       // P50 jumps at every cycle (18.0 to 18.8): average over 9 cycles
  static const int MAX_NAV_STEPS = 60;
  static const int MAX_WRITE_STEPS = 120;  // water setpoint 20 → 70 by 0.5 = 100 steps
  static const int MAX_ABORTS = 3;
  // Setpoint outside the menu: steps of 0.5 °C, limits 15-30 °C in heating, reaction ~460 ms.
  static const int MAX_SETPOINT_STEPS = 40;  // 15 → 30 = 30 steps, plus screen wake-ups
  static const int MAX_NO_EFFECT = 3;        // presses in a row with no change: limit reached, or presses not received
  // Outside the menu the heat pump only takes the key on the 3rd frame held (~460 ms): 3 frames → one step;
  // 2 frames → no reaction. 4 frames: auto-repeat starts (in the menu).
  static const int PRESS_FRAMES_OUT_OF_MENU = 3;
  static const int CONFIRMATION = 9;       // cycles (~2 s) the reached setpoint must stay stable
  static constexpr float SETPOINT_MIN = 15.0f, SETPOINT_MAX = 30.0f;  // heating
  // Pool: the range of P23, the pool setpoint (CIAT sheet: 20 to 35); both ends accepted by the heat pump.
  static constexpr float POOL_SETPOINT_MIN = 20.0f, POOL_SETPOINT_MAX = 35.0f;

  enum State : uint8_t {
    IDLE, ENTRY, GOTO, OPEN, READ, CLOSE, MODIFY, VALIDATE, REREAD_OPEN, REREAD, REREAD_CLOSE,
    RESTORE, EXIT, SETPOINT, BLOCKED
  };

  // --- commands ---

  // Reads the parameters of the list, at installer level, or at user level if installer = false (entry ~3 s instead
  // of ~8 s; parameters hidden at that level are then reported absent). False (and err) if the navigator is busy.
  bool scan(const std::vector<int> &list, std::string &err, bool installer = true) {
    if (!free_(err)) return false;
    if (list.empty()) { err = "empty list"; return false; }
    targets_ = list;
    std::sort(targets_.begin(), targets_.end());
    targets_.erase(std::unique(targets_.begin(), targets_.end()), targets_.end());
    writing_ = false;
    installer_level_ = installer;
    begin_();
    return true;
  }

  // Writes Pp = v (whitelist), reads it back, then leaves the menu.
  bool write(int p, float v, std::string &err) {
    if (!free_(err)) return false;
    if (!write_allowed(p, v, err)) return false;
    installer_level_ = true;
    targets_.assign(1, p);
    writing_ = true;
    target_val_ = v;
    begin_();
    return true;
  }

  // Adjusts the room setpoint shown outside the menu, by + and − presses read back one by one. Never OK: outside
  // the menu, OK leaves programme mode (forced comfort). Only call with the dial on heating (limits 15-30).
  // The loop reads the value back after each press and never counts steps blindly.
  bool setpoint(float v, std::string &err) { return setpoint(v, SETPOINT_MIN, SETPOINT_MAX, err); }
  // Same, with the limits of the dial's mode (heating: 15-30; pool: POOL_SETPOINT_*).
  bool setpoint(float v, float vmin, float vmax, std::string &err) {
    if (!free_(err)) return false;
    if (std::isnan(v) || v < vmin - 1e-3f || v > vmax + 1e-3f ||
        std::fabs(v * 2.0f - std::round(v * 2.0f)) > 1e-3f) {
      char t[80];
      snprintf(t, sizeof t, "setpoint %.2f: outside %.1f to %.1f or not a multiple of 0.5", v, vmin, vmax);
      err = t;
      return false;
    }
    if (v_.menu) { err = "parameter menu open"; return false; }
    if (std::isnan(v_.val) || v_.txt.empty() || v_.txt.back() != 'c') { err = "setpoint not displayed"; return false; }
    targets_.clear();
    i_ = 0;
    steps_ = 0;
    no_effect_ = 0;
    aborts_ = 0;
    in_error_ = false;
    writing_ = false;
    has_orig_ = true;
    orig_ = v_.val;
    target_val_ = v;
    sp_min_ = vmin;
    sp_max_ = vmax;
    t_start_ = t_;
    t_confirm_ = 0;
    state_ = SETPOINT;
    v_prev_ = v_;  // the starting value has just been seen: no stability wait for the 1st press
    act_();
    return true;
  }

  // Stops a scan on request (thermostat): the current press is allowed to finish, then a clean exit from the menu
  // (OK to go back to the number if a value is shown, then long OK), with no error or scan end published.
  // Never during a write or a setpoint adjustment. True if the scan is going to stop.
  bool stop_scan() {
    if (!busy() || writing_ || state_ == SETPOINT) return false;
    if (state_ == EXIT) { in_error_ = true; interrupted_ = true; resume_ = false; return true; }  // already exiting
    stop_requested_ = true;
    if (state_ == ENTRY && action_) {  // + and OK held: release at once
      keys_out_ = Keys{0, 0};
      has_keys_out_ = true;
      action_ = false;
    }
    if (!action_) stop_now_();
    return true;
  }
  // Leaves a menu left open (ESP32 rebooted in the middle of a scan, abort): long OK, with no error or scan end
  // published. Navigator idle only. True if an exit is started.
  // ⚠️ A long OK validates the displayed value: harmless for a value that was only being read.
  bool exit_menu(std::string &err) {
    if (!free_(err)) return false;
    if (!v_.menu) return false;
    targets_.clear();
    i_ = 0;
    writing_ = false;
    resume_ = false;
    interrupted_ = false;
    in_error_ = true;  // finish_() will not publish a scan end
    exit_();
    return true;
  }

  // True once, after a scan stopped by stop_scan() (to restart it later).
  bool take_interruption() {
    bool r = interrupted_ && state_ == IDLE;
    if (r) interrupted_ = false;
    return r;
  }

  // Immediate stop, without a press (silent bus, transmission off): the heat pump can no longer be acted upon.
  void interrupt(const std::string &reason) {
    if (state_ == IDLE) return;
    error_(reason + ": navigation interrupted");
    keys_out_ = Keys{0, 0};
    has_keys_out_ = true;
    state_ = IDLE;
    action_ = false;
    in_error_ = false;
  }

  // After BLOCKED: to be requested explicitly (button in HA), once the menu has been checked by hand.
  void reset() {
    state_ = IDLE;
    aborts_ = 0;
    action_ = false;
  }

  // --- to be called for each valid 12.2 ---
  void cycle(const View &v) {
    t_++;
    v_prev_ = v_;
    v_ = v;
    if (state_ == IDLE || state_ == BLOCKED) return;
    if (action_) {
      if (!v.same_display(act_before_)) act_change_ = true;
      // Entry: release as soon as the installer level is reached.
      if (state_ == ENTRY) { track_entry_(); return; }
      if (state_ == EXIT) { track_exit_(); return; }
      int elapsed = t_ - act_t_;
      int end = act_n_ + 1;  // release seen (the next frame no longer carries the key)
      if (elapsed < end) return;
      if (!act_change_ && elapsed < end + REACTION_WAIT) return;
      action_ = false;
      end_action_(act_change_);
      if (stop_requested_ && !action_ && state_ != EXIT && state_ != IDLE) { stop_now_(); return; }
      // Chaining: the next press starts in this cycle (the key has already been released for one frame).
      if (!action_ && (state_ == GOTO || state_ == MODIFY || state_ == RESTORE)) act_();
      return;
    }
    if (stop_requested_) { stop_now_(); return; }
    if (state_ == READ) { read_(); return; }
    if (state_ == REREAD) { reread_(); return; }
    act_();
  }

  // Press to pass on to Link::press(), if there is a new one.
  bool press_to_send(Keys &a) {
    if (!has_keys_out_) return false;
    a = keys_out_;
    has_keys_out_ = false;
    return true;
  }

  bool take_event(Event &e) {
    if (events_.empty()) return false;
    e = events_.front();
    events_.erase(events_.begin());
    return true;
  }

  State state() const { return state_; }
  bool busy() const { return state_ != IDLE && state_ != BLOCKED; }
  uint32_t cycles() const { return t_; }

  std::string state_text() const {
    static const char *n[] = {"idle", "entering the menu", "navigating", "opening the value", "reading",
                              "back to the number", "modifying", "validating", "reading back", "reading back",
                              "reading back", "restoring the original value", "leaving the menu",
                              "adjusting the setpoint", "blocked (check the menu, then reset)"};
    std::string s = n[state_];
    if (state_ == SETPOINT) {
      char t[24];
      snprintf(t, sizeof t, " %.1f", target_val_);
      return s + t;
    }
    if (busy() && i_ < (int) targets_.size()) {
      char t[16];
      snprintf(t, sizeof t, " P%02d", targets_[i_]);
      s += t;
    }
    return s;
  }

 private:
  bool free_(std::string &err) {
    if (state_ == BLOCKED) { err = "navigator blocked: check the menu, then reset"; return false; }
    if (state_ != IDLE) { err = "navigator busy (" + state_text() + ")"; return false; }
    return true;
  }

  void begin_() {
    stop_requested_ = false;
    interrupted_ = false;
    i_ = 0;
    aborts_ = 0;
    n_ok_ = n_absent_ = 0;
    t_start_ = t_;
    steps_ = 0;
    has_orig_ = false;
    in_error_ = false;
    if (v_.menu) {  // menu already open (after an ESP restart, for instance): close it first
      resume_ = true;
      exit_();
    } else {
      resume_ = false;
      enter_();
    }
  }

  void press_(uint8_t k, int n) {
    keys_out_ = Keys{k, (uint16_t) n};
    has_keys_out_ = true;
    action_ = true;
    act_t_ = t_;
    act_n_ = n;
    act_before_ = v_;
    act_change_ = false;
  }

  void enter_() {
    state_ = ENTRY;
    press_(PLUS | OK, ENTRY_FRAMES);
  }

  void track_entry_() {
    int elapsed = t_ - act_t_;
    bool reached = installer_level_ ? v_.installer : v_.menu;
    if (reached && v_.screen == Screen::NUMBER) {
      keys_out_ = Keys{0, 0};  // release
      has_keys_out_ = true;
      action_ = false;
      state_ = GOTO;
      order_();
      return;
    }
    if (elapsed >= USER_LEVEL_MAX && !v_.menu) { action_ = false; abort_("the menu does not open (presses not received?)"); return; }
    if (elapsed >= ENTRY_FRAMES + REACTION_WAIT) { action_ = false; abort_("installer level not reached"); }
  }

  void exit_() {
    state_ = EXIT;
    press_(OK, EXIT_FRAMES);
  }

  void track_exit_() {
    int elapsed = t_ - act_t_;
    if (!v_.menu) {
      action_ = false;
      keys_out_ = Keys{0, 0};
      has_keys_out_ = true;
      if (resume_) { resume_ = false; enter_(); return; }
      finish_();
      return;
    }
    if (elapsed >= EXIT_MAX) {
      action_ = false;
      if (++aborts_ > MAX_ABORTS) { state_ = BLOCKED; error_("cannot leave the menu: navigator blocked"); return; }
      error_("menu exit not seen, trying again");
      act_safe_();
    }
  }

  void finish_() {
    state_ = IDLE;
    if (in_error_) { in_error_ = false; return; }
    if (!writing_) {
      Event e{Event::SCAN_END};
      char t[80];
      snprintf(t, sizeof t, "%d read, %d absent, %.0f s", n_ok_, n_absent_, (t_ - t_start_) * 0.222f);
      e.msg = t;
      events_.push_back(e);
    }
  }

  // Browsing order: start from the side closest to the number shown (the menu reopens after the last one viewed).
  void order_() {
    if (targets_.size() > 1 && v_.screen == Screen::NUMBER) {
      int cur = v_.num;
      if (std::abs(cur - targets_.back()) < std::abs(cur - targets_.front())) std::reverse(targets_.begin(), targets_.end());
    }
    i_ = 0;
    steps_ = 0;
  }

  // Decides the next press, from the state and the display (no action in progress).
  void act_() {
    switch (state_) {
      case GOTO: {
        if (v_.screen != Screen::NUMBER) { abort_("number expected on the display"); return; }
        int target = targets_[i_];
        if (v_.num == target) { state_ = writing_ ? MODIFY : OPEN; open_(); return; }
        if (++steps_ > MAX_NAV_STEPS) { abort_("too many steps to reach the parameter"); return; }
        from_ = v_.num;
        press_(target > v_.num ? PLUS : MINUS, PRESS_FRAMES);
        return;
      }
      case MODIFY: {
        if (v_.screen != Screen::VALUE || std::isnan(v_.val)) { abort_("numeric value expected"); return; }
        if (equal(v_.val, target_val_)) { state_ = VALIDATE; press_(OK, PRESS_FRAMES); return; }
        if (++steps_ > MAX_WRITE_STEPS) { abort_("too many steps to reach the value"); return; }
        from_val_ = v_.val;
        press_(target_val_ > v_.val ? PLUS : MINUS, PRESS_FRAMES);
        return;
      }
      case SETPOINT: {
        if (v_.menu) { abort_("menu open while adjusting the setpoint"); return; }
        // Display stable over 2 cycles: the heat pump sometimes sends a single-cycle value.
        if (!v_.same_display(v_prev_)) return;
        if (std::isnan(v_.val) || v_.txt.empty() || v_.txt.back() != 'c') {
          abort_("setpoint not displayed during the adjustment");
          return;
        }
        if (equal(v_.val, target_val_)) {
          // Confirmation: the value must hold ~2 s, the time for a press still on its way to take effect.
          if (t_confirm_ == 0) t_confirm_ = t_;
          if (t_ - t_confirm_ < CONFIRMATION) return;
          Event e{Event::SETPOINT};
          e.reading.ok = true;
          e.reading.val = v_.val;
          e.reading.txt = v_.txt;
          e.old = orig_;
          events_.push_back(e);
          state_ = IDLE;
          return;
        }
        t_confirm_ = 0;
        if (++steps_ > MAX_SETPOINT_STEPS) { abort_("too many presses to reach the setpoint"); return; }
        from_val_ = v_.val;
        press_(target_val_ > v_.val ? PLUS : MINUS, PRESS_FRAMES_OUT_OF_MENU);
        return;
      }
      case RESTORE: {
        if (v_.screen != Screen::VALUE || std::isnan(v_.val)) { state_ = EXIT; exit_(); return; }
        if (equal(v_.val, orig_)) { state_ = CLOSE; press_(OK, PRESS_FRAMES); return; }
        if (++steps_ > MAX_WRITE_STEPS) { state_ = BLOCKED; error_("cannot put the original value back: navigator blocked"); return; }
        from_val_ = v_.val;
        press_(orig_ > v_.val ? PLUS : MINUS, PRESS_FRAMES);
        return;
      }
      default:
        return;
    }
  }

  void open_() { press_(OK, PRESS_FRAMES); }

  void end_action_(bool change) {
    switch (state_) {
      case GOTO: {
        if (!change) { abort_("no reaction to + or −"); return; }
        if (v_.screen != Screen::NUMBER) { abort_("number expected after + or −"); return; }
        int target = targets_[i_];
        bool up = act_before_.num < target;
        // Passed (or wrapped around) without going through the target: absent at this level or in this configuration.
        bool overshot = up ? (v_.num > target || v_.num < from_) : (v_.num < target || v_.num > from_);
        if (overshot) {
          Event e{Event::ABSENT};
          e.p = target;
          events_.push_back(e);
          n_absent_++;
          if (writing_) { abort_("parameter absent from the menu"); return; }
          next_();
        }
        return;
      }
      case OPEN:
      case REREAD_OPEN: {
        if (v_.screen != Screen::VALUE) { abort_("the value is not displayed"); return; }
        readings_.clear();
        state_ = state_ == OPEN ? READ : REREAD;
        read_one_();
        return;
      }
      case MODIFY: {
        if (!has_orig_) {  // OK that has just opened the value
          if (v_.screen != Screen::VALUE || std::isnan(v_.val)) { abort_("numeric value expected"); return; }
          orig_ = v_.val;
          has_orig_ = true;
          steps_ = 0;
          return;
        }
        if (!change) {  // limit reached (the heat pump saturates) before the wanted value
          abort_("value outside the heat pump's limits (press had no effect)");
          return;
        }
        bool up = target_val_ > from_val_;
        if (up ? v_.val > target_val_ + 0.05f : v_.val < target_val_ - 0.05f) {
          abort_("value impossible with the adjustment step");
        }
        return;
      }
      case RESTORE:
        if (!change) { state_ = BLOCKED; error_("cannot put the original value back: navigator blocked"); }
        return;
      case SETPOINT: {
        // Only the display counts: act_() reads the value back at the next cycle. A press with no effect may be the
        // screen waking up (backlight off before the press): it is then not counted.
        if (change) { no_effect_ = 0; return; }
        if (!act_before_.backlight && v_.backlight) return;
        if (++no_effect_ >= MAX_NO_EFFECT) {
          bool at_limit = (target_val_ > from_val_ && from_val_ >= sp_max_ - 0.05f) ||
                          (target_val_ < from_val_ && from_val_ <= sp_min_ + 0.05f);
          char t[96];
          snprintf(t, sizeof t, at_limit ? "setpoint limit reached (%.1f)"
                                         : "presses had no effect, setpoint stayed at %.1f (heat-pump limit or presses not received)",
                   from_val_);
          abort_(t);
        }
        return;
      }
      case VALIDATE: {
        if (v_.screen != Screen::NUMBER || v_.num != targets_[i_]) { abort_("return to the number not seen after validation"); return; }
        state_ = REREAD_OPEN;
        press_(OK, PRESS_FRAMES);
        return;
      }
      case CLOSE:
      case REREAD_CLOSE: {
        if (v_.screen != Screen::NUMBER || v_.num != targets_[i_]) { abort_("return to the number not seen"); return; }
        if (in_error_) { exit_(); return; }
        if (state_ == REREAD_CLOSE) { exit_(); return; }
        next_();
        return;
      }
      default:
        return;
    }
  }

  void read_one_() {
    readings_.push_back(v_);
    int n = targets_[i_] == 50 ? READINGS_P50 : is_measurement(targets_[i_]) ? READINGS_MEASURE : READINGS;
    if ((int) readings_.size() < n) return;
    Reading l = summary_();
    if (state_ == READ) {
      Event e{Event::VALUE};
      e.p = targets_[i_];
      e.reading = l;
      events_.push_back(e);
      n_ok_++;
      state_ = CLOSE;
    } else {
      Event e{Event::WRITTEN};
      e.p = targets_[i_];
      e.reading = l;
      e.old = orig_;
      if (!l.ok || std::isnan(l.val) || !equal(l.val, target_val_)) {
        e.type = Event::ERROR;
        char t[96];
        snprintf(t, sizeof t, "P%02d: read back %s instead of %.1f", targets_[i_], l.txt.c_str(), target_val_);
        e.msg = t;
      }
      events_.push_back(e);
      state_ = REREAD_CLOSE;
    }
    press_(OK, PRESS_FRAMES);
  }
  void read_() {
    if (v_.screen != Screen::VALUE) { abort_("the value disappeared while reading"); return; }
    read_one_();
  }
  void reread_() { read_(); }

  // P50: average to the tenth; other measurements and settings: median of the numeric values; otherwise the last text.
  Reading summary_() const {
    Reading l;
    std::vector<float> xs;
    for (const View &v : readings_)
      if (!std::isnan(v.val)) xs.push_back(v.val);
    l.ok = true;
    if (xs.size() == readings_.size() && targets_[i_] == 50) {  // P50 jumps at every cycle: average over 9
      double sm = 0;
      for (float x : xs) sm += x;
      l.val = mean_tenth(sm, (int) xs.size());
      l.txt = text_like(readings_.back().txt, l.val);
    } else if (xs.size() == readings_.size()) {
      std::vector<float> s = xs;
      std::sort(s.begin(), s.end());
      l.val = s[s.size() / 2];
      for (const View &v : readings_)
        if (v.val == l.val) { l.txt = v.txt; break; }
    } else {
      l.txt = readings_.back().txt;
      l.val = NAN;
    }
    return l;
  }

  void next_() {
    i_++;
    steps_ = 0;
    state_ = GOTO;
    if (i_ >= (int) targets_.size()) exit_();
  }

  void error_(const std::string &m) {
    Event e{Event::ERROR};
    if (i_ < (int) targets_.size()) e.p = targets_[i_];
    e.msg = m;
    events_.push_back(e);
  }

  // Safe abort: error published, original value put back if it was changed, back to the number, exit from the menu.
  void abort_(const std::string &m) {
    error_(m);
    in_error_ = true;
    if (++aborts_ > MAX_ABORTS) { state_ = BLOCKED; error_("too many aborts: navigator blocked"); return; }
    act_safe_();
  }

  void stop_now_() {
    stop_requested_ = false;
    resume_ = false;
    interrupted_ = true;
    in_error_ = true;  // finish_() will not publish a scan end
    if (!v_.menu) { state_ = IDLE; in_error_ = false; return; }
    if (v_.screen == Screen::VALUE) { state_ = CLOSE; press_(OK, PRESS_FRAMES); return; }  // CLOSE then exit
    exit_();
  }

  void act_safe_() {
    if (!v_.menu) {  // outside the menu: release whatever is still held (failed entry), then idle
      keys_out_ = Keys{0, 0};
      has_keys_out_ = true;
      action_ = false;
      state_ = IDLE;
      in_error_ = false;
      return;
    }
    if (v_.screen == Screen::VALUE) {
      if (writing_ && has_orig_ && !std::isnan(v_.val) && !equal(v_.val, orig_)) {
        state_ = RESTORE;
        steps_ = 0;
        act_();
        return;
      }
      state_ = CLOSE;  // value unchanged: OK goes back to the number (and validates the same value)
      press_(OK, PRESS_FRAMES);
      return;
    }
    exit_();
  }

  State state_ = IDLE;
  View v_;
  uint32_t t_ = 0, t_start_ = 0;
  std::vector<int> targets_;
  int i_ = 0, steps_ = 0, from_ = 0, aborts_ = 0, n_ok_ = 0, n_absent_ = 0, no_effect_ = 0;
  View v_prev_;  // 12.2 of the previous cycle (display stability, setpoint)
  uint32_t t_confirm_ = 0;  // cycle at which the target setpoint was seen (confirmation)
  bool stop_requested_ = false, interrupted_ = false;  // scan stop requested by the thermostat
  float sp_min_ = SETPOINT_MIN, sp_max_ = SETPOINT_MAX;  // limits of the current setpoint adjustment
  bool writing_ = false, has_orig_ = false, in_error_ = false, resume_ = false;
  bool installer_level_ = true;  // level aimed at when entering the menu (scan)
  float target_val_ = NAN, orig_ = NAN, from_val_ = NAN;
  std::vector<View> readings_;
  // action in progress
  bool action_ = false, act_change_ = false;
  uint32_t act_t_ = 0;
  int act_n_ = 0;
  View act_before_;
  // outputs
  Keys keys_out_;
  bool has_keys_out_ = false;
  std::vector<Event> events_;
};

inline Navigator &navigator() {
  static Navigator n;
  return n;
}
inline Survey &survey() {
  static Survey r;
  return r;
}

}  // namespace nav
}  // namespace ciat

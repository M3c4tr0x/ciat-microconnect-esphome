// Backup of the surveyed settings (P00-P39, P51-P99) in the ESP32 flash, to get them back after a reboot.
// ESPHome preferences only really write at their sync interval (flash_write_interval, 1 min by default) and only if
// the content changed: no flash wear at every reading. Conversion logic (tested on a PC): ciat_nav.h, Backup.
#pragma once
#include "esphome/core/preferences.h"
#include "ciat_nav.h"

namespace ciat {
namespace flash {

inline esphome::ESPPreferenceObject &pref() {
  static esphome::ESPPreferenceObject p =
      esphome::global_preferences->make_preference<nav::Backup>(0xC1A70001u, true);
  return p;
}

// Returns the number of settings restored, -1 if nothing valid is in flash.
inline int load(nav::Survey &r) {
  static nav::Backup s;  // ~1.3 kB: off the stack
  if (!pref().load(&s)) return -1;
  return nav::from_backup(r, s);
}

inline bool save(nav::Survey &r) {
  static nav::Backup s;
  nav::to_backup(r, s);
  bool ok = pref().save(&s);
  if (ok) r.changed = false;
  return ok;
}

}  // namespace flash
}  // namespace ciat

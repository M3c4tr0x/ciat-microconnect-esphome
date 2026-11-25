// PC test of the bus task (esphome/ciat_link.h), ESP-IDF functions simulated by tools/stub_idf.
// g++ -std=c++17 -I tools/stub_idf -I esphome tools/test_ciat_link.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdint>
int64_t clock_us = 1000000;
int n_written = 0;
#define private public
#include "ciat_link.h"
#undef private
using namespace ciat::link;
static int failures = 0;
#define CHECK(c, m)                                                                                             \
  do {                                                                                                          \
    if (!(c)) {                                                                                                 \
      printf("FAIL %s (line %d) inhib %d type %d missed %d bad %u tolerated %u\n", m, __LINE__, L.inhib_,      \
             L.anom_.type, L.echo_missed_, L.st_.echo_bad, L.st_.echo_missed);                                  \
      failures++;                                                                                               \
    } else {                                                                                                    \
      printf("OK   %s\n", m);                                                                                   \
    }                                                                                                           \
  } while (0)
static Link L;
static int64_t t = 1000000;
static void bytes(const uint8_t *b, int n) { for (int i = 0; i < n; i++) { t += 1042; clock_us = t; L.byte_(b[i], t); } }
static void silence(int us) { t += us; clock_us = t; }
static const uint8_t F121[7] = {0xAC, 0x3D, 0x30, 0x20, 0xF9, 0x12, 0x6A};
static const uint8_t F122[8] = {0x40, 0x25, 0x00, 0x5B, 0x5B, 0xBF, 0x58, 0x82};
// Request 12 12, then our 12.1 (forced), echo read back (echo = 1), missing (0) or different (2), then a 12.2 or not.
static void cycle12(int echo, bool hp_answers) {
  silence(5000);
  const uint8_t q[2] = {0x12, 0x12};
  bytes(q, 2);
  if (L.active()) { memcpy(L.tx_buf_, F121, 7); L.tx_len_ = 7; L.sending_ = true; L.sent12_ = true; }
  silence(8000);
  bool sent = L.sending_;
  if (sent) Link::cb_send_(&L);
  if (!sent) echo = 0;
  if (echo == 1) bytes(F121, 7);
  else if (echo == 2) { uint8_t f[7]; memcpy(f, F121, 7); f[2] ^= 0x10; bytes(f, 7); }
  silence(7000);
  if (hp_answers) bytes(F122, 8);
}
static void cycle14_echo_missing() {
  silence(5000);
  const uint8_t q[2] = {0x14, 0x14};
  bytes(q, 2);
  if (L.active()) { const uint8_t f[5] = {0xEE, 1, 2, 3, 0}; memcpy(L.tx_buf_, f, 5); L.tx_len_ = 5; L.sending_ = true; }
  silence(8000);
  if (L.sending_) Link::cb_send_(&L);
}
static void reset() { L.tx_ = 10; L.allow(true); L.st_ = Stats(); }
int main() {
  reset();
  cycle12(1, true); cycle12(1, true);
  CHECK(L.inhib_ == NONE && L.st_.echo_ok == 2, "1 normal cycles");
  // Missing echo, 12.2 from the heat pump ~25 ms after the request (case seen on the reference installation).
  cycle12(0, true); cycle12(1, true);
  CHECK(L.inhib_ == NONE, "2 missed echo + 12.2: no inhibition");
  CHECK(L.anom_.type == 4 && L.anom_.b[0] == 0x40, "2 anomaly type 4 with the 12.2");
  CHECK(L.st_.echo_missed == 1 && L.st_.echo_bad == 0, "2 counted as a tolerated missed echo, not a wrong echo");
  CHECK(L.echo_missed_ == 0, "2 counter reset by the next echo");
  // 2 missed echoes in a row tolerated, the 3rd inhibits.
  cycle12(0, true); cycle12(0, true); cycle12(0, true);
  CHECK(L.inhib_ == NONE, "3 two missed in a row tolerated");
  cycle12(1, true);
  CHECK(L.inhib_ == COLLISION && L.anom_.type == 3, "3 third missed in a row: inhibition");
  // Missing echo AND no 12.2: immediate inhibition.
  reset(); cycle12(1, true); cycle12(0, false); cycle12(1, true);
  CHECK(L.inhib_ == COLLISION && L.anom_.type == 3 && L.st_.echo_bad == 1, "4 missing echo without 12.2: inhibition");
  // Wrong echo: immediate inhibition.
  reset(); cycle12(2, true);
  CHECK(L.inhib_ == COLLISION && L.anom_.type == 2, "5 wrong echo: inhibition");
  // Missing echo of a 14.1: the heat pump cannot confirm it, inhibition.
  reset(); cycle12(1, true); cycle14_echo_missing(); cycle12(1, true);
  CHECK(L.inhib_ == COLLISION && L.anom_.type == 3, "6 missing 14.1 echo: inhibition");
  // Re-arming resets the counter.
  reset(); cycle12(0, true); cycle12(0, true); reset(); cycle12(0, true); cycle12(1, true);
  CHECK(L.inhib_ == NONE, "7 re-arming resets the counter");
  printf("%d failure(s)\n", failures);
  return failures != 0;
}

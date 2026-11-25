// Real-time link with the CIAT bus (ESP-IDF), independent of the ESPHome loop and of the network.
//
// A FreeRTOS task reads the UART byte by byte and time-stamps every byte. It:
//   - copies every byte into a buffer read by the ESPHome loop (decoding and publishing);
//   - spots the heat pump's requests (14 14 and 12 12) and keeps p1 of each 12.2 (copied into b1);
//   - SHADOW MODE (always on): measures the real terminal's answer delay and checks its 12.1;
//   - EDGE TIME-STAMPING: an interrupt on the RX pin (read only, the UART keeps the pin) dates the first edge of
//     each burst. The task, delayed by 1 to 3 ms by Wi-Fi (priority 23 on a single core), only dates a byte when it
//     reads it: "edge" delays are the true ones, the others are kept for comparison;
//   - TRANSMISSION (only if compiled AND allowed AND not inhibited): schedules the answer 10.5 ms after the start of
//     the request (the terminal answers at 10.2-11.2 ms, the heat pump accepts at least up to 22 ms), then checks the
//     echo.
//
// Transmission safeguards (docs/protocol.md):
//   - tx_compiled = false: the TX pin is never connected to the UART; it stays an input pulled up to 3.3 V
//     (optocoupler LED off, bus free). No frame can leave;
//   - never speaks on its own: only in answer to a valid heat-pump request, at most 7 bytes;
//   - inhibition (re-armed by the switch, or resumed automatically after 30 s, see ciat-emulator.yaml) when:
//       another terminal answers on the bus (3 foreign frames within 10 s), the echo differs from what was sent
//       (collision), the echo is missing and the heat pump does not answer (or 3 times in a row even if it does),
//       or the heat pump stops sending 12.2 after our 12.1 (20 cycles in a row: it refuses our answers);
//   - an answer is dropped when it can no longer leave in time (more than 16 ms after the start of the request).
#pragma once
#include <cstring>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "ciat_emul.h"

namespace ciat {
namespace link {

enum Inhibition : uint8_t { NONE = 0, OTHER_TERMINAL = 1, COLLISION = 2, HP_SILENT = 3 };

inline const char *inhibition_name(uint8_t i) {
  switch (i) {
    case OTHER_TERMINAL: return "another terminal answers on the bus";
    case COLLISION: return "echo differs from the frame sent (collision)";
    case HP_SILENT: return "the heat pump no longer sends 12.2 after our answers";
    default: return "none";
  }
}

// Last anomaly seen on the bus (diagnostic of inhibitions).
// type: 1 terminal frame not recognised as our echo, 2 wrong echo, 3 echo never received complete,
// 4 echo never received but the heat pump answered with a 12.2 (tolerated).
struct Anomaly {
  uint8_t type = 0;
  uint8_t b[16] = {};      // frame (1, 2) or last burst before the next request (3)
  uint8_t n = 0;
  int32_t delay_us = 0;    // start of the frame after the start of the request
  bool expected = false;   // we were waiting for our echo at that moment
  uint8_t tx[8] = {};      // last frame sent
  uint8_t tx_n = 0;
  uint32_t no = 0;         // incremented at each anomaly
};

// Counters, read and reset by the ESPHome loop (once a minute).
struct Stats {
  uint32_t req12 = 0, req14 = 0;             // heat-pump requests
  uint32_t sent = 0, echo_ok = 0, echo_bad = 0, too_late = 0;
  uint32_t echo_missed = 0;                  // echo not read back although the heat pump answered (tolerated)
  uint32_t shadow_n = 0, shadow_diffs = 0;   // real terminal's 12.1 checked / with a difference
  uint8_t shadow_last = 0;                   // mask of the last difference (emu::shadow_diffs)
  uint8_t shadow_frame[7] = {};              // 12.1 that caused it
  uint8_t shadow_code = 0;                   // mode code expected at that moment
  uint32_t delay_n = 0, delay_min = 0xFFFFFFFF, delay_max = 0;  // real terminal's answer delay (µs)
  uint64_t delay_sum = 0;
  // Same delays, measured between the first edges of the request and of the answer (GPIO interrupt).
  uint32_t edge_n = 0, edge_min = 0xFFFFFFFF, edge_max = 0;
  uint64_t edge_sum = 0;
  // Task delay: time it reads the first byte of a burst − time of the first edge (µs, byte included).
  uint32_t read_n = 0, read_min = 0xFFFFFFFF, read_max = 0;
  uint64_t read_sum = 0;
  uint32_t no_edge = 0;  // bursts read without a matching edge (expected: 0)
};

class Link {
 public:
  static const int64_t BYTE_US = 1042;     // 10 bits at 9600 baud
  static const int64_t SILENCE_US = 3000;  // beyond this, the next byte starts a new frame
  static const int64_t ANSWER_US = 10500;  // target answer time, from the start of the request
  static const int64_t LIMIT_US = 16000;   // beyond this, the answer is dropped
  static const int64_t TERMINAL_SEEN_US = 10LL * 1000 * 1000;  // another terminal seen less than 10 s ago: no transmission
  static const int NO_122_MAX = 20;
  // Missed echo but 12.2 from the heat pump received: our frame went through, only our receiver missed it (seen on
  // the reference installation: 12.2 at 25.6 ms, normal delay). Tolerated up to ECHO_MISSED_MAX - 1 times in a row.
  static const int ECHO_MISSED_MAX = 3;
  static const int FOREIGN_MAX = 3;        // foreign frames within 10 s before concluding there is another terminal
  static const int64_t READ_MAX_US = 8000; // beyond this, the last edge could be the answer's (~10 ms)

  // rx, tx: GPIO numbers. tx_compiled = false: TX left as an input pulled up to 3.3 V, never connected to the UART.
  bool start(int rx, int tx, bool tx_compiled) {
    if (sb_) return true;
    tx_ = tx_compiled ? tx : -1;
    if (!tx_compiled && tx >= 0) {
      gpio_reset_pin((gpio_num_t) tx);
      gpio_set_direction((gpio_num_t) tx, GPIO_MODE_INPUT);
      gpio_set_pull_mode((gpio_num_t) tx, GPIO_PULLUP_ONLY);  // optocoupler LED at 0 V: off
    }
    uart_config_t cfg = {};
    cfg.baud_rate = 9600;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    if (uart_driver_install(PORT, 1024, 0, 0, nullptr, 0) != ESP_OK) return false;
    uart_param_config(PORT, &cfg);
    uart_set_pin(PORT, tx_ >= 0 ? tx_ : UART_PIN_NO_CHANGE, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_set_line_inverse(PORT, UART_SIGNAL_RXD_INV);  // the receive optocoupler inverts; TX not inverted (TX 0 = bus 0)
    gpio_set_pull_mode((gpio_num_t) rx, GPIO_PULLUP_ONLY);
    uart_set_rx_full_threshold(PORT, 1);  // one byte = one interrupt (the task can still be delayed by 1-3 ms)
    uart_set_rx_timeout(PORT, 1);
    // Interrupt on both RX edges: the pin stays an input connected to the UART, nothing is written to it.
    // Both edges, so as not to depend on the optocoupler's inversion. The GPIO interrupt service may already be
    // installed (ESP_ERR_INVALID_STATE): harmless.
    gpio_install_isr_service(0);
    gpio_set_intr_type((gpio_num_t) rx, GPIO_INTR_ANYEDGE);
    if (gpio_isr_handler_add((gpio_num_t) rx, &Link::isr_edge_, this) == ESP_OK) gpio_intr_enable((gpio_num_t) rx);
    sb_ = xStreamBufferCreate(2048, 1);
    esp_timer_create_args_t a = {};
    a.callback = &Link::cb_send_;
    a.arg = this;
    a.name = "ciat_tx";
    esp_timer_create(&a, &timer_);
    // Priority 19: above the ESPHome loop, below Wi-Fi (23) and esp_timer (22).
    xTaskCreate(&Link::task_, "ciat_bus", 4096, this, 19, nullptr);
    return true;
  }

  // Received bytes, for the decoder in the ESPHome loop.
  size_t read(uint8_t *b, size_t n) { return sb_ ? xStreamBufferReceive(sb_, b, n, 0) : 0; }

  void command(const emu::Command &c) {
    portENTER_CRITICAL(&mux_);
    cmd_ = c;
    portEXIT_CRITICAL(&mux_);
  }
  // Key press (+ 0x01, − 0x02, OK 0x04), carried by the next `cycles` 12.1 frames.
  void press(uint8_t keys, uint16_t cycles) {
    portENTER_CRITICAL(&mux_);
    press_keys_ = keys;
    press_cycles_ = cycles;
    portEXIT_CRITICAL(&mux_);
  }
  // Allowing also re-arms an inhibition. No effect if transmission is not compiled.
  void allow(bool on) {
    portENTER_CRITICAL(&mux_);
    allowed_ = on && tx_ >= 0;
    if (on) { inhib_ = NONE; no_122_ = 0; echo_missed_ = 0; }
    portEXIT_CRITICAL(&mux_);
  }
  bool tx_compiled() const { return tx_ >= 0; }
  bool allowed() const { return allowed_; }
  uint8_t inhibition() const { return inhib_; }
  bool active() const { return allowed_ && tx_ >= 0 && inhib_ == NONE; }
  // Time (esp_timer, µs) of the last inhibition.
  int64_t t_inhibition() const { return t_inhib_; }
  Anomaly anomaly() {
    portENTER_CRITICAL(&mux_);
    Anomaly a = anom_;
    portEXIT_CRITICAL(&mux_);
    return a;
  }
  bool terminal_present() const {
    return n_foreign_ >= FOREIGN_MAX && t_terminal_ != 0 && esp_timer_get_time() - t_terminal_ < TERMINAL_SEEN_US;
  }

  Stats take_stats() {
    portENTER_CRITICAL(&mux_);
    Stats s = st_;
    st_ = Stats();
    portEXIT_CRITICAL(&mux_);
    return s;
  }

  // 12.1 the emulator would send now (Home Assistant command, mode code copied from the last 12.2).
  bool predicted_121(uint8_t out[7]) {
    portENTER_CRITICAL(&mux_);
    emu::Command c = cmd_;
    uint8_t code = emu::mode_code(has_p1_, p1_);
    portEXIT_CRITICAL(&mux_);
    return emu::frame_121(c, code, 0, out);
  }

 private:
  static const uart_port_t PORT = UART_NUM_1;  // UART0 keeps its boot role; the log goes through native USB

  static void task_(void *arg) { static_cast<Link *>(arg)->loop_(); }
  // Edge on RX: an edge after more than SILENCE_US without an edge opens a burst, whose time is kept.
  static void IRAM_ATTR isr_edge_(void *arg) {
    auto *l = static_cast<Link *>(arg);
    int64_t t = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&l->mux_edge_);
    if (t - l->t_edge_ > SILENCE_US) { l->t_burst_edge_ = t; l->n_burst_edges_++; }
    l->t_edge_ = t;
    portEXIT_CRITICAL_ISR(&l->mux_edge_);
  }
  static void cb_send_(void *arg) {
    auto *l = static_cast<Link *>(arg);
    if (!l->active()) { l->sending_ = false; return; }  // inhibited in the meantime
    uart_write_bytes(PORT, (const char *) l->tx_buf_, l->tx_len_);
    portENTER_CRITICAL(&l->mux_);
    l->st_.sent++;
    portEXIT_CRITICAL(&l->mux_);
  }

  void loop_() {
    uint8_t b;
    for (;;) {
      if (uart_read_bytes(PORT, &b, 1, pdMS_TO_TICKS(100)) != 1) continue;
      int64_t t = esp_timer_get_time();
      xStreamBufferSend(sb_, &b, 1, 0);
      byte_(b, t);
    }
  }

  void inhibit_(Inhibition i) {
    portENTER_CRITICAL(&mux_);
    if (allowed_ && inhib_ == NONE) { inhib_ = i; t_inhib_ = esp_timer_get_time(); }
    portEXIT_CRITICAL(&mux_);
  }

  void record_(uint8_t type, const uint8_t *b, size_t n, int64_t delay, bool expected) {
    portENTER_CRITICAL(&mux_);
    anom_.type = type;
    anom_.n = n > sizeof anom_.b ? sizeof anom_.b : n;
    memcpy(anom_.b, b, anom_.n);
    anom_.delay_us = (int32_t) delay;
    anom_.expected = expected;
    memcpy(anom_.tx, tx_buf_, sizeof anom_.tx);
    anom_.tx_n = tx_len_;
    anom_.no++;
    portEXIT_CRITICAL(&mux_);
  }

  void byte_(uint8_t b, int64_t t) {
    if (t - t_last_ > SILENCE_US) {
      memcpy(prev_burst_, burst_, sizeof burst_);
      prev_nb_ = nb_;
      prev_t_ = t_burst_;
      nb_ = 0;
      t_burst_ = t - BYTE_US;
      burst_edge_(t);
    }
    t_last_ = t;
    if (nb_ < sizeof burst_) burst_[nb_++] = b;
    if (nb_ == 2 && burst_[0] == burst_[1] && (b == 0x12 || b == 0x14)) { request_(b, t_burst_); return; }
    uint8_t h = burst_[0];
    size_t L = h == 0x40 ? 8 : (h == 0xAC || h == 0xAE) ? 7 : h == 0xEE ? 5 : 0;
    if (L && nb_ == L) frame_(burst_, L, t_burst_);
  }

  // Matches the burst with the last burst start seen by the edge interrupt. Accepted only if the task has not
  // taken that burst yet and its edge precedes the read by less than READ_MAX_US.
  void burst_edge_(int64_t t) {
    portENTER_CRITICAL(&mux_edge_);
    int64_t tf = t_burst_edge_;
    uint32_t nf = n_burst_edges_;
    portEXIT_CRITICAL(&mux_edge_);
    int64_t lead = t - tf;
    bool ok = nf != n_taken_ && lead > 0 && lead < READ_MAX_US;
    n_taken_ = nf;
    t_burst_f_ = ok ? tf : 0;
    portENTER_CRITICAL(&mux_);
    if (ok) {
      uint32_t us = (uint32_t) lead;
      st_.read_n++;
      st_.read_sum += us;
      if (us < st_.read_min) st_.read_min = us;
      if (us > st_.read_max) st_.read_max = us;
    } else {
      st_.no_edge++;
    }
    portEXIT_CRITICAL(&mux_);
  }

  void request_(uint8_t cmd, int64_t t0) {
    // An answer sent in the previous cycle whose echo never came back complete. If it was a 12.1 and the heat pump
    // answered it with a 12.2, the frame went through: only our receiver missed it, tolerated (except 3 times in a
    // row). Otherwise (14.1, or no 12.2): probable collision, inhibition.
    if (sending_) {
      sending_ = false;
      bool accepted = req_cmd_ == 0x12 && sent12_ && seen122_;
      bool tolerated = accepted && ++echo_missed_ < ECHO_MISSED_MAX;
      record_(tolerated ? 4 : 3, prev_burst_, prev_nb_, prev_t_ - req_t0_, true);
      portENTER_CRITICAL(&mux_);
      (tolerated ? st_.echo_missed : st_.echo_bad)++;
      portEXIT_CRITICAL(&mux_);
      if (!tolerated) inhibit_(COLLISION);
    }
    if (cmd == 0x12) {
      if (sent12_ && !seen122_) { if (++no_122_ >= NO_122_MAX) inhibit_(HP_SILENT); }
      else if (seen122_) no_122_ = 0;
      sent12_ = false;
      seen122_ = false;
    }
    req_cmd_ = cmd;
    req_t0_ = t0;
    req_t0f_ = t_burst_f_;
    req_code_ = emu::mode_code(has_p1_, p1_);
    req_has_p1_ = has_p1_;
    portENTER_CRITICAL(&mux_);
    (cmd == 0x12 ? st_.req12 : st_.req14)++;
    portEXIT_CRITICAL(&mux_);

    if (!active()) return;
    if (terminal_present()) { inhibit_(OTHER_TERMINAL); return; }

    uint8_t f[7];
    bool ok;
    size_t L;
    portENTER_CRITICAL(&mux_);
    emu::Command c = cmd_;
    uint8_t keys = 0;
    if (cmd == 0x12 && press_cycles_ > 0) { keys = press_keys_; press_cycles_--; }
    portEXIT_CRITICAL(&mux_);
    if (cmd == 0x14) { ok = emu::frame_141(c, f); L = 5; }
    else { ok = emu::frame_121(c, req_code_, keys, f); L = 7; }
    if (!ok) return;  // time or room temperature unknown: stay silent (the heat pump moves on)

    // Reference time: the request's edge if known (exact), otherwise the task's read time.
    if (req_t0f_) t0 = req_t0f_;
    int64_t now = esp_timer_get_time();
    if (now - t0 > LIMIT_US) {
      portENTER_CRITICAL(&mux_);
      st_.too_late++;
      portEXIT_CRITICAL(&mux_);
      return;
    }
    int64_t d = t0 + ANSWER_US - now;
    memcpy(tx_buf_, f, L);
    tx_len_ = L;
    sending_ = true;
    if (cmd == 0x12) sent12_ = true;
    esp_timer_start_once(timer_, d > 50 ? d : 50);
  }

  void frame_(const uint8_t *f, size_t L, int64_t t0) {
    uint8_t h = f[0];
    bool xor_ok = emu::frame_xor(f, L - 1) == f[L - 1];
    if (h == 0x40) {
      if (xor_ok) { p1_ = f[1]; has_p1_ = true; seen122_ = true; }
      return;
    }
    int64_t delay = t0 - req_t0_;
    bool after_request = delay > 2000 && delay < 40000;
    if (sending_ && after_request && L == tx_len_) {  // our own echo
      sending_ = false;
      bool ok = memcmp(f, tx_buf_, L) == 0;
      portENTER_CRITICAL(&mux_);
      (ok ? st_.echo_ok : st_.echo_bad)++;
      portEXIT_CRITICAL(&mux_);
      if (ok) echo_missed_ = 0;
      if (!ok) { record_(2, f, L, delay, true); inhibit_(COLLISION); }
      return;
    }
    // Frame from another terminal (the real one, in shadow mode).
    // Inhibition after FOREIGN_MAX frames within 10 s: a real terminal answers at every cycle (detected in under a
    // second); an isolated frame (our echo badly recognised) no longer stops everything.
    int64_t now = esp_timer_get_time();
    record_(1, f, L, delay, sending_);
    if (now - t_foreign0_ > TERMINAL_SEEN_US) { t_foreign0_ = now; n_foreign_ = 0; }
    t_terminal_ = now;
    if (++n_foreign_ >= FOREIGN_MAX && active()) inhibit_(OTHER_TERMINAL);
    portENTER_CRITICAL(&mux_);
    if (after_request) {
      uint32_t us = (uint32_t) delay;
      st_.delay_n++;
      st_.delay_sum += us;
      if (us < st_.delay_min) st_.delay_min = us;
      if (us > st_.delay_max) st_.delay_max = us;
      if (req_t0f_ && t_burst_f_) {
        uint32_t e = (uint32_t) (t_burst_f_ - req_t0f_);
        st_.edge_n++;
        st_.edge_sum += e;
        if (e < st_.edge_min) st_.edge_min = e;
        if (e > st_.edge_max) st_.edge_max = e;
      }
    }
    // Checked only after a 12.2 was received: when the ESP starts, the real terminal has not restarted.
    if (h == 0xAC && L == 7 && req_cmd_ == 0x12 && req_has_p1_) {
      uint8_t e = emu::shadow_diffs(f, req_code_);
      st_.shadow_n++;
      if (e) {
        st_.shadow_diffs++;
        st_.shadow_last = e;
        memcpy(st_.shadow_frame, f, 7);
        st_.shadow_code = req_code_;
      }
    }
    portEXIT_CRITICAL(&mux_);
  }

  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  // shared with the edge interrupt (under mux_edge_)
  portMUX_TYPE mux_edge_ = portMUX_INITIALIZER_UNLOCKED;
  int64_t t_edge_ = 0, t_burst_edge_ = 0;
  uint32_t n_burst_edges_ = 0;
  StreamBufferHandle_t sb_ = nullptr;
  esp_timer_handle_t timer_ = nullptr;
  int tx_ = -1;

  // shared with the ESPHome loop (under mux_)
  emu::Command cmd_;
  uint8_t press_keys_ = 0;
  uint16_t press_cycles_ = 0;
  volatile bool allowed_ = false;
  volatile uint8_t inhib_ = NONE;
  Stats st_;

  // owned by the task (and by the timer for tx_buf_)
  uint8_t burst_[16] = {};
  size_t nb_ = 0;
  int64_t t_last_ = 0, t_burst_ = 0, req_t0_ = 0;
  int64_t t_burst_f_ = 0, req_t0f_ = 0;  // burst starts dated by edge (0 = unknown)
  uint32_t n_taken_ = 0;
  volatile int64_t t_terminal_ = 0, t_foreign0_ = 0, t_inhib_ = 0;
  volatile int n_foreign_ = 0;
  uint8_t prev_burst_[16] = {};
  size_t prev_nb_ = 0;
  int64_t prev_t_ = 0;
  Anomaly anom_;
  uint8_t req_cmd_ = 0, req_code_ = 6, p1_ = 0;
  bool has_p1_ = false, req_has_p1_ = false, sent12_ = false, seen122_ = false;
  int no_122_ = 0;
  int echo_missed_ = 0;  // echoes missed in a row (tolerated if the heat pump answered)
  uint8_t tx_buf_[8] = {};
  size_t tx_len_ = 0;
  volatile bool sending_ = false;
};

inline Link &link() {
  static Link l;
  return l;
}

}  // namespace link
}  // namespace ciat

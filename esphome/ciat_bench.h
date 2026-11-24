// BENCH firmware for the transmit stage (HCPL-0501 #2 + BC807 + BC817).
//
// ⚠️ BENCH LINE ONLY (12 V supply + ~300 Ω), NEVER on the heat pump's wires.
//
// What this code does:
//   - PATTERN: 7-byte frames of 00 at a fixed period. A multimeter reads an average voltage; with the idle voltage and
//     the known duty cycle, the low level and the current in the BC817 follow (no oscilloscope needed).
//     A continuous low level is impossible with this circuit: the reservoir (22 µF) empties in ~0.1 s and the stage
//     lets go.
//   - ECHO: frames covering the 256 byte values + 00, FF, 55, AA; each frame must be read back identically by the
//     receive side (HCPL-0501 #1, GPIO20).
//   - BIT WIDTH: an interrupt on RX dates every edge of our frames. Each pulse is compared with a whole number of bits
//     (104.17 µs): the difference measures the distortion of the transmit + receive chain.
//
// Safeguards:
//   - the TX pin is connected to the UART only if the switch is on, a mode is chosen AND the state is "ready";
//     otherwise it stays an input pulled up to 3.3 V (optocoupler #2 LED off, line free);
//   - "ready" requires 5 s without any foreign edge and a powered line. On the heat pump's bus (one request every
//     ~106 ms, even without a terminal), this state is never reached: nothing is sent;
//   - an edge outside our own frames while "ready" LOCKS transmission until the "Re-arm" button.
#pragma once
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ciat {
namespace bench {

enum Mode : uint8_t { STOP = 0, PATTERN = 1, ECHO = 2 };
enum State : uint8_t { WAITING = 0, READY = 1, NO_LINE = 2, LOCKED = 3 };

inline const char *state_name(uint8_t e) {
  switch (e) {
    case WAITING: return "waiting for 5 s of silence";
    case READY: return "ready";
    case NO_LINE: return "line not powered (bench supply off?)";
    case LOCKED: return "LOCKED: foreign traffic on the line (Re-arm button)";
    default: return "?";
  }
}

// Deviation of a pulse from a whole number of bits, in µs.
struct Deviation {
  int32_t min = INT32_MAX, max = INT32_MIN;
  int64_t sum = 0;
  uint32_t n = 0;
  void add(int32_t e) {
    if (e < min) min = e;
    if (e > max) max = e;
    sum += e;
    n++;
  }
  float mean() const { return n ? (float) sum / (float) n : 0.0f; }
};

struct Stats {
  uint32_t sent = 0, echo_ok = 0, echo_bad = 0;
  uint32_t foreign = 0;   // edges outside our frames
  uint32_t lost = 0;      // edges lost (buffer full)
  Deviation low, high;    // line-low / line-high pulses, inside our frames
  float duty = 0;         // PATTERN: share of time the line is pulled low (0 to 1), computed on the bits sent
  uint8_t state = WAITING;
  char last_bad[96] = "";
};

class Bench {
 public:
  static const int64_t BYTE_US = 1042;                 // 10 bits at 9600 baud
  static const int64_t SILENCE_NEEDED_US = 5000000;    // 5 s without a foreign edge before "ready"
  static const int64_t NO_LINE_US = 100000;            // line low for 100 ms without an edge: no voltage
  static const int64_t FRAME_GAP_US = 3000;            // beyond this, two edges do not belong to the same frame
  static const int64_t ECHO_PERIOD_US = 50000;
  static const int N_EDGES = 512;

  bool start(int rx, int tx) {
    if (started_) return true;
    rx_ = rx;
    tx_ = tx;
    detach_tx_();
    uart_config_t cfg = {};
    cfg.baud_rate = 9600;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    if (uart_driver_install(PORT, 1024, 0, 0, nullptr, 0) != ESP_OK) return false;
    uart_param_config(PORT, &cfg);
    uart_set_pin(PORT, UART_PIN_NO_CHANGE, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);  // TX connected later
    uart_set_line_inverse(PORT, UART_SIGNAL_RXD_INV);  // as in ciat_link.h: RX inverted by the optocoupler, TX not
    gpio_set_pull_mode((gpio_num_t) rx, GPIO_PULLUP_ONLY);
    uart_set_rx_full_threshold(PORT, 1);
    uart_set_rx_timeout(PORT, 1);
    gpio_install_isr_service(0);
    gpio_set_intr_type((gpio_num_t) rx, GPIO_INTR_ANYEDGE);
    if (gpio_isr_handler_add((gpio_num_t) rx, &Bench::isr_edge_, this) == ESP_OK) gpio_intr_enable((gpio_num_t) rx);
    t_foreign_ = esp_timer_get_time();
    started_ = true;
    xTaskCreate(&Bench::task_, "ciat_bench", 4096, this, 5, nullptr);
    return true;
  }

  // Commands from the ESPHome loop.
  void mode(uint8_t m) {
    if (m > ECHO) m = STOP;
    portENTER_CRITICAL(&mux_);
    if (m != mode_) { pattern_start_ = 0; pattern_bits0_ = 0; }
    mode_ = m;
    portEXIT_CRITICAL(&mux_);
  }
  void allow(bool on) { allowed_ = on; }
  void period_ms(uint32_t ms) {
    if (ms < 10) ms = 10;
    if (ms > 1000) ms = 1000;
    portENTER_CRITICAL(&mux_);
    period_us_ = ms * 1000;
    pattern_start_ = 0;
    pattern_bits0_ = 0;
    portEXIT_CRITICAL(&mux_);
  }
  void rearm() { rearm_ = true; }
  void reset_stats() { reset_ = true; }

  bool tx_connected() const { return tx_connected_; }

  Stats stats() {
    portENTER_CRITICAL(&mux_);
    Stats s = st_;
    portEXIT_CRITICAL(&mux_);
    return s;
  }

 private:
  static const uart_port_t PORT = UART_NUM_1;
  struct Edge {
    int64_t t;
    uint8_t level;  // GPIO20 level just after the edge: 1 = line low (the receive optocoupler inverts)
  };

  static void task_(void *arg) { static_cast<Bench *>(arg)->loop_(); }

  static void IRAM_ATTR isr_edge_(void *arg) {
    auto *b = static_cast<Bench *>(arg);
    int64_t t = esp_timer_get_time();
    uint8_t level = (uint8_t) gpio_get_level((gpio_num_t) b->rx_);
    uint32_t w = b->w_;
    if (w - b->r_ < (uint32_t) N_EDGES) {
      b->edges_[w % N_EDGES].t = t;
      b->edges_[w % N_EDGES].level = level;
      b->w_ = w + 1;
    } else {
      b->lost_isr_++;
    }
  }

  void attach_tx_() {
    if (tx_connected_) return;
    uart_set_pin(PORT, tx_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);  // UART idle = 1: LED off
    tx_connected_ = true;
    ESP_LOGW("bench", "TX connected to the UART (GPIO%d)", tx_);
  }
  void detach_tx_() {
    gpio_reset_pin((gpio_num_t) tx_);
    gpio_set_direction((gpio_num_t) tx_, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t) tx_, GPIO_PULLUP_ONLY);  // optocoupler #2 LED off, line free
    if (tx_connected_) ESP_LOGW("bench", "TX detached: input pulled up to 3.3 V");
    tx_connected_ = false;
  }

  void set_state_(uint8_t e) {
    if (e == state_) return;
    ESP_LOGW("bench", "state: %s", state_name(e));
    state_ = e;
    portENTER_CRITICAL(&mux_);
    st_.state = e;
    portEXIT_CRITICAL(&mux_);
  }

  // Edges seen by the interrupt: foreign (outside our frames) or width of our pulses.
  void analyse_edges_() {
    while (r_ != w_) {
      Edge f = edges_[r_ % N_EDGES];
      r_ = r_ + 1;
      bool ours = f.t >= win_start_ && f.t <= win_end_;
      if (!ours) {
        portENTER_CRITICAL(&mux_);
        st_.foreign++;
        portEXIT_CRITICAL(&mux_);
        t_foreign_ = f.t;
        prev_valid_ = false;
        if (state_ == READY) {
          detach_tx_();
          set_state_(LOCKED);
        }
        continue;
      }
      if (prev_valid_ && f.t - prev_.t < FRAME_GAP_US) {
        int64_t w = f.t - prev_.t;
        int64_t bits = (w * 9600 + 500000) / 1000000;
        if (bits >= 1 && bits <= 10) {
          int32_t e = (int32_t) (w - (bits * 1000000 + 4800) / 9600);
          portENTER_CRITICAL(&mux_);
          if (prev_.level) st_.low.add(e); else st_.high.add(e);
          portEXIT_CRITICAL(&mux_);
        }
      }
      prev_ = f;
      prev_valid_ = true;
    }
    uint32_t p = lost_isr_;
    if (p) {
      lost_isr_ = 0;
      portENTER_CRITICAL(&mux_);
      st_.lost += p;
      portEXIT_CRITICAL(&mux_);
    }
  }

  void update_state_(int64_t now) {
    if (rearm_) {
      rearm_ = false;
      t_foreign_ = now;
      set_state_(WAITING);
    }
    if (state_ == LOCKED) return;
    bool line_low = gpio_get_level((gpio_num_t) rx_) == 1;
    int64_t quiet = now - t_foreign_;
    if (line_low && quiet > NO_LINE_US && now > win_end_) {
      set_state_(NO_LINE);
      return;
    }
    if (state_ == NO_LINE) {
      if (line_low) return;
      t_foreign_ = now;  // power back: 5 s of silence again
      set_state_(WAITING);
    }
    if (state_ == WAITING && quiet > SILENCE_NEEDED_US) set_state_(READY);
  }

  static int zero_bits(uint8_t o) {
    int n = 1;  // start bit
    for (int i = 0; i < 8; i++) if (!(o & (1 << i))) n++;
    return n;
  }

  static void echo_frame_(uint32_t i, uint8_t t[7]) {
    uint32_t k = i % 41;
    for (int j = 0; j < 7; j++) {
      if (k < 37) t[j] = (uint8_t) ((k * 7 + j) & 0xFF);  // 37 × 7 = 259: the 256 values
      else if (k == 37) t[j] = 0x00;
      else if (k == 38) t[j] = 0xFF;
      else if (k == 39) t[j] = 0x55;
      else t[j] = 0xAA;
    }
  }

  void send_(const uint8_t *t, size_t n, bool pattern) {
    uart_flush_input(PORT);
    int64_t t0 = esp_timer_get_time();
    win_start_ = t0;
    win_end_ = t0 + (int64_t) n * BYTE_US + 4000;
    uart_write_bytes(PORT, (const char *) t, n);
    uart_wait_tx_done(PORT, pdMS_TO_TICKS(30));
    uint8_t r[16];
    int k = uart_read_bytes(PORT, r, n, pdMS_TO_TICKS(5));
    bool ok = k == (int) n && memcmp(r, t, n) == 0;
    int b0 = 0;
    for (size_t j = 0; j < n; j++) b0 += zero_bits(t[j]);
    portENTER_CRITICAL(&mux_);
    st_.sent++;
    if (ok) {
      st_.echo_ok++;
    } else {
      st_.echo_bad++;
      int p = snprintf(st_.last_bad, sizeof st_.last_bad, "sent");
      for (size_t j = 0; j < n && p < 40; j++) p += snprintf(st_.last_bad + p, sizeof st_.last_bad - p, " %02X", t[j]);
      p += snprintf(st_.last_bad + p, sizeof st_.last_bad - p, " | read");
      if (k <= 0) snprintf(st_.last_bad + p, sizeof st_.last_bad - p, " nothing");
      for (int j = 0; j < k && p < (int) sizeof st_.last_bad - 4; j++)
        p += snprintf(st_.last_bad + p, sizeof st_.last_bad - p, " %02X", r[j]);
    }
    if (pattern) {
      if (pattern_start_ == 0) { pattern_start_ = t0; pattern_bits0_ = 0; }
      pattern_bits0_ += b0;
    }
    portEXIT_CRITICAL(&mux_);
  }

  void loop_() {
    int64_t next = 0;
    uint32_t i = 0;
    for (;;) {
      int64_t now = esp_timer_get_time();
      if (reset_) {
        reset_ = false;
        portENTER_CRITICAL(&mux_);
        uint8_t e = st_.state;
        st_ = Stats();
        st_.state = e;
        pattern_start_ = 0;
        pattern_bits0_ = 0;
        portEXIT_CRITICAL(&mux_);
      }
      analyse_edges_();
      update_state_(now);
      uint8_t m = mode_;
      bool transmit = state_ == READY && allowed_ && m != STOP;
      if (transmit && !tx_connected_) attach_tx_();
      if (!transmit && tx_connected_) detach_tx_();
      if (transmit && now >= next) {
        uint8_t t[7];
        if (m == PATTERN) {
          memset(t, 0, sizeof t);
          next = (next && now - next < period_us_ ? next : now) + period_us_;
        } else {
          echo_frame_(i++, t);
          next = now + ECHO_PERIOD_US;
        }
        send_(t, sizeof t, m == PATTERN);
        analyse_edges_();
      } else if (!transmit) {
        next = 0;
      }
      // Pattern duty cycle, on the bits actually sent since the pattern started.
      portENTER_CRITICAL(&mux_);
      int64_t dur = esp_timer_get_time() - pattern_start_;
      st_.duty = (m == PATTERN && pattern_start_ && dur > 0)
                     ? (float) ((double) pattern_bits0_ * (1e6 / 9600.0) / (double) dur) : 0.0f;
      portEXIT_CRITICAL(&mux_);
      vTaskDelay(1);
    }
  }

  int rx_ = -1, tx_ = -1;
  bool started_ = false;
  volatile bool tx_connected_ = false, allowed_ = false, rearm_ = false, reset_ = false;
  volatile uint8_t mode_ = STOP, state_ = WAITING;
  volatile int64_t win_start_ = 0, win_end_ = 0;  // window of our last frame (echoes expected)
  int64_t t_foreign_ = 0;                          // last foreign edge (or re-arm)
  int64_t period_us_ = 20000;
  int64_t pattern_start_ = 0, pattern_bits0_ = 0;
  Edge edges_[N_EDGES] = {};
  volatile uint32_t w_ = 0, r_ = 0, lost_isr_ = 0;
  Edge prev_ = {};
  bool prev_valid_ = false;
  Stats st_;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

inline Bench &bench() {
  static Bench b;
  return b;
}

}  // namespace bench
}  // namespace ciat

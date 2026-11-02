#pragma once

// Host runtime tests for the tcl_ac component, run by tests/component.yaml.
// Real ESPHome entities and the real component; an in-memory UART replaces the
// appliance. Timestamps are real time (pacing is 500 ms, queue expiry 5 s),
// so the whole run takes about 20 seconds.

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

#include "esphome/components/tcl_ac/tcl_ac.h"
#include "esphome/components/tcl_ac/tcl_ac_climate.h"

namespace esphome {
namespace wl6u_ac {
namespace test {

class TestUART : public uart::UARTComponent {
 public:
  std::deque<uint8_t> bytes;
  std::vector<uint8_t> written;
  void write_array(const uint8_t *data, size_t len) override {
    written.insert(written.end(), data, data + len);
  }
  bool peek_byte(uint8_t *byte) override {
    if (bytes.empty()) return false;
    *byte = bytes.front();
    return true;
  }
  bool read_array(uint8_t *data, size_t len) override {
    if (bytes.size() < len) return false;
    while (len-- != 0) {
      *data++ = bytes.front();
      bytes.pop_front();
    }
    return true;
  }
  size_t available() override { return bytes.size(); }
  uart::UARTFlushResult flush() override { return uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS; }

 protected:
  void check_logger_conflict() override {}
};

// Full frame with an explicit direction header and XOR checksum.
inline std::vector<uint8_t> frame(uint8_t b1, uint8_t b2, uint8_t cmd,
                                  std::vector<uint8_t> payload) {
  std::vector<uint8_t> f = {0xBB, b1, b2, cmd, static_cast<uint8_t>(payload.size())};
  f.insert(f.end(), payload.begin(), payload.end());
  uint8_t x = 0;
  for (uint8_t b : f)
    x ^= b;
  f.push_back(x);
  return f;
}

// AC-to-module reply (BB 01 00).
inline std::vector<uint8_t> reply(uint8_t cmd, std::vector<uint8_t> payload) {
  return frame(0x01, 0x00, cmd, std::move(payload));
}

// 55-byte status payload: power/cool/24.5 C/fan code 3, room 3.5 C.
inline std::vector<uint8_t> status_payload(bool power_on) {
  std::vector<uint8_t> p(55, 0);
  if (power_on) {
    p[2] = 0x11;  // power bit, mode nibble 1 (cool)
    p[3] = 0x38;  // target 24 (24-16=8), fan receive code 3
    p[4] = 0x02;  // half-degree bit, unit 0
    p[12] = 58;   // packed room temperature, raw 235 -> 3.5 C
    p[13] = 0x0C;
    p[29] = 3;    // indoor fan, keeps the unit "active" for energy ticks
    p[33] = 60;   // compressor frequency
  }
  return p;
}

inline std::vector<uint8_t> status_reply(bool power_on) {
  return reply(0x04, status_payload(power_on));
}

// 17-byte command-0A energy reply; kwh in 0.01 steps at P[3] low nibble.
inline std::vector<uint8_t> energy_reply(uint8_t hundredths) {
  std::vector<uint8_t> p(18, 0);
  p[2] = 0x08;
  p[3] = hundredths;
  return reply(0x0A, std::move(p));
}

inline void feed(TclAc &ac, TestUART &uart, const std::vector<uint8_t> &bytes) {
  uart.bytes.insert(uart.bytes.end(), bytes.begin(), bytes.end());
  while (uart.available())
    ac.loop();
}

// Runs the component loop for a real-time window (pacing, queue expiry).
inline void settle(TclAc &ac, uint32_t ms) {
  const uint32_t until = millis() + ms;
  while (static_cast<int32_t>(until - millis()) > 0) {
    ac.loop();
    delay(10);
  }
}

inline size_t tx_at(const TestUART &uart, size_t mark, const std::vector<uint8_t> &prefix) {
  const auto it = std::search(uart.written.begin() + mark, uart.written.end(),
                              prefix.begin(), prefix.end());
  assert(it != uart.written.end());
  return size_t(it - uart.written.begin());
}

inline bool tx_since(const TestUART &uart, size_t mark, const std::vector<uint8_t> &prefix) {
  return std::search(uart.written.begin() + mark, uart.written.end(),
                     prefix.begin(), prefix.end()) != uart.written.end();
}

inline void run() {
  TclAc ac;
  TestUART uart;
  ac.set_uart_parent(&uart);
  ac.setup();  // started_ timestamps the real clock; the 15 s poll gate stays closed
  TclAcClimate climate;
  climate.set_hub(&ac);
  climate.setup();
  const std::vector<uint8_t> kTimerWritePrefix = {0xBB, 0x00, 0x01, 0x0D, 0x77};

  // Without a synchronized status, queued changes expire without any TX.
  ac.request_property("targetTemperature", 20.0f);
  settle(ac, 5500);
  assert(!ac.has_status() && uart.written.empty());

  // Checksum-invalid replies and module->AC loopback echoes never synchronize.
  auto corrupt = status_reply(true);
  corrupt.back() ^= 1;
  feed(ac, uart, corrupt);
  feed(ac, uart, frame(0x00, 0x01, 0x03, {1, 2, 3, 4, 5}));
  assert(!ac.has_status());

  // A valid status synchronizes and publishes through the climate entity.
  feed(ac, uart, status_reply(true));
  assert(ac.has_status() && ac.status_fresh());
  assert(ac.property_state("targetTemperature") == 24.5f);
  assert(ac.property_state("windSpeed") == 6.0f);
  assert(climate.mode == climate::CLIMATE_MODE_COOL);
  assert(climate.target_temperature == 24.5f && climate.current_temperature == 3.5f);
  assert(climate.get_custom_fan_mode() == "Speed 5");

  // Direct scalar write: the float32 targetElectric cents are restored before
  // the stock truncating setter (12.36f must not become 12.35).
  size_t mark = uart.written.size();
  ac.set_property("targetElectric", 12.36f);
  settle(ac, 1500);
  const size_t at = tx_at(uart, mark, {0xBB, 0x00, 0x01, 0x03, 0x20, 0x03, 0x01});
  assert(uart.written[at + 30] == 0x36 && uart.written[at + 31] == 0x12);

  // Queued changes merge into one frame after a fresh status reply.
  feed(ac, uart, status_reply(true));
  mark = uart.written.size();
  ac.request_property("targetTemperature", 26.5f);
  ac.request_property("windSpeed", 4.0f);
  settle(ac, 1500);
  const size_t merged = tx_at(uart, mark, {0xBB, 0x00, 0x01, 0x03, 0x20, 0x03, 0x01});
  assert((uart.written[merged + 9] & 0x0F) == (31 - 26));  // target nibble
  assert(uart.written[merged + 11] & 0x02);                 // half-degree flag
  assert((uart.written[merged + 10] & 0x07) == 3);          // fan wire for 4

  // A timer edit with a stale cache defers behind an automatic timers query,
  // waits if a scalar write is still awaiting status readback, then lands as a
  // 125-byte write that re-queries the timers table.
  const std::vector<uint8_t> kTimerQuery = {0xBB, 0x00, 0x01, 0x0D, 0x03, 0x05, 0x00, 0x00, 0xB1};
  feed(ac, uart, status_reply(true));
  mark = uart.written.size();
  ac.special_timer(true, 1, 1, 90);
  settle(ac, 700);
  assert(tx_at(uart, mark, kTimerQuery) >= mark);
  assert(!tx_since(uart, mark, kTimerWritePrefix));
  ac.set_property("screen", 1.0f);  // sets awaiting_status_ = true
  feed(ac, uart, reply(0x0D, std::vector<uint8_t>(119, 0)));
  settle(ac, 700);
  assert(!tx_since(uart, mark, kTimerWritePrefix));  // blocked until fresh status
  feed(ac, uart, status_reply(true));
  settle(ac, 1200);
  const size_t write_at = tx_at(uart, mark, kTimerWritePrefix);
  assert(uart.written[write_at + 7] == 0x8C);   // ON display|enable|mandatory 0x80
  assert(uart.written[write_at + 8] == 1);      // 90 minutes -> 01:30
  assert(uart.written[write_at + 9] == 30);
  assert(tx_since(uart, write_at, kTimerQuery));  // post-write cache refresh

  // With the cache fresh the write goes out immediately; bad fields never TX.
  feed(ac, uart, reply(0x0D, std::vector<uint8_t>(119, 0)));
  feed(ac, uart, status_reply(true));
  mark = uart.written.size();
  ac.weekly_timer(2, 3, 1, 1, 480, "7", 4, 3, 24.5f, 1);
  settle(ac, 1500);
  const size_t weekly = tx_at(uart, mark, kTimerWritePrefix);
  assert(uart.written[weekly + 5 + 84] == 0xE0);  // display|enable|fahrenheit
  assert(uart.written[weekly + 5 + 85] == 0x42);  // mode nibble, fan code 2
  assert(uart.written[weekly + 5 + 87] == 245);   // 24.5 C in 0.05 C steps
  assert(uart.written[weekly + 5 + 90] == 0x01);  // day mask: Sunday
  const size_t written_now = uart.written.size();
  ac.weekly_timer(2, 0, 1, 1, 0, "0", 4, 3, 24.5f, 1);  // invalid day digit
  settle(ac, 800);
  assert(uart.written.size() == written_now);

  // A deferred edit without a timer reply is dropped after 5 s, and a late
  // reply must not resurrect it.
  feed(ac, uart, status_reply(true));
  mark = uart.written.size();
  ac.special_timer(true, 1, 1, 60);
  settle(ac, 1500);
  assert(tx_since(uart, mark, kTimerQuery));
  settle(ac, 5300);
  feed(ac, uart, reply(0x0D, std::vector<uint8_t>(119, 0)));
  settle(ac, 1500);
  assert(!tx_since(uart, mark, kTimerWritePrefix));

  // Energy counter ticks drive power; the estimate validity is tick-based.
  assert(TclAc::energy_rate_valid_ms(900.0f) == 80000);
  assert(TclAc::energy_rate_valid_ms(400.0f) == 180000);
  assert(TclAc::energy_rate_valid_ms(100.0f) == 180000);
  assert(TclAc::energy_rate_valid_ms(7200.0f) == 10000);
  assert(TclAc::energy_rate_valid_ms(0.0f) == 0 && TclAc::energy_rate_valid_ms(NAN) == 0);
  feed(ac, uart, status_reply(true));
  feed(ac, uart, energy_reply(0x00));
  settle(ac, 5300);  // minimum 5 s between ticks for a rate sample
  feed(ac, uart, energy_reply(0x01));
  assert(std::fabs(ac.energy_counter() - 0.01f) < 1e-6);
  const float watts = ac.power_watts();
  assert(std::isfinite(watts) && watts > 6600.0f && watts < 7210.0f);
  // While the compressor keeps running, missing two expected ticks (~10.6 s at
  // ~6800 W) expires power_watts() to NAN instead of holding the old high rate.
  settle(ac, 5500);
  feed(ac, uart, status_reply(true));
  settle(ac, 5500);
  assert(std::isnan(ac.power_watts()));
  // Next tick resumes the estimate; compressor stop (status[33] == 0, fan on)
  // clears it immediately on the status reply.
  feed(ac, uart, energy_reply(0x02));
  assert(std::isfinite(ac.power_watts()));
  auto fan_only = status_payload(true);
  fan_only[33] = 0;
  feed(ac, uart, reply(0x04, std::move(fan_only)));
  assert(std::isnan(ac.power_watts()));
  // A fully-off unit reports 0 W, not NAN or the last nonzero estimate.
  feed(ac, uart, status_reply(false));
  assert(ac.power_watts() == 0.0f);
  assert(climate.mode == climate::CLIMATE_MODE_OFF);

  std::puts("PASS: component runtime tests");
  std::exit(0);
}

}  // namespace test
}  // namespace wl6u_ac
}  // namespace esphome

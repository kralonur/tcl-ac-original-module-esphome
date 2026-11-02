// Host codec tests, no ESPHome or hardware. From the repository root:
//   g++ -std=c++17 -Wall -Wextra -pedantic -Ifirmware/components/tcl_ac
//       firmware/test_codec.cpp -o /tmp/tcl-codec-test && /tmp/tcl-codec-test

#ifdef NDEBUG
#error "firmware/test_codec.cpp relies on assert(); build without NDEBUG"
#endif

#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ac_codec.h"
#include "ac_report.h"
#include "ac_stream.h"
#include "ac_timer_codec.h"

namespace {

// Full AC-to-module reply frame: BB 01 00 cmd len payload... XOR checksum.
std::vector<uint8_t> reply(uint8_t cmd, std::vector<uint8_t> payload) {
  std::vector<uint8_t> f = {0xBB, 0x01, 0x00, cmd, static_cast<uint8_t>(payload.size())};
  f.insert(f.end(), payload.begin(), payload.end());
  uint8_t x = 0;
  for (uint8_t b : f)
    x ^= b;
  f.push_back(x);
  return f;
}

void test_stream() {
  ::tcl_ac::Stream stream;
  const auto frame = reply(0x03, std::vector<uint8_t>(30, 0));
  // Noise, then the frame: complete exactly at the last payload byte.
  for (uint8_t b : {0x00, 0xBB, 0xFF, 0x02})
    assert(!stream.feed(b));
  for (size_t i = 0; i < frame.size() - 1; ++i)
    assert(!stream.feed(frame[i]));
  assert(stream.feed(frame.back()));
  assert(stream.size() == frame.size());
  // A header byte >1 restarts framing instead of sticking.
  stream.reset();
  assert(!stream.feed(0xBB) && !stream.feed(0x02));
  assert(stream.size() == 0);
  assert(!stream.feed(0xBB) && !stream.feed(0x00) && !stream.feed(0x01));
  assert(stream.size() == 3);
  // An over-long payload length (>249) is dropped, framing survives.
  stream.reset();
  assert(!stream.feed(0xBB) && !stream.feed(0x01) && !stream.feed(0x00) &&
         !stream.feed(0x03));
  assert(!stream.feed(0xFA));
  assert(stream.size() == 0);
  // Partial next frame does not complete early after a complete frame.
  stream.reset();
  for (uint8_t b : frame)
    stream.feed(b);
  for (size_t i = 0; i < 4; ++i)
    assert(!stream.feed(frame[i]));
}

void test_codec_set() {
  ::tcl_ac::Codec codec;
  // Stock initial state: beep on, workMode 0 wire 8, target 16.
  assert(codec.state[2] == 0x20 && codec.state[3] == 0x08 && codec.state[4] == 0x0F);
  assert(codec.set(0x00, 24.5));
  assert((codec.state[4] & 0x0F) == (31 - 24) && (codec.state[6] & 0x02));
  assert(codec.set(0x00, 25.0) && !(codec.state[6] & 0x02));
  // Ranges and step validation refuse without touching state.
  const auto before = codec.state;
  for (double v : {15.9, 31.5, 24.3, double(NAN), double(INFINITY)})
    assert(!codec.set(0x00, v));
  assert(codec.state == before);
  // Fan enum rejects 1 (stock helper); windSpeed 3 maps to wire 6.
  assert(!codec.set(0x02, 1.0));
  assert(codec.set(0x02, 3.0) && (codec.state[5] & 0x07) == 6);
  // workMode 4 (heat) maps to wire 1.
  assert(codec.set(0x05, 4.0) && (codec.state[3] & 0x0F) == 1);
  // targetElectric 12.36 packs BCD 12.36 into S[25]/S[26].
  assert(codec.set(0x21, 12.36) && codec.state[25] == 0x36 && codec.state[26] == 0x12);
  assert(!codec.set(0x21, 100.0) && !codec.set(0x21, -0.1));
  // Unknown property IDs and non-boolean flags are refused.
  assert(!codec.set(0x99, 1.0) && !codec.set(0x0C, 2.0));
  // State frame: prefix, S[2..31] at F[7..36], XOR checksum.
  const auto f = codec.frame();
  assert(f.size() == 38);
  assert(f[0] == 0xBB && f[1] == 0x00 && f[2] == 0x01 && f[3] == 0x03 &&
         f[4] == 0x20 && f[5] == 0x03 && f[6] == 0x01);
  uint8_t x = 0;
  for (size_t i = 0; i < 37; ++i)
    x ^= f[i];
  assert(f[37] == x && f[9] == codec.state[4] && f[36] == codec.state[31]);
}

void test_codec_sync() {
  // A cool-mode, fan-3, 24.5 C status payload with power on.
  std::vector<uint8_t> p(55, 0);
  p[2] = 0x11;  // power on, mode nibble 1 (cool)
  p[3] = 0x38;  // target (24-16)=8, fan code 3
  p[4] = 0x02;  // half-degree bit for unit 0
  p[5] = 0x00;
  ::tcl_ac::Codec codec;
  assert(codec.sync(p.data(), p.size()));
  assert((codec.state[3] & 0x0F) == 3);   // stock TBB maps receive cool to wire 3
  assert((codec.state[4] & 0x0F) == (31 - 24) && (codec.state[6] & 0x02));
  assert((codec.state[5] & 0x07) == 5);   // fan code 3 -> receive wire 5
  assert(codec.state[2] & 0x04);           // power on
  // Short payloads are rejected without touching state.
  const auto before = codec.state;
  std::vector<uint8_t> short_payload(24, 0);
  assert(!codec.sync(short_payload.data(), short_payload.size()));
  assert(!codec.sync(nullptr, 55));
  assert(codec.state == before);
  // Mode nibble zero triggers the stock tail: mode 1, target 26, fan auto.
  std::vector<uint8_t> off(p);
  off[2] = 0x10;
  ::tcl_ac::Codec fresh;
  assert(fresh.sync(off.data(), off.size()));
  assert((fresh.state[3] & 0x0F) == 3 && (fresh.state[4] & 0x0F) == (31 - 26) &&
         (fresh.state[5] & 0x07) == 0);
  // Index gating: a 25-byte payload applies only present fields.
  std::vector<uint8_t> early(25, 0);
  early[2] = 0x11;
  early[3] = 0x38;
  ::tcl_ac::Codec gated;
  assert(gated.sync(early.data(), early.size()));
  assert(gated.state[13] == 0);  // P[42] fan extensions not carried
}

void test_report() {
  // Packed 10-bit room temperature: (P[12] << 2) | (P[13] >> 2 & 3).
  std::vector<uint8_t> p(59, 0);
  p[12] = 58;  // 58*4 = 232
  p[13] = 0x0C;
  assert(::tcl_ac::current_temperature_raw(p.data()) == 235);
  assert(::tcl_ac::current_temperature(p.data()) == 3);
  // Error codes in stock order, once per code; group suppression at P[51..54].
  p[11] = 0x08;   // code 1 (bit 3)
  p[36] = 0x03;   // code 1 duplicate (bit 0) then code 2 (bit 1)
  p[51] = 0x81;   // code 27 (bit 0) but bit 7 suppresses the P[51] group
  const auto errors = ::tcl_ac::error_codes(p.data(), p.size());
  assert(errors.count == 2 && errors.code[0] == 1 && errors.code[1] == 2);
  // Readback helpers: target 24.5 C, fan decode, unknown-mode 0xFF.
  p[2] = 0x11;  // power on, mode nibble 1 (cool)
  p[3] = 0x38;
  p[4] = 0x02;
  const auto s = ::tcl_ac::decode_settings(p.data(), p.size());
  assert(s.power && s.work_mode == 1 && s.wind_speed == 6 &&
         s.target_temperature == 24.5f);
  assert(::tcl_ac::scalar_state(0x00, p.data(), p.size()) == 24.5f);
  assert(::tcl_ac::scalar_state(0x02, p.data(), p.size()) == 6.0f);
  // Fahrenheit unit reads the other half-degree bit.
  p[5] = 0x80;
  p[4] = 0x01;
  assert(::tcl_ac::scalar_state(0x00, p.data(), p.size()) == 24.5f);
  // Unknown received codes report NAN, not a raw echo; short payloads gate.
  p[2] = 0x10;  // mode nibble 0: no stock value
  assert(std::isnan(::tcl_ac::scalar_state(0x05, p.data(), p.size())));
  assert(std::isnan(::tcl_ac::scalar_state(0x06, p.data(), 14)));
  assert(std::isnan(::tcl_ac::scalar_state(0x14, p.data(), p.size())));  // beep
  // Direction readback: parked -> fixed + 8; swinging -> moving bits.
  assert(::tcl_ac::swing_direction(0x00, false) == 8);
  assert(::tcl_ac::swing_direction(0x10, true) == 2);
  assert(::tcl_ac::swing_direction(0x00, true) == 8);
  p[46] = 0x10;
  p[5] = 0x40;
  assert(::tcl_ac::scalar_state(0x22, p.data(), p.size()) == 2.0f);
}

void test_timers() {
  ::tcl_ac::Timers timers;
  // Without a synchronized 119-byte cache every edit is refused.
  assert(!timers.special(true, 1, 1, 90) && !timers.clear_bank(1));
  assert(timers.sync(nullptr, 119) == false);
  std::vector<uint8_t> snapshot(118, 0);
  assert(!timers.sync(snapshot.data(), snapshot.size()));
  snapshot.assign(119, 0);
  assert(timers.sync(snapshot.data(), snapshot.size()) && timers.ready);
  // Special ON at 01:30: display|enable bits plus the mandatory 0x80.
  assert(timers.special(true, 1, 1, 90));
  assert(timers.payload[2] == 0x8C && timers.payload[3] == 1 && timers.payload[4] == 30);
  // Special OFF sets its own bit pair; the ON pair stays untouched.
  assert(timers.special(false, 1, 1, 720));
  assert(timers.payload[2] == 0x8F && timers.payload[5] == 12 && timers.payload[6] == 0);
  // Field validation refuses everything invalid without mutating.
  const auto before = timers.payload;
  assert(!timers.special(true, 2, 1, 0) && !timers.special(true, 1, 5, 0));
  assert(!timers.special(true, 1, 1, 1440) && !timers.special(true, 1, 1, -1));
  assert(timers.payload == before);
  // Weekly bank 1 slot 0: power nibble, minutes and a 11111000 day mask.
  assert(timers.weekly(1, 0, 1, 1, 480, "12345", 1, 0, NAN, 0));
  const uint8_t *rec = timers.payload.data() + 7;
  assert(rec[0] == 0xC0 && rec[1] == 0x10 && rec[4] == 8 && rec[5] == 0 &&
         rec[6] == 0x3E);
  // Weekly bank 2 slot 3: mode/fan nibbles, tenths-of-0.05 C, fahrenheit bit.
  assert(timers.weekly(2, 3, 1, 1, 480, "7", 4, 3, 24.5, 1));
  rec = timers.payload.data() + 7 + 56 + 3 * 7;
  assert(rec[0] == 0xE0 && rec[1] == 0x42 && rec[2] == 0 && rec[3] == 245 &&
         rec[4] == 8 && rec[5] == 0 && rec[6] == 0x01);
  // Bank 2 validation: half-degree and range limits, day characters 1-7.
  const auto before2 = timers.payload;
  assert(!timers.weekly(2, 0, 1, 1, 0, "1", 0, 0, 24.3, 0));
  assert(!timers.weekly(2, 0, 1, 1, 0, "1", 0, 0, 15.9, 0));
  assert(!timers.weekly(2, 8, 1, 1, 0, "1", 0, 0, 16.0, 0));
  assert(!timers.weekly(2, 0, 1, 1, 0, "0", 0, 0, 16.0, 0));
  assert(!timers.weekly(2, 0, 1, 1, 0, "8", 0, 0, 16.0, 0));
  assert(!timers.weekly(2, 0, 1, 1, 0, "", 0, 0, 16.0, 0));
  assert(!timers.weekly(3, 0, 1, 1, 0, "1", 0, 0, 16.0, 0));
  assert(timers.payload == before2);
  // Clearing bank 1 zeroes its 56 bytes, preserving P[0:7] and bank 2.
  const uint8_t bank2_first = timers.payload[63];
  assert(timers.clear_bank(1) && timers.payload[2] == 0x8F);
  for (size_t i = 7; i < 63; ++i)
    assert(timers.payload[i] == 0);
  assert(timers.payload[63] == bank2_first && timers.payload[118] == 0);
  assert(!timers.clear_bank(3));
  // Write frame: 125 bytes, prefix, payload at F[5:124], XOR at F[124].
  const auto f = timers.frame();
  assert(f.size() == 125);
  assert(f[0] == 0xBB && f[1] == 0x00 && f[2] == 0x01 && f[3] == 0x0D && f[4] == 0x77);
  uint8_t x = 0;
  for (size_t i = 0; i < 124; ++i)
    x ^= f[i];
  assert(f[124] == x && f[5] == timers.payload[0]);
  // Local date/time frame from the stock template.
  const auto d = ::tcl_ac::date_frame(2026, 12, 31, 23, 59, 59);
  assert(d.size() == 22 && d[3] == 0x0B && d[4] == 0x10 && d[5] == 0x05);
  assert(d[7] == 0xEA && d[8] == 0x07 && d[9] == 12 && d[10] == 31);
  assert(d[11] == 23 && d[12] == 59 && d[13] == 59);
  x = 0;
  for (size_t i = 0; i < 21; ++i)
    x ^= d[i];
  assert(d[21] == x);
}

}  // namespace

int main() {
  test_stream();
  test_codec_set();
  test_codec_sync();
  test_report();
  test_timers();
  std::puts("PASS: codec tests");
  return 0;
}

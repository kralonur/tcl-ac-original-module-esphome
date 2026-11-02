#pragma once

// TCL WL6U AC special/weekly timer and date/time codec. Plain C++11, no ESPHome
// or Arduino dependencies, so it can be compiled and diffed against stock
// firmware dumps on the host.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace tcl_ac {

// Wire layout of the 125-byte special/weekly timer write frame
// (stock builder 0x59972, command 0x0D):
//
//   F[0]      BB
//   F[1:3]    00 01
//   F[3]      0D  timer command
//   F[4]      77  119-byte payload
//   F[5:124]  payload (timer cache at 0x1000A744)
//   F[124]    XOR of F[0:124]

constexpr std::size_t kTimerPayloadSize = 119;
constexpr std::size_t kTimerFrameSize = 125;

inline uint8_t xor_bytes(const uint8_t* data, std::size_t length) {
  uint8_t sum = 0;
  for (std::size_t i = 0; i < length; ++i) {
    sum = static_cast<uint8_t>(sum ^ data[i]);
  }
  return sum;
}

class Timers {
 public:
  std::array<uint8_t, kTimerPayloadSize> payload{};
  bool ready = false;

  // Synchronizes from an incoming command-0D payload. Accepts only an exact
  // 119-byte snapshot so opaque prefix bytes P[0:2] and both banks are known.
  // Invalid inputs return false without mutating the cached payload or ready.
  bool sync(const uint8_t* data, std::size_t length) {
    if (data == nullptr || length != kTimerPayloadSize) {
      return false;
    }
    std::memcpy(payload.data(), data, kTimerPayloadSize);
    ready = true;
    return true;
  }

  // Updates the special ON (on=true) or OFF (on=false) timer in P[2:7]
  // (stock 0x590FC..0x5920C). Validates all inputs before mutating payload.
  bool special(bool on, int display, int enable, int minutes) {
    if (!ready) return false;
    if ((display != 0 && display != 1) || (enable != 0 && enable != 1)) {
      return false;
    }
    if (minutes < 0 || minutes > 1439) {
      return false;
    }
    const uint8_t hour = static_cast<uint8_t>(minutes / 60);
    const uint8_t minute = static_cast<uint8_t>(minutes % 60);
    const uint8_t disp_bit = on ? 0x08 : 0x02;
    const uint8_t en_bit = on ? 0x04 : 0x01;
    if (display) {
      payload[2] = static_cast<uint8_t>(payload[2] | disp_bit);
    } else {
      payload[2] = static_cast<uint8_t>(payload[2] & ~disp_bit);
    }
    if (enable) {
      payload[2] = static_cast<uint8_t>(payload[2] | en_bit);
    } else {
      payload[2] = static_cast<uint8_t>(payload[2] & ~en_bit);
    }
    payload[2] = static_cast<uint8_t>(payload[2] | 0x80);
    if (on) {
      payload[3] = hour;
      payload[4] = minute;
    } else {
      payload[5] = hour;
      payload[6] = minute;
    }
    return true;
  }

  // Edits a single weekly timer record (bank 1 in P[7:63], bank 2 in P[63:119],
  // slot 0..7, 7 bytes per record; stock 0x592B6..0x596B2) while preserving all
  // other records and opaque bytes. Validates all parameters before mutating.
  bool weekly(int bank, int slot, int display, int enable, int minutes,
              const std::string& days, int power_or_mode, int fan,
              double temperature, int fahrenheit) {
    if (!ready) return false;
    if (bank != 1 && bank != 2) return false;
    if (slot < 0 || slot > 7) return false;
    if ((display != 0 && display != 1) || (enable != 0 && enable != 1)) {
      return false;
    }
    if (minutes < 0 || minutes > 1439) return false;
    if (days.empty()) return false;
    uint8_t week_mask = 0;
    for (char c : days) {
      if (c < '1' || c > '7') return false;
      const uint8_t bit = (c == '7') ? 0 : static_cast<uint8_t>(c - '0');
      week_mask = static_cast<uint8_t>(week_mask | (1u << bit));
    }

    int tenths = 0;
    if (bank == 1) {
      if (power_or_mode != 0 && power_or_mode != 1) return false;
    } else {
      if (power_or_mode < 0 || power_or_mode > 4) return false;
      if (fan < 0 || fan > 7) return false;
      if (fahrenheit != 0 && fahrenheit != 1) return false;
      if (!std::isfinite(temperature) || temperature < 16.0 ||
          temperature > 31.0) {
        return false;
      }
      const double doubled = temperature * 2.0;
      const double rounded = std::round(doubled);
      if (std::fabs(doubled - rounded) > 1e-6) return false;
      tenths = static_cast<int>(rounded * 5.0);
    }

    uint8_t* rec = payload.data() + 7 +
                   static_cast<std::size_t>(bank - 1) * 56 +
                   static_cast<std::size_t>(slot) * 7;
    if (bank == 1) {
      rec[0] = static_cast<uint8_t>((rec[0] & 0x3F) | (display << 7) |
                                    (enable << 6));
      rec[1] = static_cast<uint8_t>((rec[1] & 0x0F) |
                                    ((power_or_mode & 0x0F) << 4));
    } else {
      rec[0] = static_cast<uint8_t>((rec[0] & 0x1F) | (display << 7) |
                                    (enable << 6) | (fahrenheit << 5));
      const uint8_t fan_code =
          static_cast<uint8_t>(fan == 0 ? 7 : (fan - 1) & 0x0F);
      rec[1] = static_cast<uint8_t>(((power_or_mode & 0x0F) << 4) | fan_code);
      rec[2] = static_cast<uint8_t>((tenths >> 8) & 0xFF);
      rec[3] = static_cast<uint8_t>(tenths & 0xFF);
    }
    rec[4] = static_cast<uint8_t>(minutes / 60);
    rec[5] = static_cast<uint8_t>(minutes % 60);
    rec[6] = week_mask;
    return true;
  }

  // Zeroes the 56-byte weekly bank (stock 0x592B6..0x592C6), preserving P[0:7]
  // and the other bank.
  bool clear_bank(int bank) {
    if (!ready) return false;
    if (bank != 1 && bank != 2) return false;
    std::memset(payload.data() + 7 + static_cast<std::size_t>(bank - 1) * 56, 0,
                56);
    return true;
  }

  std::array<uint8_t, kTimerFrameSize> frame() const {
    std::array<uint8_t, kTimerFrameSize> out{};
    out[0] = 0xBB;
    out[1] = 0x00;
    out[2] = 0x01;
    out[3] = 0x0D;
    out[4] = 0x77;
    std::memcpy(out.data() + 5, payload.data(), kTimerPayloadSize);
    out[124] = xor_bytes(out.data(), 124);
    return out;
  }
};

// Builds the 22-byte command-0B local date/time frame from stock template
// 0xED070 (builder 0x546A4):
//   F[0:7]   BB 00 01 0B 10 05 00
//   F[7:9]   year (little-endian uint16)
//   F[9:14]  month, day, hour, minute, second
//   F[14:21] 00..00
//   F[21]    XOR checksum
inline std::array<uint8_t, 22> date_frame(int year, int month, int day,
                                          int hour, int minute, int second) {
  std::array<uint8_t, 22> out{};
  out[0] = 0xBB;
  out[1] = 0x00;
  out[2] = 0x01;
  out[3] = 0x0B;
  out[4] = 0x10;
  out[5] = 0x05;
  out[6] = 0x00;
  out[7] = static_cast<uint8_t>(year & 0xFF);
  out[8] = static_cast<uint8_t>((year >> 8) & 0xFF);
  out[9] = static_cast<uint8_t>(month & 0xFF);
  out[10] = static_cast<uint8_t>(day & 0xFF);
  out[11] = static_cast<uint8_t>(hour & 0xFF);
  out[12] = static_cast<uint8_t>(minute & 0xFF);
  out[13] = static_cast<uint8_t>(second & 0xFF);
  out[21] = xor_bytes(out.data(), 21);
  return out;
}

}  // namespace tcl_ac

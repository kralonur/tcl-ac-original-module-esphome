// Stock-faithful scalar state codec for the TCL AC Wi-Fi module application
// V8-R82CT20-WFMV205.11.3.27 (application payload SHA-256 e2e7b82b...).
// Pure C++11, no external dependencies.
//
// The codec owns the 32-byte outgoing state buffer S exactly as the stock
// firmware keeps it at 0x1000AECC. A store to S[i] becomes full-frame byte
// F[i+5]; command-03 state frames carry S[2..31] at F[7..36].
//
// set(id, value) reproduces the stock setter dispatch (0x53A50) for all 50
// named non-noop scalar property IDs plus the two unnamed louver subfield
// IDs 0x34/0x35. Factory/inspection (0x3E/0x3F), Wi-Fi notification (0x43),
// logging-only (0x58) and no-op (0x26/0x4B) IDs are intentionally not
// implemented; set() also rejects non-finite, non-integral or out-of-range
// values without touching state.
//
// sync(payload, length) reproduces the stock status synchronization block
// 0x5748C for checksum-valid command 03/04 status payloads. It is index
// gated like the stock function: a field is applied only when the payload
// actually carries that byte, so 55- and 59-byte statuses synchronize their
// present fields and preserve the rest, without demanding 63-byte payloads.
// Payloads shorter than kMinStatusLength are rejected untouched: stock
// unconditionally performs a fixed ten-byte copy P[15..24] -> S[15..24]
// for any length > 15, which reads one byte past a 16..24 byte payload.

#ifndef FIRMWARE_AC_CODEC_H_
#define FIRMWARE_AC_CODEC_H_

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace tcl_ac {

class Codec {
 public:
  // Minimum 03/04 status payload length accepted by sync().
  static constexpr size_t kMinStatusLength = 25;

  // 32-byte outgoing state; S[i] is transmitted as full-frame byte F[i+5].
  std::array<uint8_t, 32> state;

  Codec() : state{} {
    // Stock profile initialization (0x7B304..0x7B352): zeroed 32-byte state,
    // then setters workMode=0 (wire 8), beepSwitch=1, targetTemperature=16.0.
    state[2] = 0x20;
    state[3] = 0x08;
    state[4] = 0x0F;
  }

  // Applies one stock property ID to the state. Returns false and leaves the
  // state untouched for unimplemented IDs or values outside the validated
  // ranges (matching the stock setter validation tables).
  bool set(uint8_t id, double value) {
    if (!std::isfinite(value)) return false;
    switch (id) {
      // Plain booleans, validated 0/1; stock set/clear masks.
      case 0x01:  // powerSwitch, F[7] & 04 (0x53B7A)
        return flag(state[2], 0x04, value);
      case 0x03:  // horizontalSwitch, F[11] & 08 (0x53BF2)
        return flag(state[6], 0x08, value);
      case 0x04:  // verticalSwitch, F[10] & 38 (0x53C52)
        return flag(state[5], 0x38, value);
      case 0x0B:  // PTC, F[11] & 80 (0x54014)
        return flag(state[6], 0x80, value);
      case 0x0C:  // ECO, F[7] & 80 (0x53E82)
        return flag(state[2], 0x80, value);
      case 0x0D:  // healthy, F[8] & 10 (0x53EC4)
        return flag(state[3], 0x10, value);
      case 0x0E:  // antiMoldew, F[8] & 20 (0x53EE4)
        return flag(state[3], 0x20, value);
      case 0x0F:  // turbo / leftTurbo, F[8] & 40 (0x53F04)
        return flag(state[3], 0x40, value);
      case 0x10:  // screen, F[7] & 40 (0x53F44)
        return flag(state[2], 0x40, value);
      case 0x11:  // 3DAirSurply, F[11] & 01 (0x53FF4)
        return flag(state[6], 0x01, value);
      case 0x14:  // beepSwitch, F[7] & 20 (0x540C4)
        return flag(state[2], 0x20, value);
      case 0x1A:  // softWind, F[10] & 40 (0x54124)
        return flag(state[5], 0x40, value);
      case 0x1C:  // lightSense, F[19] & 80 (0x53FD4)
        return flag(state[14], 0x80, value);
      case 0x1D:  // selfLearn, F[19] & 40 (0x54104)
        return flag(state[14], 0x40, value);
      case 0x1F:  // selfClean, F[19] & 10 (0x54168)
        return flag(state[14], 0x10, value);
      case 0x24:  // silenceSwitch / leftSilenceSwitch, F[8] & 80 (0x53F24)
        return flag(state[3], 0x80, value);
      case 0x25:  // filterBlockSwitch, F[19] & 08 (0x540E4)
        return flag(state[14], 0x08, value);
      case 0x28:  // eightAddHot, F[10] & 80 (0x5421A); F[33] & 80 follows the
                  // status-cache P[27] bit 6 request flag, not the input.
        if (!boolean(value)) return false;
        apply(state[5], 0x80, value != 0.0);
        apply(state[28], 0x80, eight_hot_request_ != 0);
        return true;
      case 0x2A:  // feelTheWind, F[32] & 80 (0x542C4)
        return flag(state[27], 0x80, value);
      case 0x2B:  // highTemperatureWind, F[32] & 40 (0x542E4)
        return flag(state[27], 0x40, value);
      case 0x2D:  // aiFunction, F[35] & 20 (0x5431C)
        return flag(state[30], 0x20, value);
      case 0x2E:  // constTemperatureDehumidification, F[35] & 10 (0x5433C)
        return flag(state[30], 0x10, value);
      case 0x2F:  // temperatureHumidityControlling, F[35] & 08 (0x5435C)
        return flag(state[30], 0x08, value);
      case 0x30:  // rightHorizontalSwitch, F[34] & 40 (0x543A0)
        return flag(state[29], 0x40, value);
      case 0x31:  // leftHorizontalSwitch, F[11] & 08 (0x5437E)
        return flag(state[6], 0x08, value);
      case 0x36:  // rightSilenceSwitch, F[35] & 80 (0x54474)
        return flag(state[30], 0x80, value);
      case 0x37:  // rightTurbo, F[35] & 40 (0x54496)
        return flag(state[30], 0x40, value);
      case 0x39:  // surroundWind, F[36] & 04 (0x5458C)
        return flag(state[31], 0x04, value);
      case 0x3A:  // polarityFreeze, F[36] & 08 (0x545AE)
        return flag(state[31], 0x08, value);
      case 0x3D:  // temperatureType, F[12] & 80 (0x53B98)
        return flag(state[7], 0x80, value);
      case 0x40:  // infrPower, F[34] & 80 (0x5404E)
        return flag(state[29], 0x80, value);
      case 0x41:  // beepTempEn, F[11] & 40 (0x54606)
        return flag(state[6], 0x40, value);

      // Small integer fields.
      case 0x06:  // sleep, F[19] low three bits (0x54034)
        if (!small_int(value, 0, 7)) return false;
        state[14] = (state[14] & 0xF8) | static_cast<uint8_t>(value);
        return true;
      case 0x17:  // verticalWind, F[18] & E0, input shifted left 5 (0x53D10)
        if (!small_int(value, 0, 3)) return false;
        state[13] = static_cast<uint8_t>((state[13] & 0x1F) |
                                         static_cast<uint8_t>(value) << 5);
        return true;
      case 0x18:  // horizontalWind, F[18] & 1C, input shifted left 2 (0x53CF6)
        if (!small_int(value, 0, 7)) return false;
        state[13] = static_cast<uint8_t>((state[13] & 0xE3) |
                                         static_cast<uint8_t>(value) << 2);
        return true;
      case 0x1B:  // advancedECOMode, F[12] & 1C, input shifted left 2 (0x53EA2)
        if (!small_int(value, 0, 3)) return false;
        state[7] = static_cast<uint8_t>((state[7] & 0xE3) |
                                        static_cast<uint8_t>(value) << 2);
        return true;
      case 0x29: {  // generatorMode, helper 0x52AD0
        if (!small_int(value, 0, 7)) return false;
        const uint8_t v = static_cast<uint8_t>(value);
        state[13] = (state[13] & 0xFC) | (v & 3);
        if (v > 3) state[31] |= 0x40; else state[31] &= 0xBF;
        return true;
      }
      case 0x2C:  // LRWideAngleFreeze, F[33] low three bits (0x54304)
        if (!small_int(value, 0, 7)) return false;
        state[28] = (state[28] & 0xF8) | static_cast<uint8_t>(value);
        return true;
      case 0x34:  // unnamed right-horizontal subfield, S[29] low bits (0x5445A)
        if (!small_int(value, 0, 7)) return false;
        state[29] = (state[29] & 0xF8) | static_cast<uint8_t>(value);
        return true;
      case 0x35:  // unnamed left-horizontal subfield, S[28] low bits (0x54440)
        if (!small_int(value, 0, 7)) return false;
        state[28] = (state[28] & 0xF8) | static_cast<uint8_t>(value);
        return true;
      case 0x3B:  // twoTemperature, F[36] low two bits (0x545D0)
        if (!small_int(value, 0, 3)) return false;
        state[31] = (state[31] & 0xFC) | static_cast<uint8_t>(value);
        return true;
      case 0x3C:  // twoTemperatureSkew, F[36] & 30, input shifted left 4
        if (!small_int(value, 0, 3)) return false;
        state[31] = static_cast<uint8_t>((state[31] & 0xCF) |
                                         static_cast<uint8_t>(value) << 4);
        return true;

      // Fan enums (stock helper 0x7A490 rejects input 1).
      case 0x02:  // windSpeed / leftWindSpeed, F[10] & 07 (0x53BE2)
        return fan_enum(state[5], value);
      case 0x38:  // rightWindSpeed, F[35] low three bits (0x544B8)
        return fan_enum(state[30], value);

      // workMode (stock helper 0x7A3E8), F[8] low nibble.
      case 0x05: {
        if (!small_int(value, 0, 4)) return false;
        static const uint8_t kWire[5] = {8, 3, 2, 7, 1};
        state[3] = (state[3] & 0xF0) | kWire[static_cast<int>(value)];
        return true;
      }

      // infrDirect, F[12] low two bits; stock swaps 1/2 (0x5406E).
      case 0x0A: {
        if (!integral(value) || value < 0.0 || value > 2.0) return false;
        static const uint8_t kWire[3] = {0, 2, 1};
        state[7] = (state[7] & 0xFC) | kWire[static_cast<int>(value)];
        return true;
      }

      // Temperatures.
      case 0x00: {  // targetTemperature, stock helper 0x796E8
        if (!in_range(value, 16.0, 31.0)) return false;
        const double twice = value * 2.0;
        if (twice != std::floor(twice)) return false;  // half-degree steps
        const int whole = static_cast<int>(value);     // truncates toward zero
        state[4] = static_cast<uint8_t>((state[4] & 0xF0) | ((31 - whole) & 0x0F));
        // Fraction flag threshold is > 0.1, not rounding to the nearest half.
        const bool half = (value - static_cast<double>(whole)) > 0.1;
        apply(state[6], (state[7] & 0x80) ? 0x04 : 0x02, half);
        return true;
      }
      case 0x19: {  // constantTemp, F[9] high nibble (0x54144)
        if (!in_range(value, 16.0, 31.0) || value != std::floor(value)) {
          return false;
        }
        const int whole = static_cast<int>(value);
        state[4] = static_cast<uint8_t>((state[4] & 0x0F) | ((31 - whole) & 0x0F) << 4);
        return true;
      }

      // Louver direction subfields (stock encodings, including the
      // right-hand raw-OR quirk at 0x543C2 for inputs 8..16).
      case 0x22: {  // verticalDirection, S[27] bits 3..5 / 0..2
        if (!integral(value)) return false;
        if (value >= 0.0 && value <= 3.0) {
          state[27] = static_cast<uint8_t>((state[27] & 0xC7) |
                                           static_cast<uint8_t>(value) << 3);
          return true;
        }
        if (value >= 8.0 && value <= 13.0) {
          state[27] = static_cast<uint8_t>((state[27] & 0xF8) |
                                           static_cast<uint8_t>(value - 8.0));
          return true;
        }
        return false;
      }
      case 0x23: {  // horizontalDirection / leftHorizontalDirection, S[28]
        if (!integral(value)) return false;
        if (value >= 0.0 && value <= 7.0) {
          state[28] = static_cast<uint8_t>((state[28] & 0xC7) |
                                           static_cast<uint8_t>(value) << 3);
          return true;
        }
        if (value >= 8.0 && value <= 15.0) {
          state[28] = static_cast<uint8_t>((state[28] & 0xF8) |
                                           static_cast<uint8_t>(value - 8.0));
          return true;
        }
        if (value == 16.0) {
          state[28] |= 0x40;  // sets bit 6 without clearing direction fields
          return true;
        }
        return false;
      }
      case 0x32: {  // rightHorizontalDirection, S[29]
        if (!integral(value)) return false;
        if (value >= 0.0 && value <= 7.0) {
          state[29] = static_cast<uint8_t>((state[29] & 0xC7) |
                                           static_cast<uint8_t>(value) << 3);
          return true;
        }
        if (value >= 8.0 && value <= 16.0) {
          // Stock quirk: ORs the raw input without subtracting 8, so bits 3/4
          // of S[29] can change too (0x543DC-0x5443E).
          state[29] = static_cast<uint8_t>((state[29] & 0xF8) |
                                           static_cast<uint8_t>(value));
          return true;
        }
        return false;
      }

      // targetElectric: stock typed helper 0x541AE packs trunc(value * 100)
      // as two BCD bytes into S[25]/S[26]. Exact double arithmetic, matching
      // the stock soft-float multiply-then-truncate sequence.
      case 0x21: {
        if (value < 0.0 || value > 99.99) return false;
        const int cents = static_cast<int>(value * 100.0);  // truncates
        const int low = cents % 10;
        const int tens = (cents % 100) / 10;
        const int hundreds = (cents / 100) % 10;
        const int thousands = cents / 1000;
        state[25] = static_cast<uint8_t>(low | (tens << 4));
        state[26] = static_cast<uint8_t>(hundreds | (thousands << 4));
        return true;
      }

      default:
        return false;
    }
  }

  // Applies a checksum-valid command 03/04 status payload (the frame bytes
  // starting at F[5]) exactly like stock synchronization 0x5748C. Every field
  // is index-gated: only bytes the payload actually carries are applied, so
  // 55- and 59-byte statuses work without the 63-byte extension bytes.
  // Returns false without touching state for payloads shorter than
  // kMinStatusLength.
  bool sync(const uint8_t* p, size_t n) {
    if (p == nullptr || n < kMinStatusLength) return false;

    // Stock copies the payload into the receive status cache here; only
    // cache byte 27 bit 6 feeds back into state (via the eightAddHot setter),
    // and only when the payload carries byte 27.
    if (n >= 28) eight_hot_request_ = (p[27] & 0x40) ? 1 : 0;

    // Stock clears these two bits on every ordinary synchronization.
    state[6] &= 0xBF;
    state[2] &= 0xFE;

    // Stock reads P[2] before its length gate.
    const uint8_t mode = p[2] & 0x0F;

    if (n > 2) {
      // P[2]: power bit 4 -> S[2] 0x04, screen bit 5 -> 0x40, ECO bit 6 ->
      // 0x80, turbo bit 7 -> S[3] 0x40. Mode nibble via stock TBB 0x574E4:
      // 1->3, 2->7, 3->2, 4->1, 5->8, 0 and 6..15 -> 3.
      if (p[2] & 0x10) state[2] |= 0x04; else state[2] &= 0xFB;
      if (p[2] & 0x20) state[2] |= 0x40; else state[2] &= 0xBF;
      if (p[2] & 0x40) state[2] |= 0x80; else state[2] &= 0x7F;
      static const uint8_t kModeWire[6] = {3, 3, 7, 2, 1, 8};
      state[3] = (state[3] & 0xF0) | (mode <= 5 ? kModeWire[mode] : 3);
      if (p[2] & 0x80) state[3] |= 0x40; else state[3] &= 0xBF;
    }
    if (n > 4) {
      // P[4]: bit 6 -> unnamed S[2] bit 3; bit 7 -> unnamed S[2] bit 4;
      // bit 2 -> healthy S[3] 0x10; bit 3 -> antiMoldew S[3] 0x20.
      if (p[4] & 0x40) state[2] |= 0x08; else state[2] &= 0xF7;
      if (p[4] & 0x80) state[2] |= 0x10; else state[2] &= 0xEF;
      if (p[4] & 0x04) state[3] |= 0x10; else state[3] &= 0xEF;
      if (p[4] & 0x08) state[3] |= 0x20; else state[3] &= 0xDF;
    }
    if (n > 28) {
      // P[28] bit 7 -> silence S[3] 0x80.
      if (p[28] & 0x80) state[3] |= 0x80; else state[3] &= 0x7F;
    }
    if (n > 3) {
      // Target temperature nibble: inverse of the transmit encoding.
      state[4] = static_cast<uint8_t>((state[4] & 0xF0) | ((~p[3]) & 0x0F));
      // Fan: status code -> transmit wire (stock TBH 0x57586); codes 6/7
      // leave S[5] unchanged.
      static const uint8_t kFanWire[6] = {0, 2, 3, 5, 6, 7};
      const uint8_t fan = (p[3] >> 4) & 7;
      if (fan <= 5) state[5] = (state[5] & 0xF8) | kFanWire[fan];
    }
    if (n > 5) {
      // P[5]: bit 6 -> verticalSwitch S[5] 0x38; bit 5 -> horizontalSwitch
      // S[6] 0x08; bits 0..1 -> infrDirect S[7] low bits (raw copy, unlike
      // the report path which swaps 1/2); bit 7 -> temperatureType S[7] 0x80.
      if (p[5] & 0x40) state[5] |= 0x38; else state[5] &= 0xC7;
      if (p[5] & 0x20) state[6] |= 0x08; else state[6] &= 0xF7;
      state[7] = (state[7] & 0xFC) | (p[5] & 0x03);
      if (p[5] & 0x80) state[7] |= 0x80; else state[7] &= 0x7F;
    }
    if (n > 4) {
      // P[4]: bit 4 -> 3DAirSurply S[6] 0x01; bit 1 -> unit-0 fraction S[6]
      // 0x02 (raw copy, no unit selection); bit 0 -> unit-1 fraction 0x04;
      // bit 5 -> PTC S[6] 0x80.
      if (p[4] & 0x10) state[6] |= 0x01; else state[6] &= 0xFE;
      if (p[4] & 0x02) state[6] |= 0x02; else state[6] &= 0xFD;
      if (p[4] & 0x01) state[6] |= 0x04; else state[6] &= 0xFB;
      if (p[4] & 0x20) state[6] |= 0x80; else state[6] &= 0x7F;
    }
    if (n > 10) {
      // P[10] bit 7 -> S[8] 0x80.
      if (p[10] & 0x80) state[8] |= 0x80; else state[8] &= 0x7F;
    }
    if (n > 9) {
      // Raw echo window P[6..9] -> S[9..12].
      state[9] = p[6];
      state[10] = p[7];
      state[11] = p[8];
      state[12] = p[9];
    }
    if (n > 14) {
      // Sleep: P[14] low three bits -> S[14] low three bits.
      state[14] = (state[14] & 0xF8) | (p[14] & 0x07);
    }
    if (n > 15) {
      // Fixed ten-byte copy P[15..24] -> S[15..24] (0x5775C; the stock loop
      // stores its final byte before testing the loop bound); later blocks
      // overwrite some of these bytes when the payload carries them.
      for (size_t i = 0; i < 10; ++i) state[15 + i] = p[15 + i];
    }
    if (n > 42) {
      state[13] = p[42];  // fan extensions and generator low bits, raw
    }
    if (n > 50) {
      // constantTemp: inverse of the transmit encoding, high nibble.
      state[4] = static_cast<uint8_t>((state[4] & 0x0F) | (((~p[50]) & 0x0F) << 4));
    }
    if (n > 28) {
      // advancedECOMode: (P[28]>>4)&7 -> S[7] 0x1C.
      state[7] = static_cast<uint8_t>((state[7] & 0xE3) |
                                      (((p[28] >> 4) & 7) << 2));
    }
    if (n > 45 && p[45] != 0xFF) {
      // P[45] bit 5 -> softWind S[5] 0x40; P[27] bit 7 -> eightAddHot S[5]
      // 0x80; P[45] bit 2 -> filterBlockSwitch S[14] 0x08; bit 3 -> selfClean
      // S[14] 0x10; bit 4 -> unnamed S[14] 0x20; bit 0 -> lightSense S[14]
      // 0x80. P[45] == 0xFF is the unsupported sentinel.
      if (p[45] & 0x20) state[5] |= 0x40; else state[5] &= 0xBF;
      if (p[27] & 0x80) state[5] |= 0x80; else state[5] &= 0x7F;
      if (p[45] & 0x04) state[14] |= 0x08; else state[14] &= 0xF7;
      if (p[45] & 0x08) state[14] |= 0x10; else state[14] &= 0xEF;
      if (p[45] & 0x10) state[14] |= 0x20; else state[14] &= 0xDF;
      if (p[45] & 0x01) state[14] |= 0x80; else state[14] &= 0x7F;
    }
    if (n > 5) {
      // selfLearn: P[5] bit 2 -> S[14] 0x40.
      if (p[5] & 0x04) state[14] |= 0x40; else state[14] &= 0xBF;
    }
    if (n > 49) {
      // targetElectric BCD bytes, raw copy.
      state[25] = p[48];
      state[26] = p[49];
    }
    if (n > 46) {
      // verticalDirection byte: P[46] low six bits -> S[27].
      state[27] = p[46] & 0x3F;
    }
    if (n > 27) {
      // P[27] bit 3 -> highTemperatureWind S[27] 0x40; bit 4 -> feelTheWind
      // S[27] 0x80; bit 5 -> LRWideAngleFreeze S[28] 0x40.
      if (p[27] & 0x08) state[27] |= 0x40; else state[27] &= 0xBF;
      if (p[27] & 0x10) state[27] |= 0x80; else state[27] &= 0x7F;
      if (p[27] & 0x20) state[28] |= 0x40; else state[28] &= 0xBF;
    }
    if (n > 47) {
      // horizontalDirection byte: P[47] low six bits -> S[28].
      state[28] = p[47] & 0x3F;
    }
    if (n > 55) {
      // Right louver direction/switch: P[55] low seven bits -> S[29].
      state[29] = p[55] & 0x7F;
    }
    if (n > 56) {
      // Right fan/turbo/silence/AI group: P[56] -> S[30], raw.
      state[30] = p[56];
    }
    if (n > 58) {
      // P[58]: twoTemperature low two bits; polarityFreeze bit 2 -> 0x08;
      // surroundWind bit 3 -> 0x04; twoTemperatureSkew bits 4..5 -> 0x30.
      uint8_t v = static_cast<uint8_t>((state[31] & 0xFC) | (p[58] & 0x03));
      v = (p[58] & 0x04) ? static_cast<uint8_t>(v | 0x08)
                         : static_cast<uint8_t>(v & 0xF7);
      v = (p[58] & 0x08) ? static_cast<uint8_t>(v | 0x04)
                         : static_cast<uint8_t>(v & 0xFB);
      state[31] = static_cast<uint8_t>((v & 0xCF) | (p[58] & 0x30));
      // Below this point the generator extension byte decides S[31] bit 6;
      // stock leaves the bit untouched for payloads of 58 bytes or fewer.
      if (n > 62) {
        // Generator extension (stock 0x52B06/0x52AFC/0x52AD0): applies only
        // when P[62] bit 3 is set; S[31] bit 6 is cleared otherwise.
        if ((p[62] >> 3) & 1) {
          const uint8_t gen = p[62] & 7;
          state[13] = (state[13] & 0xFC) | (gen & 3);
          if (gen > 3) state[31] |= 0x40; else state[31] &= 0xBF;
        } else {
          state[31] &= 0xBF;
        }
      } else {
        state[31] &= 0xBF;
      }
    }

    // Stock tail (0x5793A-0x57964): a zero received mode nibble requests
    // desired state mode 1, target 26.0 and fan 0 through the normal setters.
    if (mode == 0) {
      set(0x05, 1.0);
      set(0x00, 26.0);
      set(0x02, 0.0);
    }
    return true;
  }

  // 38-byte command-03 state frame (stock encoder 0x47614, queued command 03):
  // BB 00 01 03 20 03 01, then S[2..31] at F[7..36], XOR of F[0..36] at F[37].
  std::array<uint8_t, 38> frame() const {
    std::array<uint8_t, 38> f = {};
    f[0] = 0xBB;
    f[1] = 0x00;
    f[2] = 0x01;
    f[3] = 0x03;
    f[4] = 0x20;
    f[5] = 0x03;
    f[6] = 0x01;
    for (size_t i = 2; i < 32; ++i) f[i + 5] = state[i];
    uint8_t x = 0;
    for (size_t i = 0; i < 37; ++i) x ^= f[i];
    f[37] = x;
    return f;
  }

 private:
  // Request flag read by the eightAddHot setter from status-cache byte 27
  // bit 6; refreshed by sync() when the payload carries byte 27.
  uint8_t eight_hot_request_ = 0;

  static bool integral(double v) { return v == std::floor(v); }

  static bool boolean(double v) { return v == 0.0 || v == 1.0; }

  static bool in_range(double v, double lo, double hi) {
    return std::isfinite(v) && v >= lo && v <= hi;
  }

  // Finite integral value in [lo, hi], for the small integer fields.
  static bool small_int(double v, int lo, int hi) {
    return std::isfinite(v) && integral(v) && v >= lo && v <= hi;
  }

  static void apply(uint8_t& byte, uint8_t mask, bool on) {
    byte = on ? static_cast<uint8_t>(byte | mask)
              : static_cast<uint8_t>(byte & static_cast<uint8_t>(~mask));
  }

  // Boolean setter: accepts exactly 0/1, set/clears one mask.
  static bool flag(uint8_t& byte, uint8_t mask, double value) {
    if (!boolean(value)) return false;
    apply(byte, mask, value != 0.0);
    return true;
  }

  // Stock fan enum (helper 0x7A490): 0->0, 2->2, 3->6, 4->3, 5->7, 6->5;
  // input 1 and everything else is rejected.
  static bool fan_enum(uint8_t& byte, double value) {
    static const uint8_t kWire[7] = {0, 0, 2, 6, 3, 7, 5};
    if (!integral(value) || value < 0.0 || value > 6.0 || value == 1.0) {
      return false;
    }
    byte = (byte & 0xF8) | kWire[static_cast<int>(value)];
    return true;
  }
};

}  // namespace tcl_ac

#endif  // FIRMWARE_AC_CODEC_H_

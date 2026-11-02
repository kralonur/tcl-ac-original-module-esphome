// Stock-faithful readback helpers for command 03/04 status payloads, taken
// from the stock report function 0x5557C (ordinary branch 0x55C50) of
// application V8-R82CT20-WFMV205.11.3.27, plus decode_settings() for the climate entity and
// scalar_state() for single-property readback. Pure C++11, no external
// dependencies.
//
// P is the status payload, starting at full-frame byte F[5]. Unlike stock,
// which relies on padded buffers for some reads, every read here is bounded
// by the payload length.

#ifndef FIRMWARE_AC_REPORT_H_
#define FIRMWARE_AC_REPORT_H_

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace tcl_ac {

// Packed 10-bit room temperature value: (P[12] << 2) | ((P[13] >> 2) & 3).
// p must hold at least 14 bytes.
inline int current_temperature_raw(const uint8_t* p) {
  return (int(p[12]) << 2) | ((p[13] >> 2) & 3);
}

// Stock currentTemperature (0x55FBA..0x55FDE): signed integer division
// trunc((raw - 200) / 10), whole degrees, before any float conversion.
inline int current_temperature(const uint8_t* p) {
  return (current_temperature_raw(p) - 200) / 10;
}

// Stock numeric error codes (0x56154..0x567E4), in stock report order.
// The codes are stock JSON numbers, not decoded diagnostic descriptions.
struct ErrorCodes {
  std::array<uint8_t, 64> code;
  size_t count;
};

inline ErrorCodes error_codes(const uint8_t* p, size_t n) {
  struct Source {
    uint8_t index, bit, code;
  };
  // Stock emission order. Code 1 has two source bits (P[11] bit 3, P[36]
  // bit 0) and is reported once; they are adjacent so a repeat is skipped.
  static const Source kSources[] = {
      {11, 0, 56}, {11, 1, 57}, {11, 4, 54}, {11, 5, 55}, {11, 6, 52},
      {11, 7, 53}, {11, 3, 1},  {36, 0, 1},  {36, 1, 2},  {36, 2, 3},
      {36, 3, 4},  {36, 4, 5},  {36, 5, 6},  {36, 6, 7},  {36, 7, 8},
      {37, 0, 9},  {37, 1, 10}, {37, 2, 11}, {37, 3, 12}, {37, 4, 13},
      {37, 5, 14}, {37, 6, 15}, {37, 7, 16}, {38, 0, 17}, {38, 1, 18},
      {38, 2, 19}, {38, 3, 20}, {38, 4, 21}, {38, 5, 22}, {38, 6, 23},
      {38, 7, 24}, {39, 4, 25}, {39, 6, 26}, {39, 7, 78}, {51, 0, 27},
      {51, 1, 28}, {51, 2, 29}, {51, 3, 30}, {51, 4, 31}, {51, 5, 32},
      {51, 6, 33}, {52, 0, 34}, {52, 1, 35}, {52, 2, 36}, {52, 3, 37},
      {52, 4, 38}, {52, 5, 39}, {52, 6, 40}, {53, 0, 41}, {53, 1, 42},
      {53, 2, 43}, {53, 3, 44}, {53, 4, 45}, {53, 5, 46}, {53, 6, 47},
      {54, 0, 48}, {54, 1, 49}, {54, 2, 50}, {54, 3, 51}, {54, 4, 64},
      {60, 6, 80}, {61, 6, 79}, {62, 4, 81}, {62, 5, 82},
  };
  ErrorCodes out{};
  for (const Source& s : kSources) {
    if (p == nullptr || s.index >= n) continue;
    // P[51..54] bit 7 suppresses that byte's whole error group.
    if (s.index >= 51 && s.index <= 54 && (p[s.index] & 0x80)) continue;
    if (!((p[s.index] >> s.bit) & 1)) continue;
    if (out.count && out.code[out.count - 1] == s.code) continue;
    out.code[out.count++] = s.code;
  }
  return out;
}

// Ordinary settings echoed in a status payload, at the stock report's receive
// positions; each was confirmed on a TAC-12CHSD by the 2026-10-28 live suite.
// Values use stock setter numbering, so they can be written back unchanged.
// Fields the payload is too short to carry read as zero.
struct Settings {
  static constexpr uint8_t kUnknown = 0xFF;  // Receive code with no stock value.
  bool power, screen, eco, turbo, silence, vertical_swing, horizontal_swing, fahrenheit;
  uint8_t work_mode;   // 0..4; community decoders name them auto, cool, dry, fan, heat
  uint8_t wind_speed;  // 0 (auto) or 2..6, slowest to fastest
  uint8_t sleep;       // 0..7
  float target_temperature;
  uint8_t vertical_move, vertical_fix, horizontal_move, horizontal_fix;
};

// Receive-code tables shared by decode_settings() and scalar_state(): wire
// work-mode nibble and primary received fan code to stock values. 0xFF
// marks codes with no stock value (workMode codes 0 and 6..15; fan codes
// 6/7), where stock would echo the raw code unchanged.
constexpr uint8_t kMode[16] = {0xFF, 1, 3, 2, 4, 0, 0xFF, 0xFF,
                               0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uint8_t kFan[8] = {0, 2, 4, 6, 3, 5, 0xFF, 0xFF};

inline Settings decode_settings(const uint8_t* p, size_t n) {
  Settings s{};
  if (p == nullptr || n < 6) {
    s.work_mode = s.wind_speed = Settings::kUnknown;
    return s;
  }
  s.power = (p[2] >> 4) & 1;
  s.screen = (p[2] >> 5) & 1;
  s.eco = (p[2] >> 6) & 1;
  s.turbo = (p[2] >> 7) & 1;
  s.work_mode = kMode[p[2] & 0x0F];
  s.wind_speed = kFan[(p[3] >> 4) & 7];
  s.horizontal_swing = (p[5] >> 5) & 1;
  s.vertical_swing = (p[5] >> 6) & 1;
  s.fahrenheit = (p[5] >> 7) & 1;
  const bool half = (p[4] >> (s.fahrenheit ? 0 : 1)) & 1;
  s.target_temperature = float((p[3] & 0x0F) + 16) + (half ? 0.5f : 0.0f);
  if (n > 14) s.sleep = p[14] & 7;
  if (n > 28) s.silence = (p[28] >> 7) & 1;
  if (n > 46) {
    s.vertical_move = (p[46] >> 3) & 7;
    s.vertical_fix = p[46] & 7;
  }
  if (n > 47) {
    s.horizontal_move = (p[47] >> 3) & 7;
    s.horizontal_fix = p[47] & 7;
  }
  return s;
}

// Direction readback shared by IDs 22/23/32: with the axis swing bit set the
// report shows the moving-position field (direction byte bits 3..5, where
// zero maps to 8); otherwise the parked-position low three bits plus 8.
inline int swing_direction(uint8_t direction, bool swinging) {
  if (swinging) {
    const int move = (direction >> 3) & 7;
    return move != 0 ? move : 8;
  }
  return (direction & 7) + 8;
}

// Stock scalar readback for one property ID, decoded from the received
// status payload exactly like the ordinary report branch at 0x55C50.
// Every value comes from the receive
// expressions - never from the outgoing setter state or its sync cache -
// and every read is bounded by the payload length, so a short payload cannot
// surface stale bytes. Returns NAN when the payload is null, when it does
// not carry the field, when the received code has no stock value, and for
// beepSwitch (0x14), which reports a cached desired value, and beepTempEn
// (0x41), which has no known received field; unimplemented IDs return NAN too.
inline float scalar_state(uint8_t id, const uint8_t* p, size_t n) {
  if (p == nullptr) return NAN;
  struct Bit {
    uint8_t id, index, bit;
  };
  // Single-bit receive expressions. Left/right aliases (0x02/0x0F/0x24
  // leftTurbo-style names, 0x03/0x31, 0x23) share one expression per ID;
  // alternate names are chosen elsewhere from cached split flags.
  static const Bit kBits[] = {
      {0x01, 2, 4},   // powerSwitch
      {0x03, 5, 5},   // horizontalSwitch / leftHorizontalSwitch
      {0x04, 5, 6},   // verticalSwitch
      {0x0B, 4, 5},   // PTC
      {0x0C, 2, 6},   // ECO
      {0x0D, 4, 2},   // healthy
      {0x0E, 4, 3},   // antiMoldew
      {0x0F, 2, 7},   // turbo / leftTurbo
      {0x10, 2, 5},   // screen
      {0x11, 4, 4},   // 3DAirSurply
      {0x1A, 45, 5},  // softWind
      {0x1C, 45, 0},  // lightSense
      {0x1D, 5, 2},   // selfLearn
      {0x1F, 45, 3},  // selfClean
      {0x24, 28, 7},  // silenceSwitch / leftSilenceSwitch
      {0x25, 45, 2},  // filterBlockSwitch
      {0x28, 27, 7},  // eightAddHot
      {0x2A, 27, 4},  // feelTheWind
      {0x2B, 27, 3},  // highTemperatureWind
      // LRWideAngleFreeze: stock reports the boolean P[27] bit 5, although
      // the setter writes a 0..7 subfield into S[28]; setter values 1..7
      // written that way are not visible in this report.
      {0x2C, 27, 5},
      {0x2D, 56, 5},  // aiFunction
      {0x2E, 56, 4},  // constTemperatureDehumidification
      {0x2F, 56, 3},  // temperatureHumidityControlling
      {0x30, 55, 6},  // rightHorizontalSwitch
      {0x31, 5, 5},   // leftHorizontalSwitch (same expression as 0x03)
      {0x36, 56, 7},  // rightSilenceSwitch
      {0x37, 56, 6},  // rightTurbo
      {0x39, 58, 3},  // surroundWind
      {0x3A, 58, 2},  // polarityFreeze
      {0x3D, 5, 7},   // temperatureType
      {0x40, 57, 5},  // infrPower
  };
  for (const Bit& b : kBits) {
    if (b.id == id) {
      if (b.index >= n) return NAN;
      return float((p[b.index] >> b.bit) & 1);
    }
  }
  switch (id) {
    // targetTemperature (0x55D2E/0x57BF4): (P[3]&0F)+16 plus a half degree
    // from P[4] bit 1 for unit 0, bit 0 for unit 1 (P[5] bit 7). The report
    // performs no Celsius/Fahrenheit conversion.
    case 0x00: {
      if (n < 6) return NAN;
      const bool unit = (p[5] >> 7) & 1;
      const float half = ((p[4] >> (unit ? 0 : 1)) & 1) ? 0.5f : 0.0f;
      return float((p[3] & 0x0F) + 16) + half;
    }
    // windSpeed / leftWindSpeed (0x55DFE/0x571E8): codes 0..5 report
    // {0,2,4,6,3,5}; codes 6/7 have no stock value (decode_settings table).
    case 0x02: {
      if (n < 4) return NAN;
      const uint8_t value = kFan[(p[3] >> 4) & 7];
      return value == Settings::kUnknown ? NAN : float(value);
    }
    // workMode (0x55C7C): decode_settings receive mapping; codes 0 and
    // 6..15 have no stock value there.
    case 0x05: {
      if (n < 3) return NAN;
      const uint8_t value = kMode[p[2] & 0x0F];
      return value == Settings::kUnknown ? NAN : float(value);
    }
    case 0x06:  // sleep (0x55FE8)
      return n > 14 ? float(p[14] & 7) : NAN;
    // infrDirect (0x55F10): P[5]&3 with the stock swap of 1/2; 0/3 pass
    // through unchanged.
    case 0x0A: {
      if (n < 6) return NAN;
      static const uint8_t kDirect[4] = {0, 2, 1, 3};
      return float(kDirect[p[5] & 3]);
    }
    case 0x17:  // verticalWind (0x568D0)
      return n > 42 ? float(p[42] >> 5) : NAN;
    case 0x18:  // horizontalWind (0x568B0)
      return n > 42 ? float((p[42] >> 2) & 7) : NAN;
    case 0x19:  // constantTemp (0x56E56)
      return n > 50 ? float((p[50] & 0x0F) + 16) : NAN;
    case 0x1B:  // advancedECOMode (0x56090)
      return n > 28 ? float((p[28] >> 4) & 7) : NAN;
    // targetElectric (0x56DB6): BCD nibbles of P[48] (hundredths/tenths)
    // and P[49] (units/tens), reported through the stock %.2f rounding.
    case 0x21: {
      if (n < 50) return NAN;
      const int cents = (p[49] >> 4) * 1000 + (p[49] & 0x0F) * 100 +
                        (p[48] >> 4) * 10 + (p[48] & 0x0F);
      return float(cents) / 100.0f;
    }
    // Direction readbacks use the swing rule per axis: 0x22 scans P[5] bit 6
    // for direction byte P[46] (0x56D4C), 0x23 scans P[5] bit 5 for P[47]
    // (0x56D7C), and 0x32 scans P[55] bit 6 for P[55] itself (0x56E78).
    case 0x22:
      return n > 46 ? float(swing_direction(p[46], (p[5] >> 6) & 1)) : NAN;
    case 0x23:
      return n > 47 ? float(swing_direction(p[47], (p[5] >> 5) & 1)) : NAN;
    case 0x32:
      return n > 55 ? float(swing_direction(p[55], (p[55] >> 6) & 1)) : NAN;
    // Generator mode (0x56880/0x57292): P[62]&7 when the payload carries
    // P[62] and its bit 3 marks the extension; otherwise P[42]&3.
    case 0x29: {
      if (n > 62 && (p[62] & 0x08)) return float(p[62] & 7);
      return n > 42 ? float(p[42] & 3) : NAN;
    }
    case 0x34:  // Unnamed right-horizontal subfield, P[55] low three bits.
      return n > 55 ? float(p[55] & 7) : NAN;
    case 0x35:  // Unnamed left-horizontal subfield, P[47] low three bits.
      return n > 47 ? float(p[47] & 7) : NAN;
    // rightWindSpeed (0x56EC0): 3->4, 5->6, 6->3, 7->5, others unchanged -
    // deliberately a different map from the primary fan.
    case 0x38: {
      if (n < 57) return NAN;
      static const uint8_t kRight[8] = {0, 1, 2, 4, 4, 6, 3, 5};
      return float(kRight[p[56] & 7]);
    }
    case 0x3B:  // twoTemperature (0x5701C)
      return n > 58 ? float(p[58] & 3) : NAN;
    case 0x3C:  // twoTemperatureSkew (0x57076)
      return n > 58 ? float((p[58] >> 4) & 3) : NAN;
    // beepSwitch (0x14) is cached desired state; beepTempEn (0x41) has no
    // known received expression. Neither is a received status measurement.
    default:
      return NAN;
  }
}

}  // namespace tcl_ac

#endif  // FIRMWARE_AC_REPORT_H_

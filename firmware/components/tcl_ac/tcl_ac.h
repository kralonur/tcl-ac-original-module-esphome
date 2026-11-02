#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart.h"
#include "ac_codec.h"
#include "ac_report.h"
#include "ac_stream.h"
#include "ac_timer_codec.h"

namespace esphome {
namespace wl6u_ac {

class TclAc : public Component, public uart::UARTDevice {
 public:
  void setup() override { started_ = millis(); }

  void dump_config() override {
    ESP_LOGCONFIG("tcl_ac", "Experimental stock-decoded controls; raw AC TX/RX enabled");
    ESP_LOGCONFIG("tcl_ac", "Controls require fresh validated status; TX is NOT an acceptance ACK");
  }

  void loop() override {
    const uint32_t now = millis();
    if (stream_.size() && now - last_byte_ > 150)
      stream_.reset();
    // Bound each loop's work. UART debugger logs even malformed/unknown RX.
    for (size_t budget = 0; budget < 256 && available(); ++budget) {
      uint8_t byte;
      if (!read_byte(&byte))
        break;
      last_byte_ = now;
      if (stream_.feed(byte))
        receive_();
    }
    flush_requests_();
    flush_timer_();
    if (timer_refresh_ && paced_()) {
      query("timers");
      timer_refresh_ = false;
    } else if (now - started_ >= 20000 && status_fresh() && !awaiting_status_ &&
               (!energy_polled_ || now - last_energy_poll_ >= 15000) &&
               (!sent_ || now - last_tx_ >= 2000)) {
      query("energy");
      last_energy_poll_ = now;
      energy_polled_ = true;
    } else if (now - started_ >= 15000 && (!sent_ || now - last_tx_ >= 2000)) {
      query("status");
    }
  }

  void set_property(const std::string &property, float value, bool force = false) {
    ESP_LOGI("tcl_ac", "REQUEST %s = %.3f (force=%d)",
             property.c_str(), double(value), int(force));
    uint8_t id;
    if (!property_id_(property, id)) {
      ESP_LOGW("tcl_ac", "Rejected unknown/non-control property: %s", property.c_str());
      return;
    }
    // Every scalar write transmits the whole cached state, so even a forced
    // write needs one synchronized status; it skips only freshness/readback.
    if ((force ? !synced_once_() : !control_ready_()) || !can_send_())
      return;
    auto candidate = codec_;
    if (!candidate.set(id, api_value_(id, value))) {
      ESP_LOGW("tcl_ac", "Rejected value for %s; no TX", property.c_str());
      return;
    }
    codec_ = candidate;
    send_(codec_.frame());
    awaiting_status_ = true;
  }

  // Home Assistant path. Validates now, then queues: queued changes are merged
  // and sent as one frame once a fresh status has been read back and pacing
  // allows. Changes still unsent after 5 s are dropped with a warning.
  void request_property(const std::string &property, float value) {
    uint8_t id;
    if (!property_id_(property, id)) {
      ESP_LOGW("tcl_ac", "Rejected unknown/non-control property: %s", property.c_str());
      return;
    }
    const double numeric = api_value_(id, value);
    auto probe = codec_;
    if (!probe.set(id, numeric)) {
      ESP_LOGW("tcl_ac", "Rejected value %.3f for %s; not queued", double(value), property.c_str());
      return;
    }
    ESP_LOGI("tcl_ac", "QUEUED %s = %.3f", property.c_str(), double(value));
    // Direction setters hold two independent subfields (values below 8 and
    // from 8 up), so each subfield keeps its own slot.
    const bool split = id == 0x22 || id == 0x23 || id == 0x32;
    for (size_t i = 0; i < pending_count_; ++i) {
      if (pending_[i].id == id && (!split || (pending_[i].value >= 8) == (numeric >= 8))) {
        pending_[i].value = numeric;
        return;
      }
    }
    if (pending_count_ == pending_.size()) {
      ESP_LOGW("tcl_ac", "Request queue full; %s not queued", property.c_str());
      return;
    }
    if (!pending_count_)
      pending_since_ = millis();
    pending_[pending_count_++] = {id, numeric};
  }

  // Louver presets for Home Assistant selects. Labels follow the community
  // TCL decoders (tclac, tcl_ac_esphome): a fixed position uses the direction
  // subfield set by values 9.., a swing range the one set by 1... The
  // TAC-12CHSD stores every value; the physical position behind each label is
  // not verified on it. Default and fixed positions turn the swing switch off,
  // swing ranges turn it on (as tcl_ac_esphome does).
  const char *vertical_louver() const { return louver_name_(true); }
  const char *horizontal_louver() const { return louver_name_(false); }
  void set_vertical_louver(const std::string &option) { set_louver_(true, option); }
  void set_horizontal_louver(const std::string &option) { set_louver_(false, option); }

  // Last synchronized AC->module status payload, for entities.
  bool has_status() const { return status_seen_; }
  bool status_fresh() const { return status_seen_ && millis() - last_status_ <= 10000; }
  const uint8_t *status() const { return status_.data(); }
  size_t status_length() const { return status_length_; }
  // Optional HA controls read received fields, never the outgoing command cache.
  // Missing extensions and stale status return NAN, not an invented zero.
  float property_state(const std::string &property) const {
    uint8_t id;
    if (!property_id_(property, id) || !status_fresh())
      return NAN;
    return ::tcl_ac::scalar_state(id, status_.data(), status_length_);
  }
  float energy_counter() const {
    return millis() - last_energy_ <= 60000 ? energy_counter_ : NAN;
  }
  float energy_power_raw() const {
    return millis() - last_energy_ <= 60000 ? energy_power_raw_ : NAN;
  }
  // Real-time power (W) from pure d(kWh)/dt math on 0A energy counter ticks
  // (0 W when the AC is off; no hardcoded watt estimates).
  float power_watts() const {
    if (!status_fresh() || status_length_ <= 33)
      return NAN;
    const bool power_on = (status_[2] >> 4) & 1;
    if (!power_on && status_[29] == 0 && status_[33] == 0)
      return 0.0f;
    const uint32_t valid_ms = energy_rate_valid_ms(energy_rate_w_);
    if (valid_ms != 0 && millis() - prev_energy_ms_ <= valid_ms)
      return std::round(energy_rate_w_);
    return NAN;
  }
  // Milliseconds a d(kWh)/dt rate stays valid without a new counter tick: at
  // rate R watts, 0.01 kWh (36 kJ) is due within 36000000/R ms. After two
  // missed intervals the load changed, so report unknown instead of the last
  // nonzero estimate; the hard cap keeps slow loads bounded.
  static uint32_t energy_rate_valid_ms(float rate_w) {
    if (!std::isfinite(rate_w) || rate_w <= 0.0f)
      return 0;
    const double due_ms = 36000000.0 / double(rate_w);
    return std::min(uint32_t(180000), static_cast<uint32_t>(2.0 * due_ms));
  }
  // beepSwitch is not in the status reply: this is the cached value sent with
  // every control frame (stock default on, reset at boot).
  bool beep() const { return codec_.state[2] & 0x20; }
  void set_on_status(std::function<void()> callback) { on_status_ = std::move(callback); }

  void query(const std::string &kind) {
    if (!can_send_())
      return;
    if (kind == "status") {
      const std::array<uint8_t, 31> frame = {{
          0xBB,0,1,4,0x19,0,0,0,8,0x0F,0,0,0,6,0,0,0,0,0,0,
          0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0xA6}};
      send_(frame);
    } else if (kind == "auxiliary") {
      send_(std::array<uint8_t, 8>{{0xBB,0,1,9,2,4,0,0xB5}});
    } else if (kind == "energy") {
      send_(std::array<uint8_t, 9>{{0xBB,0,1,0x0A,3,2,0,5,0xB4}});
    } else if (kind == "timers") {
      send_(std::array<uint8_t, 9>{{0xBB,0,1,0x0D,3,5,0,0,0xB1}});
    } else {
      ESP_LOGW("tcl_ac", "Rejected unknown query: %s", kind.c_str());
    }
  }

  // Timer edits send immediately when the 119-byte cache is fresh. With a
  // stale cache the edit is deferred, the timers table is queried automatically
  // and the edit is sent once the reply refreshes the cache. The newest request
  // replaces an older deferred one. Fresh AC status is still required.
  void special_timer(bool on, int display, int enable, int minutes) {
    DeferredTimer op;
    op.kind = DeferredTimer::Kind::special;
    op.on = on;
    op.display = display;
    op.enable = enable;
    op.minutes = minutes;
    send_timer_(std::move(op));
  }

  void weekly_timer(int bank, int slot, int display, int enable, int minutes,
                    const std::string &days, int power_or_mode, int fan,
                    float temperature, int fahrenheit) {
    DeferredTimer op;
    op.kind = DeferredTimer::Kind::weekly;
    op.bank = bank;
    op.slot = slot;
    op.display = display;
    op.enable = enable;
    op.minutes = minutes;
    op.days = days;
    op.power_or_mode = power_or_mode;
    op.fan = fan;
    op.temperature = temperature;
    op.fahrenheit = fahrenheit;
    send_timer_(std::move(op));
  }

  void clear_timer_bank(int bank, bool confirm) {
    if (!confirm)
      return;
    DeferredTimer op;
    op.kind = DeferredTimer::Kind::clear_bank;
    op.bank = bank;
    send_timer_(std::move(op));
  }

  void clear_energy(bool confirm) {
    if (!confirm || !control_ready_() || !can_send_())
      return;
    // Explicit stock clearCurrentStatus event; never part of periodic polling.
    send_(std::array<uint8_t, 9>{{0xBB,0,1,0x0A,3,2,0,0x0D,0xBC}});
  }

  void set_clock(int year, int month, int day, int hour, int minute, int second) {
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int days[] = {31,28 + int(leap),31,30,31,30,31,31,30,31,30,31};
    if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 ||
        day > days[month - 1] || hour < 0 || hour > 23 || minute < 0 ||
        minute > 59 || second < 0 || second > 59) {
      ESP_LOGW("tcl_ac", "Rejected invalid local calendar time; no TX");
      return;
    }
    if (control_ready_() && can_send_())
      send_(::tcl_ac::date_frame(year, month, day, hour, minute, second));
  }

 protected:
  ::tcl_ac::Codec codec_;
  ::tcl_ac::Timers timers_;
  ::tcl_ac::Stream stream_;
  uint32_t started_{0}, last_tx_{0}, last_byte_{0}, last_status_{0}, last_timer_{0};
  uint32_t last_energy_poll_{0};
  bool sent_{false}, status_seen_{false}, awaiting_status_{true}, timer_refresh_{false};
  bool energy_polled_{false};
  struct Change {
    uint8_t id;
    double value;
  };
  std::array<Change, 16> pending_{};
  size_t pending_count_{0};
  uint32_t pending_since_{0};
  std::array<uint8_t, 249> status_{};
  size_t status_length_{0};
  uint32_t last_energy_{0}, prev_energy_ms_{0};
  float energy_counter_{NAN}, energy_power_raw_{NAN};
  double prev_energy_kwh_{NAN};
  float energy_rate_w_{NAN};
  std::function<void()> on_status_;

  // One deferred timer edit (see the timer actions above).
  struct DeferredTimer {
    enum class Kind : uint8_t { none, special, weekly, clear_bank };
    Kind kind = Kind::none;
    bool on = false;
    int display = 0, enable = 0, minutes = 0;
    int bank = 0, slot = 0, power_or_mode = 0, fan = 0, fahrenheit = 0;
    double temperature = 0;
    std::string days;
  };
  DeferredTimer deferred_timer_{};
  uint32_t deferred_timer_since_{0};

  // Native API numbers are float32: restore intended targetElectric
  // hundredths before the stock typed-double setter truncates (e.g. 12.36f
  // otherwise -> 12.35).
  static double api_value_(uint8_t id, float value) {
    double numeric = value;
    if (id == 0x21 && std::isfinite(value) && value >= 0 && value <= 99.99f) {
      const double cents = std::round(double(value) * 100.0);
      numeric = cents / 100.0;
      if (std::trunc(numeric * 100.0) < cents)
        numeric = std::nextafter(numeric, 100.0);
    }
    return numeric;
  }

  void flush_requests_() {
    if (!pending_count_)
      return;
    if (millis() - pending_since_ > 5000) {
      ESP_LOGW("tcl_ac", "No fresh AC status for 5 s; %u queued change(s) dropped, no TX",
               unsigned(pending_count_));
      pending_count_ = 0;
      return;
    }
    if (!status_seen_ || awaiting_status_ || millis() - last_status_ > 10000 || !paced_())
      return;
    auto candidate = codec_;
    for (size_t i = 0; i < pending_count_; ++i) {
      if (!candidate.set(pending_[i].id, pending_[i].value)) {
        ESP_LOGW("tcl_ac", "Queued change rejected; %u change(s) dropped, no TX",
                 unsigned(pending_count_));
        pending_count_ = 0;
        return;
      }
    }
    ESP_LOGI("tcl_ac", "Sending %u queued change(s)", unsigned(pending_count_));
    pending_count_ = 0;
    codec_ = candidate;
    send_(codec_.frame());
    awaiting_status_ = true;
  }

  struct Louver {
    const char *const *names;
    size_t count;
    const char *direction, *swing;
  };
  static constexpr size_t kFixedPositions = 5;
  static Louver louver_(bool vertical) {
    static const char *const kVertical[] = {"Default", "Top", "Upper", "Middle", "Lower",
                                            "Bottom", "Swing full", "Swing upper",
                                            "Swing lower"};
    static const char *const kHorizontal[] = {"Default", "Left", "Middle left", "Middle",
                                              "Middle right", "Right", "Swing full",
                                              "Swing left", "Swing middle", "Swing right"};
    if (vertical)
      return {kVertical, 9, "verticalDirection", "verticalSwitch"};
    return {kHorizontal, 10, "horizontalDirection", "horizontalSwitch"};
  }

  const char *louver_name_(bool vertical) const {
    if (!status_seen_)
      return nullptr;
    const auto s = ::tcl_ac::decode_settings(status_.data(), status_length_);
    const unsigned move = vertical ? s.vertical_move : s.horizontal_move;
    const unsigned fix = vertical ? s.vertical_fix : s.horizontal_fix;
    const Louver louver = louver_(vertical);
    if (!move && fix > kFixedPositions)
      return nullptr;
    const size_t index = move ? kFixedPositions + move : fix;
    return index < louver.count ? louver.names[index] : nullptr;
  }

  void set_louver_(bool vertical, const std::string &option) {
    const Louver louver = louver_(vertical);
    size_t index = 0;
    while (index < louver.count && option != louver.names[index])
      ++index;
    if (index == louver.count) {
      ESP_LOGW("tcl_ac", "Rejected louver option: %s", option.c_str());
      return;
    }
    const std::string direction = louver.direction, swing = louver.swing;
    if (index <= kFixedPositions) {  // Default (0) or a fixed position: no swing.
      request_property(swing, 0);
      request_property(direction, 0);
      request_property(direction, float(8 + index));
    } else {
      request_property(swing, 1);
      request_property(direction, 8);
      request_property(direction, float(index - kFixedPositions));
    }
  }

  bool paced_() const { return !sent_ || millis() - last_tx_ >= 500; }

  bool can_send_() const {
    if (!paced_()) {
      ESP_LOGW("tcl_ac", "Rate limited: wait 500 ms between frames; request NOT queued");
      return false;
    }
    return true;
  }

  bool synced_once_() const {
    if (!status_seen_) {
      ESP_LOGW("tcl_ac", "No AC status since boot; forced control still needs one, no TX");
      return false;
    }
    return true;
  }

  bool control_ready_() const {
    if (!status_seen_ || awaiting_status_ || millis() - last_status_ > 10000) {
      ESP_LOGW("tcl_ac", "No fresh synchronized AC status; control rejected, no TX");
      return false;
    }
    return true;
  }

  bool timer_cache_fresh_() const {
    return timers_.ready && millis() - last_timer_ <= 30000;
  }

  template<size_t N> void send_(const std::array<uint8_t, N> &frame) {
    ESP_LOGI("tcl_ac", "TX command=%02X bytes=%u; AC acceptance unconfirmed",
             frame[3], unsigned(N));
    write_array(frame.data(), N);
    last_tx_ = millis();
    sent_ = true;
  }

  void send_timers_() {
    send_(timers_.frame());
    timers_.ready = false;
    timer_refresh_ = true;
  }

  static bool apply_timer_(::tcl_ac::Timers &timers, const DeferredTimer &op) {
    switch (op.kind) {
      case DeferredTimer::Kind::special:
        return timers.special(op.on, op.display, op.enable, op.minutes);
      case DeferredTimer::Kind::weekly:
        return timers.weekly(op.bank, op.slot, op.display, op.enable, op.minutes,
                             op.days, op.power_or_mode, op.fan, op.temperature,
                             op.fahrenheit);
      case DeferredTimer::Kind::clear_bank:
        return timers.clear_bank(op.bank);
      default:
        return false;
    }
  }

  void send_timer_(DeferredTimer op) {
    if (!control_ready_())
      return;
    if (!timer_cache_fresh_()) {
      defer_timer_(std::move(op));
      return;
    }
    if (!can_send_())
      return;
    auto candidate = timers_;
    if (!apply_timer_(candidate, op)) {
      ESP_LOGW("tcl_ac", "Rejected timer fields; no TX");
      return;
    }
    timers_ = candidate;
    send_timers_();
  }

  void defer_timer_(DeferredTimer op) {
    // Validate against a throwaway ready cache so rejected fields never queue.
    ::tcl_ac::Timers validator;
    validator.ready = true;
    if (!apply_timer_(validator, op)) {
      ESP_LOGW("tcl_ac", "Rejected timer fields; not queued");
      return;
    }
    ESP_LOGI("tcl_ac", "Timer cache stale; querying timers first, edit deferred");
    deferred_timer_ = std::move(op);
    deferred_timer_since_ = millis();
    timer_refresh_ = true;
  }

  // Applies the deferred edit once the timers query has refreshed the cache;
  // dropped with a warning after 5 s without a timer reply.
  void flush_timer_() {
    if (deferred_timer_.kind == DeferredTimer::Kind::none)
      return;
    if (millis() - deferred_timer_since_ > 5000) {
      ESP_LOGW("tcl_ac", "No timer reply for 5 s; deferred timer edit dropped, no TX");
      deferred_timer_.kind = DeferredTimer::Kind::none;
      return;
    }
    if (!timer_cache_fresh_() || !paced_())
      return;
    const DeferredTimer op = deferred_timer_;
    deferred_timer_.kind = DeferredTimer::Kind::none;
    auto candidate = timers_;
    if (!apply_timer_(candidate, op)) {
      ESP_LOGW("tcl_ac", "Deferred timer edit rejected; no TX");
      return;
    }
    timers_ = candidate;
    send_timers_();
  }

  void log_decoded_status_(const uint8_t *p, size_t len) const {
    const auto s = ::tcl_ac::decode_settings(p, len);  // 255 = unknown code
    // Stock numeric error codes, comma-separated; "none" when empty.
    const auto errors = ::tcl_ac::error_codes(p, len);
    char error_text[4 * 64 + 1] = "none";
    for (size_t i = 0, used = 0; i < errors.count; ++i)
      used += std::snprintf(error_text + used, sizeof(error_text) - used, "%s%u",
                            i ? "," : "", unsigned(errors.code[i]));
    ESP_LOGI("tcl_ac",
             "RX decoded status: powerSwitch=%u workMode=%u targetTemperature=%.1f "
             "currentTemperature=%d currentTemperatureRaw=%d windSpeed=%u "
             "horizontalSwitch=%u verticalSwitch=%u ECO=%u turbo=%u screen=%u sleep=%u "
             "healthy=%u antiMoldew=%u PTC=%u 3DAirSurply=%u temperatureType=%u "
             "errorCode=%s",
             unsigned(s.power), unsigned(s.work_mode), double(s.target_temperature),
             ::tcl_ac::current_temperature(p), ::tcl_ac::current_temperature_raw(p),
             unsigned(s.wind_speed), unsigned(s.horizontal_swing), unsigned(s.vertical_swing),
             unsigned(s.eco), unsigned(s.turbo), unsigned(s.screen), unsigned(s.sleep),
             unsigned((p[4] >> 2) & 1), unsigned((p[4] >> 3) & 1),
             unsigned((p[4] >> 5) & 1), unsigned((p[4] >> 4) & 1), unsigned(s.fahrenheit),
             error_text);
    if (len > 50) {
      const double elec = double(p[48] & 0x0F) * 0.01 + double(p[48] >> 4) * 0.1 +
                          double(p[49] & 0x0F) + double(p[49] >> 4) * 10.0;
      ESP_LOGI("tcl_ac",
               "RX decoded extended: eightAddHot=%u silenceSwitch=%u softWind=%u "
               "selfClean=%u lightSense=%u filterBlockSwitch=%u constantTemp=%u "
               "targetElectric=%.2f",
               unsigned((p[27] >> 7) & 1), unsigned((p[28] >> 7) & 1),
               unsigned((p[45] >> 5) & 1), unsigned((p[45] >> 3) & 1),
               unsigned(p[45] & 1), unsigned((p[45] >> 2) & 1),
               unsigned((p[50] & 0x0F) + 16), elec);
      ESP_LOGI("tcl_ac",
               "RX decoded hardware: voltage=%uV currentRaw=%u compressorHz=%u "
               "indoorFan=%u outdoorFan=%u indoorCoilRaw=%u outdoorTempRaw=%u "
               "outdoorCoilRaw=%u dischargeRaw=%u",
               unsigned(p[40]), unsigned(p[41]), unsigned(p[33]),
               unsigned(p[29]), unsigned(p[34]), unsigned(p[25]),
               unsigned(p[30]), unsigned(p[31]), unsigned(p[32]));
    }
  }

  void log_decoded_energy_(const uint8_t *p, size_t len) {
    energy_counter_ = energy_power_raw_ = NAN;
    if (len < 17 || !(p[2] & 0x08))
      return;
    auto lo = [](uint8_t b) { return double(b & 0x0F); };
    auto hi = [](uint8_t b) { return double(b >> 4); };
    const double kwh = lo(p[15]) * 1e-6 + hi(p[15]) * 1e-5 + lo(p[16]) * 1e-4 +
                       hi(p[16]) * 1e-3 + lo(p[3]) * 1e-2 + hi(p[3]) * 1e-1 +
                       lo(p[4]) + hi(p[4]) * 10.0 + lo(p[5]) * 100.0 +
                       hi(p[5]) * 1000.0 + lo(p[6]) * 10000.0 + hi(p[6]) * 100000.0;
    bool valid_bcd = true;
    for (size_t index : {size_t(3), size_t(4), size_t(5), size_t(6), size_t(15), size_t(16)})
      valid_bcd &= (p[index] & 0x0F) <= 9 && (p[index] >> 4) <= 9;
    if (valid_bcd) {
      const uint32_t now = millis();
      const bool active = status_seen_ && status_length_ > 33 &&
                          (((status_[2] >> 4) & 1) || status_[29] > 0 || status_[33] > 0);
      if (active) {
        if (std::isfinite(prev_energy_kwh_) && kwh > prev_energy_kwh_ && now > prev_energy_ms_) {
          const double dt_s = double(now - prev_energy_ms_) / 1000.0;
          if (dt_s >= 5.0 && dt_s <= 600.0)
            energy_rate_w_ = float((kwh - prev_energy_kwh_) * 3600000.0 / dt_s);
          prev_energy_kwh_ = kwh;
          prev_energy_ms_ = now;
        } else if (!std::isfinite(prev_energy_kwh_) || kwh < prev_energy_kwh_) {
          prev_energy_kwh_ = kwh;
          prev_energy_ms_ = now;
        }
      }
      last_energy_ = now;
      energy_counter_ = float(kwh);
      energy_power_raw_ = len > 17 ? float(p[17]) : NAN;
    }
    ESP_LOGI("tcl_ac", "RX decoded energy: electricity=%.6f powerRaw=%u",
             kwh, len > 17 ? unsigned(p[17]) : 0u);
  }

  void log_decoded_timers_(const uint8_t *p) const {
    ESP_LOGI("tcl_ac",
             "RX decoded specialTimer: on=(display=%u enable=%u time=%u) "
             "off=(display=%u enable=%u time=%u)",
             unsigned((p[2] >> 3) & 1), unsigned((p[2] >> 2) & 1),
             unsigned(p[3]) * 60u + unsigned(p[4]),
             unsigned((p[2] >> 1) & 1), unsigned(p[2] & 1),
             unsigned(p[5]) * 60u + unsigned(p[6]));
  }

  void receive_() {
    const auto &f = stream_.frame;
    uint8_t checksum = 0;
    for (size_t i = 0; i < stream_.size(); ++i)
      checksum ^= f[i];
    if (checksum) {
      ESP_LOGW("tcl_ac", "RX invalid checksum; raw bytes retained in AC <<< log");
      return;
    }
    ESP_LOGI("tcl_ac", "RX checksum-valid command=%02X payload=%u; not an acceptance ACK",
             f[3], unsigned(f[4]));
    // AC replies use header BB 01 00; this module transmits BB 00 01. Any
    // module->AC frame here (a loopback echo of any earlier TX) is ignored.
    if (f[1] != 1 || f[2] != 0) {
      ESP_LOGW("tcl_ac", "RX header %02X %02X is not AC->module (01 00); ignored", f[1], f[2]);
      return;
    }
    if ((f[3] == 3 || f[3] == 4) && codec_.sync(f.data() + 5, f[4])) {
      last_status_ = millis();
      status_seen_ = true;
      awaiting_status_ = false;
      status_length_ = f[4];
      std::copy(f.begin() + 5, f.begin() + 5 + f[4], status_.begin());
      if (status_length_ > 33 && status_[33] == 0) {
        prev_energy_kwh_ = NAN;
        energy_rate_w_ = NAN;
      }
      log_decoded_status_(f.data() + 5, f[4]);
      ESP_LOGI("tcl_ac", "RX status synchronized; controls available for 10 s");
      if (on_status_)
        on_status_();
    } else if (f[3] == 0x0A) {
      log_decoded_energy_(f.data() + 5, f[4]);
    } else if (f[3] == 0x0D && timers_.sync(f.data() + 5, f[4])) {
      last_timer_ = millis();
      log_decoded_timers_(f.data() + 5);
      ESP_LOGI("tcl_ac", "RX timer cache synchronized; timer editing available for 30 s");
    }
  }

  static bool property_id_(const std::string &name, uint8_t &id) {
    struct Entry { const char *name; uint8_t id; };
    static const Entry properties[] = {
        {"powerSwitch",1}, {"targetTemperature",0}, {"temperatureType",0x3D},
        {"windSpeed",2}, {"leftWindSpeed",2}, {"horizontalSwitch",3},
        {"verticalSwitch",4}, {"horizontalDirection",0x23}, {"verticalDirection",0x22},
        {"horizontalWind",0x18}, {"verticalWind",0x17}, {"workMode",5},
        {"ECO",0x0C}, {"advancedECOMode",0x1B}, {"healthy",0x0D},
        {"antiMoldew",0x0E}, {"turbo",0x0F}, {"leftTurbo",0x0F},
        {"softWind",0x1A}, {"silenceSwitch",0x24}, {"leftSilenceSwitch",0x24},
        {"screen",0x10}, {"lightSense",0x1C}, {"3DAirSurply",0x11},
        {"PTC",0x0B}, {"sleep",6}, {"infrPower",0x40}, {"infrDirect",0x0A},
        {"beepSwitch",0x14}, {"filterBlockSwitch",0x25}, {"selfLearn",0x1D},
        {"targetElectric",0x21}, {"constantTemp",0x19}, {"selfClean",0x1F},
        {"eightAddHot",0x28}, {"generatorMode",0x29}, {"feelTheWind",0x2A},
        {"highTemperatureWind",0x2B}, {"LRWideAngleFreeze",0x2C},
        {"aiFunction",0x2D}, {"constTemperatureDehumidification",0x2E},
        {"temperatureHumidityControlling",0x2F}, {"rightHorizontalSwitch",0x30},
        {"leftHorizontalSwitch",0x31}, {"rightHorizontalDirection",0x32},
        {"leftHorizontalDirection",0x23}, {"rightSilenceSwitch",0x36},
        {"rightTurbo",0x37}, {"rightWindSpeed",0x38}, {"twoTemperature",0x3B},
        {"twoTemperatureSkew",0x3C}, {"polarityFreeze",0x3A},
        {"surroundWind",0x39}, {"beepTempEn",0x41},
        // Descriptive project names for the two unnamed decoded louver setters.
        {"rightHorizontalSubfield",0x34}, {"leftHorizontalSubfield",0x35},
    };
    for (const auto &entry : properties) {
      if (name == entry.name) {
        id = entry.id;
        return true;
      }
    }
    return false;
  }
};

}  // namespace wl6u_ac
}  // namespace esphome

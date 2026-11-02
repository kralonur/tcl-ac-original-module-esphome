#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace tcl_ac {

// Framing only. Consumers must validate XOR, direction, command and payload length.
class Stream {
 public:
  std::array<uint8_t, 255> frame{};

  void reset() { count_ = expected_ = 0; }
  size_t size() const { return count_; }

  bool feed(uint8_t byte) {
    if (expected_ && count_ == expected_)
      reset();
    if (!count_) {
      if (byte == 0xBB)
        frame[count_++] = byte;
      return false;
    }
    if ((count_ == 1 || count_ == 2) && byte > 1) {
      reset();
      return feed(byte);
    }
    if (count_ == 4 && byte > 249) {
      reset();
      return false;
    }
    frame[count_++] = byte;
    if (count_ == 5)
      expected_ = size_t(byte) + 6;
    return expected_ && count_ == expected_;
  }

 private:
  size_t count_{0};
  size_t expected_{0};
};

}  // namespace tcl_ac

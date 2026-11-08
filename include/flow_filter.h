#pragma once

// Pure C++17 XON/XOFF receive filter. Must not include Arduino headers.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace flow_filter {

constexpr std::uint8_t kXon = 0x11;
constexpr std::uint8_t kXoff = 0x13;

class Filter {
 public:
  explicit Filter(bool enabled = true)
      : enabled_(enabled), paused_(false) {}

  // When disabled every byte is passed through unchanged and paused is false.
  // When enabled, 0x11/0x13 are stripped and 0x13 sets / 0x11 clears `paused`.
  // `out` must have capacity for at least `length` bytes; the return value is
  // the number of bytes written.
  std::size_t process(const std::uint8_t* data, std::size_t length,
                      std::uint8_t* out, std::size_t outCapacity);

  bool enabled() const { return enabled_.load(std::memory_order_acquire); }
  void setEnabled(bool enabled);

  bool paused() const { return paused_.load(std::memory_order_acquire); }

 private:
  std::atomic<bool> enabled_;
  std::atomic<bool> paused_;
};

}  // namespace flow_filter

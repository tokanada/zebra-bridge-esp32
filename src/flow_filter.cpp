#include "flow_filter.h"

namespace flow_filter {

std::size_t Filter::process(const std::uint8_t* data, std::size_t length,
                            std::uint8_t* out, std::size_t outCapacity) {
  std::size_t written = 0;
  for (std::size_t i = 0; i < length; ++i) {
    const std::uint8_t byte = data[i];
    if (enabled_.load(std::memory_order_acquire)) {
      if (byte == kXoff) {
        paused_.store(true, std::memory_order_release);
        continue;
      }
      if (byte == kXon) {
        paused_.store(false, std::memory_order_release);
        continue;
      }
    }
    if (written >= outCapacity) {
      break;
    }
    out[written++] = byte;
  }
  return written;
}

void Filter::setEnabled(bool enabled) {
  enabled_.store(enabled, std::memory_order_release);
  if (!enabled) {
    paused_.store(false, std::memory_order_release);
  }
}

}  // namespace flow_filter

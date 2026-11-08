#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <cstring>
#include <functional>

#include "config_store.h"
#include "flow_filter.h"

// Owns the printer UART, the 32 KiB stream buffer and the TX/RX tasks.
// BLE writes enqueue into the buffer; bytes from the printer's TX line are
// filtered and handed to the RX callback (which notifies the BLE client).
class PrinterLink {
 public:
  using RxCallback = std::function<void(const std::uint8_t*, std::size_t)>;

  static constexpr std::size_t kBufferSize = 32 * 1024;
  static constexpr std::size_t kTxChunkSize = 64;
  static constexpr std::uint32_t kWriteBlockTimeoutMs = 15000;

  bool begin(const Config& config);
  void setRxCallback(RxCallback callback);

  // Applies a new flow mode to the active UART without changing the pin map.
  void setFlowMode(console::FlowMode flow);

  // Accounts for a payload received from the BLE write characteristic.
  void noteBleWrite(std::size_t bytes);

  // Blocks up to `timeoutMs` for buffer space; returns bytes accepted.
  std::size_t enqueue(const std::uint8_t* data, std::size_t length,
                      std::uint32_t timeoutMs);
  std::size_t enqueue(const char* text, std::uint32_t timeoutMs);

  bool isPaused() const { return filter_.paused(); }

  // Returns true once after the TX task has forwarded bytes to the printer.
  bool takeActivity();

  std::uint32_t bytesFromBle() const { return bytesFromBle_; }
  std::uint32_t bytesToPrinter() const { return bytesToPrinter_; }
  std::uint32_t bytesDropped() const { return bytesDropped_; }
  std::uint32_t writeCount() const { return writeCount_; }
  std::size_t queueFill() const;
  std::size_t queueCapacity() const { return kBufferSize; }

 private:
  static void txTaskEntry(void* arg);
  static void rxTaskEntry(void* arg);

  void txTask();
  void rxTask();

  HardwareSerial* uart_ = nullptr;
  StreamBufferHandle_t stream_ = nullptr;
  flow_filter::Filter filter_{true};
  RxCallback rxCallback_;

  int rxPin_ = -1;
  int txPin_ = -1;
  int ctsPin_ = -1;

  volatile bool activity_ = false;
  volatile std::uint32_t bytesFromBle_ = 0;
  volatile std::uint32_t bytesToPrinter_ = 0;
  volatile std::uint32_t bytesDropped_ = 0;
  volatile std::uint32_t writeCount_ = 0;
};

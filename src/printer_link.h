#pragma once

#ifndef ZB_PRINTER_USB
#define ZB_PRINTER_USB 0
#endif

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <cstring>
#include <functional>

#include "config_store.h"
#include "flow_filter.h"

#if ZB_PRINTER_USB
#include <freertos/semphr.h>

#include "usb/usb_host.h"
#endif

// Owns the printer link, the 32 KiB stream buffer and the TX/RX tasks.
// BLE writes enqueue into the buffer; bytes from the printer are filtered
// (UART backend) or forwarded unchanged (USB backend) to the RX callback,
// which notifies the BLE client.
//
// Two backends share this class:
//   - UART backend (: !ZB_PRINTER_USB) lives in printer_link.cpp.
//   - USB host backend (: ZB_PRINTER_USB) lives in usb_printer_link.cpp.
// Only one of the two translation units has a body for ZB_PRINTER_USB builds.
class PrinterLink {
 public:
  using RxCallback = std::function<void(const std::uint8_t*, std::size_t)>;

  static constexpr std::size_t kBufferSize = 32 * 1024;
  static constexpr std::size_t kTxChunkSize = 64;
  static constexpr std::uint32_t kWriteBlockTimeoutMs = 15000;

  bool begin(const Config& config);
  void setRxCallback(RxCallback callback);

  // Applies a new flow mode to the active link without changing the pin map.
  // The USB backend stores the mode but ignores it.
  void setFlowMode(console::FlowMode flow);

  // Accounts for a payload received from the BLE write characteristic.
  void noteBleWrite(std::size_t bytes);

  // Blocks up to `timeoutMs` for buffer space; returns bytes accepted.
  std::size_t enqueue(const std::uint8_t* data, std::size_t length,
                      std::uint32_t timeoutMs);
  std::size_t enqueue(const char* text, std::uint32_t timeoutMs);

  bool isPaused() const;

  // Returns true once after the TX task has forwarded bytes to the printer.
  bool takeActivity();

  std::uint32_t bytesFromBle() const { return bytesFromBle_; }
  std::uint32_t bytesToPrinter() const { return bytesToPrinter_; }
  std::uint32_t bytesDropped() const { return bytesDropped_; }
  std::uint32_t writeCount() const { return writeCount_; }
  std::size_t queueFill() const;
  std::size_t queueCapacity() const { return kBufferSize; }

  // True on the ESP32-S3 USB-host build, false on the UART builds.
  bool isUsb() const;
  // UART: always true. USB: a printer-class device is open and claimed.
  bool printerAttached() const;
  std::uint16_t usbVid() const;
  std::uint16_t usbPid() const;
  // USB: IEEE 1284 device ID string (without the 2-byte length prefix), or ""
  // when unknown. UART: "".
  const char* deviceId() const;

 private:
#if ZB_PRINTER_USB
  static void usbLibTaskEntry(void* arg);
  static void clientTaskEntry(void* arg);
  static void txTaskEntry(void* arg);
  static void clientEventCallback(const usb_host_client_event_msg_t* event,
                                  void* arg);
  static void txDoneCallback(usb_transfer_t* transfer);
  static void rxDoneCallback(usb_transfer_t* transfer);
  static void controlDoneCallback(usb_transfer_t* transfer);

  void usbLibTask();
  void clientTask();
  void txTask();
  void processNewDevice(std::uint8_t address);
  void processDeviceGone();
  void fetchDeviceId();
  void startRx();

  TaskHandle_t usbLibTaskHandle_ = nullptr;
  TaskHandle_t clientTaskHandle_ = nullptr;
  TaskHandle_t txTaskHandle_ = nullptr;
  usb_host_client_handle_t client_ = nullptr;
  usb_device_handle_t device_ = nullptr;

  SemaphoreHandle_t installDone_ = nullptr;
  SemaphoreHandle_t txDone_ = nullptr;
  SemaphoreHandle_t controlDone_ = nullptr;

  volatile bool installOk_ = false;
  volatile bool newDevPending_ = false;
  volatile std::uint8_t newDevAddress_ = 0;
  volatile bool devGonePending_ = false;
  volatile bool attached_ = false;
  volatile bool txInFlight_ = false;
  volatile bool rxInFlight_ = false;
  volatile bool controlInFlight_ = false;

  bool interfaceClaimed_ = false;
  std::uint8_t interfaceNumber_ = 0;
  std::uint8_t interfaceAlt_ = 0;
  std::uint8_t outEndpoint_ = 0;
  std::uint8_t inEndpoint_ = 0;
  std::uint16_t outMps_ = 64;
  std::uint16_t inMps_ = 64;

  usb_transfer_t* txTransfer_ = nullptr;
  usb_transfer_t* rxTransfer_ = nullptr;
  usb_transfer_t* controlTransfer_ = nullptr;

  char deviceId_[128] = {0};
  std::uint16_t vid_ = 0;
  std::uint16_t pid_ = 0;
  console::FlowMode flowMode_ = console::FlowMode::XonXoff;
  console::LogLevel logLevel_ = console::LogLevel::Info;
#else
  static void txTaskEntry(void* arg);
  static void rxTaskEntry(void* arg);

  void txTask();
  void rxTask();

  HardwareSerial* uart_ = nullptr;

  int rxPin_ = -1;
  int txPin_ = -1;
  int ctsPin_ = -1;
#endif

  StreamBufferHandle_t stream_ = nullptr;
  flow_filter::Filter filter_{true};
  RxCallback rxCallback_;

  volatile bool activity_ = false;
  volatile std::uint32_t bytesFromBle_ = 0;
  volatile std::uint32_t bytesToPrinter_ = 0;
  volatile std::uint32_t bytesDropped_ = 0;
  volatile std::uint32_t writeCount_ = 0;
};

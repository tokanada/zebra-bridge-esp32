#include "printer_link.h"

#if ZB_PRINTER_USB

#include <Arduino.h>
#include <esp_err.h>
#include <esp_intr_alloc.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"

namespace {

constexpr std::size_t kControlBufferSize =
    sizeof(usb_setup_packet_t) + 248;  // 256 bytes, the host control limit
constexpr std::size_t kTxTransferSize = 4096;
constexpr std::uint32_t kUsbInstallTimeoutMs = 2000;
constexpr std::uint32_t kControlTimeoutMs = 1000;
constexpr std::uint32_t kTxTimeoutMs = 30000;
constexpr std::uint32_t kDetachTimeoutMs = 500;
constexpr std::uint32_t kUnattachedPollMs = 50;
constexpr std::uint32_t kTxRetryDelayMs = 100;
constexpr std::uint8_t kPrinterGetDeviceId = 0x00;

bool logEnabled(console::LogLevel configured, console::LogLevel wanted) {
  return static_cast<int>(configured) >= static_cast<int>(wanted);
}

}  // namespace

bool PrinterLink::begin(const Config& config) {
  // The USB backend has no UART flow control; keep the mode for `show` only.
  flowMode_ = config.flow;
  logLevel_ = config.log;

  stream_ = xStreamBufferCreate(kBufferSize, 1);
  if (stream_ == nullptr) {
    Serial.println("error: cannot allocate 32 KiB stream buffer");
    return false;
  }

  installDone_ = xSemaphoreCreateBinary();
  txDone_ = xSemaphoreCreateBinary();
  controlDone_ = xSemaphoreCreateBinary();
  if (installDone_ == nullptr || txDone_ == nullptr ||
      controlDone_ == nullptr) {
    Serial.println("error: cannot allocate usb semaphores");
    return false;
  }

  if (xTaskCreate(usbLibTaskEntry, "usb_lib", 4096, this, 5,
                  &usbLibTaskHandle_) != pdPASS) {
    Serial.println("error: cannot create usb_lib task");
    return false;
  }

  if (xSemaphoreTake(installDone_, pdMS_TO_TICKS(kUsbInstallTimeoutMs)) !=
          pdTRUE ||
      !installOk_) {
    Serial.println("error: usb host install failed");
    return false;
  }

  if (xTaskCreate(clientTaskEntry, "usb_client", 4096, this, 5,
                  &clientTaskHandle_) != pdPASS) {
    Serial.println("error: cannot create usb_client task");
    return false;
  }

  if (xTaskCreate(txTaskEntry, "printer_tx", 4096, this, 4,
                  &txTaskHandle_) != pdPASS) {
    Serial.println("error: cannot create printer TX task");
    return false;
  }

  return true;
}

void PrinterLink::setRxCallback(RxCallback callback) {
  rxCallback_ = std::move(callback);
}

void PrinterLink::setFlowMode(console::FlowMode flow) {
  // USB ignores flow control; remember the configured mode for diagnostics.
  flowMode_ = flow;
}

void PrinterLink::noteBleWrite(std::size_t bytes) {
  bytesFromBle_ += static_cast<std::uint32_t>(bytes);
  ++writeCount_;
}

std::size_t PrinterLink::enqueue(const std::uint8_t* data, std::size_t length,
                                 std::uint32_t timeoutMs) {
  if (stream_ == nullptr || data == nullptr || length == 0) {
    return 0;
  }

  std::size_t accepted = 0;
  const std::uint32_t started = millis();
  while (accepted < length) {
    std::uint32_t remainingMs = 0;
    if (timeoutMs > 0) {
      const std::uint32_t elapsed = millis() - started;
      if (elapsed >= timeoutMs) {
        break;
      }
      remainingMs = timeoutMs - elapsed;
      if (remainingMs > 100) {
        remainingMs = 100;
      }
    }

    TickType_t ticks = pdMS_TO_TICKS(remainingMs);
    if (timeoutMs > 0 && ticks == 0) {
      ticks = 1;
    }
    const std::size_t sent = xStreamBufferSend(
        stream_, data + accepted, length - accepted, ticks);
    accepted += sent;

    // The loop task can call enqueue() for `test`; keep the task watchdog fed
    // while the print queue is full instead of letting it fire mid-write.
    esp_task_wdt_reset();

    if (timeoutMs == 0) {
      break;
    }
  }

  if (accepted < length) {
    const std::size_t dropped = length - accepted;
    bytesDropped_ += static_cast<std::uint32_t>(dropped);
    Serial.printf("error: print queue full, dropped %u of %u bytes\n",
                  static_cast<unsigned>(dropped),
                  static_cast<unsigned>(length));
  }
  return accepted;
}

std::size_t PrinterLink::enqueue(const char* text, std::uint32_t timeoutMs) {
  if (text == nullptr) {
    return 0;
  }
  const std::size_t length = std::strlen(text);
  return enqueue(reinterpret_cast<const std::uint8_t*>(text), length, timeoutMs);
}

bool PrinterLink::isPaused() const {
  // USB host transfers NAK while the printer is busy; there is no XON/XOFF.
  return false;
}

bool PrinterLink::takeActivity() {
  if (activity_) {
    activity_ = false;
    return true;
  }
  return false;
}

std::size_t PrinterLink::queueFill() const {
  if (stream_ == nullptr) {
    return 0;
  }
  return xStreamBufferBytesAvailable(stream_);
}

bool PrinterLink::isUsb() const { return true; }

bool PrinterLink::printerAttached() const { return attached_; }

std::uint16_t PrinterLink::usbVid() const { return vid_; }

std::uint16_t PrinterLink::usbPid() const { return pid_; }

const char* PrinterLink::deviceId() const { return deviceId_; }

void PrinterLink::usbLibTaskEntry(void* arg) {
  static_cast<PrinterLink*>(arg)->usbLibTask();
}

void PrinterLink::clientTaskEntry(void* arg) {
  static_cast<PrinterLink*>(arg)->clientTask();
}

void PrinterLink::txTaskEntry(void* arg) {
  static_cast<PrinterLink*>(arg)->txTask();
}

void PrinterLink::usbLibTask() {
  usb_host_config_t config = {};
  config.skip_phy_setup = false;
  config.intr_flags = ESP_INTR_FLAG_LEVEL1;

  const esp_err_t err = usb_host_install(&config);
  installOk_ = (err == ESP_OK);
  xSemaphoreGive(installDone_);
  if (err != ESP_OK) {
    Serial.printf("error: usb_host_install failed: %d\n", static_cast<int>(err));
    vTaskDelete(nullptr);
    return;
  }

  for (;;) {
    std::uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

void PrinterLink::clientEventCallback(const usb_host_client_event_msg_t* event,
                                      void* arg) {
  auto* self = static_cast<PrinterLink*>(arg);
  // This callback runs in the client task. Never block or submit transfers
  // here; just record the event for clientTask() to process.
  switch (event->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      self->newDevAddress_ = event->new_dev.address;
      self->newDevPending_ = true;
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      self->devGonePending_ = true;
      break;
    default:
      break;
  }
}

void PrinterLink::clientTask() {
  usb_host_client_config_t config = {};
  config.is_synchronous = false;
  config.max_num_event_msg = 5;
  config.async.client_event_callback = &PrinterLink::clientEventCallback;
  config.async.callback_arg = this;

  const esp_err_t err = usb_host_client_register(&config, &client_);
  if (err != ESP_OK) {
    Serial.printf("error: usb_host_client_register failed: %d\n",
                  static_cast<int>(err));
    vTaskDelete(nullptr);
    return;
  }

  for (;;) {
    // Process anything recorded by the event callback (including events that
    // arrived while a previous device was being set up) before blocking again.
    if (devGonePending_) {
      devGonePending_ = false;
      processDeviceGone();
    }
    if (newDevPending_) {
      newDevPending_ = false;
      processNewDevice(newDevAddress_);
    }
    usb_host_client_handle_events(client_, portMAX_DELAY);
  }
}

void PrinterLink::processNewDevice(std::uint8_t address) {
  if (device_ != nullptr) {
    return;  // Only one printer is supported.
  }

  esp_err_t err = usb_host_device_open(client_, address, &device_);
  if (err != ESP_OK) {
    device_ = nullptr;
    Serial.printf("error: usb_host_device_open failed: %d\n",
                  static_cast<int>(err));
    return;
  }

  const usb_device_desc_t* deviceDesc = nullptr;
  if (usb_host_get_device_descriptor(device_, &deviceDesc) == ESP_OK &&
      deviceDesc != nullptr) {
    vid_ = deviceDesc->idVendor;
    pid_ = deviceDesc->idProduct;
  }

  bool foundPrinter = false;
  std::uint8_t intfNumber = 0;
  std::uint8_t intfAlt = 0;
  outEndpoint_ = 0;
  inEndpoint_ = 0;
  outMps_ = 64;
  inMps_ = 64;

  const usb_config_desc_t* configDesc = nullptr;
  err = usb_host_get_active_config_descriptor(device_, &configDesc);
  if (err == ESP_OK && configDesc != nullptr) {
    const std::uint8_t* base =
        reinterpret_cast<const std::uint8_t*>(configDesc);
    const std::uint16_t total = configDesc->wTotalLength;
    std::uint16_t offset = 0;
    bool inPrinterInterface = false;

    while (offset + 2 <= total) {
      const std::uint8_t bLength = base[offset];
      const std::uint8_t bDescriptorType = base[offset + 1];
      if (bLength == 0 || offset + bLength > total) {
        break;
      }

      if (bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE &&
          bLength >= USB_INTF_DESC_SIZE) {
        const auto* intf =
            reinterpret_cast<const usb_intf_desc_t*>(base + offset);
        if (!foundPrinter && intf->bInterfaceClass == USB_CLASS_PRINTER) {
          foundPrinter = true;
          intfNumber = intf->bInterfaceNumber;
          intfAlt = intf->bAlternateSetting;
          inPrinterInterface = true;
        } else {
          inPrinterInterface = false;
        }
      } else if (bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
                 bLength >= USB_EP_DESC_SIZE && inPrinterInterface) {
        const auto* ep = reinterpret_cast<const usb_ep_desc_t*>(base + offset);
        if (USB_EP_DESC_GET_XFERTYPE(ep) == USB_BM_ATTRIBUTES_XFER_BULK) {
          const std::uint16_t mps = USB_EP_DESC_GET_MPS(ep);
          if (USB_EP_DESC_GET_EP_DIR(ep) == 0) {
            if (outEndpoint_ == 0) {
              outEndpoint_ = ep->bEndpointAddress;
              outMps_ = mps != 0 ? mps : 64;
            }
          } else if (inEndpoint_ == 0) {
            inEndpoint_ = ep->bEndpointAddress;
            inMps_ = mps != 0 ? mps : 64;
          }
        }
      }

      offset = static_cast<std::uint16_t>(offset + bLength);
    }
  } else {
    Serial.printf("error: usb config descriptor failed: %d\n",
                  static_cast<int>(err));
  }

  if (!foundPrinter || outEndpoint_ == 0) {
    if (logEnabled(logLevel_, console::LogLevel::Info)) {
      Serial.printf("usb: device %04x:%04x is not a printer\n",
                    static_cast<unsigned>(vid_),
                    static_cast<unsigned>(pid_));
    }
    usb_host_device_close(client_, device_);
    device_ = nullptr;
    vid_ = 0;
    pid_ = 0;
    outEndpoint_ = 0;
    inEndpoint_ = 0;
    return;
  }

  interfaceNumber_ = intfNumber;
  interfaceAlt_ = intfAlt;
  err = usb_host_interface_claim(client_, device_, interfaceNumber_,
                                 interfaceAlt_);
  if (err != ESP_OK) {
    Serial.printf("error: usb interface claim failed: %d\n",
                  static_cast<int>(err));
    usb_host_device_close(client_, device_);
    device_ = nullptr;
    vid_ = 0;
    pid_ = 0;
    outEndpoint_ = 0;
    inEndpoint_ = 0;
    return;
  }
  interfaceClaimed_ = true;

  fetchDeviceId();

  // The printer may have been unplugged while the control transfer ran.
  if (devGonePending_) {
    return;
  }

  attached_ = true;
  if (logEnabled(logLevel_, console::LogLevel::Info)) {
    Serial.printf("usb: printer attached VID:PID=%04x:%04x\n",
                  static_cast<unsigned>(vid_), static_cast<unsigned>(pid_));
    if (deviceId_[0] != '\0') {
      Serial.printf("usb: id %s\n", deviceId_);
    }
  }

  if (inEndpoint_ != 0) {
    startRx();
  }
}

void PrinterLink::fetchDeviceId() {
  if (controlTransfer_ != nullptr && controlInFlight_) {
    return;  // A previous transfer never completed; do not overwrite it.
  }
  if (controlTransfer_ == nullptr) {
    if (usb_host_transfer_alloc(kControlBufferSize, 0, &controlTransfer_) !=
            ESP_OK ||
        controlTransfer_ == nullptr) {
      controlTransfer_ = nullptr;
      if (logEnabled(logLevel_, console::LogLevel::Debug)) {
        Serial.println("usb: cannot allocate GET_DEVICE_ID transfer");
      }
      return;
    }
  }

  auto* setup =
      reinterpret_cast<usb_setup_packet_t*>(controlTransfer_->data_buffer);
  setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN |
                         USB_BM_REQUEST_TYPE_TYPE_CLASS |
                         USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
  setup->bRequest = kPrinterGetDeviceId;
  setup->wValue = 0;  // Configuration index (zero-based).
  setup->wIndex =
      static_cast<std::uint16_t>((interfaceNumber_ << 8) | interfaceAlt_);
  setup->wLength = 248;  // 8-byte setup + 248 = 256, the control limit.

  controlTransfer_->device_handle = device_;
  controlTransfer_->bEndpointAddress = 0;
  controlTransfer_->num_bytes = static_cast<int>(kControlBufferSize);
  controlTransfer_->callback = &PrinterLink::controlDoneCallback;
  controlTransfer_->context = this;
  controlTransfer_->timeout_ms = 0;

  xSemaphoreTake(controlDone_, 0);
  controlInFlight_ = true;
  const esp_err_t err =
      usb_host_transfer_submit_control(client_, controlTransfer_);
  if (err != ESP_OK) {
    controlInFlight_ = false;
    if (logEnabled(logLevel_, console::LogLevel::Debug)) {
      Serial.printf("usb: GET_DEVICE_ID submit failed: %d\n",
                    static_cast<int>(err));
    }
    return;
  }

  // Transfer completions are dispatched by usb_host_client_handle_events()
  // on this same task, so pump events instead of blocking on a semaphore.
  const std::uint32_t started = millis();
  while (controlInFlight_ && (millis() - started) < kControlTimeoutMs) {
    usb_host_client_handle_events(client_, pdMS_TO_TICKS(20));
  }
  if (controlInFlight_) {
    if (logEnabled(logLevel_, console::LogLevel::Debug)) {
      Serial.println("usb: GET_DEVICE_ID timed out");
    }
    return;
  }
  xSemaphoreTake(controlDone_, 0);

  if (controlTransfer_->status != USB_TRANSFER_STATUS_COMPLETED) {
    if (logEnabled(logLevel_, console::LogLevel::Debug)) {
      Serial.printf("usb: GET_DEVICE_ID status %d\n",
                    static_cast<int>(controlTransfer_->status));
    }
    return;
  }

  const std::size_t header = sizeof(usb_setup_packet_t);
  const int actual = controlTransfer_->actual_num_bytes;
  if (actual <= static_cast<int>(header) + 2) {
    return;
  }

  const std::uint8_t* response =
      controlTransfer_->data_buffer + static_cast<std::size_t>(header);
  const std::size_t responseLength =
      static_cast<std::size_t>(actual) - header;
  std::size_t available = responseLength - 2;
  const std::size_t declared =
      (static_cast<std::size_t>(response[0]) << 8) | response[1];
  if (declared >= 2 && (declared - 2) < available) {
    available = declared - 2;
  }
  if (available > sizeof(deviceId_) - 1) {
    available = sizeof(deviceId_) - 1;
  }
  std::memcpy(deviceId_, response + 2, available);
  deviceId_[available] = '\0';
}

void PrinterLink::processDeviceGone() {
  const bool wasAttached = attached_;
  attached_ = false;

  // Let the host stack fail any in-flight transfers. Their callbacks run
  // while we pump events here, and free the transfer handles for reuse.
  std::uint32_t started = millis();
  while (txInFlight_ && (millis() - started) < kDetachTimeoutMs) {
    usb_host_client_handle_events(client_, pdMS_TO_TICKS(20));
  }
  started = millis();
  while (rxInFlight_ && (millis() - started) < kDetachTimeoutMs) {
    usb_host_client_handle_events(client_, pdMS_TO_TICKS(20));
  }
  started = millis();
  while (controlInFlight_ && (millis() - started) < kDetachTimeoutMs) {
    usb_host_client_handle_events(client_, pdMS_TO_TICKS(20));
  }

  if (interfaceClaimed_ && device_ != nullptr) {
    usb_host_interface_release(client_, device_, interfaceNumber_);
  }
  interfaceClaimed_ = false;

  if (device_ != nullptr) {
    usb_host_device_close(client_, device_);
    device_ = nullptr;
  }

  if (controlTransfer_ != nullptr && !controlInFlight_) {
    usb_host_transfer_free(controlTransfer_);
    controlTransfer_ = nullptr;
  }
  if (rxTransfer_ != nullptr && !rxInFlight_) {
    usb_host_transfer_free(rxTransfer_);
    rxTransfer_ = nullptr;
  }

  outEndpoint_ = 0;
  inEndpoint_ = 0;
  outMps_ = 64;
  inMps_ = 64;
  interfaceNumber_ = 0;
  interfaceAlt_ = 0;
  vid_ = 0;
  pid_ = 0;
  deviceId_[0] = '\0';

  if (wasAttached && logEnabled(logLevel_, console::LogLevel::Info)) {
    Serial.println("usb: printer detached");
  }
}

void PrinterLink::startRx() {
  if (inEndpoint_ == 0) {
    return;
  }
  if (rxTransfer_ == nullptr) {
    if (usb_host_transfer_alloc(inMps_, 0, &rxTransfer_) != ESP_OK ||
        rxTransfer_ == nullptr) {
      rxTransfer_ = nullptr;
      Serial.println("error: cannot allocate usb RX transfer");
      return;
    }
  } else if (rxInFlight_) {
    return;
  }

  rxTransfer_->device_handle = device_;
  rxTransfer_->bEndpointAddress = inEndpoint_;
  rxTransfer_->num_bytes = static_cast<int>(inMps_);
  rxTransfer_->callback = &PrinterLink::rxDoneCallback;
  rxTransfer_->context = this;
  rxTransfer_->timeout_ms = 0;

  rxInFlight_ = true;
  if (usb_host_transfer_submit(rxTransfer_) != ESP_OK) {
    rxInFlight_ = false;
    Serial.println("error: usb: RX submit failed");
  }
}

void PrinterLink::txDoneCallback(usb_transfer_t* transfer) {
  auto* self = static_cast<PrinterLink*>(transfer->context);
  self->txInFlight_ = false;
  xSemaphoreGive(self->txDone_);
}

void PrinterLink::rxDoneCallback(usb_transfer_t* transfer) {
  auto* self = static_cast<PrinterLink*>(transfer->context);

  if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
    const int actual = transfer->actual_num_bytes;
    if (actual > 0 && self->rxCallback_) {
      self->rxCallback_(transfer->data_buffer,
                        static_cast<std::size_t>(actual));
    }

    if (self->attached_) {
      transfer->device_handle = self->device_;
      transfer->bEndpointAddress = self->inEndpoint_;
      transfer->num_bytes = static_cast<int>(self->inMps_);
      transfer->callback = &PrinterLink::rxDoneCallback;
      transfer->context = self;
      transfer->timeout_ms = 0;
      if (usb_host_transfer_submit(transfer) == ESP_OK) {
        return;
      }
      Serial.println("error: usb: RX resubmit failed");
    }
  }

  self->rxInFlight_ = false;
}

void PrinterLink::controlDoneCallback(usb_transfer_t* transfer) {
  auto* self = static_cast<PrinterLink*>(transfer->context);
  self->controlInFlight_ = false;
  xSemaphoreGive(self->controlDone_);
}

void PrinterLink::txTask() {
  // Allocate once; the transfer is reused for every chunk and is not freed on
  // detach (the TX task may be between the attached_ check and submission).
  if (usb_host_transfer_alloc(kTxTransferSize, 0, &txTransfer_) != ESP_OK ||
      txTransfer_ == nullptr) {
    txTransfer_ = nullptr;
    Serial.println("error: cannot allocate usb TX transfer");
    vTaskDelete(nullptr);
    return;
  }

  for (;;) {
    if (!attached_ || device_ == nullptr || outEndpoint_ == 0) {
      // Data stays in the 32 KiB stream buffer; BLE writes hit the normal
      // 15 s back-pressure/drop path once it fills.
      vTaskDelay(pdMS_TO_TICKS(kUnattachedPollMs));
      continue;
    }

    const std::size_t received =
        xStreamBufferReceive(stream_, txTransfer_->data_buffer,
                             kTxTransferSize, pdMS_TO_TICKS(50));
    if (received == 0) {
      continue;
    }

    txTransfer_->device_handle = device_;
    txTransfer_->bEndpointAddress = outEndpoint_;
    txTransfer_->num_bytes = static_cast<int>(received);
    txTransfer_->callback = &PrinterLink::txDoneCallback;
    txTransfer_->context = this;
    txTransfer_->timeout_ms = 0;

    xSemaphoreTake(txDone_, 0);
    txInFlight_ = true;
    const esp_err_t err = usb_host_transfer_submit(txTransfer_);
    if (err != ESP_OK) {
      txInFlight_ = false;
      Serial.printf("error: usb: tx submit failed: %d\n",
                    static_cast<int>(err));
      bytesDropped_ += static_cast<std::uint32_t>(received);
      vTaskDelay(pdMS_TO_TICKS(kTxRetryDelayMs));
      continue;
    }

    if (xSemaphoreTake(txDone_, pdMS_TO_TICKS(kTxTimeoutMs)) != pdTRUE) {
      // A bulk OUT transfer can legitimately stay pending while the printer
      // NAKs a full buffer, but 30 s means something is wrong. Never reuse the
      // transfer until its callback has actually run.
      Serial.println("error: usb: tx timeout");
      while (txInFlight_) {
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    if (txTransfer_->status == USB_TRANSFER_STATUS_COMPLETED) {
      const int actual = txTransfer_->actual_num_bytes;
      if (actual > 0) {
        bytesToPrinter_ += static_cast<std::uint32_t>(actual);
        activity_ = true;
      }
    } else {
      Serial.printf("error: usb: tx status %d\n",
                    static_cast<int>(txTransfer_->status));
      bytesDropped_ += static_cast<std::uint32_t>(received);
    }
  }
}

#endif  // ZB_PRINTER_USB

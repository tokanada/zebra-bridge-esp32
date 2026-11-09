#include "printer_link.h"

#if !ZB_PRINTER_USB

#include <esp_task_wdt.h>
#include <freertos/stream_buffer.h>

#include "board_config.h"

namespace {

constexpr std::uint32_t kXoffWarnIntervalMs = 60000;

bool addCurrentTaskToWatchdog() {
  const esp_err_t err = esp_task_wdt_add(nullptr);
  return err == ESP_OK || err == ESP_ERR_INVALID_ARG;
}

}  // namespace

bool PrinterLink::isPaused() const { return filter_.paused(); }

bool PrinterLink::isUsb() const { return false; }

bool PrinterLink::printerAttached() const { return true; }

std::uint16_t PrinterLink::usbVid() const { return 0; }

std::uint16_t PrinterLink::usbPid() const { return 0; }

const char* PrinterLink::deviceId() const { return ""; }

bool PrinterLink::begin(const Config& config) {
  filter_.setEnabled(config.flow == console::FlowMode::XonXoff);
  rxPin_ = config.rxPin;
  txPin_ = config.txPin;
  ctsPin_ = config.ctsPin;

  stream_ = xStreamBufferCreate(kBufferSize, 1);
  if (stream_ == nullptr) {
    Serial.println("error: cannot allocate 32 KiB stream buffer");
    return false;
  }

  uart_ = new HardwareSerial(ZB_UART_NUM);
  uart_->setRxBufferSize(1024);
  uart_->begin(config.baud, SERIAL_8N1, config.rxPin, config.txPin);

  if (config.ctsPin >= 0 && config.flow == console::FlowMode::Cts) {
    uart_->setPins(config.rxPin, config.txPin, config.ctsPin, -1);
    uart_->setHwFlowCtrlMode(UART_HW_FLOWCTRL_CTS);
  }

  BaseType_t ok = xTaskCreate(txTaskEntry, "printer_tx", 4096, this, 3, nullptr);
  if (ok != pdPASS) {
    Serial.println("error: cannot create printer TX task");
    return false;
  }
  ok = xTaskCreate(rxTaskEntry, "printer_rx", 4096, this, 2, nullptr);
  if (ok != pdPASS) {
    Serial.println("error: cannot create printer RX task");
    return false;
  }
  return true;
}

void PrinterLink::setRxCallback(RxCallback callback) {
  rxCallback_ = std::move(callback);
}

void PrinterLink::setFlowMode(console::FlowMode flow) {
  filter_.setEnabled(flow == console::FlowMode::XonXoff);
  if (uart_ == nullptr) {
    return;
  }
  if (flow == console::FlowMode::Cts && ctsPin_ >= 0) {
    uart_->setPins(rxPin_, txPin_, ctsPin_, -1);
    uart_->setHwFlowCtrlMode(UART_HW_FLOWCTRL_CTS);
  } else {
    uart_->setHwFlowCtrlMode(UART_HW_FLOWCTRL_DISABLE);
  }
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

void PrinterLink::txTaskEntry(void* arg) {
  static_cast<PrinterLink*>(arg)->txTask();
}

void PrinterLink::rxTaskEntry(void* arg) {
  static_cast<PrinterLink*>(arg)->rxTask();
}

void PrinterLink::txTask() {
  addCurrentTaskToWatchdog();

  std::uint8_t buffer[kTxChunkSize];
  std::size_t pending = 0;
  bool wasPaused = false;
  std::uint32_t pausedAt = 0;
  std::uint32_t lastWarning = 0;

  for (;;) {
    esp_task_wdt_reset();

    if (filter_.paused()) {
      const std::uint32_t now = millis();
      if (!wasPaused) {
        wasPaused = true;
        pausedAt = now;
        lastWarning = now;
      }
      if (now - pausedAt >= kXoffWarnIntervalMs &&
          now - lastWarning >= kXoffWarnIntervalMs) {
        lastWarning = now;
        Serial.println("warn: printer has held XOFF for more than 60 s");
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    wasPaused = false;

    if (pending == 0) {
      pending = xStreamBufferReceive(stream_, buffer, sizeof(buffer),
                                     pdMS_TO_TICKS(50));
    }
    if (pending == 0) {
      continue;
    }

    // XOFF may have arrived while the receive was pending; keep the chunk and
    // wait for XON instead of writing it after the printer asked us to stop.
    if (filter_.paused()) {
      continue;
    }

    // Only write what fits in the UART TX FIFO so a deasserted CTS (printer
    // off or unplugged) never blocks this task long enough to trip the
    // task watchdog. Unsent bytes stay in `buffer` for the next pass.
    const int room = uart_->availableForWrite();
    if (room <= 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    const std::size_t count =
        pending < static_cast<std::size_t>(room) ? pending
                                                 : static_cast<std::size_t>(room);
    const std::size_t written = uart_->write(buffer, count);
    if (written < pending) {
      std::memmove(buffer, buffer + written, pending - written);
    }
    pending -= written;
    bytesToPrinter_ += static_cast<std::uint32_t>(written);
    if (written > 0) {
      activity_ = true;
    }
  }
}

void PrinterLink::rxTask() {
  addCurrentTaskToWatchdog();

  std::uint8_t raw[256];
  std::uint8_t filtered[256];

  for (;;) {
    esp_task_wdt_reset();

    const int available = uart_->available();
    if (available > 0) {
      const std::size_t wanted =
          static_cast<std::size_t>(available) < sizeof(raw)
              ? static_cast<std::size_t>(available)
              : sizeof(raw);
      const std::size_t read =
          uart_->readBytes(raw, wanted);

      if (read > 0) {
        const std::size_t count =
            filter_.process(raw, read, filtered, sizeof(filtered));
        if (count > 0 && rxCallback_) {
          rxCallback_(filtered, count);
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

#endif  // !ZB_PRINTER_USB

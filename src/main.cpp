#include <Arduino.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include <string>

#include "ble_printer.h"
#include "board_config.h"
#include "config_store.h"
#include "console.h"
#include "printer_link.h"

namespace {

#if defined(CONFIG_IDF_TARGET_ESP32C3)
constexpr console::Target kTarget = console::Target::Esp32C3;
#else
constexpr console::Target kTarget = console::Target::Esp32;
#endif

constexpr std::uint32_t kHealthCheckIntervalMs = 10000;
constexpr std::uint32_t kDataFlickerMs = 30;

ConfigStore configStore;
Config config;
PrinterLink printerLink;
BlePrinter blePrinter;

std::uint32_t flickerUntil = 0;
std::uint32_t lastHealthCheck = 0;
volatile bool bootComplete = false;

bool ledActiveLow = false;
int ledPin = -1;

void setLed(bool on) {
  if (ledPin < 0) {
    return;
  }
  const bool level = ledActiveLow ? !on : on;
  digitalWrite(ledPin, level ? HIGH : LOW);
}

void updateLed() {
  const std::uint32_t now = millis();
  if (blePrinter.isConnected()) {
    setLed(true);
  } else if (blePrinter.isAdvertising()) {
    setLed((now / 1000) % 2 == 0);  // slow blink, 1 s toggle
  } else {
    setLed((now / 100) % 2 == 0);  // fast blink, 100 ms toggle
  }
  if (now < flickerUntil) {
    setLed(false);
  }
}

// Keeps the fast boot blink running while setup() performs the blocking BLE
// initialization. Deletes itself once setup() completes.
void bootLedTask(void*) {
  while (!bootComplete) {
    setLed((millis() / 100) % 2 == 0);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  vTaskDelete(nullptr);
}

void printHelp() {
  Serial.println("commands:");
  Serial.println("  help");
  Serial.println("  show");
  Serial.println("  set name <text>            1..29 printable ASCII chars");
  Serial.println("  set serial <text>          1..32 printable ASCII chars");
  Serial.println("  set baud <n>               1200 2400 4800 9600 14400 19200 28800 38400 57600 115200");
  Serial.println("  set flow <none|xonxoff|cts>");
  Serial.println("  set tx|rx|cts|led <gpio>   -1 disables cts/led");
  Serial.println("  set ledlow <0|1>");
  Serial.println("  set log <error|info|debug>");
  Serial.println("  test                       queue a ZPL test label");
  Serial.println("  reset-config               erase the zbridge NVS namespace");
  Serial.println("  reboot");
}

void printConfig() {
  Serial.printf("config source: %s hash 0x%08x\n", buildConfigSource(),
                static_cast<unsigned>(buildConfigHash()));
  Serial.println("config:");
  Serial.printf("  name:    %s\n", config.name.c_str());
  Serial.printf("  serial:  %s\n", config.serial.c_str());
  Serial.printf("  baud:    %u\n", static_cast<unsigned>(config.baud));
  Serial.printf("  flow:    %s\n", console::flowModeName(config.flow));
  Serial.printf("  tx:      %d\n", config.txPin);
  Serial.printf("  rx:      %d\n", config.rxPin);
  Serial.printf("  cts:     %d\n", config.ctsPin);
  Serial.printf("  led:     %d\n", config.ledPin);
  Serial.printf("  ledlow:  %d\n", config.ledActiveLow ? 1 : 0);
  Serial.printf("  log:     %s\n", console::logLevelName(config.log));
}

void printShow() {
  Serial.printf("uptime: %lu s\n",
                static_cast<unsigned long>(millis() / 1000));
  if (blePrinter.isConnected()) {
    Serial.printf("connected: y (%s)\n", blePrinter.peerAddress().c_str());
  } else {
    Serial.println("connected: n");
  }
  Serial.printf("mtu: %u\n", blePrinter.mtu());
  Serial.printf("ble bytes: %lu\n",
                static_cast<unsigned long>(printerLink.bytesFromBle()));
  Serial.printf("printer bytes: %lu\n",
                static_cast<unsigned long>(printerLink.bytesToPrinter()));
  Serial.printf("dropped: %lu\n",
                static_cast<unsigned long>(printerLink.bytesDropped()));
  Serial.printf("queue fill: %u / %u\n",
                static_cast<unsigned>(printerLink.queueFill()),
                static_cast<unsigned>(printerLink.queueCapacity()));
  Serial.printf("xoff: %s\n", printerLink.isPaused() ? "yes" : "no");
  Serial.printf("writes: %lu\n",
                static_cast<unsigned long>(printerLink.writeCount()));
  printConfig();
}

void queueTestLabel() {
  std::string label = "^XA\n";
  label += "^FO50,50^A0N,40,40^FDZebra Bridge Test^FS\n";
  label += "^FO50,110^A0N,28,28^FD";
  label += config.serial;
  label += "^FS\n^XZ\n";

  const std::size_t queued = printerLink.enqueue(label.c_str(),
                                                 PrinterLink::kWriteBlockTimeoutMs);
  if (queued == label.size()) {
    Serial.printf("test label queued (%u bytes)\n",
                  static_cast<unsigned>(queued));
  } else {
    Serial.printf("error: test label only partially queued (%u of %u bytes)\n",
                  static_cast<unsigned>(queued),
                  static_cast<unsigned>(label.size()));
  }
}

void processCommand(const std::string& line) {
  const console::ParsedCommand command = console::parse(line, kTarget);

  switch (command.command) {
    case console::Command::None:
      return;

    case console::Command::Invalid:
      Serial.printf("error: %s\n", command.error.c_str());
      return;

    case console::Command::Help:
      printHelp();
      return;

    case console::Command::Show:
      printShow();
      return;

    case console::Command::Test:
      queueTestLabel();
      return;

    case console::Command::ResetConfig:
      configStore.clear();
      Serial.println("config erased; reboot to apply");
      return;

    case console::Command::Reboot:
      Serial.println("rebooting...");
      Serial.flush();
      delay(100);
      esp_restart();
      return;

    case console::Command::SetName:
      config.name = command.text;
      break;
    case console::Command::SetSerial:
      config.serial = command.text;
      break;
    case console::Command::SetBaud:
      config.baud = command.number;
      break;
    case console::Command::SetFlow:
      config.flow = command.flow;
      printerLink.setFlowMode(config.flow);
      break;
    case console::Command::SetTx:
      config.txPin = command.gpio;
      break;
    case console::Command::SetRx:
      config.rxPin = command.gpio;
      break;
    case console::Command::SetCts:
      config.ctsPin = command.gpio;
      break;
    case console::Command::SetLed:
      config.ledPin = command.gpio;
      break;
    case console::Command::SetLedLow:
      config.ledActiveLow = command.flag;
      ledActiveLow = command.flag;
      break;
    case console::Command::SetLog:
      config.log = command.log;
      blePrinter.setLogLevel(command.log);
      break;
  }

  configStore.save(config);
  Serial.println("saved; reboot to apply");
}

void readConsole() {
  static std::string line;
  static bool discarding = false;

  while (Serial.available() > 0) {
    const int value = Serial.read();
    if (value < 0) {
      break;
    }
    const char character = static_cast<char>(value);

    if (character == '\r' || character == '\n') {
      if (!discarding && !line.empty()) {
        processCommand(line);
      }
      line.clear();
      discarding = false;
      continue;
    }

    if (discarding) {
      continue;
    }

    if (line.size() >= console::kMaxLineLength) {
      line.clear();
      discarding = true;
      Serial.println("error: line exceeds 128 characters");
      continue;
    }
    line.push_back(character);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  configStore.begin();
  config = configStore.load();

  ledPin = config.ledPin;
  ledActiveLow = config.ledActiveLow;
  if (ledPin >= 0) {
    pinMode(ledPin, OUTPUT);
    setLed(false);
    xTaskCreate(bootLedTask, "led_boot", 2048, nullptr, 1, nullptr);
  }

  Serial.println();
  Serial.println("zebra-bridge-esp32 starting");
  printConfig();

  esp_task_wdt_init(10, true);

  printerLink.setRxCallback(
      [](const std::uint8_t* data, std::size_t length) {
        blePrinter.notifyFromPrinter(data, length);
      });
  if (!printerLink.begin(config)) {
    Serial.println("error: printer link init failed");
  }

  if (!blePrinter.begin(config, &printerLink, config.log)) {
    Serial.println("error: BLE init failed after 3 attempts; restarting");
    bootComplete = true;
    for (int i = 0; i < 20; ++i) {
      setLed((i % 2) == 0);
      delay(100);
    }
    esp_restart();
  }

  esp_task_wdt_add(nullptr);

  lastHealthCheck = millis();
  bootComplete = true;
  Serial.println("ready");
}

void loop() {
  esp_task_wdt_reset();

  readConsole();
  blePrinter.poll();

  if (printerLink.takeActivity()) {
    flickerUntil = millis() + kDataFlickerMs;
  }
  updateLed();

  const std::uint32_t now = millis();
  if (now - lastHealthCheck >= kHealthCheckIntervalMs) {
    lastHealthCheck = now;
    if (!blePrinter.isConnected() && !blePrinter.isAdvertising()) {
      Serial.println("warn: advertising stopped; restarting");
      blePrinter.startAdvertising();
    }
  }

  delay(2);
}

#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <string>

#include "config_store.h"

class PrinterLink;

// NimBLE GATT peripheral that emulates the Zebra ZD421 parser profile and
// forwards WRITE payloads to the printer link.
class BlePrinter {
 public:
  bool begin(const Config& config, PrinterLink* link,
             console::LogLevel logLevel);
  bool startAdvertising();

  void poll();  // call frequently: finishes idle job-summary logging

  void setLogLevel(console::LogLevel level) { logLevel_ = level; }

  bool isConnected() const { return connected_; }
  bool isAdvertising() const;
  const std::string& peerAddress() const { return peerAddress_; }
  std::uint16_t mtu() const { return mtu_; }

  void notifyFromPrinter(const std::uint8_t* data, std::size_t length);

 private:
  class ServerCallbacks;
  class WriteCallbacks;

  void handleConnect(NimBLEConnInfo& connInfo);
  void handleDisconnect(NimBLEConnInfo& connInfo);
  void handleMtuChange(std::uint16_t mtu);
  void handleWrite(NimBLECharacteristic* characteristic);

  PrinterLink* link_ = nullptr;
  NimBLEServer* server_ = nullptr;
  NimBLECharacteristic* writeChar_ = nullptr;
  NimBLECharacteristic* readChar_ = nullptr;

  std::string deviceName_;
  std::string peerAddress_;
  std::uint16_t connHandle_ = 0;
  std::uint16_t mtu_ = 23;
  bool connected_ = false;
  bool subscribed_ = false;

  console::LogLevel logLevel_ = console::LogLevel::Info;

  bool jobOpen_ = false;
  std::uint32_t jobBytes_ = 0;
  std::uint32_t jobWrites_ = 0;
  std::uint32_t lastWriteMs_ = 0;
};

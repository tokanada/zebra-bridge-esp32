#include "ble_printer.h"

#include <algorithm>

#include "printer_link.h"

namespace {

constexpr const char* kZebraServiceUuid = "38eb4a80-c570-11e3-9507-0002a5d5c51b";
constexpr const char* kZebraWriteUuid = "38eb4a82-c570-11e3-9507-0002a5d5c51b";
constexpr const char* kZebraReadUuid = "38eb4a81-c570-11e3-9507-0002a5d5c51b";
constexpr const char* kDeviceInfoServiceUuid = "180A";

constexpr std::uint8_t kReadInitialValue[3] = {0x02, '0', 0x03};

void printEscaped(const std::uint8_t* data, std::size_t length) {
  for (std::size_t i = 0; i < length; ++i) {
    const std::uint8_t byte = data[i];
    if (byte >= 0x20 && byte <= 0x7e) {
      Serial.write(byte);
    } else {
      Serial.printf("\\x%02x", byte);
    }
  }
}

}  // namespace

class BlePrinter::ServerCallbacks : public NimBLEServerCallbacks {
 public:
  explicit ServerCallbacks(BlePrinter* owner) : owner_(owner) {}

  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    (void)server;
    owner_->handleConnect(connInfo);
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo,
                    int reason) override {
    (void)server;
    (void)reason;
    owner_->handleDisconnect(connInfo);
  }

  void onMTUChange(std::uint16_t mtu, NimBLEConnInfo& connInfo) override {
    (void)connInfo;
    owner_->handleMtuChange(mtu);
  }

 private:
  BlePrinter* owner_;
};

class BlePrinter::WriteCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit WriteCallbacks(BlePrinter* owner) : owner_(owner) {}

  void onWrite(NimBLECharacteristic* characteristic,
               NimBLEConnInfo& connInfo) override {
    (void)connInfo;
    owner_->handleWrite(characteristic);
  }

  void onSubscribe(NimBLECharacteristic* characteristic,
                   NimBLEConnInfo& connInfo, std::uint16_t subValue) override {
    (void)characteristic;
    (void)connInfo;
    owner_->subscribed_ = (subValue & 0x01) != 0;
    if (owner_->logLevel_ >= console::LogLevel::Info) {
      Serial.printf("ble: client %s notifications\n",
                    subValue != 0 ? "enabled" : "disabled");
    }
  }

 private:
  BlePrinter* owner_;
};

bool BlePrinter::begin(const Config& config, PrinterLink* link,
                       console::LogLevel logLevel) {
  link_ = link;
  logLevel_ = logLevel;
  deviceName_ = config.name;

  bool initialized = false;
  for (int attempt = 1; attempt <= 3; ++attempt) {
    if (NimBLEDevice::init(deviceName_)) {
      initialized = true;
      break;
    }
    Serial.printf("error: BLE init attempt %d failed\n", attempt);
    NimBLEDevice::deinit(true);
    delay(500);
  }
  if (!initialized) {
    return false;
  }

  if (!NimBLEDevice::setMTU(517)) {
    Serial.println("warn: could not set preferred MTU 517");
  }
  NimBLEDevice::setSecurityAuth(false, false, false);

  server_ = NimBLEDevice::createServer();
  if (server_ == nullptr) {
    Serial.println("error: cannot create BLE server");
    return false;
  }
  server_->setCallbacks(new ServerCallbacks(this));

  NimBLEService* zebraService = server_->createService(kZebraServiceUuid);
  if (zebraService == nullptr) {
    Serial.println("error: cannot create Zebra service");
    return false;
  }

  writeChar_ = zebraService->createCharacteristic(
      kZebraWriteUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR,
      512);
  if (writeChar_ == nullptr) {
    Serial.println("error: cannot create write characteristic");
    return false;
  }
  writeChar_->setCallbacks(new WriteCallbacks(this));

  readChar_ = zebraService->createCharacteristic(
      kZebraReadUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  if (readChar_ == nullptr) {
    Serial.println("error: cannot create read characteristic");
    return false;
  }
  readChar_->setCallbacks(new WriteCallbacks(this));
  readChar_->setValue(kReadInitialValue, sizeof(kReadInitialValue));

  if (!zebraService->start()) {
    Serial.println("error: cannot start Zebra service");
    return false;
  }

  NimBLEService* deviceInfo = server_->createService(kDeviceInfoServiceUuid);
  if (deviceInfo == nullptr) {
    Serial.println("error: cannot create device information service");
    return false;
  }

  struct InfoCharacteristic {
    const char* uuid;
    const char* value;
  };
  const InfoCharacteristic info[] = {
      {"2A29", "Zebra Technologies"},
      {"2A24", "ZD421"},
      {"2A25", config.serial.c_str()},
      {"2A26", "V1.0.0"},
      {"2A27", "1.0"},
      {"2A28", "1.0"},
  };
  for (const auto& entry : info) {
    NimBLECharacteristic* characteristic =
        deviceInfo->createCharacteristic(entry.uuid, NIMBLE_PROPERTY::READ);
    if (characteristic == nullptr) {
      Serial.println("error: cannot create device information characteristic");
      return false;
    }
    characteristic->setValue(entry.value);
  }
  if (!deviceInfo->start()) {
    Serial.println("error: cannot start device information service");
    return false;
  }

  return startAdvertising();
}

bool BlePrinter::startAdvertising() {
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  if (advertising == nullptr) {
    return false;
  }

  NimBLEAdvertisementData advertisement;
  advertisement.setFlags(BLE_HS_ADV_F_DISC_GEN |
                         BLE_HS_ADV_F_BREDR_UNSUP);
  if (!advertisement.addServiceUUID(NimBLEUUID(kZebraServiceUuid))) {
    Serial.println("error: cannot add Zebra service UUID to advertisement");
    return false;
  }
  if (!advertisement.addTxPower()) {
    Serial.println("error: cannot add TX power to advertisement");
    return false;
  }

  NimBLEAdvertisementData scanResponse;
  if (!scanResponse.setName(deviceName_, true)) {
    Serial.println("error: cannot add local name to scan response");
    return false;
  }

  if (advertisement.getPayload().size() > 31 ||
      scanResponse.getPayload().size() > 31) {
    Serial.println("error: advertisement data exceeds 31 bytes");
    return false;
  }

  advertising->setConnectableMode(BLE_GAP_CONN_MODE_UND);
  advertising->setDiscoverableMode(BLE_GAP_DISC_MODE_GEN);
  advertising->enableScanResponse(true);
  if (!advertising->setAdvertisementData(advertisement) ||
      !advertising->setScanResponseData(scanResponse)) {
    Serial.println("error: cannot set advertisement data");
    return false;
  }

  const bool started = advertising->start();
  if (started && logLevel_ >= console::LogLevel::Info) {
    Serial.printf("ble: advertising as %s\n", deviceName_.c_str());
  }
  return started;
}

void BlePrinter::handleConnect(NimBLEConnInfo& connInfo) {
  connected_ = true;
  subscribed_ = false;
  connHandle_ = connInfo.getConnHandle();
  mtu_ = connInfo.getMTU();
  peerAddress_ = connInfo.getAddress().toString();

  if (logLevel_ >= console::LogLevel::Info) {
    Serial.printf("ble: connected to %s\n", peerAddress_.c_str());
    Serial.printf("ble: MTU %u\n", mtu_);
  }

  if (server_ != nullptr) {
    // Ask for a 15 ms interval (12 * 1.25 ms): Apple allows min == max == 15 ms,
    // and it roughly doubles throughput versus 30 ms so multi-label jobs reach
    // the printer before the previous label finishes printing. Also request
    // 251-byte link-layer packets (data length extension).
    server_->updateConnParams(connHandle_, 12, 12, 0, 500);
    server_->setDataLen(connHandle_, 251);
  }
}

void BlePrinter::handleDisconnect(NimBLEConnInfo& connInfo) {
  (void)connInfo;
  connected_ = false;
  subscribed_ = false;
  mtu_ = 23;
  if (logLevel_ >= console::LogLevel::Info) {
    Serial.println("ble: disconnected");
  }
  startAdvertising();
}

void BlePrinter::handleMtuChange(std::uint16_t mtu) {
  mtu_ = mtu;
  if (logLevel_ >= console::LogLevel::Info) {
    Serial.printf("ble: MTU now %u\n", mtu_);
  }
}

bool BlePrinter::isAdvertising() const {
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  return advertising != nullptr && advertising->isAdvertising();
}

void BlePrinter::handleWrite(NimBLECharacteristic* characteristic) {
  const NimBLEAttValue value = characteristic->getValue();

  if (logLevel_ >= console::LogLevel::Debug) {
    Serial.printf("ble: write %u bytes: ", value.size());
    printEscaped(value.data(), std::min<std::size_t>(value.size(), 64));
    Serial.println();
  }

  if (logLevel_ >= console::LogLevel::Info) {
    if (!jobOpen_) {
      jobOpen_ = true;
      jobBytes_ = 0;
      jobWrites_ = 0;
      jobStartMs_ = millis();
    }
    jobBytes_ += value.size();
    ++jobWrites_;
    lastWriteMs_ = millis();
  }

  if (link_ != nullptr) {
    link_->noteBleWrite(value.size());
    link_->enqueue(value.data(), value.size(),
                   PrinterLink::kWriteBlockTimeoutMs);
  }
}

void BlePrinter::poll() {
  if (jobOpen_ && millis() - lastWriteMs_ >= 2000) {
    jobOpen_ = false;
    if (logLevel_ >= console::LogLevel::Info) {
      const std::uint32_t elapsedMs = lastWriteMs_ - jobStartMs_;
      const std::uint32_t rate =
          elapsedMs > 0 ? static_cast<std::uint32_t>(
                              (static_cast<std::uint64_t>(jobBytes_) * 1000) /
                              elapsedMs)
                        : 0;
      Serial.printf("job: %u bytes in %u writes over %u ms (%u B/s)\n",
                    static_cast<unsigned>(jobBytes_),
                    static_cast<unsigned>(jobWrites_),
                    static_cast<unsigned>(elapsedMs),
                    static_cast<unsigned>(rate));
    }
  }
}

void BlePrinter::notifyFromPrinter(const std::uint8_t* data,
                                   std::size_t length) {
  if (!connected_ || !subscribed_ || readChar_ == nullptr || length == 0) {
    return;
  }

  const std::size_t chunkSize = mtu_ > 3 ? mtu_ - 3 : 20;
  for (std::size_t offset = 0; offset < length; offset += chunkSize) {
    const std::size_t count = std::min(chunkSize, length - offset);
    if (!readChar_->notify(data + offset, count, connHandle_)) {
      if (logLevel_ >= console::LogLevel::Debug) {
        Serial.println("ble: notification failed");
      }
      break;
    }
  }
}

#include "config_store.h"

#include "board_config.h"

namespace {

constexpr const char* kNamespace = "zbridge";

}  // namespace

Config defaultConfig() {
  Config config;
  config.name = ZB_DEFAULT_NAME;
  config.serial = ZB_DEFAULT_SERIAL;
  config.baud = ZB_DEFAULT_BAUD;
  config.flow = console::FlowMode::XonXoff;
  config.txPin = ZB_PIN_TX;
  config.rxPin = ZB_PIN_RX;
  config.ctsPin = ZB_PIN_CTS;
  config.ledPin = ZB_PIN_LED;
  config.ledActiveLow = ZB_LED_ACTIVE_LOW != 0;
  config.log = console::LogLevel::Info;
  return config;
}

void ConfigStore::begin() {
  prefs_.begin(kNamespace, false);
}

Config ConfigStore::load() const {
  Config config = defaultConfig();

  if (prefs_.isKey("name")) {
    const String value = prefs_.getString("name", config.name.c_str());
    config.name = value.c_str();
  }
  if (prefs_.isKey("serial")) {
    const String value = prefs_.getString("serial", config.serial.c_str());
    config.serial = value.c_str();
  }
  if (prefs_.isKey("baud")) {
    config.baud = prefs_.getUInt("baud", config.baud);
  }
  if (prefs_.isKey("flow")) {
    config.flow = static_cast<console::FlowMode>(
        prefs_.getUChar("flow", static_cast<std::uint8_t>(config.flow)));
  }
  if (prefs_.isKey("tx")) {
    config.txPin = prefs_.getInt("tx", config.txPin);
  }
  if (prefs_.isKey("rx")) {
    config.rxPin = prefs_.getInt("rx", config.rxPin);
  }
  if (prefs_.isKey("cts")) {
    config.ctsPin = prefs_.getInt("cts", config.ctsPin);
  }
  if (prefs_.isKey("led")) {
    config.ledPin = prefs_.getInt("led", config.ledPin);
  }
  if (prefs_.isKey("ledlow")) {
    config.ledActiveLow = prefs_.getBool("ledlow", config.ledActiveLow);
  }
  if (prefs_.isKey("log")) {
    config.log = static_cast<console::LogLevel>(
        prefs_.getUChar("log", static_cast<std::uint8_t>(config.log)));
  }

  return config;
}

void ConfigStore::save(const Config& config) {
  prefs_.putString("name", config.name.c_str());
  prefs_.putString("serial", config.serial.c_str());
  prefs_.putUInt("baud", config.baud);
  prefs_.putUChar("flow", static_cast<std::uint8_t>(config.flow));
  prefs_.putInt("tx", config.txPin);
  prefs_.putInt("rx", config.rxPin);
  prefs_.putInt("cts", config.ctsPin);
  prefs_.putInt("led", config.ledPin);
  prefs_.putBool("ledlow", config.ledActiveLow);
  prefs_.putUChar("log", static_cast<std::uint8_t>(config.log));
}

void ConfigStore::clear() {
  prefs_.clear();
}

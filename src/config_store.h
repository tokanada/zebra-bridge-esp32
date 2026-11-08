#pragma once

#include <Preferences.h>

#include "console.h"

struct Config {
  std::string name;
  std::string serial;
  std::uint32_t baud = 9600;
  console::FlowMode flow = console::FlowMode::XonXoff;
  int txPin = -1;
  int rxPin = -1;
  int ctsPin = -1;
  int ledPin = -1;
  bool ledActiveLow = false;
  console::LogLevel log = console::LogLevel::Info;
};

Config defaultConfig();

class ConfigStore {
 public:
  void begin();
  Config load() const;
  void save(const Config& config);
  void clear();

 private:
  mutable Preferences prefs_;
};

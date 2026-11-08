#pragma once

#include <cstdint>

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

// Build-time configuration baked in by scripts/bridge_config.py. If no
// generated header was available (e.g. the native test environment) these
// report "none" / the empty-config hash, so the firmware behaves as if there
// were no bridge.ini.
std::uint32_t buildConfigHash();
const char* buildConfigSource();

class ConfigStore {
 public:
  // Opens the namespace and discards saved console settings when the
  // build-time config hash differs from the one stored in NVS ("cfghash").
  void begin();
  Config load() const;
  void save(const Config& config);
  // Erases all keys, then restores "cfghash" so a reset does not re-trigger
  // the clear on the next boot.
  void clear();

 private:
  mutable Preferences prefs_;
};

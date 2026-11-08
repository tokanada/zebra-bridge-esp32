#pragma once

// Pure C++17 command parser and validators. This header and console.cpp must
// not include any Arduino headers so they build on the host for unit tests.

#include <cstddef>
#include <cstdint>
#include <string>

namespace console {

constexpr std::size_t kMaxLineLength = 128;
constexpr std::size_t kMaxNameLength = 29;
constexpr std::size_t kMaxSerialLength = 32;

enum class Target : std::uint8_t {
  Esp32 = 0,
  Esp32C3 = 1,
};

enum class Command : std::uint8_t {
  None = 0,
  Help,
  Show,
  SetName,
  SetSerial,
  SetBaud,
  SetFlow,
  SetTx,
  SetRx,
  SetCts,
  SetLed,
  SetLedLow,
  SetLog,
  Test,
  ResetConfig,
  Reboot,
  Invalid,
};

enum class FlowMode : std::uint8_t {
  None = 0,
  XonXoff = 1,
  Cts = 2,
};

enum class LogLevel : std::uint8_t {
  Error = 0,
  Info = 1,
  Debug = 2,
};

struct ParsedCommand {
  Command command = Command::None;
  std::string text;   // name / serial payload
  std::uint32_t number = 0;  // baud
  int gpio = 0;
  FlowMode flow = FlowMode::XonXoff;
  LogLevel log = LogLevel::Info;
  bool flag = false;  // ledlow
  std::string error;
};

// Parses one console line. A trailing CR, LF or CRLF is accepted and the line
// is otherwise trimmed. `target` selects the GPIO validity table.
ParsedCommand parse(const std::string& line, Target target);

bool validateName(const std::string& value, std::string& error);
bool validateSerial(const std::string& value, std::string& error);
bool validateBaud(std::uint32_t baud);
bool parseFlowMode(const std::string& value, FlowMode& out, std::string& error);
bool validateFlow(const std::string& value, FlowMode& out, std::string& error);
bool validateGpio(Target target, int gpio, bool allowDisabled,
                  std::string& error);
bool validateLogLevel(const std::string& value, LogLevel& out,
                      std::string& error);

const char* flowModeName(FlowMode mode);
const char* logLevelName(LogLevel level);

}  // namespace console

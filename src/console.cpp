#include "console.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <vector>

namespace console {
namespace {

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string trim(const std::string& value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
}

std::vector<std::string> splitWhitespace(const std::string& value) {
  std::vector<std::string> tokens;
  std::size_t i = 0;
  while (i < value.size()) {
    while (i < value.size() &&
           std::isspace(static_cast<unsigned char>(value[i]))) {
      ++i;
    }
    const std::size_t start = i;
    while (i < value.size() &&
           !std::isspace(static_cast<unsigned char>(value[i]))) {
      ++i;
    }
    if (i > start) {
      tokens.push_back(value.substr(start, i - start));
    }
  }
  return tokens;
}

// Removes the first `count` whitespace-delimited tokens and returns the
// trimmed remainder (used for free-form name / serial text).
std::string remainderAfterTokens(const std::string& value, std::size_t count) {
  std::size_t i = 0;
  for (std::size_t token = 0; token < count; ++token) {
    while (i < value.size() &&
           std::isspace(static_cast<unsigned char>(value[i]))) {
      ++i;
    }
    while (i < value.size() &&
           !std::isspace(static_cast<unsigned char>(value[i]))) {
      ++i;
    }
  }
  return trim(value.substr(i));
}

bool isPrintableAscii(const std::string& value) {
  for (char c : value) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7e) {
      return false;
    }
  }
  return true;
}

bool parseDecimal(const std::string& value, long& out) {
  if (value.empty()) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0') {
    return false;
  }
  out = parsed;
  return true;
}

bool validBaud(std::uint32_t baud) {
  switch (baud) {
    case 1200:
    case 2400:
    case 4800:
    case 9600:
    case 14400:
    case 19200:
    case 28800:
    case 38400:
    case 57600:
    case 115200:
      return true;
    default:
      return false;
  }
}

bool gpioAllowed(Target target, int gpio) {
  switch (target) {
    case Target::Esp32C3:
      // GPIO0-10 and 18-19 are available; 11-17 are SPI flash.
      // GPIO20/21 (UART0) are deliberately rejected: the ROM bootloader
      // prints on them at reset and the printer would see garbage.
      return (gpio >= 0 && gpio <= 10) || (gpio >= 18 && gpio <= 19);
    case Target::Esp32S3:
      // GPIO19/20 are the native USB OTG host pins wired to the printer;
      // GPIO43/44 are the console UART0; GPIO22-34 do not exist or are flash.
      return (gpio >= 0 && gpio <= 18) || gpio == 21 ||
             (gpio >= 35 && gpio <= 42) || (gpio >= 45 && gpio <= 48);
    case Target::Esp32:
    default:
      // Output-capable GPIOs on the WROOM-32 module. 6-11 are SPI flash and
      // 34-39 are input-only, so they are rejected for TX/RX/CTS/LED use.
      switch (gpio) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 12:
        case 13:
        case 14:
        case 15:
        case 16:
        case 17:
        case 18:
        case 19:
        case 21:
        case 22:
        case 23:
        case 25:
        case 26:
        case 27:
        case 32:
        case 33:
          return true;
        default:
          return false;
      }
  }
}

}  // namespace

bool validateName(const std::string& value, std::string& error) {
  if (value.empty() || value.size() > kMaxNameLength) {
    error = "name must be 1..29 characters";
    return false;
  }
  if (!isPrintableAscii(value)) {
    error = "name must be printable ASCII";
    return false;
  }
  return true;
}

bool validateSerial(const std::string& value, std::string& error) {
  if (value.empty() || value.size() > kMaxSerialLength) {
    error = "serial must be 1..32 characters";
    return false;
  }
  if (!isPrintableAscii(value)) {
    error = "serial must be printable ASCII";
    return false;
  }
  return true;
}

bool validateBaud(std::uint32_t baud) { return validBaud(baud); }

bool parseFlowMode(const std::string& value, FlowMode& out, std::string& error) {
  const std::string lowered = toLower(value);
  if (lowered == "none") {
    out = FlowMode::None;
    return true;
  }
  if (lowered == "xonxoff") {
    out = FlowMode::XonXoff;
    return true;
  }
  if (lowered == "cts") {
    out = FlowMode::Cts;
    return true;
  }
  error = "flow must be one of none, xonxoff, cts";
  return false;
}

bool validateFlow(const std::string& value, FlowMode& out, std::string& error) {
  return parseFlowMode(value, out, error);
}

bool validateGpio(Target target, int gpio, bool allowDisabled,
                  std::string& error) {
  if (gpio == -1) {
    if (allowDisabled) {
      return true;
    }
    error = "this pin cannot be disabled (-1)";
    return false;
  }
  if (!gpioAllowed(target, gpio)) {
    error = "GPIO is not valid for this target";
    return false;
  }
  return true;
}

bool validateLogLevel(const std::string& value, LogLevel& out,
                      std::string& error) {
  const std::string lowered = toLower(value);
  if (lowered == "error") {
    out = LogLevel::Error;
    return true;
  }
  if (lowered == "info") {
    out = LogLevel::Info;
    return true;
  }
  if (lowered == "debug") {
    out = LogLevel::Debug;
    return true;
  }
  error = "log must be one of error, info, debug";
  return false;
}

const char* flowModeName(FlowMode mode) {
  switch (mode) {
    case FlowMode::None:
      return "none";
    case FlowMode::Cts:
      return "cts";
    case FlowMode::XonXoff:
    default:
      return "xonxoff";
  }
}

const char* logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Error:
      return "error";
    case LogLevel::Debug:
      return "debug";
    case LogLevel::Info:
    default:
      return "info";
  }
}

ParsedCommand parse(const std::string& line, Target target) {
  ParsedCommand result;

  // Strip trailing CR / LF / CRLF only; embedded terminators are invalid.
  std::string content = line;
  while (!content.empty() &&
         (content.back() == '\r' || content.back() == '\n')) {
    content.pop_back();
  }
  if (content.size() > kMaxLineLength) {
    result.command = Command::Invalid;
    result.error = "line exceeds 128 characters";
    return result;
  }
  if (content.find('\r') != std::string::npos ||
      content.find('\n') != std::string::npos) {
    result.command = Command::Invalid;
    result.error = "embedded line terminator";
    return result;
  }

  content = trim(content);
  if (content.empty()) {
    result.command = Command::None;
    return result;
  }

  const std::vector<std::string> tokens = splitWhitespace(content);
  if (tokens.empty()) {
    result.command = Command::None;
    return result;
  }

  const std::string verb = toLower(tokens[0]);

  if (verb == "help") {
    result.command = Command::Help;
    return result;
  }
  if (verb == "show") {
    result.command = Command::Show;
    return result;
  }
  if (verb == "test") {
    result.command = Command::Test;
    return result;
  }
  if (verb == "reboot") {
    result.command = Command::Reboot;
    return result;
  }
  if (verb == "reset-config") {
    result.command = Command::ResetConfig;
    return result;
  }

  if (verb != "set") {
    result.command = Command::Invalid;
    result.error = "unknown command: " + tokens[0];
    return result;
  }

  if (tokens.size() < 2) {
    result.command = Command::Invalid;
    result.error = "set requires a parameter";
    return result;
  }

  const std::string key = toLower(tokens[1]);

  if (key == "name" || key == "serial") {
    const std::string value = remainderAfterTokens(content, 2);
    std::string error;
    if (key == "name") {
      if (!validateName(value, error)) {
        result.command = Command::Invalid;
        result.error = error;
        return result;
      }
      result.command = Command::SetName;
    } else {
      if (!validateSerial(value, error)) {
        result.command = Command::Invalid;
        result.error = error;
        return result;
      }
      result.command = Command::SetSerial;
    }
    result.text = value;
    return result;
  }

  if (tokens.size() < 3) {
    result.command = Command::Invalid;
    result.error = "set " + tokens[1] + " requires a value";
    return result;
  }

  const std::string value = tokens[2];

  if (key == "baud") {
    long parsed = 0;
    if (!parseDecimal(value, parsed) || parsed < 0 || parsed > 115200 ||
        !validBaud(static_cast<std::uint32_t>(parsed))) {
      result.command = Command::Invalid;
      result.error = "invalid baud rate";
      return result;
    }
    result.command = Command::SetBaud;
    result.number = static_cast<std::uint32_t>(parsed);
    return result;
  }

  if (key == "flow") {
    std::string error;
    if (!parseFlowMode(value, result.flow, error)) {
      result.command = Command::Invalid;
      result.error = error;
      return result;
    }
    result.command = Command::SetFlow;
    return result;
  }

  if (key == "log") {
    std::string error;
    if (!validateLogLevel(value, result.log, error)) {
      result.command = Command::Invalid;
      result.error = error;
      return result;
    }
    result.command = Command::SetLog;
    return result;
  }

  if (key == "ledlow") {
    if (value != "0" && value != "1") {
      result.command = Command::Invalid;
      result.error = "ledlow must be 0 or 1";
      return result;
    }
    result.command = Command::SetLedLow;
    result.flag = value == "1";
    return result;
  }

  if (key == "tx" || key == "rx" || key == "cts" || key == "led") {
    long parsed = 0;
    if (!parseDecimal(value, parsed) || parsed < -1 || parsed > 48) {
      result.command = Command::Invalid;
      result.error = "invalid GPIO number";
      return result;
    }
    const bool allowDisabled =
        (key == "cts" || key == "led");
    std::string error;
    if (!validateGpio(target, static_cast<int>(parsed), allowDisabled, error)) {
      result.command = Command::Invalid;
      result.error = error;
      return result;
    }
    result.gpio = static_cast<int>(parsed);
    if (key == "tx") {
      result.command = Command::SetTx;
    } else if (key == "rx") {
      result.command = Command::SetRx;
    } else if (key == "cts") {
      result.command = Command::SetCts;
    } else {
      result.command = Command::SetLed;
    }
    return result;
  }

  result.command = Command::Invalid;
  result.error = "unknown set parameter: " + tokens[1];
  return result;
}

}  // namespace console

#include <unity.h>

#include <string>

#include "console.h"

namespace {

std::string repeat(char value, std::size_t count) {
  return std::string(count, value);
}

void test_simple_commands() {
  using namespace console;

  TEST_ASSERT_EQUAL(static_cast<int>(Command::Help),
                    static_cast<int>(parse("help", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Show),
                    static_cast<int>(parse("show", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Test),
                    static_cast<int>(parse("test", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::ResetConfig),
                    static_cast<int>(parse("reset-config", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Reboot),
                    static_cast<int>(parse("reboot", Target::Esp32).command));
}

void test_set_name_and_serial() {
  using namespace console;

  ParsedCommand command = parse("set name Zebra Printer", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetName),
                    static_cast<int>(command.command));
  TEST_ASSERT_EQUAL_STRING("Zebra Printer", command.text.c_str());

  command = parse("set serial ZD421-EMULATION-0001", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetSerial),
                    static_cast<int>(command.command));
  TEST_ASSERT_EQUAL_STRING("ZD421-EMULATION-0001", command.text.c_str());
}

void test_set_numeric_and_enums() {
  using namespace console;

  ParsedCommand command = parse("set baud 115200", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetBaud),
                    static_cast<int>(command.command));
  TEST_ASSERT_EQUAL_UINT32(115200, command.number);

  command = parse("set flow cts", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetFlow),
                    static_cast<int>(command.command));
  TEST_ASSERT_EQUAL(static_cast<int>(FlowMode::Cts),
                    static_cast<int>(command.flow));

  command = parse("set log debug", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetLog),
                    static_cast<int>(command.command));
  TEST_ASSERT_EQUAL(static_cast<int>(LogLevel::Debug),
                    static_cast<int>(command.log));

  command = parse("set ledlow 1", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetLedLow),
                    static_cast<int>(command.command));
  TEST_ASSERT_TRUE(command.flag);

  command = parse("set ledlow 0", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetLedLow),
                    static_cast<int>(command.command));
  TEST_ASSERT_FALSE(command.flag);
}

void test_set_gpio() {
  using namespace console;

  ParsedCommand command = parse("set tx 17", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetTx),
                    static_cast<int>(command.command));
  TEST_ASSERT_EQUAL_INT(17, command.gpio);

  command = parse("set rx 16", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetRx),
                    static_cast<int>(command.command));
  TEST_ASSERT_EQUAL_INT(16, command.gpio);

  command = parse("set cts -1", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetCts),
                    static_cast<int>(command.command));
  TEST_ASSERT_EQUAL_INT(-1, command.gpio);

  command = parse("set led 2", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetLed),
                    static_cast<int>(command.command));
  TEST_ASSERT_EQUAL_INT(2, command.gpio);

  command = parse("set led -1", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetLed),
                    static_cast<int>(command.command));
}

void test_invalid_values_rejected() {
  using namespace console;

  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set baud 12345", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set baud 4294976896", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set baud abc", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set flow magic", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set log loud", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set ledlow 2", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set tx -1", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set name", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("bogus", Target::Esp32).command));
}

void test_name_and_serial_lengths() {
  using namespace console;

  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetName),
                    static_cast<int>(parse("set name " + repeat('a', 29),
                                           Target::Esp32)
                                         .command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set name " + repeat('a', 30),
                                           Target::Esp32)
                                         .command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetSerial),
                    static_cast<int>(parse("set serial " + repeat('b', 32),
                                           Target::Esp32)
                                         .command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set serial " + repeat('b', 33),
                                           Target::Esp32)
                                         .command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set name caf\xc3\xa9", Target::Esp32).command));
}

void test_gpio_per_target() {
  using namespace console;

  // WROOM: GPIO17 is valid, input-only GPIO34 and flash GPIO6 are not.
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetTx),
                    static_cast<int>(parse("set tx 17", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set tx 34", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set tx 6", Target::Esp32).command));

  // C3: GPIO5 is valid, GPIO20/21 (UART0) and flash GPIO12 are not.
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetTx),
                    static_cast<int>(parse("set tx 5", Target::Esp32C3).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set tx 20", Target::Esp32C3).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set tx 21", Target::Esp32C3).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set tx 12", Target::Esp32C3).command));

  // S3 USB build: GPIO48 (onboard WS2812) is valid; GPIO19/20 (USB), 43/44
  // (console UART0) and 26 (not present / flash) are not.
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetLed),
                    static_cast<int>(parse("set led 48", Target::Esp32S3).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set led 19", Target::Esp32S3).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set led 20", Target::Esp32S3).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set led 43", Target::Esp32S3).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set led 44", Target::Esp32S3).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set tx 26", Target::Esp32S3).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetTx),
                    static_cast<int>(parse("set tx 21", Target::Esp32S3).command));
}

void test_line_terminators() {
  using namespace console;

  TEST_ASSERT_EQUAL(static_cast<int>(Command::Show),
                    static_cast<int>(parse("show\r\n", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Show),
                    static_cast<int>(parse("show\r", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Show),
                    static_cast<int>(parse("show\n", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Show),
                    static_cast<int>(parse("  show  ", Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("sh\row", Target::Esp32).command));
}

void test_case_insensitivity() {
  using namespace console;

  TEST_ASSERT_EQUAL(static_cast<int>(Command::Help),
                    static_cast<int>(parse("HELP", Target::Esp32).command));
  ParsedCommand command = parse("SeT NaMe MyPrinter", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(Command::SetName),
                    static_cast<int>(command.command));
  TEST_ASSERT_EQUAL_STRING("MyPrinter", command.text.c_str());

  command = parse("SET FLOW XONXOFF", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(FlowMode::XonXoff),
                    static_cast<int>(command.flow));

  command = parse("SET LOG Error", Target::Esp32);
  TEST_ASSERT_EQUAL(static_cast<int>(LogLevel::Error),
                    static_cast<int>(command.log));
}

void test_overlong_line_rejected() {
  using namespace console;

  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse(repeat('x', 129), Target::Esp32).command));
  TEST_ASSERT_EQUAL(static_cast<int>(Command::Invalid),
                    static_cast<int>(parse("set name " + repeat('a', 120),
                                           Target::Esp32)
                                         .command));
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_simple_commands);
  RUN_TEST(test_set_name_and_serial);
  RUN_TEST(test_set_numeric_and_enums);
  RUN_TEST(test_set_gpio);
  RUN_TEST(test_invalid_values_rejected);
  RUN_TEST(test_name_and_serial_lengths);
  RUN_TEST(test_gpio_per_target);
  RUN_TEST(test_line_terminators);
  RUN_TEST(test_case_insensitivity);
  RUN_TEST(test_overlong_line_rejected);
  return UNITY_END();
}

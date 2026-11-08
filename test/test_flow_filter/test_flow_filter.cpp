#include <unity.h>

#include <cstdint>

#include "flow_filter.h"

namespace {

void runFilter(flow_filter::Filter& filter, const std::uint8_t* input,
               std::size_t length, const std::uint8_t* expected,
               std::size_t expectedLength) {
  std::uint8_t output[64] = {};
  const std::size_t produced =
      filter.process(input, length, output, sizeof(output));
  TEST_ASSERT_EQUAL_UINT32(expectedLength, produced);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, output, expectedLength);
}

void test_passthrough_without_flow_control_chars() {
  flow_filter::Filter filter(true);
  const std::uint8_t input[] = {'A', 'B', 'C', 0x00, 0x7f, 0xff};
  runFilter(filter, input, sizeof(input), input, sizeof(input));
  TEST_ASSERT_FALSE(filter.paused());
}

void test_xoff_and_xon_stripped_and_state_toggled() {
  flow_filter::Filter filter(true);
  const std::uint8_t input[] = {'A', 0x13, 'B', 0x11, 'C'};
  const std::uint8_t expected[] = {'A', 'B', 'C'};
  runFilter(filter, input, sizeof(input), expected, sizeof(expected));
  TEST_ASSERT_FALSE(filter.paused());

  const std::uint8_t xoff[] = {'X', 0x13};
  const std::uint8_t onlyX[] = {'X'};
  runFilter(filter, xoff, sizeof(xoff), onlyX, sizeof(onlyX));
  TEST_ASSERT_TRUE(filter.paused());

  const std::uint8_t xon[] = {0x11, 'Y'};
  const std::uint8_t onlyY[] = {'Y'};
  runFilter(filter, xon, sizeof(xon), onlyY, sizeof(onlyY));
  TEST_ASSERT_FALSE(filter.paused());
}

void test_filter_disabled_passes_everything() {
  flow_filter::Filter filter(false);
  const std::uint8_t input[] = {0x11, 'A', 0x13, 'B'};
  runFilter(filter, input, sizeof(input), input, sizeof(input));
  TEST_ASSERT_FALSE(filter.paused());
}

void test_disabling_clears_paused_state() {
  flow_filter::Filter filter(true);
  const std::uint8_t xoff[] = {0x13};
  std::uint8_t output[8] = {};
  filter.process(xoff, sizeof(xoff), output, sizeof(output));
  TEST_ASSERT_TRUE(filter.paused());

  filter.setEnabled(false);
  TEST_ASSERT_FALSE(filter.paused());

  const std::uint8_t input[] = {'A', 0x13, 'B'};
  runFilter(filter, input, sizeof(input), input, sizeof(input));
  TEST_ASSERT_FALSE(filter.paused());
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_passthrough_without_flow_control_chars);
  RUN_TEST(test_xoff_and_xon_stripped_and_state_toggled);
  RUN_TEST(test_filter_disabled_passes_everything);
  RUN_TEST(test_disabling_clears_paused_state);
  return UNITY_END();
}

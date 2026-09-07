// Host test for the PORTABLE sole-worker scheduling policy, run
// under `pio test -e native`. Exercises the round-robin loop selection, the
// wrap-safe heartbeat-due timer, and the single-spec SpecProvider match decision
// — all pure logic, no Arduino, no core.
#include <unity.h>

#include "sole_worker_policy.h"

using mwf::sole::heartbeatDue;
using mwf::sole::RoundRobin;
using mwf::sole::specMatches;

// Round-robin starts on the workflow loop, then strictly alternates.
void test_round_robin_starts_workflow_then_alternates() {
  RoundRobin rr;
  TEST_ASSERT_TRUE(rr.nextIsWorkflow());   // tick 0: workflow
  TEST_ASSERT_FALSE(rr.nextIsWorkflow());  // tick 1: activity
  TEST_ASSERT_TRUE(rr.nextIsWorkflow());   // tick 2: workflow
  TEST_ASSERT_FALSE(rr.nextIsWorkflow());  // tick 3: activity
}

// Over a full WFT1→activity→WFT2 turn (say 4 ticks) each loop is polled twice.
void test_round_robin_balances_both_loops() {
  RoundRobin rr;
  int wf = 0, act = 0;
  for (int i = 0; i < 6; ++i) (rr.nextIsWorkflow() ? wf : act) += 1;
  TEST_ASSERT_EQUAL_INT(3, wf);
  TEST_ASSERT_EQUAL_INT(3, act);
}

// Heartbeat fires only once the interval has fully elapsed.
void test_heartbeat_due_boundary() {
  TEST_ASSERT_FALSE(heartbeatDue(/*now=*/9999, /*last=*/0, /*interval=*/10000));
  TEST_ASSERT_TRUE(heartbeatDue(/*now=*/10000, /*last=*/0, /*interval=*/10000));
  TEST_ASSERT_TRUE(heartbeatDue(/*now=*/25000, /*last=*/0, /*interval=*/10000));
}

// millis() wraparound: now has wrapped past 0 but the elapsed delta is small, so
// the heartbeat is NOT spuriously due; once the real interval passes, it is.
void test_heartbeat_due_wraparound() {
  const uint32_t last = 0xFFFFFFF0u;          // ~16 ms before wrap
  TEST_ASSERT_FALSE(heartbeatDue(/*now=*/0x00000005u, last, 10000));  // ~21 ms
  TEST_ASSERT_TRUE(heartbeatDue(/*now=*/last + 10000u, last, 10000)); // exactly due
}

// The single-spec SpecProvider: matches only the exact registered name.
void test_spec_matches_exact_name_only() {
  TEST_ASSERT_TRUE(specMatches("mwf_device_sole", "mwf_device_sole"));
  TEST_ASSERT_FALSE(specMatches("SomeOtherWorkflow", "mwf_device_sole"));
  TEST_ASSERT_FALSE(specMatches("mwf_device_sole", ""));  // unparsed spec => no match
  TEST_ASSERT_FALSE(specMatches("", "mwf_device_sole"));
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_round_robin_starts_workflow_then_alternates);
  RUN_TEST(test_round_robin_balances_both_loops);
  RUN_TEST(test_heartbeat_due_boundary);
  RUN_TEST(test_heartbeat_due_wraparound);
  RUN_TEST(test_spec_matches_exact_name_only);
  return UNITY_END();
}

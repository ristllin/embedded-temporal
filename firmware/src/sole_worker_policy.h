// firmware: sole-worker scheduling policy — the small PORTABLE decisions that
// glue the two loops together in main.cpp, factored out so they are host-testable
// without Arduino / core (see test/test_sole_worker_policy). Header-only.
//
//   * RoundRobin   — which loop to poll this tick (alternates workflow/activity).
//   * heartbeatDue — millis-safe "is the re-register due?" (wrap-tolerant).
//   * specMatches  — does an incoming workflow_type resolve to our one spec?
#pragma once
#include <cstdint>
#include <string_view>

namespace mwf {
namespace sole {

// Alternates the two poll loops on the single main task. Starts with the workflow
// loop so a freshly-triggered run's first WFT is picked up promptly.
struct RoundRobin {
  bool workflowNext = true;
  // Returns true when THIS tick should poll the workflow loop, then flips.
  bool nextIsWorkflow() {
    bool v = workflowNext;
    workflowNext = !workflowNext;
    return v;
  }
};

// True once `interval` ms have elapsed since `last` (by millis()). Unsigned
// subtraction makes it correct across the ~49-day millis() wraparound.
inline bool heartbeatDue(uint32_t now, uint32_t last, uint32_t interval) {
  return (now - last) >= interval;
}

// The SpecProvider decision: our device hosts exactly ONE workflow spec, so an
// incoming workflow_type resolves iff it equals that spec's name (and the name is
// non-empty — an unparsed/absent spec resolves nothing).
inline bool specMatches(std::string_view workflowType, std::string_view specName) {
  return !specName.empty() && workflowType == specName;
}

}  // namespace sole
}  // namespace mwf

// core: retry policy — backoff/attempt bookkeeping for activity execution
// and transport re-polls. Mirrors Temporal's RetryPolicy semantics.
#pragma once
#include <cstdint>

namespace mwf_core {

// Temporal-shaped retry policy (initial interval, backoff coefficient, max
// interval, max attempts). Portable + host-tested; no wall-clock sleeping here —
// the caller applies the returned delay against its own IClock.
struct RetryPolicy {
  uint32_t initial_interval_ms = 1000;
  double   backoff_coefficient = 2.0;
  uint32_t max_interval_ms     = 100000;
  uint32_t max_attempts        = 0;  // 0 = unlimited
};

// Returns the delay in ms before attempt `attempt` (1-based): initial interval
// times backoff^(attempt-1), saturating at max_interval_ms. Whether another
// attempt is allowed at all is shouldRetry's job — check it first.
uint32_t nextBackoffMs(const RetryPolicy& policy, uint32_t attempt);

// True if another attempt is permitted under the policy.
bool shouldRetry(const RetryPolicy& policy, uint32_t attempt);

}  // namespace mwf_core

// core: retry policy — Temporal-shaped backoff bookkeeping.
#include "mwf_core/retry.h"

namespace mwf_core {

uint32_t nextBackoffMs(const RetryPolicy& policy, uint32_t attempt) {
  if (attempt <= 1) return policy.initial_interval_ms;
  // initial * backoff^(attempt-1), capped at max_interval; computed in double
  // so a large attempt count saturates instead of overflowing.
  double d = static_cast<double>(policy.initial_interval_ms);
  const double cap = static_cast<double>(policy.max_interval_ms);
  for (uint32_t i = 1; i < attempt; ++i) {
    d *= policy.backoff_coefficient;
    if (d >= cap) return policy.max_interval_ms;
  }
  if (d >= cap) return policy.max_interval_ms;
  return static_cast<uint32_t>(d);
}

bool shouldRetry(const RetryPolicy& policy, uint32_t attempt) {
  return policy.max_attempts == 0 || attempt < policy.max_attempts;
}

}  // namespace mwf_core

// core/test: ActivityRegistry (CONTRACTS.md §7) + retry policy.
#include <gtest/gtest.h>

#include "mwf_core/activity_registry.h"
#include "mwf_core/retry.h"

namespace {

mwf::Bytes bytesOf(const char* s) {
  return mwf::Bytes(s, s + std::string(s).size());
}
std::string strOf(const mwf::Bytes& b) {
  return std::string(b.begin(), b.end());
}

}  // namespace

TEST(ActivityRegistry, RegisterHasInvoke) {
  mwf_core::ActivityRegistry reg;
  EXPECT_FALSE(reg.has("echo"));

  reg.registerActivity("echo", [](const mwf::Bytes& arg) {
    return mwf::Result<mwf::Bytes>::success(arg);
  });
  EXPECT_TRUE(reg.has("echo"));

  auto r = reg.invoke("echo", bytesOf("\"hi\""));
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(strOf(r.value), "\"hi\"");
}

TEST(ActivityRegistry, UnknownActivityFailsCleanly) {
  mwf_core::ActivityRegistry reg;
  auto r = reg.invoke("nope", bytesOf("1"));
  ASSERT_FALSE(r.ok);
  EXPECT_NE(r.error.find("nope"), std::string::npos);
}

TEST(ActivityRegistry, HandlerFailurePropagates) {
  mwf_core::ActivityRegistry reg;
  reg.registerActivity("boom", [](const mwf::Bytes&) {
    return mwf::Result<mwf::Bytes>::failure("kaput");
  });
  auto r = reg.invoke("boom", bytesOf("1"));
  ASSERT_FALSE(r.ok);
  EXPECT_EQ(r.error, "kaput");
}

TEST(ActivityRegistry, ReRegisterReplacesHandler) {
  mwf_core::ActivityRegistry reg;
  reg.registerActivity("f", [](const mwf::Bytes&) {
    return mwf::Result<mwf::Bytes>::success(bytesOf("1"));
  });
  reg.registerActivity("f", [](const mwf::Bytes&) {
    return mwf::Result<mwf::Bytes>::success(bytesOf("2"));
  });
  auto r = reg.invoke("f", bytesOf("null"));
  ASSERT_TRUE(r.ok);
  EXPECT_EQ(strOf(r.value), "2");
}

TEST(Retry, BackoffGrowsAndCaps) {
  mwf_core::RetryPolicy p;  // 1000ms, x2, cap 100000ms
  EXPECT_EQ(mwf_core::nextBackoffMs(p, 0), 1000u);
  EXPECT_EQ(mwf_core::nextBackoffMs(p, 1), 1000u);
  EXPECT_EQ(mwf_core::nextBackoffMs(p, 2), 2000u);
  EXPECT_EQ(mwf_core::nextBackoffMs(p, 5), 16000u);
  EXPECT_EQ(mwf_core::nextBackoffMs(p, 8), 100000u);   // capped
  EXPECT_EQ(mwf_core::nextBackoffMs(p, 200), 100000u); // no overflow
}

TEST(Retry, MaxAttempts) {
  mwf_core::RetryPolicy p;
  p.max_attempts = 3;
  EXPECT_TRUE(mwf_core::shouldRetry(p, 1));
  EXPECT_TRUE(mwf_core::shouldRetry(p, 2));
  EXPECT_FALSE(mwf_core::shouldRetry(p, 3));
  p.max_attempts = 0;  // unlimited
  EXPECT_TRUE(mwf_core::shouldRetry(p, 1000));
}

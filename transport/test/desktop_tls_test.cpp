// transport/test: server-free behavior checks for DesktopTransport's TLS
// posture. No gtest — a self-contained pass/fail binary (non-zero exit on any
// failure) registered with ctest, matching h2_security_test. Verifies the
// SECURITY.md desktop guarantee at the seam:
//   1. An insecure (tls=false) channel REFUSES to send a constructor bearer:
//      call() fails FAILED_PRECONDITION before any network I/O.
//   2. Same refusal for caller-supplied authorization metadata.
//   3. Without credentials the insecure channel is allowed (local dev server
//      case) — the refusal is credential-gated, not blanket.
//   4. The default-constructed transport is TLS-on: with a bearer it does NOT
//      refuse, it attempts the (validating) TLS connection.
#include <iostream>
#include <string>

#include "desktop_transport.h"

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  std::cout << (cond ? "  ok   " : "  FAIL ") << what << "\n";
  if (!cond) ++g_failures;
}

constexpr int kFailedPrecondition = 9;  // grpc::StatusCode::FAILED_PRECONDITION

// A port nothing listens on: connection attempts fail fast and locally.
const char* kDeadTarget = "127.0.0.1:1";

}  // namespace

int main() {
  std::cout << "[tls] DesktopTransport credential/TLS posture\n";

  // 1. Insecure channel + constructor bearer ⇒ refuse, no send.
  {
    mwf_transport::DesktopTransport t(kDeadTarget, "sekret-key", /*tls=*/false);
    auto r = t.call("/x.Y/Z", {}, {}, /*deadlineMs=*/2000);
    check(r.grpc_status == kFailedPrecondition,
          "insecure + ctor bearer -> FAILED_PRECONDITION");
    check(r.message.find("insecure") != std::string::npos,
          "refusal message names the insecure channel");
  }

  // 2. Insecure channel + caller-supplied authorization metadata ⇒ refuse.
  {
    mwf_transport::DesktopTransport t(kDeadTarget, "", /*tls=*/false);
    mwf::Metadata md;
    md["authorization"] = "Bearer sekret-key";
    auto r = t.call("/x.Y/Z", {}, md, /*deadlineMs=*/2000);
    check(r.grpc_status == kFailedPrecondition,
          "insecure + caller authorization -> FAILED_PRECONDITION");
  }

  // 3. Insecure channel, no credentials ⇒ allowed (fails on the dead target
  //    with a transport-level status, never the refusal).
  {
    mwf_transport::DesktopTransport t(kDeadTarget, "", /*tls=*/false);
    auto r = t.call("/x.Y/Z", {}, {}, /*deadlineMs=*/2000);
    check(r.grpc_status != kFailedPrecondition,
          "insecure without credentials is not refused (status="
              + std::to_string(r.grpc_status) + ")");
  }

  // 4. Default = TLS-on: a bearer is accepted and the call attempts the
  //    validating TLS connection (dead target ⇒ transport error, not refusal).
  {
    mwf_transport::DesktopTransport t(kDeadTarget, "sekret-key");
    auto r = t.call("/x.Y/Z", {}, {}, /*deadlineMs=*/2000);
    check(r.grpc_status != kFailedPrecondition,
          "default TLS-on + bearer attempts the call (status="
              + std::to_string(r.grpc_status) + ")");
  }

  if (g_failures) {
    std::cout << "[tls] FAILED: " << g_failures << " check(s)\n";
    return 1;
  }
  std::cout << "[tls] all checks passed\n";
  return 0;
}

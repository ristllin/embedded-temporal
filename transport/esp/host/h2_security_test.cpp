// transport esp/host/: host-twin unit checks for the transport + JSON hardening
// (security review). No live server, no gtest — a self-contained pass/fail
// binary (returns non-zero on any failure) so it drops into ctest like the
// example probes. Exercises the EXACT code the ESP32 links:
//   1. GrpcH2Session::unwrapUnaryFrame — the gRPC frame-length parse. The star
//      case is an attacker-supplied length prefix of 0xFFFFFFFF: the old
//      `size() < 5 + mlen` guard wrapped in 32-bit and let an ~4 GB OOB read
//      through. It must now reject cleanly as "truncated", never crash.
//   2. mwf_core::jsonutil::parse — deep-nesting guard on server-sourced JSON.
#include <cstdint>
#include <iostream>
#include <string>

#include "../grpc_h2.h"
#include "mwf_core/detail/json_util.h"

using mwf::Bytes;
using mwf_transport::GrpcH2Session;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  std::cout << (cond ? "  ok   " : "  FAIL ") << what << "\n";
  if (!cond) ++g_failures;
}

// Build a gRPC frame: 1-byte flag + 4-byte BE length prefix + `payload`. The
// prefix is set explicitly (may deliberately disagree with payload.size()).
Bytes frame(uint8_t flag, uint32_t lenPrefix, const Bytes& payload) {
  Bytes b;
  b.push_back(flag);
  b.push_back(static_cast<uint8_t>(lenPrefix >> 24));
  b.push_back(static_cast<uint8_t>(lenPrefix >> 16));
  b.push_back(static_cast<uint8_t>(lenPrefix >> 8));
  b.push_back(static_cast<uint8_t>(lenPrefix));
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}

void testFrameParse() {
  std::cout << "[frame] GrpcH2Session::unwrapUnaryFrame\n";
  constexpr int kOk = 0, kInternal = 13;

  // Empty body (trailers-only OK) ⇒ empty message, status OK.
  {
    Bytes out{0xAB};  // pre-dirtied; must be left untouched / cleared to empty
    std::string msg;
    int st = GrpcH2Session::unwrapUnaryFrame(Bytes{}, out, msg);
    check(st == kOk, "empty frame -> OK");
  }

  // Short frame (< 5 bytes) ⇒ INTERNAL.
  {
    Bytes out;
    std::string msg;
    int st = GrpcH2Session::unwrapUnaryFrame(Bytes{0, 0, 3}, out, msg);
    check(st == kInternal && msg.rfind("short", 0) == 0, "3-byte frame -> short/INTERNAL");
  }

  // Compressed flag (nonzero) ⇒ INTERNAL.
  {
    Bytes out;
    std::string msg;
    int st = GrpcH2Session::unwrapUnaryFrame(frame(1, 0, {}), out, msg);
    check(st == kInternal && msg.find("compressed") != std::string::npos,
          "compressed flag -> INTERNAL");
  }

  // ★ THE overflow case: length prefix 0xFFFFFFFF over a 5-byte frame. `5+mlen`
  //   wraps to 4 in 32-bit; the fixed guard compares mlen against size()-5 and
  //   rejects. Must be a clean "truncated", never an OOB assign / crash.
  {
    Bytes out;
    std::string msg;
    Bytes f = frame(0, 0xFFFFFFFFu, {});  // header only, no payload bytes
    int st = GrpcH2Session::unwrapUnaryFrame(f, out, msg);
    check(st == kInternal && msg.find("truncated") != std::string::npos,
          "0xFFFFFFFF length prefix -> truncated (no OOB, no crash)");
    check(out.empty(), "0xFFFFFFFF case leaves output empty");
  }

  // Length prefix 0xFFFFFFFF over a frame that also carries some payload bytes —
  // still must reject (the wrap must not depend on payload size).
  {
    Bytes out;
    std::string msg;
    Bytes f = frame(0, 0xFFFFFFFFu, Bytes(32, 0x7E));
    int st = GrpcH2Session::unwrapUnaryFrame(f, out, msg);
    check(st == kInternal && msg.find("truncated") != std::string::npos,
          "0xFFFFFFFF prefix + payload -> truncated");
  }

  // Prefix one byte past what was received ⇒ truncated.
  {
    Bytes out;
    std::string msg;
    Bytes f = frame(0, 4, Bytes{1, 2, 3});  // says 4, only 3 present
    int st = GrpcH2Session::unwrapUnaryFrame(f, out, msg);
    check(st == kInternal && msg.find("truncated") != std::string::npos,
          "prefix past end -> truncated");
  }

  // Exact-fit valid frame ⇒ OK with the exact message bytes.
  {
    Bytes payload{0xDE, 0xAD, 0xBE, 0xEF};
    Bytes out;
    std::string msg;
    int st = GrpcH2Session::unwrapUnaryFrame(frame(0, 4, payload), out, msg);
    check(st == kOk && out == payload, "exact-fit frame -> OK + correct bytes");
  }

  // Prefix shorter than the bytes present (trailing extra) ⇒ OK, only the
  // declared prefix is returned (in-bounds, no over-read).
  {
    Bytes out;
    std::string msg;
    Bytes f = frame(0, 2, Bytes{0xA1, 0xA2, 0xA3, 0xA4});
    int st = GrpcH2Session::unwrapUnaryFrame(f, out, msg);
    check(st == kOk && out == Bytes({0xA1, 0xA2}), "short prefix -> OK + first N bytes");
  }

  // Zero-length message frame ⇒ OK, empty message.
  {
    Bytes out{0x11};
    std::string msg;
    int st = GrpcH2Session::unwrapUnaryFrame(frame(0, 0, {}), out, msg);
    check(st == kOk && out.empty(), "0-length message frame -> OK + empty");
  }
}

std::string nested(int depth, char open, char close) {
  return std::string(static_cast<size_t>(depth), open) +
         std::string(static_cast<size_t>(depth), close);
}

void testJsonDepth() {
  std::cout << "[json] mwf_core::jsonutil::parse depth guard\n";
  using mwf_core::jsonutil::parse;

  check(parse(std::string_view("{\"a\":[1,2,3],\"b\":{\"c\":true}}")).ok,
        "valid shallow object -> ok");

  // Deeply nested arrays: must fail cleanly (no stack overflow / crash).
  {
    std::string deep = nested(4096, '[', ']');
    auto r = parse(std::string_view(deep));
    check(!r.ok && r.error.find("deep") != std::string::npos,
          "4096-deep array -> clean 'too deep' failure");
  }

  // Nested objects too.
  {
    std::string deep;
    for (int i = 0; i < 500; ++i) deep += "{\"k\":";
    deep += "1";
    for (int i = 0; i < 500; ++i) deep += "}";
    auto r = parse(std::string_view(deep));
    check(!r.ok, "500-deep object -> failure");
  }

  // At the limit (64) must still parse; just past it must fail.
  check(parse(std::string_view(nested(64, '[', ']'))).ok, "64-deep (at limit) -> ok");
  check(!parse(std::string_view(nested(65, '[', ']'))).ok, "65-deep (past limit) -> fail");

  // Bracket characters INSIDE a string literal must not be miscounted as depth.
  {
    std::string s = "{\"k\":\"";
    s += std::string(300, '[');  // 300 '[' chars, but inside a JSON string
    s += "\"}";
    check(parse(std::string_view(s)).ok, "brackets inside string not counted -> ok");
  }
}

}  // namespace

int main() {
  testFrameParse();
  testJsonDepth();
  std::cout << (g_failures ? "\nFAILURES: " + std::to_string(g_failures) + "\n"
                           : "\nall security-fix checks passed\n");
  return g_failures ? 1 : 0;
}

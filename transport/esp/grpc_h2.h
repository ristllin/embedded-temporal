// transport esp/: portable gRPC-over-HTTP/2 core.
//
// The nghttp2 session driver + gRPC framing, written against ABSTRACT I/O
// callbacks (send / recv / now-ms) so the SAME code runs on:
//   - the host twin  (POSIX TCP socket, optionally OpenSSL TLS)  — esp/host/
//   - the ESP32-S3   (WiFiClientSecure = mbedTLS on lwIP, PSRAM-routed)
//
// No Arduino, no sockets, no TLS here — only nghttp2 (vendored under
// esp/third_party/nghttp2) + the mwf contract types.
//
// Model: ONE in-flight unary call at a time (matches the worker's single-poll
// pattern). Open session → submit request (headers + 5-byte gRPC frame + proto
// bytes) → pump until the response completes (DATA + trailers) → extract
// grpc-status → keep the session alive for the next call (h2 connection reuse;
// the owner reconnects on GOAWAY / io error via H2Transport below).
//
// gRPC wire facts this implements (verified against the gRPC-over-HTTP/2 spec):
//   - request  = HTTP/2 POST to the full method path;
//     headers: content-type: application/grpc, te: trailers, grpc-timeout,
//     plus caller metadata (authorization: Bearer …, temporal-namespace: …).
//   - body     = 1-byte compressed flag (0) + 4-byte BE length + protobuf.
//   - response = same framing; grpc-status / grpc-message arrive in HTTP/2
//     TRAILERS — or in the initial HEADERS on a trailers-only error response.
//   - grpc-message is percent-encoded (decoded here).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "mwf/contracts.h"

// Forward-declare nghttp2 internals so this header stays nghttp2-free for
// consumers (only grpc_h2.cpp includes <nghttp2/nghttp2.h>).
struct nghttp2_session;

namespace mwf_transport {

// ── The abstract byte-stream seam ────────────────────────────────────────────
// An ALREADY-CONNECTED bidirectional stream (TLS with ALPN "h2" negotiated, or
// a plaintext TCP socket for h2c prior-knowledge against a local dev server).
struct H2IO {
  void* ctx = nullptr;
  // Write up to len bytes. Returns bytes written (>0) or <0 on a fatal error.
  // May write fewer than len; the core loops.
  int (*send)(void* ctx, const uint8_t* data, size_t len) = nullptr;
  // Read up to len bytes. Returns >0 bytes read, 0 = no data within the io's
  // short internal poll window (the core re-checks the deadline and pumps
  // again), <0 = connection closed / fatal error.
  int (*recv)(void* ctx, uint8_t* buf, size_t len) = nullptr;
  // Monotonic milliseconds (host: steady_clock; device: millis()).
  uint64_t (*nowMs)(void* ctx) = nullptr;
};

// Allocation instrumentation (routes nghttp2's allocator through a counter so
// the h2 session footprint is measurable on the host).
struct H2MemStats {
  size_t current = 0;  // live bytes
  size_t peak = 0;     // high-water mark
  size_t total = 0;    // cumulative allocated
  size_t count = 0;    // allocation count
};

// ── The session ──────────────────────────────────────────────────────────────
class GrpcH2Session {
 public:
  GrpcH2Session();
  ~GrpcH2Session();
  GrpcH2Session(const GrpcH2Session&) = delete;
  GrpcH2Session& operator=(const GrpcH2Session&) = delete;

  // Start the h2 client session on a connected stream: sends the client
  // connection preface + SETTINGS. `authority` = host[:port] for :authority;
  // `scheme` = "https" (TLS) or "http" (h2c dev server).
  bool begin(const H2IO& io, std::string authority, std::string scheme);

  // One unary gRPC call. Blocks, pumping the io, until response + trailers or
  // the client deadline. On deadline: RST_STREAM(CANCEL) locally and returns
  // grpc_status 4 (DEADLINE_EXCEEDED) — the session survives for the next call.
  // On io/session failure: returns grpc_status 14 (UNAVAILABLE) and alive()
  // goes false — the owner must reconnect.
  mwf::GrpcResult call(std::string_view fullMethod, const mwf::Bytes& requestMsg,
                       const mwf::Metadata& metadata, int deadlineMs);

  // False after GOAWAY / io error / protocol failure → owner reconnects.
  bool alive() const { return alive_; }

  // Best-effort GOAWAY + free the nghttp2 session. Safe to call repeatedly.
  void shutdown();

  const H2MemStats& memStats() const { return mem_stats_; }

  // grpc-message percent-decoding (exposed for tests).
  static std::string percentDecode(std::string_view in);

  // Unwrap a unary gRPC frame (1-byte flag + 4-byte BE length + message) out of
  // the concatenated response DATA bytes. Returns the gRPC status to surface:
  //   0  (OK)       → `out` holds the message bytes (empty body ⇒ empty message)
  //   13 (INTERNAL) → malformed: short / compressed / truncated (frame length
  //                   exceeds the bytes actually received); `message` set, `out`
  //                   left untouched.
  // The length prefix is attacker-controlled, so the truncation check is done
  // WITHOUT arithmetic on it (see the .cpp). Exposed for host tests.
  static int unwrapUnaryFrame(const mwf::Bytes& frame, mwf::Bytes& out,
                              std::string& message);

 private:
  friend struct H2Callbacks;

  bool pumpSend();                    // flush nghttp2 wants-write via io.send
  bool pumpRecv(uint64_t deadlineAt); // one recv + mem_recv feed
  void fail(const char* why);

  H2IO io_{};
  std::string authority_;
  std::string scheme_ = "https";
  nghttp2_session* session_ = nullptr;
  bool alive_ = false;

  // Per-call state (single in-flight call).
  int32_t stream_id_ = -1;
  bool stream_done_ = false;
  int http_status_ = 0;
  int grpc_status_ = -1;          // -1 = not seen
  std::string grpc_message_;
  mwf::Bytes resp_data_;          // concatenated DATA frames (gRPC-framed)
  mwf::Bytes req_frame_;          // 5-byte prefix + request protobuf
  size_t req_sent_ = 0;
  std::string fail_reason_;

  H2MemStats mem_stats_;
  void* mem_holder_ = nullptr;    // nghttp2_mem storage (opaque here)
};

// ── Reconnecting ITransport base ─────────────────────────────────────────────
// The reconnect/retry policy written once; host and esp subclasses provide the
// stream. openStream() must (re)connect the underlying socket/TLS and fill
// io/authority/scheme; closeStream() tears it down.
class H2Transport : public mwf::ITransport {
 public:
  ~H2Transport() override = default;

  mwf::GrpcResult call(std::string_view fullMethod, const mwf::Bytes& requestMsg,
                       const mwf::Metadata& metadata, int deadlineMs) override;
  void close() override;

  const H2MemStats& memStats() const { return session_.memStats(); }

 protected:
  virtual bool openStream(H2IO& io, std::string& authority, std::string& scheme,
                          std::string& err) = 0;
  virtual void closeStream() = 0;

 private:
  bool ensureSession(std::string& err);

  GrpcH2Session session_;
  bool connected_ = false;
};

}  // namespace mwf_transport

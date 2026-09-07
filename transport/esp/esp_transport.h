// transport esp/: the ESP32-S3 mwf::ITransport.
//
// gRPC-over-HTTP/2 via the portable grpc_h2 core (vendored nghttp2) on a
// WiFiClientSecure stream — mbedTLS on lwIP, ALPN "h2". Assumes firmware has
// already installed the PSRAM mbedTLS allocator (mwf::installPsramMbedtls())
// so the TLS handshake + h2 session state land in PSRAM, and started the TLS
// arbiter (mwf::arbiter::begin()).
//
// Connection model: ONE persistent h2 connection reused across unary calls
// (poll → respond → poll rides a single TLS session; reconnect on GOAWAY /
// io error / idle server close — H2Transport's policy). The arbiter slot is
// held for the connection's lifetime: the worker is a long-lived TLS consumer
// by design, and 2 slots leave one free for the REST/whoami path.
//
// Long-poll timing: the Mistral frontend holds PollActivityTaskQueue ~45 s
// then answers OK-taskless (empty task_token) — NOT a client deadline. The
// worker's client deadline stays ~70 s (grpc-timeout header); the io recv
// yields (delay + task-watchdog feed) every poll window so a 70 s blocking
// call inside loop() cannot trip the 8 s task watchdog.
//
// TLS posture: setInsecure(), matching the device fleet's current posture —
// a CA bundle is a planned follow-up (see ../SECURITY.md's open TODO; adopt
// both together).
#pragma once

#include <string>

#include "grpc_h2.h"

// Keep Arduino out of consumers that only route bytes. NB: in arduino-esp32
// 3.x `WiFiClientSecure` is a typedef of NetworkClientSecure — forward-declare
// the real class.
class NetworkClientSecure;

namespace mwf_transport {

// Optional TLS-arbiter integration (wired by firmware main to mwf::arbiter::
// acquireWork/releaseWork; a transport→device include would be a cycle). When
// unset, the transport runs unarbitrated.
void espTransportSetArbiterHooks(bool (*acquire)(uint32_t timeoutMs),
                                 void (*release)());

class EspTransport final : public H2Transport {
 public:
  // target = "host:port" (e.g. whoami's scheduler_url, wf-scheduler.mistral.ai:443).
  // bearer, when non-empty, is attached as authorization: Bearer … unless the
  // caller's metadata already carries one (same rule as the desktop twin).
  explicit EspTransport(std::string target, std::string bearer = {});
  ~EspTransport() override;

  mwf::GrpcResult call(std::string_view fullMethod, const mwf::Bytes& requestMsg,
                       const mwf::Metadata& metadata, int deadlineMs) override;

 protected:
  bool openStream(H2IO& io, std::string& authority, std::string& scheme,
                  std::string& err) override;
  void closeStream() override;

 private:
  static int ioSend(void* ctx, const uint8_t* data, size_t len);
  static int ioRecv(void* ctx, uint8_t* buf, size_t len);
  static uint64_t ioNowMs(void* ctx);

  std::string host_;
  int port_ = 443;
  std::string bearer_;
  NetworkClientSecure* client_ = nullptr;  // heap: alive only while connected
  bool holding_slot_ = false;           // TLS arbiter slot held with the conn
};

}  // namespace mwf_transport

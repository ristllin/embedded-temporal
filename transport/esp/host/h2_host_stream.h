// transport esp/host/: the HOST TWIN of the device byte stream — a POSIX
// TCP socket, optionally wrapped in OpenSSL TLS with ALPN "h2". Feeds the SAME
// grpc_h2 core the ESP32 uses (that's the point: the framing layer proven here
// is byte-identical to what runs on the device; only the stream differs).
//
//   - tls=false → plaintext h2c prior-knowledge (local `temporal server
//     start-dev` on :7233 — gRPC always uses prior knowledge, no Upgrade).
//   - tls=true  → OpenSSL, ALPN must negotiate "h2" (wf-scheduler.mistral.ai:443).
//
// recv() semantics per H2IO: >0 bytes, 0 = nothing within the short poll
// window (~100 ms), <0 = closed/error.
#pragma once

#include <cstdint>
#include <string>

#include "../grpc_h2.h"

namespace mwf_transport {

class HostH2Stream {
 public:
  HostH2Stream() = default;
  ~HostH2Stream() { close(); }
  HostH2Stream(const HostH2Stream&) = delete;
  HostH2Stream& operator=(const HostH2Stream&) = delete;

  // Connect TCP (+ TLS handshake with ALPN h2 when tls). err on failure.
  bool connect(const std::string& host, int port, bool tls, std::string& err);
  void close();
  bool connected() const { return fd_ >= 0; }

  // H2IO view over this stream.
  H2IO io();

 private:
  static int ioSend(void* ctx, const uint8_t* data, size_t len);
  static int ioRecv(void* ctx, uint8_t* buf, size_t len);
  static uint64_t ioNowMs(void* ctx);

  int fd_ = -1;
  void* ssl_ = nullptr;      // SSL*      (opaque: keep OpenSSL out of the header)
  void* ssl_ctx_ = nullptr;  // SSL_CTX*
};

// ITransport over the host stream + the shared grpc_h2 core.
class H2HostTransport final : public H2Transport {
 public:
  // target = "host:port". tls: -1 = infer from port 443 (host-twin convenience;
  // the desktop transport itself defaults TLS-on with no inference), 0/1
  // explicit. bearer, when non-empty, is attached as authorization: Bearer …
  // unless the caller's metadata already carries one (same as DesktopTransport).
  H2HostTransport(std::string target, std::string bearer = {}, int tls = -1);

  mwf::GrpcResult call(std::string_view fullMethod, const mwf::Bytes& requestMsg,
                       const mwf::Metadata& metadata, int deadlineMs) override;

 protected:
  bool openStream(H2IO& io, std::string& authority, std::string& scheme,
                  std::string& err) override;
  void closeStream() override;

 private:
  std::string host_;
  int port_ = 443;
  bool tls_ = true;
  std::string bearer_;
  HostH2Stream stream_;
};

}  // namespace mwf_transport

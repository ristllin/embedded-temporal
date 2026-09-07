// transport: DesktopTransport — grpc++ impl of mwf::ITransport.
// The desktop twin of the ESP32 nghttp2+mbedTLS transport; core/worker never
// sees HTTP/2 or protobuf-lib, only the ITransport seam (CONTRACTS.md §1).
//
// Uses grpc++'s GENERIC stub (grpc::GenericStub + grpc::ByteBuffer), NOT a
// generated service stub. This is the natural fit for ITransport::call, whose
// signature is already (fullMethod, request bytes) → (status, response bytes):
// the generic stub sends arbitrary /package.Service/Method unary calls with
// opaque payloads, so this transport needs NO generated proto at all — the
// proto layer's codec (IProtoCodec) produces the request bytes and decodes the
// response bytes on the far side of the seam.
#pragma once
#include <memory>
#include <string>

#include "mwf/contracts.h"

namespace mwf_transport {

// One gRPC channel to the frontend (Mistral/Temporal Cloud on :443 TLS, or a
// local dev frontend insecure). Long-poll aware: DEADLINE_EXCEEDED is normal.
class DesktopTransport : public mwf::ITransport {
 public:
  // target e.g. "wf-scheduler.mistral.ai:443" or "localhost:7233".
  // bearer = API key (empty → no Authorization header, e.g. local dev server).
  // tls defaults ON (certificate-validating SslCredentials); pass tls=false
  // explicitly for a local plaintext dev frontend. On an insecure channel the
  // transport REFUSES to attach any bearer/authorization credential: call()
  // fails instead of leaking the key (SECURITY.md's desktop guarantee).
  DesktopTransport(std::string target, std::string bearer, bool tls = true);
  ~DesktopTransport() override;

  mwf::GrpcResult call(std::string_view fullMethod, const mwf::Bytes& requestMsg,
                       const mwf::Metadata& metadata, int deadlineMs) override;
  void close() override;

 private:
  // grpc++ types (Channel + GenericStub) are held in a pimpl so this header
  // stays free of the grpc include (GenericStub is a typedef and can't be
  // forward-declared). The unique_ptr dtor is emitted in the .cpp where Impl
  // is complete.
  struct Impl;
  std::string target_;
  std::string bearer_;
  bool tls_ = true;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mwf_transport

// transport: DesktopTransport bodies.
// TLS-on by default (certificate-validating SslCredentials + Bearer API key +
// temporal-namespace metadata); insecure only on explicit opt-out, and never
// with credentials attached. Uses the grpc++ GENERIC stub so the transport is
// proto-agnostic — request/response are opaque protobuf bytes produced and
// consumed by IProtoCodec.
#include "desktop_transport.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/slice.h>

#include <chrono>
#include <vector>

namespace mwf_transport {
namespace {

// mwf::Bytes → grpc::ByteBuffer (single slice).
grpc::ByteBuffer toByteBuffer(const mwf::Bytes& b) {
  grpc::Slice slice(b.data(), b.size());
  return grpc::ByteBuffer(&slice, 1);
}

// grpc::ByteBuffer → mwf::Bytes (concatenate all slices).
mwf::Bytes fromByteBuffer(const grpc::ByteBuffer& buf) {
  mwf::Bytes out;
  std::vector<grpc::Slice> slices;
  if (!buf.Dump(&slices).ok()) return out;
  for (const auto& s : slices) out.insert(out.end(), s.begin(), s.end());
  return out;
}

}  // namespace

// Pimpl: owns the grpc channel + generic stub, keeping grpc out of the header.
struct DesktopTransport::Impl {
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<grpc::GenericStub> stub;
};

DesktopTransport::DesktopTransport(std::string target, std::string bearer, bool tls)
    : target_(std::move(target)), bearer_(std::move(bearer)), tls_(tls),
      impl_(std::make_unique<Impl>()) {
  auto creds = tls_ ? grpc::SslCredentials(grpc::SslCredentialsOptions())
                    : grpc::InsecureChannelCredentials();
  impl_->channel = grpc::CreateChannel(target_, creds);
  impl_->stub = std::make_unique<grpc::GenericStub>(impl_->channel);
}

DesktopTransport::~DesktopTransport() { close(); }

mwf::GrpcResult DesktopTransport::call(std::string_view fullMethod,
                                       const mwf::Bytes& requestMsg,
                                       const mwf::Metadata& metadata,
                                       int deadlineMs) {
  mwf::GrpcResult result;
  if (!impl_ || !impl_->stub) {
    result.grpc_status = static_cast<int>(grpc::StatusCode::UNAVAILABLE);
    result.message = "transport closed";
    return result;
  }

  grpc::ClientContext ctx;

  // Auth + routing metadata. Temporal wants authorization: Bearer <key> and
  // temporal-namespace. Caller-supplied metadata wins; fall back to the
  // constructor bearer if the caller didn't set authorization. Credentials
  // never ride a plaintext channel: fail loudly instead.
  bool has_auth = metadata.count("authorization") != 0;
  if (!tls_ && (has_auth || !bearer_.empty())) {
    result.grpc_status = static_cast<int>(grpc::StatusCode::FAILED_PRECONDITION);
    result.message =
        "refusing to send bearer credentials over an insecure (non-TLS) "
        "channel; enable TLS or construct the transport without a key";
    return result;
  }
  for (const auto& kv : metadata) ctx.AddMetadata(kv.first, kv.second);
  if (!has_auth && !bearer_.empty())
    ctx.AddMetadata("authorization", "Bearer " + bearer_);

  // Temporal REQUIRES a deadline on the long-poll (else INVALID_ARGUMENT
  // "Context timeout is not set."). A poll deadline is ~20–70s; that's fine here.
  if (deadlineMs > 0)
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::milliseconds(deadlineMs));

  grpc::ByteBuffer request = toByteBuffer(requestMsg);

  // Generic unary call over a completion queue, driven synchronously: the
  // ITransport seam is blocking by contract (the worker loop owns the thread).
  grpc::CompletionQueue cq;
  grpc::ByteBuffer reply;
  grpc::Status status;
  const std::string method(fullMethod);

  auto reader = impl_->stub->PrepareUnaryCall(&ctx, method, request, &cq);
  reader->StartCall();
  reader->Finish(&reply, &status, reinterpret_cast<void*>(1));

  void* tag = nullptr;
  bool ok = false;
  cq.Next(&tag, &ok);  // blocks until this unary call finishes

  result.grpc_status = static_cast<int>(status.error_code());
  result.message = status.error_message();
  // DEADLINE_EXCEEDED (4) on a poll is a normal empty long-poll: leave response
  // empty and let the worker re-poll. Only populate bytes on OK.
  if (status.ok()) result.response = fromByteBuffer(reply);
  return result;
}

void DesktopTransport::close() { impl_.reset(); }

}  // namespace mwf_transport

// contracts: the seven frozen interfaces. See CONTRACTS.md for the full spec.
#pragma once
#include <functional>
#include "types.h"
#include "temporal_types.h"

namespace mwf {

// ── 1. ITransport ─────────────────────────────────────────────────────────────
struct GrpcResult {
  int         grpc_status = 0;   // 0 OK; 4 DEADLINE_EXCEEDED (empty long-poll, re-poll)
  std::string message;           // grpc-message trailer
  Bytes       response;          // response protobuf bytes; empty on non-OK
};
struct ITransport {
  virtual ~ITransport() = default;
  virtual GrpcResult call(std::string_view fullMethod, const Bytes& requestMsg,
                          const Metadata& metadata, int deadlineMs) = 0;
  virtual void close() = 0;
};

// ── 3. IPayloadCodec (2. IProtoCodec is a generated template, see proto) ───
struct DecodedActivityInput { /*json*/ Bytes argument_json; WorkflowContext context; bool empty = false; };
struct IPayloadCodec {
  virtual ~IPayloadCodec() = default;
  virtual Result<DecodedActivityInput> decodeActivityInput(const temporal::Payload&) = 0;
  virtual temporal::Payload encodeActivityResult(const Bytes& result_json,
                                                 const WorkflowContext& echoCtx, bool empty) = 0;
};

// ── 4. IDurableStore ──────────────────────────────────────────────────────────
struct IDurableStore {
  virtual ~IDurableStore() = default;
  virtual bool put(std::string_view key, const Bytes& value) = 0;
  virtual std::optional<Bytes> get(std::string_view key) = 0;
  virtual bool erase(std::string_view key) = 0;
  virtual std::vector<std::string> keys(std::string_view prefix) = 0;
};

// ── 6. determinism seams ──────────────────────────────────────────────────────
struct IClock  { virtual ~IClock() = default;  virtual uint64_t nowMs() = 0; };
struct IRandom { virtual ~IRandom() = default; virtual uint64_t next()  = 0; };

// ── 7. IActivityRegistry ──────────────────────────────────────────────────────
using ActivityFn = std::function<Result<Bytes>(const Bytes& arg_json)>;  // json-as-bytes across seam
struct IActivityRegistry {
  virtual ~IActivityRegistry() = default;
  virtual void registerActivity(std::string name, ActivityFn) = 0;
  virtual bool has(std::string_view name) = 0;
  virtual Result<Bytes> invoke(std::string_view name, const Bytes& arg_json) = 0;
};

// 5. IWorkflowSpec is a JSON data grammar (see CONTRACTS.md §5), not a vtable —
//    parsed by the replay engine in core. No interface here by design.

}  // namespace mwf

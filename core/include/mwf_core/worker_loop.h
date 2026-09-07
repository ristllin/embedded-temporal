// core: WorkerLoop — the activity worker poll loop.
// Long-polls the activity task queue over ITransport, decodes the task payload
// with the IPayloadCodec, dispatches to the IActivityRegistry, and reports the
// result back. DEADLINE_EXCEEDED on a poll is a normal empty long-poll (idle),
// not an error.
//
// The loop is PORTABLE: it works purely on request/response BYTES plus plain
// value structs. All protobuf knowledge lives behind IWorkerProtoAdapter, whose
// desktop twin is implemented over mwf_proto_host (libprotobuf) and whose
// device twin will be nanopb — the same loop runs on both. No grpc/protobuf
// includes here by design.
#pragma once
#include <string>
#include <vector>

#include "mwf/contracts.h"

namespace mwf_core {

// Everything the loop needs, injected — no globals, host-testable with fakes.
struct WorkerConfig {
  std::string ns;           // temporal namespace
  std::string task_queue;   // activity task queue name
  std::string identity;     // worker identity string
  int         poll_deadline_ms = 70000;     // client poll window (server holds ~60s)
  int         respond_deadline_ms = 10000;  // unary respond calls are quick
  // Extra per-call gRPC metadata (e.g. {"temporal-namespace": ...} for the
  // Mistral frontend). The transport adds authorization itself.
  mwf::Metadata call_metadata;
};

// gRPC full-method paths the loop drives (strings, not stubs — the ITransport
// seam is (fullMethod, bytes) → (status, bytes); see proto/README).
inline constexpr const char* kMethodPollActivityTaskQueue =
    "/temporal.api.workflowservice.v1.WorkflowService/PollActivityTaskQueue";
inline constexpr const char* kMethodRespondActivityTaskCompleted =
    "/temporal.api.workflowservice.v1.WorkflowService/RespondActivityTaskCompleted";
inline constexpr const char* kMethodRespondActivityTaskFailed =
    "/temporal.api.workflowservice.v1.WorkflowService/RespondActivityTaskFailed";

// The one activity task a poll may return, in portable value types (the
// adapter maps PollActivityTaskQueueResponse → this).
struct PolledActivityTask {
  bool        has_task = false;  // false ⇔ empty poll (no/empty task_token)
  mwf::Bytes  task_token;
  std::string activity_name;                        // activity_type.name
  std::vector<mwf::temporal::Payload> inputs;       // input.payloads
  std::string workflow_id;                          // workflow_execution
  std::string run_id;
};

// §2-shaped proto seam for the worker loop: typed request builders / response
// parser over BYTES + plain structs. Desktop impl = mwf_proto_host ProtoCodec;
// device impl = nanopb. Keeps this repo protobuf-lib-free.
struct IWorkerProtoAdapter {
  virtual ~IWorkerProtoAdapter() = default;

  // PollActivityTaskQueueRequest{namespace, task_queue{name,NORMAL}, identity}.
  virtual mwf::Bytes buildPollRequest(const WorkerConfig&) = 0;

  // PollActivityTaskQueueResponse bytes → PolledActivityTask. Returns false on
  // a parse failure; an empty/taskless response is success with has_task=false.
  virtual bool parsePollResponse(const mwf::Bytes&, PolledActivityTask& out) = 0;

  // RespondActivityTaskCompletedRequest{task_token, result{[payload]},
  // identity, namespace}.
  virtual mwf::Bytes buildRespondCompleted(const WorkerConfig&,
                                           const mwf::Bytes& task_token,
                                           const mwf::temporal::Payload& result) = 0;

  // RespondActivityTaskFailedRequest{task_token, failure{message}, identity,
  // namespace}.
  virtual mwf::Bytes buildRespondFailed(const WorkerConfig&,
                                        const mwf::Bytes& task_token,
                                        const std::string& message) = 0;
};

// One tick's outcome, for callers that log/backoff (the loop never throws).
enum class TickOutcome {
  Idle,            // empty long-poll (DEADLINE_EXCEEDED or taskless response)
  Completed,       // task executed, RespondActivityTaskCompleted acked
  Failed,          // task failed cleanly, RespondActivityTaskFailed acked
  TransportError,  // a gRPC call errored (poll or respond) — backoff + re-poll
  ProtocolError,   // poll response bytes did not parse — server/proto mismatch
};

struct TickResult {
  TickOutcome outcome = TickOutcome::Idle;
  std::string activity;  // activity name, when a task was polled
  std::string detail;    // error/status text for logs
};

// Drives one activity worker. Transport + payload codec + registry come from
// contracts; the proto adapter is the loop's own thin seam (above). Runs on
// the caller's thread/task; runOnce() does one poll+dispatch+respond cycle.
class WorkerLoop {
 public:
  WorkerLoop(mwf::ITransport& transport, mwf::IPayloadCodec& payloadCodec,
             mwf::IActivityRegistry& registry, IWorkerProtoAdapter& proto,
             WorkerConfig config);

  // One poll → (maybe) dispatch → respond cycle. deadlineMs bounds the poll
  // (the long-poll hold); responds use config.respond_deadline_ms.
  TickResult runOnce(int deadlineMs);

  // Convenience: runOnce(config.poll_deadline_ms). Returns false only on
  // TransportError so a caller's `while (tick());` stops to backoff/rebuild —
  // Idle/Completed/Failed all keep the loop turning.
  bool tick();

  const WorkerConfig& config() const { return config_; }

 private:
  TickResult respondFailed(const PolledActivityTask& task, std::string message);

  mwf::ITransport&        transport_;
  mwf::IPayloadCodec&     payloadCodec_;
  mwf::IActivityRegistry& registry_;
  IWorkerProtoAdapter&    proto_;
  WorkerConfig            config_;
};

}  // namespace mwf_core

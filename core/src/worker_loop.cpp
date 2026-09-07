// core: WorkerLoop bodies.
//
// Result-payload encoding rule (see conformance/goldens/NOTES.md):
//   * input decoded as json/wf_v1 (context.execution_id set) → echo the context
//     back through IPayloadCodec::encodeActivityResult (the Mistral envelope).
//   * input decoded as json/plain (empty context — e.g. the Temporal Python
//     SDK's default data converter) → respond with a plain
//     {"encoding":"json/plain"} payload, which is what that SDK expects to
//     decode. Emitting wf_v1 metadata there would break the default converter.
#include "mwf_core/worker_loop.h"

namespace mwf_core {
namespace {

constexpr int kGrpcOk = 0;
constexpr int kGrpcDeadlineExceeded = 4;

const char kNullJson[] = "null";

mwf::Bytes nullJson() { return mwf::Bytes(kNullJson, kNullJson + 4); }

bool isNullJson(const mwf::Bytes& b) {
  return b.size() == 4 && b[0] == 'n' && b[1] == 'u' && b[2] == 'l' && b[3] == 'l';
}

// Plain Temporal payload (default data converter shape): json/plain + raw JSON.
mwf::temporal::Payload plainPayload(const mwf::Bytes& result_json) {
  mwf::temporal::Payload p;
  p.metadata["encoding"] = "json/plain";
  p.data = result_json.empty() ? nullJson() : result_json;
  return p;
}

}  // namespace

WorkerLoop::WorkerLoop(mwf::ITransport& transport, mwf::IPayloadCodec& payloadCodec,
                       mwf::IActivityRegistry& registry, IWorkerProtoAdapter& proto,
                       WorkerConfig config)
    : transport_(transport),
      payloadCodec_(payloadCodec),
      registry_(registry),
      proto_(proto),
      config_(std::move(config)) {}

TickResult WorkerLoop::respondFailed(const PolledActivityTask& task,
                                     std::string message) {
  mwf::Bytes req = proto_.buildRespondFailed(config_, task.task_token, message);
  mwf::GrpcResult r = transport_.call(kMethodRespondActivityTaskFailed, req,
                                      config_.call_metadata,
                                      config_.respond_deadline_ms);
  TickResult out;
  out.activity = task.activity_name;
  if (r.grpc_status != kGrpcOk) {
    out.outcome = TickOutcome::TransportError;
    out.detail = "RespondActivityTaskFailed grpc_status=" +
                 std::to_string(r.grpc_status) + " " + r.message +
                 " (original failure: " + message + ")";
    return out;
  }
  out.outcome = TickOutcome::Failed;
  out.detail = std::move(message);
  return out;
}

TickResult WorkerLoop::runOnce(int deadlineMs) {
  TickResult out;

  // ── 1. poll ────────────────────────────────────────────────────────────────
  mwf::Bytes pollReq = proto_.buildPollRequest(config_);
  mwf::GrpcResult poll = transport_.call(kMethodPollActivityTaskQueue, pollReq,
                                         config_.call_metadata, deadlineMs);
  if (poll.grpc_status == kGrpcDeadlineExceeded) {
    out.outcome = TickOutcome::Idle;
    out.detail = "empty long-poll (DEADLINE_EXCEEDED)";
    return out;
  }
  if (poll.grpc_status != kGrpcOk) {
    out.outcome = TickOutcome::TransportError;
    out.detail = "PollActivityTaskQueue grpc_status=" +
                 std::to_string(poll.grpc_status) + " " + poll.message;
    return out;
  }

  PolledActivityTask task;
  if (!proto_.parsePollResponse(poll.response, task)) {
    out.outcome = TickOutcome::ProtocolError;
    out.detail = "PollActivityTaskQueueResponse did not parse (" +
                 std::to_string(poll.response.size()) + " bytes)";
    return out;
  }
  if (!task.has_task) {
    // The frontend may answer OK with an empty response body (no task).
    out.outcome = TickOutcome::Idle;
    out.detail = "taskless poll response";
    return out;
  }
  out.activity = task.activity_name;

  // ── 2. decode input ────────────────────────────────────────────────────────
  // Zero-arg activities arrive with an empty Payloads: argument = null, plain.
  mwf::DecodedActivityInput input;
  if (task.inputs.empty()) {
    input.argument_json = nullJson();
  } else {
    auto decoded = payloadCodec_.decodeActivityInput(task.inputs.front());
    if (!decoded) {
      return respondFailed(task, "activity input decode failed: " + decoded.error);
    }
    input = std::move(decoded.value);
  }

  // ── 3. dispatch ────────────────────────────────────────────────────────────
  mwf::Result<mwf::Bytes> result =
      registry_.invoke(task.activity_name, input.argument_json);
  if (!result) {
    return respondFailed(task, result.error);
  }

  // ── 4. respond completed ───────────────────────────────────────────────────
  const bool wfV1 = !input.context.execution_id.empty();
  mwf::temporal::Payload resultPayload;
  if (wfV1) {
    // Echo the WorkflowContext back (Mistral envelope). A "null" result mirrors
    // the SDK's None → empty_payload set AND data == b"null" (NOTES.md §2).
    const bool empty = result.value.empty() || isNullJson(result.value);
    const mwf::Bytes data = result.value.empty() ? nullJson() : result.value;
    resultPayload = payloadCodec_.encodeActivityResult(data, input.context, empty);
  } else {
    resultPayload = plainPayload(result.value);
  }

  mwf::Bytes req =
      proto_.buildRespondCompleted(config_, task.task_token, resultPayload);
  mwf::GrpcResult r = transport_.call(kMethodRespondActivityTaskCompleted, req,
                                      config_.call_metadata,
                                      config_.respond_deadline_ms);
  if (r.grpc_status != kGrpcOk) {
    out.outcome = TickOutcome::TransportError;
    out.detail = "RespondActivityTaskCompleted grpc_status=" +
                 std::to_string(r.grpc_status) + " " + r.message;
    return out;
  }
  out.outcome = TickOutcome::Completed;
  // Wire-evidence for the live E2E gate: which encoding the input arrived in
  // (json/wf_v1 = Mistral SDK envelope, json/plain = default data converter)
  // and how large the task_token was (feeds the nanopb buffer caps on-device).
  out.detail = std::string("wire=") + (wfV1 ? "json/wf_v1" : "json/plain") +
               " token_bytes=" + std::to_string(task.task_token.size()) +
               " result_bytes=" + std::to_string(result.value.size());
  return out;
}

bool WorkerLoop::tick() {
  return runOnce(config_.poll_deadline_ms).outcome != TickOutcome::TransportError;
}

}  // namespace mwf_core

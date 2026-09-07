// core: WorkflowLoop bodies.
//
// The device runs the WORKFLOW: poll a workflow task, replay its history through
// the deterministic ReplayEngine, respond with the engine's Commands. See the
// header for the full choreography.
//
// Codec rules (conformance/goldens/NOTES.md + spec/codec-wire-format.md):
//   * INBOUND — the parsed History already carries base64-unwrapped inner bytes;
//     each ActivityTaskCompleted result is passed through IPayloadCodec so the
//     engine binds inner JSON. In v1 the codec body transform is IDENTITY (no
//     encryption/offload/compression), so this preserves the inner JSON for both
//     json/plain (the goldens) and json/wf_v1 bodies — a future encrypting codec
//     makes this pass load-bearing.
//   * OUTBOUND — ScheduleActivity inputs and the CompleteWorkflow result are
//     re-wrapped through IPayloadCodec::encodeActivityResult with the run's
//     WorkflowContext, so the Mistral frontend receives the wf_v1 envelope
//     (encoding=json/wf_v1, namespace + execution_id echoed).
//
// execution_id mapping: WorkflowContext.execution_id ← the Temporal WORKFLOW_ID
// (grounded in conformance's payload goldens, whose execution_id is
// workflow-id-shaped, e.g. "linear-goldencapture-0001", not the UUID run_id).
// Durable state stays keyed by RUN_ID (exec/<runId>) — a run is the unique
// execution instance.
#include "mwf_core/workflow_loop.h"

#include <utility>

namespace mwf_core {
namespace {

constexpr int kGrpcOk = 0;
constexpr int kGrpcDeadlineExceeded = 4;

const char kNullJson[] = "null";

mwf::Bytes bytesOf(const std::string& s) { return mwf::Bytes(s.begin(), s.end()); }
std::string strOf(const mwf::Bytes& b) { return std::string(b.begin(), b.end()); }

// The SDK's None → empty_payload set AND data == b"null" (NOTES.md §2). Mirror
// it: a JSON "null" (or genuinely empty) value marks the wf_v1 empty_payload key.
bool isEmptyValue(const std::string& json) { return json.empty() || json == kNullJson; }

// Zero-arg determinism seams — the v1 replay walk never consults a clock or RNG,
// so these are inert. (A v1.1 timer/random grammar would inject real ones.)
struct NoClock : mwf::IClock { uint64_t nowMs() override { return 0; } };
struct NoRandom : mwf::IRandom { uint64_t next() override { return 0; } };

}  // namespace

WorkflowLoop::WorkflowLoop(mwf::ITransport& transport, mwf::IPayloadCodec& payloadCodec,
                           IWorkflowProtoAdapter& proto, mwf::IDurableStore& store,
                           SpecProvider specProvider, WorkflowConfig config)
    : transport_(transport),
      payloadCodec_(payloadCodec),
      proto_(proto),
      store_(store),
      specProvider_(std::move(specProvider)),
      config_(std::move(config)) {}

bool WorkflowLoop::sendRespond(const WorkflowTaskInfo& task,
                               const std::vector<ProtoCommand>& commands,
                               WorkflowTickResult& out) {
  mwf::Bytes req =
      proto_.buildRespondWorkflowCompleted(config_, task.task_token, commands);
  mwf::GrpcResult r = transport_.call(kMethodRespondWorkflowTaskCompleted, req,
                                      config_.call_metadata,
                                      config_.respond_deadline_ms);
  if (r.grpc_status != kGrpcOk) {
    out.outcome = WorkflowTickOutcome::TransportError;
    out.detail = "RespondWorkflowTaskCompleted grpc_status=" +
                 std::to_string(r.grpc_status) + " " + r.message;
    return false;
  }
  return true;
}

WorkflowTickResult WorkflowLoop::respondFail(const WorkflowTaskInfo& task,
                                             std::string message) {
  WorkflowTickResult out;
  out.workflow_type = task.workflow_type;
  out.workflow_id = task.workflow_id;
  out.run_id = task.run_id;
  ProtoCommand fail;
  fail.kind = ProtoCommand::Kind::FailWorkflow;
  fail.failure = message;
  if (!sendRespond(task, {fail}, out)) return out;  // TransportError classified
  out.outcome = WorkflowTickOutcome::Failed;
  out.detail = std::move(message);
  return out;
}

WorkflowTickResult WorkflowLoop::runOnce(int deadlineMs) {
  WorkflowTickResult out;

  // ── 1. poll ──────────────────────────────────────────────────────────────
  mwf::Bytes pollReq = proto_.buildPollWorkflowRequest(config_);
  mwf::GrpcResult poll = transport_.call(kMethodPollWorkflowTaskQueue, pollReq,
                                         config_.call_metadata, deadlineMs);
  if (poll.grpc_status == kGrpcDeadlineExceeded) {
    out.outcome = WorkflowTickOutcome::Idle;
    out.detail = "empty long-poll (DEADLINE_EXCEEDED)";
    return out;
  }
  if (poll.grpc_status != kGrpcOk) {
    out.outcome = WorkflowTickOutcome::TransportError;
    out.detail = "PollWorkflowTaskQueue grpc_status=" +
                 std::to_string(poll.grpc_status) + " " + poll.message;
    return out;
  }

  mwf::Result<WorkflowTaskInfo> parsed = proto_.parseWorkflowTask(poll.response);
  if (!parsed) {
    out.outcome = WorkflowTickOutcome::ProtocolError;
    out.detail = "PollWorkflowTaskQueueResponse did not parse: " + parsed.error;
    return out;
  }
  WorkflowTaskInfo task = std::move(parsed.value);
  if (task.empty) {
    out.outcome = WorkflowTickOutcome::Idle;
    out.detail = "taskless poll response";
    return out;
  }
  out.workflow_type = task.workflow_type;
  out.workflow_id = task.workflow_id;
  out.run_id = task.run_id;

  // ── 2. resolve the spec by workflow type ─────────────────────────────────
  const WorkflowSpec* spec = specProvider_ ? specProvider_(task.workflow_type) : nullptr;
  if (!spec) {
    return respondFail(task, "no workflow spec registered for type '" +
                                 task.workflow_type + "'");
  }

  // ── 3. the run's WorkflowContext (execution_id ← workflow_id) ─────────────
  mwf::WorkflowContext ctx;
  ctx.ns = task.ns.empty() ? config_.ns : task.ns;
  ctx.execution_id = task.workflow_id.empty() ? task.run_id : task.workflow_id;

  // ── 4. codec-decode inbound bound results ────────────────────────────────
  // Route each ActivityTaskCompleted result through IPayloadCodec so the engine
  // binds INNER JSON (tolerating json/plain AND json/wf_v1; v1 body = identity).
  History history = std::move(task.history);
  for (auto& e : history.events) {
    if (e.type != EventType::ActivityTaskCompleted || e.payloads.empty()) continue;
    mwf::temporal::Payload p;
    p.metadata["encoding"] = "json/plain";  // History already unwrapped to inner bytes
    p.data = bytesOf(e.payloads.front());
    auto dec = payloadCodec_.decodeActivityInput(p);
    if (!dec) {
      out.outcome = WorkflowTickOutcome::ProtocolError;
      out.detail = "inbound activity result decode failed: " + dec.error;
      return out;
    }
    e.payloads.front() = strOf(dec.value.argument_json);
  }

  // ── 5. restore prior EngineState + decide ────────────────────────────────
  NoClock clock;
  NoRandom random;
  ReplayEngine eng{clock, random};

  auto prior = ReplayEngine::loadState(store_, task.run_id);
  mwf::Result<Decision> decided =
      prior.ok ? eng.decide(*spec, history, prior.value) : eng.decide(*spec, history);
  if (!decided) {
    out.outcome = WorkflowTickOutcome::ProtocolError;
    out.detail = "replay decide failed: " + decided.error;
    return out;
  }
  Decision decision = std::move(decided.value);

  // ── 6. map engine Commands → ProtoCommands, codec-encoding outbound ───────
  const std::string activityQueue = config_.activity_task_queue.empty()
                                         ? config_.task_queue
                                         : config_.activity_task_queue;
  std::vector<ProtoCommand> commands;
  commands.reserve(decision.commands.size());
  for (const Command& c : decision.commands) {
    ProtoCommand pc;
    switch (c.kind) {
      case Command::Kind::ScheduleActivity:
        pc.kind = ProtoCommand::Kind::ScheduleActivity;
        pc.seq = c.seq;
        pc.activity_name = c.activity_name;
        pc.activity_task_queue = activityQueue;
        pc.input = payloadCodec_.encodeActivityResult(
            bytesOf(c.args_json), ctx, isEmptyValue(c.args_json));
        break;
      case Command::Kind::CompleteWorkflow:
        pc.kind = ProtoCommand::Kind::CompleteWorkflow;
        pc.result = payloadCodec_.encodeActivityResult(
            bytesOf(c.result_json), ctx, isEmptyValue(c.result_json));
        break;
      case Command::Kind::FailWorkflow:
        pc.kind = ProtoCommand::Kind::FailWorkflow;
        pc.failure = c.failure;
        break;
    }
    commands.push_back(std::move(pc));
  }

  // ── 7. persist state BEFORE responding (at-most-once, power-cycle safe) ───
  // If we crash after this and before the respond lands, resume restores this
  // state and the DETERMINISTIC engine re-emits the identical commands; Temporal
  // dedups by activity_id, so the replay is idempotent.
  if (!ReplayEngine::saveState(store_, decision.state)) {
    out.outcome = WorkflowTickOutcome::ProtocolError;
    out.detail = "failed to persist EngineState for run " + task.run_id;
    return out;
  }

  // ── 8. respond ───────────────────────────────────────────────────────────
  if (!sendRespond(task, commands, out)) return out;  // TransportError classified

  // ── 9. classify ──────────────────────────────────────────────────────────
  bool completed = false, failed = false, scheduled = false;
  for (const ProtoCommand& pc : commands) {
    if (pc.kind == ProtoCommand::Kind::CompleteWorkflow) completed = true;
    else if (pc.kind == ProtoCommand::Kind::FailWorkflow) failed = true;
    else if (pc.kind == ProtoCommand::Kind::ScheduleActivity) scheduled = true;
  }
  if (completed) {
    out.outcome = WorkflowTickOutcome::Completed;
    out.detail = "CompleteWorkflowExecution";
  } else if (failed) {
    out.outcome = WorkflowTickOutcome::Failed;
    out.detail = "FailWorkflowExecution";
  } else if (scheduled) {
    out.outcome = WorkflowTickOutcome::Progressed;
    out.detail = "scheduled " + std::to_string(commands.size()) + " activity(ies)";
  } else if (decision.waiting_on_signal) {
    // Blocked at a wait_signal step: the workflow is durably PAUSED until a human
    // (or any client) delivers the signal. workflow_id is the signal target.
    out.outcome = WorkflowTickOutcome::Waiting;
    out.signal_name = decision.waiting_signal_name;
    out.detail = "waiting for signal '" + decision.waiting_signal_name + "'";
  } else {
    // No new commands: the workflow task is answered (empty command list) while
    // it waits on an in-flight activity, or the history was already terminal.
    out.outcome = WorkflowTickOutcome::Progressed;
    out.detail = decision.workflow_finished ? "already terminal (no commands)"
                                            : "awaiting in-flight activity";
  }

  // ── 10. drop durable state once the run is terminal ──────────────────────
  // Step 7 persisted state on EVERY tick (at-most-once safety), including the
  // terminal one. A completed/failed run would otherwise leave exec/<runId>
  // behind forever. Now that the terminal respond has landed, erase it. erase
  // is idempotent, so an already-terminal re-delivery (state already gone) is a
  // harmless no-op. Keyed by the state's run id — exactly what step 7 wrote.
  if (completed || failed || decision.workflow_finished) {
    store_.erase("exec/" + decision.state.run_id);
  }
  return out;
}

bool WorkflowLoop::tick() {
  return runOnce(config_.poll_deadline_ms).outcome != WorkflowTickOutcome::TransportError;
}

}  // namespace mwf_core

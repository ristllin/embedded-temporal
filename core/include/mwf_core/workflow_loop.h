// core: WorkflowLoop — the WORKFLOW poll loop. The device
// (or desktop) runs the WORKFLOW itself: it long-polls the workflow task queue,
// drives the deterministic ReplayEngine over the delivered history, and responds
// with the engine's Commands as protobuf workflow Commands.
//
// It is the self-hosting twin of WorkerLoop: same portable shape (bytes + plain
// structs behind IWorkflowProtoAdapter), same "DEADLINE_EXCEEDED is a normal
// idle long-poll" rule. What it adds over the worker is the STATE MACHINE:
//   poll → parse task → look up spec by workflow_type → codec-decode the bound
//   activity results in history → restore prior EngineState (exec/<runId>) →
//   ReplayEngine::decide → codec-encode outbound payloads → persist new state
//   BEFORE responding (at-most-once, power-cycle safe) → RespondWorkflowTaskCompleted.
//
// Codec integration (see conformance/goldens/NOTES.md + spec/codec-wire-
// format.md): inbound activity results are decoded through IPayloadCodec so the
// engine binds INNER JSON (tolerating json/plain AND json/wf_v1); outbound
// ScheduleActivity inputs and the CompleteWorkflow result are re-encoded through
// IPayloadCodec::encodeActivityResult with the run's WorkflowContext, so the
// Mistral frontend gets the wf_v1 envelope it expects.
#pragma once
#include <functional>
#include <string>
#include <string_view>

#include "mwf/contracts.h"
#include "mwf_core/replay_engine.h"
#include "mwf_core/workflow_proto_adapter.h"
#include "mwf_core/workflow_spec.h"

namespace mwf_core {

// Resolves a workflow type name → its declarative spec. The device loads specs
// from flash/provisioning; a test provides a map. Returns nullptr when unknown
// (the loop then fails the execution cleanly). The pointee must outlive the tick.
using SpecProvider =
    std::function<const WorkflowSpec*(std::string_view workflowType)>;

// One tick's outcome, for callers that log/backoff (the loop never throws).
enum class WorkflowTickOutcome {
  Idle,            // empty long-poll (DEADLINE_EXCEEDED or taskless response)
  Progressed,      // decided + responded, workflow still running (activity(s) scheduled)
  Waiting,         // responded (no commands) — BLOCKED on a wait_signal (human-in-
                   //   the-loop pause). workflow_id + signal_name identify the target.
  Completed,       // responded with CompleteWorkflowExecution
  Failed,          // responded with FailWorkflowExecution (activity failed / no spec)
  TransportError,  // a gRPC call errored (poll or respond) — backoff + re-poll
  ProtocolError,   // poll bytes did not parse, or the engine hit nondeterminism
};

struct WorkflowTickResult {
  WorkflowTickOutcome outcome = WorkflowTickOutcome::Idle;
  std::string workflow_type;  // when a task was polled
  std::string workflow_id;    // when a task was polled — the Mistral execution id
                              //   (the SIGNAL target: ClientOps::signalWorkflow)
  std::string run_id;         // when a task was polled
  std::string signal_name;    // when outcome == Waiting: the awaited signal name
  std::string detail;         // error/status text for logs
};

// Drives one workflow. Transport + payload codec + durable store come from
// contracts; the proto adapter + spec provider are the loop's own seams.
// Runs on the caller's thread/task; runOnce() does one poll+decide+respond cycle.
class WorkflowLoop {
 public:
  WorkflowLoop(mwf::ITransport& transport, mwf::IPayloadCodec& payloadCodec,
               IWorkflowProtoAdapter& proto, mwf::IDurableStore& store,
               SpecProvider specProvider, WorkflowConfig config);

  // One poll → (maybe) decide → respond cycle. deadlineMs bounds the poll (the
  // long-poll hold); responds use config.respond_deadline_ms.
  WorkflowTickResult runOnce(int deadlineMs);

  // Convenience: runOnce(config.poll_deadline_ms). Returns false only on
  // TransportError so a caller's `while (tick());` stops to backoff/rebuild.
  bool tick();

  const WorkflowConfig& config() const { return config_; }

 private:
  // Respond with a single FailWorkflowExecution command (missing spec, etc.).
  WorkflowTickResult respondFail(const WorkflowTaskInfo& task, std::string message);
  // Send a built RespondWorkflowTaskCompleted request, classify the transport
  // result into `out` (leaves out.outcome untouched on success).
  bool sendRespond(const WorkflowTaskInfo& task,
                   const std::vector<ProtoCommand>& commands,
                   WorkflowTickResult& out);

  mwf::ITransport&        transport_;
  mwf::IPayloadCodec&     payloadCodec_;
  IWorkflowProtoAdapter&  proto_;
  mwf::IDurableStore&     store_;
  SpecProvider            specProvider_;
  WorkflowConfig          config_;
};

}  // namespace mwf_core

// core: IWorkflowProtoAdapter — the WORKFLOW poll loop's protobuf seam.
// Twin of IWorkerProtoAdapter (the activity worker seam): the
// WorkflowLoop drives PollWorkflowTaskQueue / RespondWorkflowTaskCompleted over
// ITransport purely on request/response BYTES plus plain value structs. ALL
// protobuf knowledge lives behind this interface — no grpc/protobuf includes
// here by design.
//
// Two concrete twins implement THIS seam and run the same WorkflowLoop:
//   * device  → nanopb,
//   * desktop → libprotobuf over mwf_proto_host.
// The FAKE twin in test/ (de)serializes these plain structs trivially — it
// reuses the history.h golden JSON loader for parseWorkflowTask and a trivial
// encoder for the outbound commands — so the loop is fully host-testable with no
// protobuf at all.
#pragma once
#include <string>
#include <vector>

#include "mwf/contracts.h"
#include "mwf_core/history.h"

namespace mwf_core {

// Everything the workflow loop needs, injected — no globals, host-testable with
// fakes. Mirror of WorkerConfig.
struct WorkflowConfig {
  std::string ns;           // temporal namespace
  std::string task_queue;   // WORKFLOW task queue name (the loop polls this)
  std::string identity;     // worker identity string
  int         poll_deadline_ms = 70000;     // client poll window (server holds ~60s)
  int         respond_deadline_ms = 10000;  // unary respond calls are quick
  // Activity task queue for ScheduleActivityTask commands. Empty (default) ⇒
  // schedule activities on the workflow's OWN task_queue (the common single-
  // queue deployment); set it to fan activities out to a distinct worker pool.
  std::string activity_task_queue;
  // Extra per-call gRPC metadata (e.g. {"temporal-namespace": ...}). The
  // transport adds authorization itself.
  mwf::Metadata call_metadata;
};

// gRPC full-method paths the workflow loop drives (strings, not stubs — the
// ITransport seam is (fullMethod, bytes) → (status, bytes)).
inline constexpr const char* kMethodPollWorkflowTaskQueue =
    "/temporal.api.workflowservice.v1.WorkflowService/PollWorkflowTaskQueue";
inline constexpr const char* kMethodRespondWorkflowTaskCompleted =
    "/temporal.api.workflowservice.v1.WorkflowService/RespondWorkflowTaskCompleted";
// The signal SENDER's RPC (ClientOps::signalWorkflow) — the control side that
// delivers a WorkflowExecutionSignaled event to a waiting (wait_signal) workflow.
inline constexpr const char* kMethodSignalWorkflowExecution =
    "/temporal.api.workflowservice.v1.WorkflowService/SignalWorkflowExecution";

// The one workflow task a poll may return, in portable value types (the adapter
// maps PollWorkflowTaskQueueResponse → this). The parsed HISTORY is the existing
// mwf_core::History (the replay engine's input) — the adapter is responsible for
// translating the protobuf HistoryEvent list into it.
struct WorkflowTaskInfo {
  bool         empty = true;   // true ⇔ empty poll (no/empty task_token)
  mwf::Bytes   task_token;
  std::string  workflow_type;  // workflowType.name — keys the SpecProvider
  std::string  ns;             // "namespace" is a keyword; the frontend's namespace
  std::string  workflow_id;    // workflowExecution.workflowId — the Mistral execution_id
  std::string  run_id;         // workflowExecution.runId — the durable-state key (exec/<runId>)
  History      history;        // decoded event stream fed to ReplayEngine::decide
};

// Portable command struct the loop hands the adapter to serialize into
// RespondWorkflowTaskCompleted's `commands` list. Plain mirror of the engine's
// Command, but carrying the ALREADY-CODEC-ENCODED payloads (the loop wraps the
// engine's raw JSON through IPayloadCodec before building these, so the adapter
// is a pure struct→bytes step with no codec knowledge).
struct ProtoCommand {
  enum class Kind { ScheduleActivity, CompleteWorkflow, FailWorkflow, StartTimer };
  Kind kind = Kind::ScheduleActivity;

  // ScheduleActivityTask: activity_id on the wire is std::to_string(seq).
  uint32_t                seq = 0;
  std::string             activity_name;
  std::string             activity_task_queue;  // where the activity runs
  mwf::temporal::Payload  input;                 // codec-encoded activity argument

  // CompleteWorkflowExecution.
  mwf::temporal::Payload  result;                // codec-encoded workflow result

  // FailWorkflowExecution.
  std::string             failure;

  // StartTimer (v1.1-reserved; carried so the seam is stable for the timer
  // grammar — the v1 loop never emits it).
  std::string             timer_id;
  uint64_t                timer_ms = 0;
};

// All the SignalWorkflowExecutionRequest fields ClientOps supplies, in portable
// value types (the adapter maps this → the wire message). run_id may be empty to
// target the latest run of workflow_id; `input` is the single signal payload
// (ClientOps wraps the raw-JSON argument into it — json/plain in v1).
struct SignalRequest {
  std::string ns;
  std::string workflow_id;
  std::string run_id;         // optional; empty ⇒ latest run
  std::string signal_name;
  std::string identity;
  mwf::temporal::Payload input;
};

// The §2-shaped proto seam for the workflow loop: typed request builder /
// response parser / command serializer over BYTES + plain structs. Keeps this
// repo protobuf-lib-free.
struct IWorkflowProtoAdapter {
  virtual ~IWorkflowProtoAdapter() = default;

  // PollWorkflowTaskQueueRequest{namespace, taskQueue{name,NORMAL}, identity}.
  virtual mwf::Bytes buildPollWorkflowRequest(const WorkflowConfig&) = 0;

  // PollWorkflowTaskQueueResponse bytes → WorkflowTaskInfo (task_token,
  // execution ids, workflow_type, and the decoded History). An empty/taskless
  // response is SUCCESS with .empty == true. A genuine parse failure is a
  // Result failure (→ ProtocolError at the loop).
  virtual mwf::Result<WorkflowTaskInfo> parseWorkflowTask(const mwf::Bytes&) = 0;

  // RespondWorkflowTaskCompletedRequest{task_token, commands[], identity,
  // namespace}. The loop passes the full command list (already codec-encoded).
  virtual mwf::Bytes buildRespondWorkflowCompleted(
      const WorkflowConfig&, const mwf::Bytes& task_token,
      const std::vector<ProtoCommand>&) = 0;

  // SignalWorkflowExecutionRequest{namespace, workflowExecution{workflow_id,
  // run_id}, signal_name, input(payload), identity}. The signal-SENDER path
  // (ClientOps::signalWorkflow); NOT part of the worker poll loop. Empty Bytes on
  // an over-cap field (the caller then reports a build failure). Defaulted so the
  // worker-only fakes need not implement it; the host + nanopb twins override it.
  virtual mwf::Bytes buildSignalWorkflowRequest(const SignalRequest&) { return {}; }
};

}  // namespace mwf_core

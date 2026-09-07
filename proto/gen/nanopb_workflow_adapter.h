// proto: NanopbWorkflowAdapter — the DEVICE twin of core's
// IWorkflowProtoAdapter seam. Maps the WORKFLOW loop's portable structs
// ⇄ the nanopb device-subset workflow-task messages (gen/nanopb-device, from
// proto/mwf/device_workflow.proto — a wire-identical trim of the temporal.api
// workflow-task messages; see that file's header for why a mirror). The
// self-hosting twin of NanopbWorkerAdapter: same "all nanopb knowledge stays in
// the .cpp" discipline.
//
// ⚠ PSRAM. Unlike the activity subset (~34 KB), the workflow messages are HUGE
// at the device caps: PollWorkflowTaskQueueResponse ≈ 2.4 MB (History.events[64]
// × a Payloads-carrying HistoryEvent) and RespondWorkflowTaskCompletedRequest ≈
// 617 KB (commands[16]). NEITHER fits the ~300 KB internal SRAM, so both are
// allocated through the big-buffer hook below and freed per call. On the device
// firmware installs a heap_caps_malloc(SPIRAM) hook; the default is malloc/free
// (the host conformance test + no-PSRAM boards). See device_workflow.proto's
// header + nanopb/device_workflow.options for the size accounting.
//
// Portable C++17, no Arduino: host-compiled (default malloc hook) and
// device-compiled by firmware. All nanopb types stay in the .cpp.
#pragma once

#include <cstddef>

#include "mwf_core/workflow_proto_adapter.h"

namespace mwf_proto {

// Big-buffer allocator seam (see the PSRAM note above). Portable code can't call
// heap_caps_malloc, so the multi-megabyte workflow structs route through these
// hooks. Defaults to malloc/free; firmware overrides with PSRAM versions at
// boot. Passing null for either restores the default.
using BigAllocFn = void* (*)(std::size_t);
using BigFreeFn  = void  (*)(void*);
void setBigBufferAllocator(BigAllocFn alloc, BigFreeFn free);

class NanopbWorkflowAdapter final : public mwf_core::IWorkflowProtoAdapter {
 public:
  // PollWorkflowTaskQueueRequest{namespace, task_queue{name, NORMAL}, identity}.
  // Small (~400 B) — built on the stack.
  mwf::Bytes buildPollWorkflowRequest(const mwf_core::WorkflowConfig&) override;

  // PollWorkflowTaskQueueResponse bytes → WorkflowTaskInfo (task_token, execution
  // ids, workflow_type, decoded History). Empty/taskless response ⇒ success with
  // .empty == true. A decode failure — or a non-empty next_page_token (v1 refuses
  // paged history rather than replay a truncated one) — is a Result failure
  // (→ ProtocolError at the loop). The ~2.4 MB response struct is PSRAM-allocated.
  mwf::Result<mwf_core::WorkflowTaskInfo> parseWorkflowTask(
      const mwf::Bytes&) override;

  // RespondWorkflowTaskCompletedRequest{task_token, commands[], identity,
  // namespace}. The ~617 KB request struct is PSRAM-allocated. Over-cap payloads
  // return an empty Bytes (the loop then classifies a transport failure), never a
  // silent truncation.
  mwf::Bytes buildRespondWorkflowCompleted(
      const mwf_core::WorkflowConfig&, const mwf::Bytes& task_token,
      const std::vector<mwf_core::ProtoCommand>&) override;

  // SignalWorkflowExecutionRequest{namespace, workflowExecution{workflow_id,
  // run_id}, signal_name, input(payload), identity} — the signal-SENDER path
  // (ClientOps::signalWorkflow). ~40 KB at the caps (one embedded Payloads), so
  // it too is PSRAM-allocated. Empty Bytes on any over-cap field.
  mwf::Bytes buildSignalWorkflowRequest(const mwf_core::SignalRequest&) override;
};

}  // namespace mwf_proto

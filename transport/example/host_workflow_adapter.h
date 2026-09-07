// transport example: HostWorkflowAdapter — the desktop (libprotobuf) twin of
// core's IWorkflowProtoAdapter seam. Twin of HostProtoAdapter (the
// activity-worker seam): maps the WorkflowLoop's portable structs <-> the
// generated Temporal WORKFLOW messages via proto's ProtoCodec. The device
// twin will do the same over nanopb — the WorkflowLoop itself is shared,
// protobuf-lib-free.
//
// Three jobs (mirror of the header seam):
//   * buildPollWorkflowRequest      → PollWorkflowTaskQueueRequest
//   * parseWorkflowTask             → PollWorkflowTaskQueueResponse → WorkflowTaskInfo
//                                     (incl. the protobuf History → mwf_core::History
//                                      translation the ReplayEngine consumes)
//   * buildRespondWorkflowCompleted → RespondWorkflowTaskCompletedRequest with the
//                                     engine's ProtoCommands mapped to Temporal Commands.
//
// Payload handling: the wf_v1 envelope keeps the INNER JSON in Payload.data and
// carries the WorkflowContext in Payload.metadata (see codec). The engine
// binds inner JSON, so every history payload is stored as its raw data() bytes;
// the WorkflowLoop re-encodes outbound payloads through IPayloadCodec itself, so
// the ProtoCommand.input/result Payloads this adapter serializes are ALREADY
// wf_v1-wrapped — a pure struct→proto step here, no codec knowledge.
#pragma once

#include <string>

#include "mwf_core/history.h"
#include "mwf_core/workflow_proto_adapter.h"
#include "proto_codec.h"  // proto/gen: ProtoCodec<Msg> + mwf::proto aliases
#include "temporal/api/enums/v1/command_type.pb.h"
#include "temporal/api/enums/v1/event_type.pb.h"
#include "temporal/api/enums/v1/task_queue.pb.h"

namespace mwf_example {

class HostWorkflowAdapter final : public mwf_core::IWorkflowProtoAdapter {
 public:
  mwf::Bytes buildPollWorkflowRequest(const mwf_core::WorkflowConfig& c) override {
    mwf::proto::PollWorkflowTaskQueueRequest req;
    req.set_namespace_(c.ns);
    req.mutable_task_queue()->set_name(c.task_queue);
    req.mutable_task_queue()->set_kind(
        ::temporal::api::enums::v1::TASK_QUEUE_KIND_NORMAL);
    req.set_identity(c.identity);
    return poll_req_.encode(req);
  }

  mwf::Result<mwf_core::WorkflowTaskInfo> parseWorkflowTask(
      const mwf::Bytes& bytes) override {
    using R = mwf::Result<mwf_core::WorkflowTaskInfo>;
    mwf::proto::PollWorkflowTaskQueueResponse resp;
    if (!poll_resp_.decode(bytes, resp))
      return R::failure("PollWorkflowTaskQueueResponse did not parse");

    mwf_core::WorkflowTaskInfo out;
    if (resp.task_token().empty()) {
      out.empty = true;  // empty long-poll release (OK + no task)
      return R::success(std::move(out));
    }
    out.empty = false;
    out.task_token.assign(resp.task_token().begin(), resp.task_token().end());
    out.workflow_type = resp.workflow_type().name();
    out.workflow_id = resp.workflow_execution().workflow_id();
    out.run_id = resp.workflow_execution().run_id();
    // The frontend's namespace is not echoed on the poll response envelope; the
    // loop falls back to config.ns when this is empty (see runOnce §3).

    // Translate the protobuf HistoryEvent list into the engine's lean History.
    for (const auto& ev : resp.history().events()) {
      mwf_core::HistoryEvent he;
      he.event_id = ev.event_id();
      he.type = mapEventType(ev.event_type());
      switch (he.type) {
        case mwf_core::EventType::WorkflowExecutionStarted: {
          const auto& a = ev.workflow_execution_started_event_attributes();
          he.workflow_type = a.workflow_type().name();
          he.workflow_id = a.workflow_id();
          // run id: prefer originalExecutionRunId, fall back to firstExecutionRunId.
          he.run_id = !a.original_execution_run_id().empty()
                          ? a.original_execution_run_id()
                          : a.first_execution_run_id();
          appendPayloads(a.input(), he);
          break;
        }
        case mwf_core::EventType::ActivityTaskScheduled: {
          const auto& a = ev.activity_task_scheduled_event_attributes();
          he.activity_name = a.activity_type().name();
          he.activity_id = a.activity_id();
          appendPayloads(a.input(), he);
          break;
        }
        case mwf_core::EventType::ActivityTaskStarted: {
          const auto& a = ev.activity_task_started_event_attributes();
          he.scheduled_event_id = a.scheduled_event_id();
          break;
        }
        case mwf_core::EventType::ActivityTaskCompleted: {
          const auto& a = ev.activity_task_completed_event_attributes();
          he.scheduled_event_id = a.scheduled_event_id();
          he.started_event_id = a.started_event_id();
          appendPayloads(a.result(), he);
          break;
        }
        case mwf_core::EventType::ActivityTaskFailed: {
          const auto& a = ev.activity_task_failed_event_attributes();
          he.scheduled_event_id = a.scheduled_event_id();
          he.failure_message = a.failure().message();
          break;
        }
        case mwf_core::EventType::WorkflowExecutionCompleted: {
          const auto& a = ev.workflow_execution_completed_event_attributes();
          appendPayloads(a.result(), he);
          break;
        }
        case mwf_core::EventType::WorkflowExecutionFailed: {
          const auto& a = ev.workflow_execution_failed_event_attributes();
          he.failure_message = a.failure().message();
          break;
        }
        case mwf_core::EventType::WorkflowExecutionSignaled: {
          // The human-in-the-loop signal: the engine binds signal_name to a
          // wait_signal step and its input payload as that step's result. Without
          // this case the signal event carries an empty name and the workflow
          // re-blocks forever (never unblocks the wait_signal).
          const auto& a = ev.workflow_execution_signaled_event_attributes();
          he.signal_name = a.signal_name();
          appendPayloads(a.input(), he);
          break;
        }
        default:
          // WorkflowTaskScheduled/Started/Completed and everything else are not
          // consulted by the v1 replay walk — carried as Unknown, event_id only.
          break;
      }
      out.history.events.push_back(std::move(he));
    }
    return R::success(std::move(out));
  }

  mwf::Bytes buildRespondWorkflowCompleted(
      const mwf_core::WorkflowConfig& c, const mwf::Bytes& task_token,
      const std::vector<mwf_core::ProtoCommand>& commands) override {
    mwf::proto::RespondWorkflowTaskCompletedReq req;
    req.set_task_token(task_token.data(), task_token.size());
    req.set_identity(c.identity);
    req.set_namespace_(c.ns);
    for (const auto& pc : commands) addCommand(req, pc);
    return respond_.encode(req);
  }

  mwf::Bytes buildSignalWorkflowRequest(
      const mwf_core::SignalRequest& s) override {
    mwf::proto::SignalWorkflowExecutionRequest req;
    req.set_namespace_(s.ns);
    req.mutable_workflow_execution()->set_workflow_id(s.workflow_id);
    if (!s.run_id.empty()) req.mutable_workflow_execution()->set_run_id(s.run_id);
    req.set_signal_name(s.signal_name);
    req.set_identity(s.identity);
    toProtoPayload(s.input, req.mutable_input()->add_payloads());
    return signal_.encode(req);
  }

 private:
  // Copy an mwf value Payload into a protobuf Payload (metadata + data).
  static void toProtoPayload(const mwf::temporal::Payload& src,
                             mwf::proto::Payload* dst) {
    for (const auto& kv : src.metadata)
      (*dst->mutable_metadata())[kv.first] = kv.second;
    dst->set_data(src.data.data(), src.data.size());
  }

  // Append each protobuf payload's INNER data bytes (raw JSON, wf_v1-unwrapped
  // by construction) as a history-event payload string. Context metadata is
  // dropped — the loop re-derives it from the workflow id.
  static void appendPayloads(const mwf::proto::Payloads& payloads,
                             mwf_core::HistoryEvent& he) {
    for (const auto& p : payloads.payloads())
      he.payloads.emplace_back(p.data().begin(), p.data().end());
  }

  static mwf_core::EventType mapEventType(int t) {
    using namespace ::temporal::api::enums::v1;
    switch (t) {
      case EVENT_TYPE_WORKFLOW_EXECUTION_STARTED:
        return mwf_core::EventType::WorkflowExecutionStarted;
      case EVENT_TYPE_WORKFLOW_TASK_SCHEDULED:
        return mwf_core::EventType::WorkflowTaskScheduled;
      case EVENT_TYPE_WORKFLOW_TASK_STARTED:
        return mwf_core::EventType::WorkflowTaskStarted;
      case EVENT_TYPE_WORKFLOW_TASK_COMPLETED:
        return mwf_core::EventType::WorkflowTaskCompleted;
      case EVENT_TYPE_ACTIVITY_TASK_SCHEDULED:
        return mwf_core::EventType::ActivityTaskScheduled;
      case EVENT_TYPE_ACTIVITY_TASK_STARTED:
        return mwf_core::EventType::ActivityTaskStarted;
      case EVENT_TYPE_ACTIVITY_TASK_COMPLETED:
        return mwf_core::EventType::ActivityTaskCompleted;
      case EVENT_TYPE_ACTIVITY_TASK_FAILED:
        return mwf_core::EventType::ActivityTaskFailed;
      case EVENT_TYPE_TIMER_STARTED:
        return mwf_core::EventType::TimerStarted;
      case EVENT_TYPE_TIMER_FIRED:
        return mwf_core::EventType::TimerFired;
      case EVENT_TYPE_WORKFLOW_EXECUTION_COMPLETED:
        return mwf_core::EventType::WorkflowExecutionCompleted;
      case EVENT_TYPE_WORKFLOW_EXECUTION_FAILED:
        return mwf_core::EventType::WorkflowExecutionFailed;
      case EVENT_TYPE_WORKFLOW_EXECUTION_SIGNALED:
        return mwf_core::EventType::WorkflowExecutionSignaled;
      default:
        return mwf_core::EventType::Unknown;
    }
  }

  void addCommand(mwf::proto::RespondWorkflowTaskCompletedReq& req,
                  const mwf_core::ProtoCommand& pc) {
    using namespace ::temporal::api::enums::v1;
    mwf::proto::Command* cmd = req.add_commands();
    switch (pc.kind) {
      case mwf_core::ProtoCommand::Kind::ScheduleActivity: {
        cmd->set_command_type(COMMAND_TYPE_SCHEDULE_ACTIVITY_TASK);
        auto* a = cmd->mutable_schedule_activity_task_command_attributes();
        a->set_activity_id(std::to_string(pc.seq));
        a->mutable_activity_type()->set_name(pc.activity_name);
        a->mutable_task_queue()->set_name(pc.activity_task_queue);
        toProtoPayload(pc.input, a->mutable_input()->add_payloads());
        // Timeouts: bound so a stuck activity can't wedge the workflow. Mirrors
        // the probe (schedule_to_close 120s / start_to_close 60s).
        a->mutable_schedule_to_close_timeout()->set_seconds(120);
        a->mutable_start_to_close_timeout()->set_seconds(60);
        break;
      }
      case mwf_core::ProtoCommand::Kind::CompleteWorkflow: {
        cmd->set_command_type(COMMAND_TYPE_COMPLETE_WORKFLOW_EXECUTION);
        auto* a = cmd->mutable_complete_workflow_execution_command_attributes();
        toProtoPayload(pc.result, a->mutable_result()->add_payloads());
        break;
      }
      case mwf_core::ProtoCommand::Kind::FailWorkflow: {
        cmd->set_command_type(COMMAND_TYPE_FAIL_WORKFLOW_EXECUTION);
        auto* a = cmd->mutable_fail_workflow_execution_command_attributes();
        a->mutable_failure()->set_message(pc.failure);
        a->mutable_failure()->set_source("mwf-cpp-workflow");
        a->mutable_failure()->mutable_application_failure_info()->set_type(
            "MwfWorkflowFailure");
        break;
      }
      case mwf_core::ProtoCommand::Kind::StartTimer:
        // v1.1-reserved; the v1 loop never emits it. Skip (no command added
        // would leave an empty Command — but the loop guarantees this is unused).
        req.mutable_commands()->RemoveLast();
        break;
    }
  }

  mwf::ProtoCodec<mwf::proto::PollWorkflowTaskQueueRequest> poll_req_;
  mwf::ProtoCodec<mwf::proto::PollWorkflowTaskQueueResponse> poll_resp_;
  mwf::ProtoCodec<mwf::proto::RespondWorkflowTaskCompletedReq> respond_;
  mwf::ProtoCodec<mwf::proto::SignalWorkflowExecutionRequest> signal_;
};

}  // namespace mwf_example

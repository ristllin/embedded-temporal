// proto: NanopbWorkflowAdapter bodies. The only TU (besides the worker
// adapter) that touches the nanopb device-subset types. See
// nanopb_workflow_adapter.h. Requires -DPB_FIELD_32BIT (device_workflow subset).
#include "nanopb_workflow_adapter.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include <pb_decode.h>
#include <pb_encode.h>

#include "mwf/device_workflow.pb.h"  // pulls in mwf/device_activity.pb.h

namespace mwf_proto {
namespace {

// ── big-buffer allocator hook (default malloc/free; PSRAM on device) ──────────
void* defaultBigAlloc(std::size_t n) { return std::malloc(n); }
void  defaultBigFree(void* p) { std::free(p); }
BigAllocFn g_bigAlloc = &defaultBigAlloc;
BigFreeFn  g_bigFree  = &defaultBigFree;

// RAII holder for a per-call PSRAM message buffer.
template <class T>
struct BigBuf {
  T* p = nullptr;
  BigBuf() : p(static_cast<T*>(g_bigAlloc(sizeof(T)))) {}
  ~BigBuf() { if (p) g_bigFree(p); }
  BigBuf(const BigBuf&) = delete;
  BigBuf& operator=(const BigBuf&) = delete;
  explicit operator bool() const { return p != nullptr; }
};

// temporal enum ints (temporal.api.enums.v1) — wire values, not the nanopb oneof
// arm field numbers. Kept local so the device subset needs no enum headers.
constexpr int kEvtWorkflowExecutionStarted   = 1;
constexpr int kEvtWorkflowExecutionCompleted = 2;
constexpr int kEvtWorkflowExecutionFailed    = 3;
constexpr int kEvtWorkflowTaskScheduled      = 5;
constexpr int kEvtWorkflowTaskStarted        = 6;
constexpr int kEvtWorkflowTaskCompleted      = 7;
constexpr int kEvtActivityTaskScheduled      = 10;
constexpr int kEvtActivityTaskStarted        = 11;
constexpr int kEvtActivityTaskCompleted      = 12;
constexpr int kEvtActivityTaskFailed         = 13;
constexpr int kEvtTimerStarted               = 17;
constexpr int kEvtTimerFired                 = 18;
constexpr int kEvtWorkflowExecutionSignaled  = 26;

constexpr int kCmdScheduleActivityTask       = 1;
constexpr int kCmdStartTimer                 = 3;
constexpr int kCmdCompleteWorkflowExecution  = 4;
constexpr int kCmdFailWorkflowExecution      = 5;

constexpr int kTaskQueueKindNormal = 1;  // TASK_QUEUE_KIND_NORMAL

// Default activity timeouts the Mistral frontend requires on a schedule command
// (mirrors conformance/probe/sole_worker_probe.py --with-activity). The
// portable ProtoCommand carries no timeouts, so the device twin supplies them.
constexpr int64_t kScheduleToCloseSecs = 120;
constexpr int64_t kStartToCloseSecs    = 60;

// Bounded C-string copy into a nanopb char[N]. Over-long ⇒ false (refuse, never
// truncate a request field).
template <size_t N>
bool setStr(char (&dst)[N], const std::string& src) {
  if (src.size() >= N) return false;
  std::memcpy(dst, src.data(), src.size());
  dst[src.size()] = '\0';
  return true;
}

// Bounded bytes copy into a nanopb PB_BYTES_ARRAY_T.
template <class PbBytes>
bool setBytes(PbBytes& dst, const uint8_t* src, size_t n) {
  if (n > sizeof(dst.bytes)) return false;
  dst.size = static_cast<pb_size_t>(n);
  if (n) std::memcpy(dst.bytes, src, n);
  return true;
}

// mwf::temporal::Payload → nanopb Payload (identical treatment to the worker
// adapter's fillPayload). False on any over-cap field.
bool fillPayload(mwf_device_v1_Payload& dst, const mwf::temporal::Payload& src) {
  if (src.metadata.size() > (sizeof(dst.metadata) / sizeof(dst.metadata[0])))
    return false;
  pb_size_t i = 0;
  for (const auto& kv : src.metadata) {
    auto& e = dst.metadata[i++];
    if (!setStr(e.key, kv.first)) return false;
    if (!setBytes(e.value, reinterpret_cast<const uint8_t*>(kv.second.data()),
                  kv.second.size()))
      return false;
  }
  dst.metadata_count = i;
  return setBytes(dst.data, src.data.data(), src.data.size());
}

// A device Payload's raw data bytes as a std::string (the History model stores
// inner-JSON payload text; json/plain goldens carry it verbatim, and the loop
// re-runs IPayloadCodec over ActivityTaskCompleted results itself).
std::string payloadData(const mwf_device_v1_Payload& p) {
  return std::string(reinterpret_cast<const char*>(p.data.bytes), p.data.size);
}
void appendPayloads(std::vector<std::string>& out,
                    const mwf_device_v1_Payloads& ps) {
  for (pb_size_t i = 0; i < ps.payloads_count; ++i)
    out.push_back(payloadData(ps.payloads[i]));
}

mwf_core::EventType mapEventType(int32_t t) {
  using ET = mwf_core::EventType;
  switch (t) {
    case kEvtWorkflowExecutionStarted:   return ET::WorkflowExecutionStarted;
    case kEvtWorkflowExecutionCompleted: return ET::WorkflowExecutionCompleted;
    case kEvtWorkflowExecutionFailed:    return ET::WorkflowExecutionFailed;
    case kEvtWorkflowTaskScheduled:      return ET::WorkflowTaskScheduled;
    case kEvtWorkflowTaskStarted:        return ET::WorkflowTaskStarted;
    case kEvtWorkflowTaskCompleted:      return ET::WorkflowTaskCompleted;
    case kEvtActivityTaskScheduled:      return ET::ActivityTaskScheduled;
    case kEvtActivityTaskStarted:        return ET::ActivityTaskStarted;
    case kEvtActivityTaskCompleted:      return ET::ActivityTaskCompleted;
    case kEvtActivityTaskFailed:         return ET::ActivityTaskFailed;
    case kEvtTimerStarted:               return ET::TimerStarted;
    case kEvtTimerFired:                 return ET::TimerFired;
    case kEvtWorkflowExecutionSignaled:  return ET::WorkflowExecutionSignaled;
    default:                             return ET::Unknown;
  }
}

// nanopb HistoryEvent → mwf_core::HistoryEvent. Reads whichever oneof arm
// decoded; scalar id/type fields are always valid.
mwf_core::HistoryEvent mapEvent(const mwf_device_v1_HistoryEvent& de) {
  mwf_core::HistoryEvent he;
  he.event_id = de.event_id;
  he.type = mapEventType(de.event_type);

  switch (de.which_attributes) {
    case mwf_device_v1_HistoryEvent_workflow_execution_started_event_attributes_tag: {
      const auto& a = de.attributes.workflow_execution_started_event_attributes;
      if (a.has_workflow_type) he.workflow_type = a.workflow_type.name;
      he.workflow_id = a.workflow_id;
      // History model: run_id ← originalExecutionRunId, else firstExecutionRunId.
      he.run_id = a.original_execution_run_id[0] ? a.original_execution_run_id
                                                 : a.first_execution_run_id;
      if (a.has_input) appendPayloads(he.payloads, a.input);
      break;
    }
    case mwf_device_v1_HistoryEvent_activity_task_scheduled_event_attributes_tag: {
      const auto& a = de.attributes.activity_task_scheduled_event_attributes;
      he.activity_id = a.activity_id;
      if (a.has_activity_type) he.activity_name = a.activity_type.name;
      if (a.has_input) appendPayloads(he.payloads, a.input);
      break;
    }
    case mwf_device_v1_HistoryEvent_activity_task_started_event_attributes_tag:
      he.scheduled_event_id =
          de.attributes.activity_task_started_event_attributes.scheduled_event_id;
      break;
    case mwf_device_v1_HistoryEvent_activity_task_completed_event_attributes_tag: {
      const auto& a = de.attributes.activity_task_completed_event_attributes;
      he.scheduled_event_id = a.scheduled_event_id;
      he.started_event_id = a.started_event_id;
      if (a.has_result) appendPayloads(he.payloads, a.result);
      break;
    }
    case mwf_device_v1_HistoryEvent_activity_task_failed_event_attributes_tag: {
      const auto& a = de.attributes.activity_task_failed_event_attributes;
      he.scheduled_event_id = a.scheduled_event_id;
      he.started_event_id = a.started_event_id;
      if (a.has_failure) he.failure_message = a.failure.message;
      break;
    }
    case mwf_device_v1_HistoryEvent_workflow_task_started_event_attributes_tag:
      he.scheduled_event_id =
          de.attributes.workflow_task_started_event_attributes.scheduled_event_id;
      break;
    case mwf_device_v1_HistoryEvent_workflow_task_completed_event_attributes_tag: {
      const auto& a = de.attributes.workflow_task_completed_event_attributes;
      he.scheduled_event_id = a.scheduled_event_id;
      he.started_event_id = a.started_event_id;
      break;
    }
    case mwf_device_v1_HistoryEvent_timer_started_event_attributes_tag:
      he.timer_id = de.attributes.timer_started_event_attributes.timer_id;
      break;
    case mwf_device_v1_HistoryEvent_timer_fired_event_attributes_tag: {
      const auto& a = de.attributes.timer_fired_event_attributes;
      he.timer_id = a.timer_id;
      he.started_event_id = a.started_event_id;
      break;
    }
    case mwf_device_v1_HistoryEvent_workflow_execution_completed_event_attributes_tag: {
      const auto& a = de.attributes.workflow_execution_completed_event_attributes;
      if (a.has_result) appendPayloads(he.payloads, a.result);
      break;
    }
    case mwf_device_v1_HistoryEvent_workflow_execution_failed_event_attributes_tag: {
      const auto& a = de.attributes.workflow_execution_failed_event_attributes;
      if (a.has_failure) he.failure_message = a.failure.message;
      break;
    }
    case mwf_device_v1_HistoryEvent_workflow_execution_signaled_event_attributes_tag: {
      const auto& a = de.attributes.workflow_execution_signaled_event_attributes;
      he.signal_name = a.signal_name;
      if (a.has_input) appendPayloads(he.payloads, a.input);
      break;
    }
    default:
      break;  // WorkflowTaskScheduled (id-only) + skipped/unknown arms
  }
  return he;
}

// Encode a nanopb message into mwf::Bytes (size pass + encode pass). Empty result
// = encode failure (over-cap field, etc.).
mwf::Bytes encodeMsg(const pb_msgdesc_t* fields, const void* msg) {
  size_t size = 0;
  if (!pb_get_encoded_size(&size, fields, msg)) return {};
  mwf::Bytes out(size);
  pb_ostream_t os = pb_ostream_from_buffer(out.data(), out.size());
  if (!pb_encode(&os, fields, msg)) return {};
  out.resize(os.bytes_written);
  return out;
}

}  // namespace

void setBigBufferAllocator(BigAllocFn alloc, BigFreeFn free) {
  g_bigAlloc = alloc ? alloc : &defaultBigAlloc;
  g_bigFree  = free ? free : &defaultBigFree;
}

mwf::Bytes NanopbWorkflowAdapter::buildPollWorkflowRequest(
    const mwf_core::WorkflowConfig& c) {
  mwf_device_v1_PollWorkflowTaskQueueRequest req =
      mwf_device_v1_PollWorkflowTaskQueueRequest_init_zero;
  if (!setStr(req.namespace_, c.ns) || !setStr(req.identity, c.identity) ||
      !setStr(req.task_queue.name, c.task_queue))
    return {};
  req.has_task_queue = true;
  req.task_queue.kind = kTaskQueueKindNormal;
  return encodeMsg(mwf_device_v1_PollWorkflowTaskQueueRequest_fields, &req);
}

mwf::Result<mwf_core::WorkflowTaskInfo> NanopbWorkflowAdapter::parseWorkflowTask(
    const mwf::Bytes& bytes) {
  using R = mwf::Result<mwf_core::WorkflowTaskInfo>;

  // ~2.4 MB at the device caps — PSRAM on device, never the stack.
  BigBuf<mwf_device_v1_PollWorkflowTaskQueueResponse> resp;
  if (!resp) return R::failure("PSRAM alloc failed for PollWorkflowTaskQueueResponse");
  *resp.p = mwf_device_v1_PollWorkflowTaskQueueResponse_init_zero;

  pb_istream_t is = pb_istream_from_buffer(bytes.data(), bytes.size());
  if (!pb_decode(&is, mwf_device_v1_PollWorkflowTaskQueueResponse_fields, resp.p))
    return R::failure(std::string("nanopb decode failed: ") + PB_GET_ERROR(&is));

  mwf_core::WorkflowTaskInfo info;
  if (resp.p->task_token.size == 0) {
    info.empty = true;  // taskless long-poll release (idle)
    return R::success(std::move(info));
  }

  // v1 single-page assumption: a paged history is refused (do NOT replay a
  // truncated history). A non-empty next_page_token is the signal.
  if (resp.p->next_page_token.size != 0)
    return R::failure(
        "paged workflow history refused (non-empty next_page_token; v1 assumes "
        "single-page)");

  info.empty = false;
  info.task_token.assign(resp.p->task_token.bytes,
                         resp.p->task_token.bytes + resp.p->task_token.size);
  if (resp.p->has_workflow_type) info.workflow_type = resp.p->workflow_type.name;
  if (resp.p->has_workflow_execution) {
    info.workflow_id = resp.p->workflow_execution.workflow_id;
    info.run_id = resp.p->workflow_execution.run_id;
  }
  // No namespace on the poll response; the loop falls back to config.ns.

  info.history.events.reserve(resp.p->history.events_count);
  for (pb_size_t i = 0; i < resp.p->history.events_count; ++i)
    info.history.events.push_back(mapEvent(resp.p->history.events[i]));

  return R::success(std::move(info));
}

mwf::Bytes NanopbWorkflowAdapter::buildRespondWorkflowCompleted(
    const mwf_core::WorkflowConfig& c, const mwf::Bytes& task_token,
    const std::vector<mwf_core::ProtoCommand>& commands) {
  // ~617 KB at the device caps — PSRAM on device, never the stack.
  BigBuf<mwf_device_v1_RespondWorkflowTaskCompletedRequest> req;
  if (!req) return {};
  *req.p = mwf_device_v1_RespondWorkflowTaskCompletedRequest_init_zero;

  if (!setBytes(req.p->task_token, task_token.data(), task_token.size()) ||
      !setStr(req.p->identity, c.identity) || !setStr(req.p->namespace_, c.ns))
    return {};

  const size_t maxCmds =
      sizeof(req.p->commands) / sizeof(req.p->commands[0]);  // 16
  pb_size_t n = 0;
  for (const auto& pc : commands) {
    if (n >= maxCmds) return {};  // over-cap fan-out: refuse rather than drop
    auto& cmd = req.p->commands[n];
    switch (pc.kind) {
      case mwf_core::ProtoCommand::Kind::ScheduleActivity: {
        cmd.command_type = kCmdScheduleActivityTask;
        cmd.which_attributes =
            mwf_device_v1_Command_schedule_activity_task_command_attributes_tag;
        auto& s = cmd.attributes.schedule_activity_task_command_attributes;
        if (!setStr(s.activity_id, std::to_string(pc.seq))) return {};
        s.has_activity_type = true;
        if (!setStr(s.activity_type.name, pc.activity_name)) return {};
        s.has_task_queue = true;
        if (!setStr(s.task_queue.name, pc.activity_task_queue)) return {};
        s.has_input = true;
        s.input.payloads_count = 1;
        if (!fillPayload(s.input.payloads[0], pc.input)) return {};
        s.has_schedule_to_close_timeout = true;
        s.schedule_to_close_timeout.seconds = kScheduleToCloseSecs;
        s.has_start_to_close_timeout = true;
        s.start_to_close_timeout.seconds = kStartToCloseSecs;
        break;
      }
      case mwf_core::ProtoCommand::Kind::CompleteWorkflow: {
        cmd.command_type = kCmdCompleteWorkflowExecution;
        cmd.which_attributes =
            mwf_device_v1_Command_complete_workflow_execution_command_attributes_tag;
        auto& w = cmd.attributes.complete_workflow_execution_command_attributes;
        w.has_result = true;
        w.result.payloads_count = 1;
        if (!fillPayload(w.result.payloads[0], pc.result)) return {};
        break;
      }
      case mwf_core::ProtoCommand::Kind::FailWorkflow: {
        cmd.command_type = kCmdFailWorkflowExecution;
        cmd.which_attributes =
            mwf_device_v1_Command_fail_workflow_execution_command_attributes_tag;
        auto& f = cmd.attributes.fail_workflow_execution_command_attributes;
        f.has_failure = true;
        // Truncate an over-long message rather than fail the whole respond.
        std::string msg =
            pc.failure.substr(0, sizeof(f.failure.message) - 1);
        setStr(f.failure.message, msg);
        setStr(f.failure.source, std::string("mwf-esp-workflow"));
        break;
      }
      case mwf_core::ProtoCommand::Kind::StartTimer: {
        // v1 loop never emits this; handled for seam completeness.
        cmd.command_type = kCmdStartTimer;
        cmd.which_attributes =
            mwf_device_v1_Command_start_timer_command_attributes_tag;
        auto& t = cmd.attributes.start_timer_command_attributes;
        if (!setStr(t.timer_id, pc.timer_id)) return {};
        t.has_start_to_fire_timeout = true;
        t.start_to_fire_timeout.seconds =
            static_cast<int64_t>(pc.timer_ms / 1000);
        t.start_to_fire_timeout.nanos =
            static_cast<int32_t>((pc.timer_ms % 1000) * 1000000);
        break;
      }
    }
    ++n;
  }
  req.p->commands_count = n;
  return encodeMsg(mwf_device_v1_RespondWorkflowTaskCompletedRequest_fields,
                   req.p);
}

mwf::Bytes NanopbWorkflowAdapter::buildSignalWorkflowRequest(
    const mwf_core::SignalRequest& s) {
  // ~40 KB at the caps (an embedded Payloads[4]) — PSRAM on device, never the
  // ESP32 task stack.
  BigBuf<mwf_device_v1_SignalWorkflowExecutionRequest> req;
  if (!req) return {};
  *req.p = mwf_device_v1_SignalWorkflowExecutionRequest_init_zero;

  if (!setStr(req.p->namespace_, s.ns) ||
      !setStr(req.p->signal_name, s.signal_name) ||
      !setStr(req.p->identity, s.identity))
    return {};
  req.p->has_workflow_execution = true;
  if (!setStr(req.p->workflow_execution.workflow_id, s.workflow_id)) return {};
  // run_id is optional (empty ⇒ target the latest run).
  if (!s.run_id.empty() && !setStr(req.p->workflow_execution.run_id, s.run_id))
    return {};
  req.p->has_input = true;
  req.p->input.payloads_count = 1;
  if (!fillPayload(req.p->input.payloads[0], s.input)) return {};
  return encodeMsg(mwf_device_v1_SignalWorkflowExecutionRequest_fields, req.p);
}

}  // namespace mwf_proto

// core: WorkflowLoop — the Waiting (blocked-on-wait_signal) outcome, end to end
// through the loop (cookbook 04, durable human-in-the-loop).
//
// The WorkflowLoop must distinguish a wait_signal PAUSE (a human hasn't approved
// yet) from an in-flight-activity wait — both respond with an EMPTY command list —
// so the firmware can display "waiting for the button" and know WHICH execution to
// signal. This drives the REAL loop + REAL PayloadCodecV1 with a fake transport /
// adapter over a hand-authored SIGNALED history:
//   * the unsignalled prefix  → tick reports Waiting, carrying workflow_id +
//     signal_name, having responded with zero commands (the durable pause),
//   * feeding the signalled prefix on the SAME durable store → tick Completes,
//     proving the pause survives + the signal unblocks (the reboot-while-paused
//     resume path is exercised: tick 2 restores the state tick 1 persisted).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mwf_codec/payload_codec.h"
#include "mwf_core/detail/json_util.h"
#include "mwf_core/workflow_loop.h"
#include "test_util.h"

using mwf::Bytes;
using mwf_core::History;
using mwf_core::ProtoCommand;
using mwf_core::WorkflowConfig;
using mwf_core::WorkflowLoop;
using mwf_core::WorkflowSpec;
using mwf_core::WorkflowTaskInfo;
using mwf_core::WorkflowTickOutcome;

namespace {

Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
std::string str(const Bytes& b) { return std::string(b.begin(), b.end()); }

bool jsonEq(const std::string& a, const std::string& b) {
  auto pa = mwf_core::jsonutil::parse(a);
  auto pb = mwf_core::jsonutil::parse(b);
  return pa.ok && pb.ok && pa.value == pb.value;
}

// wait_signal("approve") -> complete(/results/approval). The signal payload
// binds under /results/approval, so the completion carries the human's decision.
const char* kApprovalSpec = R"({
  "name": "WaitApproval",
  "steps": [
    {"type": "wait_signal", "signal": "approve", "id": "approval"},
    {"type": "complete", "result_from": "/results/approval"}
  ]
})";

// A run that gets signalled and completes. Payloads are base64 of raw JSON:
//   signal input {"approved":true,"by":"button"} -> eyJhcHByb3ZlZCI6dHJ1ZSwiYnkiOiJidXR0b24ifQ==
//   wf result    same object                     -> (identical)
const char* kApprovalHistory = R"({"events":[
  {"eventId":"1","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_STARTED",
   "workflowExecutionStartedEventAttributes":{
     "workflowType":{"name":"WaitApproval"},"workflowId":"hitl-exec-7f",
     "originalExecutionRunId":"run-hitl"}},
  {"eventId":"2","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED",
   "workflowTaskScheduledEventAttributes":{}},
  {"eventId":"3","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED",
   "workflowTaskStartedEventAttributes":{"scheduledEventId":"2"}},
  {"eventId":"4","eventType":"EVENT_TYPE_WORKFLOW_TASK_COMPLETED",
   "workflowTaskCompletedEventAttributes":{"scheduledEventId":"2","startedEventId":"3"}},
  {"eventId":"5","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_SIGNALED",
   "workflowExecutionSignaledEventAttributes":{"signalName":"approve",
     "input":{"payloads":[{"metadata":{"encoding":"anNvbi9wbGFpbg=="},
       "data":"eyJhcHByb3ZlZCI6dHJ1ZSwiYnkiOiJidXR0b24ifQ=="}]}}},
  {"eventId":"6","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED",
   "workflowTaskScheduledEventAttributes":{}},
  {"eventId":"7","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED",
   "workflowTaskStartedEventAttributes":{"scheduledEventId":"6"}},
  {"eventId":"8","eventType":"EVENT_TYPE_WORKFLOW_TASK_COMPLETED",
   "workflowTaskCompletedEventAttributes":{"scheduledEventId":"6","startedEventId":"7"}},
  {"eventId":"9","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_COMPLETED",
   "workflowExecutionCompletedEventAttributes":{
     "result":{"payloads":[{"metadata":{"encoding":"anNvbi9wbGFpbg=="},
       "data":"eyJhcHByb3ZlZCI6dHJ1ZSwiYnkiOiJidXR0b24ifQ=="}]}}}
]})";

constexpr size_t kPausedPrefix = 4;      // Started + first WFT closed, NO signal
constexpr size_t kSignalledPrefix = 5;   // the signal has landed (event 5)

// Fake adapter that hands the loop a chosen prefix of the authored history,
// carrying the execution ids from event 1. Captures the outbound commands.
struct WaitSignalAdapter : mwf_core::IWorkflowProtoAdapter {
  History task_history;
  std::string workflow_type = "WaitApproval";
  std::string workflow_id;
  std::string run_id;
  bool respond_called = false;
  std::vector<ProtoCommand> last_commands;

  Bytes buildPollWorkflowRequest(const WorkflowConfig&) override { return bytes("POLL"); }

  mwf::Result<WorkflowTaskInfo> parseWorkflowTask(const Bytes&) override {
    WorkflowTaskInfo t;
    t.empty = false;
    t.task_token = bytes("tok");
    t.workflow_type = workflow_type;
    t.workflow_id = workflow_id;
    t.run_id = run_id;
    t.history = task_history;  // copy
    return mwf::Result<WorkflowTaskInfo>::success(std::move(t));
  }

  Bytes buildRespondWorkflowCompleted(const WorkflowConfig&, const Bytes&,
                                      const std::vector<ProtoCommand>& c) override {
    respond_called = true;
    last_commands = c;
    return bytes("RESP");
  }
};

struct FakeTransport : mwf::ITransport {
  mwf::GrpcResult poll_result{0, "", {}};
  mwf::GrpcResult respond_result{0, "", {}};
  mwf::GrpcResult call(std::string_view fullMethod, const Bytes&, const mwf::Metadata&,
                       int) override {
    if (fullMethod == mwf_core::kMethodRespondWorkflowTaskCompleted) return respond_result;
    return poll_result;  // PollWorkflowTaskQueue → any non-empty body triggers parse
  }
  void close() override {}
};

History loadOrDie(std::string_view json) {
  auto h = mwf_core::loadHistoryJson(json);
  if (!h) throw std::runtime_error("history load failed: " + h.error);
  return std::move(h.value);
}

struct WaitingLoopTest : ::testing::Test {
  FakeTransport transport;
  mwf_codec::PayloadCodecV1 codec;  // the REAL codec (wf_v1 envelope on complete)
  WaitSignalAdapter adapter;
  mwf_test::MemStore store;
  WorkflowSpec spec = mwf_test::parseSpecOrDie(kApprovalSpec);
  History full = loadOrDie(kApprovalHistory);
  WorkflowConfig config{"default", "wf-tq", "wf@host", 70000, 10000, "", {}};

  mwf_core::SpecProvider provider() {
    return [this](std::string_view t) -> const WorkflowSpec* {
      return t == spec.name ? &spec : nullptr;
    };
  }
  WorkflowLoop makeLoop() {
    // A non-empty poll body so the adapter's parseWorkflowTask is reached.
    transport.poll_result = {0, "", bytes("WF-TASK")};
    return WorkflowLoop(transport, codec, adapter, store, provider(), config);
  }
};

}  // namespace

// The unsignalled prefix → the loop reports Waiting, having responded with zero
// commands, and surfaces the workflow_id (signal target) + the awaited name.
TEST_F(WaitingLoopTest, BlockedOnSignalReportsWaitingWithWorkflowId) {
  adapter.task_history = full.prefix(kPausedPrefix);
  adapter.workflow_id = full.events[0].workflow_id;  // "hitl-exec-7f"
  adapter.run_id = full.events[0].run_id;

  auto r = makeLoop().runOnce(1000);

  EXPECT_EQ(r.outcome, WorkflowTickOutcome::Waiting);
  EXPECT_EQ(r.workflow_id, "hitl-exec-7f");  // exactly what ClientOps must signal
  EXPECT_EQ(r.signal_name, "approve");
  EXPECT_EQ(r.workflow_type, "WaitApproval");
  // A pause is a real WFT response with an EMPTY command list (waiting == the
  // absence of a completion command), NOT a no-op.
  ASSERT_TRUE(adapter.respond_called);
  EXPECT_TRUE(adapter.last_commands.empty());
  // The durable state was persisted under the run so a reboot resumes to the pause.
  EXPECT_TRUE(store.get("exec/" + std::string(full.events[0].run_id)).has_value());
}

// Feeding the signalled prefix on the SAME store → the loop unblocks + completes,
// carrying the signal payload. tick 2 restores the state tick 1 persisted, so this
// also proves the reboot-while-paused resume path.
TEST_F(WaitingLoopTest, SignalThenCompletesCarryingPayload) {
  // tick 1: pause.
  adapter.task_history = full.prefix(kPausedPrefix);
  adapter.workflow_id = full.events[0].workflow_id;
  adapter.run_id = full.events[0].run_id;
  ASSERT_EQ(makeLoop().runOnce(1000).outcome, WorkflowTickOutcome::Waiting);

  // tick 2: the signal has arrived — same durable store (resume).
  adapter.task_history = full.prefix(kSignalledPrefix);
  adapter.respond_called = false;
  adapter.last_commands.clear();
  auto r = makeLoop().runOnce(1000);

  EXPECT_EQ(r.outcome, WorkflowTickOutcome::Completed);
  EXPECT_EQ(r.workflow_id, "hitl-exec-7f");
  ASSERT_EQ(adapter.last_commands.size(), 1u);
  EXPECT_EQ(adapter.last_commands[0].kind, ProtoCommand::Kind::CompleteWorkflow);
  EXPECT_TRUE(jsonEq(str(adapter.last_commands[0].result.data),
                     R"({"approved":true,"by":"button"})"));
  EXPECT_EQ(adapter.last_commands[0].result.metadata.at("encoding"), "json/wf_v1");
}

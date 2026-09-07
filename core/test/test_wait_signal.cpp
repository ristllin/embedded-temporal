// test_wait_signal.cpp — the wait_signal grammar END-TO-END through the replay
// engine (durable human-in-the-loop). Uses a hand-authored SIGNALED history
// fixture modelled on the shape of the golden histories (Started →
// WorkflowTask{Scheduled,Started,Completed} → WorkflowExecutionSignaled →
// WorkflowTask{...} → WorkflowExecutionCompleted). Proves:
//   (a) a wait_signal workflow BLOCKS (no completion command) with the signal absent,
//   (b) UNBLOCKS + completes when present, binding the signal payload (incl. bind_to),
//   (c) determinism (decide twice == same),
//   (d) EngineState serialize→restore across the PAUSED point resumes correctly,
//       fed either the full prefix or only the event suffix,
//   (+) multiple same-name signals bind to same-name steps in history order.
#include <gtest/gtest.h>

#include "mwf_core/detail/json_util.h"
#include "mwf_core/replay_engine.h"
#include "test_util.h"

using mwf_core::Command;
using mwf_core::Decision;
using mwf_core::EngineState;
using mwf_core::History;
using mwf_core::ReplayEngine;
using mwf_core::WorkflowSpec;

namespace {

struct Engines {
  mwf_test::BombClock clock;  // throws if the v1 walk ever consults it
  mwf_test::BombRandom random;
  ReplayEngine eng{clock, random};
};

bool jsonEq(const std::string& a, const std::string& b) {
  auto pa = mwf_core::jsonutil::parse(a);
  auto pb = mwf_core::jsonutil::parse(b);
  return pa.ok && pb.ok && pa.value == pb.value;
}

// Events strictly after `event_id` — the delta a rebooted worker would see.
History suffixAfter(const History& h, int64_t event_id) {
  History out;
  for (const auto& e : h.events)
    if (e.event_id > event_id) out.events.push_back(e);
  return out;
}

Decision decideOrDie(ReplayEngine& eng, const WorkflowSpec& spec, const History& h) {
  auto r = eng.decide(spec, h);
  EXPECT_TRUE(r.ok) << r.error;
  if (!r.ok) throw std::runtime_error(r.error);
  return std::move(r.value);
}

History loadOrDie(std::string_view json) {
  auto h = mwf_core::loadHistoryJson(json);
  if (!h) throw std::runtime_error("history load failed: " + h.error);
  return std::move(h.value);
}

// wait_signal("approve") -> complete(/results/decision). `bind_to:"decision"`
// aliases the signal payload, so the trailing complete reading /results/decision
// only resolves if bind_to bound it.
const char* kApprovalSpec = R"({
  "name": "WaitApproval",
  "steps": [
    {"type": "wait_signal", "signal": "approve", "id": "approval", "bind_to": "decision"},
    {"type": "complete", "result_from": "/results/decision"}
  ]
})";

// A full run that DID get signalled and completed. Payloads (base64 of raw JSON):
//   input          "req-1"            -> InJlcS0xIg==
//   signal input   {"approved":true}  -> eyJhcHByb3ZlZCI6dHJ1ZX0=
//   wf result      {"approved":true}  -> eyJhcHByb3ZlZCI6dHJ1ZX0=
const char* kApprovalHistory = R"({"events":[
  {"eventId":"1","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_STARTED",
   "workflowExecutionStartedEventAttributes":{
     "workflowType":{"name":"WaitApproval"},"workflowId":"wf-sig",
     "originalExecutionRunId":"run-sig",
     "input":{"payloads":[{"metadata":{"encoding":"anNvbi9wbGFpbg=="},"data":"InJlcS0xIg=="}]}}},
  {"eventId":"2","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED",
   "workflowTaskScheduledEventAttributes":{}},
  {"eventId":"3","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED",
   "workflowTaskStartedEventAttributes":{"scheduledEventId":"2"}},
  {"eventId":"4","eventType":"EVENT_TYPE_WORKFLOW_TASK_COMPLETED",
   "workflowTaskCompletedEventAttributes":{"scheduledEventId":"2","startedEventId":"3"}},
  {"eventId":"5","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_SIGNALED",
   "workflowExecutionSignaledEventAttributes":{"signalName":"approve",
     "input":{"payloads":[{"metadata":{"encoding":"anNvbi9wbGFpbg=="},"data":"eyJhcHByb3ZlZCI6dHJ1ZX0="}]}}},
  {"eventId":"6","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED",
   "workflowTaskScheduledEventAttributes":{}},
  {"eventId":"7","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED",
   "workflowTaskStartedEventAttributes":{"scheduledEventId":"6"}},
  {"eventId":"8","eventType":"EVENT_TYPE_WORKFLOW_TASK_COMPLETED",
   "workflowTaskCompletedEventAttributes":{"scheduledEventId":"6","startedEventId":"7"}},
  {"eventId":"9","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_COMPLETED",
   "workflowExecutionCompletedEventAttributes":{
     "result":{"payloads":[{"metadata":{"encoding":"anNvbi9wbGFpbg=="},"data":"eyJhcHByb3ZlZCI6dHJ1ZX0="}]}}}
]})";

// The paused point: Started + the first workflow task closed, NO signal yet.
constexpr size_t kPausedPrefix = 4;
// The signal has arrived (event 5) — the workflow can unblock now.
constexpr size_t kSignalledPrefix = 5;

}  // namespace

// ── (a) blocks when the signal is absent ─────────────────────────────────────
TEST(WaitSignal, BlocksWhenSignalAbsent) {
  Engines fx;
  WorkflowSpec spec = mwf_test::parseSpecOrDie(kApprovalSpec);
  History h = loadOrDie(kApprovalHistory);

  Decision d = decideOrDie(fx.eng, spec, h.prefix(kPausedPrefix));
  EXPECT_TRUE(d.commands.empty()) << "a blocked wait_signal must emit no command";
  EXPECT_FALSE(d.workflow_finished);
  // The step is unbound — the pause lives purely in "results has no binding yet".
  EXPECT_TRUE(jsonEq(d.state.results_json, "{}"));
  EXPECT_FALSE(d.state.has_pending);  // no in-flight activity — a signal wait
}

// ── (b) unblocks + completes when the signal is present, binding its payload ──
TEST(WaitSignal, UnblocksAndCompletesBindingPayload) {
  Engines fx;
  WorkflowSpec spec = mwf_test::parseSpecOrDie(kApprovalSpec);
  History h = loadOrDie(kApprovalHistory);

  // The moment the signal lands, the walk unblocks and drives the workflow to
  // completion with the signal payload (reached via the bind_to alias).
  Decision d = decideOrDie(fx.eng, spec, h.prefix(kSignalledPrefix));
  ASSERT_EQ(d.commands.size(), 1u);
  EXPECT_EQ(d.commands[0].kind, Command::Kind::CompleteWorkflow);
  EXPECT_TRUE(jsonEq(d.commands[0].result_json, R"({"approved":true})"));
  // The payload bound under BOTH the step id and the bind_to alias.
  EXPECT_TRUE(jsonEq(d.state.results_json,
                     R"({"approval":{"approved":true},"decision":{"approved":true}})"));

  // The already-terminal full history emits nothing and reports finished.
  Decision done = decideOrDie(fx.eng, spec, h);
  EXPECT_TRUE(done.commands.empty());
  EXPECT_TRUE(done.workflow_finished);
}

// ── (c) determinism: decide twice == same commands + same serialized state ───
TEST(WaitSignal, DecideIsDeterministic) {
  Engines fx;
  WorkflowSpec spec = mwf_test::parseSpecOrDie(kApprovalSpec);
  History h = loadOrDie(kApprovalHistory);

  for (size_t k = 1; k <= h.events.size(); ++k) {
    auto r1 = fx.eng.decide(spec, h.prefix(k));
    auto r2 = fx.eng.decide(spec, h.prefix(k));
    ASSERT_TRUE(r1.ok) << "prefix " << k << ": " << r1.error;
    ASSERT_TRUE(r2.ok) << "prefix " << k << ": " << r2.error;
    EXPECT_EQ(r1.value.commands, r2.value.commands) << "prefix " << k;
    EXPECT_EQ(ReplayEngine::serializeState(r1.value.state),
              ReplayEngine::serializeState(r2.value.state)) << "prefix " << k;
  }
}

// ── (d) EngineState round-trips THROUGH the paused point and resumes ─────────
TEST(WaitSignal, SerializeRestoreAcrossPausedPointResumes) {
  Engines fx;
  WorkflowSpec spec = mwf_test::parseSpecOrDie(kApprovalSpec);
  History h = loadOrDie(kApprovalHistory);

  // Snapshot AT the paused point (blocked on the signal), then reboot: bytes →
  // restore. The pause is captured entirely by results_json + last_processed.
  Decision paused = decideOrDie(fx.eng, spec, h.prefix(kPausedPrefix));
  ASSERT_TRUE(paused.commands.empty());
  auto restored = ReplayEngine::restoreState(ReplayEngine::serializeState(paused.state));
  ASSERT_TRUE(restored.ok) << restored.error;
  EXPECT_EQ(restored.value.last_processed_event_id, 4);

  const std::string kExpected = R"({"approved":true})";

  // Resume fed the FULL history prefix (server re-delivers the whole history).
  auto viaFull = fx.eng.decide(spec, h.prefix(kSignalledPrefix), restored.value);
  ASSERT_TRUE(viaFull.ok) << viaFull.error;
  ASSERT_EQ(viaFull.value.commands.size(), 1u);
  EXPECT_EQ(viaFull.value.commands[0].kind, Command::Kind::CompleteWorkflow);
  EXPECT_TRUE(jsonEq(viaFull.value.commands[0].result_json, kExpected));

  // Resume fed ONLY the event SUFFIX after the snapshot (the reboot delta) — the
  // signal (event 5) is the only new event, yet the workflow still completes.
  History suffix = suffixAfter(h.prefix(kSignalledPrefix), restored.value.last_processed_event_id);
  ASSERT_EQ(suffix.events.size(), 1u);  // just the signal
  auto viaSuffix = fx.eng.decide(spec, suffix, restored.value);
  ASSERT_TRUE(viaSuffix.ok) << viaSuffix.error;
  ASSERT_EQ(viaSuffix.value.commands.size(), 1u);
  EXPECT_EQ(viaSuffix.value.commands[0].kind, Command::Kind::CompleteWorkflow);
  EXPECT_TRUE(jsonEq(viaSuffix.value.commands[0].result_json, kExpected));

  // And it matches a from-scratch replay at the same horizon — resume is exact.
  Decision scratch = decideOrDie(fx.eng, spec, h.prefix(kSignalledPrefix));
  EXPECT_EQ(viaFull.value.commands, scratch.commands);
  EXPECT_EQ(viaSuffix.value.commands, scratch.commands);
}

// ── (+) multiple same-name signals bind to same-name steps in history order ──
// Two wait_signal steps both wait on "approve"; two approve signals arrive. The
// first step binds the first signal, the second binds the second — exercised
// across a paused-and-resumed point to prove the in-order claim survives reboot.
TEST(WaitSignal, MultipleSameNameSignalsBindInOrder) {
  Engines fx;
  const char* twoStepSpec = R"({
    "name": "TwoApprovals",
    "steps": [
      {"type": "wait_signal", "signal": "approve", "id": "first"},
      {"type": "wait_signal", "signal": "approve", "id": "second"},
      {"type": "complete", "result_from": "/results/second"}
    ]})";
  // signal #1 {"n":1} -> eyJuIjoxfQ==   ;  signal #2 {"n":2} -> eyJuIjoyfQ==
  const char* twoSigHistory = R"({"events":[
    {"eventId":"1","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_STARTED",
     "workflowExecutionStartedEventAttributes":{
       "workflowType":{"name":"TwoApprovals"},"workflowId":"wf-2","originalExecutionRunId":"run-2"}},
    {"eventId":"2","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
    {"eventId":"3","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"2"}},
    {"eventId":"4","eventType":"EVENT_TYPE_WORKFLOW_TASK_COMPLETED","workflowTaskCompletedEventAttributes":{"scheduledEventId":"2","startedEventId":"3"}},
    {"eventId":"5","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_SIGNALED",
     "workflowExecutionSignaledEventAttributes":{"signalName":"approve",
       "input":{"payloads":[{"metadata":{},"data":"eyJuIjoxfQ=="}]}}},
    {"eventId":"6","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
    {"eventId":"7","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"6"}},
    {"eventId":"8","eventType":"EVENT_TYPE_WORKFLOW_TASK_COMPLETED","workflowTaskCompletedEventAttributes":{"scheduledEventId":"6","startedEventId":"7"}},
    {"eventId":"9","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_SIGNALED",
     "workflowExecutionSignaledEventAttributes":{"signalName":"approve",
       "input":{"payloads":[{"metadata":{},"data":"eyJuIjoyfQ=="}]}}},
    {"eventId":"10","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
    {"eventId":"11","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"10"}},
    {"eventId":"12","eventType":"EVENT_TYPE_WORKFLOW_TASK_COMPLETED","workflowTaskCompletedEventAttributes":{"scheduledEventId":"10","startedEventId":"11"}}
  ]})";
  WorkflowSpec spec = mwf_test::parseSpecOrDie(twoStepSpec);
  History h = loadOrDie(twoSigHistory);

  // Only signal #1 present (through event 8): first step bound, second BLOCKS.
  Decision one = decideOrDie(fx.eng, spec, h.prefix(8));
  EXPECT_TRUE(one.commands.empty()) << "still waiting on the 2nd approve";
  EXPECT_TRUE(jsonEq(one.state.results_json, R"({"first":{"n":1}})"));

  // Reboot at the paused point, then feed the whole history: both bind, in order.
  auto restored = ReplayEngine::restoreState(ReplayEngine::serializeState(one.state));
  ASSERT_TRUE(restored.ok) << restored.error;
  auto resumed = fx.eng.decide(spec, h, restored.value);
  ASSERT_TRUE(resumed.ok) << resumed.error;
  ASSERT_EQ(resumed.value.commands.size(), 1u);
  EXPECT_EQ(resumed.value.commands[0].kind, Command::Kind::CompleteWorkflow);
  EXPECT_TRUE(jsonEq(resumed.value.commands[0].result_json, R"({"n":2})"))
      << "the 2nd step must bind the 2nd approve, not the 1st";

  // From-scratch full replay agrees.
  Decision scratch = decideOrDie(fx.eng, spec, h);
  EXPECT_EQ(resumed.value.commands, scratch.commands);
}

// test_replay_bugfixes.cpp — regression tests for the replay-engine correctness
// fixes (security review, 2026-07):
//   Bug #1  a wait_signal signal SEEN before its step is reachable is lost across
//           resume → the workflow hangs while a from-scratch replay completes
//           (determinism violation). Covered: (A) signal arrives while an
//           activity is still in-flight, carried across serialize→restore; (B)
//           two distinct signals arrive out of order across a restore.
//   Bug #2  restoreState (declared "Never throws") threw nlohmann type_error on a
//           type-corrupt state blob. Covered: every mistyped field → clean
//           Result::failure, no exception.
//   LOW     duplicate binding id on a single execution path silently misbinds
//           (parse-time rejection; shared ids across conditional branches stay
//           legal); numeric predicate compared via double mis-orders int64 past
//           2^53 (now compared as int64 when both sides are integral).
#include <gtest/gtest.h>

#include "mwf_core/detail/json_util.h"
#include "mwf_core/history.h"
#include "mwf_core/replay_engine.h"
#include "mwf_core/workflow_spec.h"
#include "test_util.h"

using mwf_core::Command;
using mwf_core::Decision;
using mwf_core::EngineState;
using mwf_core::History;
using mwf_core::ReplayEngine;
using mwf_core::WorkflowSpec;

namespace {

struct Engines {
  mwf_test::BombClock clock;  // aborts if the v1 walk ever consults a determinism seam
  mwf_test::BombRandom random;
  ReplayEngine eng{clock, random};
};

bool jsonEq(const std::string& a, const std::string& b) {
  auto pa = mwf_core::jsonutil::parse(a);
  auto pb = mwf_core::jsonutil::parse(b);
  return pa.ok && pb.ok && pa.value == pb.value;
}

History loadOrDie(std::string_view json) {
  auto h = mwf_core::loadHistoryJson(json);
  if (!h) throw std::runtime_error("history load failed: " + h.error);
  return std::move(h.value);
}

Decision decideOrDie(ReplayEngine& eng, const WorkflowSpec& spec, const History& h) {
  auto r = eng.decide(spec, h);
  EXPECT_TRUE(r.ok) << r.error;
  if (!r.ok) throw std::runtime_error(r.error);
  return std::move(r.value);
}

// Round-trip a snapshot through the durable bytes, exactly as a power cycle does.
EngineState reboot(const EngineState& s) {
  auto restored = ReplayEngine::restoreState(ReplayEngine::serializeState(s));
  EXPECT_TRUE(restored.ok) << restored.error;
  return std::move(restored.value);
}

// ── Bug #1 (A): signal delivered while an activity is still in-flight ──────────
// Spec: activity A → wait_signal("go") → complete(/results/go). "go" (event 7)
// arrives while A is Started (the COMMON case: Temporal delivers signals
// promptly). Tick 1 blocks on A, but the signal advances the watermark. A resume
// must still consume "go" once A completes — not hang.
const char* kActThenSignalSpec = R"({
  "name": "ActThenSignal",
  "steps": [
    {"type":"activity","name":"A","id":"a","args":123},
    {"type":"wait_signal","signal":"go","id":"go"},
    {"type":"complete","result_from":"/results/go"}
  ]
})";

// A Started (events 5/6) + "go" already signalled (event 7); A NOT yet completed.
const char* kActInflightWithSignal = R"({"events":[
  {"eventId":"1","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_STARTED",
   "workflowExecutionStartedEventAttributes":{"workflowType":{"name":"ActThenSignal"},
     "workflowId":"wf-x","originalExecutionRunId":"run-x"}},
  {"eventId":"2","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
  {"eventId":"3","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"2"}},
  {"eventId":"4","eventType":"EVENT_TYPE_WORKFLOW_TASK_COMPLETED","workflowTaskCompletedEventAttributes":{"scheduledEventId":"2","startedEventId":"3"}},
  {"eventId":"5","eventType":"EVENT_TYPE_ACTIVITY_TASK_SCHEDULED","activityTaskScheduledEventAttributes":{"activityType":{"name":"A"},"activityId":"1"}},
  {"eventId":"6","eventType":"EVENT_TYPE_ACTIVITY_TASK_STARTED","activityTaskStartedEventAttributes":{"scheduledEventId":"5"}},
  {"eventId":"7","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_SIGNALED",
   "workflowExecutionSignaledEventAttributes":{"signalName":"go","input":{"payloads":[{"metadata":{},"data":"eyJuIjoxfQ=="}]}}},
  {"eventId":"8","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
  {"eventId":"9","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"8"}}
]})";

// Same, plus A completes (event 10) — the delta a rebooted worker would replay.
const char* kActCompletedWithSignal = R"({"events":[
  {"eventId":"1","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_STARTED",
   "workflowExecutionStartedEventAttributes":{"workflowType":{"name":"ActThenSignal"},
     "workflowId":"wf-x","originalExecutionRunId":"run-x"}},
  {"eventId":"2","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
  {"eventId":"3","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"2"}},
  {"eventId":"4","eventType":"EVENT_TYPE_WORKFLOW_TASK_COMPLETED","workflowTaskCompletedEventAttributes":{"scheduledEventId":"2","startedEventId":"3"}},
  {"eventId":"5","eventType":"EVENT_TYPE_ACTIVITY_TASK_SCHEDULED","activityTaskScheduledEventAttributes":{"activityType":{"name":"A"},"activityId":"1"}},
  {"eventId":"6","eventType":"EVENT_TYPE_ACTIVITY_TASK_STARTED","activityTaskStartedEventAttributes":{"scheduledEventId":"5"}},
  {"eventId":"7","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_SIGNALED",
   "workflowExecutionSignaledEventAttributes":{"signalName":"go","input":{"payloads":[{"metadata":{},"data":"eyJuIjoxfQ=="}]}}},
  {"eventId":"8","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
  {"eventId":"9","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"8"}},
  {"eventId":"10","eventType":"EVENT_TYPE_ACTIVITY_TASK_COMPLETED","activityTaskCompletedEventAttributes":{"scheduledEventId":"5","result":{"payloads":[{"metadata":{},"data":"eyJuIjoxfQ=="}]}}},
  {"eventId":"11","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
  {"eventId":"12","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"11"}}
]})";

TEST(ReplayBug1SignalWhileActivityInflight, ResumesAcrossSerializeRestore) {
  Engines fx;
  WorkflowSpec spec = mwf_test::parseSpecOrDie(kActThenSignalSpec);
  History h1 = loadOrDie(kActInflightWithSignal);
  History h2 = loadOrDie(kActCompletedWithSignal);

  // Tick 1: blocked on the in-flight activity (NOT a signal wait). The signal has
  // advanced the watermark but is not yet claimed — results carries no "go".
  Decision d1 = decideOrDie(fx.eng, spec, h1);
  EXPECT_TRUE(d1.commands.empty());
  EXPECT_FALSE(d1.waiting_on_signal) << "the walk stops at the in-flight activity, not the signal";
  EXPECT_TRUE(d1.state.has_pending);
  EXPECT_TRUE(jsonEq(d1.state.results_json, "{}"));
  EXPECT_EQ(d1.state.last_processed_event_id, 9);
  EXPECT_TRUE(d1.state.signal_consumed.empty()) << "the signal step was never reached";

  // Power cycle, then resume against the full history (A now completed). Before
  // the fix this HUNG (the pre-watermark signal was dropped from the suffix).
  EngineState resumed = reboot(d1.state);
  auto d2 = fx.eng.decide(spec, h2, resumed);
  ASSERT_TRUE(d2.ok) << d2.error;
  ASSERT_EQ(d2.value.commands.size(), 1u);
  EXPECT_EQ(d2.value.commands[0].kind, Command::Kind::CompleteWorkflow);
  EXPECT_TRUE(jsonEq(d2.value.commands[0].result_json, R"({"n":1})"));

  // Determinism oracle: resume matches a from-scratch replay at the same horizon.
  Decision scratch = decideOrDie(fx.eng, spec, h2);
  EXPECT_EQ(d2.value.commands, scratch.commands);
}

// ── Bug #1 (B): two distinct signals arrive OUT OF ORDER across a restore ──────
// wait a → wait b → complete(/results/b). "b" arrives first (event 5); the run
// blocks on "a". "a" arrives later (event 8). A resume must claim BOTH, in the
// spec's step order, from the full history — not hang on "b".
const char* kTwoApproversSpec = R"({"name":"TwoApprovers","steps":[
  {"type":"wait_signal","signal":"a","id":"a"},
  {"type":"wait_signal","signal":"b","id":"b"},
  {"type":"complete","result_from":"/results/b"}]})";

const char* kOnlyBSignalled = R"({"events":[
  {"eventId":"1","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_STARTED","workflowExecutionStartedEventAttributes":{"workflowType":{"name":"TwoApprovers"},"workflowId":"wf","originalExecutionRunId":"run"}},
  {"eventId":"2","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
  {"eventId":"3","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"2"}},
  {"eventId":"4","eventType":"EVENT_TYPE_WORKFLOW_TASK_COMPLETED","workflowTaskCompletedEventAttributes":{"scheduledEventId":"2","startedEventId":"3"}},
  {"eventId":"5","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_SIGNALED","workflowExecutionSignaledEventAttributes":{"signalName":"b","input":{"payloads":[{"metadata":{},"data":"eyJuIjoyfQ=="}]}}},
  {"eventId":"6","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
  {"eventId":"7","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"6"}}
]})";

const char* kBThenASignalled = R"({"events":[
  {"eventId":"1","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_STARTED","workflowExecutionStartedEventAttributes":{"workflowType":{"name":"TwoApprovers"},"workflowId":"wf","originalExecutionRunId":"run"}},
  {"eventId":"2","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
  {"eventId":"3","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"2"}},
  {"eventId":"4","eventType":"EVENT_TYPE_WORKFLOW_TASK_COMPLETED","workflowTaskCompletedEventAttributes":{"scheduledEventId":"2","startedEventId":"3"}},
  {"eventId":"5","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_SIGNALED","workflowExecutionSignaledEventAttributes":{"signalName":"b","input":{"payloads":[{"metadata":{},"data":"eyJuIjoyfQ=="}]}}},
  {"eventId":"6","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
  {"eventId":"7","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"6"}},
  {"eventId":"8","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_SIGNALED","workflowExecutionSignaledEventAttributes":{"signalName":"a","input":{"payloads":[{"metadata":{},"data":"eyJuIjoxfQ=="}]}}},
  {"eventId":"9","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
  {"eventId":"10","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"9"}}
]})";

TEST(ReplayBug1OutOfOrderSignals, ResumeClaimsBothInStepOrder) {
  Engines fx;
  WorkflowSpec spec = mwf_test::parseSpecOrDie(kTwoApproversSpec);
  History h1 = loadOrDie(kOnlyBSignalled);
  History h2 = loadOrDie(kBThenASignalled);

  // Tick 1: "b" is present but the walk BLOCKS on "a" (first step). Nothing bound.
  Decision d1 = decideOrDie(fx.eng, spec, h1);
  EXPECT_TRUE(d1.commands.empty());
  EXPECT_TRUE(d1.waiting_on_signal);
  EXPECT_EQ(d1.waiting_signal_name, "a");
  EXPECT_TRUE(jsonEq(d1.state.results_json, "{}"));

  // Power cycle, resume once "a" arrives. Before the fix this stayed BLOCKED on
  // "b" (the earlier "b" fell below the watermark and vanished from the suffix).
  EngineState resumed = reboot(d1.state);
  auto d2 = fx.eng.decide(spec, h2, resumed);
  ASSERT_TRUE(d2.ok) << d2.error;
  ASSERT_EQ(d2.value.commands.size(), 1u) << "resume must complete, not hang on 'b'";
  EXPECT_EQ(d2.value.commands[0].kind, Command::Kind::CompleteWorkflow);
  EXPECT_TRUE(jsonEq(d2.value.commands[0].result_json, R"({"n":2})"));

  Decision scratch = decideOrDie(fx.eng, spec, h2);
  EXPECT_EQ(d2.value.commands, scratch.commands);
}

// ── Bug #2: restoreState honours its "Never throws" contract ──────────────────
TEST(ReplayBug2RestoreState, NeverThrowsOnTypeCorruptBlob) {
  auto B = [](const std::string& s) { return mwf::Bytes(s.begin(), s.end()); };
  auto restore = [&](const std::string& json) {
    // The whole point of the fix: no exception may escape, even on garbage.
    return ReplayEngine::restoreState(B(json));
  };

  // Baseline: a well-formed blob still restores.
  auto ok = restore(R"({"runId":"r","specHash":1,"lastProcessedEventId":3,"nextSeq":2,"resultsJson":"{}"})");
  EXPECT_TRUE(ok.ok) << ok.error;

  // Each mistyped field must return a clean failure — NOT throw type_error.
  const char* corrupt[] = {
      R"({"runId":"r","specHash":1,"lastProcessedEventId":3,"nextSeq":"two","resultsJson":"{}"})",
      R"({"runId":"r","specHash":1,"lastProcessedEventId":"x","nextSeq":2,"resultsJson":"{}"})",
      R"({"runId":"r","specHash":1,"lastProcessedEventId":3,"nextSeq":2,"resultsJson":"{}",
          "pending":{"scheduledEventId":"nope","stepId":"a","activityName":"A"}})",
      R"({"runId":"r","specHash":1,"lastProcessedEventId":3,"nextSeq":2,"resultsJson":"{}",
          "pending":{"scheduledEventId":5,"stepId":7,"activityName":"A"}})",
      R"({"runId":5,"specHash":1,"lastProcessedEventId":3,"nextSeq":2,"resultsJson":"{}"})",
  };
  for (const char* c : corrupt) {
    mwf::Result<EngineState> r{};
    EXPECT_NO_THROW({ r = restore(c); }) << "restoreState must not throw on: " << c;
    EXPECT_FALSE(r.ok) << "a type-corrupt blob must be a clean failure: " << c;
  }

  // A resultsJson-string that is itself corrupt JSON is caught downstream by
  // decide(), not here — restoreState only type-checks the envelope. A valid
  // signalConsumed map round-trips.
  EngineState s;
  s.run_id = "r";
  s.spec_hash = 7;
  s.last_processed_event_id = 9;
  s.next_seq = 3;
  s.signal_consumed["go"] = 2;
  auto rt = ReplayEngine::restoreState(ReplayEngine::serializeState(s));
  ASSERT_TRUE(rt.ok) << rt.error;
  EXPECT_EQ(rt.value.signal_consumed.at("go"), 2u);
}

// ── LOW: duplicate binding id on a single execution path is rejected ──────────
TEST(SpecDuplicateId, StraightLineSequenceRejected) {
  const char* dup = R"({"name":"Dup","steps":[
    {"type":"activity","name":"A","id":"x","args":1},
    {"type":"activity","name":"B","id":"x","args":2},
    {"type":"complete","result":true}]})";
  auto r = mwf_core::parseWorkflowSpec(std::string_view(dup));
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("duplicate binding id"), std::string::npos) << r.error;
}

TEST(SpecDuplicateId, DefaultedActivityIdCollisionRejected) {
  // Both activities default their id to the name "A" — a straight-line collision.
  const char* dup = R"({"name":"Dup","steps":[
    {"type":"activity","name":"A","args":1},
    {"type":"activity","name":"A","args":2},
    {"type":"complete","result":true}]})";
  auto r = mwf_core::parseWorkflowSpec(std::string_view(dup));
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("duplicate binding id"), std::string::npos) << r.error;
}

TEST(SpecDuplicateId, DuplicateAcrossNestedSequenceRejected) {
  const char* dup = R"({"name":"Dup","steps":[
    {"type":"activity","name":"A","id":"x","args":1},
    {"type":"sequence","steps":[{"type":"activity","name":"B","id":"x","args":2}]},
    {"type":"complete","result":true}]})";
  auto r = mwf_core::parseWorkflowSpec(std::string_view(dup));
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("duplicate binding id"), std::string::npos) << r.error;
}

TEST(SpecDuplicateId, DuplicateWaitSignalIdRejected) {
  const char* dup = R"({"name":"Dup","steps":[
    {"type":"wait_signal","signal":"s1","id":"k"},
    {"type":"wait_signal","signal":"s2","id":"k"},
    {"type":"complete","result":true}]})";
  auto r = mwf_core::parseWorkflowSpec(std::string_view(dup));
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("duplicate binding id"), std::string::npos) << r.error;
}

TEST(SpecDuplicateId, SharedIdAcrossConditionalBranchesAccepted) {
  // The DOCUMENTED-legitimate pattern (matches kConditionalSpec): the two
  // branches share id "branch" because only one executes.
  auto r = mwf_core::parseWorkflowSpec(std::string_view(mwf_test::kConditionalSpec));
  EXPECT_TRUE(r.ok) << r.error;
}

TEST(SpecDuplicateId, IdReusedAfterConditionalBranchRejected) {
  // A branch binds "x"; a later straight-line step reuses "x" → on the taken-
  // branch path "x" is bound twice. Must be rejected (the union of branch ids
  // merges back into the enclosing path).
  const char* dup = R"({"name":"Dup","steps":[
    {"type":"conditional",
      "predicate":{"path":"/input","op":"exists"},
      "true_steps":[{"type":"activity","name":"A","id":"x","args":1}],
      "false_steps":[{"type":"activity","name":"B","id":"x","args":2}]},
    {"type":"activity","name":"C","id":"x","args":3},
    {"type":"complete","result":true}]})";
  auto r = mwf_core::parseWorkflowSpec(std::string_view(dup));
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("duplicate binding id"), std::string::npos) << r.error;
}

// ── LOW: numeric predicate compares as int64 past double precision ────────────
// activity result 9007199254740993 (2^53 + 1) vs threshold 2^53. As a double the
// two are indistinguishable (both round to 2^53), so a gt-comparison would be
// FALSE; as int64 it is TRUE. The chosen branch proves which path evalPredicate
// took.
TEST(PredicateInt64, ComparesBeyondDoublePrecision) {
  Engines fx;
  const char* spec = R"({"name":"BigIntCmp","steps":[
    {"type":"activity","name":"measure","id":"m","args":0},
    {"type":"conditional",
      "predicate":{"path":"/results/m","op":"gt","value":9007199254740992},
      "true_steps":[{"type":"complete","result":"above"}],
      "false_steps":[{"type":"complete","result":"below_or_equal"}]}
  ]})";
  // measure completes with 9007199254740993 -> base64("9007199254740993").
  const char* history = R"({"events":[
    {"eventId":"1","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_STARTED","workflowExecutionStartedEventAttributes":{"workflowType":{"name":"BigIntCmp"},"workflowId":"wf","originalExecutionRunId":"run"}},
    {"eventId":"2","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
    {"eventId":"3","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"2"}},
    {"eventId":"4","eventType":"EVENT_TYPE_WORKFLOW_TASK_COMPLETED","workflowTaskCompletedEventAttributes":{"scheduledEventId":"2","startedEventId":"3"}},
    {"eventId":"5","eventType":"EVENT_TYPE_ACTIVITY_TASK_SCHEDULED","activityTaskScheduledEventAttributes":{"activityType":{"name":"measure"},"activityId":"1"}},
    {"eventId":"6","eventType":"EVENT_TYPE_ACTIVITY_TASK_STARTED","activityTaskStartedEventAttributes":{"scheduledEventId":"5"}},
    {"eventId":"7","eventType":"EVENT_TYPE_ACTIVITY_TASK_COMPLETED","activityTaskCompletedEventAttributes":{"scheduledEventId":"5","result":{"payloads":[{"metadata":{},"data":"OTAwNzE5OTI1NDc0MDk5Mw=="}]}}},
    {"eventId":"8","eventType":"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED","workflowTaskScheduledEventAttributes":{}},
    {"eventId":"9","eventType":"EVENT_TYPE_WORKFLOW_TASK_STARTED","workflowTaskStartedEventAttributes":{"scheduledEventId":"8"}}
  ]})";
  WorkflowSpec s = mwf_test::parseSpecOrDie(spec);
  History h = loadOrDie(history);
  Decision d = decideOrDie(fx.eng, s, h);
  ASSERT_EQ(d.commands.size(), 1u);
  EXPECT_EQ(d.commands[0].kind, Command::Kind::CompleteWorkflow);
  EXPECT_TRUE(jsonEq(d.commands[0].result_json, "\"above\""))
      << "int64 gt must see 2^53+1 > 2^53 (a double compare would mis-order it): "
      << d.commands[0].result_json;
}

}  // namespace

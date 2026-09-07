// test_replay_engine.cpp — the replay-vs-oracle suite.
//
// Replays the REAL golden Temporal histories (conformance, captured from a
// live dev server) against the matching declarative specs and asserts the
// engine's Commands against the history's own next events — the replay-
// correctness oracle — for EVERY prefix of all three goldens. Plus durability
// (serialize/restore mid-run == uninterrupted) and determinism/nondeterminism
// checks.
#include <gtest/gtest.h>

#include "mwf_core/detail/json_util.h"
#include "mwf_core/replay_engine.h"
#include "test_util.h"

using mwf_core::Command;
using mwf_core::Decision;
using mwf_core::EngineState;
using mwf_core::EventType;
using mwf_core::History;
using mwf_core::HistoryEvent;
using mwf_core::ReplayEngine;
using mwf_core::WorkflowSpec;

namespace {

struct Engines {
  mwf_test::BombClock clock;   // throws if the v1 walk ever consults it
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

}  // namespace

// ── Linear golden: prefix-by-prefix expectations ─────────────────────────────

TEST(ReplayLinear, AfterStartOnlySchedulesFirstActivity) {
  Engines fx;
  History h = mwf_test::loadGolden("linear");
  WorkflowSpec spec = mwf_test::parseSpecOrDie(mwf_test::kLinearSpec);

  for (size_t k : {size_t(1), size_t(2), size_t(3), size_t(4)}) {
    Decision d = decideOrDie(fx.eng, spec, h.prefix(k));
    ASSERT_EQ(d.commands.size(), 1u) << "prefix " << k;
    EXPECT_EQ(d.commands[0].kind, Command::Kind::ScheduleActivity);
    EXPECT_EQ(d.commands[0].activity_name, "greet");
    EXPECT_EQ(d.commands[0].seq, 1u);
    EXPECT_TRUE(jsonEq(d.commands[0].args_json, "\"world\""));
    EXPECT_FALSE(d.workflow_finished);
  }
}

TEST(ReplayLinear, InFlightActivityEmitsNothing) {
  Engines fx;
  History h = mwf_test::loadGolden("linear");
  WorkflowSpec spec = mwf_test::parseSpecOrDie(mwf_test::kLinearSpec);

  for (size_t k : {size_t(5), size_t(6)}) {  // greet Scheduled / Started, no result yet
    Decision d = decideOrDie(fx.eng, spec, h.prefix(k));
    EXPECT_TRUE(d.commands.empty()) << "prefix " << k;
    EXPECT_TRUE(d.state.has_pending);
    EXPECT_EQ(d.state.pending_activity_name, "greet");
    EXPECT_EQ(d.state.pending_scheduled_event_id, 5);
  }
}

TEST(ReplayLinear, AfterFirstResultSchedulesSecondActivity) {
  Engines fx;
  History h = mwf_test::loadGolden("linear");
  WorkflowSpec spec = mwf_test::parseSpecOrDie(mwf_test::kLinearSpec);

  Decision d = decideOrDie(fx.eng, spec, h.prefix(7));
  ASSERT_EQ(d.commands.size(), 1u);
  EXPECT_EQ(d.commands[0].kind, Command::Kind::ScheduleActivity);
  EXPECT_EQ(d.commands[0].activity_name, "shout");
  EXPECT_EQ(d.commands[0].seq, 2u);  // activityId "2" on the wire — matches golden event 11
  EXPECT_TRUE(jsonEq(d.commands[0].args_json, "\"hello, world\""));
}

TEST(ReplayLinear, AfterAllActivitiesCompletesWorkflow) {
  Engines fx;
  History h = mwf_test::loadGolden("linear");
  WorkflowSpec spec = mwf_test::parseSpecOrDie(mwf_test::kLinearSpec);

  Decision d = decideOrDie(fx.eng, spec, h.prefix(13));
  ASSERT_EQ(d.commands.size(), 1u);
  EXPECT_EQ(d.commands[0].kind, Command::Kind::CompleteWorkflow);
  EXPECT_TRUE(jsonEq(d.commands[0].result_json, "\"HELLO, WORLD!\""));
}

TEST(ReplayLinear, TerminalHistoryEmitsNothing) {
  Engines fx;
  History h = mwf_test::loadGolden("linear");
  WorkflowSpec spec = mwf_test::parseSpecOrDie(mwf_test::kLinearSpec);

  Decision d = decideOrDie(fx.eng, spec, h);
  EXPECT_TRUE(d.commands.empty());
  EXPECT_TRUE(d.workflow_finished);
  EXPECT_EQ(d.state.run_id, "019f7276-05a4-7f32-b62d-213a9e7b918d");
  EXPECT_EQ(d.state.last_processed_event_id, 17);
  // Both results were re-bound during the replay walk.
  EXPECT_TRUE(jsonEq(d.state.results_json,
                     R"({"greet":"hello, world","shout":"HELLO, WORLD!"})"));
}

// ── Conditional goldens: branch choice matches the real Python worker ────────

TEST(ReplayConditional, BranchCommandMatchesWhatTheRealWorkerScheduled) {
  Engines fx;
  WorkflowSpec spec = mwf_test::parseSpecOrDie(mwf_test::kConditionalSpec);

  for (const char* name : {"conditional_even", "conditional_odd"}) {
    History h = mwf_test::loadGolden(name);
    // Prefix through the parity result (event 7): the engine must now pick the
    // branch — compare against the activity the live worker actually scheduled,
    // recorded as history event 11.
    Decision d = decideOrDie(fx.eng, spec, h.prefix(7));
    const HistoryEvent* oracle = h.find(11);
    ASSERT_NE(oracle, nullptr);
    ASSERT_EQ(oracle->type, EventType::ActivityTaskScheduled);
    ASSERT_EQ(d.commands.size(), 1u) << name;
    EXPECT_EQ(d.commands[0].kind, Command::Kind::ScheduleActivity);
    EXPECT_EQ(d.commands[0].activity_name, oracle->activity_name) << name;
    EXPECT_EQ(std::to_string(d.commands[0].seq), oracle->activity_id) << name;
    EXPECT_TRUE(jsonEq(d.commands[0].args_json, oracle->payloads.at(0))) << name;
  }
}

TEST(ReplayConditional, SharedBranchIdFeedsTheTrailingComplete) {
  Engines fx;
  WorkflowSpec spec = mwf_test::parseSpecOrDie(mwf_test::kConditionalSpec);

  History even = mwf_test::loadGolden("conditional_even");
  Decision de = decideOrDie(fx.eng, spec, even.prefix(13));
  ASSERT_EQ(de.commands.size(), 1u);
  EXPECT_EQ(de.commands[0].kind, Command::Kind::CompleteWorkflow);
  EXPECT_TRUE(jsonEq(de.commands[0].result_json, "\"8 is even; halved=4\""));

  History odd = mwf_test::loadGolden("conditional_odd");
  Decision dodd = decideOrDie(fx.eng, spec, odd.prefix(13));
  ASSERT_EQ(dodd.commands.size(), 1u);
  EXPECT_TRUE(jsonEq(dodd.commands[0].result_json, "\"7 is odd; tripled+1=22\""));
}

// ── The oracle: every prefix of every golden, self-consistent with history ───

namespace {

void runOracle(const char* golden_name, const char* spec_text) {
  SCOPED_TRACE(golden_name);
  Engines fx;
  WorkflowSpec spec = mwf_test::parseSpecOrDie(spec_text);
  History full = mwf_test::loadGolden(golden_name);

  for (size_t k = 1; k <= full.events.size(); ++k) {
    SCOPED_TRACE("prefix " + std::to_string(k));
    History p = full.prefix(k);

    auto r1 = fx.eng.decide(spec, p);
    auto r2 = fx.eng.decide(spec, p);
    ASSERT_TRUE(r1.ok) << r1.error;
    ASSERT_TRUE(r2.ok) << r2.error;
    // Determinism: identical decisions on identical inputs.
    ASSERT_EQ(r1.value.commands, r2.value.commands);
    ASSERT_EQ(ReplayEngine::serializeState(r1.value.state),
              ReplayEngine::serializeState(r2.value.state));

    const auto& cmds = r1.value.commands;
    ASSERT_LE(cmds.size(), 1u);  // v1: one outstanding thing at a time

    if (!cmds.empty() && cmds[0].kind == Command::Kind::ScheduleActivity) {
      // The engine's command must match the activity the REAL run scheduled
      // next — name, activityId, and argument payload.
      const HistoryEvent* next = nullptr;
      for (size_t i = k; i < full.events.size(); ++i) {
        if (full.events[i].type == EventType::ActivityTaskScheduled) {
          next = &full.events[i];
          break;
        }
      }
      ASSERT_NE(next, nullptr) << "engine scheduled but real history never did";
      EXPECT_EQ(cmds[0].activity_name, next->activity_name);
      EXPECT_EQ(std::to_string(cmds[0].seq), next->activity_id);
      ASSERT_FALSE(next->payloads.empty());
      EXPECT_TRUE(jsonEq(cmds[0].args_json, next->payloads[0]))
          << cmds[0].args_json << " vs " << next->payloads[0];
    } else if (!cmds.empty() && cmds[0].kind == Command::Kind::CompleteWorkflow) {
      const HistoryEvent* fin = nullptr;
      for (size_t i = k; i < full.events.size(); ++i) {
        if (full.events[i].type == EventType::WorkflowExecutionCompleted) {
          fin = &full.events[i];
          break;
        }
      }
      ASSERT_NE(fin, nullptr) << "engine completed but real history never did";
      ASSERT_FALSE(fin->payloads.empty());
      EXPECT_TRUE(jsonEq(cmds[0].result_json, fin->payloads[0]))
          << cmds[0].result_json << " vs " << fin->payloads[0];
    } else {
      // No command => the prefix must be waiting on an in-flight activity, or
      // already terminal. Verify against the prefix itself.
      bool terminal = false;
      std::vector<int64_t> open;  // scheduled-but-unfinished activity event ids
      for (const auto& e : p.events) {
        if (e.type == EventType::ActivityTaskScheduled) open.push_back(e.event_id);
        if (e.type == EventType::ActivityTaskCompleted ||
            e.type == EventType::ActivityTaskFailed) {
          for (auto it = open.begin(); it != open.end(); ++it) {
            if (*it == e.scheduled_event_id) { open.erase(it); break; }
          }
        }
        if (e.type == EventType::WorkflowExecutionCompleted ||
            e.type == EventType::WorkflowExecutionFailed) {
          terminal = true;
        }
      }
      EXPECT_TRUE(terminal || !open.empty())
          << "engine idle with no in-flight activity and no terminal event";
      EXPECT_EQ(r1.value.workflow_finished, terminal);
    }
  }
}

}  // namespace

TEST(ReplayOracle, AllPrefixesLinear) {
  runOracle("linear", mwf_test::kLinearSpec);
}
TEST(ReplayOracle, AllPrefixesConditionalEven) {
  runOracle("conditional_even", mwf_test::kConditionalSpec);
}
TEST(ReplayOracle, AllPrefixesConditionalOdd) {
  runOracle("conditional_odd", mwf_test::kConditionalSpec);
}

// ── Durability: reboot mid-run, restore, continue ────────────────────────────

TEST(Durability, StateSerializationRoundTrips) {
  Engines fx;
  History h = mwf_test::loadGolden("linear");
  WorkflowSpec spec = mwf_test::parseSpecOrDie(mwf_test::kLinearSpec);

  Decision d = decideOrDie(fx.eng, spec, h.prefix(12));  // shout in flight
  mwf::Bytes bytes = ReplayEngine::serializeState(d.state);
  auto restored = ReplayEngine::restoreState(bytes);
  ASSERT_TRUE(restored.ok) << restored.error;
  const EngineState& s = restored.value;
  EXPECT_EQ(s.run_id, d.state.run_id);
  EXPECT_EQ(s.spec_hash, d.state.spec_hash);
  EXPECT_EQ(s.last_processed_event_id, 12);
  EXPECT_EQ(s.next_seq, d.state.next_seq);
  EXPECT_EQ(s.results_json, d.state.results_json);
  EXPECT_TRUE(s.has_pending);
  EXPECT_EQ(s.pending_scheduled_event_id, 11);
  EXPECT_EQ(s.pending_activity_name, "shout");
  EXPECT_EQ(s.pending_step_id, "shout");
}

// For every golden and every snapshot point m: replay-from-scratch on prefix k
// must equal (state at prefix m) + continuation — fed EITHER the full history
// prefix or only the event suffix (m, k]. The reboot-resume proof, exhaustively.
namespace {

void runDurabilityMatrix(const char* golden_name, const char* spec_text) {
  SCOPED_TRACE(golden_name);
  Engines fx;
  WorkflowSpec spec = mwf_test::parseSpecOrDie(spec_text);
  History full = mwf_test::loadGolden(golden_name);
  const size_t n = full.events.size();

  for (size_t m = 1; m < n; ++m) {       // snapshot point
    Decision snap = decideOrDie(fx.eng, spec, full.prefix(m));
    // Reboot: state through bytes.
    auto restored = ReplayEngine::restoreState(ReplayEngine::serializeState(snap.state));
    ASSERT_TRUE(restored.ok) << restored.error;

    for (size_t k = m; k <= n; ++k) {    // continuation horizon
      Decision scratch = decideOrDie(fx.eng, spec, full.prefix(k));

      auto viaFull = fx.eng.decide(spec, full.prefix(k), restored.value);
      ASSERT_TRUE(viaFull.ok) << "m=" << m << " k=" << k << ": " << viaFull.error;
      EXPECT_EQ(viaFull.value.commands, scratch.commands) << "m=" << m << " k=" << k;

      History suffix = suffixAfter(full.prefix(k), snap.state.last_processed_event_id);
      auto viaSuffix = fx.eng.decide(spec, suffix, restored.value);
      ASSERT_TRUE(viaSuffix.ok) << "m=" << m << " k=" << k << ": " << viaSuffix.error;
      EXPECT_EQ(viaSuffix.value.commands, scratch.commands) << "m=" << m << " k=" << k;
      EXPECT_EQ(viaSuffix.value.workflow_finished, scratch.workflow_finished);
    }
  }
}

}  // namespace

TEST(Durability, ScratchEqualsRestoredContinuationLinear) {
  runDurabilityMatrix("linear", mwf_test::kLinearSpec);
}
TEST(Durability, ScratchEqualsRestoredContinuationConditionalEven) {
  runDurabilityMatrix("conditional_even", mwf_test::kConditionalSpec);
}
TEST(Durability, ScratchEqualsRestoredContinuationConditionalOdd) {
  runDurabilityMatrix("conditional_odd", mwf_test::kConditionalSpec);
}

TEST(Durability, SaveAndLoadThroughIDurableStore) {
  Engines fx;
  mwf_test::MemStore store;
  History h = mwf_test::loadGolden("linear");
  WorkflowSpec spec = mwf_test::parseSpecOrDie(mwf_test::kLinearSpec);

  Decision d = decideOrDie(fx.eng, spec, h.prefix(7));
  ASSERT_TRUE(ReplayEngine::saveState(store, d.state));
  ASSERT_EQ(store.keys("exec/").size(), 1u);
  EXPECT_EQ(store.keys("exec/")[0], "exec/" + d.state.run_id);

  auto loaded = ReplayEngine::loadState(store, d.state.run_id);
  ASSERT_TRUE(loaded.ok) << loaded.error;
  EXPECT_EQ(ReplayEngine::serializeState(loaded.value),
            ReplayEngine::serializeState(d.state));

  EXPECT_FALSE(ReplayEngine::loadState(store, "no-such-run").ok);
}

TEST(Durability, ResumeRejectsMismatchedSpec) {
  Engines fx;
  History h = mwf_test::loadGolden("linear");
  WorkflowSpec spec = mwf_test::parseSpecOrDie(mwf_test::kLinearSpec);
  WorkflowSpec other = mwf_test::parseSpecOrDie(mwf_test::kConditionalSpec);

  Decision d = decideOrDie(fx.eng, spec, h.prefix(7));
  auto r = fx.eng.decide(other, h, d.state);
  ASSERT_FALSE(r.ok);
  EXPECT_NE(r.error.find("hash mismatch"), std::string::npos) << r.error;
}

// ── Error semantics ──────────────────────────────────────────────────────────

TEST(ReplayErrors, HistorySpecMismatchIsNondeterminism) {
  Engines fx;
  // The linear history scheduled 'greet' first; the conditional spec expects
  // 'parity' — the engine must refuse, like a Temporal SDK replayer would.
  History h = mwf_test::loadGolden("linear");
  WorkflowSpec spec = mwf_test::parseSpecOrDie(mwf_test::kConditionalSpec);
  auto r = fx.eng.decide(spec, h.prefix(7));
  ASSERT_FALSE(r.ok);
  EXPECT_NE(r.error.find("nondeterminism"), std::string::npos) << r.error;
}

TEST(ReplayErrors, UnresolvedPredicatePathFailsDeterministically) {
  Engines fx;
  const char* spec_text = R"({
    "name": "ConditionalWorkflow",
    "steps": [
      {"type": "activity", "name": "parity", "args_from": "/input"},
      {"type": "conditional",
       "predicate": {"path": "/results/nope", "op": "eq", "value": "even"},
       "true_steps":  [{"type": "complete", "result": 1}],
       "false_steps": [{"type": "complete", "result": 0}]}
    ]})";
  WorkflowSpec spec = mwf_test::parseSpecOrDie(spec_text);
  History h = mwf_test::loadGolden("conditional_even");
  auto r1 = fx.eng.decide(spec, h.prefix(7));
  auto r2 = fx.eng.decide(spec, h.prefix(7));
  ASSERT_FALSE(r1.ok);
  EXPECT_EQ(r1.error, r2.error);  // same failure every time — deterministic
  EXPECT_NE(r1.error.find("does not resolve"), std::string::npos) << r1.error;
}

TEST(ReplayErrors, FailedActivityFailsTheWorkflow) {
  Engines fx;
  // Synthetic: greet scheduled then failed ("world" input = IndvcmxkIg==).
  auto h = mwf_core::loadHistoryJson(std::string_view(R"({"events":[
    {"eventId":"1","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_STARTED",
     "workflowExecutionStartedEventAttributes":{
       "workflowType":{"name":"LinearWorkflow"},"workflowId":"wf-x",
       "originalExecutionRunId":"run-x",
       "input":{"payloads":[{"metadata":{},"data":"IndvcmxkIg=="}]}}},
    {"eventId":"2","eventType":"EVENT_TYPE_ACTIVITY_TASK_SCHEDULED",
     "activityTaskScheduledEventAttributes":{"activityId":"1",
       "activityType":{"name":"greet"},
       "input":{"payloads":[{"metadata":{},"data":"IndvcmxkIg=="}]}}},
    {"eventId":"3","eventType":"EVENT_TYPE_ACTIVITY_TASK_FAILED",
     "activityTaskFailedEventAttributes":{"scheduledEventId":"2",
       "startedEventId":"2","failure":{"message":"boom"}}}
  ]})"));
  ASSERT_TRUE(h.ok) << h.error;
  WorkflowSpec spec = mwf_test::parseSpecOrDie(mwf_test::kLinearSpec);
  Decision d = decideOrDie(fx.eng, spec, h.value);
  ASSERT_EQ(d.commands.size(), 1u);
  EXPECT_EQ(d.commands[0].kind, Command::Kind::FailWorkflow);
  EXPECT_NE(d.commands[0].failure.find("greet"), std::string::npos);
  EXPECT_NE(d.commands[0].failure.find("boom"), std::string::npos);
}

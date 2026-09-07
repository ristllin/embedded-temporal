// core: WorkflowLoop unit tests (the workflow-side loop).
//
// Drives the WHOLE workflow loop — poll → parse task → look up spec → codec-
// decode inbound results → ReplayEngine::decide → codec-encode outbound → persist
// → RespondWorkflowTaskCompleted — against the REAL conformance golden histories,
// with a fake transport + fake proto adapter and the REAL PayloadCodecV1. Proves:
//   * the terminal command set per golden matches the engine's oracle,
//   * the self-replay oracle holds THROUGH the loop for every prefix of every
//     golden (successive prefixes fed as successive workflow tasks),
//   * outbound payloads are wf_v1-encoded with the run's WorkflowContext,
//   * a power-cycle mid-run resumes from the durable store and reaches the same
//     terminal command (and that resume is load-bearing: a fresh store can't).
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "mwf_codec/payload_codec.h"
#include "mwf_core/detail/json_util.h"
#include "mwf_core/workflow_loop.h"
#include "test_util.h"

using mwf::Bytes;
using mwf_core::Command;
using mwf_core::EventType;
using mwf_core::History;
using mwf_core::HistoryEvent;
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

// Events strictly after `event_id` — the delta a rebooted worker would see.
History suffixAfter(const History& h, int64_t event_id) {
  History out;
  for (const auto& e : h.events)
    if (e.event_id > event_id) out.events.push_back(e);
  return out;
}

// ── fake transport: one scripted poll result + one respond result, records all ─
struct RecordedCall {
  std::string method;
  int deadline_ms = 0;
};
struct FakeTransport : mwf::ITransport {
  mwf::GrpcResult poll_result;     // default OK, empty body
  mwf::GrpcResult respond_result;  // default OK
  std::vector<RecordedCall> calls;

  mwf::GrpcResult call(std::string_view fullMethod, const Bytes&,
                       const mwf::Metadata&, int deadlineMs) override {
    calls.push_back({std::string(fullMethod), deadlineMs});
    if (fullMethod == mwf_core::kMethodPollWorkflowTaskQueue) return poll_result;
    if (fullMethod == mwf_core::kMethodRespondWorkflowTaskCompleted) return respond_result;
    return {};
  }
  void close() override {}
};

// ── fake proto adapter ────────────────────────────────────────────────────────
// parseWorkflowTask reads a trivial descriptor the test placed in the poll
// response ("<golden>|<prefixLen>[|<afterEventId>]") and reuses the history.h
// golden loader to build the WorkflowTaskInfo — a real parse, no protobuf. The
// execution ids come from the full golden's WorkflowExecutionStarted event (in
// real Temporal they ride the poll RESPONSE envelope, so they are present even
// when the delivered history is a suffix). buildRespondWorkflowCompleted captures
// the outbound ProtoCommands for assertions.
struct FakeWorkflowAdapter : mwf_core::IWorkflowProtoAdapter {
  bool parse_fail = false;
  int poll_builds = 0;
  bool respond_called = false;
  Bytes respond_token;
  std::vector<ProtoCommand> last_commands;

  Bytes buildPollWorkflowRequest(const WorkflowConfig&) override {
    ++poll_builds;
    return bytes("POLL-WF-REQ");
  }

  mwf::Result<WorkflowTaskInfo> parseWorkflowTask(const Bytes& resp) override {
    using R = mwf::Result<WorkflowTaskInfo>;
    if (parse_fail) return R::failure("scripted parse failure");
    std::string desc = str(resp);
    if (desc.empty()) {  // taskless poll
      WorkflowTaskInfo t;
      t.empty = true;
      return R::success(std::move(t));
    }
    // split "<golden>|<k>[|<after>]"
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= desc.size(); ++i) {
      if (i == desc.size() || desc[i] == '|') {
        parts.push_back(desc.substr(start, i - start));
        start = i + 1;
      }
    }
    const std::string& name = parts[0];
    size_t k = parts.size() > 1 ? std::stoul(parts[1]) : 0;

    History full = mwf_test::loadGolden(name);
    History delivered = full.prefix(k);
    if (parts.size() > 2) delivered = suffixAfter(delivered, std::stoll(parts[2]));

    WorkflowTaskInfo t;
    t.empty = false;
    t.task_token = bytes("tok-" + name + "-" + std::to_string(k));
    if (!full.events.empty()) {
      t.workflow_type = full.events[0].workflow_type;
      t.workflow_id = full.events[0].workflow_id;
      t.run_id = full.events[0].run_id;
    }
    t.ns = "";  // exercise the loop's fallback to config.ns
    t.history = std::move(delivered);
    return R::success(std::move(t));
  }

  Bytes buildRespondWorkflowCompleted(
      const WorkflowConfig&, const Bytes& token,
      const std::vector<ProtoCommand>& commands) override {
    respond_called = true;
    respond_token = token;
    last_commands = commands;
    return bytes("RESPOND-WF-REQ");
  }
};

// ── fixture ───────────────────────────────────────────────────────────────────
struct WorkflowLoopTest : ::testing::Test {
  FakeTransport transport;
  mwf_codec::PayloadCodecV1 codec;  // the REAL payload codec
  FakeWorkflowAdapter adapter;
  mwf_test::MemStore store;
  WorkflowSpec linear = mwf_test::parseSpecOrDie(mwf_test::kLinearSpec);
  WorkflowSpec conditional = mwf_test::parseSpecOrDie(mwf_test::kConditionalSpec);
  WorkflowConfig config{"default", "wf-tq", "wf@host", 70000, 10000, "", {}};

  std::map<std::string, const WorkflowSpec*> specs{
      {"LinearWorkflow", &linear}, {"ConditionalWorkflow", &conditional}};

  mwf_core::SpecProvider provider() {
    return [this](std::string_view t) -> const WorkflowSpec* {
      auto it = specs.find(std::string(t));
      return it == specs.end() ? nullptr : it->second;
    };
  }
  WorkflowLoop makeLoop() {
    return WorkflowLoop(transport, codec, adapter, store, provider(), config);
  }
  // Point the fake at a golden prefix (optionally a suffix after event `after`).
  void deliver(const std::string& golden, size_t k, int64_t after = -1) {
    std::string desc = golden + "|" + std::to_string(k);
    if (after >= 0) desc += "|" + std::to_string(after);
    transport.poll_result = {0, "", bytes(desc)};
  }
};

// The single ProtoCommand the last respond carried (asserts exactly one).
const ProtoCommand& onlyCommand(const FakeWorkflowAdapter& a) {
  EXPECT_EQ(a.last_commands.size(), 1u);
  return a.last_commands.front();
}

}  // namespace

// ── idle / error paths ────────────────────────────────────────────────────────

TEST_F(WorkflowLoopTest, PollDeadlineExceededIsIdle) {
  transport.poll_result = {4, "Deadline Exceeded", {}};
  auto r = makeLoop().runOnce(2000);
  EXPECT_EQ(r.outcome, WorkflowTickOutcome::Idle);
  ASSERT_EQ(transport.calls.size(), 1u);
  EXPECT_EQ(transport.calls[0].method, mwf_core::kMethodPollWorkflowTaskQueue);
  EXPECT_EQ(transport.calls[0].deadline_ms, 2000);
  EXPECT_FALSE(adapter.respond_called);
}

TEST_F(WorkflowLoopTest, TasklessPollIsIdle) {
  transport.poll_result = {0, "", {}};  // OK, empty body → empty task
  auto r = makeLoop().runOnce(1000);
  EXPECT_EQ(r.outcome, WorkflowTickOutcome::Idle);
  EXPECT_FALSE(adapter.respond_called);
}

TEST_F(WorkflowLoopTest, PollTransportErrorStopsTick) {
  transport.poll_result = {14, "connect refused", {}};
  auto loop = makeLoop();
  auto r = loop.runOnce(1000);
  EXPECT_EQ(r.outcome, WorkflowTickOutcome::TransportError);
  EXPECT_FALSE(loop.tick());
}

TEST_F(WorkflowLoopTest, ParseFailureIsProtocolError) {
  adapter.parse_fail = true;
  transport.poll_result = {0, "", bytes("junk")};
  auto r = makeLoop().runOnce(1000);
  EXPECT_EQ(r.outcome, WorkflowTickOutcome::ProtocolError);
  EXPECT_FALSE(adapter.respond_called);
}

TEST_F(WorkflowLoopTest, UnknownWorkflowTypeFailsExecution) {
  specs.clear();  // provider now returns nullptr for everything
  deliver("linear", 1);
  auto r = makeLoop().runOnce(1000);
  EXPECT_EQ(r.outcome, WorkflowTickOutcome::Failed);
  ASSERT_TRUE(adapter.respond_called);
  const ProtoCommand& c = onlyCommand(adapter);
  EXPECT_EQ(c.kind, ProtoCommand::Kind::FailWorkflow);
  EXPECT_NE(c.failure.find("no workflow spec"), std::string::npos);
}

// ── per-golden terminal / mid-history commands ────────────────────────────────

TEST_F(WorkflowLoopTest, LinearMidHistorySchedulesFirstActivity) {
  deliver("linear", 1);  // just WorkflowExecutionStarted
  auto r = makeLoop().runOnce(1000);
  EXPECT_EQ(r.outcome, WorkflowTickOutcome::Progressed);
  const ProtoCommand& c = onlyCommand(adapter);
  EXPECT_EQ(c.kind, ProtoCommand::Kind::ScheduleActivity);
  EXPECT_EQ(c.activity_name, "greet");
  EXPECT_EQ(c.seq, 1u);
  EXPECT_EQ(c.activity_task_queue, "wf-tq");  // defaults to the workflow's queue
  EXPECT_TRUE(jsonEq(str(c.input.data), "\"world\""));
}

TEST_F(WorkflowLoopTest, LinearFinalTaskCompletesWorkflow) {
  deliver("linear", 13);  // both activities completed
  auto r = makeLoop().runOnce(1000);
  EXPECT_EQ(r.outcome, WorkflowTickOutcome::Completed);
  const ProtoCommand& c = onlyCommand(adapter);
  EXPECT_EQ(c.kind, ProtoCommand::Kind::CompleteWorkflow);
  EXPECT_TRUE(jsonEq(str(c.result.data), "\"HELLO, WORLD!\""));
}

TEST_F(WorkflowLoopTest, ConditionalBranchesMatchTheRealWorker) {
  struct Case { const char* golden; const char* branch; const char* result; };
  for (const Case& tc : {Case{"conditional_even", "even_branch", "\"8 is even; halved=4\""},
                         Case{"conditional_odd", "odd_branch", "\"7 is odd; tripled+1=22\""}}) {
    adapter.last_commands.clear();
    adapter.respond_called = false;
    store.kv.clear();
    // After the parity result: engine must pick the branch.
    deliver(tc.golden, 7);
    auto r1 = makeLoop().runOnce(1000);
    EXPECT_EQ(r1.outcome, WorkflowTickOutcome::Progressed) << tc.golden;
    const ProtoCommand& sched = onlyCommand(adapter);
    EXPECT_EQ(sched.kind, ProtoCommand::Kind::ScheduleActivity);
    EXPECT_EQ(sched.activity_name, tc.branch) << tc.golden;

    // The trailing complete reads whichever branch ran.
    deliver(tc.golden, 13);
    auto r2 = makeLoop().runOnce(1000);
    EXPECT_EQ(r2.outcome, WorkflowTickOutcome::Completed) << tc.golden;
    const ProtoCommand& done = onlyCommand(adapter);
    EXPECT_EQ(done.kind, ProtoCommand::Kind::CompleteWorkflow);
    EXPECT_TRUE(jsonEq(str(done.result.data), tc.result)) << tc.golden;
  }
}

// ── codec path: outbound payloads are wf_v1-wrapped with the run's context ─────

TEST_F(WorkflowLoopTest, OutboundScheduleInputIsWfV1Encoded) {
  deliver("linear", 1);
  makeLoop().runOnce(1000);
  const ProtoCommand& c = onlyCommand(adapter);
  EXPECT_EQ(c.input.metadata.at("encoding"), "json/wf_v1");
  EXPECT_EQ(c.input.metadata.at("execution_id"), "linear-85597381");  // the workflow_id
  EXPECT_EQ(c.input.metadata.at("namespace"), "default");             // config.ns fallback
  EXPECT_EQ(c.input.metadata.at("encoding_options"), "");
}

TEST_F(WorkflowLoopTest, OutboundCompleteResultIsWfV1Encoded) {
  deliver("linear", 13);
  makeLoop().runOnce(1000);
  const ProtoCommand& c = onlyCommand(adapter);
  EXPECT_EQ(c.result.metadata.at("encoding"), "json/wf_v1");
  EXPECT_EQ(c.result.metadata.at("execution_id"), "linear-85597381");
}

TEST_F(WorkflowLoopTest, ConfigurableActivityQueueOverride) {
  config.activity_task_queue = "activities-only";
  deliver("linear", 1);
  makeLoop().runOnce(1000);
  EXPECT_EQ(onlyCommand(adapter).activity_task_queue, "activities-only");
}

// ── the self-replay oracle, THROUGH the full loop + codec ─────────────────────
// Feed successive PREFIXES of a golden as successive workflow tasks on one loop
// (with the durable store carrying state between ticks, exactly as Temporal
// delivers growing histories). Each respond must match what the REAL history did
// next — the replay-vs-oracle check, now proven end-to-end through
// poll/decode/encode.
namespace {

void runLoopOracle(WorkflowLoopTest& fx, const char* golden) {
  SCOPED_TRACE(golden);
  History full = mwf_test::loadGolden(golden);
  const size_t n = full.events.size();

  for (size_t k = 1; k <= n; ++k) {
    SCOPED_TRACE("prefix " + std::to_string(k));
    fx.adapter.last_commands.clear();
    fx.adapter.respond_called = false;
    fx.deliver(golden, k);
    auto loop = fx.makeLoop();  // fresh loop object; SAME durable store (resume)
    auto r = loop.runOnce(1000);
    ASSERT_NE(r.outcome, WorkflowTickOutcome::TransportError);
    ASSERT_NE(r.outcome, WorkflowTickOutcome::ProtocolError) << r.detail;
    ASSERT_TRUE(fx.adapter.respond_called);

    const auto& cmds = fx.adapter.last_commands;
    ASSERT_LE(cmds.size(), 1u);

    if (!cmds.empty() && cmds[0].kind == ProtoCommand::Kind::ScheduleActivity) {
      const HistoryEvent* next = nullptr;
      for (size_t i = k; i < n; ++i)
        if (full.events[i].type == EventType::ActivityTaskScheduled) { next = &full.events[i]; break; }
      ASSERT_NE(next, nullptr) << "loop scheduled but real history never did";
      EXPECT_EQ(cmds[0].activity_name, next->activity_name);
      EXPECT_EQ(std::to_string(cmds[0].seq), next->activity_id);
      ASSERT_FALSE(next->payloads.empty());
      EXPECT_TRUE(jsonEq(str(cmds[0].input.data), next->payloads[0]))  // codec inner == history
          << str(cmds[0].input.data) << " vs " << next->payloads[0];
      EXPECT_EQ(cmds[0].input.metadata.at("encoding"), "json/wf_v1");
    } else if (!cmds.empty() && cmds[0].kind == ProtoCommand::Kind::CompleteWorkflow) {
      const HistoryEvent* fin = nullptr;
      for (size_t i = k; i < n; ++i)
        if (full.events[i].type == EventType::WorkflowExecutionCompleted) { fin = &full.events[i]; break; }
      ASSERT_NE(fin, nullptr) << "loop completed but real history never did";
      ASSERT_FALSE(fin->payloads.empty());
      EXPECT_TRUE(jsonEq(str(cmds[0].result.data), fin->payloads[0]))
          << str(cmds[0].result.data) << " vs " << fin->payloads[0];
      EXPECT_EQ(cmds[0].result.metadata.at("encoding"), "json/wf_v1");
    } else {
      EXPECT_TRUE(cmds.empty());  // waiting on an in-flight activity, or terminal
    }
  }
}

}  // namespace

TEST_F(WorkflowLoopTest, LoopOracleLinear) { runLoopOracle(*this, "linear"); }
TEST_F(WorkflowLoopTest, LoopOracleConditionalEven) { runLoopOracle(*this, "conditional_even"); }
TEST_F(WorkflowLoopTest, LoopOracleConditionalOdd) { runLoopOracle(*this, "conditional_odd"); }

// ── power-cycle mid-workflow: resume from the durable store ───────────────────
// Run to a mid-run task (state persisted), then move ONLY the durable bytes to a
// fresh store + fresh loop and continue with a history SUFFIX. Reaching the same
// terminal command proves resume works; a control run with an empty store CANNOT
// decide the suffix — so the resume is genuinely load-bearing.
TEST_F(WorkflowLoopTest, PowerCycleResumeReachesSameTerminal) {
  // 1. Loop A: deliver conditional_even prefix 7 → schedules even_branch.
  deliver("conditional_even", 7);
  auto rA = makeLoop().runOnce(1000);
  ASSERT_EQ(rA.outcome, WorkflowTickOutcome::Progressed);
  EXPECT_EQ(onlyCommand(adapter).activity_name, "even_branch");
  const std::string runId = rA.run_id;
  ASSERT_FALSE(runId.empty());
  const std::string key = "exec/" + runId;
  ASSERT_TRUE(store.get(key).has_value());  // state persisted BEFORE the respond

  // 2. Power cycle: copy ONLY the durable bytes into a genuinely fresh store.
  mwf_test::MemStore store2;
  store2.put(key, *store.get(key));

  // 3. Loop B over store2 + a fresh adapter, fed only the SUFFIX (events 8..13).
  FakeTransport transport2;
  FakeWorkflowAdapter adapter2;
  {
    std::string desc = "conditional_even|13|7";  // suffixAfter(prefix13, 7)
    transport2.poll_result = {0, "", bytes(desc)};
  }
  WorkflowLoop loopB(transport2, codec, adapter2, store2, provider(), config);
  auto rB = loopB.runOnce(1000);
  ASSERT_EQ(rB.outcome, WorkflowTickOutcome::Completed) << rB.detail;
  ASSERT_EQ(adapter2.last_commands.size(), 1u);
  EXPECT_EQ(adapter2.last_commands[0].kind, ProtoCommand::Kind::CompleteWorkflow);
  EXPECT_TRUE(jsonEq(str(adapter2.last_commands[0].result.data), "\"8 is even; halved=4\""));

  // 4. Control: the SAME suffix against an EMPTY store cannot resume — the engine
  // has no workflow start event and no prior state, so it fails deterministically.
  mwf_test::MemStore empty;
  FakeTransport transport3;
  FakeWorkflowAdapter adapter3;
  transport3.poll_result = {0, "", bytes("conditional_even|13|7")};
  WorkflowLoop loopC(transport3, codec, adapter3, empty, provider(), config);
  auto rC = loopC.runOnce(1000);
  EXPECT_NE(rC.outcome, WorkflowTickOutcome::Completed);
  EXPECT_EQ(rC.outcome, WorkflowTickOutcome::ProtocolError);
}

// ── terminal state cleanup: a finished run must not leak exec/<runId> forever ──
// The loop persists state on EVERY tick (at-most-once), so a completed run would
// otherwise leave its durable record behind permanently. A mid-run tick must
// KEEP state (resume depends on it); the terminal tick must ERASE it.
TEST_F(WorkflowLoopTest, CompletedRunErasesDurableState) {
  // Mid-run: schedules greet, state persisted for resume.
  deliver("linear", 1);
  auto rA = makeLoop().runOnce(1000);
  ASSERT_EQ(rA.outcome, WorkflowTickOutcome::Progressed);
  const std::string key = "exec/" + rA.run_id;
  ASSERT_FALSE(rA.run_id.empty());
  EXPECT_TRUE(store.get(key).has_value()) << "mid-run state must survive for resume";

  // Terminal: emits CompleteWorkflow — the exec/<runId> record must be gone.
  deliver("linear", 13);
  auto rB = makeLoop().runOnce(1000);
  ASSERT_EQ(rB.outcome, WorkflowTickOutcome::Completed);
  EXPECT_FALSE(store.get(key).has_value())
      << "a completed run must erase its durable state, not leak it forever";
  EXPECT_TRUE(store.keys("exec/").empty());
}

// The already-terminal branch (history carries WorkflowExecutionCompleted, the
// engine emits no command) must also erase the leftover state.
TEST_F(WorkflowLoopTest, AlreadyTerminalHistoryErasesDurableState) {
  deliver("linear", 1);
  auto rA = makeLoop().runOnce(1000);
  ASSERT_EQ(rA.outcome, WorkflowTickOutcome::Progressed);
  const std::string key = "exec/" + rA.run_id;
  ASSERT_TRUE(store.get(key).has_value());

  deliver("linear", 17);  // full history, incl. WorkflowExecutionCompleted (event 17)
  auto rB = makeLoop().runOnce(1000);
  ASSERT_EQ(rB.outcome, WorkflowTickOutcome::Progressed);
  EXPECT_EQ(rB.detail, "already terminal (no commands)");
  EXPECT_FALSE(store.get(key).has_value())
      << "an already-terminal redelivery must clear the leftover state";
}

// core: WorkerLoop unit tests.
// Fakes for the three injected seams + the proto adapter; asserts the full
// poll → decode → invoke → respond choreography without any grpc/protobuf.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mwf_core/activity_registry.h"
#include "mwf_core/worker_loop.h"

using mwf::Bytes;
using mwf_core::PolledActivityTask;
using mwf_core::TickOutcome;
using mwf_core::WorkerConfig;
using mwf_core::WorkerLoop;

namespace {

Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
std::string str(const Bytes& b) { return std::string(b.begin(), b.end()); }

// ── fake transport: scripted result per method, records every call ───────────
struct RecordedCall {
  std::string method;
  Bytes request;
  int deadline_ms = 0;
};

struct FakeTransport : mwf::ITransport {
  std::map<std::string, mwf::GrpcResult> script;  // method → result
  std::vector<RecordedCall> calls;

  mwf::GrpcResult call(std::string_view fullMethod, const Bytes& requestMsg,
                       const mwf::Metadata&, int deadlineMs) override {
    calls.push_back({std::string(fullMethod), requestMsg, deadlineMs});
    auto it = script.find(std::string(fullMethod));
    if (it != script.end()) return it->second;
    mwf::GrpcResult ok;  // default: OK, empty body
    return ok;
  }
  void close() override {}
};

// ── fake payload codec: plain/wf_v1 behavior mirroring PayloadCodecV1 ─────────
struct FakeCodec : mwf::IPayloadCodec {
  int decode_calls = 0;
  int encode_calls = 0;
  mwf::WorkflowContext last_echoed_ctx;
  bool last_empty = false;

  mwf::Result<mwf::DecodedActivityInput> decodeActivityInput(
      const mwf::temporal::Payload& p) override {
    ++decode_calls;
    using R = mwf::Result<mwf::DecodedActivityInput>;
    auto enc = p.metadata.find("encoding");
    if (enc == p.metadata.end()) return R::failure("missing encoding");
    mwf::DecodedActivityInput out;
    out.argument_json = p.data;
    if (enc->second == "json/wf_v1") {
      auto eid = p.metadata.find("execution_id");
      out.context.execution_id = eid == p.metadata.end() ? "" : eid->second;
      auto ns = p.metadata.find("namespace");
      out.context.ns = ns == p.metadata.end() ? "" : ns->second;
      if (out.context.execution_id.empty()) return R::failure("no execution_id");
    }
    return R::success(std::move(out));
  }

  mwf::temporal::Payload encodeActivityResult(const Bytes& result_json,
                                              const mwf::WorkflowContext& ctx,
                                              bool empty) override {
    ++encode_calls;
    last_echoed_ctx = ctx;
    last_empty = empty;
    mwf::temporal::Payload p;
    p.metadata["encoding"] = "json/wf_v1";
    p.metadata["execution_id"] = ctx.execution_id;
    p.data = result_json;
    return p;
  }
};

// ── fake proto adapter: marker bytes out, scripted task in ────────────────────
struct FakeAdapter : mwf_core::IWorkerProtoAdapter {
  PolledActivityTask next_task;      // returned by parsePollResponse
  bool parse_ok = true;
  // Records:
  int poll_builds = 0;
  Bytes completed_token;
  mwf::temporal::Payload completed_payload;
  bool completed_called = false;
  Bytes failed_token;
  std::string failed_message;
  bool failed_called = false;

  Bytes buildPollRequest(const WorkerConfig&) override {
    ++poll_builds;
    return bytes("POLL-REQ");
  }
  bool parsePollResponse(const Bytes&, PolledActivityTask& out) override {
    if (!parse_ok) return false;
    out = next_task;
    return true;
  }
  Bytes buildRespondCompleted(const WorkerConfig&, const Bytes& token,
                              const mwf::temporal::Payload& result) override {
    completed_called = true;
    completed_token = token;
    completed_payload = result;
    return bytes("COMPLETED-REQ");
  }
  Bytes buildRespondFailed(const WorkerConfig&, const Bytes& token,
                           const std::string& message) override {
    failed_called = true;
    failed_token = token;
    failed_message = message;
    return bytes("FAILED-REQ");
  }
};

struct WorkerLoopTest : ::testing::Test {
  FakeTransport transport;
  FakeCodec codec;
  FakeAdapter adapter;
  mwf_core::ActivityRegistry registry;
  WorkerConfig config{"default", "test-tq", "test@host", 70000, 10000, {}};

  WorkerLoop makeLoop() {
    return WorkerLoop(transport, codec, registry, adapter, config);
  }

  PolledActivityTask greetTask(const std::string& encoding,
                               const std::string& arg_json) {
    PolledActivityTask t;
    t.has_task = true;
    t.task_token = bytes("tok-1");
    t.activity_name = "greet";
    mwf::temporal::Payload p;
    p.metadata["encoding"] = encoding;
    if (encoding == "json/wf_v1") {
      p.metadata["execution_id"] = "exec-42";
      p.metadata["namespace"] = "default";
    }
    p.data = bytes(arg_json);
    t.inputs.push_back(std::move(p));
    return t;
  }
};

// Empty long-poll (DEADLINE_EXCEEDED) → Idle, exactly one transport call.
TEST_F(WorkerLoopTest, PollEmptyDeadlineExceededIsIdle) {
  transport.script[mwf_core::kMethodPollActivityTaskQueue] = {4, "Deadline Exceeded", {}};
  auto loop = makeLoop();
  auto r = loop.runOnce(2000);
  EXPECT_EQ(r.outcome, TickOutcome::Idle);
  ASSERT_EQ(transport.calls.size(), 1u);
  EXPECT_EQ(transport.calls[0].method, mwf_core::kMethodPollActivityTaskQueue);
  EXPECT_EQ(str(transport.calls[0].request), "POLL-REQ");
  EXPECT_EQ(transport.calls[0].deadline_ms, 2000);
  EXPECT_FALSE(adapter.completed_called);
  EXPECT_FALSE(adapter.failed_called);
  EXPECT_TRUE(loop.tick());  // idle keeps the loop turning
}

// OK poll but taskless response → Idle (no dispatch).
TEST_F(WorkerLoopTest, TasklessPollResponseIsIdle) {
  adapter.next_task.has_task = false;
  auto r = makeLoop().runOnce(1000);
  EXPECT_EQ(r.outcome, TickOutcome::Idle);
  EXPECT_EQ(codec.decode_calls, 0);
}

// json/plain task → invoke → RespondCompleted with a json/plain result payload
// (NOT the wf_v1 envelope — the Python default converter must decode it).
TEST_F(WorkerLoopTest, PlainTaskSuccessRespondsPlain) {
  adapter.next_task = greetTask("json/plain", "\"world\"");
  registry.registerActivity("greet", [](const Bytes& arg) {
    EXPECT_EQ(std::string(arg.begin(), arg.end()), "\"world\"");
    return mwf::Result<Bytes>::success(bytes("\"hello, world\""));
  });

  auto r = makeLoop().runOnce(1000);
  EXPECT_EQ(r.outcome, TickOutcome::Completed);
  EXPECT_EQ(r.activity, "greet");

  ASSERT_EQ(transport.calls.size(), 2u);
  EXPECT_EQ(transport.calls[1].method, mwf_core::kMethodRespondActivityTaskCompleted);
  EXPECT_EQ(str(transport.calls[1].request), "COMPLETED-REQ");
  EXPECT_EQ(transport.calls[1].deadline_ms, config.respond_deadline_ms);

  ASSERT_TRUE(adapter.completed_called);
  EXPECT_EQ(str(adapter.completed_token), "tok-1");
  EXPECT_EQ(adapter.completed_payload.metadata.at("encoding"), "json/plain");
  EXPECT_EQ(str(adapter.completed_payload.data), "\"hello, world\"");
  EXPECT_EQ(codec.encode_calls, 0);  // plain-in ⇒ codec envelope NOT used
}

// json/wf_v1 task → result goes through codec.encodeActivityResult, echoing ctx.
TEST_F(WorkerLoopTest, WfV1TaskEchoesContextThroughCodec) {
  adapter.next_task = greetTask("json/wf_v1", "\"world\"");
  registry.registerActivity("greet", [](const Bytes&) {
    return mwf::Result<Bytes>::success(bytes("\"hi\""));
  });

  auto r = makeLoop().runOnce(1000);
  EXPECT_EQ(r.outcome, TickOutcome::Completed);
  EXPECT_EQ(codec.encode_calls, 1);
  EXPECT_EQ(codec.last_echoed_ctx.execution_id, "exec-42");
  EXPECT_FALSE(codec.last_empty);
  ASSERT_TRUE(adapter.completed_called);
  EXPECT_EQ(adapter.completed_payload.metadata.at("encoding"), "json/wf_v1");
  EXPECT_EQ(adapter.completed_payload.metadata.at("execution_id"), "exec-42");
}

// wf_v1 "null" result → empty=true through the codec (SDK None semantics).
TEST_F(WorkerLoopTest, WfV1NullResultMarksEmpty) {
  adapter.next_task = greetTask("json/wf_v1", "null");
  registry.registerActivity("greet", [](const Bytes&) {
    return mwf::Result<Bytes>::success(bytes("null"));
  });
  auto r = makeLoop().runOnce(1000);
  EXPECT_EQ(r.outcome, TickOutcome::Completed);
  EXPECT_TRUE(codec.last_empty);
  EXPECT_EQ(str(adapter.completed_payload.data), "null");
}

// Activity handler failure → RespondActivityTaskFailed with the message.
TEST_F(WorkerLoopTest, InvokeFailureRespondsFailed) {
  adapter.next_task = greetTask("json/plain", "\"world\"");
  registry.registerActivity("greet", [](const Bytes&) {
    return mwf::Result<Bytes>::failure("boom: no greeting today");
  });

  auto r = makeLoop().runOnce(1000);
  EXPECT_EQ(r.outcome, TickOutcome::Failed);
  EXPECT_EQ(r.detail, "boom: no greeting today");
  ASSERT_EQ(transport.calls.size(), 2u);
  EXPECT_EQ(transport.calls[1].method, mwf_core::kMethodRespondActivityTaskFailed);
  ASSERT_TRUE(adapter.failed_called);
  EXPECT_EQ(str(adapter.failed_token), "tok-1");
  EXPECT_EQ(adapter.failed_message, "boom: no greeting today");
  EXPECT_FALSE(adapter.completed_called);
}

// Unregistered activity → Failed (reported), not a crash/transport error.
TEST_F(WorkerLoopTest, UnregisteredActivityRespondsFailed) {
  adapter.next_task = greetTask("json/plain", "1");
  auto r = makeLoop().runOnce(1000);
  EXPECT_EQ(r.outcome, TickOutcome::Failed);
  ASSERT_TRUE(adapter.failed_called);
  EXPECT_NE(adapter.failed_message.find("no activity registered"), std::string::npos);
}

// Undecodable input payload → RespondFailed (the workflow sees the error).
TEST_F(WorkerLoopTest, DecodeFailureRespondsFailed) {
  PolledActivityTask t;
  t.has_task = true;
  t.task_token = bytes("tok-x");
  t.activity_name = "greet";
  t.inputs.push_back({});  // no encoding metadata → codec failure
  adapter.next_task = t;
  registry.registerActivity("greet", [](const Bytes&) {
    ADD_FAILURE() << "must not invoke on decode failure";
    return mwf::Result<Bytes>::success(Bytes{});
  });

  auto r = makeLoop().runOnce(1000);
  EXPECT_EQ(r.outcome, TickOutcome::Failed);
  EXPECT_NE(adapter.failed_message.find("decode failed"), std::string::npos);
}

// Zero-input task → dispatched with "null" argument, plain result.
TEST_F(WorkerLoopTest, NoInputsDispatchesNullArgument) {
  PolledActivityTask t;
  t.has_task = true;
  t.task_token = bytes("tok-0");
  t.activity_name = "ping";
  adapter.next_task = t;
  registry.registerActivity("ping", [](const Bytes& arg) {
    EXPECT_EQ(std::string(arg.begin(), arg.end()), "null");
    return mwf::Result<Bytes>::success(bytes("\"pong\""));
  });
  auto r = makeLoop().runOnce(1000);
  EXPECT_EQ(r.outcome, TickOutcome::Completed);
  EXPECT_EQ(adapter.completed_payload.metadata.at("encoding"), "json/plain");
}

// Poll transport error (UNAVAILABLE) → TransportError; tick() returns false.
TEST_F(WorkerLoopTest, PollTransportErrorStopsTick) {
  transport.script[mwf_core::kMethodPollActivityTaskQueue] = {14, "connect refused", {}};
  auto loop = makeLoop();
  auto r = loop.runOnce(1000);
  EXPECT_EQ(r.outcome, TickOutcome::TransportError);
  EXPECT_NE(r.detail.find("grpc_status=14"), std::string::npos);
  EXPECT_FALSE(loop.tick());
}

// Unparseable poll response bytes → ProtocolError (no respond attempted).
TEST_F(WorkerLoopTest, ParseFailureIsProtocolError) {
  adapter.parse_ok = false;
  auto r = makeLoop().runOnce(1000);
  EXPECT_EQ(r.outcome, TickOutcome::ProtocolError);
  EXPECT_FALSE(adapter.failed_called);
  EXPECT_FALSE(adapter.completed_called);
}

}  // namespace

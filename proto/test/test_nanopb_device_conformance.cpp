// proto: cross-lib conformance — the DEVICE nanopb adapter vs the REAL
// temporal.api messages via libprotobuf.
//
// The device subset (proto/mwf/device_activity.proto) is a hand-mirrored,
// wire-identical trim; this test is what makes "wire-identical" a checked
// property instead of a comment:
//   1. adapter buildPollRequest (nanopb)          → libprotobuf PollActivityTaskQueueRequest
//   2. libprotobuf PollActivityTaskQueueResponse  → adapter parsePollResponse (nanopb)
//   3. adapter buildRespondCompleted (nanopb)     → libprotobuf RespondActivityTaskCompletedRequest
//   4. adapter buildRespondFailed (nanopb)        → libprotobuf RespondActivityTaskFailedRequest
//   5. taskless response / over-cap payload edge cases
#include <cstdio>
#include <cstdlib>
#include <string>

#include "nanopb_worker_adapter.h"
#include "proto_codec.h"
#include "temporal/api/enums/v1/task_queue.pb.h"

static int g_failures = 0;
#define CHECK(cond, what)                                        \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL: %s (%s:%d)\n", what, __FILE__, __LINE__); \
      ++g_failures;                                              \
    }                                                            \
  } while (0)

using mwf::Bytes;

static mwf_core::WorkerConfig config() {
  mwf_core::WorkerConfig c;
  c.ns = "00000000-0000-4000-8000-000000000000:11111111-1111-4111-8111-111111111111";
  c.task_queue = "mwf-esp-e2e";
  c.identity = "mwf-esp-worker@nanopb-conformance";
  return c;
}

int main() {
  mwf_proto::NanopbWorkerAdapter adapter;
  const auto cfg = config();

  // ── 1. poll request: nanopb → libprotobuf ──────────────────────────────────
  {
    Bytes wire = adapter.buildPollRequest(cfg);
    CHECK(!wire.empty(), "buildPollRequest produced bytes");
    mwf::proto::PollActivityTaskQueueRequest req;
    CHECK(req.ParseFromArray(wire.data(), static_cast<int>(wire.size())),
          "libprotobuf parses nanopb poll request");
    CHECK(req.namespace_() == cfg.ns, "poll.namespace");
    CHECK(req.task_queue().name() == cfg.task_queue, "poll.task_queue.name");
    CHECK(req.task_queue().kind() ==
              ::temporal::api::enums::v1::TASK_QUEUE_KIND_NORMAL,
          "poll.task_queue.kind == NORMAL");
    CHECK(req.identity() == cfg.identity, "poll.identity");
  }

  // ── 2. poll response: libprotobuf → nanopb ─────────────────────────────────
  {
    mwf::proto::PollActivityTaskQueueResponse resp;
    const std::string token(121, '\x42');  // observed live size (dev server)
    resp.set_task_token(token);
    resp.mutable_activity_type()->set_name("device.echo");
    resp.mutable_workflow_type()->set_name("EchoWorkflow");
    resp.mutable_workflow_execution()->set_workflow_id("wf-esp-1");
    resp.mutable_workflow_execution()->set_run_id("run-abc-123");
    resp.set_attempt(1);
    auto* p = resp.mutable_input()->add_payloads();
    (*p->mutable_metadata())["encoding"] = "json/plain";
    p->set_data("\"hello esp\"");
    // Fields the device subset deliberately SKIPS (unknown-field path):
    resp.mutable_header();  // empty Header message, field 7
    resp.mutable_scheduled_time()->set_seconds(1789000000);
    resp.mutable_retry_policy()->set_maximum_attempts(3);

    std::string wire;
    CHECK(resp.SerializeToString(&wire), "serialize full poll response");
    mwf_core::PolledActivityTask task;
    CHECK(adapter.parsePollResponse(Bytes(wire.begin(), wire.end()), task),
          "nanopb parses libprotobuf poll response (with unknown fields)");
    CHECK(task.has_task, "task present");
    CHECK(task.task_token.size() == 121, "task_token size");
    CHECK(task.activity_name == "device.echo", "activity name");
    CHECK(task.workflow_id == "wf-esp-1", "workflow id");
    CHECK(task.run_id == "run-abc-123", "run id");
    CHECK(task.inputs.size() == 1, "one input payload");
    CHECK(task.inputs[0].metadata.at("encoding") == "json/plain",
          "payload metadata");
    CHECK(std::string(task.inputs[0].data.begin(), task.inputs[0].data.end()) ==
              "\"hello esp\"",
          "payload data");
  }

  // ── 3. respond completed: nanopb → libprotobuf ─────────────────────────────
  {
    Bytes token{1, 2, 3, 4, 5};
    mwf::temporal::Payload result;
    result.metadata["encoding"] = "json/plain";
    const std::string big(4096, 'x');  // real-sized JSON result under the 8 KB cap
    result.data.assign(big.begin(), big.end());
    Bytes wire = adapter.buildRespondCompleted(cfg, token, result);
    CHECK(!wire.empty(), "buildRespondCompleted produced bytes");
    mwf::proto::RespondActivityTaskCompletedReq req;
    CHECK(req.ParseFromArray(wire.data(), static_cast<int>(wire.size())),
          "libprotobuf parses nanopb respond-completed");
    CHECK(req.task_token() == std::string("\x01\x02\x03\x04\x05", 5),
          "completed.task_token");
    CHECK(req.namespace_() == cfg.ns, "completed.namespace");
    CHECK(req.identity() == cfg.identity, "completed.identity");
    CHECK(req.result().payloads_size() == 1, "completed.result one payload");
    CHECK(req.result().payloads(0).metadata().at("encoding") == "json/plain",
          "completed.result payload metadata");
    CHECK(req.result().payloads(0).data() == big, "completed.result 4 KB data");
  }

  // ── 4. respond failed: nanopb → libprotobuf ────────────────────────────────
  {
    Bytes token{9, 9, 9};
    Bytes wire = adapter.buildRespondFailed(cfg, token, "boom: division by zero");
    CHECK(!wire.empty(), "buildRespondFailed produced bytes");
    mwf::proto::RespondActivityTaskFailedReq req;
    CHECK(req.ParseFromArray(wire.data(), static_cast<int>(wire.size())),
          "libprotobuf parses nanopb respond-failed");
    CHECK(req.failure().message() == "boom: division by zero", "failure.message");
    CHECK(req.failure().source() == "mwf-esp-worker", "failure.source");
    CHECK(req.failure().has_application_failure_info(),
          "failure has application_failure_info (oneof member via field 5)");
    CHECK(req.failure().application_failure_info().type() == "MwfActivityFailure",
          "failure info type");
  }

  // ── 5. edges ───────────────────────────────────────────────────────────────
  {
    // Taskless response (empty message) → has_task=false, success.
    mwf_core::PolledActivityTask task;
    CHECK(adapter.parsePollResponse(Bytes{}, task), "empty response parses");
    CHECK(!task.has_task, "empty response is taskless");

    // Over-cap payload data (> 8 KB) → decode FAILS (ProtocolError upstream),
    // never a silent truncation.
    mwf::proto::PollActivityTaskQueueResponse resp;
    resp.set_task_token("t");
    auto* p = resp.mutable_input()->add_payloads();
    p->set_data(std::string(9000, 'y'));
    std::string wire;
    (void)resp.SerializeToString(&wire);
    CHECK(!adapter.parsePollResponse(Bytes(wire.begin(), wire.end()), task),
          "over-cap payload data rejected");

    // Over-cap outbound result → encode refuses (empty bytes), no truncation.
    mwf::temporal::Payload result;
    result.data.assign(9000, 'z');
    CHECK(adapter.buildRespondCompleted(cfg, Bytes{1}, result).empty(),
          "over-cap outbound result refused");
  }

  if (g_failures == 0) std::printf("nanopb device conformance: ALL OK\n");
  return g_failures ? 1 : 0;
}

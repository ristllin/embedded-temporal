// test_roundtrip.cpp — IProtoCodec host-twin conformance.
//
// Constructs representative §2 messages, runs them through mwf::ProtoCodec<Msg>
// (encode -> bytes -> decode), and asserts structural equality both ways. This
// pins the libprotobuf host twin; the same messages will later be checked
// byte-for-byte against nanopb + the conformance goldens so the two codecs
// cannot drift.
#include <cstdio>
#include <cstdlib>
#include <string>

#include <google/protobuf/util/message_differencer.h>

#include "proto_codec.h"  // gen/proto_codec.h — mwf::ProtoCodec + type aliases

using namespace mwf;

static int g_failures = 0;
#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

// Round-trip a message through the codec, asserting SEMANTIC equality.
// NB: we use MessageDifferencer, not byte comparison — protobuf does not
// guarantee a deterministic serialization order for map<> fields (Payload.metadata
// etc.), so two equal messages can serialize to different byte strings. The codec
// contract is "decode(encode(msg)) preserves the message", not byte-identity.
template <class Msg>
static Msg roundtrip(const Msg& in, const char* name) {
  ProtoCodec<Msg> codec;
  Bytes wire = codec.encode(in);
  CHECK(!wire.empty() || in.ByteSizeLong() == 0, "encode produced no bytes");

  Msg out;
  bool ok = codec.decode(wire, out);
  CHECK(ok, "decode returned false");
  CHECK(google::protobuf::util::MessageDifferencer::Equals(in, out),
        "decode(encode(msg)) != msg");
  std::printf("  ok: %-32s (%zu bytes)\n", name, wire.size());
  return out;
}

int main() {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  std::puts("proto host round-trip:");

  // 1) Payload — metadata map + data bytes (the core envelope the codec wraps).
  {
    proto::Payload p;
    (*p.mutable_metadata())["encoding"] = "json/plain";
    (*p.mutable_metadata())["execution_id"] = "exec-42";
    p.set_data("{\"hello\":\"world\"}");
    proto::Payload back = roundtrip(p, "Payload");
    CHECK(back.metadata().at("encoding") == "json/plain", "Payload metadata lost");
    CHECK(back.data() == "{\"hello\":\"world\"}", "Payload data lost");
  }

  // 2) PollActivityTaskQueueResponse — the activity worker's poll result, nesting
  //    WorkflowExecution, ActivityType, WorkflowType, Payloads(input), token bytes.
  {
    proto::PollActivityTaskQueueResponse r;
    r.set_task_token(std::string("tok-\x00\x01\x02-bin", 13));  // binary-safe token
    r.set_activity_id("act-7");
    r.mutable_activity_type()->set_name("SummarizeText");
    r.mutable_workflow_type()->set_name("DocPipeline");
    r.set_workflow_namespace("default");
    auto* we = r.mutable_workflow_execution();
    we->set_workflow_id("wf-1");
    we->set_run_id("run-abc");
    auto* pl = r.mutable_input()->add_payloads();
    (*pl->mutable_metadata())["encoding"] = "json/plain";
    pl->set_data("[1,2,3]");
    proto::PollActivityTaskQueueResponse back =
        roundtrip(r, "PollActivityTaskQueueResponse");
    CHECK(back.task_token() == std::string("tok-\x00\x01\x02-bin", 13),
          "binary task_token lost");
    CHECK(back.activity_type().name() == "SummarizeText", "activity_type lost");
    CHECK(back.workflow_execution().run_id() == "run-abc", "workflow_execution lost");
    CHECK(back.input().payloads_size() == 1, "input payloads lost");
  }

  // 3) HistoryEvent — an ActivityTaskStarted event with nested attributes; the
  //    unit the core replay engine consumes.
  {
    proto::HistoryEvent ev;
    ev.set_event_id(5);
    ev.set_task_id(99);
    ev.mutable_event_time()->set_seconds(1720000000);
    ev.mutable_event_time()->set_nanos(123);
    auto* attrs = ev.mutable_activity_task_started_event_attributes();
    attrs->set_scheduled_event_id(4);
    attrs->set_attempt(1);
    attrs->set_identity("nimbus-worker");
    proto::HistoryEvent back = roundtrip(ev, "HistoryEvent");
    CHECK(back.event_id() == 5, "event_id lost");
    CHECK(back.event_time().seconds() == 1720000000, "event_time lost");
    CHECK(back.activity_task_started_event_attributes().identity() == "nimbus-worker",
          "nested attributes lost");
  }

  // 4) Empty message: an empty Payloads must round-trip to zero bytes and back.
  {
    proto::Payloads empty;
    roundtrip(empty, "Payloads(empty)");
  }

  if (g_failures) {
    std::fprintf(stderr, "\n%d check(s) FAILED\n", g_failures);
    return 1;
  }
  std::puts("\nall round-trips green");
  google::protobuf::ShutdownProtobufLibrary();
  return 0;
}

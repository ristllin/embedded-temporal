// proto: cross-lib conformance for the DEVICE WORKFLOW-TASK subset.
//
// The proof that proto/mwf/device_workflow.proto is WIRE-IDENTICAL to the real
// temporal.api workflow-task messages — asserted two ways, one of them against
// conformance's REAL golden histories:
//
//   1. GOLDEN HISTORIES: load each conformance golden (linear /
//      conditional_even / conditional_odd) via the FULL libprotobuf
//      (JsonStringToMessage → temporal History), pack it into a real
//      PollWorkflowTaskQueueResponse, serialize to BINARY, DECODE that binary
//      with the nanopb device types, and assert every field the replay engine
//      needs survives (event ids/types, activity names + ids, payload data bytes,
//      scheduled/started back-refs, workflow type/run ids, results).
//   2. TRIMMED-ONEOF WIRE-COMPAT: a hand-built response whose History contains a
//      MarkerRecordedEventAttributes event (arm #25 — deliberately NOT in the
//      device oneof) wedged between kept events. nanopb must decode the kept
//      events and SKIP the marker as an unknown field (proto3), proving the
//      trimmed oneof stays wire-compatible.
//   3. COMMANDS (reverse): nanopb-ENCODE a RespondWorkflowTaskCompletedRequest
//      carrying ScheduleActivityTask + StartTimer + CompleteWorkflowExecution +
//      FailWorkflowExecution commands, libprotobuf-DECODE the real message, and
//      assert each command arm + its fields.
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <google/protobuf/util/json_util.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "proto_codec.h"  // mwf::proto:: aliases over libprotobuf
#include "temporal/api/enums/v1/event_type.pb.h"
#include "temporal/api/enums/v1/command_type.pb.h"

#include "mwf/device_workflow.pb.h"  // nanopb device types (needs PB_FIELD_32BIT)
#include "nanopb_workflow_adapter.h" // the DEVICE workflow adapter (Signal builder)

static int g_failures = 0;
#define CHECK(cond, what)                                          \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL: %s (%s:%d)\n", what, __FILE__, __LINE__); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

namespace hist = ::temporal::api::history::v1;
namespace enums = ::temporal::api::enums::v1;

// ── nanopb encode helpers (mirror gen/nanopb_worker_adapter.cpp) ──────────────
template <size_t N>
static bool setStr(char (&dst)[N], const std::string& src) {
  if (src.size() >= N) return false;
  std::memcpy(dst, src.data(), src.size());
  dst[src.size()] = '\0';
  return true;
}
template <class PbBytes>
static bool setBytes(PbBytes& dst, const void* src, size_t n) {
  if (n > sizeof(dst.bytes)) return false;
  dst.size = static_cast<pb_size_t>(n);
  if (n) std::memcpy(dst.bytes, src, n);
  return true;
}
static std::string encodeMsg(const pb_msgdesc_t* fields, const void* msg) {
  size_t size = 0;
  if (!pb_get_encoded_size(&size, fields, msg)) return {};
  std::string out(size, '\0');
  pb_ostream_t os =
      pb_ostream_from_buffer(reinterpret_cast<pb_byte_t*>(out.data()), size);
  if (!pb_encode(&os, fields, msg)) return {};
  out.resize(os.bytes_written);
  return out;
}

static std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A device Payload's decoded data bytes, as a std::string.
static std::string devData(const mwf_device_v1_Payload& p) {
  return std::string(reinterpret_cast<const char*>(p.data.bytes), p.data.size);
}

// ── 1+2. golden-history + trimmed-oneof round-trip ────────────────────────────
static void checkGolden(const std::string& path) {
  const std::string json = readFile(path);
  CHECK(!json.empty(), "golden file read");
  if (json.empty()) return;

  hist::History srcHist;
  auto st = google::protobuf::util::JsonStringToMessage(json, &srcHist);
  CHECK(st.ok(), "golden JSON → temporal History parsed");
  if (!st.ok()) {
    std::printf("  (%s: %s)\n", path.c_str(), std::string(st.message()).c_str());
    return;
  }
  CHECK(srcHist.events_size() > 0, "history has events");

  // Pack into a real PollWorkflowTaskQueueResponse (as the server sends it).
  mwf::proto::PollWorkflowTaskQueueResponse resp;
  resp.set_task_token(std::string(64, '\x37'));
  resp.mutable_workflow_type()->set_name("LinearWorkflow");
  resp.mutable_workflow_execution()->set_workflow_id("wf-golden");
  resp.mutable_workflow_execution()->set_run_id("run-golden-1");
  resp.set_started_event_id(3);
  resp.set_previous_started_event_id(0);
  resp.set_attempt(1);
  *resp.mutable_history() = srcHist;

  std::string wire;
  CHECK(resp.SerializeToString(&wire), "serialize PollWFTQResponse");

  // Decode with the nanopb device types (heap — ~2.5 MB at the caps; PSRAM on device).
  auto dev = std::make_unique<mwf_device_v1_PollWorkflowTaskQueueResponse>();
  *dev = mwf_device_v1_PollWorkflowTaskQueueResponse_init_zero;
  pb_istream_t is =
      pb_istream_from_buffer(reinterpret_cast<const pb_byte_t*>(wire.data()),
                             wire.size());
  bool ok = pb_decode(&is, mwf_device_v1_PollWorkflowTaskQueueResponse_fields,
                      dev.get());
  CHECK(ok, "nanopb decodes real PollWFTQResponse binary");
  if (!ok) {
    std::printf("  (%s: %s)\n", path.c_str(), PB_GET_ERROR(&is));
    return;
  }

  // Poll metadata the loop needs.
  CHECK(dev->has_workflow_type && std::string(dev->workflow_type.name) ==
                                      "LinearWorkflow",
        "workflow_type.name survived");
  CHECK(dev->has_workflow_execution &&
            std::string(dev->workflow_execution.run_id) == "run-golden-1",
        "workflow_execution.run_id survived");
  CHECK(dev->started_event_id == 3, "started_event_id survived");
  CHECK(dev->attempt == 1, "attempt survived");
  CHECK(dev->next_page_token.size == 0, "single-page: next_page_token empty");

  // Every event survived, in order, with type + id intact.
  CHECK(dev->has_history, "history present");
  CHECK(dev->history.events_count ==
            static_cast<pb_size_t>(srcHist.events_size()),
        "event count matches");

  int checkedActivities = 0, checkedResults = 0;
  for (int i = 0; i < srcHist.events_size(); ++i) {
    const auto& se = srcHist.events(i);
    const auto& de = dev->history.events[i];
    CHECK(de.event_id == se.event_id(), "event_id matches");
    CHECK(de.event_type == static_cast<int32_t>(se.event_type()),
          "event_type matches");

    switch (se.event_type()) {
      case enums::EVENT_TYPE_WORKFLOW_EXECUTION_STARTED: {
        CHECK(de.which_attributes ==
                  mwf_device_v1_HistoryEvent_workflow_execution_started_event_attributes_tag,
              "started arm decoded");
        const auto& sa = se.workflow_execution_started_event_attributes();
        const auto& da = de.attributes.workflow_execution_started_event_attributes;
        CHECK(std::string(da.workflow_type.name) == sa.workflow_type().name(),
              "started.workflow_type.name");
        if (sa.input().payloads_size() == 1) {
          CHECK(da.input.payloads_count == 1, "started.input one payload");
          CHECK(devData(da.input.payloads[0]) == sa.input().payloads(0).data(),
                "started.input payload data bytes");
        }
        break;
      }
      case enums::EVENT_TYPE_ACTIVITY_TASK_SCHEDULED: {
        CHECK(de.which_attributes ==
                  mwf_device_v1_HistoryEvent_activity_task_scheduled_event_attributes_tag,
              "activity-scheduled arm decoded");
        const auto& sa = se.activity_task_scheduled_event_attributes();
        const auto& da = de.attributes.activity_task_scheduled_event_attributes;
        CHECK(std::string(da.activity_id) == sa.activity_id(),
              "scheduled.activity_id");
        CHECK(std::string(da.activity_type.name) == sa.activity_type().name(),
              "scheduled.activity_type.name");
        CHECK(da.input.payloads_count ==
                  static_cast<pb_size_t>(sa.input().payloads_size()),
              "scheduled.input count");
        if (sa.input().payloads_size() == 1)
          CHECK(devData(da.input.payloads[0]) == sa.input().payloads(0).data(),
                "scheduled.input payload data bytes");
        ++checkedActivities;
        break;
      }
      case enums::EVENT_TYPE_ACTIVITY_TASK_STARTED: {
        const auto& sa = se.activity_task_started_event_attributes();
        const auto& da = de.attributes.activity_task_started_event_attributes;
        CHECK(da.scheduled_event_id == sa.scheduled_event_id(),
              "activity-started scheduled_event_id back-ref");
        break;
      }
      case enums::EVENT_TYPE_ACTIVITY_TASK_COMPLETED: {
        const auto& sa = se.activity_task_completed_event_attributes();
        const auto& da = de.attributes.activity_task_completed_event_attributes;
        CHECK(da.scheduled_event_id == sa.scheduled_event_id(),
              "activity-completed scheduled_event_id back-ref");
        CHECK(da.started_event_id == sa.started_event_id(),
              "activity-completed started_event_id back-ref");
        if (sa.result().payloads_size() == 1)
          CHECK(devData(da.result.payloads[0]) == sa.result().payloads(0).data(),
                "completed.result payload data bytes");
        ++checkedResults;
        break;
      }
      case enums::EVENT_TYPE_WORKFLOW_EXECUTION_COMPLETED: {
        const auto& sa = se.workflow_execution_completed_event_attributes();
        const auto& da = de.attributes.workflow_execution_completed_event_attributes;
        if (sa.result().payloads_size() == 1)
          CHECK(devData(da.result.payloads[0]) == sa.result().payloads(0).data(),
                "wf-completed.result payload data bytes");
        break;
      }
      default:
        break;  // WorkflowTask scheduled/started/completed: id-only, checked above
    }
  }
  CHECK(checkedActivities >= 1, "at least one ActivityTaskScheduled checked");
  CHECK(checkedResults >= 1, "at least one ActivityTaskCompleted checked");
  std::printf("  golden %s: %d events, %d activities, %d results — OK\n",
              path.c_str(), srcHist.events_size(), checkedActivities,
              checkedResults);
}

// ── 1b. REAL Mistral history: the field-overflow regression (hardware-caught) ──
// A golden-only conformance suite hid a live bug: the goldens use SHORT synthetic
// ids ("linear-goldencapture-0001"), but a REAL Mistral PollWorkflowTaskQueue
// history carries a 64-hex-char execution id as workflow_id — which overran the
// old `* max_size:64` nanopb cap (holds 63 chars + NUL) and made the device
// abort the whole poll with "string overflow" on the ESP32-S3.
//
// This drives the ACTUAL captured wire: probe/workflow_task_1.json is a full
// PollWorkflowTaskQueueResponse (proto-JSON). We parse it with libprotobuf,
// re-serialize to the exact server binary, and decode THAT with the nanopb
// device types — asserting the decode now succeeds and every replay-critical
// field (incl. the 64-char workflow_id in BOTH the top-level WorkflowExecution
// and the WorkflowExecutionStartedEventAttributes) survives. RED before the cap
// fix (string overflow), GREEN after.
static void checkRealMistralHistory(const std::string& path) {
  const std::string json = readFile(path);
  CHECK(!json.empty(), "real probe file read");
  if (json.empty()) return;

  // The probe is a whole response as the Mistral frontend sent it.
  mwf::proto::PollWorkflowTaskQueueResponse resp;
  auto st = google::protobuf::util::JsonStringToMessage(json, &resp);
  CHECK(st.ok(), "real probe JSON → temporal PollWFTQResponse parsed");
  if (!st.ok()) {
    std::printf("  (%s: %s)\n", path.c_str(), std::string(st.message()).c_str());
    return;
  }

  // Guard: prove the fixture actually exercises the >63-char case the bug needs,
  // so this regression can never silently stop testing the overflow.
  CHECK(resp.workflow_execution().workflow_id().size() == 64,
        "fixture workflow_id is a 64-char execution id (exercises the overflow)");

  std::string wire;
  CHECK(resp.SerializeToString(&wire), "serialize real PollWFTQResponse");

  auto dev = std::make_unique<mwf_device_v1_PollWorkflowTaskQueueResponse>();
  *dev = mwf_device_v1_PollWorkflowTaskQueueResponse_init_zero;
  pb_istream_t is = pb_istream_from_buffer(
      reinterpret_cast<const pb_byte_t*>(wire.data()), wire.size());
  bool ok = pb_decode(&is, mwf_device_v1_PollWorkflowTaskQueueResponse_fields,
                      dev.get());
  CHECK(ok, "nanopb decodes REAL Mistral PollWFTQResponse (was: string overflow)");
  if (!ok) {
    std::printf("  (%s: nanopb decode failed: %s)\n", path.c_str(),
                PB_GET_ERROR(&is));
    return;
  }

  // task_token (opaque server bytes) survived intact — 174 B measured.
  CHECK(std::string(reinterpret_cast<const char*>(dev->task_token.bytes),
                    dev->task_token.size) == resp.task_token(),
        "real task_token bytes survived");

  // Top-level WorkflowExecution (shared type): the 64-char workflow_id + run_id.
  CHECK(dev->has_workflow_execution, "workflow_execution present");
  CHECK(std::string(dev->workflow_execution.workflow_id) ==
            resp.workflow_execution().workflow_id(),
        "workflow_execution.workflow_id (64-hex) survived");
  CHECK(std::string(dev->workflow_execution.run_id) ==
            resp.workflow_execution().run_id(),
        "workflow_execution.run_id survived");
  CHECK(dev->has_workflow_type &&
            std::string(dev->workflow_type.name) == resp.workflow_type().name(),
        "workflow_type.name survived");
  CHECK(dev->started_event_id == resp.started_event_id(),
        "started_event_id survived");
  CHECK(dev->attempt == resp.attempt(), "attempt survived");

  // Every event survived, in order.
  CHECK(dev->has_history, "history present");
  CHECK(dev->history.events_count ==
            static_cast<pb_size_t>(resp.history().events_size()),
        "real history event count matches");

  bool sawStarted = false, sawTaskStarted = false;
  for (int i = 0; i < resp.history().events_size(); ++i) {
    const auto& se = resp.history().events(i);
    const auto& de = dev->history.events[i];
    CHECK(de.event_id == se.event_id(), "real event_id matches");
    CHECK(de.event_type == static_cast<int32_t>(se.event_type()),
          "real event_type matches");

    if (se.event_type() == enums::EVENT_TYPE_WORKFLOW_EXECUTION_STARTED) {
      sawStarted = true;
      CHECK(de.which_attributes ==
                mwf_device_v1_HistoryEvent_workflow_execution_started_event_attributes_tag,
            "started arm decoded");
      const auto& sa = se.workflow_execution_started_event_attributes();
      const auto& da = de.attributes.workflow_execution_started_event_attributes;
      // THE overflow field: workflow_id=28, a 64-char hex id on real data.
      CHECK(std::string(da.workflow_id) == sa.workflow_id(),
            "started.workflow_id (64-hex — the overflow field) survived");
      CHECK(std::string(da.workflow_type.name) == sa.workflow_type().name(),
            "started.workflow_type.name survived");
      CHECK(da.input.payloads_count == 1, "started.input one payload");
      if (da.input.payloads_count == 1)
        CHECK(devData(da.input.payloads[0]) == sa.input().payloads(0).data(),
              "started.input payload data bytes survived");
    } else if (se.event_type() == enums::EVENT_TYPE_WORKFLOW_TASK_STARTED) {
      sawTaskStarted = true;
      const auto& sa = se.workflow_task_started_event_attributes();
      const auto& da = de.attributes.workflow_task_started_event_attributes;
      CHECK(da.scheduled_event_id == sa.scheduled_event_id(),
            "wf-task-started scheduled_event_id back-ref survived");
    }
  }
  CHECK(sawStarted, "real history had a WorkflowExecutionStarted event");
  CHECK(sawTaskStarted, "real history had a WorkflowTaskStarted event");
  std::printf("  real Mistral %s: %d events, 64-hex workflow_id decoded — OK\n",
              path.c_str(), resp.history().events_size());
}

// ── 2. explicit trimmed-oneof: an OMITTED arm (MarkerRecorded #25) is skipped ──
static void checkTrimmedOneofSkipsUnknownArm() {
  mwf::proto::PollWorkflowTaskQueueResponse resp;
  resp.set_started_event_id(9);
  auto* h = resp.mutable_history();
  // kept arm
  auto* e1 = h->add_events();
  e1->set_event_id(1);
  e1->set_event_type(enums::EVENT_TYPE_ACTIVITY_TASK_SCHEDULED);
  auto* a = e1->mutable_activity_task_scheduled_event_attributes();
  a->set_activity_id("act-1");
  a->mutable_activity_type()->set_name("greet");
  // OMITTED arm: MarkerRecorded (#25) — not in the device oneof
  auto* e2 = h->add_events();
  e2->set_event_id(2);
  e2->set_event_type(enums::EVENT_TYPE_MARKER_RECORDED);
  auto* m = e2->mutable_marker_recorded_event_attributes();
  m->set_marker_name("Version");
  (*m->mutable_details())["foo"];  // some bulk on an unknown arm
  // another kept arm after it
  auto* e3 = h->add_events();
  e3->set_event_id(3);
  e3->set_event_type(enums::EVENT_TYPE_ACTIVITY_TASK_COMPLETED);
  e3->mutable_activity_task_completed_event_attributes()->set_scheduled_event_id(1);

  std::string wire;
  CHECK(resp.SerializeToString(&wire), "serialize response with marker event");

  auto dev = std::make_unique<mwf_device_v1_PollWorkflowTaskQueueResponse>();
  *dev = mwf_device_v1_PollWorkflowTaskQueueResponse_init_zero;
  pb_istream_t is = pb_istream_from_buffer(
      reinterpret_cast<const pb_byte_t*>(wire.data()), wire.size());
  bool ok = pb_decode(&is, mwf_device_v1_PollWorkflowTaskQueueResponse_fields,
                      dev.get());
  CHECK(ok, "nanopb decodes history containing an unknown (marker) arm");
  if (!ok) return;

  // All three events present; the marker event decoded with NO active oneof arm.
  CHECK(dev->history.events_count == 3, "all 3 events kept (marker not dropped)");
  CHECK(dev->history.events[0].which_attributes ==
            mwf_device_v1_HistoryEvent_activity_task_scheduled_event_attributes_tag,
        "event 1 activity-scheduled arm decoded");
  CHECK(std::string(dev->history.events[0]
                        .attributes.activity_task_scheduled_event_attributes
                        .activity_id) == "act-1",
        "event 1 activity_id intact around the skipped arm");
  CHECK(dev->history.events[1].event_id == 2 &&
            dev->history.events[1].event_type ==
                enums::EVENT_TYPE_MARKER_RECORDED,
        "marker event id+type kept (scalar fields), unknown arm skipped");
  CHECK(dev->history.events[1].which_attributes == 0,
        "marker's oneof arm decoded as unknown (which_attributes == 0)");
  CHECK(dev->history.events[2].which_attributes ==
            mwf_device_v1_HistoryEvent_activity_task_completed_event_attributes_tag,
        "event 3 after the skipped arm decoded correctly");
  std::printf("  trimmed-oneof: MarkerRecorded (#25) skipped, neighbors OK\n");
}

// ── 3. commands: nanopb encode → libprotobuf decode ───────────────────────────
static void checkCommands() {
  auto req = std::make_unique<mwf_device_v1_RespondWorkflowTaskCompletedRequest>();
  *req = mwf_device_v1_RespondWorkflowTaskCompletedRequest_init_zero;
  const std::string token = "\x11\x22\x33";
  setBytes(req->task_token, token.data(), token.size());
  setStr(req->identity, std::string("mwf-esp-worker@wf"));
  setStr(req->namespace_, std::string("ns-uuid:ns-uuid"));

  req->commands_count = 4;

  // [0] ScheduleActivityTask
  {
    auto& c = req->commands[0];
    c.command_type = enums::COMMAND_TYPE_SCHEDULE_ACTIVITY_TASK;
    c.which_attributes =
        mwf_device_v1_Command_schedule_activity_task_command_attributes_tag;
    auto& s = c.attributes.schedule_activity_task_command_attributes;
    setStr(s.activity_id, std::string("1"));
    s.has_activity_type = true;
    setStr(s.activity_type.name, std::string("greet"));
    s.has_task_queue = true;
    setStr(s.task_queue.name, std::string("mwf-goldens"));
    s.has_input = true;
    s.input.payloads_count = 1;
    auto& p = s.input.payloads[0];
    p.metadata_count = 1;
    setStr(p.metadata[0].key, std::string("encoding"));
    const std::string enc = "json/plain";
    setBytes(p.metadata[0].value, enc.data(), enc.size());
    const std::string data = "\"world\"";
    setBytes(p.data, data.data(), data.size());
    s.has_start_to_close_timeout = true;
    s.start_to_close_timeout.seconds = 10;
  }
  // [1] StartTimer
  {
    auto& c = req->commands[1];
    c.command_type = enums::COMMAND_TYPE_START_TIMER;
    c.which_attributes =
        mwf_device_v1_Command_start_timer_command_attributes_tag;
    auto& t = c.attributes.start_timer_command_attributes;
    setStr(t.timer_id, std::string("timer-7"));
    t.has_start_to_fire_timeout = true;
    t.start_to_fire_timeout.seconds = 30;
  }
  // [2] CompleteWorkflowExecution
  {
    auto& c = req->commands[2];
    c.command_type = enums::COMMAND_TYPE_COMPLETE_WORKFLOW_EXECUTION;
    c.which_attributes =
        mwf_device_v1_Command_complete_workflow_execution_command_attributes_tag;
    auto& w = c.attributes.complete_workflow_execution_command_attributes;
    w.has_result = true;
    w.result.payloads_count = 1;
    const std::string data = "\"HELLO, WORLD!\"";
    setBytes(w.result.payloads[0].data, data.data(), data.size());
  }
  // [3] FailWorkflowExecution
  {
    auto& c = req->commands[3];
    c.command_type = enums::COMMAND_TYPE_FAIL_WORKFLOW_EXECUTION;
    c.which_attributes =
        mwf_device_v1_Command_fail_workflow_execution_command_attributes_tag;
    auto& f = c.attributes.fail_workflow_execution_command_attributes;
    f.has_failure = true;
    setStr(f.failure.message, std::string("boom in replay"));
  }

  std::string wire = encodeMsg(
      mwf_device_v1_RespondWorkflowTaskCompletedRequest_fields, req.get());
  CHECK(!wire.empty(), "nanopb encodes RespondWorkflowTaskCompletedRequest");

  mwf::proto::RespondWorkflowTaskCompletedReq lp;
  CHECK(lp.ParseFromArray(wire.data(), static_cast<int>(wire.size())),
        "libprotobuf parses nanopb RespondWorkflowTaskCompletedRequest");
  CHECK(lp.task_token() == token, "commands.task_token");
  CHECK(lp.identity() == "mwf-esp-worker@wf", "commands.identity");
  CHECK(lp.namespace_() == "ns-uuid:ns-uuid", "commands.namespace");
  CHECK(lp.commands_size() == 4, "four commands");

  const auto& c0 = lp.commands(0);
  CHECK(c0.command_type() == enums::COMMAND_TYPE_SCHEDULE_ACTIVITY_TASK,
        "cmd0 type SCHEDULE_ACTIVITY_TASK");
  CHECK(c0.has_schedule_activity_task_command_attributes(), "cmd0 schedule arm");
  const auto& s = c0.schedule_activity_task_command_attributes();
  CHECK(s.activity_id() == "1", "cmd0.activity_id");
  CHECK(s.activity_type().name() == "greet", "cmd0.activity_type.name");
  CHECK(s.task_queue().name() == "mwf-goldens", "cmd0.task_queue.name");
  CHECK(s.input().payloads_size() == 1 &&
            s.input().payloads(0).data() == "\"world\"",
        "cmd0.input payload data");
  CHECK(s.input().payloads(0).metadata().at("encoding") == "json/plain",
        "cmd0.input payload metadata");
  CHECK(s.start_to_close_timeout().seconds() == 10, "cmd0.start_to_close 10s");

  const auto& c1 = lp.commands(1);
  CHECK(c1.command_type() == enums::COMMAND_TYPE_START_TIMER, "cmd1 START_TIMER");
  CHECK(c1.start_timer_command_attributes().timer_id() == "timer-7",
        "cmd1.timer_id");
  CHECK(c1.start_timer_command_attributes().start_to_fire_timeout().seconds() ==
            30,
        "cmd1.start_to_fire 30s");

  const auto& c2 = lp.commands(2);
  CHECK(c2.command_type() == enums::COMMAND_TYPE_COMPLETE_WORKFLOW_EXECUTION,
        "cmd2 COMPLETE_WORKFLOW_EXECUTION");
  CHECK(c2.complete_workflow_execution_command_attributes()
                .result()
                .payloads(0)
                .data() == "\"HELLO, WORLD!\"",
        "cmd2.result payload data");

  const auto& c3 = lp.commands(3);
  CHECK(c3.command_type() == enums::COMMAND_TYPE_FAIL_WORKFLOW_EXECUTION,
        "cmd3 FAIL_WORKFLOW_EXECUTION");
  CHECK(c3.fail_workflow_execution_command_attributes().failure().message() ==
            "boom in replay",
        "cmd3.failure.message");
  std::printf("  commands: 4 arms (schedule/timer/complete/fail) → OK\n");
}

// ── 4. SignalWorkflowExecutionRequest: nanopb ⇄ libprotobuf round-trip ────────
// The SIGNAL SENDER's RPC (ClientOps::signalWorkflow → wait_signal unblocks). The
// DEVICE nanopb adapter builds it; libprotobuf decodes the REAL temporal message
// and every field survives — then the reverse. Proves the hand-mirrored
// SignalWorkflowExecutionRequest (field numbers 1/2/3/4/5) is wire-identical.
static void checkSignalRequest() {
  // Forward: nanopb adapter encode → libprotobuf decode.
  mwf_proto::NanopbWorkflowAdapter adapter;
  mwf_core::SignalRequest s;
  s.ns = "ns-uuid:ns-uuid";
  s.workflow_id = std::string(64, 'a');  // 64-hex execution id — the cap lesson
  s.run_id = "run-abc";
  s.signal_name = "approve";
  s.identity = "mwf-client@sig";
  s.input.metadata["encoding"] = "json/plain";
  const std::string arg = "{\"approved\":true}";
  s.input.data = mwf::Bytes(arg.begin(), arg.end());

  mwf::Bytes wire = adapter.buildSignalWorkflowRequest(s);
  CHECK(!wire.empty(), "nanopb adapter builds SignalWorkflowExecutionRequest");

  mwf::proto::SignalWorkflowExecutionRequest lp;
  CHECK(lp.ParseFromArray(wire.data(), static_cast<int>(wire.size())),
        "libprotobuf parses nanopb SignalWorkflowExecutionRequest");
  CHECK(lp.namespace_() == s.ns, "signal.namespace");
  CHECK(lp.workflow_execution().workflow_id() == s.workflow_id,
        "signal.workflow_id (64-hex) survived");
  CHECK(lp.workflow_execution().run_id() == s.run_id, "signal.run_id");
  CHECK(lp.signal_name() == "approve", "signal.signal_name");
  CHECK(lp.identity() == "mwf-client@sig", "signal.identity");
  CHECK(lp.input().payloads_size() == 1, "signal.input one payload");
  CHECK(lp.input().payloads_size() == 1 && lp.input().payloads(0).data() == arg,
        "signal.input payload data");
  CHECK(lp.input().payloads_size() == 1 &&
            lp.input().payloads(0).metadata().at("encoding") == "json/plain",
        "signal.input payload metadata");

  // Reverse: libprotobuf encode → nanopb device decode.
  mwf::proto::SignalWorkflowExecutionRequest src;
  src.set_namespace_("N");
  src.mutable_workflow_execution()->set_workflow_id("wf-rev");
  src.mutable_workflow_execution()->set_run_id("run-rev");
  src.set_signal_name("cancel");
  src.set_identity("id-rev");
  auto* p = src.mutable_input()->add_payloads();
  (*p->mutable_metadata())["encoding"] = "json/plain";
  p->set_data("42");
  std::string sbin;
  CHECK(src.SerializeToString(&sbin), "serialize libprotobuf SignalWFExecRequest");

  auto dev = std::make_unique<mwf_device_v1_SignalWorkflowExecutionRequest>();
  *dev = mwf_device_v1_SignalWorkflowExecutionRequest_init_zero;
  pb_istream_t is = pb_istream_from_buffer(
      reinterpret_cast<const pb_byte_t*>(sbin.data()), sbin.size());
  bool ok =
      pb_decode(&is, mwf_device_v1_SignalWorkflowExecutionRequest_fields, dev.get());
  CHECK(ok, "nanopb decodes libprotobuf SignalWorkflowExecutionRequest");
  if (ok) {
    CHECK(std::string(dev->namespace_) == "N", "rev signal.namespace");
    CHECK(dev->has_workflow_execution &&
              std::string(dev->workflow_execution.workflow_id) == "wf-rev",
          "rev signal.workflow_id");
    CHECK(std::string(dev->workflow_execution.run_id) == "run-rev",
          "rev signal.run_id");
    CHECK(std::string(dev->signal_name) == "cancel", "rev signal.signal_name");
    CHECK(std::string(dev->identity) == "id-rev", "rev signal.identity");
    CHECK(dev->has_input && dev->input.payloads_count == 1,
          "rev signal.input one payload");
    CHECK(dev->input.payloads_count == 1 && devData(dev->input.payloads[0]) == "42",
          "rev signal.input payload data");
  }
  std::printf("  signal: SignalWorkflowExecutionRequest nanopb<->libprotobuf — OK\n");
}

int main() {
#ifdef MWF_GOLDENS_DIR
  const std::string dir = MWF_GOLDENS_DIR;
  for (const char* name : {"linear.json", "conditional_even.json",
                           "conditional_odd.json"}) {
    checkGolden(dir + "/" + name);
  }
#else
  std::printf("  (MWF_GOLDENS_DIR not set — golden round-trip skipped)\n");
#endif
#ifdef MWF_PROBE_JSON
  checkRealMistralHistory(MWF_PROBE_JSON);
#else
  std::printf("  (MWF_PROBE_JSON not set — real-history regression skipped)\n");
#endif
  checkTrimmedOneofSkipsUnknownArm();
  checkCommands();
  checkSignalRequest();

  if (g_failures == 0)
    std::printf("nanopb device WORKFLOW conformance: ALL OK\n");
  return g_failures ? 1 : 0;
}

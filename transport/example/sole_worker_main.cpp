// transport example: sole_worker_main — the DESKTOP SOLE-WORKER gate.
//
// Proves our REAL components act as a SOLE Mistral Workflows worker with NO
// Python host: the desktop orchestrates the WHOLE workflow itself. One process,
// one task queue, ONE thread round-robining BOTH loops:
//
//   WorkflowLoop (core) — polls the WORKFLOW task queue, replays a declarative
//                         WorkflowSpec through the deterministic ReplayEngine,
//                         responds ScheduleActivityTask / CompleteWorkflow.
//   WorkerLoop   (core) — polls the SAME queue for the activity task, runs
//                         device.echo, responds RespondActivityTaskCompleted.
//
// over DesktopTransport (grpc++ generic stub, TLS) + PayloadCodecV1 (wf_v1) +
// the two libprotobuf proto adapters — exactly the seams the ESP32 will run over
// nanopb. This is the desktop de-risk before hardware.
//
// The one C++ binary has three modes so the whole E2E is Python-free:
//   --mode worker  : whoami → REST-register the workflow (self-registration!) →
//                    run both loops until the workflow COMPLETES (or --max-seconds).
//                    Prints the emitted command sequence as evidence.
//   --mode trigger : wait for worker-active → REST execute → poll executions to a
//                    terminal status → assert result contains --expect → evidence JSON.
//   --mode cleanup : archive the workflow (+ optionally terminate a leftover run).
//
//   ./mwf_sole_worker --mode worker  --api-key-env MISTRAL_API_KEY \
//       --workflow mwf_sole_desktop --task-queue mwf-sole-desktop
//   ./mwf_sole_worker --mode trigger --api-key-env MISTRAL_API_KEY \
//       --workflow mwf_sole_desktop --task-queue mwf-sole-desktop --expect desktop-sole
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "desktop_transport.h"
#include "host_proto_adapter.h"       // IWorkerProtoAdapter (activity)
#include "host_workflow_adapter.h"    // IWorkflowProtoAdapter (workflow)
#include "mwf_codec/payload_codec.h"
#include "mwf_core/activity_registry.h"
#include "mwf_core/worker_loop.h"
#include "mwf_core/workflow_loop.h"
#include "mwf_core/workflow_spec.h"
#include "rest_client.h"

using mwf::Bytes;

namespace {

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

std::string toString(const Bytes& b) { return std::string(b.begin(), b.end()); }
Bytes toBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

// ── in-memory durable store (device would use NVS/flash) ──────────────────────
struct MemStore : mwf::IDurableStore {
  std::map<std::string, Bytes> kv;
  bool put(std::string_view k, const Bytes& v) override { kv[std::string(k)] = v; return true; }
  std::optional<Bytes> get(std::string_view k) override {
    auto it = kv.find(std::string(k));
    return it == kv.end() ? std::nullopt : std::optional<Bytes>(it->second);
  }
  bool erase(std::string_view k) override { return kv.erase(std::string(k)) > 0; }
  std::vector<std::string> keys(std::string_view prefix) override {
    std::vector<std::string> out;
    for (auto& p : kv)
      if (p.first.rfind(std::string(prefix), 0) == 0) out.push_back(p.first);
    return out;
  }
};

// Schemas registered with Mistral (opaque to the engine; the frontend validates
// input structurally). device.echo's output is an object, so output_schema is
// permissive.
constexpr const char* kInputSchema =
    "{\"additionalProperties\":false,"
    "\"properties\":{\"name\":{\"title\":\"Name\",\"type\":\"string\"}},"
    "\"required\":[\"name\"],\"title\":\"run_Input\",\"type\":\"object\"}";
constexpr const char* kOutputSchema =
    "{\"additionalProperties\":true,"
    "\"properties\":{\"echo\":{\"type\":\"string\"},\"host\":{\"type\":\"string\"}},"
    "\"title\":\"run_Output\",\"type\":\"object\"}";

// ── the declarative workflow: device.echo → complete with its result ──────────
// steps: [ activity device.echo(args_from /input/name) as "echo",
//          complete result_from /results/echo ]
std::string workflowSpecJson(const std::string& name) {
  return
    "{\"name\":\"" + name + "\","
    "\"input_schema\":" + std::string(kInputSchema) + ","
    "\"output_schema\":" + std::string(kOutputSchema) + ","
    "\"steps\":["
      "{\"type\":\"activity\",\"name\":\"device.echo\",\"id\":\"echo\",\"args_from\":\"/input/name\"},"
      "{\"type\":\"complete\",\"result_from\":\"/results/echo\"}"
    "]}";
}

const char* wfOutcomeName(mwf_core::WorkflowTickOutcome o) {
  switch (o) {
    case mwf_core::WorkflowTickOutcome::Idle: return "idle";
    case mwf_core::WorkflowTickOutcome::Progressed: return "progressed";
    case mwf_core::WorkflowTickOutcome::Waiting: return "waiting";
    case mwf_core::WorkflowTickOutcome::Completed: return "completed";
    case mwf_core::WorkflowTickOutcome::Failed: return "failed";
    case mwf_core::WorkflowTickOutcome::TransportError: return "transport-error";
    case mwf_core::WorkflowTickOutcome::ProtocolError: return "protocol-error";
  }
  return "?";
}
const char* actOutcomeName(mwf_core::TickOutcome o) {
  switch (o) {
    case mwf_core::TickOutcome::Idle: return "idle";
    case mwf_core::TickOutcome::Completed: return "completed";
    case mwf_core::TickOutcome::Failed: return "failed";
    case mwf_core::TickOutcome::TransportError: return "transport-error";
    case mwf_core::TickOutcome::ProtocolError: return "protocol-error";
  }
  return "?";
}

struct Args {
  std::string mode = "worker";           // worker | trigger | cleanup
  std::string api_key_env = "MISTRAL_API_KEY";
  std::string target;                    // override; else whoami
  std::string ns;                        // override; else whoami
  std::string workflow = "mwf_sole_desktop";
  std::string task_queue = "mwf-sole-desktop";
  std::string identity = "mwf-sole@desktop";
  std::string worker_name = "mwf-sole-worker";
  std::string input_name = "nimbus";     // trigger input {"name": <this>}
  std::string expect = "desktop-sole";   // trigger result assertion
  std::string evidence;                  // trigger evidence JSON path
  std::string exec_id;                   // cleanup: terminate this run
  int tls = -1;                          // unset → whoami's tls (else on); --tls 0 opts out
  int poll_ms = 10000;
  int max_seconds = 120;                 // worker overall budget
  int timeout_s = 120;                   // trigger wait-for-terminal
};

bool parseArgs(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto next = [&](std::string& into) {
      if (i + 1 >= argc) return false;
      into = argv[++i];
      return true;
    };
    std::string v;
    if (k == "--mode" && next(v)) a.mode = v;
    else if (k == "--api-key-env" && next(v)) a.api_key_env = v;
    else if (k == "--target" && next(v)) a.target = v;
    else if (k == "--namespace" && next(v)) a.ns = v;
    else if (k == "--workflow" && next(v)) a.workflow = v;
    else if (k == "--task-queue" && next(v)) a.task_queue = v;
    else if (k == "--identity" && next(v)) a.identity = v;
    else if (k == "--worker-name" && next(v)) a.worker_name = v;
    else if (k == "--input-name" && next(v)) a.input_name = v;
    else if (k == "--expect" && next(v)) a.expect = v;
    else if (k == "--evidence" && next(v)) a.evidence = v;
    else if (k == "--exec-id" && next(v)) a.exec_id = v;
    else if (k == "--tls" && next(v)) a.tls = std::atoi(v.c_str());
    else if (k == "--poll-ms" && next(v)) a.poll_ms = std::atoi(v.c_str());
    else if (k == "--max-seconds" && next(v)) a.max_seconds = std::atoi(v.c_str());
    else if (k == "--timeout-s" && next(v)) a.timeout_s = std::atoi(v.c_str());
    else {
      std::cerr << "unknown/incomplete arg: " << k << "\n";
      return false;
    }
  }
  return true;
}

std::string loadKey(const std::string& env_name) {
  const char* key = std::getenv(env_name.c_str());
  if (!key || !*key) {
    std::cerr << "[sole] env var " << env_name << " is empty/unset\n";
    return {};
  }
  return key;
}

// Resolve target+namespace from whoami unless overridden.
bool resolveEndpoint(const std::string& bearer, Args& a) {
  if (!a.target.empty() && !a.ns.empty()) return true;
  mwf_transport::RestClient rest("https://api.mistral.ai", bearer);
  auto who = rest.whoami();
  if (!who) { std::cerr << "[sole] whoami failed: " << who.error << "\n"; return false; }
  if (a.target.empty()) a.target = who.value.scheduler_url;
  if (a.ns.empty()) a.ns = who.value.namespace_;
  if (a.tls < 0) a.tls = who.value.tls ? 1 : 0;
  std::cout << "[sole] whoami: scheduler=" << a.target << " namespace=" << a.ns
            << " tls=" << who.value.tls << "\n" << std::flush;
  return true;
}

// ── worker mode ───────────────────────────────────────────────────────────────
int runWorker(const std::string& bearer, Args& a) {
  if (!resolveEndpoint(bearer, a)) return 1;

  // 1. SELF-REGISTER the workflow via REST (proves the device can deploy itself).
  mwf_transport::RestClient rest("https://api.mistral.ai", bearer);
  std::cout << "[sole] registering workflow '" << a.workflow << "' on queue '"
            << a.task_queue << "' ...\n" << std::flush;
  auto reg = rest.registerWorkflow(a.workflow, a.task_queue, kInputSchema,
                                   kOutputSchema, a.worker_name);
  if (!reg) { std::cerr << "[sole] register failed: " << reg.error << "\n"; return 1; }
  std::cout << "[sole] register OK: " << reg.value.substr(0, 200) << "\n" << std::flush;

  // 2. Build the declarative spec + activity registry.
  auto specParsed = mwf_core::parseWorkflowSpec(workflowSpecJson(a.workflow));
  if (!specParsed) { std::cerr << "[sole] spec parse: " << specParsed.error << "\n"; return 1; }
  mwf_core::WorkflowSpec spec = std::move(specParsed.value);

  mwf_core::ActivityRegistry registry;
  const std::string identity = a.identity;
  registry.registerActivity("device.echo", [identity](const Bytes& arg) {
    // arg is inner JSON (the codec already unwrapped wf_v1). Echo it back inside
    // a branded object so the run result carries the desktop host marker.
    std::string argJson = toString(arg);
    if (argJson.empty()) argJson = "null";
    std::string out = "{\"echo\":" + argJson +
                      ",\"host\":\"desktop-sole\",\"worker\":\"" + identity +
                      "\",\"engine\":\"mwf-cpp\"}";
    return mwf::Result<Bytes>::success(toBytes(out));
  });

  // 3. One transport, both loops, one queue.
  // TLS on unless explicitly opted out; the transport refuses to send the
  // bearer over plaintext.
  mwf_transport::DesktopTransport transport(a.target, bearer, a.tls != 0);
  mwf_codec::PayloadCodecV1 codec;
  mwf_example::HostWorkflowAdapter wfAdapter;
  mwf_example::HostProtoAdapter actAdapter;
  MemStore store;

  mwf_core::WorkflowConfig wcfg;
  wcfg.ns = a.ns;
  wcfg.task_queue = a.task_queue;
  wcfg.identity = a.identity;
  wcfg.poll_deadline_ms = a.poll_ms;
  wcfg.call_metadata["temporal-namespace"] = a.ns;
  auto provider = [&spec](std::string_view t) -> const mwf_core::WorkflowSpec* {
    return t == spec.name ? &spec : nullptr;
  };
  mwf_core::WorkflowLoop wfLoop(transport, codec, wfAdapter, store, provider, wcfg);

  mwf_core::WorkerConfig acfg;
  acfg.ns = a.ns;
  acfg.task_queue = a.task_queue;
  acfg.identity = a.identity;
  acfg.poll_deadline_ms = a.poll_ms;
  acfg.call_metadata["temporal-namespace"] = a.ns;
  mwf_core::WorkerLoop actLoop(transport, codec, registry, actAdapter, acfg);

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  std::cout << "[sole] both loops polling queue=" << a.task_queue
            << " poll_ms=" << a.poll_ms << " (round-robin, one thread)\n" << std::flush;

  std::vector<std::string> command_stream;  // proof: SCHEDULE.. then COMPLETE..
  bool activity_ran = false;
  bool completed = false;
  int consecutive_transport_errors = 0;
  auto t_start = std::chrono::steady_clock::now();
  auto elapsed_s = [&] {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - t_start).count();
  };

  long round = 0;
  while (!g_stop && elapsed_s() < a.max_seconds && !completed) {
    ++round;
    // ── workflow tick ──
    auto t0 = std::chrono::steady_clock::now();
    mwf_core::WorkflowTickResult wr = wfLoop.runOnce(a.poll_ms);
    auto wms = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count();
    std::cout << "[sole] r" << round << " WF " << wfOutcomeName(wr.outcome)
              << (wr.detail.empty() ? "" : " (" + wr.detail + ")")
              << " elapsed_ms=" << wms << "\n" << std::flush;
    if (wr.outcome == mwf_core::WorkflowTickOutcome::Progressed &&
        wr.detail.rfind("scheduled", 0) == 0)
      command_stream.push_back("SCHEDULE_ACTIVITY_TASK");
    if (wr.outcome == mwf_core::WorkflowTickOutcome::Completed) {
      command_stream.push_back("COMPLETE_WORKFLOW_EXECUTION");
      completed = true;
      break;
    }
    if (wr.outcome == mwf_core::WorkflowTickOutcome::Failed) {
      std::cerr << "[sole] workflow FAILED: " << wr.detail << "\n";
      break;
    }
    if (wr.outcome == mwf_core::WorkflowTickOutcome::TransportError) {
      if (++consecutive_transport_errors >= 5) { std::cerr << "[sole] 5 transport errors\n"; break; }
    } else {
      consecutive_transport_errors = 0;
    }

    // ── activity tick ──
    t0 = std::chrono::steady_clock::now();
    mwf_core::TickResult ar = actLoop.runOnce(a.poll_ms);
    auto ams = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count();
    std::cout << "[sole] r" << round << " ACT " << actOutcomeName(ar.outcome)
              << (ar.activity.empty() ? "" : " activity=" + ar.activity)
              << (ar.detail.empty() ? "" : " (" + ar.detail + ")")
              << " elapsed_ms=" << ams << "\n" << std::flush;
    if (ar.outcome == mwf_core::TickOutcome::Completed) activity_ran = true;
    if (ar.outcome == mwf_core::TickOutcome::TransportError) {
      if (++consecutive_transport_errors >= 5) { std::cerr << "[sole] 5 transport errors\n"; break; }
    } else {
      consecutive_transport_errors = 0;
    }
  }

  transport.close();

  std::cout << "[sole] ===== WORKER SUMMARY =====\n";
  std::cout << "[sole] activity_ran=" << (activity_ran ? "true" : "false")
            << " workflow_completed=" << (completed ? "true" : "false") << "\n";
  std::cout << "[sole] command_stream=[";
  for (size_t i = 0; i < command_stream.size(); ++i)
    std::cout << (i ? "," : "") << command_stream[i];
  std::cout << "]\n" << std::flush;
  return completed ? 0 : 1;
}

// ── trigger mode (Python-free server-side trigger + verify) ───────────────────
int runTrigger(const std::string& bearer, Args& a) {
  mwf_transport::RestClient rest("https://api.mistral.ai", bearer);

  // Wait for the worker to be active (dispatch gate).
  std::cout << "[sole] waiting for workflow '" << a.workflow << "' active ...\n" << std::flush;
  bool active = false;
  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now() - t0).count() < a.timeout_s) {
    auto r = rest.workflowActive(a.workflow);
    if (r && r.value) { active = true; break; }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  if (!active) { std::cerr << "[sole] workflow never went active\n"; return 1; }
  std::cout << "[sole] workflow active; triggering\n" << std::flush;

  std::string input = "{\"name\":\"" + a.input_name + "\"}";
  auto ex = rest.executeWorkflow(a.workflow, input, a.task_queue);
  if (!ex) { std::cerr << "[sole] execute failed: " << ex.error << "\n"; return 1; }
  std::string execution_id = ex.value;
  std::cout << "[sole] execution_id=" << execution_id << "\n" << std::flush;

  // Poll to a terminal status.
  std::string status, raw;
  static const char* kTerminal[] = {"COMPLETED", "FAILED", "TERMINATED", "TIMED_OUT", "CANCELED"};
  auto isTerminal = [&](const std::string& s) {
    for (auto* t : kTerminal) if (s == t) return true; return false;
  };
  auto t1 = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now() - t1).count() < a.timeout_s) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto gs = rest.getExecution(execution_id);
    if (!gs) { std::cerr << "[sole] getExecution: " << gs.error << "\n"; continue; }
    status = gs.value.status;
    raw = gs.value.raw;
    std::cout << "[sole] status=" << status << "\n" << std::flush;
    if (isTerminal(status)) break;
  }

  bool ok = (status == "COMPLETED") && (raw.find(a.expect) != std::string::npos);
  std::cout << "[sole] ===== TRIGGER VERDICT =====\n";
  std::cout << "[sole] execution_id=" << execution_id << " status=" << status
            << " expect(\"" << a.expect << "\")=" << (raw.find(a.expect) != std::string::npos)
            << "\n";
  std::cout << "[sole] final=" << raw << "\n" << std::flush;

  if (!a.evidence.empty()) {
    std::ofstream f(a.evidence);
    f << "{\"execution_id\":\"" << execution_id << "\",\"status\":\"" << status
      << "\",\"expect\":\"" << a.expect << "\",\"ok\":" << (ok ? "true" : "false")
      << ",\"final\":" << raw << "}\n";
    std::cout << "[sole] evidence -> " << a.evidence << "\n" << std::flush;
  }
  return ok ? 0 : 1;
}

// ── cleanup mode ──────────────────────────────────────────────────────────────
// Best-effort — cleanup never fails the gate (the worker is already stopped and
// the run is terminal by the time we get here). Two subtleties handled:
//   * terminate only a NON-terminal run (a COMPLETED run 409s "not running").
//   * archive requires the deployment INACTIVE; without a heartbeat it lapses
//     shortly after the worker stops, so we poll active→false before archiving.
int runCleanup(const std::string& bearer, Args& a) {
  mwf_transport::RestClient rest("https://api.mistral.ai", bearer);
  static const char* kTerminal[] = {"COMPLETED", "FAILED", "TERMINATED", "TIMED_OUT", "CANCELED"};

  if (!a.exec_id.empty()) {
    auto gs = rest.getExecution(a.exec_id);
    bool terminal = false;
    if (gs) for (auto* t : kTerminal) if (gs.value.status == t) terminal = true;
    if (terminal) {
      std::cout << "[sole] terminate " << a.exec_id << ": skipped (already "
                << gs.value.status << ")\n";
    } else {
      auto t = rest.terminateExecution(a.exec_id);
      std::cout << "[sole] terminate " << a.exec_id << ": "
                << (t ? "ok" : ("FAILED: " + t.error)) << "\n";
    }
  }

  // Wait for the deployment to lapse to inactive (bounded), then archive.
  auto t0 = std::chrono::steady_clock::now();
  bool inactive = false;
  while (std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now() - t0).count() < a.timeout_s) {
    auto act = rest.workflowActive(a.workflow);
    if (act && !act.value) { inactive = true; break; }
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }
  if (!inactive) {
    std::cout << "[sole] archive " << a.workflow
              << ": deferred (still active; lapses via missed heartbeat)\n";
    return 0;
  }
  auto ar = rest.archiveWorkflow(a.workflow);
  std::cout << "[sole] archive " << a.workflow << ": "
            << (ar ? "ok" : ("deferred: " + ar.error)) << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parseArgs(argc, argv, a)) return 2;
  std::string bearer = loadKey(a.api_key_env);
  if (bearer.empty()) return 2;

  if (a.mode == "worker") return runWorker(bearer, a);
  if (a.mode == "trigger") return runTrigger(bearer, a);
  if (a.mode == "cleanup") return runCleanup(bearer, a);
  std::cerr << "[sole] unknown --mode " << a.mode << " (worker|trigger|cleanup)\n";
  return 2;
}

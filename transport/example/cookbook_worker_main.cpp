// transport example: cookbook_worker_main — the GENERIC desktop cookbook worker.
//
// A superset of sole_worker_main.cpp (which it deliberately does NOT edit): the
// SAME sole-worker mechanics — one process, one task queue, ONE thread round-
// robining BOTH the WorkflowLoop (core, replays a declarative WorkflowSpec
// through the deterministic ReplayEngine) and the WorkerLoop (runs the activity)
// over DesktopTransport + PayloadCodecV1 + the two libprotobuf proto adapters —
// but driven by a WorkflowSpec loaded FROM DISK (`--spec <file.json>`) instead of
// a hard-coded one, and backed by a FIXED cookbook activity set:
//
//   echo         — returns its JSON arg wrapped as {echo:<arg>, engine, host}.
//   ai.chat      — calls Mistral /v1/chat/completions with {prompt, system?} →
//                  {text: <reply>}.  Real AI, real network.
//   ai.classify  — like ai.chat but forces a single lowercase label (optionally
//                  constrained by a `labels` array) → {label: <one word>}, used
//                  to drive a `conditional` branch in the spec grammar.
//
// This is the engine behind cookbooks/01..03. Each cookbook ships its own
// spec.json + run_local.sh (register → trigger → poll COMPLETED → archive); this
// binary is the C++ worker they all share. NO Python / mistralai-workflows.
//
// Modes (same three as the sole worker, so a cookbook's run_local.sh reads like
// the sole-worker e2e):
//   --mode worker  : whoami → REST-register `--workflow` on `--queue` (schemas
//                    come from the spec, else permissive) → run both loops until
//                    `--max-completions` workflow(s) COMPLETE (or --max-seconds).
//                    Prints one `command_stream #k=[...]` line per completion —
//                    the determinism evidence cookbook 03 diffs.
//   --mode trigger : wait active → REST execute (`--input-json`/`--input-file`)
//                    → poll GET /executions/{id} to terminal → assert COMPLETED
//                    (+ optional `--expect` substring) → evidence JSON (carries
//                    the extracted `result` object, for reruns-are-identical).
//   --mode cleanup : archive the deployment (+ terminate a leftover run).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "desktop_transport.h"
#include "host_proto_adapter.h"       // IWorkerProtoAdapter (activity)
#include "host_workflow_adapter.h"    // IWorkflowProtoAdapter (workflow)
#include "mwf/types.h"                // mwf::json (MWF_JSON_NLOHMANN)
#include "mwf_codec/payload_codec.h"
#include "mwf_core/activity_registry.h"
#include "mwf_core/worker_loop.h"
#include "mwf_core/workflow_loop.h"
#include "mwf_core/workflow_spec.h"
#include "rest_client.h"

using mwf::Bytes;
using json = mwf::json;

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

// Permissive fallback schemas (used when the spec omits them). The frontend does
// structural validation only; cookbooks keep inputs/outputs open objects.
constexpr const char* kDefaultInputSchema =
    "{\"type\":\"object\",\"additionalProperties\":true,\"title\":\"cookbook_Input\"}";
constexpr const char* kDefaultOutputSchema =
    "{\"type\":\"object\",\"additionalProperties\":true,\"title\":\"cookbook_Output\"}";

// ── Mistral chat helper (libcurl; the RestClient HTTPS pattern) ───────────────
// Reads nothing global — the API key is passed in. Returns the assistant text
// (choices[0].message.content). Retries once on a transport/5xx blip.
std::size_t curlWrite(char* ptr, std::size_t size, std::size_t nmemb, void* ud) {
  static_cast<std::string*>(ud)->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string mistralChat(const std::string& apiKey, const std::string& model,
                        const std::string& system, const std::string& user,
                        std::string& err) {
  // Build the request body with the JSON lib (never hand-splice user text).
  json body;
  body["model"] = model;
  body["temperature"] = 0;  // as deterministic as the provider allows
  json messages = json::array();
  if (!system.empty()) messages.push_back({{"role", "system"}, {"content", system}});
  messages.push_back({{"role", "user"}, {"content", user}});
  body["messages"] = std::move(messages);
  const std::string payload = body.dump();

  for (int attempt = 0; attempt < 2; ++attempt) {
    CURL* c = curl_easy_init();
    if (!c) { err = "curl_easy_init failed"; return {}; }
    std::string resp;
    struct curl_slist* headers = nullptr;
    const std::string auth = "Authorization: Bearer " + apiKey;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, "https://api.mistral.ai/v1/chat/completions");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "embedded-temporal-cookbook/0.1");
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) { err = std::string("curl: ") + curl_easy_strerror(rc); continue; }
    if (status < 200 || status >= 300) {
      err = "chat HTTP " + std::to_string(status) + ": " + resp.substr(0, 300);
      if (status >= 500) continue;  // transient — retry
      return {};
    }
    try {
      json j = json::parse(resp);
      std::string content = j.at("choices").at(0).at("message").at("content").get<std::string>();
      err.clear();
      return content;
    } catch (const std::exception& e) {
      err = std::string("parse chat response: ") + e.what() + " body=" + resp.substr(0, 300);
      return {};
    }
  }
  return {};
}

// Parse an activity arg (json-as-bytes) tolerantly: valid JSON → that value; a
// bare non-JSON blob → a JSON string of it; empty → null.
json parseArg(const Bytes& arg) {
  std::string s = toString(arg);
  if (s.empty()) return json(nullptr);
  try { return json::parse(s); } catch (...) { return json(s); }
}

// Pull the free-text content out of an activity arg that may be a bare string or
// an object carrying `prompt`/`text`.
std::string argText(const json& a) {
  if (a.is_string()) return a.get<std::string>();
  if (a.is_object()) {
    if (a.contains("prompt") && a["prompt"].is_string()) return a["prompt"].get<std::string>();
    if (a.contains("text") && a["text"].is_string()) return a["text"].get<std::string>();
  }
  return {};
}

std::string argSystem(const json& a) {
  if (a.is_object() && a.contains("system") && a["system"].is_string())
    return a["system"].get<std::string>();
  return {};
}

// Reduce a model reply to a single lowercase label token (leading [a-z] run).
std::string toLabel(const std::string& raw) {
  std::string out;
  for (char ch : raw) {
    char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lc >= 'a' && lc <= 'z') out += lc;
    else if (!out.empty()) break;  // stop at first break after we've started
  }
  return out;
}

// ── the fixed cookbook activity registry ──────────────────────────────────────
void registerCookbookActivities(mwf_core::ActivityRegistry& registry,
                                const std::string& apiKey, const std::string& model) {
  // echo — deterministic, no network. Wrap the arg with the engine/host brand.
  registry.registerActivity("echo", [](const Bytes& arg) {
    json out;
    out["echo"] = parseArg(arg);
    out["engine"] = "mwf-cpp";
    out["host"] = "desktop";
    return mwf::Result<Bytes>::success(toBytes(out.dump()));
  });

  // ai.chat — real Mistral chat. {prompt, system?} (or a bare string) → {text}.
  registry.registerActivity("ai.chat", [apiKey, model](const Bytes& arg) {
    json a = parseArg(arg);
    std::string prompt = argText(a);
    if (prompt.empty())
      return mwf::Result<Bytes>::failure("ai.chat: no prompt/text in arg");
    std::string err;
    std::string reply = mistralChat(apiKey, model, argSystem(a), prompt, err);
    if (!err.empty()) return mwf::Result<Bytes>::failure("ai.chat: " + err);
    json out;
    out["text"] = reply;
    return mwf::Result<Bytes>::success(toBytes(out.dump()));
  });

  // ai.classify — forces a single lowercase label. Optional `labels` array
  // constrains the choice; the reply is reduced to one token → {label}.
  registry.registerActivity("ai.classify", [apiKey, model](const Bytes& arg) {
    json a = parseArg(arg);
    std::string text = argText(a);
    if (text.empty())
      return mwf::Result<Bytes>::failure("ai.classify: no text/prompt in arg");
    std::string constraint;
    if (a.is_object() && a.contains("labels") && a["labels"].is_array() &&
        !a["labels"].empty()) {
      std::ostringstream os;
      os << " chosen strictly from this set: ";
      for (std::size_t i = 0; i < a["labels"].size(); ++i) {
        if (i) os << ", ";
        os << (a["labels"][i].is_string() ? a["labels"][i].get<std::string>()
                                          : a["labels"][i].dump());
      }
      constraint = os.str();
    }
    std::string system =
        "You are a strict text classifier. Read the user's text and respond with "
        "EXACTLY ONE lowercase word — the single best label" + constraint +
        ". Output only that one word: no punctuation, no quotes, no explanation.";
    // A caller-supplied system prompt (rare) is appended for extra guidance.
    std::string extra = argSystem(a);
    if (!extra.empty()) system += " " + extra;

    std::string err;
    std::string reply = mistralChat(apiKey, model, system, text, err);
    if (!err.empty()) return mwf::Result<Bytes>::failure("ai.classify: " + err);
    std::string label = toLabel(reply);
    if (label.empty()) label = "unknown";
    json out;
    out["label"] = label;
    return mwf::Result<Bytes>::success(toBytes(out.dump()));
  });
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
  std::string model = "mistral-small-latest";
  std::string target;                    // override; else whoami
  std::string ns;                        // override; else whoami
  std::string spec_path;                 // worker: WorkflowSpec JSON on disk
  std::string workflow = "mwf_cookbook";
  std::string task_queue = "mwf-cookbook";
  std::string identity = "mwf-cookbook@desktop";
  std::string worker_name = "mwf-cookbook-worker";
  std::string input_json = "{}";         // trigger: raw JSON value for "input"
  std::string input_file;                // trigger: read input JSON from a file
  std::string expect;                    // trigger: optional result-substring assert
  std::string evidence;                  // trigger evidence JSON path
  std::string exec_id;                   // cleanup: terminate this run
  std::string signal_name = "approve";   // hitl/signal: the wait_signal name
  std::string signal_input = "{\"approved\":true,\"by\":\"desktop\"}";  // hitl/signal payload
  int pause_observe_s = 8;               // hitl: confirm the pause this long before signaling
  int max_completions = 1;               // worker: stop after N completed runs
  int tls = -1;                          // unset → whoami's tls (else on); --tls 0 opts out
  int poll_ms = 4000;
  int max_seconds = 180;                 // worker overall budget
  int timeout_s = 180;                   // trigger wait-for-terminal
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
    else if (k == "--model" && next(v)) a.model = v;
    else if (k == "--target" && next(v)) a.target = v;
    else if (k == "--namespace" && next(v)) a.ns = v;
    else if (k == "--spec" && next(v)) a.spec_path = v;
    else if (k == "--workflow" && next(v)) a.workflow = v;
    else if ((k == "--queue" || k == "--task-queue") && next(v)) a.task_queue = v;
    else if (k == "--identity" && next(v)) a.identity = v;
    else if (k == "--worker-name" && next(v)) a.worker_name = v;
    else if (k == "--input-json" && next(v)) a.input_json = v;
    else if (k == "--input-file" && next(v)) a.input_file = v;
    else if (k == "--expect" && next(v)) a.expect = v;
    else if (k == "--evidence" && next(v)) a.evidence = v;
    else if (k == "--exec-id" && next(v)) a.exec_id = v;
    else if (k == "--signal-name" && next(v)) a.signal_name = v;
    else if (k == "--signal-input" && next(v)) a.signal_input = v;
    else if (k == "--pause-observe-s" && next(v)) a.pause_observe_s = std::atoi(v.c_str());
    else if (k == "--max-completions" && next(v)) a.max_completions = std::atoi(v.c_str());
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
    std::cerr << "[cook] env var " << env_name << " is empty/unset\n";
    return {};
  }
  return key;
}

std::optional<std::string> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::nullopt;
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool resolveEndpoint(const std::string& bearer, Args& a) {
  if (!a.target.empty() && !a.ns.empty()) return true;
  mwf_transport::RestClient rest("https://api.mistral.ai", bearer);
  auto who = rest.whoami();
  if (!who) { std::cerr << "[cook] whoami failed: " << who.error << "\n"; return false; }
  if (a.target.empty()) a.target = who.value.scheduler_url;
  if (a.ns.empty()) a.ns = who.value.namespace_;
  if (a.tls < 0) a.tls = who.value.tls ? 1 : 0;
  std::cout << "[cook] whoami: scheduler=" << a.target << " namespace=" << a.ns
            << " tls=" << who.value.tls << "\n" << std::flush;
  return true;
}

// Collect the distinct wait_signal signal names a spec declares (recursing into
// sequences + conditional branches), in first-seen order.
void collectSignals(const std::vector<mwf_core::WorkflowStep>& steps,
                    std::vector<std::string>& out) {
  for (const auto& s : steps) {
    switch (s.kind) {
      case mwf_core::WorkflowStep::Kind::WaitSignal:
        if (std::find(out.begin(), out.end(), s.signal) == out.end()) out.push_back(s.signal);
        break;
      case mwf_core::WorkflowStep::Kind::Sequence:
        collectSignals(s.steps, out);
        break;
      case mwf_core::WorkflowStep::Kind::Conditional:
        collectSignals(s.true_steps, out);
        collectSignals(s.false_steps, out);
        break;
      default:
        break;
    }
  }
}

// Build the SignalDefinition[] JSON the frontend requires for a wait_signal
// workflow — each {"name","input_schema"} with a permissive open-object schema.
// "[]" when the spec waits on nothing (cookbooks 01..03).
std::string buildSignalsJson(const mwf_core::WorkflowSpec& spec) {
  std::vector<std::string> names;
  collectSignals(spec.steps, names);
  std::string out = "[";
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) out += ",";
    out += "{\"name\":\"" + names[i] +
           "\",\"input_schema\":{\"type\":\"object\",\"additionalProperties\":true}}";
  }
  out += "]";
  return out;
}

// ── worker mode ───────────────────────────────────────────────────────────────
int runWorker(const std::string& bearer, Args& a) {
  if (a.spec_path.empty()) { std::cerr << "[cook] worker needs --spec <file.json>\n"; return 2; }
  auto specText = readFile(a.spec_path);
  if (!specText) { std::cerr << "[cook] cannot read spec: " << a.spec_path << "\n"; return 2; }

  auto specParsed = mwf_core::parseWorkflowSpec(*specText);
  if (!specParsed) { std::cerr << "[cook] spec parse: " << specParsed.error << "\n"; return 1; }
  mwf_core::WorkflowSpec spec = std::move(specParsed.value);
  // The loop looks up the spec by the REGISTERED workflow type name, so pin the
  // spec's name to --workflow (the spec file's "name" is a placeholder).
  spec.name = a.workflow;

  const std::string inSchema =
      spec.input_schema_json.empty() ? kDefaultInputSchema : spec.input_schema_json;
  const std::string outSchema =
      spec.output_schema_json.empty() ? kDefaultOutputSchema : spec.output_schema_json;

  if (!resolveEndpoint(bearer, a)) return 1;

  // 1. SELF-REGISTER the workflow via REST. Declare any wait_signal signals — the
  //    frontend 422s a signal whose name isn't registered on the workflow.
  const std::string signalsJson = buildSignalsJson(spec);
  mwf_transport::RestClient rest("https://api.mistral.ai", bearer);
  std::cout << "[cook] registering workflow '" << a.workflow << "' on queue '"
            << a.task_queue << "' signals=" << signalsJson << " ...\n" << std::flush;
  auto reg = rest.registerWorkflow(a.workflow, a.task_queue, inSchema, outSchema,
                                   a.worker_name, signalsJson);
  if (!reg) { std::cerr << "[cook] register failed: " << reg.error << "\n"; return 1; }
  std::cout << "[cook] register OK: " << reg.value.substr(0, 200) << "\n" << std::flush;

  // 2. Fixed cookbook activity registry.
  mwf_core::ActivityRegistry registry;
  registerCookbookActivities(registry, bearer, a.model);

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

  std::cout << "[cook] both loops polling queue=" << a.task_queue
            << " poll_ms=" << a.poll_ms << " max_completions=" << a.max_completions
            << " (round-robin, one thread)\n" << std::flush;

  std::vector<std::string> command_stream;  // per-execution; reset on each COMPLETE
  int completions = 0;
  bool activity_ran = false;
  int consecutive_transport_errors = 0;
  auto t_start = std::chrono::steady_clock::now();
  auto elapsed_s = [&] {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - t_start).count();
  };
  auto printStream = [&](int k) {
    std::cout << "[cook] command_stream #" << k << "=[";
    for (size_t i = 0; i < command_stream.size(); ++i)
      std::cout << (i ? "," : "") << command_stream[i];
    std::cout << "]\n" << std::flush;
  };

  long round = 0;
  while (!g_stop && elapsed_s() < a.max_seconds && completions < a.max_completions) {
    ++round;
    // ── workflow tick ──
    mwf_core::WorkflowTickResult wr = wfLoop.runOnce(a.poll_ms);
    std::cout << "[cook] r" << round << " WF " << wfOutcomeName(wr.outcome)
              << (wr.detail.empty() ? "" : " (" + wr.detail + ")") << "\n" << std::flush;
    if (wr.outcome == mwf_core::WorkflowTickOutcome::Progressed &&
        wr.detail.rfind("scheduled", 0) == 0)
      command_stream.push_back("SCHEDULE_ACTIVITY_TASK");
    if (wr.outcome == mwf_core::WorkflowTickOutcome::Completed) {
      command_stream.push_back("COMPLETE_WORKFLOW_EXECUTION");
      ++completions;
      printStream(completions);
      command_stream.clear();
      consecutive_transport_errors = 0;
      continue;
    }
    if (wr.outcome == mwf_core::WorkflowTickOutcome::Failed) {
      std::cerr << "[cook] workflow FAILED: " << wr.detail << "\n";
      break;
    }
    if (wr.outcome == mwf_core::WorkflowTickOutcome::TransportError) {
      if (++consecutive_transport_errors >= 5) { std::cerr << "[cook] 5 transport errors\n"; break; }
    } else {
      consecutive_transport_errors = 0;
    }

    // ── activity tick ──
    mwf_core::TickResult ar = actLoop.runOnce(a.poll_ms);
    std::cout << "[cook] r" << round << " ACT " << actOutcomeName(ar.outcome)
              << (ar.activity.empty() ? "" : " activity=" + ar.activity)
              << (ar.detail.empty() ? "" : " (" + ar.detail + ")") << "\n" << std::flush;
    if (ar.outcome == mwf_core::TickOutcome::Completed) activity_ran = true;
    if (ar.outcome == mwf_core::TickOutcome::Failed)
      std::cerr << "[cook] activity FAILED: " << ar.detail << "\n";
    if (ar.outcome == mwf_core::TickOutcome::TransportError) {
      if (++consecutive_transport_errors >= 5) { std::cerr << "[cook] 5 transport errors\n"; break; }
    } else {
      consecutive_transport_errors = 0;
    }
  }

  transport.close();

  std::cout << "[cook] ===== WORKER SUMMARY =====\n";
  std::cout << "[cook] activity_ran=" << (activity_ran ? "true" : "false")
            << " completions=" << completions << "/" << a.max_completions << "\n" << std::flush;

  const int rc = completions >= a.max_completions ? 0 : 1;
  // The run is complete and every byte of output is flushed. Exit via _Exit to
  // skip C++ static + pthread TSD teardown: this process links gRPC AND
  // libcurl→OpenSSL, and OpenSSL's per-thread destructor SIGSEGVs on the gRPC
  // worker threads at exit (init_thread_destructor → OPENSSL_sk_num) once
  // activities have opened HTTPS connections (cookbooks 02/03). The workflow
  // work is already done and server-observed; a fast, clean exit here is correct
  // and sidesteps that benign third-party teardown crash. (Worker mode writes no
  // files; trigger/cleanup return normally and flush their evidence first.)
  std::cout.flush();
  std::fflush(nullptr);
  std::_Exit(rc);
}

// ── trigger mode ──────────────────────────────────────────────────────────────
int runTrigger(const std::string& bearer, Args& a) {
  mwf_transport::RestClient rest("https://api.mistral.ai", bearer);

  std::string input = a.input_json;
  if (!a.input_file.empty()) {
    auto f = readFile(a.input_file);
    if (!f) { std::cerr << "[cook] cannot read --input-file " << a.input_file << "\n"; return 2; }
    input = *f;
  }
  // Validate the input is JSON before we spend a trigger on it.
  try { (void)json::parse(input); }
  catch (const std::exception& e) { std::cerr << "[cook] input not JSON: " << e.what() << "\n"; return 2; }

  std::cout << "[cook] waiting for workflow '" << a.workflow << "' active ...\n" << std::flush;
  bool active = false;
  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now() - t0).count() < a.timeout_s) {
    auto r = rest.workflowActive(a.workflow);
    if (r && r.value) { active = true; break; }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  if (!active) { std::cerr << "[cook] workflow never went active\n"; return 1; }
  std::cout << "[cook] workflow active; triggering\n" << std::flush;

  auto ex = rest.executeWorkflow(a.workflow, input, a.task_queue);
  if (!ex) { std::cerr << "[cook] execute failed: " << ex.error << "\n"; return 1; }
  std::string execution_id = ex.value;
  std::cout << "[cook] execution_id=" << execution_id << "\n" << std::flush;

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
    if (!gs) { std::cerr << "[cook] getExecution: " << gs.error << "\n"; continue; }
    status = gs.value.status;
    raw = gs.value.raw;
    std::cout << "[cook] status=" << status << "\n" << std::flush;
    if (isTerminal(status)) break;
  }

  // Extract the result object out of the execution response (see evidence shape:
  // the top-level {..., "result": {...}} of GET /executions/{id}).
  std::string result_dump = "null";
  bool have_result = false;
  try {
    json j = json::parse(raw);
    if (j.contains("result")) { result_dump = j["result"].dump(); have_result = true; }
  } catch (...) { /* leave result null */ }

  bool expect_ok = a.expect.empty() || (raw.find(a.expect) != std::string::npos);
  bool ok = (status == "COMPLETED") && expect_ok;
  std::cout << "[cook] ===== TRIGGER VERDICT =====\n";
  std::cout << "[cook] execution_id=" << execution_id << " status=" << status
            << " expect(\"" << a.expect << "\")=" << (expect_ok ? "true" : "false")
            << " result=" << result_dump << "\n" << std::flush;

  if (!a.evidence.empty()) {
    std::ofstream f(a.evidence);
    f << "{\"execution_id\":\"" << execution_id << "\",\"status\":\"" << status
      << "\",\"expect\":\"" << a.expect << "\",\"ok\":" << (ok ? "true" : "false")
      << ",\"have_result\":" << (have_result ? "true" : "false")
      << ",\"result\":" << result_dump << ",\"final\":" << (raw.empty() ? "null" : raw) << "}\n";
    std::cout << "[cook] evidence -> " << a.evidence << "\n" << std::flush;
  }
  return ok ? 0 : 1;
}

// ── hitl mode ───────────────────────────────────────────────────────────────
// The CLIENT side of the durable human-in-the-loop demo (cookbook 04): trigger
// the workflow, PROVE it pauses (RUNNING with no completion — the worker is
// blocked on wait_signal), then deliver the signal over REST and PROVE it then
// COMPLETES. The worker (a separate --mode worker process, or the device) drives
// the replay; this process is the "human pressing the button".
int runHitl(const std::string& bearer, Args& a) {
  mwf_transport::RestClient rest("https://api.mistral.ai", bearer);

  std::string input = a.input_json;
  if (!a.input_file.empty()) {
    auto f = readFile(a.input_file);
    if (!f) { std::cerr << "[cook] cannot read --input-file " << a.input_file << "\n"; return 2; }
    input = *f;
  }
  try { (void)json::parse(input); }
  catch (const std::exception& e) { std::cerr << "[cook] input not JSON: " << e.what() << "\n"; return 2; }
  try { (void)json::parse(a.signal_input); }
  catch (const std::exception& e) { std::cerr << "[cook] --signal-input not JSON: " << e.what() << "\n"; return 2; }

  static const char* kTerminal[] = {"COMPLETED", "FAILED", "TERMINATED", "TIMED_OUT", "CANCELED"};
  auto isTerminal = [&](const std::string& s) {
    for (auto* t : kTerminal) if (s == t) return true; return false;
  };

  // 1. wait for the worker to make the workflow dispatchable, then trigger.
  std::cout << "[cook] waiting for workflow '" << a.workflow << "' active ...\n" << std::flush;
  bool active = false;
  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now() - t0).count() < a.timeout_s) {
    auto r = rest.workflowActive(a.workflow);
    if (r && r.value) { active = true; break; }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  if (!active) { std::cerr << "[cook] workflow never went active\n"; return 1; }

  auto ex = rest.executeWorkflow(a.workflow, input, a.task_queue);
  if (!ex) { std::cerr << "[cook] execute failed: " << ex.error << "\n"; return 1; }
  const std::string execution_id = ex.value;
  std::cout << "[cook] execution_id=" << execution_id << "\n" << std::flush;

  // 2. observe the PAUSE: for pause_observe_s the run must stay RUNNING (the
  //    worker replays device.echo → then BLOCKS on wait_signal). A premature
  //    terminal here means the pause never happened — fail loudly.
  std::cout << "[cook] observing pause for " << a.pause_observe_s
            << "s (expect RUNNING, no completion) ...\n" << std::flush;
  std::string status = "UNKNOWN", raw;
  auto tp = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now() - tp).count() < a.pause_observe_s) {
    auto gs = rest.getExecution(execution_id);
    if (gs) { status = gs.value.status; raw = gs.value.raw; }
    std::cout << "[cook]   paused? status=" << status << "\n" << std::flush;
    if (isTerminal(status)) {
      std::cerr << "[cook] FAIL: workflow reached " << status
                << " BEFORE the signal — it never paused on wait_signal\n";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  std::cout << "[cook] PAUSED confirmed: status=" << status
            << " (blocked on wait_signal '" << a.signal_name << "')\n" << std::flush;

  // 3. deliver the signal (the "button press").
  auto sig = rest.signalExecution(execution_id, a.signal_name, a.signal_input);
  if (!sig) { std::cerr << "[cook] signal failed: " << sig.error << "\n"; return 1; }
  std::cout << "[cook] signal sent: name=" << a.signal_name << " input=" << a.signal_input
            << " -> " << sig.value.substr(0, 120) << "\n" << std::flush;

  // 4. observe COMPLETION (the worker's next poll unblocks + completes).
  auto t1 = std::chrono::steady_clock::now();
  while (std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now() - t1).count() < a.timeout_s) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto gs = rest.getExecution(execution_id);
    if (!gs) { std::cerr << "[cook] getExecution: " << gs.error << "\n"; continue; }
    status = gs.value.status; raw = gs.value.raw;
    std::cout << "[cook] post-signal status=" << status << "\n" << std::flush;
    if (isTerminal(status)) break;
  }

  std::string result_dump = "null";
  bool have_result = false;
  try {
    json j = json::parse(raw);
    if (j.contains("result")) { result_dump = j["result"].dump(); have_result = true; }
  } catch (...) { /* leave result null */ }

  bool expect_ok = a.expect.empty() || (raw.find(a.expect) != std::string::npos);
  bool ok = (status == "COMPLETED") && expect_ok;
  std::cout << "[cook] ===== HITL VERDICT =====\n";
  std::cout << "[cook] execution_id=" << execution_id << " status=" << status
            << " expect(\"" << a.expect << "\")=" << (expect_ok ? "true" : "false")
            << " result=" << result_dump << "\n" << std::flush;

  if (!a.evidence.empty()) {
    std::ofstream f(a.evidence);
    f << "{\"execution_id\":\"" << execution_id << "\",\"status\":\"" << status
      << "\",\"signal_name\":\"" << a.signal_name << "\",\"paused_confirmed\":true"
      << ",\"expect\":\"" << a.expect << "\",\"ok\":" << (ok ? "true" : "false")
      << ",\"have_result\":" << (have_result ? "true" : "false")
      << ",\"result\":" << result_dump << ",\"final\":" << (raw.empty() ? "null" : raw) << "}\n";
    std::cout << "[cook] evidence -> " << a.evidence << "\n" << std::flush;
  }
  return ok ? 0 : 1;
}

// ── cleanup mode ──────────────────────────────────────────────────────────────
int runCleanup(const std::string& bearer, Args& a) {
  mwf_transport::RestClient rest("https://api.mistral.ai", bearer);
  static const char* kTerminal[] = {"COMPLETED", "FAILED", "TERMINATED", "TIMED_OUT", "CANCELED"};

  if (!a.exec_id.empty()) {
    auto gs = rest.getExecution(a.exec_id);
    bool terminal = false;
    if (gs) for (auto* t : kTerminal) if (gs.value.status == t) terminal = true;
    if (terminal) {
      std::cout << "[cook] terminate " << a.exec_id << ": skipped (already "
                << gs.value.status << ")\n";
    } else {
      auto t = rest.terminateExecution(a.exec_id);
      std::cout << "[cook] terminate " << a.exec_id << ": "
                << (t ? "ok" : ("FAILED: " + t.error)) << "\n";
    }
  }

  auto t0 = std::chrono::steady_clock::now();
  bool inactive = false;
  while (std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now() - t0).count() < a.timeout_s) {
    auto act = rest.workflowActive(a.workflow);
    if (act && !act.value) { inactive = true; break; }
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }
  if (!inactive) {
    std::cout << "[cook] archive " << a.workflow
              << ": deferred (still active; lapses via missed heartbeat)\n";
    return 0;
  }
  auto ar = rest.archiveWorkflow(a.workflow);
  std::cout << "[cook] archive " << a.workflow << ": "
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
  if (a.mode == "hitl") return runHitl(bearer, a);
  if (a.mode == "cleanup") return runCleanup(bearer, a);
  std::cerr << "[cook] unknown --mode " << a.mode << " (worker|trigger|hitl|cleanup)\n";
  return 2;
}

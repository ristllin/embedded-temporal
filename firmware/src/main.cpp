// firmware: ESP32-S3 worker app entry point.
//
// The device as a Mistral Workflows ACTIVITY WORKER, end to end:
//   WiFi up → whoami (minimal HTTPS GET to the Mistral control plane) →
//   EspTransport (gRPC-over-HTTP/2: vendored nghttp2 + mbedTLS/PSRAM, ALPN h2)
//   + NanopbWorkerAdapter (proto device subset) + PayloadCodecV1 (codec)
//   + ActivityRegistry with one `device.echo` activity
//   → WorkerLoop::runOnce driven from loop().
//
// Long-poll timing: the Mistral frontend holds PollActivityTaskQueue ~45 s then
// answers OK-taskless — the loop's Idle path. Client deadline 70 s; the
// transport feeds the task watchdog inside its recv window, so a 70 s blocking
// poll under the 8 s WDT is safe.
//
// Config (NVS namespace "mwf" — provisioning fills these; empty key ⇒
// the worker idles and says so): apiKey, staSsid/staPass (blank = stored WiFi
// creds), taskQueue (default "mwf-esp").
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_task_wdt.h>

#include <memory>

#include "durable_store.h"
#include "esp_transport.h"          // transport esp/
#include "mwf_codec/payload_codec.h"
#include "mwf_core/activity_registry.h"
#include "mwf_core/client_ops.h"        // ClientOps::signalWorkflow (cookbook 4)
#include "mwf_core/worker_loop.h"
#include "mwf_core/workflow_loop.h"     // WORKFLOW poll loop (self-hosting twin)
#include "mwf_core/workflow_spec.h"     // declarative WorkflowSpec grammar
#include "nanopb_worker_adapter.h"      // proto gen/ (activity subset)
#include "nanopb_workflow_adapter.h"    // proto gen/ (workflow subset)
#include "psram_alloc.h"
#include "psram_tls.h"
#include "sole_worker_policy.h"         // round-robin + heartbeat-due + spec-match
#include "tls_arbiter.h"

static const char* kTag = "mwf.main";

// Build id from tools/git_version.py (git describe); falls back when git is absent.
#ifndef MWF_FW_BUILD
#define MWF_FW_BUILD "dev"
#endif

// Provisioning secrets (gitignored, generated at flash time — never committed).
#if __has_include("mwf_prov_secrets.h")
#include "mwf_prov_secrets.h"
#endif

static mwf_device::DurableStore g_store;

// ── cookbook selection (build-time) ───────────────────────────────────────────
// -DMWF_COOKBOOK=4 builds the durable human-in-the-loop demo; the DEFAULT (unset,
// or 1) is the original sole worker (echo → complete). Nothing about the default
// build changes.
#ifndef MWF_COOKBOOK
#define MWF_COOKBOOK 1
#endif

// The device runs the WORKFLOW itself (WorkflowLoop) AND its activities
// (WorkerLoop). The spec is DATA; `name` matches the registered workflow type so
// the SpecProvider resolves it by workflow_type. Per cookbook we vary the spec,
// the input schema, and the declared signals (the frontend 422s a signal whose
// name is not declared here).
#if MWF_COOKBOOK == 4
// ── cookbook 04: device.echo → wait_signal("approve") → device.echo → complete ─
// The workflow PAUSES on the signal; the device delivers it on a BOOT-button
// press (ClientOps::signalWorkflow), then its own next poll unblocks + completes.
static const char kWorkflowName[] = "mwf_device_hitl";
static const char kWorkflowSpecJson[] = R"JSON({
  "name": "mwf_device_hitl",
  "steps": [
    {"type":"activity","name":"device.echo","id":"request","args_from":"/input"},
    {"type":"wait_signal","signal":"approve","id":"gate"},
    {"type":"activity","name":"device.echo","id":"after","args_from":"/results/gate"},
    {"type":"complete","result_from":"/results/after"}
  ]
})JSON";
static const char kWorkflowInputSchema[] =
    "{\"type\":\"object\",\"additionalProperties\":true,\"title\":\"hitl_Input\"}";
static const char kWorkflowSignals[] =
    "[{\"name\":\"approve\",\"input_schema\":"
    "{\"type\":\"object\",\"additionalProperties\":true}}]";
#else
// ── cookbook 01 (default): device.echo → complete ─────────────────────────────
static const char kWorkflowName[] = "mwf_device_sole";
static const char kWorkflowSpecJson[] = R"JSON({
  "name": "mwf_device_sole",
  "steps": [
    {"type":"activity","name":"device.echo","id":"echo","args_from":"/input"},
    {"type":"complete","result_from":"/results/echo"}
  ]
})JSON";
static const char kWorkflowInputSchema[] =
    "{\"type\":\"object\",\"additionalProperties\":false,"
    "\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"],"
    "\"title\":\"run_Input\"}";
static const char kWorkflowSignals[] = "[]";
#endif
static mwf_core::WorkflowSpec g_spec;
static bool g_specOk = false;

// ── worker assembly (constructed once whoami succeeds) ────────────────────────
// ONE EspTransport serves BOTH loops: loop() drives them round-robin on the
// single main task, so they never call the transport concurrently — one h2
// connection (one TLS arbiter slot) carries PollActivity + PollWorkflow +
// Respond* multiplexed by gRPC method path. The 2nd arbiter slot stays free for
// whoami / register / heartbeat.
struct WorkerParts {
  std::unique_ptr<mwf_transport::EspTransport> transport;
  mwf_codec::PayloadCodecV1 codec;
  mwf_proto::NanopbWorkerAdapter workerAdapter;
  mwf_proto::NanopbWorkflowAdapter workflowAdapter;
  mwf_core::ActivityRegistry registry;
  std::unique_ptr<mwf_core::WorkerLoop> loop;        // runs device.echo activity
  std::unique_ptr<mwf_core::WorkflowLoop> wfLoop;    // runs the workflow itself
#if MWF_COOKBOOK == 4
  std::unique_ptr<mwf_core::ClientOps> client;       // signal SENDER (button press)
#endif
};
static WorkerParts* g_worker = nullptr;

#if MWF_COOKBOOK == 4
// ── cookbook 04 human-in-the-loop signal state ────────────────────────────────
// GPIO0 is the onboard BOOT button (active-low; pressed = LOW). On a debounced
// press we deliver the "approve" signal to whichever execution the WorkflowLoop
// last reported as Waiting. g_wfNs is the whoami namespace (SignalRequest field).
static constexpr int kBootButtonPin = 0;
static std::string g_wfNs;               // namespace to target when signaling
static std::string g_waitingWorkflowId;  // execution currently blocked on the signal
static std::string g_signaledWorkflowId; // last execution we already signaled (no double-fire)
#endif

struct Config {
  String apiKey;
  String taskQueue;
} g_cfg;

// Cadence: Mistral rejects poll deadlines <2 s and releases a taskless poll at
// ~the deadline, so a 10 s poll on each loop keeps a full workflow
// (WFT1 → activity → WFT2) turning over in ~30-40 s of round-robin. Heartbeat
// (re-register) sustains worker-active; dispatch itself needs no heartbeat.
static constexpr int kPollDeadlineMs = 10000;
static constexpr uint32_t kHeartbeatMs = 10000;

// ── whoami: minimal HTTPS GET (single-write pattern) ──────────────────────────
// TODO(transport follow-up): share transport RestClient's parsing — this is
// the documented "hardcode-with-TODO" skeleton (flat JSON, two fields).
static bool jsonField(const String& body, const char* key, String& out) {
  String pat = String("\"") + key + "\"";
  int k = body.indexOf(pat);
  if (k < 0) return false;
  int colon = body.indexOf(':', k + pat.length());
  if (colon < 0) return false;
  int q1 = body.indexOf('"', colon + 1);
  if (q1 < 0) return false;
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return false;
  out = body.substring(q1 + 1, q2);
  return true;
}

static bool whoami(const String& apiKey, String& scheduler, String& ns) {
  if (!mwf::arbiter::acquireWork(10000)) return false;
  WiFiClientSecure client;
  client.setInsecure();  // fleet TLS posture (CA bundle = documented follow-up)
  bool ok = false;
  if (client.connect("api.mistral.ai", 443, 15000)) {
    String req =
        "GET /v1/workflows/workers/whoami HTTP/1.1\r\n"
        "Host: api.mistral.ai\r\n"
        "Authorization: Bearer " + apiKey + "\r\n"
        "Connection: close\r\n\r\n";
    client.print(req);  // ONE write — headers assembled in RAM first
    // Read the full response (Connection: close ⇒ until EOF), bounded.
    String resp;
    uint32_t t0 = millis();
    while ((client.connected() || client.available()) && millis() - t0 < 15000 &&
           resp.length() < 8192) {
      while (client.available()) resp += static_cast<char>(client.read());
      esp_task_wdt_reset();
      delay(10);
    }
    int bodyAt = resp.indexOf("\r\n\r\n");
    String body = bodyAt >= 0 ? resp.substring(bodyAt + 4) : resp;
    bool http200 = resp.startsWith("HTTP/1.1 200") || resp.startsWith("HTTP/1.0 200");
    if (http200 && jsonField(body, "scheduler_url", scheduler) &&
        jsonField(body, "namespace", ns)) {
      ok = true;
    } else {
      ESP_LOGE(kTag, "whoami parse failed (http200=%d, %u B body)", (int)http200,
               (unsigned)body.length());
    }
  } else {
    ESP_LOGE(kTag, "whoami: TLS connect failed");
  }
  client.stop();
  mwf::arbiter::releaseWork();
  return ok;
}

// ── register: self-register the workflow so the frontend dispatches to us ──────
// POST /v1/workflows/register (body shape lifted from a captured live register /
// conformance/probe/sole_worker_probe.py). deployment_name == task_queue.
// Reused as the HEARTBEAT: the Mistral SDK's own fallback when the heartbeat
// endpoint 404s is to re-POST /register every 10 s, and it is idempotent — so a
// periodic re-register is the device's heartbeat (no response parsing needed).
// Returns true on HTTP 2xx.
static bool registerWorkflow(const String& apiKey, const String& queue) {
  if (!mwf::arbiter::acquireWork(10000)) return false;
  WiFiClientSecure client;
  client.setInsecure();  // fleet TLS posture (CA bundle = documented follow-up)
  bool ok = false;
  if (client.connect("api.mistral.ai", 443, 15000)) {
    // Permissive output_schema: the raw sole-worker result is device.echo's
    // object, not a {"result": string}; keep the frontend from output-validating.
    String body =
        String("{\"definitions\":[{\"name\":\"") + kWorkflowName +
        "\",\"task_queue\":\"" + queue +
        "\",\"input_schema\":" + kWorkflowInputSchema +
        ",\"output_schema\":{\"type\":\"object\",\"additionalProperties\":true,"
        "\"title\":\"run_Output\"},"
        "\"signals\":" + kWorkflowSignals + ",\"queries\":[],\"updates\":[],"
        "\"enforce_determinism\":true,\"on_behalf_of\":false,"
        "\"execution_timeout\":\"PT1H\",\"schedules\":[]}],"
        "\"deployment_name\":\"" + queue +
        "\",\"worker_name\":\"mwf-esp32s3-sole\","
        "\"deployment_location\":{\"location_type\":\"local\"}}";
    String req =
        String("POST /v1/workflows/register HTTP/1.1\r\n") +
        "Host: api.mistral.ai\r\n"
        "Authorization: Bearer " + apiKey + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + String(body.length()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
    client.print(req);  // ONE write — request assembled in RAM first
    String resp;
    uint32_t t0 = millis();
    while ((client.connected() || client.available()) && millis() - t0 < 15000 &&
           resp.length() < 8192) {
      while (client.available()) resp += static_cast<char>(client.read());
      esp_task_wdt_reset();
      delay(10);
    }
    ok = resp.startsWith("HTTP/1.1 20") || resp.startsWith("HTTP/1.0 20");
    if (!ok)
      ESP_LOGW(kTag, "register non-2xx: %s",
               resp.substring(0, 60).c_str());
  } else {
    ESP_LOGE(kTag, "register: TLS connect failed");
  }
  client.stop();
  mwf::arbiter::releaseWork();
  return ok;
}

static void buildWorker(const String& scheduler, const String& ns) {
  g_worker = new WorkerParts();
  g_worker->transport = std::make_unique<mwf_transport::EspTransport>(
      std::string(scheduler.c_str()), std::string(g_cfg.apiKey.c_str()));

  // device.echo(arg) → {"echo": <arg>, "device": "mwf-esp32s3-sole", ...} — the
  // one activity of the sole workflow; proves poll → decode → dispatch → respond
  // on hardware without any peripherals, and brands the run as this device.
  g_worker->registry.registerActivity("device.echo", [](const mwf::Bytes& arg) {
    std::string in(arg.begin(), arg.end());
    if (in.empty()) in = "null";
    std::string out = "{\"echo\":" + in +
                      ",\"device\":\"mwf-esp32s3-sole\",\"build\":\"" MWF_FW_BUILD
                      "\",\"heap_kb\":" +
                      std::to_string(ESP.getFreeHeap() / 1024) + "}";
    return mwf::Result<mwf::Bytes>::success(mwf::Bytes(out.begin(), out.end()));
  });

  const std::string identity =
      std::string("mwf-esp32s3-sole@") + WiFi.macAddress().c_str();

  // Activity worker loop (device.echo).
  mwf_core::WorkerConfig wcfg;
  wcfg.ns = ns.c_str();
  wcfg.task_queue = g_cfg.taskQueue.c_str();
  wcfg.identity = identity;
  wcfg.poll_deadline_ms = kPollDeadlineMs;
  wcfg.respond_deadline_ms = 10000;
  wcfg.call_metadata["temporal-namespace"] = wcfg.ns;
  g_worker->loop = std::make_unique<mwf_core::WorkerLoop>(
      *g_worker->transport, g_worker->codec, g_worker->registry,
      g_worker->workerAdapter, wcfg);

  // Workflow loop (replays kWorkflowSpecJson). Same queue: activities schedule on
  // the workflow's own task_queue (activity_task_queue left empty).
  mwf_core::WorkflowConfig fcfg;
  fcfg.ns = ns.c_str();
  fcfg.task_queue = g_cfg.taskQueue.c_str();
  fcfg.identity = identity;
  fcfg.poll_deadline_ms = kPollDeadlineMs;
  fcfg.respond_deadline_ms = 10000;
  fcfg.call_metadata["temporal-namespace"] = fcfg.ns;
  mwf_core::SpecProvider provider =
      [](std::string_view t) -> const mwf_core::WorkflowSpec* {
    return mwf::sole::specMatches(t, g_specOk ? g_spec.name : std::string())
               ? &g_spec
               : nullptr;
  };
  g_worker->wfLoop = std::make_unique<mwf_core::WorkflowLoop>(
      *g_worker->transport, g_worker->codec, g_worker->workflowAdapter, g_store,
      std::move(provider), fcfg);

#if MWF_COOKBOOK == 4
  // Signal SENDER over the SAME EspTransport (one h2 connection, one arbiter
  // slot): the button-press path delivers SignalWorkflowExecution. Called only
  // from loop() — never concurrent with the worker polls. The ~40 KB nanopb
  // Signal request rides the PSRAM big-buffer allocator installed in setup().
  g_wfNs = ns.c_str();
  g_waitingWorkflowId.clear();
  g_signaledWorkflowId.clear();
  g_worker->client = std::make_unique<mwf_core::ClientOps>(
      *g_worker->transport, g_worker->workflowAdapter, identity);
#endif

  ESP_LOGI(kTag, "worker up (%s): scheduler=%s ns=%s queue=%s spec=%s",
           kWorkflowName, scheduler.c_str(), ns.c_str(), wcfg.task_queue.c_str(),
           g_specOk ? "ok" : "MISSING");
}

#if MWF_COOKBOOK == 4
// Deliver the "approve" signal to the execution the WorkflowLoop last reported as
// Waiting. One press → one signal per execution (g_signaledWorkflowId latch).
static void sendApproveSignal(const char* via) {
  if (!g_worker || !g_worker->client) {
    ESP_LOGW(kTag, "signal(%s): worker not ready", via);
    return;
  }
  if (g_waitingWorkflowId.empty()) {
    ESP_LOGW(kTag, "signal(%s): no workflow is waiting yet", via);
    return;
  }
  const std::string target = g_waitingWorkflowId;
  const std::string payload = "{\"approved\":true,\"by\":\"button\"}";
  ESP_LOGI(kTag, "signal(%s): approve -> workflow=%s", via, target.c_str());
  auto r = g_worker->client->signalWorkflow(
      g_wfNs, target, "approve", mwf::Bytes(payload.begin(), payload.end()));
  if (r.ok) {
    ESP_LOGI(kTag, "signal sent");
    g_signaledWorkflowId = target;   // don't re-arm/re-fire for this execution
    g_waitingWorkflowId.clear();
  } else {
    ESP_LOGE(kTag, "signal FAILED: %s", r.error.c_str());  // keep armed → retry
  }
}

// GPIO0 BOOT button: active-low. A falling-edge ISR latches the press (with a
// 50 ms debounce) so a quick tap is caught even while loop() is blocked in a
// ~10 s workflow long-poll; loop() drains the latch and does the actual signal
// (never call TLS from an ISR). digitalPinToInterrupt(0) is attached at runtime,
// after the strapping pin has done its boot job.
static volatile bool g_btnPressed = false;
static uint32_t g_btnLastMs = 0;  // loop()-only, not touched by the ISR
// The ISR must do NOTHING flash-resident: it runs with the cache disabled
// (this firmware writes flash on every workflow tick), and millis() pulls in a
// flash-resident 64-bit divide (__udivdi3) → a cache-disabled panic if the
// button is pressed mid-write. So the ISR only sets a flag; debounce + the TLS
// signal happen in loop() (cache on).
static void IRAM_ATTR onBootButtonIsr() { g_btnPressed = true; }
static void pollSignalButton() {
  if (g_btnPressed) {
    g_btnPressed = false;
    const uint32_t now = millis();  // safe here: loop() context, cache enabled
    if (now - g_btnLastMs < 50) return;  // debounce mechanical bounce
    g_btnLastMs = now;
    sendApproveSignal("button");
  }
}

// Serial fallback so the demo is drivable without a physical press: a line
// "SIGNAL" on the console delivers the same approve signal.
static void pollSignalSerial() {
  static std::string line;
  while (Serial.available()) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\n' || ch == '\r') {
      if (line == "SIGNAL" || line == "signal") sendApproveSignal("serial");
      line.clear();
    } else if (line.size() < 32) {
      line += ch;
    }
  }
}
#endif

void setup() {
  Serial.begin(115200);
  delay(200);
  ESP_LOGI(kTag, "firmware boot (build %s, contracts %s)", MWF_FW_BUILD, "0.1.1");
#if MWF_COOKBOOK == 4
  pinMode(kBootButtonPin, INPUT_PULLUP);  // BOOT button = human-in-the-loop input
  attachInterrupt(digitalPinToInterrupt(kBootButtonPin), onBootButtonIsr, FALLING);
  ESP_LOGW(kTag, "cookbook 4 (human-in-the-loop): press BOOT (GPIO0) or type "
                 "SIGNAL to approve a waiting workflow");
#endif

  // 1) Lift: route mbedTLS + allocation churn to PSRAM before any TLS. This is
  //    what makes the h2 session + TLS handshake + nanopb structs land in the
  //    8 MB PSRAM instead of the ~300 KB internal SRAM.
  mwf::installPsramMbedtls();

  // 2) Lift: working-set allocator hooks -> PSRAM (no-op without PSRAM). The SAME
  //    PSRAM allocator backs the workflow adapter's big-buffer hook: the workflow
  //    PollResponse (~2.4 MB) + Respond request (~617 KB) MUST land in the 8 MB
  //    PSRAM, never the ~300 KB internal SRAM.
  if (ESP.getPsramSize() > 0) {
    auto psAlloc = [](size_t n) -> void* {
      void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
      return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);  // fall back to internal
    };
    auto psFree = [](void* p) { heap_caps_free(p); };
    mwf::setWorkingAllocators(psAlloc, psFree);
    mwf_proto::setBigBufferAllocator(psAlloc, psFree);
  }

  // 2b) Parse the embedded declarative workflow spec once (the SpecProvider hands
  //     &g_spec to the WorkflowLoop by workflow_type). Never throws.
  {
    auto sp = mwf_core::parseWorkflowSpec(kWorkflowSpecJson);
    g_specOk = sp.ok;
    if (sp.ok) g_spec = std::move(sp.value);
    else ESP_LOGE(kTag, "workflow spec parse FAILED: %s", sp.error.c_str());
  }

  // 3) Lift: bound concurrent outbound TLS to 2 slots; the gRPC transport
  //    holds one for its persistent h2 connection, whoami/REST takes the other.
  mwf::arbiter::begin();
  mwf_transport::espTransportSetArbiterHooks(
      [](uint32_t timeoutMs) { return mwf::arbiter::acquireWork(timeoutMs); },
      []() { mwf::arbiter::releaseWork(); });

  // 4) No-SD durable state: NVS journal + LittleFS blobs.
  if (g_store.begin()) {
    ESP_LOGI(kTag, "durable store up (%u keys)", (unsigned)g_store.keys("").size());
  } else {
    ESP_LOGE(kTag, "durable store failed to mount");
  }

  // 4b) First-boot provisioning. If the "mwf" namespace has no apiKey yet
  //     and the build was compiled with -DMWF_PROV_* secrets, seed NVS once.
  //     This writes ONLY the "mwf" namespace — NVS namespaces owned by any
  //     other firmware are untouched, so reflashing that firmware restores it.
  //     Secrets are passed at `pio run` time, never committed.
#if defined(MWF_PROV_APIKEY) && defined(MWF_PROV_SSID) && defined(MWF_PROV_PASS)
  {
    Preferences pp;
    pp.begin("mwf", /*readOnly=*/false);
    if (pp.getString("apiKey", "").isEmpty()) {
      pp.putString("apiKey", MWF_PROV_APIKEY);
      pp.putString("staSsid", MWF_PROV_SSID);
      pp.putString("staPass", MWF_PROV_PASS);
#ifdef MWF_PROV_QUEUE
      pp.putString("taskQueue", MWF_PROV_QUEUE);
#endif
      ESP_LOGW(kTag, "provisioned NVS 'mwf' from build flags (first boot)");
    }
    pp.end();
  }
#endif

  // 5) Config (provisioning writes these; see header).
  Preferences prefs;
  prefs.begin("mwf", /*readOnly=*/true);
  g_cfg.apiKey = prefs.getString("apiKey", "");
  g_cfg.taskQueue = prefs.getString("taskQueue", "mwf-esp");
  String ssid = prefs.getString("staSsid", "");
  String pass = prefs.getString("staPass", "");
  prefs.end();

  // 6) WiFi (STA).
  WiFi.mode(WIFI_STA);
  if (ssid.length()) WiFi.begin(ssid.c_str(), pass.c_str());
  else WiFi.begin();  // stored creds if previously provisioned
  ESP_LOGI(kTag, "config: apiKey=%s taskQueue=%s wifi=%s",
           g_cfg.apiKey.length() ? "set" : "MISSING", g_cfg.taskQueue.c_str(),
           ssid.length() ? ssid.c_str() : "(stored)");

  ESP_LOGI(kTag, "heap: free=%u KB  psram=%u KB",
           (unsigned)(ESP.getFreeHeap() / 1024),
           (unsigned)(ESP.getFreePsram() / 1024));

  // Watchdog on the main loop: a hung loop panics + reboots in ~8 s. The gRPC
  // transport feeds it inside its long-poll recv window.
  esp_task_wdt_config_t wdt{/*timeout_ms=*/8000, /*idle_core_mask=*/0, /*trigger_panic=*/true};
  esp_err_t err = esp_task_wdt_init(&wdt);
  if (err == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(nullptr);
}

// Drop the worker + schedule a whoami retry after repeated transport errors
// (shared by both loops). Returns true if the worker was torn down.
static bool onTransportError(int& consec, uint32_t& nextWhoamiAt,
                             const char* which, const std::string& detail) {
  ESP_LOGW(kTag, "%s transport error (%d consec): %s", which, ++consec,
           detail.c_str());
  if (consec >= 5) {
    delete g_worker;
    g_worker = nullptr;
    consec = 0;
    nextWhoamiAt = millis() + 15000;  // scheduler target may have moved
    return true;
  }
  delay(2000u * consec);
  return false;
}

void loop() {
  esp_task_wdt_reset();
  static uint32_t lastLog = 0;
  static uint32_t nextWhoamiAt = 0;
  static uint32_t lastHeartbeatAt = 0;
  static int transportErrors = 0;
  static mwf::sole::RoundRobin rr;  // alternates workflow-poll / activity-poll

  if (millis() - lastLog > 10000) {
    lastLog = millis();
    ESP_LOGI(kTag, "alive: free=%u KB  psram=%u KB  wifi=%d  worker=%s",
             (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(ESP.getFreePsram() / 1024), (int)WiFi.status(),
             g_worker ? "up" : "down");
  }

#if MWF_COOKBOOK == 4
  // Human-in-the-loop: drain a BOOT-button press / SIGNAL line into the approve
  // signal. Runs every iteration so a press between long-polls is served promptly.
  pollSignalButton();
  pollSignalSerial();
#endif

  if (WiFi.status() != WL_CONNECTED || g_cfg.apiKey.isEmpty()) {
    delay(100);
    return;
  }

  // ── bring the worker up: whoami → assembly → register (retry with backoff) ──
  if (!g_worker) {
    if (millis() < nextWhoamiAt) {
      delay(100);
      return;
    }
    String scheduler, ns;
    if (!whoami(g_cfg.apiKey, scheduler, ns)) {
      nextWhoamiAt = millis() + 30000;
      ESP_LOGW(kTag, "whoami failed; retrying in 30 s");
      return;
    }
    buildWorker(scheduler, ns);
    // Self-register the workflow so the frontend dispatches WFTs to us, then
    // arm the heartbeat clock. A failed first register is non-fatal — the
    // periodic heartbeat retries it.
    bool reg = registerWorkflow(g_cfg.apiKey, g_cfg.taskQueue);
    lastHeartbeatAt = millis();
    ESP_LOGI(kTag, "register %s (queue=%s workflow=%s)",
             reg ? "ok" : "FAILED (will retry via heartbeat)",
             g_cfg.taskQueue.c_str(), kWorkflowName);
  }

  // ── heartbeat: sustain worker-active (re-register), interleaved with polls ──
  if (mwf::sole::heartbeatDue(millis(), lastHeartbeatAt, kHeartbeatMs)) {
    registerWorkflow(g_cfg.apiKey, g_cfg.taskQueue);  // idempotent; best-effort
    lastHeartbeatAt = millis();
  }

  // ── ONE round-robin poll → dispatch → respond cycle (≤10 s, WDT-fed) ────────
  // Alternate the workflow loop and the activity loop over the shared transport
  // so a full workflow (WFT1 → schedule device.echo → activity → WFT2 →
  // complete) turns over in ~30-40 s. Both share the single main task; neither
  // re-enters the transport concurrently.
  if (rr.nextIsWorkflow()) {
    mwf_core::WorkflowTickResult r = g_worker->wfLoop->runOnce(kPollDeadlineMs);
    switch (r.outcome) {
      case mwf_core::WorkflowTickOutcome::Idle:
        transportErrors = 0;
        break;
      case mwf_core::WorkflowTickOutcome::Progressed:
      case mwf_core::WorkflowTickOutcome::Completed:
      case mwf_core::WorkflowTickOutcome::Failed:
        transportErrors = 0;
        ESP_LOGI(kTag, "workflow %s: %s (%s)", r.workflow_type.c_str(),
                 r.detail.c_str(), r.run_id.c_str());
        break;
      case mwf_core::WorkflowTickOutcome::Waiting:
        transportErrors = 0;
#if MWF_COOKBOOK == 4
        // Durable pause on the signal: arm the button target. Log once per new
        // execution; a re-poll while still waiting re-arms without re-logging; an
        // already-signaled execution is left alone (no double-fire).
        if (r.workflow_id != g_signaledWorkflowId) {
          if (r.workflow_id != g_waitingWorkflowId)
            ESP_LOGW(kTag, "WAITING for button (workflow=%s signal=%s)",
                     r.workflow_id.c_str(), r.signal_name.c_str());
          g_waitingWorkflowId = r.workflow_id;
        }
#else
        ESP_LOGI(kTag, "workflow waiting: %s", r.detail.c_str());
#endif
        break;
      case mwf_core::WorkflowTickOutcome::ProtocolError:
        ESP_LOGE(kTag, "workflow protocol error: %s", r.detail.c_str());
        delay(5000);
        break;
      case mwf_core::WorkflowTickOutcome::TransportError:
        onTransportError(transportErrors, nextWhoamiAt, "workflow", r.detail);
        break;
    }
  } else {
    mwf_core::TickResult r = g_worker->loop->runOnce(kPollDeadlineMs);
    switch (r.outcome) {
      case mwf_core::TickOutcome::Idle:
        transportErrors = 0;
        break;
      case mwf_core::TickOutcome::Completed:
      case mwf_core::TickOutcome::Failed:
        transportErrors = 0;
        ESP_LOGI(kTag, "activity %s: %s (%s)",
                 r.outcome == mwf_core::TickOutcome::Completed ? "completed" : "failed",
                 r.activity.c_str(), r.detail.c_str());
        break;
      case mwf_core::TickOutcome::ProtocolError:
        ESP_LOGE(kTag, "activity protocol error: %s", r.detail.c_str());
        delay(5000);
        break;
      case mwf_core::TickOutcome::TransportError:
        onTransportError(transportErrors, nextWhoamiAt, "activity", r.detail);
        break;
    }
  }
}

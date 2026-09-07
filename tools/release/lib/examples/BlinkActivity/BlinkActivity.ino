// BlinkActivity — a minimal Mistral Workflows / Temporal activity worker.
//
// The board joins WiFi, connects to the workflow frontend over gRPC/TLS, and
// long-polls the task queue. When a workflow schedules the "device.blink"
// activity, the handler blinks the LED and returns a small JSON result that
// the workflow sees as the activity's return value.
//
// Fill in the five config values below, flash, then from any Temporal/Mistral
// Workflows client start a workflow that schedules activity "device.blink"
// on task queue TASK_QUEUE with an input like: {"times": 3}

#include <WiFi.h>
#include <EmbeddedTemporal.h>

// ── config ──────────────────────────────────────────────────────────────────
const char* WIFI_SSID   = "your-wifi";
const char* WIFI_PASS   = "your-password";
const char* TARGET      = "wf-scheduler.mistral.ai:443";  // frontend host:port
const char* API_KEY     = "your-api-key";                 // sent as Bearer token
const char* NAMESPACE_  = "your-namespace";
const char* TASK_QUEUE  = "esp32-blink";

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// ── worker parts (contracts-injected, no globals inside the library) ────────
static mwf_transport::EspTransport* transport = nullptr;
static mwf_codec::PayloadCodecV1    codec;
static mwf_proto::NanopbWorkerAdapter adapter;
static mwf_core::ActivityRegistry   registry;
static mwf_core::WorkerLoop*        worker = nullptr;

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
  }
  Serial.printf("\nWiFi up: %s\n", WiFi.localIP().toString().c_str());

  // device.blink({"times": N}) -> {"blinked": N} — parses the one integer it
  // needs from the raw JSON argument bytes; no JSON library required.
  registry.registerActivity("device.blink", [](const mwf::Bytes& arg) {
    std::string in(arg.begin(), arg.end());
    int times = 3;
    size_t pos = in.find("\"times\"");
    if (pos != std::string::npos) {
      pos = in.find_first_of("0123456789", pos);
      if (pos != std::string::npos) times = atoi(in.c_str() + pos);
    }
    if (times < 1) times = 1;
    if (times > 20) times = 20;
    for (int i = 0; i < times; i++) {
      digitalWrite(LED_BUILTIN, HIGH); delay(150);
      digitalWrite(LED_BUILTIN, LOW);  delay(150);
    }
    std::string out = "{\"blinked\":" + std::to_string(times) + "}";
    return mwf::Result<mwf::Bytes>::success(mwf::Bytes(out.begin(), out.end()));
  });

  transport = new mwf_transport::EspTransport(TARGET, API_KEY);

  mwf_core::WorkerConfig cfg;
  cfg.ns = NAMESPACE_;
  cfg.task_queue = TASK_QUEUE;
  cfg.identity = std::string("esp32-blink@") + WiFi.macAddress().c_str();
  cfg.poll_deadline_ms = 70000;  // server holds the long-poll ~45-60 s
  cfg.call_metadata["temporal-namespace"] = cfg.ns;
  worker = new mwf_core::WorkerLoop(*transport, codec, registry, adapter, cfg);

  Serial.printf("worker up: queue=%s ns=%s\n", TASK_QUEUE, NAMESPACE_);
}

void loop() {
  mwf_core::TickResult r = worker->runOnce(worker->config().poll_deadline_ms);
  switch (r.outcome) {
    case mwf_core::TickOutcome::Idle:
      break;  // empty long-poll; poll again
    case mwf_core::TickOutcome::Completed:
      Serial.printf("activity %s completed\n", r.activity.c_str());
      break;
    case mwf_core::TickOutcome::Failed:
      Serial.printf("activity %s failed: %s\n", r.activity.c_str(), r.detail.c_str());
      break;
    case mwf_core::TickOutcome::TransportError:
      Serial.printf("transport error: %s (backing off)\n", r.detail.c_str());
      delay(5000);
      break;
    case mwf_core::TickOutcome::ProtocolError:
      Serial.printf("protocol error: %s\n", r.detail.c_str());
      delay(5000);
      break;
  }
}

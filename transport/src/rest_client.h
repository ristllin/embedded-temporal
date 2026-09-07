// transport: RestClient — Mistral REST control-plane client.
// The workers ride gRPC (ITransport), but the control plane — worker identity,
// deployment registration, heartbeat — is plain HTTPS REST against
// api.mistral.ai. On desktop this is libcurl; on device the same paths reuse
// a single-write HTTPS pattern (one write() of a PSRAM-buffered request body;
// never serializeJson straight into the TLS client).
//
// REST surface (Mistral control plane, from mistral-workflows-cpp README):
//   GET  /v1/workflows/workers/whoami       — discover the Temporal frontend
//   POST /v1/workflows/register             — register a workflow (metadata-only)
//   POST /v1/workflows/workers/heartbeat    — worker liveness
//   POST /v1/workflows/executions/register  — per-run registration
//   GET  /v1/workflows/executions           — list executions
#pragma once
#include <string>
#include <vector>
#include "mwf/contracts.h"

namespace mwf_transport {

// Parsed GET /v1/workflows/workers/whoami — the real Temporal frontend the
// worker's gRPC ITransport should dial. Proven live: scheduler_url =
// "wf-scheduler.mistral.ai:443", a real namespace, tls = true.
struct WhoAmI {
  std::string scheduler_url;   // e.g. "wf-scheduler.mistral.ai:443" → DesktopTransport target
  std::string namespace_;      // Temporal namespace → temporal-namespace metadata
  bool        tls = true;      // whether the scheduler expects TLS
  std::string raw;             // the raw JSON body (forward-compat / debugging)
};

// One entry from GET /v1/workflows/executions (shape is best-effort until the
// live schema is pinned; raw carries the untouched object).
struct ExecutionInfo {
  std::string workflow_id;
  std::string run_id;
  std::string status;
  std::string raw;
};

// Low-level HTTP outcome, exposed so callers can distinguish transport failure
// (curl error) from an API-level non-2xx.
struct HttpResponse {
  bool        transport_ok = false;  // the request reached the server
  long        status = 0;            // HTTP status code
  std::string body;                  // response body
  std::string error;                // curl/transport error, if any
};

class RestClient {
 public:
  // base_url e.g. "https://api.mistral.ai"; api_key → Authorization: Bearer.
  RestClient(std::string base_url, std::string api_key);

  // GET /v1/workflows/workers/whoami → {scheduler_url, namespace, tls}.
  mwf::Result<WhoAmI> whoami();

  // GET /v1/workflows/executions → list of in-flight/recent executions.
  mwf::Result<std::vector<ExecutionInfo>> listExecutions();

  // POST /v1/workflows/register — metadata-only workflow registration.
  // Stub: issues the documented request; body assembly is best-effort until the
  // live schema is confirmed. Returns the raw response body on 2xx.
  mwf::Result<std::string> registerWorker(std::string_view task_queue,
                                          std::string_view version);

  // POST /v1/workflows/workers/heartbeat — worker liveness.
  mwf::Result<std::string> heartbeat();

  // ── self-registration + server-side trigger (probe-evidence bodies) ────────
  // POST /v1/workflows/register — the deployment. Sends the EXACT minimal body a
  // live `run_worker()` sends (see docs/mistral-deploy-trigger.md + the
  // sole-worker probe): one WorkflowSpecWithTaskQueue definition on `task_queue`,
  // deployment_name == task_queue, a local deployment_location. This alone flips
  // workflow.active + makes the queue dispatchable — NO heartbeat is needed for
  // dispatch. input_schema_json / output_schema_json are the raw JSON-Schema
  // objects (opaque to the frontend beyond structural validation). Returns the
  // raw response (workflow_registration_refs, has_conflicts, ...).
  // signals_json is the raw JSON array of SignalDefinition objects
  // ([{"name","input_schema"[,"description"]}, ...]) declared on the workflow —
  // Mistral's frontend REJECTS a signal whose name isn't declared here (HTTP 422
  // "Signal handler '<name>' not found"), so a wait_signal workflow MUST list its
  // signals. Defaults to "[]" (no signals — cookbooks 01..03).
  mwf::Result<std::string> registerWorkflow(std::string_view name,
                                            std::string_view task_queue,
                                            std::string_view input_schema_json,
                                            std::string_view output_schema_json,
                                            std::string_view worker_name,
                                            std::string_view signals_json = "[]");

  // POST /v1/workflows/{name}/execute {"input": <obj>, "task_queue": q} →
  // returns the execution_id (64-hex Temporal workflow id). input_json is the
  // raw JSON value for the "input" field; task_queue may be empty (frontend
  // routes to the workflow's registered queue).
  mwf::Result<std::string> executeWorkflow(std::string_view name,
                                           std::string_view input_json,
                                           std::string_view task_queue);

  // GET /v1/workflows/{name} → workflow.active (worker-liveness / dispatch gate).
  mwf::Result<bool> workflowActive(std::string_view name);

  // GET /v1/workflows/executions/{execution_id} → the raw status JSON (status +
  // result live inside). The caller reads status/result from `.raw`.
  struct ExecutionStatus {
    std::string status;  // RUNNING|COMPLETED|FAILED|TERMINATED|TIMED_OUT|CANCELED
    std::string raw;     // full response body (result object lives here)
  };
  mwf::Result<ExecutionStatus> getExecution(std::string_view execution_id);

  // POST /v1/workflows/executions/{id}/signals {"name","input"} — deliver a
  // signal to a running execution (the client side of the wait_signal grammar;
  // the mistralai-workflows integration helper posts exactly this). A workflow
  // paused on `wait_signal "<signal_name>"` unblocks on its next task; input_json
  // is the raw-JSON signal argument (e.g. {"approved":true}). Returns the raw
  // response body on 2xx (SignalWorkflowResponse: {"message":"Signal accepted"}).
  mwf::Result<std::string> signalExecution(std::string_view execution_id,
                                           std::string_view signal_name,
                                           std::string_view input_json);

  // POST /v1/workflows/executions/{id}/terminate — best-effort cleanup of a
  // stuck/leftover run (204 on success).
  mwf::Result<std::string> terminateExecution(std::string_view execution_id);

  // PUT /v1/workflows/{name-or-id}/archive — the strongest deployment cleanup
  // (no delete endpoint exists; an archived workflow stops dispatching).
  mwf::Result<std::string> archiveWorkflow(std::string_view name);

 private:
  HttpResponse get(const std::string& path);
  HttpResponse post(const std::string& path, const std::string& json_body);
  HttpResponse put(const std::string& path, const std::string& json_body);

  std::string base_url_;   // e.g. "https://api.mistral.ai" (no trailing slash)
  std::string api_key_;
};

}  // namespace mwf_transport

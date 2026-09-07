// transport: RestClient bodies — libcurl HTTPS + a tiny,
// dependency-free JSON field reader (the control-plane responses we consume are
// shallow: whoami is a flat object, executions a flat array of flat objects).
// A full JSON lib (nlohmann via mwf::json — the device build vendors it) can
// replace the reader later; keeping it self-contained avoids pulling a JSON
// dependency into the desktop transport build for three fields.
//
// On device the HTTPS body is built in PSRAM and written in ONE client.write()
// — the whole request goes out in one write() so per-chunk TLS records don't
// fragment/collapse the internal heap. That path lives in esp/; this file is
// the desktop libcurl twin.
#include "rest_client.h"

#include <curl/curl.h>

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace mwf_transport {
namespace {

// ── minimal JSON field readers ────────────────────────────────────────────────
// Not a general parser: they locate "key" then read the immediately-following
// scalar. Sufficient for the flat control-plane payloads we consume; they do NOT
// attempt nested-object disambiguation beyond the first match.

// Find the position just after the colon following "key".
std::size_t valuePos(const std::string& s, const std::string& key, std::size_t from = 0) {
  const std::string needle = "\"" + key + "\"";
  std::size_t k = s.find(needle, from);
  if (k == std::string::npos) return std::string::npos;
  std::size_t c = s.find(':', k + needle.size());
  if (c == std::string::npos) return std::string::npos;
  ++c;
  while (c < s.size() && std::isspace(static_cast<unsigned char>(s[c]))) ++c;
  return c;
}

// Read a JSON string value for "key" (handles \" and \\ escapes).
bool jsonString(const std::string& s, const std::string& key, std::string& out,
                std::size_t from = 0) {
  std::size_t p = valuePos(s, key, from);
  if (p == std::string::npos || p >= s.size() || s[p] != '"') return false;
  ++p;  // past opening quote
  std::string v;
  while (p < s.size()) {
    char ch = s[p];
    if (ch == '\\' && p + 1 < s.size()) {
      char n = s[p + 1];
      switch (n) {
        case 'n': v += '\n'; break;
        case 't': v += '\t'; break;
        case 'r': v += '\r'; break;
        default:  v += n;    break;  // covers \" \\ \/ and others verbatim
      }
      p += 2;
      continue;
    }
    if (ch == '"') { out = std::move(v); return true; }
    v += ch;
    ++p;
  }
  return false;
}

// Read a JSON bool value for "key".
bool jsonBool(const std::string& s, const std::string& key, bool& out,
              std::size_t from = 0) {
  std::size_t p = valuePos(s, key, from);
  if (p == std::string::npos) return false;
  if (s.compare(p, 4, "true") == 0)  { out = true;  return true; }
  if (s.compare(p, 5, "false") == 0) { out = false; return true; }
  return false;
}

// Split a top-level JSON array of objects into the raw text of each object.
// Brace-matched, quote/escape aware. If s is an object with an array field
// (e.g. {"executions":[...]}), the caller should hand in that array slice; we
// also tolerate a bare object by returning it as a single element.
std::vector<std::string> splitObjects(const std::string& s) {
  std::vector<std::string> out;
  int depth = 0;
  bool in_str = false, esc = false;
  std::size_t start = std::string::npos;
  for (std::size_t i = 0; i < s.size(); ++i) {
    char ch = s[i];
    if (in_str) {
      if (esc)            esc = false;
      else if (ch == '\\') esc = true;
      else if (ch == '"')  in_str = false;
      continue;
    }
    if (ch == '"')      { in_str = true; continue; }
    if (ch == '{') {
      if (depth == 0) start = i;
      ++depth;
    } else if (ch == '}') {
      --depth;
      if (depth == 0 && start != std::string::npos) {
        out.push_back(s.substr(start, i - start + 1));
        start = std::string::npos;
      }
    }
  }
  return out;
}

// curl write callback → append to a std::string.
std::size_t writeCb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* body = static_cast<std::string*>(userdata);
  body->append(ptr, size * nmemb);
  return size * nmemb;
}

// One-time global curl init (thread-unsafe by design of curl_global_init; the
// desktop control-plane client is used from a single control thread).
struct CurlGlobal {
  CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlGlobal() { curl_global_cleanup(); }
};
CurlGlobal g_curl_global;

}  // namespace

RestClient::RestClient(std::string base_url, std::string api_key)
    : base_url_(std::move(base_url)), api_key_(std::move(api_key)) {
  while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();
}

HttpResponse RestClient::get(const std::string& path) {
  HttpResponse r;
  CURL* c = curl_easy_init();
  if (!c) { r.error = "curl_easy_init failed"; return r; }

  const std::string url = base_url_ + path;
  struct curl_slist* headers = nullptr;
  const std::string auth = "Authorization: Bearer " + api_key_;
  headers = curl_slist_append(headers, auth.c_str());
  headers = curl_slist_append(headers, "Accept: application/json");

  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "mwf-transport/0.1");

  CURLcode rc = curl_easy_perform(c);
  if (rc == CURLE_OK) {
    r.transport_ok = true;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
  } else {
    r.error = curl_easy_strerror(rc);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(c);
  return r;
}

HttpResponse RestClient::post(const std::string& path, const std::string& json_body) {
  HttpResponse r;
  CURL* c = curl_easy_init();
  if (!c) { r.error = "curl_easy_init failed"; return r; }

  const std::string url = base_url_ + path;
  struct curl_slist* headers = nullptr;
  const std::string auth = "Authorization: Bearer " + api_key_;
  headers = curl_slist_append(headers, auth.c_str());
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, json_body.c_str());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "mwf-transport/0.1");

  CURLcode rc = curl_easy_perform(c);
  if (rc == CURLE_OK) {
    r.transport_ok = true;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
  } else {
    r.error = curl_easy_strerror(rc);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(c);
  return r;
}

HttpResponse RestClient::put(const std::string& path, const std::string& json_body) {
  HttpResponse r;
  CURL* c = curl_easy_init();
  if (!c) { r.error = "curl_easy_init failed"; return r; }

  const std::string url = base_url_ + path;
  struct curl_slist* headers = nullptr;
  const std::string auth = "Authorization: Bearer " + api_key_;
  headers = curl_slist_append(headers, auth.c_str());
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PUT");
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, json_body.c_str());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "mwf-transport/0.1");

  CURLcode rc = curl_easy_perform(c);
  if (rc == CURLE_OK) {
    r.transport_ok = true;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
  } else {
    r.error = curl_easy_strerror(rc);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(c);
  return r;
}

mwf::Result<WhoAmI> RestClient::whoami() {
  HttpResponse http = get("/v1/workflows/workers/whoami");
  if (!http.transport_ok)
    return mwf::Result<WhoAmI>::failure("whoami transport error: " + http.error);
  if (http.status < 200 || http.status >= 300)
    return mwf::Result<WhoAmI>::failure(
        "whoami HTTP " + std::to_string(http.status) + ": " + http.body);

  WhoAmI w;
  w.raw = http.body;
  // The frontend field is documented as scheduler_url; accept a couple of
  // observed aliases so a wire-name tweak doesn't silently blank it.
  if (!jsonString(http.body, "scheduler_url", w.scheduler_url))
    if (!jsonString(http.body, "temporal_url", w.scheduler_url))
      jsonString(http.body, "target_host", w.scheduler_url);
  if (!jsonString(http.body, "namespace", w.namespace_))
    jsonString(http.body, "namespace_", w.namespace_);
  jsonBool(http.body, "tls", w.tls);

  if (w.scheduler_url.empty())
    return mwf::Result<WhoAmI>::failure(
        "whoami: scheduler_url missing in response: " + http.body);
  return mwf::Result<WhoAmI>::success(std::move(w));
}

mwf::Result<std::vector<ExecutionInfo>> RestClient::listExecutions() {
  HttpResponse http = get("/v1/workflows/executions");
  if (!http.transport_ok)
    return mwf::Result<std::vector<ExecutionInfo>>::failure(
        "listExecutions transport error: " + http.error);
  if (http.status < 200 || http.status >= 300)
    return mwf::Result<std::vector<ExecutionInfo>>::failure(
        "listExecutions HTTP " + std::to_string(http.status) + ": " + http.body);

  std::vector<ExecutionInfo> out;
  for (const std::string& obj : splitObjects(http.body)) {
    ExecutionInfo e;
    e.raw = obj;
    jsonString(obj, "workflow_id", e.workflow_id);
    jsonString(obj, "run_id", e.run_id);
    if (!jsonString(obj, "status", e.status)) jsonString(obj, "state", e.status);
    // Skip the outer wrapper object (e.g. {"executions":[...]}) which yields no
    // execution fields; only keep objects that look like an execution row.
    if (!e.workflow_id.empty() || !e.run_id.empty() || !e.status.empty())
      out.push_back(std::move(e));
  }
  return mwf::Result<std::vector<ExecutionInfo>>::success(std::move(out));
}

mwf::Result<std::string> RestClient::registerWorker(std::string_view task_queue,
                                                    std::string_view version) {
  // Metadata-only registration (README: {name, task_queue, input_schema,
  // output_schema, type:"code"}). Body assembly here is best-effort — the live
  // field set is confirmed against the running control plane before the
  // desktop activity-worker E2E gate.
  std::string body = "{";
  body += "\"task_queue\":\"" + std::string(task_queue) + "\",";
  body += "\"version\":\"" + std::string(version) + "\",";
  body += "\"type\":\"code\"}";
  HttpResponse http = post("/v1/workflows/register", body);
  if (!http.transport_ok)
    return mwf::Result<std::string>::failure("register transport error: " + http.error);
  if (http.status < 200 || http.status >= 300)
    return mwf::Result<std::string>::failure(
        "register HTTP " + std::to_string(http.status) + ": " + http.body);
  return mwf::Result<std::string>::success(http.body);
}

mwf::Result<std::string> RestClient::heartbeat() {
  HttpResponse http = post("/v1/workflows/workers/heartbeat", "{}");
  if (!http.transport_ok)
    return mwf::Result<std::string>::failure("heartbeat transport error: " + http.error);
  if (http.status < 200 || http.status >= 300)
    return mwf::Result<std::string>::failure(
        "heartbeat HTTP " + std::to_string(http.status) + ": " + http.body);
  return mwf::Result<std::string>::success(http.body);
}

mwf::Result<std::string> RestClient::registerWorkflow(
    std::string_view name, std::string_view task_queue,
    std::string_view input_schema_json, std::string_view output_schema_json,
    std::string_view worker_name, std::string_view signals_json) {
  // The EXACT minimal WorkflowSpecWithTaskQueue the sole-worker probe registered
  // (docs/mistral-deploy-trigger.md §1). Field order/keys mirror a captured live
  // register; the two schema objects are injected verbatim as JSON.
  std::string n(name), q(task_queue), w(worker_name);
  std::string body;
  body += "{\"definitions\":[{";
  body += "\"name\":\"" + n + "\",";
  body += "\"task_queue\":\"" + q + "\",";
  body += "\"input_schema\":" + std::string(input_schema_json) + ",";
  body += "\"output_schema\":" + std::string(output_schema_json) + ",";
  body += "\"signals\":" + std::string(signals_json) + ",\"queries\":[],\"updates\":[],";
  body += "\"enforce_determinism\":true,";
  body += "\"on_behalf_of\":false,";
  body += "\"execution_timeout\":\"PT1H\",";
  body += "\"plugin_metadata\":null,\"display_name\":null,\"description\":null,";
  body += "\"is_technical\":false,\"schedules\":[]";
  body += "}],";
  body += "\"deployment_name\":\"" + q + "\",";
  body += "\"worker_name\":\"" + w + "\",";
  body += "\"deployment_location\":{\"location_type\":\"local\"}}";

  HttpResponse http = post("/v1/workflows/register", body);
  if (!http.transport_ok)
    return mwf::Result<std::string>::failure("registerWorkflow transport error: " + http.error);
  if (http.status < 200 || http.status >= 300)
    return mwf::Result<std::string>::failure(
        "registerWorkflow HTTP " + std::to_string(http.status) + ": " + http.body);
  return mwf::Result<std::string>::success(http.body);
}

mwf::Result<std::string> RestClient::executeWorkflow(std::string_view name,
                                                     std::string_view input_json,
                                                     std::string_view task_queue) {
  std::string body = "{\"input\":" + std::string(input_json);
  if (!task_queue.empty())
    body += ",\"task_queue\":\"" + std::string(task_queue) + "\"";
  body += "}";
  HttpResponse http = post("/v1/workflows/" + std::string(name) + "/execute", body);
  if (!http.transport_ok)
    return mwf::Result<std::string>::failure("executeWorkflow transport error: " + http.error);
  if (http.status < 200 || http.status >= 300)
    return mwf::Result<std::string>::failure(
        "executeWorkflow HTTP " + std::to_string(http.status) + ": " + http.body);
  std::string execution_id;
  if (!jsonString(http.body, "execution_id", execution_id))
    return mwf::Result<std::string>::failure(
        "executeWorkflow: no execution_id in response: " + http.body);
  return mwf::Result<std::string>::success(std::move(execution_id));
}

mwf::Result<bool> RestClient::workflowActive(std::string_view name) {
  HttpResponse http = get("/v1/workflows/" + std::string(name));
  if (!http.transport_ok)
    return mwf::Result<bool>::failure("workflowActive transport error: " + http.error);
  if (http.status < 200 || http.status >= 300)
    return mwf::Result<bool>::failure(
        "workflowActive HTTP " + std::to_string(http.status) + ": " + http.body);
  bool active = false;
  jsonBool(http.body, "active", active);
  return mwf::Result<bool>::success(active);
}

mwf::Result<RestClient::ExecutionStatus> RestClient::getExecution(
    std::string_view execution_id) {
  HttpResponse http = get("/v1/workflows/executions/" + std::string(execution_id));
  if (!http.transport_ok)
    return mwf::Result<ExecutionStatus>::failure("getExecution transport error: " + http.error);
  if (http.status < 200 || http.status >= 300)
    return mwf::Result<ExecutionStatus>::failure(
        "getExecution HTTP " + std::to_string(http.status) + ": " + http.body);
  ExecutionStatus s;
  s.raw = http.body;
  jsonString(http.body, "status", s.status);
  return mwf::Result<ExecutionStatus>::success(std::move(s));
}

mwf::Result<std::string> RestClient::signalExecution(std::string_view execution_id,
                                                     std::string_view signal_name,
                                                     std::string_view input_json) {
  // {"name": "<signal>", "input": <raw JSON value>}. input defaults to null when
  // the caller passes an empty string (a signal with no payload).
  std::string in(input_json);
  if (in.empty()) in = "null";
  std::string body = "{\"name\":\"" + std::string(signal_name) + "\",\"input\":" + in + "}";
  HttpResponse http = post(
      "/v1/workflows/executions/" + std::string(execution_id) + "/signals", body);
  if (!http.transport_ok)
    return mwf::Result<std::string>::failure("signalExecution transport error: " + http.error);
  if (http.status < 200 || http.status >= 300)
    return mwf::Result<std::string>::failure(
        "signalExecution HTTP " + std::to_string(http.status) + ": " + http.body);
  return mwf::Result<std::string>::success(http.body);
}

mwf::Result<std::string> RestClient::terminateExecution(std::string_view execution_id) {
  HttpResponse http =
      post("/v1/workflows/executions/" + std::string(execution_id) + "/terminate", "{}");
  if (!http.transport_ok)
    return mwf::Result<std::string>::failure("terminateExecution transport error: " + http.error);
  if (http.status < 200 || http.status >= 300)
    return mwf::Result<std::string>::failure(
        "terminateExecution HTTP " + std::to_string(http.status) + ": " + http.body);
  return mwf::Result<std::string>::success(http.body);
}

mwf::Result<std::string> RestClient::archiveWorkflow(std::string_view name) {
  HttpResponse http = put("/v1/workflows/" + std::string(name) + "/archive", "{}");
  if (!http.transport_ok)
    return mwf::Result<std::string>::failure("archiveWorkflow transport error: " + http.error);
  if (http.status < 200 || http.status >= 300)
    return mwf::Result<std::string>::failure(
        "archiveWorkflow HTTP " + std::to_string(http.status) + ": " + http.body);
  return mwf::Result<std::string>::success(http.body);
}

}  // namespace mwf_transport

// core: History loader — Temporal proto-JSON -> lean event model.
#include "mwf_core/history.h"

#include <cstdlib>

#include "mwf_core/detail/json_util.h"

namespace mwf_core {
namespace {

// Base64 decode (standard alphabet, '=' padding). Returns false on bad input.
bool b64decode(std::string_view in, std::string& out) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  out.clear();
  out.reserve(in.size() * 3 / 4);
  int buf = 0, bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\n' || c == '\r') continue;
    const int v = val(c);
    if (v < 0) return false;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((buf >> bits) & 0xFF);
    }
  }
  return true;
}

int64_t asInt64(const mwf::json& j) {
  if (j.is_number_integer()) return j.get<int64_t>();
  if (j.is_string()) return std::strtoll(j.get<std::string>().c_str(), nullptr, 10);
  return 0;
}

EventType typeFromString(const std::string& s) {
  struct Row { const char* name; EventType t; };
  static const Row kRows[] = {
      {"EVENT_TYPE_WORKFLOW_EXECUTION_STARTED",   EventType::WorkflowExecutionStarted},
      {"EVENT_TYPE_WORKFLOW_TASK_SCHEDULED",      EventType::WorkflowTaskScheduled},
      {"EVENT_TYPE_WORKFLOW_TASK_STARTED",        EventType::WorkflowTaskStarted},
      {"EVENT_TYPE_WORKFLOW_TASK_COMPLETED",      EventType::WorkflowTaskCompleted},
      {"EVENT_TYPE_ACTIVITY_TASK_SCHEDULED",      EventType::ActivityTaskScheduled},
      {"EVENT_TYPE_ACTIVITY_TASK_STARTED",        EventType::ActivityTaskStarted},
      {"EVENT_TYPE_ACTIVITY_TASK_COMPLETED",      EventType::ActivityTaskCompleted},
      {"EVENT_TYPE_ACTIVITY_TASK_FAILED",         EventType::ActivityTaskFailed},
      {"EVENT_TYPE_TIMER_STARTED",                EventType::TimerStarted},
      {"EVENT_TYPE_TIMER_FIRED",                  EventType::TimerFired},
      {"EVENT_TYPE_WORKFLOW_EXECUTION_COMPLETED", EventType::WorkflowExecutionCompleted},
      {"EVENT_TYPE_WORKFLOW_EXECUTION_FAILED",    EventType::WorkflowExecutionFailed},
      {"EVENT_TYPE_WORKFLOW_EXECUTION_SIGNALED",  EventType::WorkflowExecutionSignaled},
  };
  for (const auto& r : kRows)
    if (s == r.name) return r.t;
  return EventType::Unknown;
}

// attrs[key].payloads[i].data (base64) -> decoded raw-JSON texts.
bool extractPayloads(const mwf::json& attrs, const char* key,
                     std::vector<std::string>& out, std::string& err) {
  if (!attrs.contains(key)) return true;  // absent input/result is fine
  const mwf::json& p = attrs[key];
  if (!p.is_object() || !p.contains("payloads")) return true;
  const mwf::json& arr = p["payloads"];
  if (!arr.is_array()) { err = "payloads is not an array"; return false; }
  for (const auto& pl : arr) {
    std::string decoded;
    if (pl.is_object() && pl.contains("data") && pl["data"].is_string()) {
      if (!b64decode(pl["data"].get<std::string>(), decoded)) {
        err = "bad base64 in payload data";
        return false;
      }
    } else {
      decoded = "null";  // Temporal encodes a null payload as data "null"; treat absent as null
    }
    out.push_back(std::move(decoded));
  }
  return true;
}

std::string failureMessage(const mwf::json& attrs) {
  if (attrs.contains("failure") && attrs["failure"].is_object() &&
      attrs["failure"].contains("message") && attrs["failure"]["message"].is_string()) {
    return attrs["failure"]["message"].get<std::string>();
  }
  return {};
}

}  // namespace

const HistoryEvent* History::find(int64_t event_id) const {
  for (const auto& e : events)
    if (e.event_id == event_id) return &e;
  return nullptr;
}

History History::prefix(size_t n) const {
  History h;
  const size_t take = n < events.size() ? n : events.size();
  h.events.assign(events.begin(), events.begin() + take);
  return h;
}

mwf::Result<History> loadHistoryJson(std::string_view history_json) {
  auto parsed = jsonutil::parse(history_json);
  if (!parsed) return mwf::Result<History>::failure("history: " + parsed.error);
  const mwf::json& root = parsed.value;
  if (!root.is_object() || !root.contains("events") || !root["events"].is_array()) {
    return mwf::Result<History>::failure("history: root must be {\"events\":[...]}");
  }

  History h;
  h.events.reserve(root["events"].size());
  for (const auto& ej : root["events"]) {
    if (!ej.is_object()) return mwf::Result<History>::failure("history: event must be an object");
    HistoryEvent e;
    if (ej.contains("eventId")) e.event_id = asInt64(ej["eventId"]);
    if (e.event_id <= 0) return mwf::Result<History>::failure("history: missing/invalid eventId");
    if (ej.contains("eventType") && ej["eventType"].is_string()) {
      e.type = typeFromString(ej["eventType"].get<std::string>());
    }

    // Locate the per-type attributes object (key ends in "EventAttributes").
    const mwf::json* attrs = nullptr;
    for (auto it = ej.begin(); it != ej.end(); ++it) {
      const std::string& k = it.key();
      static const std::string kSuffix = "EventAttributes";
      if (k.size() > kSuffix.size() &&
          k.compare(k.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
        attrs = &it.value();
        break;
      }
    }

    std::string err;
    if (attrs && attrs->is_object()) {
      const mwf::json& a = *attrs;
      switch (e.type) {
        case EventType::WorkflowExecutionStarted:
          if (a.contains("workflowType") && a["workflowType"].is_object() &&
              a["workflowType"].contains("name")) {
            if (!a["workflowType"]["name"].is_string())
              return mwf::Result<History>::failure("history: workflowType.name must be a string");
            e.workflow_type = a["workflowType"]["name"].get<std::string>();
          }
          if (a.contains("workflowId") && a["workflowId"].is_string())
            e.workflow_id = a["workflowId"].get<std::string>();
          if (a.contains("originalExecutionRunId") && a["originalExecutionRunId"].is_string())
            e.run_id = a["originalExecutionRunId"].get<std::string>();
          else if (a.contains("firstExecutionRunId") && a["firstExecutionRunId"].is_string())
            e.run_id = a["firstExecutionRunId"].get<std::string>();
          if (!extractPayloads(a, "input", e.payloads, err))
            return mwf::Result<History>::failure("history: " + err);
          break;
        case EventType::ActivityTaskScheduled:
          if (a.contains("activityType") && a["activityType"].is_object() &&
              a["activityType"].contains("name")) {
            if (!a["activityType"]["name"].is_string())
              return mwf::Result<History>::failure("history: activityType.name must be a string");
            e.activity_name = a["activityType"]["name"].get<std::string>();
          }
          if (a.contains("activityId") && a["activityId"].is_string())
            e.activity_id = a["activityId"].get<std::string>();
          if (!extractPayloads(a, "input", e.payloads, err))
            return mwf::Result<History>::failure("history: " + err);
          break;
        case EventType::ActivityTaskStarted:
          if (a.contains("scheduledEventId")) e.scheduled_event_id = asInt64(a["scheduledEventId"]);
          break;
        case EventType::ActivityTaskCompleted:
          if (a.contains("scheduledEventId")) e.scheduled_event_id = asInt64(a["scheduledEventId"]);
          if (a.contains("startedEventId"))   e.started_event_id   = asInt64(a["startedEventId"]);
          if (!extractPayloads(a, "result", e.payloads, err))
            return mwf::Result<History>::failure("history: " + err);
          break;
        case EventType::ActivityTaskFailed:
          if (a.contains("scheduledEventId")) e.scheduled_event_id = asInt64(a["scheduledEventId"]);
          if (a.contains("startedEventId"))   e.started_event_id   = asInt64(a["startedEventId"]);
          e.failure_message = failureMessage(a);
          break;
        case EventType::TimerStarted:
          if (a.contains("timerId") && a["timerId"].is_string())
            e.timer_id = a["timerId"].get<std::string>();
          break;
        case EventType::TimerFired:
          if (a.contains("timerId") && a["timerId"].is_string())
            e.timer_id = a["timerId"].get<std::string>();
          if (a.contains("startedEventId")) e.started_event_id = asInt64(a["startedEventId"]);
          break;
        case EventType::WorkflowExecutionCompleted:
          if (!extractPayloads(a, "result", e.payloads, err))
            return mwf::Result<History>::failure("history: " + err);
          break;
        case EventType::WorkflowExecutionFailed:
          e.failure_message = failureMessage(a);
          break;
        case EventType::WorkflowExecutionSignaled:
          if (a.contains("signalName") && a["signalName"].is_string())
            e.signal_name = a["signalName"].get<std::string>();
          if (!extractPayloads(a, "input", e.payloads, err))
            return mwf::Result<History>::failure("history: " + err);
          break;
        default:
          break;  // WorkflowTask* + Unknown: id/type only
      }
    }
    h.events.push_back(std::move(e));
  }
  return mwf::Result<History>::success(std::move(h));
}

mwf::Result<History> loadHistoryJson(const mwf::Bytes& history_json) {
  return loadHistoryJson(std::string_view(
      reinterpret_cast<const char*>(history_json.data()), history_json.size()));
}

}  // namespace mwf_core

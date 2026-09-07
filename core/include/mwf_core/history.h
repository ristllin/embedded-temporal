// core: History — a lean internal model of the Temporal HistoryEvent
// sequence. NOT full protobuf: only the event types + fields the
// replay engine needs, covering everything present in the conformance
// golden histories plus the v1.1-reserved timer/signal events.
//
// Loader input format: canonical Temporal history JSON (google.protobuf JSON,
// camelCase — the tctl / Temporal-UI export shape that
// temporalio.client.WorkflowHistory.from_json round-trips):
//   {"events":[{"eventId":"1","eventType":"EVENT_TYPE_...",
//               "<type>EventAttributes":{...}}, ...]}
// Payload bytes are base64 `data` fields whose decoded content is raw JSON
// (goldens use the default json/plain converter); the model stores the decoded
// JSON text. Unknown event types load tolerantly as EventType::Unknown.
#pragma once
#include <string>
#include <vector>
#include "mwf/types.h"

namespace mwf_core {

enum class EventType {
  Unknown = 0,
  WorkflowExecutionStarted,
  WorkflowTaskScheduled,
  WorkflowTaskStarted,
  WorkflowTaskCompleted,
  ActivityTaskScheduled,
  ActivityTaskStarted,
  ActivityTaskCompleted,
  ActivityTaskFailed,
  TimerStarted,
  TimerFired,
  WorkflowExecutionCompleted,
  WorkflowExecutionFailed,
  WorkflowExecutionSignaled,
};

struct HistoryEvent {
  int64_t event_id = 0;
  EventType type = EventType::Unknown;

  // ActivityTaskScheduled
  std::string activity_name;   // activityType.name
  std::string activity_id;     // activityId

  // Back-refs: Activity Started/Completed/Failed -> their ActivityTaskScheduled;
  // TimerFired -> its TimerStarted (startedEventId).
  int64_t scheduled_event_id = 0;
  int64_t started_event_id = 0;

  // Decoded raw-JSON payload texts, in order (WorkflowExecutionStarted /
  // ActivityTaskScheduled `input`, ActivityTask/WorkflowExecution Completed
  // `result`, WorkflowExecutionSignaled `input`).
  std::vector<std::string> payloads;

  std::string failure_message; // ActivityTaskFailed / WorkflowExecutionFailed
  std::string signal_name;     // WorkflowExecutionSignaled
  std::string timer_id;        // TimerStarted / TimerFired

  // WorkflowExecutionStarted extras
  std::string workflow_type;   // workflowType.name
  std::string workflow_id;
  std::string run_id;          // originalExecutionRunId (falls back to firstExecutionRunId)
};

struct History {
  std::vector<HistoryEvent> events;

  const HistoryEvent* find(int64_t event_id) const;
  // First `n` events — the test harness's prefix oracle building block.
  History prefix(size_t n) const;
};

// Load canonical Temporal history JSON (shape above). Never throws.
mwf::Result<History> loadHistoryJson(std::string_view history_json);
mwf::Result<History> loadHistoryJson(const mwf::Bytes& history_json);

}  // namespace mwf_core

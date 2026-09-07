// core/test: History model + Temporal proto-JSON loader, fed by the REAL
// golden histories captured from a live dev server (conformance).
#include <gtest/gtest.h>

#include "mwf_core/history.h"
#include "test_util.h"

using mwf_core::EventType;
using mwf_core::History;
using mwf_core::loadHistoryJson;

TEST(History, LoadsLinearGolden) {
  History h = mwf_test::loadGolden("linear");
  ASSERT_EQ(h.events.size(), 17u);

  const auto& start = h.events[0];
  EXPECT_EQ(start.event_id, 1);
  EXPECT_EQ(start.type, EventType::WorkflowExecutionStarted);
  EXPECT_EQ(start.workflow_type, "LinearWorkflow");
  EXPECT_EQ(start.workflow_id, "linear-85597381");
  EXPECT_EQ(start.run_id, "019f7276-05a4-7f32-b62d-213a9e7b918d");
  ASSERT_EQ(start.payloads.size(), 1u);
  EXPECT_EQ(start.payloads[0], "\"world\"");  // base64 data decoded to raw JSON

  const auto& sched = h.events[4];
  EXPECT_EQ(sched.event_id, 5);
  EXPECT_EQ(sched.type, EventType::ActivityTaskScheduled);
  EXPECT_EQ(sched.activity_name, "greet");
  EXPECT_EQ(sched.activity_id, "1");
  ASSERT_EQ(sched.payloads.size(), 1u);
  EXPECT_EQ(sched.payloads[0], "\"world\"");

  const auto& done = h.events[6];
  EXPECT_EQ(done.type, EventType::ActivityTaskCompleted);
  EXPECT_EQ(done.scheduled_event_id, 5);
  EXPECT_EQ(done.started_event_id, 6);
  ASSERT_EQ(done.payloads.size(), 1u);
  EXPECT_EQ(done.payloads[0], "\"hello, world\"");

  const auto& fin = h.events[16];
  EXPECT_EQ(fin.event_id, 17);
  EXPECT_EQ(fin.type, EventType::WorkflowExecutionCompleted);
  ASSERT_EQ(fin.payloads.size(), 1u);
  EXPECT_EQ(fin.payloads[0], "\"HELLO, WORLD!\"");
}

TEST(History, GoldenEventSequenceMatchesReadme) {
  // WorkflowExecutionStarted -> (WFT S/S/C -> Activity S/S/C) x2 -> WFT S/S/C
  // -> WorkflowExecutionCompleted — the documented oracle shape, all 3 goldens.
  const EventType expected[17] = {
      EventType::WorkflowExecutionStarted,
      EventType::WorkflowTaskScheduled, EventType::WorkflowTaskStarted,
      EventType::WorkflowTaskCompleted,
      EventType::ActivityTaskScheduled, EventType::ActivityTaskStarted,
      EventType::ActivityTaskCompleted,
      EventType::WorkflowTaskScheduled, EventType::WorkflowTaskStarted,
      EventType::WorkflowTaskCompleted,
      EventType::ActivityTaskScheduled, EventType::ActivityTaskStarted,
      EventType::ActivityTaskCompleted,
      EventType::WorkflowTaskScheduled, EventType::WorkflowTaskStarted,
      EventType::WorkflowTaskCompleted,
      EventType::WorkflowExecutionCompleted,
  };
  for (const char* name : {"linear", "conditional_even", "conditional_odd"}) {
    History h = mwf_test::loadGolden(name);
    ASSERT_EQ(h.events.size(), 17u) << name;
    for (size_t i = 0; i < 17; ++i) {
      EXPECT_EQ(h.events[i].type, expected[i]) << name << " event " << i + 1;
      EXPECT_EQ(h.events[i].event_id, static_cast<int64_t>(i + 1)) << name;
    }
  }
}

TEST(History, ConditionalGoldensDivergeAtSecondActivity) {
  History even = mwf_test::loadGolden("conditional_even");
  History odd = mwf_test::loadGolden("conditional_odd");
  EXPECT_EQ(even.events[4].activity_name, "parity");
  EXPECT_EQ(odd.events[4].activity_name, "parity");
  EXPECT_EQ(even.events[6].payloads[0], "\"even\"");
  EXPECT_EQ(odd.events[6].payloads[0], "\"odd\"");
  EXPECT_EQ(even.events[10].activity_name, "even_branch");
  EXPECT_EQ(odd.events[10].activity_name, "odd_branch");
  EXPECT_EQ(even.events[10].payloads[0], "8");
  EXPECT_EQ(odd.events[10].payloads[0], "7");
}

TEST(History, FindAndPrefix) {
  History h = mwf_test::loadGolden("linear");
  ASSERT_NE(h.find(5), nullptr);
  EXPECT_EQ(h.find(5)->activity_name, "greet");
  EXPECT_EQ(h.find(99), nullptr);
  History p = h.prefix(7);
  ASSERT_EQ(p.events.size(), 7u);
  EXPECT_EQ(p.events.back().type, EventType::ActivityTaskCompleted);
  EXPECT_EQ(h.prefix(99).events.size(), 17u);  // clamped
}

TEST(History, LoaderIsTolerantAndBounded) {
  // Unknown event types load as Unknown; garbage fails cleanly.
  auto ok = loadHistoryJson(std::string_view(
      R"({"events":[{"eventId":"1","eventType":"EVENT_TYPE_SOMETHING_NEW"}]})"));
  ASSERT_TRUE(ok.ok) << ok.error;
  EXPECT_EQ(ok.value.events[0].type, EventType::Unknown);

  EXPECT_FALSE(loadHistoryJson(std::string_view("[]")).ok);
  EXPECT_FALSE(loadHistoryJson(std::string_view("{")).ok);
  EXPECT_FALSE(loadHistoryJson(std::string_view(R"({"events":[{"eventType":"x"}]})")).ok);
}

TEST(History, HostileNonStringTypeNamesFailCleanly) {
  // A forged/corrupt server response with the wrong JSON type where a type
  // name belongs must come back as a clean Result failure (the loader is
  // documented never to throw), not a nlohmann type_error.
  auto wf = loadHistoryJson(std::string_view(R"({"events":[
    {"eventId":"1","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_STARTED",
     "workflowExecutionStartedEventAttributes":{"workflowType":{"name":123}}}
  ]})"));
  EXPECT_FALSE(wf.ok);
  EXPECT_NE(wf.error.find("workflowType"), std::string::npos) << wf.error;

  auto act = loadHistoryJson(std::string_view(R"({"events":[
    {"eventId":"1","eventType":"EVENT_TYPE_ACTIVITY_TASK_SCHEDULED",
     "activityTaskScheduledEventAttributes":{"activityType":{"name":{"x":1}},
       "activityId":"1"}}
  ]})"));
  EXPECT_FALSE(act.ok);
  EXPECT_NE(act.error.find("activityType"), std::string::npos) << act.error;

  // Non-object type wrappers and non-string sibling fields stay tolerated
  // (skipped), matching the loader's existing dialect.
  auto tolerant = loadHistoryJson(std::string_view(R"({"events":[
    {"eventId":"1","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_STARTED",
     "workflowExecutionStartedEventAttributes":{"workflowType":42,"workflowId":7}}
  ]})"));
  ASSERT_TRUE(tolerant.ok) << tolerant.error;
  EXPECT_TRUE(tolerant.value.events[0].workflow_type.empty());
}

TEST(History, LoadsSignalAndFailureShapes) {
  // Synthetic events for the enum rows the goldens don't exercise.
  auto r = loadHistoryJson(std::string_view(R"({"events":[
    {"eventId":"1","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_SIGNALED",
     "workflowExecutionSignaledEventAttributes":{"signalName":"go",
       "input":{"payloads":[{"metadata":{},"data":"NDI="}]}}},
    {"eventId":"2","eventType":"EVENT_TYPE_ACTIVITY_TASK_FAILED",
     "activityTaskFailedEventAttributes":{"scheduledEventId":"1","startedEventId":"1",
       "failure":{"message":"boom"}}},
    {"eventId":"3","eventType":"EVENT_TYPE_TIMER_STARTED",
     "timerStartedEventAttributes":{"timerId":"t1"}},
    {"eventId":"4","eventType":"EVENT_TYPE_TIMER_FIRED",
     "timerFiredEventAttributes":{"timerId":"t1","startedEventId":"3"}},
    {"eventId":"5","eventType":"EVENT_TYPE_WORKFLOW_EXECUTION_FAILED",
     "workflowExecutionFailedEventAttributes":{"failure":{"message":"dead"}}}
  ]})"));
  ASSERT_TRUE(r.ok) << r.error;
  const auto& ev = r.value.events;
  EXPECT_EQ(ev[0].type, EventType::WorkflowExecutionSignaled);
  EXPECT_EQ(ev[0].signal_name, "go");
  EXPECT_EQ(ev[0].payloads[0], "42");
  EXPECT_EQ(ev[1].type, EventType::ActivityTaskFailed);
  EXPECT_EQ(ev[1].failure_message, "boom");
  EXPECT_EQ(ev[2].timer_id, "t1");
  EXPECT_EQ(ev[3].type, EventType::TimerFired);
  EXPECT_EQ(ev[3].started_event_id, 3);
  EXPECT_EQ(ev[4].failure_message, "dead");
}

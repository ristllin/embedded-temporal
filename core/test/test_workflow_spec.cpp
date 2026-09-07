// core/test: WorkflowSpec grammar parser (CONTRACTS.md §5, v1).
#include <gtest/gtest.h>

#include "mwf_core/workflow_spec.h"
#include "test_util.h"

using mwf_core::WorkflowStep;
using mwf_core::parseWorkflowSpec;

TEST(WorkflowSpec, ParsesLinearSpec) {
  auto r = parseWorkflowSpec(std::string_view(mwf_test::kLinearSpec));
  ASSERT_TRUE(r.ok) << r.error;
  const auto& spec = r.value;
  EXPECT_EQ(spec.name, "LinearWorkflow");
  ASSERT_EQ(spec.steps.size(), 3u);
  EXPECT_EQ(spec.steps[0].kind, WorkflowStep::Kind::Activity);
  EXPECT_EQ(spec.steps[0].name, "greet");
  EXPECT_EQ(spec.steps[0].id, "greet");  // id defaults to name
  EXPECT_EQ(spec.steps[0].args_from, "/input");
  EXPECT_EQ(spec.steps[1].args_from, "/results/greet");
  EXPECT_EQ(spec.steps[2].kind, WorkflowStep::Kind::Complete);
  EXPECT_EQ(spec.steps[2].result_from, "/results/shout");
  EXPECT_NE(spec.spec_hash, 0u);
}

TEST(WorkflowSpec, ParsesConditionalSpec) {
  auto r = parseWorkflowSpec(std::string_view(mwf_test::kConditionalSpec));
  ASSERT_TRUE(r.ok) << r.error;
  const auto& c = r.value.steps[1];
  ASSERT_EQ(c.kind, WorkflowStep::Kind::Conditional);
  EXPECT_EQ(c.predicate.path, "/results/parity");
  EXPECT_EQ(c.predicate.op, mwf_core::Predicate::Op::Eq);
  EXPECT_EQ(c.predicate.value, mwf::json("even"));
  ASSERT_EQ(c.true_steps.size(), 1u);
  ASSERT_EQ(c.false_steps.size(), 1u);
  EXPECT_EQ(c.true_steps[0].name, "even_branch");
  EXPECT_EQ(c.true_steps[0].id, "branch");  // explicit shared id
  EXPECT_EQ(c.false_steps[0].name, "odd_branch");
}

TEST(WorkflowSpec, SpecHashIsStableAndContentSensitive) {
  auto a = parseWorkflowSpec(std::string_view(mwf_test::kLinearSpec));
  auto b = parseWorkflowSpec(std::string_view(mwf_test::kLinearSpec));
  auto c = parseWorkflowSpec(std::string_view(mwf_test::kConditionalSpec));
  ASSERT_TRUE(a.ok && b.ok && c.ok);
  EXPECT_EQ(a.value.spec_hash, b.value.spec_hash);
  EXPECT_NE(a.value.spec_hash, c.value.spec_hash);
}

TEST(WorkflowSpec, SequenceStepAndLiterals) {
  const char* text = R"({
    "name": "seq",
    "steps": [
      {"type": "sequence", "steps": [
        {"type": "activity", "name": "a", "args": {"n": 1}},
        {"type": "complete", "result": "done"}
      ]}
    ]})";
  auto r = parseWorkflowSpec(std::string_view(text));
  ASSERT_TRUE(r.ok) << r.error;
  const auto& seq = r.value.steps[0];
  ASSERT_EQ(seq.kind, WorkflowStep::Kind::Sequence);
  ASSERT_EQ(seq.steps.size(), 2u);
  EXPECT_TRUE(seq.steps[0].has_args_literal);
  EXPECT_TRUE(seq.steps[1].has_result_literal);
}

TEST(WorkflowSpec, RejectsMalformed) {
  // not JSON
  EXPECT_FALSE(parseWorkflowSpec(std::string_view("not json")).ok);
  // missing name
  EXPECT_FALSE(parseWorkflowSpec(std::string_view(
      R"({"steps":[{"type":"complete","result":1}]})")).ok);
  // empty steps
  EXPECT_FALSE(parseWorkflowSpec(std::string_view(
      R"({"name":"x","steps":[]})")).ok);
  // unknown step type
  EXPECT_FALSE(parseWorkflowSpec(std::string_view(
      R"({"name":"x","steps":[{"type":"warp"}]})")).ok);
  // activity with both args and args_from
  EXPECT_FALSE(parseWorkflowSpec(std::string_view(
      R"({"name":"x","steps":[{"type":"activity","name":"a","args":1,"args_from":"/input"}]})")).ok);
  // activity with neither
  EXPECT_FALSE(parseWorkflowSpec(std::string_view(
      R"({"name":"x","steps":[{"type":"activity","name":"a"}]})")).ok);
  // bad predicate op
  EXPECT_FALSE(parseWorkflowSpec(std::string_view(
      R"({"name":"x","steps":[{"type":"conditional","predicate":{"path":"/a","op":"~","value":1}}]})")).ok);
  // predicate path not a pointer
  EXPECT_FALSE(parseWorkflowSpec(std::string_view(
      R"({"name":"x","steps":[{"type":"conditional","predicate":{"path":"a","op":"eq","value":1}}]})")).ok);
  // complete without a result source
  EXPECT_FALSE(parseWorkflowSpec(std::string_view(
      R"({"name":"x","steps":[{"type":"complete"}]})")).ok);
}

TEST(WorkflowSpec, ReservedV11KindsAreNamedInTheError) {
  for (const char* kind : {"parallel", "timer"}) {
    std::string text = std::string(R"({"name":"x","steps":[{"type":")") + kind + R"("}]})";
    auto r = parseWorkflowSpec(std::string_view(text));
    ASSERT_FALSE(r.ok) << kind;
    EXPECT_NE(r.error.find("reserved"), std::string::npos) << r.error;
  }
}

TEST(WorkflowSpec, ParsesWaitSignal) {
  const char* text = R"({
    "name": "WaitApproval",
    "steps": [
      {"type": "wait_signal", "signal": "approve", "id": "approval", "bind_to": "decision"},
      {"type": "complete", "result_from": "/results/decision"}
    ]})";
  auto r = parseWorkflowSpec(std::string_view(text));
  ASSERT_TRUE(r.ok) << r.error;
  const auto& w = r.value.steps[0];
  EXPECT_EQ(w.kind, WorkflowStep::Kind::WaitSignal);
  EXPECT_EQ(w.signal, "approve");
  EXPECT_EQ(w.id, "approval");
  EXPECT_EQ(w.bind_to, "decision");
}

TEST(WorkflowSpec, WaitSignalIdDefaultsToSignalName) {
  auto r = parseWorkflowSpec(std::string_view(
      R"({"name":"x","steps":[{"type":"wait_signal","signal":"go"},{"type":"complete","result":1}]})"));
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.value.steps[0].id, "go");   // id defaults to the signal name
  EXPECT_TRUE(r.value.steps[0].bind_to.empty());
}

TEST(WorkflowSpec, RejectsMalformedWaitSignal) {
  // missing signal
  EXPECT_FALSE(parseWorkflowSpec(std::string_view(
      R"({"name":"x","steps":[{"type":"wait_signal","id":"a"}]})")).ok);
  // empty signal
  EXPECT_FALSE(parseWorkflowSpec(std::string_view(
      R"({"name":"x","steps":[{"type":"wait_signal","signal":""}]})")).ok);
  // bind_to present but empty
  EXPECT_FALSE(parseWorkflowSpec(std::string_view(
      R"({"name":"x","steps":[{"type":"wait_signal","signal":"s","bind_to":""}]})")).ok);
}

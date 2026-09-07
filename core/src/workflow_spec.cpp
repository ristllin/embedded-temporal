// core: WorkflowSpec parser. Grammar in workflow_spec.h.
#include "mwf_core/workflow_spec.h"

#include <set>

#include "mwf_core/detail/json_util.h"

namespace mwf_core {
namespace {

using jsonutil::dump;

std::string stepErr(const std::string& where, const std::string& what) {
  return "spec: " + where + ": " + what;
}

bool parsePredicate(const mwf::json& j, Predicate& out, std::string& err,
                    const std::string& where) {
  if (!j.is_object()) { err = stepErr(where, "predicate must be an object"); return false; }
  if (!j.contains("path") || !j["path"].is_string()) {
    err = stepErr(where, "predicate.path must be a string"); return false;
  }
  out.path = j["path"].get<std::string>();
  if (out.path.empty() || out.path[0] != '/') {
    err = stepErr(where, "predicate.path must be a JSON Pointer starting with '/'");
    return false;
  }
  const std::string op = j.contains("op") && j["op"].is_string()
                             ? j["op"].get<std::string>() : std::string();
  if      (op == "eq")     out.op = Predicate::Op::Eq;
  else if (op == "ne")     out.op = Predicate::Op::Ne;
  else if (op == "lt")     out.op = Predicate::Op::Lt;
  else if (op == "le")     out.op = Predicate::Op::Le;
  else if (op == "gt")     out.op = Predicate::Op::Gt;
  else if (op == "ge")     out.op = Predicate::Op::Ge;
  else if (op == "exists") out.op = Predicate::Op::Exists;
  else { err = stepErr(where, "predicate.op must be one of eq|ne|lt|le|gt|ge|exists"); return false; }
  if (out.op != Predicate::Op::Exists) {
    if (!j.contains("value")) {
      err = stepErr(where, "predicate.value required for op '" + op + "'");
      return false;
    }
    out.value = j["value"];
  }
  return true;
}

bool parseSteps(const mwf::json& arr, std::vector<WorkflowStep>& out,
                std::string& err, const std::string& where);

bool parseStep(const mwf::json& j, WorkflowStep& out, std::string& err,
               const std::string& where) {
  if (!j.is_object()) { err = stepErr(where, "step must be an object"); return false; }
  if (!j.contains("type") || !j["type"].is_string()) {
    err = stepErr(where, "step.type must be a string"); return false;
  }
  const std::string type = j["type"].get<std::string>();

  if (type == "parallel" || type == "timer") {
    err = stepErr(where, "step type '" + type + "' is reserved for v1.1 — not interpreted by the v1 engine");
    return false;
  }

  if (type == "activity") {
    out.kind = WorkflowStep::Kind::Activity;
    if (!j.contains("name") || !j["name"].is_string() || j["name"].get<std::string>().empty()) {
      err = stepErr(where, "activity.name must be a non-empty string"); return false;
    }
    out.name = j["name"].get<std::string>();
    out.id = j.contains("id") && j["id"].is_string() ? j["id"].get<std::string>() : out.name;
    if (out.id.empty()) { err = stepErr(where, "activity.id must not be empty"); return false; }
    const bool hasFrom = j.contains("args_from");
    const bool hasLit  = j.contains("args");
    if (hasFrom == hasLit) {
      err = stepErr(where, "activity requires exactly one of args_from | args"); return false;
    }
    if (hasFrom) {
      if (!j["args_from"].is_string()) { err = stepErr(where, "args_from must be a string"); return false; }
      out.args_from = j["args_from"].get<std::string>();
      if (out.args_from.empty() || out.args_from[0] != '/') {
        err = stepErr(where, "args_from must be a JSON Pointer starting with '/'"); return false;
      }
    } else {
      out.has_args_literal = true;
      out.args_literal = j["args"];
    }
    return true;
  }

  if (type == "wait_signal") {
    out.kind = WorkflowStep::Kind::WaitSignal;
    if (!j.contains("signal") || !j["signal"].is_string() ||
        j["signal"].get<std::string>().empty()) {
      err = stepErr(where, "wait_signal.signal must be a non-empty string"); return false;
    }
    out.signal = j["signal"].get<std::string>();
    // id (the /results binding key) defaults to the signal name.
    out.id = j.contains("id") && j["id"].is_string() ? j["id"].get<std::string>() : out.signal;
    if (out.id.empty()) { err = stepErr(where, "wait_signal.id must not be empty"); return false; }
    if (j.contains("bind_to")) {
      if (!j["bind_to"].is_string() || j["bind_to"].get<std::string>().empty()) {
        err = stepErr(where, "wait_signal.bind_to must be a non-empty string when present");
        return false;
      }
      out.bind_to = j["bind_to"].get<std::string>();
    }
    return true;
  }

  if (type == "sequence") {
    out.kind = WorkflowStep::Kind::Sequence;
    if (!j.contains("steps")) { err = stepErr(where, "sequence.steps required"); return false; }
    return parseSteps(j["steps"], out.steps, err, where + ".steps");
  }

  if (type == "conditional") {
    out.kind = WorkflowStep::Kind::Conditional;
    if (!j.contains("predicate")) { err = stepErr(where, "conditional.predicate required"); return false; }
    if (!parsePredicate(j["predicate"], out.predicate, err, where)) return false;
    if (j.contains("true_steps") &&
        !parseSteps(j["true_steps"], out.true_steps, err, where + ".true_steps"))
      return false;
    if (j.contains("false_steps") &&
        !parseSteps(j["false_steps"], out.false_steps, err, where + ".false_steps"))
      return false;
    return true;
  }

  if (type == "complete") {
    out.kind = WorkflowStep::Kind::Complete;
    const bool hasFrom = j.contains("result_from");
    const bool hasLit  = j.contains("result");
    if (hasFrom == hasLit) {
      err = stepErr(where, "complete requires exactly one of result_from | result"); return false;
    }
    if (hasFrom) {
      if (!j["result_from"].is_string()) { err = stepErr(where, "result_from must be a string"); return false; }
      out.result_from = j["result_from"].get<std::string>();
      if (out.result_from.empty() || out.result_from[0] != '/') {
        err = stepErr(where, "result_from must be a JSON Pointer starting with '/'"); return false;
      }
    } else {
      out.has_result_literal = true;
      out.result_literal = j["result"];
    }
    return true;
  }

  err = stepErr(where, "unknown step type '" + type + "'");
  return false;
}

bool parseSteps(const mwf::json& arr, std::vector<WorkflowStep>& out,
                std::string& err, const std::string& where) {
  if (!arr.is_array()) { err = stepErr(where, "must be an array"); return false; }
  out.clear();
  out.reserve(arr.size());
  size_t i = 0;
  for (const auto& sj : arr) {
    WorkflowStep step;
    if (!parseStep(sj, step, err, where + "[" + std::to_string(i) + "]")) return false;
    out.push_back(std::move(step));
    ++i;
  }
  return true;
}

// Reject a binding id (activity/wait_signal `id`) reused on a single execution
// path — the replay walk short-circuits on `results.contains(id)`, so a repeat
// id in a straight-line sequence would bind to the first step's result and never
// advance/schedule the second (a silent misbind). `seen` accumulates ids along
// the current path (threaded through nested sequences, which all execute); a
// conditional FORKS — each branch inherits the enclosing ids but is checked
// independently, so the two branches may legitimately SHARE an id (only one
// runs). Both branches' ids then merge back so a later step reusing a branch id
// is still caught. Reads (/results/<id> refs) don't bind and aren't tracked.
bool checkPathIds(const std::vector<WorkflowStep>& steps, std::set<std::string>& seen,
                  std::string& err, const std::string& where) {
  size_t i = 0;
  for (const auto& s : steps) {
    const std::string here = where + "[" + std::to_string(i) + "]";
    ++i;
    switch (s.kind) {
      case WorkflowStep::Kind::Activity:
      case WorkflowStep::Kind::WaitSignal:
        if (!seen.insert(s.id).second) {
          err = stepErr(here, "duplicate binding id '" + s.id +
                                  "' on a single execution path (ids must be unique "
                                  "among steps that execute in one run; sharing an id "
                                  "across the two branches of a conditional is allowed)");
          return false;
        }
        break;
      case WorkflowStep::Kind::Sequence:
        if (!checkPathIds(s.steps, seen, err, here + ".steps")) return false;
        break;
      case WorkflowStep::Kind::Conditional: {
        std::set<std::string> t = seen, f = seen;
        if (!checkPathIds(s.true_steps, t, err, here + ".true_steps")) return false;
        if (!checkPathIds(s.false_steps, f, err, here + ".false_steps")) return false;
        seen.insert(t.begin(), t.end());
        seen.insert(f.begin(), f.end());
        break;
      }
      case WorkflowStep::Kind::Complete:
        break;
    }
  }
  return true;
}

}  // namespace

mwf::Result<WorkflowSpec> parseWorkflowSpec(std::string_view spec_json) {
  auto parsed = jsonutil::parse(spec_json);
  if (!parsed) return mwf::Result<WorkflowSpec>::failure("spec: " + parsed.error);
  const mwf::json& j = parsed.value;
  if (!j.is_object()) return mwf::Result<WorkflowSpec>::failure("spec: root must be an object");

  WorkflowSpec spec;
  if (!j.contains("name") || !j["name"].is_string() || j["name"].get<std::string>().empty()) {
    return mwf::Result<WorkflowSpec>::failure("spec: name must be a non-empty string");
  }
  spec.name = j["name"].get<std::string>();
  if (j.contains("input_schema"))  spec.input_schema_json  = dump(j["input_schema"]);
  if (j.contains("output_schema")) spec.output_schema_json = dump(j["output_schema"]);

  if (!j.contains("steps")) return mwf::Result<WorkflowSpec>::failure("spec: steps required");
  std::string err;
  if (!parseSteps(j["steps"], spec.steps, err, "steps")) {
    return mwf::Result<WorkflowSpec>::failure(err);
  }
  if (spec.steps.empty()) return mwf::Result<WorkflowSpec>::failure("spec: steps must be non-empty");

  // Binding ids must be unique along any single (non-branching) execution path.
  std::set<std::string> seen_ids;
  if (!checkPathIds(spec.steps, seen_ids, err, "steps")) {
    return mwf::Result<WorkflowSpec>::failure(err);
  }

  // Canonical hash pins durable state to this exact spec content.
  spec.spec_hash = jsonutil::fnv1a64(dump(j));
  return mwf::Result<WorkflowSpec>::success(std::move(spec));
}

mwf::Result<WorkflowSpec> parseWorkflowSpec(const mwf::Bytes& spec_json) {
  return parseWorkflowSpec(std::string_view(
      reinterpret_cast<const char*>(spec_json.data()), spec_json.size()));
}

}  // namespace mwf_core

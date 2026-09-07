// core: WorkflowSpec — the declarative IWorkflowSpec JSON grammar
// (CONTRACTS.md §5). A workflow is DATA, not code: the ReplayEngine interprets a
// parsed spec against a recorded Temporal event history, so replay is
// deterministic without a sandbox.
//
// ── Spec grammar (v1) ─────────────────────────────────────────────────────────
//
//   WorkflowSpec = {
//     "name":          string            (required)
//     "input_schema":  object            (optional, opaque to the engine)
//     "output_schema": object            (optional, opaque to the engine)
//     "steps":         [Step, ...]       (required, non-empty; an implicit sequence)
//   }
//
//   Step (discriminated on "type"):
//
//   activity     {"type":"activity", "name":string, "id"?:string,
//                 "args_from":pointer | "args":json}
//     Schedules the activity `name` and awaits its result. `id` (default:
//     `name`) is the key its result binds under — later steps reference it as
//     /results/<id>. Ids must be unique among the steps EXECUTED in one run;
//     the two branches of a conditional may share an id (only one executes),
//     which lets a downstream step read whichever branch ran (/results/<id>).
//     Exactly one of `args_from` (a source ref, below) or `args` (a literal
//     JSON value) supplies the single activity argument.
//
//   sequence     {"type":"sequence", "steps":[Step,...]}
//     Ordered sub-steps. Top-level "steps" is already an implicit sequence;
//     this exists for grouping inside branches.
//
//   conditional  {"type":"conditional", "predicate":Predicate,
//                 "true_steps"?:[Step,...], "false_steps"?:[Step,...]}
//     Evaluates the predicate over accumulated bindings and walks the chosen
//     branch. Falls through to the next sibling step when the branch finishes
//     without completing the workflow.
//
//   wait_signal  {"type":"wait_signal", "signal":string, "id"?:string,
//                 "bind_to"?:string}
//     Durable human-in-the-loop pause: the walk BLOCKS here (emits no completion
//     command) until a WorkflowExecutionSignaled history event whose signal_name
//     equals `signal` appears. When present, the signal's input payload binds as
//     this step's result — under /results/<id> (id defaults to `signal`) and,
//     when given, ALSO under /results/<bind_to> — and the walk continues. Purely
//     history-driven: no clock, no new worker Command (waiting is the ABSENCE of
//     a completion command), so a reboot-while-paused resumes to the same block.
//     Multiple signals of the same name bind to successive same-name wait_signal
//     steps in history order.
//
//   complete     {"type":"complete", "result_from":pointer | "result":json}
//     CompleteWorkflowExecution with the resolved JSON result.
//
//   v1.1 reserved (parse error today, names reserved): parallel, timer.
//   Deferred entirely: agent, memory_op, try_except, loop.
//
// ── Source refs + predicate grammar ───────────────────────────────────────────
//
// A source ref ("args_from" / "result_from" / predicate "path") is an RFC 6901
// JSON Pointer into the deterministic bindings document the engine accumulates:
//
//   { "input":   <workflow input argument>,          // from WorkflowExecutionStarted
//     "results": { "<step id>": <activity result>, ... } }
//
// e.g. "/input", "/results/greet", "/results/parity/kind". An unresolvable ref
// at evaluation time is a decide() error (deterministic — same spec + history
// always fails the same way).
//
//   Predicate = {"path":pointer, "op":Op, "value"?:json}
//   Op: "eq" | "ne"           — JSON deep equality vs `value`
//       "lt" | "le" | "gt" | "ge" — numeric compare (both sides numbers) or
//                                    lexicographic (both sides strings)
//       "exists"              — true iff `path` resolves (`value` ignored)
//
// Predicates are total and pure: no clocks, no randomness, no I/O — evaluation
// depends only on bound results, which come only from history events.
#pragma once
#include <string>
#include <vector>
#include "mwf/types.h"

namespace mwf_core {

struct Predicate {
  enum class Op { Eq, Ne, Lt, Le, Gt, Ge, Exists };
  std::string path;   // JSON Pointer into the bindings document
  Op op = Op::Eq;
  mwf::json value;    // comparison literal (unused for Exists)
};

struct WorkflowStep {
  enum class Kind { Activity, Sequence, Conditional, Complete, WaitSignal };
  Kind kind = Kind::Activity;

  // activity / wait_signal
  std::string id;               // binding key (activity: defaults to `name`;
                                //   wait_signal: defaults to `signal`)
  std::string name;             // activity type name
  std::string args_from;        // source ref; empty => use args literal
  bool has_args_literal = false;
  mwf::json args_literal;

  // wait_signal
  std::string signal;           // signal name this step waits for
  std::string bind_to;          // optional extra /results alias for the payload

  // sequence
  std::vector<WorkflowStep> steps;

  // conditional
  Predicate predicate;
  std::vector<WorkflowStep> true_steps;
  std::vector<WorkflowStep> false_steps;

  // complete
  std::string result_from;      // source ref; empty => use result literal
  bool has_result_literal = false;
  mwf::json result_literal;
};

struct WorkflowSpec {
  std::string name;
  std::string input_schema_json;   // compact dump; empty when absent
  std::string output_schema_json;
  std::vector<WorkflowStep> steps; // implicit top-level sequence
  uint64_t spec_hash = 0;          // FNV-1a64 of the canonical spec dump —
                                   // pins durable state to the exact spec
};

// Parse + validate a WorkflowSpec from JSON text/bytes. Rejects unknown step
// kinds, v1.1-reserved kinds (parallel/timer), missing/duplicate arg sources,
// and malformed predicates. Never throws.
mwf::Result<WorkflowSpec> parseWorkflowSpec(std::string_view spec_json);
mwf::Result<WorkflowSpec> parseWorkflowSpec(const mwf::Bytes& spec_json);

}  // namespace mwf_core

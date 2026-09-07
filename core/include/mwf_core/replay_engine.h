// core: ReplayEngine — the declarative IWorkflowSpec interpreter.
//
// Determinism by construction (CONTRACTS.md §5/§6): a workflow is DATA, not
// arbitrary C++. decide() is a PURE function of (spec, history[, prior state]) —
// no clocks, no randomness, no I/O — so the same inputs always yield the same
// Commands. The only sources of nondeterminism (activity results, and in v1.1
// clock/random) come from recorded history events, never live calls.
//
// Semantics (v1 grammar: activity + sequence + conditional + wait_signal +
// complete):
//   * activity step, history shows it Completed  -> bind result, keep walking.
//   * activity step, Scheduled/Started only      -> emit nothing (wait).
//   * activity step, not yet in history          -> emit ScheduleActivity, stop
//                                                   (v1 = one outstanding activity).
//   * activity step, history shows it Failed     -> emit FailWorkflow.
//   * conditional -> evaluate predicate over bound results, walk the branch.
//   * wait_signal -> a matching WorkflowExecutionSignaled event present? bind its
//                    payload (as this step's result) and keep walking : emit
//                    nothing and STOP (the workflow BLOCKS — no completion
//                    command until the signal arrives). Same-name signals bind to
//                    same-name wait_signal steps in history order.
//   * complete    -> emit CompleteWorkflowExecution (nothing if history already
//                    carries a terminal event).
// A spec-vs-history mismatch (recorded activity name differs from the step the
// walk reaches) is a nondeterminism error, mirroring Temporal SDK behaviour.
//
// Durable resume: decide() also returns an EngineState snapshot
// {run_id, spec_hash, last_processed_event_id, bound results (+ one in-flight
// activity)}. serializeState/restoreState move it through IDurableStore bytes;
// a restored engine continues from a history SUFFIX (events after
// last_processed_event_id are the only ones consulted; earlier ones are
// skipped) and provably emits the same commands as a from-scratch replay.
#pragma once
#include <map>
#include <string>
#include <vector>
#include "mwf/contracts.h"
#include "mwf_core/history.h"
#include "mwf_core/workflow_spec.h"

namespace mwf_core {

// Portable command struct — what the worker responds to
// RespondWorkflowTaskCompleted with (the proto layer maps these to protobuf
// Commands).
struct Command {
  enum class Kind { ScheduleActivity, CompleteWorkflow, FailWorkflow };
  Kind kind = Kind::ScheduleActivity;

  // ScheduleActivity: seq is 1-based scheduling order; activity_id on the wire
  // is std::to_string(seq) (matches SDK behaviour observed in the goldens).
  uint32_t seq = 0;
  std::string activity_name;
  std::string args_json;    // raw JSON text of the single activity argument

  std::string result_json;  // CompleteWorkflow
  std::string failure;      // FailWorkflow

  bool operator==(const Command& o) const {
    return kind == o.kind && seq == o.seq && activity_name == o.activity_name &&
           args_json == o.args_json && result_json == o.result_json &&
           failure == o.failure;
  }
  bool operator!=(const Command& o) const { return !(*this == o); }
};

// Everything needed to resume after a reboot, mid-run.
struct EngineState {
  std::string run_id;
  uint64_t spec_hash = 0;             // must match the spec on resume
  int64_t last_processed_event_id = 0;
  uint32_t next_seq = 1;              // 1 + activities scheduled so far
  bool has_input = false;
  std::string input_json;             // workflow input (raw JSON text)
  std::string results_json = "{}";    // {"<step id>": <result>, ...}
  // Per-signal-name count of signals already CLAIMED (bound) by wait_signal
  // steps the walk has REACHED across prior incarnations. Signals are collected
  // from the FULL history (not the resume suffix): a signal can advance the
  // watermark before its wait_signal step is reachable (blocked behind an
  // in-flight activity, or an earlier unsatisfied wait), so a suffix-only
  // collection would lose it. This count skips the already-claimed prefix per
  // name so each wait_signal step still binds the same signal a from-scratch
  // replay would — resume-safe without persisting the payloads twice.
  std::map<std::string, uint32_t> signal_consumed;
  // One in-flight activity (v1 = single outstanding): lets a resumed engine
  // match a suffix's ActivityTaskCompleted (whose scheduledEventId points into
  // the already-processed part) back to its spec step.
  bool has_pending = false;
  int64_t pending_scheduled_event_id = 0;
  std::string pending_step_id;
  std::string pending_activity_name;
};

struct Decision {
  std::vector<Command> commands;      // empty = nothing to do (waiting / finished)
  bool workflow_finished = false;     // history carries a terminal event
  // Human-in-the-loop pause: the walk reached a wait_signal step whose signal is
  // not yet in history, so it BLOCKED (no command emitted). Distinguishes a
  // signal-wait from an in-flight-activity wait (both have empty commands) so the
  // caller can surface "waiting for a human" and target the signal.
  bool waiting_on_signal = false;
  std::string waiting_signal_name;    // the `signal` of the blocked wait_signal step
  EngineState state;                  // snapshot for durable resume
};

class ReplayEngine {
 public:
  // Clock/random are the §6 determinism seams — reserved for the v1.1 grammar
  // (timer / random ops); the v1 walk never consults them.
  ReplayEngine(mwf::IClock& clock, mwf::IRandom& random);

  // Deterministically derive the next Commands from spec + full history.
  mwf::Result<Decision> decide(const WorkflowSpec& spec, const History& history);

  // Resume: prior state + history (full OR the suffix after
  // prior.last_processed_event_id — earlier events are skipped either way).
  mwf::Result<Decision> decide(const WorkflowSpec& spec, const History& history,
                               const EngineState& prior);

  // EngineState <-> IDurableStore bytes (JSON). Never throws.
  static mwf::Bytes serializeState(const EngineState& state);
  static mwf::Result<EngineState> restoreState(const mwf::Bytes& bytes);

  // Convenience: persist under "exec/<runId>" (CONTRACTS.md §4 key scheme).
  static bool saveState(mwf::IDurableStore& store, const EngineState& state);
  static mwf::Result<EngineState> loadState(mwf::IDurableStore& store,
                                            std::string_view run_id);

 private:
  mwf::IClock& clock_;
  mwf::IRandom& random_;
};

}  // namespace mwf_core

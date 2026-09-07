// core: ReplayEngine — deterministic spec-vs-history walk.
#include "mwf_core/replay_engine.h"

#include <map>

#include "mwf_core/detail/json_util.h"

namespace mwf_core {
namespace {

using jsonutil::dump;
using jsonutil::pointerGet;

enum class ActStatus { Scheduled, Started, Completed, Failed };

// One activity execution extracted from the considered history events, in
// scheduling order. `counted` = already reflected in next_seq (the pending
// record restored from prior state was counted in its original incarnation).
struct ActRecord {
  int64_t sched_id = 0;
  std::string name;
  ActStatus status = ActStatus::Scheduled;
  std::string result_json;
  std::string failure;
  bool counted = false;
};

// One WorkflowExecutionSignaled event from the considered history, in order.
struct SignalRecord {
  std::string name;          // signal_name
  std::string payload_json;  // first input payload ("null" if none)
};

enum class Outcome { Continue, Stop, Error };

struct WalkCtx {
  std::vector<ActRecord>* records = nullptr;
  size_t cursor = 0;
  std::vector<SignalRecord>* signals = nullptr;  // FULL-history signal events, in order
  std::map<std::string, size_t> signal_claim;    // per-name claim counter (UNBOUND
                                                 // wait_signal steps only) — see
                                                 // walkWaitSignal for why this makes
                                                 // resume index the suffix identically
  std::map<std::string, size_t> signal_base;     // per-name count of signals CLAIMED in
                                                 // prior incarnations (seed from prior
                                                 // EngineState.signal_consumed) — the
                                                 // full-history list is indexed from here
  std::map<std::string, size_t> signal_consumed; // per-name running total consumed
                                                 // (base + newly bound this walk) —
                                                 // persisted back to EngineState
  mwf::json bindings;               // {"input":..., "results":{...}}
  std::vector<Command> commands;
  uint32_t next_seq = 1;            // 1 + activities scheduled in history so far
  bool history_finished = false;    // terminal event present: suppress emission
  // wait bookkeeping (v1 single outstanding activity)
  bool has_pending = false;
  int64_t pending_sched = 0;
  std::string pending_step_id;
  std::string pending_name;
  // human-in-the-loop: set when the walk BLOCKS at a wait_signal step whose
  // signal has not yet arrived (durable pause). Lets the loop distinguish a
  // signal-wait from an in-flight-activity wait.
  bool waiting_on_signal = false;
  std::string waiting_signal_name;
  std::string error;
};

bool evalPredicate(const Predicate& p, const WalkCtx& ctx, bool& out,
                   std::string& err) {
  const mwf::json* v = pointerGet(ctx.bindings, p.path);
  if (p.op == Predicate::Op::Exists) {
    out = (v != nullptr);
    return true;
  }
  if (!v) {
    err = "predicate path '" + p.path + "' does not resolve";
    return false;
  }
  switch (p.op) {
    case Predicate::Op::Eq: out = (*v == p.value); return true;
    case Predicate::Op::Ne: out = (*v != p.value); return true;
    default: break;
  }
  int cmp;  // -1 / 0 / +1
  if (v->is_number() && p.value.is_number()) {
    // Compare as int64 when BOTH sides are integral — a double comparison loses
    // precision past 2^53 and mis-orders large ids/counters. Fall back to double
    // only when either side is fractional.
    if (v->is_number_integer() && p.value.is_number_integer()) {
      const int64_t a = v->get<int64_t>(), b = p.value.get<int64_t>();
      cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
    } else {
      const double a = v->get<double>(), b = p.value.get<double>();
      cmp = (a < b) ? -1 : (a > b) ? 1 : 0;
    }
  } else if (v->is_string() && p.value.is_string()) {
    cmp = v->get<std::string>().compare(p.value.get<std::string>());
    cmp = (cmp < 0) ? -1 : (cmp > 0) ? 1 : 0;
  } else {
    err = "predicate op on '" + p.path + "' requires two numbers or two strings";
    return false;
  }
  switch (p.op) {
    case Predicate::Op::Lt: out = cmp < 0;  return true;
    case Predicate::Op::Le: out = cmp <= 0; return true;
    case Predicate::Op::Gt: out = cmp > 0;  return true;
    case Predicate::Op::Ge: out = cmp >= 0; return true;
    default: err = "bad predicate op"; return false;
  }
}

Outcome walkSteps(const std::vector<WorkflowStep>& steps, WalkCtx& ctx);

Outcome walkActivity(const WorkflowStep& s, WalkCtx& ctx) {
  // Bound already (prior-state resume; ids are unique per executed path).
  if (ctx.bindings["results"].contains(s.id)) return Outcome::Continue;

  if (ctx.cursor < ctx.records->size()) {
    ActRecord& rec = (*ctx.records)[ctx.cursor++];
    if (rec.name != s.name) {
      ctx.error = "nondeterminism: history scheduled activity '" + rec.name +
                  "' where the spec expects '" + s.name + "'";
      return Outcome::Error;
    }
    if (!rec.counted) {
      ++ctx.next_seq;
      rec.counted = true;
    }
    switch (rec.status) {
      case ActStatus::Completed: {
        auto r = jsonutil::parse(rec.result_json);
        if (!r) {
          ctx.error = "history: result of activity '" + s.name + "' is not valid JSON";
          return Outcome::Error;
        }
        ctx.bindings["results"][s.id] = std::move(r.value);
        return Outcome::Continue;
      }
      case ActStatus::Failed: {
        if (!ctx.history_finished) {
          Command c;
          c.kind = Command::Kind::FailWorkflow;
          c.failure = "activity '" + s.name + "' failed: " + rec.failure;
          ctx.commands.push_back(std::move(c));
        }
        return Outcome::Stop;
      }
      default: {  // Scheduled / Started — in flight, wait
        ctx.has_pending = true;
        ctx.pending_sched = rec.sched_id;
        ctx.pending_step_id = s.id;
        ctx.pending_name = rec.name;
        return Outcome::Stop;
      }
    }
  }

  // Not in history yet — schedule it (unless the history is already terminal).
  if (ctx.history_finished) return Outcome::Stop;
  mwf::json args;
  if (s.has_args_literal) {
    args = s.args_literal;
  } else {
    const mwf::json* v = pointerGet(ctx.bindings, s.args_from);
    if (!v) {
      ctx.error = "activity '" + s.name + "': args_from '" + s.args_from +
                  "' does not resolve";
      return Outcome::Error;
    }
    args = *v;
  }
  Command c;
  c.kind = Command::Kind::ScheduleActivity;
  c.seq = ctx.next_seq;  // NOT incremented here: next_seq tracks history-
                         // scheduled activities; this one will appear as an
                         // ActivityTaskScheduled event and be counted then.
  c.activity_name = s.name;
  c.args_json = dump(args);
  ctx.commands.push_back(std::move(c));
  return Outcome::Stop;
}

Outcome walkWaitSignal(const WorkflowStep& s, WalkCtx& ctx) {
  // Already bound: either resumed (the signal was consumed + persisted into
  // results in a prior incarnation, whose consumption is already reflected in
  // signal_base) or an earlier signal already satisfied this step in this walk.
  // Either way, do NOT claim a new record — the base already skips it. Continue.
  if (ctx.bindings["results"].contains(s.id)) return Outcome::Continue;

  // Claim the next unclaimed signal of this name from the FULL-history list. The
  // index = per-name base (signals claimed by REACHED wait_signal steps in prior
  // incarnations, seeded from EngineState.signal_consumed) + the number of
  // PRECEDING UNBOUND same-name steps in this walk (signal_claim). Bound steps
  // short-circuit above and never claim; the base counts what they consumed
  // before the snapshot. So a resume indexes the full list exactly as a
  // from-scratch replay does. Deterministic: depends only on walk position +
  // history + the persisted per-name count, never on a live consumed set.
  const size_t idx = ctx.signal_base[s.signal] + ctx.signal_claim[s.signal]++;
  const SignalRecord* match = nullptr;
  size_t seen = 0;
  for (const auto& rec : *ctx.signals) {
    if (rec.name != s.signal) continue;
    if (seen == idx) { match = &rec; break; }
    ++seen;
  }
  if (!match) {
    // Signal not yet delivered → the workflow BLOCKS: emit no command and stop
    // here (whether or not the history is terminal). The pause is reproduced
    // purely from history (results lacks this id), so a reboot resumes to it.
    // Flag it (unless the run already terminated) so the loop reports Waiting —
    // not "awaiting in-flight activity" — and knows WHICH signal it wants.
    if (!ctx.history_finished) {
      ctx.waiting_on_signal = true;
      ctx.waiting_signal_name = s.signal;
    }
    return Outcome::Stop;
  }
  auto pv = jsonutil::parse(match->payload_json);
  if (!pv) {
    ctx.error = "history: payload of signal '" + s.signal + "' is not valid JSON";
    return Outcome::Error;
  }
  ctx.bindings["results"][s.id] = pv.value;
  if (!s.bind_to.empty()) ctx.bindings["results"][s.bind_to] = std::move(pv.value);
  // One more signal of this name is now claimed: idx is 0-based, so idx+1 is the
  // total consumed through this step (base + this walk's matches so far).
  ctx.signal_consumed[s.signal] = idx + 1;
  return Outcome::Continue;
}

Outcome walkStep(const WorkflowStep& s, WalkCtx& ctx) {
  switch (s.kind) {
    case WorkflowStep::Kind::Activity:
      return walkActivity(s, ctx);
    case WorkflowStep::Kind::WaitSignal:
      return walkWaitSignal(s, ctx);
    case WorkflowStep::Kind::Sequence:
      return walkSteps(s.steps, ctx);
    case WorkflowStep::Kind::Conditional: {
      bool taken = false;
      if (!evalPredicate(s.predicate, ctx, taken, ctx.error)) return Outcome::Error;
      return walkSteps(taken ? s.true_steps : s.false_steps, ctx);
    }
    case WorkflowStep::Kind::Complete: {
      if (ctx.history_finished) return Outcome::Stop;
      mwf::json result;
      if (s.has_result_literal) {
        result = s.result_literal;
      } else {
        const mwf::json* v = pointerGet(ctx.bindings, s.result_from);
        if (!v) {
          ctx.error = "complete: result_from '" + s.result_from + "' does not resolve";
          return Outcome::Error;
        }
        result = *v;
      }
      Command c;
      c.kind = Command::Kind::CompleteWorkflow;
      c.result_json = dump(result);
      ctx.commands.push_back(std::move(c));
      return Outcome::Stop;
    }
  }
  ctx.error = "internal: unknown step kind";
  return Outcome::Error;
}

Outcome walkSteps(const std::vector<WorkflowStep>& steps, WalkCtx& ctx) {
  for (const auto& s : steps) {
    const Outcome oc = walkStep(s, ctx);
    if (oc != Outcome::Continue) return oc;
  }
  return Outcome::Continue;
}

mwf::Result<Decision> decideImpl(const WorkflowSpec& spec, const History& history,
                                 const EngineState* prior) {
  using R = mwf::Result<Decision>;
  EngineState st = prior ? *prior : EngineState{};
  if (prior && prior->spec_hash != spec.spec_hash) {
    return R::failure("resume: state was recorded for a different spec (hash mismatch)");
  }

  // Signals are collected from the FULL history, independent of the resume
  // watermark. A signal can advance the watermark (be "seen") before its
  // wait_signal step is reachable — blocked behind an in-flight activity, or an
  // earlier unsatisfied wait — so a suffix-only collection would drop it across
  // resume and hang a workflow a from-scratch replay would complete. The
  // per-name consumed count in EngineState (seeded into signal_base below) skips
  // the prefix already claimed by reached wait_signal steps in prior
  // incarnations, so each step still binds the same signal a fresh replay would.
  std::vector<SignalRecord> signals;
  for (const auto& e : history.events) {
    if (e.type != EventType::WorkflowExecutionSignaled) continue;
    SignalRecord sr;
    sr.name = e.signal_name;
    sr.payload_json = e.payloads.empty() ? "null" : e.payloads[0];
    signals.push_back(std::move(sr));
  }

  // Extract activity records + workflow metadata from the considered events
  // (those after the prior state's last_processed_event_id).
  std::vector<ActRecord> records;
  std::map<int64_t, size_t> by_sched;
  if (st.has_pending) {
    ActRecord rec;
    rec.sched_id = st.pending_scheduled_event_id;
    rec.name = st.pending_activity_name;
    rec.counted = true;  // counted in the incarnation that consumed its Scheduled event
    by_sched[rec.sched_id] = records.size();
    records.push_back(std::move(rec));
  }

  bool finished = false;
  int64_t max_id = st.last_processed_event_id;
  for (const auto& e : history.events) {
    if (e.event_id <= st.last_processed_event_id) continue;  // suffix semantics
    if (e.event_id > max_id) max_id = e.event_id;
    switch (e.type) {
      case EventType::WorkflowExecutionStarted:
        if (!st.run_id.empty() && !e.run_id.empty() && st.run_id != e.run_id) {
          return R::failure("resume: history run id '" + e.run_id +
                            "' does not match state run id '" + st.run_id + "'");
        }
        if (st.run_id.empty()) st.run_id = e.run_id;
        if (!e.payloads.empty()) {
          st.has_input = true;
          st.input_json = e.payloads[0];
        }
        break;
      case EventType::ActivityTaskScheduled: {
        by_sched[e.event_id] = records.size();
        ActRecord rec;
        rec.sched_id = e.event_id;
        rec.name = e.activity_name;
        records.push_back(std::move(rec));
        break;
      }
      case EventType::ActivityTaskStarted: {
        auto it = by_sched.find(e.scheduled_event_id);
        if (it != by_sched.end() && records[it->second].status == ActStatus::Scheduled) {
          records[it->second].status = ActStatus::Started;
        }
        break;
      }
      case EventType::ActivityTaskCompleted: {
        auto it = by_sched.find(e.scheduled_event_id);
        if (it == by_sched.end()) {
          return R::failure("history: ActivityTaskCompleted references unknown scheduledEventId " +
                            std::to_string(e.scheduled_event_id));
        }
        records[it->second].status = ActStatus::Completed;
        records[it->second].result_json = e.payloads.empty() ? "null" : e.payloads[0];
        break;
      }
      case EventType::ActivityTaskFailed: {
        auto it = by_sched.find(e.scheduled_event_id);
        if (it == by_sched.end()) {
          return R::failure("history: ActivityTaskFailed references unknown scheduledEventId " +
                            std::to_string(e.scheduled_event_id));
        }
        records[it->second].status = ActStatus::Failed;
        records[it->second].failure = e.failure_message;
        break;
      }
      // WorkflowExecutionSignaled is collected from the FULL history above (not
      // gated on the watermark), so it is intentionally NOT handled here. The
      // pre-switch max_id bump still advances the watermark past a new signal.
      case EventType::WorkflowExecutionCompleted:
      case EventType::WorkflowExecutionFailed:
        finished = true;
        break;
      default:
        break;  // WorkflowTask* bookkeeping, timers (v1.1) — no-ops in v1
    }
  }

  // Bindings document: {"input": ..., "results": {...}}.
  WalkCtx ctx;
  ctx.records = &records;
  ctx.signals = &signals;
  ctx.bindings = mwf::json::object();
  if (st.has_input) {
    auto in = jsonutil::parse(st.input_json);
    if (!in) return R::failure("history: workflow input is not valid JSON");
    ctx.bindings["input"] = std::move(in.value);
  }
  auto results = jsonutil::parse(st.results_json);
  if (!results || !results.value.is_object()) {
    return R::failure("state: results is not a JSON object");
  }
  ctx.bindings["results"] = std::move(results.value);
  ctx.next_seq = st.next_seq;
  ctx.history_finished = finished;
  // Seed the per-name signal offsets from prior state. signal_base is the count
  // claimed before this incarnation (used to index the full-history list past
  // the already-consumed prefix); signal_consumed starts as a copy so names with
  // no new claim this walk keep their prior total when persisted.
  for (const auto& kv : st.signal_consumed) {
    ctx.signal_base[kv.first] = kv.second;
    ctx.signal_consumed[kv.first] = kv.second;
  }

  const Outcome oc = walkSteps(spec.steps, ctx);
  if (oc == Outcome::Error) return R::failure(ctx.error);
  if (oc == Outcome::Continue) {
    return R::failure("spec: execution path ended without a complete step");
  }

  Decision d;
  d.commands = std::move(ctx.commands);
  d.workflow_finished = finished;
  d.waiting_on_signal = ctx.waiting_on_signal;
  d.waiting_signal_name = std::move(ctx.waiting_signal_name);
  d.state.run_id = st.run_id;
  d.state.spec_hash = spec.spec_hash;
  d.state.last_processed_event_id = max_id;
  d.state.next_seq = ctx.next_seq;
  d.state.has_input = st.has_input;
  d.state.input_json = st.input_json;
  d.state.results_json = dump(ctx.bindings["results"]);
  for (const auto& kv : ctx.signal_consumed) {
    if (kv.second > 0) d.state.signal_consumed[kv.first] = static_cast<uint32_t>(kv.second);
  }
  d.state.has_pending = ctx.has_pending;
  d.state.pending_scheduled_event_id = ctx.pending_sched;
  d.state.pending_step_id = ctx.pending_step_id;
  d.state.pending_activity_name = ctx.pending_name;
  return R::success(std::move(d));
}

}  // namespace

ReplayEngine::ReplayEngine(mwf::IClock& clock, mwf::IRandom& random)
    : clock_(clock), random_(random) {
  (void)clock_;   // §6 seams, consulted only by the v1.1 grammar (timer/random)
  (void)random_;
}

mwf::Result<Decision> ReplayEngine::decide(const WorkflowSpec& spec,
                                           const History& history) {
  return decideImpl(spec, history, nullptr);
}

mwf::Result<Decision> ReplayEngine::decide(const WorkflowSpec& spec,
                                           const History& history,
                                           const EngineState& prior) {
  return decideImpl(spec, history, &prior);
}

mwf::Bytes ReplayEngine::serializeState(const EngineState& s) {
  mwf::json j = mwf::json::object();
  j["runId"] = s.run_id;
  j["specHash"] = s.spec_hash;
  j["lastProcessedEventId"] = s.last_processed_event_id;
  j["nextSeq"] = s.next_seq;
  j["hasInput"] = s.has_input;
  j["inputJson"] = s.input_json;
  j["resultsJson"] = s.results_json;
  if (!s.signal_consumed.empty()) {
    mwf::json sc = mwf::json::object();
    for (const auto& kv : s.signal_consumed) sc[kv.first] = kv.second;
    j["signalConsumed"] = std::move(sc);
  }
  if (s.has_pending) {
    mwf::json p = mwf::json::object();
    p["scheduledEventId"] = s.pending_scheduled_event_id;
    p["stepId"] = s.pending_step_id;
    p["activityName"] = s.pending_activity_name;
    j["pending"] = std::move(p);
  }
  const std::string text = jsonutil::dump(j);
  return mwf::Bytes(text.begin(), text.end());
}

mwf::Result<EngineState> ReplayEngine::restoreState(const mwf::Bytes& bytes) {
  using R = mwf::Result<EngineState>;
  auto parsed = jsonutil::parse(bytes);
  if (!parsed) return R::failure("state: " + parsed.error);
  const mwf::json& j = parsed.value;
  if (!j.is_object()) return R::failure("state: root must be an object");
  EngineState s;
  // Type-check every field, not just presence: a corrupt/foreign state blob
  // (wrong-typed flash bytes) must return a clean failure — restoreState is
  // "Never throws" (header §), and a from-scratch fallback depends on that. A
  // bare .get<T>() on a mistyped node throws nlohmann type_error, which would
  // escape loadState → WorkflowLoop::runOnce and abort() on -fno-exceptions.
  if (!j.contains("runId") || !j["runId"].is_string() ||
      !j.contains("specHash") || !j["specHash"].is_number() ||
      !j.contains("lastProcessedEventId") || !j["lastProcessedEventId"].is_number() ||
      !j.contains("nextSeq") || !j["nextSeq"].is_number() ||
      !j.contains("resultsJson") || !j["resultsJson"].is_string()) {
    return R::failure("state: missing or mistyped required fields");
  }
  s.run_id = j["runId"].get<std::string>();
  s.spec_hash = j["specHash"].get<uint64_t>();
  s.last_processed_event_id = j["lastProcessedEventId"].get<int64_t>();
  s.next_seq = j["nextSeq"].get<uint32_t>();
  s.has_input = j.contains("hasInput") && j["hasInput"].is_boolean() &&
                j["hasInput"].get<bool>();
  if (j.contains("inputJson") && j["inputJson"].is_string()) {
    s.input_json = j["inputJson"].get<std::string>();
  }
  s.results_json = j["resultsJson"].get<std::string>();
  if (j.contains("signalConsumed") && j["signalConsumed"].is_object()) {
    for (auto it = j["signalConsumed"].begin(); it != j["signalConsumed"].end(); ++it) {
      if (it.value().is_number()) s.signal_consumed[it.key()] = it.value().get<uint32_t>();
    }
  }
  if (j.contains("pending") && j["pending"].is_object()) {
    const mwf::json& p = j["pending"];
    if (!p.contains("scheduledEventId") || !p["scheduledEventId"].is_number() ||
        !p.contains("stepId") || !p["stepId"].is_string() ||
        !p.contains("activityName") || !p["activityName"].is_string()) {
      return R::failure("state: malformed pending record");
    }
    s.has_pending = true;
    s.pending_scheduled_event_id = p["scheduledEventId"].get<int64_t>();
    s.pending_step_id = p["stepId"].get<std::string>();
    s.pending_activity_name = p["activityName"].get<std::string>();
  }
  return R::success(std::move(s));
}

bool ReplayEngine::saveState(mwf::IDurableStore& store, const EngineState& state) {
  if (state.run_id.empty()) return false;
  return store.put("exec/" + state.run_id, serializeState(state));
}

mwf::Result<EngineState> ReplayEngine::loadState(mwf::IDurableStore& store,
                                                 std::string_view run_id) {
  auto bytes = store.get("exec/" + std::string(run_id));
  if (!bytes) {
    return mwf::Result<EngineState>::failure("state: no record for run '" +
                                             std::string(run_id) + "'");
  }
  return restoreState(*bytes);
}

}  // namespace mwf_core

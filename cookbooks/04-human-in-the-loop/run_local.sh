#!/usr/bin/env bash
# cookbooks/04-human-in-the-loop — durable human-in-the-loop, desktop path.
#
# Our C++ cookbook_worker registers a workflow that PAUSES on a signal
# (echo -> wait_signal("approve") -> echo -> complete), triggers it, PROVES it
# is paused (status RUNNING, no completion — the worker is blocked on the
# wait_signal), then delivers the signal over REST
# (POST /v1/workflows/executions/{id}/signals {"name","input"}) and PROVES it
# then reaches COMPLETED, carrying the signal payload through to the result.
# NO Python / mistralai-workflows — the desktop orchestrates it itself.
#
#   ./run_local.sh
#
# Secrets: MISTRAL_API_KEY from the env or a local .env file (never printed).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
TRANSPORT="$REPO/transport"
BUILD="$TRANSPORT/build"
WORKER="$BUILD/cookbook_worker"
SPEC="$HERE/spec.json"
EV="$HERE/evidence"

# ── secrets ──────────────────────────────────────────────────────────────────
# MISTRAL_API_KEY from the env, else MWF_ENV_FILE, else <repo-root>/.env
# (same dialect as probe/_common.py: optional export prefix, optional quotes).
if [[ -z "${MISTRAL_API_KEY:-}" ]]; then
  ENV_FILE="${MWF_ENV_FILE:-}"
  if [[ -z "$ENV_FILE" ]]; then
    ROOT="$(git -C "$HERE" rev-parse --show-toplevel 2>/dev/null || true)"
    ENV_FILE="${ROOT:-$REPO}/.env"   # no git (release tarball) → script-relative root
  fi
  if [[ -f "$ENV_FILE" ]]; then
    MISTRAL_API_KEY="$(grep -E '^(export[[:space:]]+)?MISTRAL_API_KEY=' "$ENV_FILE" | head -1 | cut -d= -f2- | tr -d '"'"'"' ' || true)"
  fi
fi
if [[ -z "${MISTRAL_API_KEY:-}" ]]; then
  echo "MISTRAL_API_KEY is not set and was not found in ${ENV_FILE:-<repo>/.env}." >&2
  echo "Export it, or put MISTRAL_API_KEY=... in a .env file at the repo root." >&2
  exit 2
fi
export MISTRAL_API_KEY

mkdir -p "$EV"
STAMP="$(date +%s)"
WF="cookbook04_hitl_$STAMP"
QUEUE="cookbook04-hitl-$STAMP"
INPUT='{"request":"deploy release v2 to production","by":"cookbook-04"}'
SIGNAL_INPUT='{"approved":true,"by":"desktop"}'

# ── build ────────────────────────────────────────────────────────────────────
echo "[cb04] building cookbook_worker ..."
[[ -f "$REPO/proto/gen/host/mwf/worker_service.pb.cc" ]] || "$REPO/proto/gen_host.sh" >/dev/null
[[ -x "$WORKER" ]] || cmake -S "$TRANSPORT" -B "$BUILD" >/dev/null
cmake --build "$BUILD" --target cookbook_worker >/dev/null
[[ -x "$WORKER" ]] || { echo "[cb04] build failed: $WORKER missing" >&2; exit 2; }
echo "[cb04] workflow=$WF queue=$QUEUE"

WORKER_PID=""
cleanup() {
  local rc=$?
  echo "[cb04] cleanup ..."
  [[ -n "$WORKER_PID" ]] && kill "$WORKER_PID" 2>/dev/null || true
  local exid=""
  [[ -f "$EV/hitl.json" ]] && exid="$(sed -n 's/.*"execution_id":"\([0-9a-f]*\)".*/\1/p' "$EV/hitl.json" | head -1)"
  "$WORKER" --mode cleanup --workflow "$WF" --timeout-s 45 ${exid:+--exec-id "$exid"} \
    >>"$EV/cleanup.log" 2>&1 || true
  wait 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT

# ── worker: register + run both loops until the workflow COMPLETES ────────────
# max-seconds must comfortably span the pause-observe window + signal round-trip.
echo "[cb04] starting worker (register + both loops) ..."
"$WORKER" --mode worker --spec "$SPEC" --workflow "$WF" --queue "$QUEUE" \
  --max-completions 1 --poll-ms 4000 --max-seconds 200 \
  >"$EV/worker.log" 2>&1 &
WORKER_PID=$!
echo "[cb04] worker pid=$WORKER_PID (log: evidence/worker.log)"

# ── hitl client: trigger -> prove paused -> signal -> prove COMPLETED ─────────
echo "[cb04] triggering + running the human-in-the-loop dance ..."
set +e
"$WORKER" --mode hitl --workflow "$WF" --queue "$QUEUE" \
  --input-json "$INPUT" --signal-name approve --signal-input "$SIGNAL_INPUT" \
  --pause-observe-s 8 --expect '"approved":true' \
  --timeout-s 180 --evidence "$EV/hitl.json" | tee "$EV/hitl.log"
HITL_RC=${PIPESTATUS[0]}
set -e

wait "$WORKER_PID" 2>/dev/null && WORKER_RC=0 || WORKER_RC=$?
WORKER_PID=""

EXEC_ID="$(sed -n 's/.*"execution_id":"\([0-9a-f]*\)".*/\1/p' "$EV/hitl.json" | head -1)"

echo "[cb04] ===== VERDICT ====="
echo "[cb04] execution_id=$EXEC_ID hitl_rc=$HITL_RC worker_rc=$WORKER_RC"

FAIL=0
[[ "$HITL_RC" -eq 0 ]]   || { echo "[cb04] FAIL: hitl did not reach COMPLETED after the signal"; FAIL=1; }
[[ "$WORKER_RC" -eq 0 ]] || { echo "[cb04] FAIL: worker did not observe completion"; FAIL=1; }
# The pause was observed, the signal payload flowed to the result, and the echo ran.
grep -q '"paused_confirmed":true'  "$EV/hitl.json" || { echo "[cb04] FAIL: pause not confirmed"; FAIL=1; }
grep -q '"approved":true'          "$EV/hitl.json" || { echo "[cb04] FAIL: signal payload not in result"; FAIL=1; }
grep -q '"engine":"mwf-cpp"'       "$EV/hitl.json" || { echo "[cb04] FAIL: echo brand missing from result"; FAIL=1; }
# The worker's command stream: schedule, (pause), schedule, complete.
grep -q 'command_stream #1=\[SCHEDULE_ACTIVITY_TASK,SCHEDULE_ACTIVITY_TASK,COMPLETE_WORKFLOW_EXECUTION\]' "$EV/worker.log" \
  || { echo "[cb04] WARN: command stream not schedule,schedule,complete (see worker.log)"; }
# The worker logged the durable pause on the wait_signal.
grep -q "waiting for signal 'approve'" "$EV/worker.log" \
  || { echo "[cb04] WARN: worker did not log the wait_signal pause (see worker.log)"; }

if [[ "$FAIL" -eq 0 ]]; then
  echo "[cb04] PASS (execution_id=$EXEC_ID)"
  exit 0
fi
echo "[cb04] FAIL"
exit 1

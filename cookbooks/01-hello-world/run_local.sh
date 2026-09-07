#!/usr/bin/env bash
# cookbooks/01-hello-world — the simplest end-to-end: our C++ cookbook_worker
# registers a one-activity workflow (echo → complete) on real Mistral Workflows,
# triggers it, polls to COMPLETED, asserts the echo came back, and archives.
# NO Python / mistralai-workflows anywhere — the desktop orchestrates it itself.
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
WF="cookbook01_hello_$STAMP"
QUEUE="cookbook01-hello-$STAMP"
INPUT='{"greeting":"hello-from-cookbook-01","name":"world"}'

# ── build ────────────────────────────────────────────────────────────────────
echo "[cb01] building cookbook_worker ..."
[[ -f "$REPO/proto/gen/host/mwf/worker_service.pb.cc" ]] || "$REPO/proto/gen_host.sh" >/dev/null
[[ -x "$WORKER" ]] || cmake -S "$TRANSPORT" -B "$BUILD" >/dev/null
cmake --build "$BUILD" --target cookbook_worker >/dev/null
[[ -x "$WORKER" ]] || { echo "[cb01] build failed: $WORKER missing" >&2; exit 2; }
echo "[cb01] workflow=$WF queue=$QUEUE"

WORKER_PID=""
cleanup() {
  local rc=$?
  echo "[cb01] cleanup ..."
  [[ -n "$WORKER_PID" ]] && kill "$WORKER_PID" 2>/dev/null || true
  local exid=""
  [[ -f "$EV/trigger.json" ]] && exid="$(sed -n 's/.*"execution_id":"\([0-9a-f]*\)".*/\1/p' "$EV/trigger.json" | head -1)"
  "$WORKER" --mode cleanup --workflow "$WF" --timeout-s 45 ${exid:+--exec-id "$exid"} \
    >>"$EV/cleanup.log" 2>&1 || true
  wait 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT

# ── worker: register + run both loops (backgrounded) ─────────────────────────
echo "[cb01] starting worker (register + both loops) ..."
"$WORKER" --mode worker --spec "$SPEC" --workflow "$WF" --queue "$QUEUE" \
  --max-completions 1 --poll-ms 4000 --max-seconds 150 \
  >"$EV/worker.log" 2>&1 &
WORKER_PID=$!
echo "[cb01] worker pid=$WORKER_PID (log: evidence/worker.log)"

# ── trigger + verify ─────────────────────────────────────────────────────────
echo "[cb01] triggering with input: $INPUT"
set +e
"$WORKER" --mode trigger --workflow "$WF" --queue "$QUEUE" \
  --input-json "$INPUT" --expect "mwf-cpp" \
  --timeout-s 150 --evidence "$EV/trigger.json" | tee "$EV/trigger.log"
TRIG_RC=${PIPESTATUS[0]}
set -e

wait "$WORKER_PID" 2>/dev/null && WORKER_RC=0 || WORKER_RC=$?
WORKER_PID=""

EXEC_ID="$(sed -n 's/.*"execution_id":"\([0-9a-f]*\)".*/\1/p' "$EV/trigger.json" | head -1)"
CMD="$(grep -o 'command_stream #1=\[[^]]*\]' "$EV/worker.log" | tail -1)"

echo "[cb01] ===== VERDICT ====="
echo "[cb01] execution_id=$EXEC_ID trigger_rc=$TRIG_RC worker_rc=$WORKER_RC"
echo "[cb01] $CMD"

FAIL=0
[[ "$TRIG_RC" -eq 0 ]]   || { echo "[cb01] FAIL: trigger did not reach COMPLETED"; FAIL=1; }
[[ "$WORKER_RC" -eq 0 ]] || { echo "[cb01] FAIL: worker did not observe completion"; FAIL=1; }
# The echo activity ran (brand present) AND the input round-tripped in the result.
grep -q '"engine":"mwf-cpp"'         "$EV/trigger.json" || { echo "[cb01] FAIL: echo brand missing"; FAIL=1; }
grep -q 'hello-from-cookbook-01'     "$EV/trigger.json" || { echo "[cb01] FAIL: input not echoed back"; FAIL=1; }
grep -q 'command_stream #1=\[SCHEDULE_ACTIVITY_TASK,COMPLETE_WORKFLOW_EXECUTION\]' "$EV/worker.log" \
  || { echo "[cb01] FAIL: command stream not ScheduleActivity->CompleteWorkflow"; FAIL=1; }

if [[ "$FAIL" -eq 0 ]]; then
  echo "[cb01] PASS (execution_id=$EXEC_ID)"
  exit 0
fi
echo "[cb01] FAIL"
exit 1

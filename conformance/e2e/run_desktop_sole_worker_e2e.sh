#!/usr/bin/env bash
# run_desktop_sole_worker_e2e.sh — the DESKTOP SOLE-WORKER gate, live vs
# real Mistral. Proves our REAL components (WorkflowLoop + WorkerLoop + codec +
# transport) drive a whole Mistral workflow to COMPLETED as the SOLE worker —
# the desktop orchestrates the workflow ITSELF, NO Python / mistralai-workflows
# host anywhere. This is the de-risk gate before the ESP32 integration.
#
# Flow (100% C++ + curl; ZERO python worker):
#   1. build mwf_sole_worker (transport)
#   2. `--mode worker`  : whoami → REST-register the workflow (self-registration)
#                         → run BOTH loops round-robin on ONE queue until the
#                         workflow COMPLETES. Backgrounded; log → evidence.
#   3. `--mode trigger` : wait active → REST execute (task_queue = worker queue)
#                         → poll GET /executions/{id} to COMPLETED → assert the
#                         result carries the device.echo "desktop-sole" brand.
#   4. server-of-record : the run appears in GET /v1/workflows/runs.
#   5. anti-python gate  : assert NO mistralai-workflows / python worker process
#                          is involved (the whole point).
#   6. cleanup           : archive the deployment, terminate any leftover run.
#
# Secrets: MISTRAL_API_KEY from the env or a local .env file (never printed).
# Evidence → e2e/evidence/sole_*.{json,log}.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
TRANSPORT="${MWF_TRANSPORT_DIR:-$REPO/../transport}"
BUILD="$TRANSPORT/build"
SOLE="$BUILD/mwf_sole_worker"
EV="$REPO/e2e/evidence"
BASE="${MISTRAL_BASE_URL:-https://api.mistral.ai}"

STAMP="$(date +%s)"
WF="${MWF_SOLE_WORKFLOW:-mwf_sole_desktop_$STAMP}"
QUEUE="${MWF_SOLE_QUEUE:-mwf-sole-desktop-$STAMP}"
INPUT_NAME="${MWF_SOLE_INPUT:-nimbus}"

# ── secrets ──────────────────────────────────────────────────────────────────
# MISTRAL_API_KEY from the env, else MWF_ENV_FILE, else <repo-root>/.env
# (same dialect as probe/_common.py: optional export prefix, optional quotes).
if [[ -z "${MISTRAL_API_KEY:-}" ]]; then
  ENV_FILE="${MWF_ENV_FILE:-}"
  if [[ -z "$ENV_FILE" ]]; then
    ROOT="$(git -C "$REPO" rev-parse --show-toplevel 2>/dev/null || true)"
    ENV_FILE="${ROOT:-$REPO/..}/.env"   # no git (release tarball) → script-relative root
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

# ── 1. build ─────────────────────────────────────────────────────────────────
echo "[sole-e2e] building mwf_sole_worker ..."
[[ -f "$TRANSPORT/../proto/gen/host/mwf/worker_service.pb.cc" ]] || "$TRANSPORT/../proto/gen_host.sh" >/dev/null
if [[ ! -x "$SOLE" ]]; then
  cmake -S "$TRANSPORT" -B "$BUILD" >/dev/null
fi
cmake --build "$BUILD" --target mwf_sole_worker >/dev/null
[[ -x "$SOLE" ]] || { echo "[sole-e2e] build failed: $SOLE missing" >&2; exit 2; }
echo "[sole-e2e] workflow=$WF queue=$QUEUE"

WORKER_PID=""
cleanup() {
  local rc=$?
  echo "[sole-e2e] cleanup ..."
  [[ -n "$WORKER_PID" ]] && kill "$WORKER_PID" 2>/dev/null || true
  # Archive the deployment + terminate any leftover run (best-effort).
  local exid=""
  [[ -f "$EV/sole_trigger.json" ]] && exid="$(sed -n 's/.*"execution_id":"\([0-9a-f]*\)".*/\1/p' "$EV/sole_trigger.json" | head -1)"
  "$SOLE" --mode cleanup --api-key-env MISTRAL_API_KEY \
      --workflow "$WF" --timeout-s 45 ${exid:+--exec-id "$exid"} >"$EV/sole_cleanup.log" 2>&1 || true
  wait 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT

# ── 2. worker: register + run both loops (backgrounded) ──────────────────────
echo "[sole-e2e] starting SOLE worker (register + both loops) ..."
"$SOLE" --mode worker --api-key-env MISTRAL_API_KEY \
    --workflow "$WF" --task-queue "$QUEUE" --identity "mwf-sole@desktop" \
    --poll-ms 10000 --max-seconds 150 \
    >"$EV/sole_worker.log" 2>&1 &
WORKER_PID=$!
echo "[sole-e2e] worker pid=$WORKER_PID (log: e2e/evidence/sole_worker.log)"

# ── 3. trigger + verify (server-side, C++ REST) ──────────────────────────────
echo "[sole-e2e] triggering + verifying ..."
set +e
"$SOLE" --mode trigger --api-key-env MISTRAL_API_KEY \
    --workflow "$WF" --task-queue "$QUEUE" \
    --input-name "$INPUT_NAME" --expect "desktop-sole" \
    --timeout-s 150 --evidence "$EV/sole_trigger.json" \
    | tee "$EV/sole_trigger.log"
TRIG_RC=${PIPESTATUS[0]}
set -e

EXEC_ID="$(sed -n 's/.*"execution_id":"\([0-9a-f]*\)".*/\1/p' "$EV/sole_trigger.json" | head -1)"
echo "[sole-e2e] execution_id=$EXEC_ID trigger_rc=$TRIG_RC"

# ── 4. server-of-record: the run is listed in /v1/workflows/runs ─────────────
RUNS="$(curl -sS -H "Authorization: Bearer $MISTRAL_API_KEY" "$BASE/v1/workflows/runs?limit=50")"
if grep -q "$EXEC_ID" <<<"$RUNS"; then
  echo "[sole-e2e] server-of-record: execution listed in /v1/workflows/runs ✓"
  LISTED=1
else
  echo "[sole-e2e] server-of-record: execution NOT in /v1/workflows/runs ✗"
  LISTED=0
fi

# ── 5. anti-python gate: no mistralai-workflows / python worker involved ─────
if pgrep -fl 'mistralai|live_worker\.py|run_worker' | grep -v grep >/dev/null 2>&1; then
  echo "[sole-e2e] ANTI-PYTHON GATE FAILED: a mistralai/python worker is running ✗"
  pgrep -fl 'mistralai|live_worker\.py|run_worker' || true
  PY_CLEAN=0
else
  echo "[sole-e2e] anti-python gate: no mistralai-workflows/python worker involved ✓"
  PY_CLEAN=1
fi

# ── 6. wait for the worker to observe completion + emit its command stream ───
wait "$WORKER_PID" 2>/dev/null && WORKER_RC=0 || WORKER_RC=$?
WORKER_PID=""
echo "[sole-e2e] worker exited rc=$WORKER_RC"
echo "[sole-e2e] worker command stream:"
grep -E 'command_stream|WORKER SUMMARY|activity_ran' "$EV/sole_worker.log" || true

# ── verdict ──────────────────────────────────────────────────────────────────
echo "[sole-e2e] ===== VERDICT ====="
CMD_LINE="$(grep -o 'command_stream=\[[^]]*\]' "$EV/sole_worker.log" | tail -1)"
echo "[sole-e2e] $CMD_LINE"
FAIL=0
[[ "$TRIG_RC" -eq 0 ]]   || { echo "[sole-e2e] FAIL: trigger did not reach COMPLETED with desktop-sole"; FAIL=1; }
[[ "$WORKER_RC" -eq 0 ]] || { echo "[sole-e2e] FAIL: worker did not observe workflow completion"; FAIL=1; }
[[ "$LISTED" -eq 1 ]]    || { echo "[sole-e2e] FAIL: not server-of-record"; FAIL=1; }
[[ "$PY_CLEAN" -eq 1 ]]  || { echo "[sole-e2e] FAIL: python worker involved"; FAIL=1; }
grep -q 'command_stream=\[SCHEDULE_ACTIVITY_TASK,COMPLETE_WORKFLOW_EXECUTION\]' "$EV/sole_worker.log" \
  || { echo "[sole-e2e] FAIL: command stream not ScheduleActivity→CompleteWorkflow"; FAIL=1; }

if [[ "$FAIL" -eq 0 ]]; then
  echo "[sole-e2e] DESKTOP SOLE-WORKER GATE: PASSED ✓ (execution_id=$EXEC_ID)"
  exit 0
fi
echo "[sole-e2e] DESKTOP SOLE-WORKER GATE: FAILED ✗"
exit 1

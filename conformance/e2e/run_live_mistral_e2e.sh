#!/usr/bin/env bash
# run_live_mistral_e2e.sh — the pre-hardware live gate, against real Mistral.
#
# Two server-side executions, both verified via GET /v1/workflows/executions +
# the /v1/workflows/runs listing:
#
#   E2E-1  mwf_e2e_py        Python worker executes the activity.
#   E2E-2  mwf_e2e_cpp_live  Python worker hosts the workflow SHELL only; the
#                            activity is scheduled onto task queue mwf-cpp-live,
#                            polled exclusively by the C++ mwf_worker binary
#                            (transport) against wf-scheduler.mistral.ai:443.
#                            The run result must carry the "from C++" brand.
#
# Secrets: MISTRAL_API_KEY from the environment or a local .env file
# (never printed). Evidence lands in e2e/evidence/ (ids + statuses + the
# control-plane HTTP trace + the C++ worker's wire/token log).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
PY="${WFPY:-$REPO/.venv/bin/python}"
CPP_WORKER="${MWF_CPP_WORKER:-$REPO/../transport/build/mwf_worker}"
EV="$REPO/e2e/evidence"
CPP_QUEUE="${MWF_CPP_TASK_QUEUE:-mwf-cpp-live}"
DEPLOYMENT="${DEPLOYMENT_NAME:-mwf-e2e}"

# ── secrets ────────────────────────────────────────────────────────────────── #
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

# Build only when using the default path — an MWF_CPP_WORKER override that is
# missing is the caller's error, not a reason to build the default target.
DEFAULT_CPP_WORKER="$REPO/../transport/build/mwf_worker"
if [[ ! -x "$CPP_WORKER" && "$CPP_WORKER" == "$DEFAULT_CPP_WORKER" ]]; then
  echo "[e2e] mwf_worker not built at $CPP_WORKER — building" >&2
  [[ -f "$REPO/../proto/gen/host/mwf/worker_service.pb.cc" ]] || "$REPO/../proto/gen_host.sh" >/dev/null
  cmake -S "$REPO/../transport" -B "$REPO/../transport/build" >/dev/null
  cmake --build "$REPO/../transport/build" --target mwf_worker -j 8 >/dev/null
fi
[[ -x "$CPP_WORKER" ]] || { echo "C++ worker missing: $CPP_WORKER" >&2; exit 2; }

mkdir -p "$EV"
: > "$EV/http_trace.jsonl"

PY_WORKER_PID=""
CPP_WORKER_PID=""
cleanup() {
  echo "[e2e] cleanup: stopping workers"
  [[ -n "$CPP_WORKER_PID" ]] && kill "$CPP_WORKER_PID" 2>/dev/null || true
  [[ -n "$PY_WORKER_PID" ]] && kill "$PY_WORKER_PID" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT

# ── Python worker (hosts both workflows; registers the deployment) ─────────── #
echo "[e2e] starting Python worker (DEPLOYMENT_NAME=$DEPLOYMENT)"
DEPLOYMENT_NAME="$DEPLOYMENT" MWF_HTTP_TRACE="$EV/http_trace.jsonl" \
  MWF_CPP_TASK_QUEUE="$CPP_QUEUE" \
  "$PY" e2e/live_worker.py > "$EV/py_worker.log" 2>&1 &
PY_WORKER_PID=$!
echo "[e2e] python worker pid=$PY_WORKER_PID (log: e2e/evidence/py_worker.log)"

# ── E2E-1: pure-Python server-side execution ──────────────────────────────── #
"$PY" e2e/live_trigger.py \
  --workflow mwf_e2e_py --task-queue "$DEPLOYMENT" \
  --input '{"name": "nimbus"}' --expect "from Python" \
  --evidence "$EV/py_e2e.json"
echo "[e2e] E2E-1 (Python-only) PASSED"

# ── C++ worker on its own task queue ──────────────────────────────────────── #
echo "[e2e] starting C++ worker on queue '$CPP_QUEUE'"
"$CPP_WORKER" --whoami --api-key-env MISTRAL_API_KEY \
  --task-queue "$CPP_QUEUE" --identity mwf-cpp-live@desktop \
  > "$EV/cpp_worker.log" 2>&1 &
CPP_WORKER_PID=$!
echo "[e2e] C++ worker pid=$CPP_WORKER_PID (log: e2e/evidence/cpp_worker.log)"
sleep 3   # let it establish the long-poll before triggering

# ── E2E-2: the money shot — C++ executes the activity on real Mistral ─────── #
"$PY" e2e/live_trigger.py \
  --workflow mwf_e2e_cpp_live --task-queue "$DEPLOYMENT" \
  --input '{"name": "nimbus"}' --expect "from C++" \
  --evidence "$EV/cpp_e2e.json"
echo "[e2e] E2E-2 (C++ activity) PASSED"

sleep 1
echo "[e2e] C++ worker wire evidence:"
grep -E "completed|wire=" "$EV/cpp_worker.log" || true

echo "[e2e] both live E2Es green; evidence in e2e/evidence/"

#!/usr/bin/env bash
# run_desktop_worker_e2e.sh — the desktop worker E2E gate, in one command.
#
# Boots a throwaway `temporal server start-dev`, starts the C++ mwf_worker
# (built in ../transport/build) polling task queue "mwf-cpp-e2e", then runs
# the Python driver: a Python workflow host schedules its activities onto the
# C++ worker's queue and asserts the result carries the C++ output. Exit 0 iff
# the whole chain is green. All processes are cleaned up on exit.
#
#   ./e2e/run_desktop_worker_e2e.sh
#
# Env overrides:
#   WFPY            python with temporalio installed (else repo .venv, created)
#   MWF_WORKER_BIN  path to mwf_worker (default ../transport/build/mwf_worker)
#   TEMPORAL_PORT   dev-server port (default 7239 — off 7233 so a running dev
#                   server or broker can't collide with the throwaway one)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
PORT="${TEMPORAL_PORT:-7239}"
WORKER_BIN="${MWF_WORKER_BIN:-$REPO/../transport/build/mwf_worker}"
CPP_TQ="mwf-cpp-e2e"

# --- 1. python interpreter (same pattern as capture/run_all.sh) -------------- #
if [[ -n "${WFPY:-}" ]]; then
  PY="$WFPY"
else
  if [[ ! -d .venv ]]; then
    echo "[e2e] creating .venv + installing deps"
    python3 -m venv .venv
    ./.venv/bin/pip install -q --upgrade pip
    ./.venv/bin/pip install -q -e .
  fi
  PY="$REPO/.venv/bin/python"
fi
"$PY" -c "import temporalio" || { echo "[e2e] temporalio missing in $PY" >&2; exit 1; }

# --- 2. the C++ worker binary ------------------------------------------------ #
if [[ ! -x "$WORKER_BIN" ]]; then
  echo "[e2e] mwf_worker not built at $WORKER_BIN — building" >&2
  [[ -f "$REPO/../proto/gen/host/mwf/worker_service.pb.cc" ]] || "$REPO/../proto/gen_host.sh" >/dev/null
  cmake -B "$REPO/../transport/build" -S "$REPO/../transport" \
    -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/grpc;/opt/homebrew/opt/protobuf;/opt/homebrew/opt/abseil"
  cmake --build "$REPO/../transport/build" --target mwf_worker -j 8
fi
[[ -x "$WORKER_BIN" ]] || { echo "[e2e] no mwf_worker binary" >&2; exit 1; }
echo "[e2e] worker: $WORKER_BIN"

# --- 3. temporal CLI --------------------------------------------------------- #
TEMPORAL_BIN="$(command -v temporal || echo /opt/homebrew/bin/temporal)"
[[ -x "$TEMPORAL_BIN" ]] || { echo "[e2e] 'temporal' CLI not found (brew install temporal)" >&2; exit 1; }

# --- 4. boot dev server + worker, with teardown ------------------------------ #
TMPDIR_RUN="$(mktemp -d -t mwf-m3-e2e-XXXX)"
SERVER_LOG="$TMPDIR_RUN/server.log"
WORKER_LOG="$TMPDIR_RUN/worker.log"
SERVER_PID=""
WORKER_PID=""
cleanup() {
  local rc=$?
  [[ -n "$WORKER_PID" ]] && { kill "$WORKER_PID" 2>/dev/null || true; wait "$WORKER_PID" 2>/dev/null || true; }
  [[ -n "$SERVER_PID" ]] && { kill "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true; }
  if [[ $rc -ne 0 ]]; then
    echo "[e2e] --- worker log ---";  tail -40 "$WORKER_LOG" 2>/dev/null || true
    echo "[e2e] --- server log ---";  tail -20 "$SERVER_LOG" 2>/dev/null || true
  fi
  rm -rf "$TMPDIR_RUN" || true
  exit $rc
}
trap cleanup EXIT

"$TEMPORAL_BIN" server start-dev \
  --db-filename "$TMPDIR_RUN/dev.db" --headless --ip 127.0.0.1 --port "$PORT" \
  >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
echo "[e2e] dev server pid=$SERVER_PID port=$PORT"
for _ in $(seq 1 40); do nc -z 127.0.0.1 "$PORT" 2>/dev/null && break; sleep 0.5; done
nc -z 127.0.0.1 "$PORT" 2>/dev/null || { echo "[e2e] server never came up" >&2; exit 1; }

# --tls 0: the local dev server is plaintext; the worker's TLS-on default
# (and its bearer refusal) only apply to real, credentialed endpoints.
"$WORKER_BIN" --target "127.0.0.1:$PORT" --namespace default \
  --task-queue "$CPP_TQ" --identity "mwf-cpp-e2e@$(hostname -s)" \
  --tls 0 --poll-ms 15000 >"$WORKER_LOG" 2>&1 &
WORKER_PID=$!
echo "[e2e] C++ worker pid=$WORKER_PID task_queue=$CPP_TQ (log: $WORKER_LOG)"
sleep 1
kill -0 "$WORKER_PID" 2>/dev/null || { echo "[e2e] worker died on startup" >&2; exit 1; }

# --- 5. the gate ------------------------------------------------------------- #
export TEMPORAL_ADDRESS="127.0.0.1:$PORT"
export MWF_CPP_TASK_QUEUE="$CPP_TQ"
"$PY" e2e/desktop_worker_e2e.py
RC=$?

echo "[e2e] --- C++ worker ticks ---"
grep '^\[worker\] tick' "$WORKER_LOG" | tail -8 || true
echo "[e2e] result: $([[ $RC -eq 0 ]] && echo GREEN || echo RED)"
exit $RC

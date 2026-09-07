#!/usr/bin/env bash
# run_all.sh — regenerate every golden vector from scratch.
#
# Reproducible in one command: provisions a Python venv, boots a local
# `temporal server start-dev` (SQLite, no cloud), runs the history + payload
# captures against it, and tears the server down. Optionally runs the live
# Mistral smoke with --live (needs MISTRAL_API_KEY; read-only).
#
#   ./capture/run_all.sh            # local goldens only (deterministic, offline)
#   ./capture/run_all.sh --live     # + live api.mistral.ai smoke (read-only)
#
# Env overrides:
#   WFPY     path to a python interpreter that already has temporalio +
#            mistralai-workflows (skips venv creation)
#   TEMPORAL_PORT   dev-server port (default 7233)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
PORT="${TEMPORAL_PORT:-7233}"
LIVE=0
[[ "${1:-}" == "--live" ]] && LIVE=1

# --- 1. python interpreter -------------------------------------------------- #
if [[ -n "${WFPY:-}" ]]; then
  PY="$WFPY"
else
  if [[ ! -d .venv ]]; then
    echo "[run_all] creating .venv + installing deps (temporalio, mistralai-workflows)"
    python3 -m venv .venv
    ./.venv/bin/pip install -q --upgrade pip
    ./.venv/bin/pip install -q -e .
  fi
  PY="$REPO/.venv/bin/python"
fi
echo "[run_all] python: $PY"
"$PY" -c "import temporalio, mistralai.workflows; print('[run_all] deps OK')"

# --- 2. locate temporal CLI ------------------------------------------------- #
TEMPORAL_BIN="$(command -v temporal || echo /opt/homebrew/bin/temporal)"
if [[ ! -x "$TEMPORAL_BIN" ]]; then
  echo "[run_all] ERROR: 'temporal' CLI not found. Install with: brew install temporal" >&2
  echo "[run_all] (or download the release binary from github.com/temporalio/cli)" >&2
  exit 1
fi
echo "[run_all] temporal: $TEMPORAL_BIN ($($TEMPORAL_BIN --version))"

# --- 3. boot dev server ----------------------------------------------------- #
# temporal needs to CREATE the sqlite db (an existing empty file => "no such
# table"), so reserve a unique dir and point at a not-yet-existing file in it.
TMPDIR_RUN="$(mktemp -d -t mwf-temporal-XXXX)"
DBFILE="$TMPDIR_RUN/dev.db"
LOGFILE="$TMPDIR_RUN/dev.log"
"$TEMPORAL_BIN" server start-dev \
  --db-filename "$DBFILE" --headless --ip 127.0.0.1 --port "$PORT" \
  >"$LOGFILE" 2>&1 &
SERVER_PID=$!
cleanup() {
  echo "[run_all] stopping dev server (pid $SERVER_PID)"
  kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
  rm -rf "$TMPDIR_RUN" || true
}
trap cleanup EXIT

echo "[run_all] waiting for dev server on 127.0.0.1:$PORT ..."
for _ in $(seq 1 30); do
  if nc -z 127.0.0.1 "$PORT" 2>/dev/null; then break; fi
  sleep 0.5
done
nc -z 127.0.0.1 "$PORT" 2>/dev/null || { echo "[run_all] server never came up"; cat "$LOGFILE"; exit 1; }

# --- 4. captures ------------------------------------------------------------ #
export TEMPORAL_ADDRESS="127.0.0.1:$PORT"
echo "[run_all] === capture_history ==="
"$PY" capture/capture_history.py
echo "[run_all] === capture_payloads ==="
"$PY" capture/capture_payloads.py

# --- 5. optional live smoke ------------------------------------------------- #
if [[ "$LIVE" == "1" ]]; then
  echo "[run_all] === live_smoke (read-only) ==="
  "$PY" capture/live_smoke.py || echo "[run_all] live smoke failed (non-fatal)"
fi

echo "[run_all] DONE. goldens under goldens/"

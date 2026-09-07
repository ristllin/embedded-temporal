#!/usr/bin/env bash
# cookbooks/03-reproducibility — Temporal determinism, demonstrated live.
#
# The workflow is DATA (a declarative spec) replayed by a deterministic engine,
# so the SAME input always yields the SAME decisions: the same command stream
# (ScheduleActivity… → CompleteWorkflow) and the same result. We prove it by
# triggering the identical input TWICE and diffing:
#   * the worker's per-execution command stream (from worker.log), and
#   * the extracted result of each execution (from the trigger evidence).
# A third run with a different input takes the other conditional branch — same
# structure, different (still deterministic) result — so Studio shows multiple
# executions for screenshots. Activities are all `echo` (no AI): fully
# deterministic, so "same result" is exact, not approximate.
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
WF="cookbook03_repro_$STAMP"
QUEUE="cookbook03-repro-$STAMP"

# Two identical inputs (n=4 → 'big' branch) + one different (n=1 → 'small').
INPUT_SAME='{"n":4,"note":"determinism-demo"}'
INPUT_ALT='{"n":1,"note":"determinism-demo"}'

# ── build ────────────────────────────────────────────────────────────────────
echo "[cb03] building cookbook_worker ..."
[[ -f "$REPO/proto/gen/host/mwf/worker_service.pb.cc" ]] || "$REPO/proto/gen_host.sh" >/dev/null
[[ -x "$WORKER" ]] || cmake -S "$TRANSPORT" -B "$BUILD" >/dev/null
cmake --build "$BUILD" --target cookbook_worker >/dev/null
[[ -x "$WORKER" ]] || { echo "[cb03] build failed: $WORKER missing" >&2; exit 2; }
echo "[cb03] workflow=$WF queue=$QUEUE"

WORKER_PID=""
cleanup() {
  local rc=$?
  echo "[cb03] cleanup ..."
  [[ -n "$WORKER_PID" ]] && kill "$WORKER_PID" 2>/dev/null || true
  local exid=""
  [[ -f "$EV/trigger_a.json" ]] && exid="$(sed -n 's/.*"execution_id":"\([0-9a-f]*\)".*/\1/p' "$EV/trigger_a.json" | head -1)"
  "$WORKER" --mode cleanup --workflow "$WF" --timeout-s 45 ${exid:+--exec-id "$exid"} \
    >>"$EV/cleanup.log" 2>&1 || true
  wait 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT

# ── worker: register + service THREE completions ─────────────────────────────
echo "[cb03] starting worker (register + both loops, max-completions=3) ..."
"$WORKER" --mode worker --spec "$SPEC" --workflow "$WF" --queue "$QUEUE" \
  --max-completions 3 --poll-ms 3000 --max-seconds 200 \
  >"$EV/worker.log" 2>&1 &
WORKER_PID=$!
echo "[cb03] worker pid=$WORKER_PID (log: evidence/worker.log)"

trigger() {  # <label> <input-json> <evidence-file>
  echo "[cb03] trigger $1 input=$2 ..."
  set +e
  "$WORKER" --mode trigger --workflow "$WF" --queue "$QUEUE" \
    --input-json "$2" --timeout-s 180 --evidence "$3" | tee "${3%.json}.log"
  local rc=${PIPESTATUS[0]}
  set -e
  return "$rc"
}

# Sequential (each polled to COMPLETED before the next), so command_stream #1/#2/#3
# map to runs A/B/C in order.
trigger "A (same input, run 1)" "$INPUT_SAME" "$EV/trigger_a.json"; RC_A=$?
trigger "B (same input, run 2)" "$INPUT_SAME" "$EV/trigger_b.json"; RC_B=$?
trigger "C (different input)"    "$INPUT_ALT"  "$EV/trigger_c.json"; RC_C=$?

wait "$WORKER_PID" 2>/dev/null && WORKER_RC=0 || WORKER_RC=$?
WORKER_PID=""

# ── determinism evidence ─────────────────────────────────────────────────────
CMD1="$(grep -o 'command_stream #1=\[[^]]*\]' "$EV/worker.log" | sed 's/.*=//' | tail -1)"
CMD2="$(grep -o 'command_stream #2=\[[^]]*\]' "$EV/worker.log" | sed 's/.*=//' | tail -1)"
CMD3="$(grep -o 'command_stream #3=\[[^]]*\]' "$EV/worker.log" | sed 's/.*=//' | tail -1)"

{
  echo "run A (n=4): command_stream=$CMD1"
  echo "run B (n=4): command_stream=$CMD2"
  echo "run C (n=1): command_stream=$CMD3"
} > "$EV/determinism.txt"

echo "[cb03] ===== VERDICT ====="
A_EXEC="$(sed -n 's/.*"execution_id":"\([0-9a-f]*\)".*/\1/p' "$EV/trigger_a.json" | head -1)"
B_EXEC="$(sed -n 's/.*"execution_id":"\([0-9a-f]*\)".*/\1/p' "$EV/trigger_b.json" | head -1)"
C_EXEC="$(sed -n 's/.*"execution_id":"\([0-9a-f]*\)".*/\1/p' "$EV/trigger_c.json" | head -1)"
echo "[cb03] A execution_id=$A_EXEC rc=$RC_A"
echo "[cb03] B execution_id=$B_EXEC rc=$RC_B"
echo "[cb03] C execution_id=$C_EXEC rc=$RC_C"
echo "[cb03] command streams: A=$CMD1  B=$CMD2  C=$CMD3"

FAIL=0
[[ "$RC_A" -eq 0 && "$RC_B" -eq 0 && "$RC_C" -eq 0 ]] || { echo "[cb03] FAIL: a trigger did not COMPLETE"; FAIL=1; }
[[ "$WORKER_RC" -eq 0 ]] || { echo "[cb03] FAIL: worker did not service all completions"; FAIL=1; }

# Command streams of the two identical-input runs must match exactly.
if [[ -n "$CMD1" && "$CMD1" == "$CMD2" ]]; then
  echo "[cb03] command stream A == B : $CMD1  (identical)"
else
  echo "[cb03] FAIL: command stream A ('$CMD1') != B ('$CMD2')"; FAIL=1
fi

# Extracted results of the two identical-input runs must be byte-identical; the
# different-input run must differ (still deterministic, just another branch).
python3 - "$EV/trigger_a.json" "$EV/trigger_b.json" "$EV/trigger_c.json" >>"$EV/determinism.txt" 2>&1 <<'PY'
import json, sys
def result(p):
    d = json.load(open(p)); return d.get("status"), d.get("result")
sa, ra = result(sys.argv[1]); sb, rb = result(sys.argv[2]); sc, rc = result(sys.argv[3])
da, db, dc = json.dumps(ra, sort_keys=True), json.dumps(rb, sort_keys=True), json.dumps(rc, sort_keys=True)
print("\nresult A:", da)
print("result B:", db)
print("result C:", dc)
ok = True
if sa != "COMPLETED" or sb != "COMPLETED" or sc != "COMPLETED":
    print("FAIL: not all COMPLETED"); ok = False
if da == db:
    print("OK: result A == result B (byte-identical, same input -> same result)")
else:
    print("FAIL: result A != result B"); ok = False
if dc != da:
    print("OK: result C differs (n=1 took the 'small' branch)")
else:
    print("FAIL: different input produced the same result")
    ok = False
sys.exit(0 if ok else 1)
PY
PY_RC=$?
[[ "$PY_RC" -eq 0 ]] || FAIL=1

echo "[cb03] --- determinism.txt ---"
cat "$EV/determinism.txt"

if [[ "$FAIL" -eq 0 ]]; then
  echo "[cb03] PASS (A=$A_EXEC B=$B_EXEC C=$C_EXEC; A and B reproducible)"
  exit 0
fi
echo "[cb03] FAIL"
exit 1

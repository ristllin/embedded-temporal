#!/usr/bin/env bash
# cookbooks/02-ai-workflow — a multi-activity AI workflow on real Mistral:
#   ai.classify (sentiment of the input text)  ->  conditional on the label  ->
#   ai.chat (branch-specific reply)            ->  complete (label + reply).
# Driven end-to-end by our C++ cookbook_worker (no Python / mistralai-workflows).
# Triggered twice — a positive review and a critical one — so the two runs take
# DIFFERENT branches and produce different, real AI text.
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
WF="cookbook02_ai_$STAMP"
QUEUE="cookbook02-ai-$STAMP"

POS_TEXT="I absolutely love this little device. The LED ring is gorgeous and it just works. Best desk gadget I have bought all year!"
NEG_TEXT="Honestly disappointed. It kept disconnecting, the setup was confusing, and it feels flimsy for the price."

# Build a trigger input file: the review text, plus branch-specific chat prompts
# with that text interpolated in (python does the JSON escaping safely).
make_input() {
  local text="$1" out="$2"
  TEXT="$text" python3 - "$out" <<'PY'
import json, os, sys
text = os.environ["TEXT"]
inp = {
  "text": text,
  "classify": {
    "text": text,
    "labels": ["positive", "negative", "neutral"]
  },
  "positive": {
    "system": "You are a warm, concise customer-support agent.",
    "prompt": f'A customer left this positive review: "{text}". Write a genuine 2-sentence thank-you that mentions something specific they liked.'
  },
  "negative": {
    "system": "You are an empathetic, concise customer-support agent.",
    "prompt": f'A customer left this critical review: "{text}". Write a brief, 2-sentence empathetic reply that acknowledges the issue and offers a concrete next step.'
  }
}
open(sys.argv[1], "w").write(json.dumps(inp))
PY
}

# ── build ────────────────────────────────────────────────────────────────────
echo "[cb02] building cookbook_worker ..."
[[ -f "$REPO/proto/gen/host/mwf/worker_service.pb.cc" ]] || "$REPO/proto/gen_host.sh" >/dev/null
[[ -x "$WORKER" ]] || cmake -S "$TRANSPORT" -B "$BUILD" >/dev/null
cmake --build "$BUILD" --target cookbook_worker >/dev/null
[[ -x "$WORKER" ]] || { echo "[cb02] build failed: $WORKER missing" >&2; exit 2; }
echo "[cb02] workflow=$WF queue=$QUEUE"

make_input "$POS_TEXT" "$EV/input_pos.json"
make_input "$NEG_TEXT" "$EV/input_neg.json"

WORKER_PID=""
cleanup() {
  local rc=$?
  echo "[cb02] cleanup ..."
  [[ -n "$WORKER_PID" ]] && kill "$WORKER_PID" 2>/dev/null || true
  local exid=""
  [[ -f "$EV/trigger_pos.json" ]] && exid="$(sed -n 's/.*"execution_id":"\([0-9a-f]*\)".*/\1/p' "$EV/trigger_pos.json" | head -1)"
  "$WORKER" --mode cleanup --workflow "$WF" --timeout-s 45 ${exid:+--exec-id "$exid"} \
    >>"$EV/cleanup.log" 2>&1 || true
  wait 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT

# ── worker: register + service TWO completions (backgrounded) ────────────────
echo "[cb02] starting worker (register + both loops, max-completions=2) ..."
"$WORKER" --mode worker --spec "$SPEC" --workflow "$WF" --queue "$QUEUE" \
  --max-completions 2 --poll-ms 3000 --max-seconds 220 \
  >"$EV/worker.log" 2>&1 &
WORKER_PID=$!
echo "[cb02] worker pid=$WORKER_PID (log: evidence/worker.log)"

# ── trigger #1: positive review (expect the 'positive' branch) ───────────────
echo "[cb02] trigger #1 (positive review) ..."
set +e
"$WORKER" --mode trigger --workflow "$WF" --queue "$QUEUE" \
  --input-file "$EV/input_pos.json" --timeout-s 200 --evidence "$EV/trigger_pos.json" \
  | tee "$EV/trigger_pos.log"
RC_POS=${PIPESTATUS[0]}
set -e

# ── trigger #2: critical review (expect the 'false' branch) ──────────────────
echo "[cb02] trigger #2 (critical review) ..."
set +e
"$WORKER" --mode trigger --workflow "$WF" --queue "$QUEUE" \
  --input-file "$EV/input_neg.json" --timeout-s 200 --evidence "$EV/trigger_neg.json" \
  | tee "$EV/trigger_neg.log"
RC_NEG=${PIPESTATUS[0]}
set -e

wait "$WORKER_PID" 2>/dev/null && WORKER_RC=0 || WORKER_RC=$?
WORKER_PID=""

# ── verdict ──────────────────────────────────────────────────────────────────
echo "[cb02] ===== VERDICT ====="
POS_EXEC="$(sed -n 's/.*"execution_id":"\([0-9a-f]*\)".*/\1/p' "$EV/trigger_pos.json" | head -1)"
NEG_EXEC="$(sed -n 's/.*"execution_id":"\([0-9a-f]*\)".*/\1/p' "$EV/trigger_neg.json" | head -1)"
echo "[cb02] positive execution_id=$POS_EXEC rc=$RC_POS"
echo "[cb02] critical execution_id=$NEG_EXEC rc=$RC_NEG"
echo "[cb02] worker_rc=$WORKER_RC (services both completions)"

# Inspect the two results: COMPLETED, a label, a non-empty reply, DIFFERENT
# branches (labels differ), and both AI replies present.
python3 - "$EV/trigger_pos.json" "$EV/trigger_neg.json" <<'PY'
import json, sys
def load(p):
    d = json.load(open(p))
    r = d.get("result") or {}
    label = (r.get("classify") or {}).get("label")
    text  = (r.get("reply") or {}).get("text") or ""
    return d.get("status"), label, text
ps, pl, pt = load(sys.argv[1])
ns, nl, nt = load(sys.argv[2])
# The conditional branches on: label == "positive" (true) vs anything else (false).
pbranch = "true (positive reply)"  if pl == "positive" else "false (empathetic reply)"
nbranch = "true (positive reply)"  if nl == "positive" else "false (empathetic reply)"
print(f"[cb02] positive review: status={ps} label={pl!r} -> branch {pbranch}")
print(f"[cb02]   reply={pt[:140]!r}")
print(f"[cb02] critical review: status={ns} label={nl!r} -> branch {nbranch}")
print(f"[cb02]   reply={nt[:140]!r}")
fail = 0
if ps != "COMPLETED": print("[cb02] FAIL: positive run not COMPLETED"); fail = 1
if ns != "COMPLETED": print("[cb02] FAIL: critical run not COMPLETED"); fail = 1
if not pt.strip():    print("[cb02] FAIL: positive reply text empty"); fail = 1
if not nt.strip():    print("[cb02] FAIL: critical reply text empty"); fail = 1
# The two inputs must take DIFFERENT branches of the conditional.
if pl == "positive" and nl != "positive":
    print("[cb02] branches diverged: positive->true, critical->false OK")
else:
    print(f"[cb02] FAIL: branches did not diverge as expected (pos={pl!r} crit={nl!r})"); fail = 1
sys.exit(1 if fail else 0)
PY
PY_RC=$?

FAIL=0
[[ "$RC_POS" -eq 0 ]]   || { echo "[cb02] FAIL: positive trigger rc=$RC_POS"; FAIL=1; }
[[ "$RC_NEG" -eq 0 ]]   || { echo "[cb02] FAIL: critical trigger rc=$RC_NEG"; FAIL=1; }
[[ "$WORKER_RC" -eq 0 ]] || { echo "[cb02] FAIL: worker did not service both completions"; FAIL=1; }
[[ "$PY_RC" -eq 0 ]]    || FAIL=1

if [[ "$FAIL" -eq 0 ]]; then
  echo "[cb02] PASS (positive=$POS_EXEC critical=$NEG_EXEC)"
  exit 0
fi
echo "[cb02] FAIL"
exit 1

#!/usr/bin/env bash
# verify_sanitized.sh — pre-publication sanitization gate.
#
#   ./tools/release/verify_sanitized.sh [ref]     # default ref: public-snapshot
#
# Re-runs the sanitization sweep's grep battery against the full tracked tree
# of <ref> and exits non-zero on ANY hit:
#   * denylisted identifiers (see below): plain, case-insensitive, AND their
#     base64 forms at all three framing offsets (an identifier embedded inside
#     a larger base64 blob is still caught);
#   * credential patterns (API keys, tokens, private keys, key assignments);
#   * absolute home paths (/Users/...);
#   * tracked symlinks (the published tree must contain none);
#   * commit identities: every author/committer email on <ref>'s history must
#     be in the allowed set.
#
# DENYLIST: the personal identifiers themselves must not appear in this
# committed script (it ships with the tree it guards), so they are read one
# per line (comments with #) from a NON-COMMITTED file:
#     $SCRUB_DENYLIST, or <repo>/.scrub-denylist (gitignored)
# The release gate runs with the maintainer's denylist. Without one the scan
# is incomplete, so the script fails closed; set ALLOW_NO_DENYLIST=1 to run
# only the generic checks (useful for third parties).
#
# NOTE: scans use `git grep -a` (--text) deliberately — one doc embeds a
# literal NUL byte, and without -a grep would classify it binary and silently
# report a false "clean".
set -uo pipefail

# Preflight: python3 computes the base64 framing cores; without it that whole
# battery would be skipped, so its absence is fatal (fail closed).
command -v python3 >/dev/null 2>&1 || {
  echo "ERROR: python3 not found in PATH — base64 identifier checks cannot run" >&2
  exit 2
}

REF="${1:-public-snapshot}"
REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"
git rev-parse --verify --quiet "${REF}^{commit}" >/dev/null || {
  echo "ERROR: ref '$REF' not found (run tools/release/build_snapshot.sh first?)" >&2
  exit 2
}

ALLOWED_IDENTITIES=(
  "contact@cumulo-nimbus.ai"
  "32160010+ristllin@users.noreply.github.com"
)

FAIL=0
pass() { echo "ok    $1"; }
fail() { echo "FAIL  $1"; FAIL=1; }

show_hits() { sed 's/^/      /' <<<"$1" | head -40; }

# git grep helpers; hit => finding. Exit 0 = hits, 1 = clean; anything else is
# a grep FAILURE (bad ref, bad pattern, ...) and must abort, not read as clean.
grep_verdict() { # label, rc, hits
  case "$2" in
    0) fail "$1"; show_hits "$3" ;;
    1) pass "$1" ;;
    *) echo "ERROR: git grep exited $2 during check '$1' — aborting" >&2; exit 2 ;;
  esac
}
scan_fixed() { # label, literal (case-insensitive)
  local hits rc
  hits="$(git grep -a -i -n -F -e "$2" "$REF" --)"; rc=$?
  grep_verdict "$1" "$rc" "$hits"
}
scan_fixed_cs() { # label, literal (case-sensitive; base64 cores)
  local hits rc
  hits="$(git grep -a -n -F -e "$2" "$REF" --)"; rc=$?
  grep_verdict "$1" "$rc" "$hits"
}
scan_regex() { # label, ERE
  local hits rc
  hits="$(git grep -a -n -E -e "$2" "$REF" --)"; rc=$?
  grep_verdict "$1" "$rc" "$hits"
}

echo "== verify_sanitized: ref $REF ($(git rev-parse "$REF")) =="

# ── 1. Denylisted identifiers (plain + base64 at 3 framing offsets) ──────────
DENYLIST="${SCRUB_DENYLIST:-$REPO_ROOT/.scrub-denylist}"
if [ -f "$DENYLIST" ]; then
  # `|| [ -n "$literal" ]` keeps a final line that lacks a trailing newline —
  # without it, that entry would be silently skipped (fail open).
  while IFS= read -r literal || [ -n "$literal" ]; do
    case "$literal" in ''|'#'*) continue ;; esac
    scan_fixed "denylist plain: ${literal:0:4}…(${#literal} ch)" "$literal"
    cores="$(python3 - "$literal" <<'PYEOF'
import base64, sys
s = sys.argv[1].encode()
for shift in (0, 1, 2):
    e1 = base64.b64encode(b"\x00" * shift + s + b"\x00\x00").decode()
    e2 = base64.b64encode(b"\xff" * shift + s + b"\xff\xff").decode()
    best = cur = ""
    for a, c in zip(e1, e2):
        if a == c:
            cur += a
            if len(cur) > len(best):
                best = cur
        else:
            cur = ""
    if len(best) >= 8:
        print(best)
PYEOF
)" || { echo "ERROR: base64 frame computation failed for entry ${literal:0:4}… — aborting" >&2; exit 2; }
    if [ -z "$cores" ]; then
      echo "warn  denylist entry ${literal:0:4}…(${#literal} ch): too short for an 8-char base64 core — PLAIN-ONLY coverage"
      continue
    fi
    while IFS= read -r core; do
      scan_fixed_cs "denylist b64:   ${literal:0:4}… frame '${core:0:6}…'" "$core"
    done <<<"$cores"
  done < "$DENYLIST"
else
  if [ "${ALLOW_NO_DENYLIST:-0}" = "1" ]; then
    echo "warn  no denylist at $DENYLIST — identifier checks SKIPPED (generic checks only)"
  else
    echo "FAIL  no denylist at $DENYLIST — identifier checks cannot run" >&2
    echo "      (set SCRUB_DENYLIST=<file>, or ALLOW_NO_DENYLIST=1 for generic-only)" >&2
    exit 2
  fi
fi

# ── 2. Credential patterns ───────────────────────────────────────────────────
scan_regex "secrets: OpenAI-style key"     'sk-[A-Za-z0-9]{20}'
scan_regex "secrets: AWS access key"       'AKIA[0-9A-Z]{16}'
scan_regex "secrets: GitHub token"         '(ghp|gho|ghs|ghr)_[A-Za-z0-9]{20}'
scan_regex "secrets: GitHub fine-grained"  'github_pat_[A-Za-z0-9_]{20}'
scan_regex "secrets: Slack token"          'xox[baprs]-[A-Za-z0-9-]{10}'
scan_regex "secrets: Google API key"       'AIza[0-9A-Za-z_-]{35}'
scan_regex "secrets: private key block"    'BEGIN [A-Z ]*PRIVATE KEY'
scan_regex "secrets: Mistral key assign"   'MISTRAL_API_KEY[[:space:]]*=[[:space:]]*["'"'"']?[A-Za-z0-9]{16}'

# ── 3. Home paths ────────────────────────────────────────────────────────────
scan_regex "home path: /Users/*"           '/Users/[A-Za-z0-9_]'

# ── 4. Symlinks ──────────────────────────────────────────────────────────────
links="$(git ls-tree -r "$REF" | awk '$1=="120000" {print $4}')"
if [ -n "$links" ]; then
  fail "symlinks in tree"; show_hits "$links"
else pass "symlinks in tree: none"; fi

# ── 5. Commit identities on the ref's whole history ──────────────────────────
bad_ids=""
while IFS= read -r addr; do
  ok=0
  for allowed in "${ALLOWED_IDENTITIES[@]}"; do
    [ "$addr" = "$allowed" ] && ok=1 && break
  done
  [ "$ok" = "1" ] || bad_ids+="$addr"$'\n'
done < <(git log --format='%ae%n%ce' "$REF" | sort -u)
if [ -n "$bad_ids" ]; then
  fail "commit identity outside allowed set"; show_hits "$bad_ids"
else pass "commit identities: all allowed"; fi

echo
if [ "$FAIL" = "0" ]; then
  echo "RESULT: CLEAN — $REF passed the sanitization battery"
else
  echo "RESULT: FINDINGS — $REF is NOT clean (see FAIL lines above)"
fi
exit "$FAIL"

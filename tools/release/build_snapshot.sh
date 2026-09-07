#!/usr/bin/env bash
# build_snapshot.sh — build the publishable tree as the orphan branch `public-snapshot`.
#
# Clean-slate publish (Option A): the granular local history is never published;
# instead this script snapshots the tracked tree of a ref (default HEAD) minus
# publication exclusions into a SINGLE orphan commit on `public-snapshot`,
# authored with the public identity. At the publish gate the orchestrator
# force-pushes that branch over the remote's previous snapshot — this script
# itself NEVER pushes, tags, or touches any remote.
#
#   ./tools/release/build_snapshot.sh [ref]      # default ref: HEAD
#
# What it does:
#   1. Exports the tracked tree of <ref> (untracked/ignored files never ship).
#   2. Drops the exclusion list below (docs/history/ stays private).
#   3. Dereferences in-tree relative symlinks (nanopb SPM shims) so the
#      published tree contains ZERO symlinks, then verifies that invariant.
#   4. Writes one orphan commit (no parents) with the public identity and
#      resets the local branch `public-snapshot` to it. Idempotent: re-running
#      simply rebuilds the tree and moves the branch.
#
# Verify the result with tools/release/verify_sanitized.sh (defaults to the
# public-snapshot branch this script creates).
set -euo pipefail

REF="${1:-HEAD}"
BRANCH="public-snapshot"
VERSION="v0.1.1"
AUTHOR_NAME="Roy Darnell"
AUTHOR_EMAIL="contact@cumulo-nimbus.ai"

# Paths in the tracked tree that must NOT be published.
#   docs/history/ — internal research/lineage notes; referenced by no published
#   doc (links were removed at the source), excluded from the snapshot by the
#   program's publish ruling.
EXCLUDES=(
  "docs/history"
  "PUBLISHING.md"                        # internal maintainer release runbook
  ".github/workflows/publish-pypi.yml"   # PyPI path is not published; keep private
)

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"
COMMIT_ID="$(git rev-parse --verify "${REF}^{commit}")"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/mwf-snapshot.XXXXXX")"
WORK="$(cd "$WORK" && pwd -P)"   # normalize (macOS TMPDIR: symlinks, trailing /)
trap 'rm -rf "$WORK"' EXIT
EXPORT="$WORK/tree"
mkdir -p "$EXPORT"

# 1. Export exactly the tracked tree (tar round-trip keeps modes + symlinks,
#    which step 3 then resolves).
git archive "$COMMIT_ID" | tar -x -C "$EXPORT"

# 2. Apply exclusions.
for path in "${EXCLUDES[@]}"; do
  if [ -e "$EXPORT/$path" ]; then
    rm -rf "${EXPORT:?}/$path"
    echo "excluded: $path"
  else
    echo "warning: exclusion '$path' not present in $REF" >&2
  fi
done

# 3. Dereference symlinks (relative, in-tree only) so the snapshot has none.
while IFS= read -r -d '' link; do
  target="$(readlink "$link")"
  case "$target" in
    /*) echo "ERROR: absolute symlink in tree: $link -> $target" >&2; exit 1 ;;
  esac
  resolved="$(cd "$(dirname "$link")" && cd "$(dirname "$target")" && pwd -P)/$(basename "$target")"
  case "$resolved" in
    "$EXPORT"/*) ;;
    *) echo "ERROR: symlink escapes the tree: $link -> $target" >&2; exit 1 ;;
  esac
  rm "$link"
  cp "$resolved" "$link"
  echo "dereferenced symlink: ${link#"$EXPORT"/} -> $target"
done < <(find "$EXPORT" -type l -print0)

remaining=$(find "$EXPORT" -type l | wc -l | tr -d ' ')
if [ "$remaining" != "0" ]; then
  echo "ERROR: $remaining symlink(s) remain in the snapshot tree" >&2
  find "$EXPORT" -type l >&2
  exit 1
fi
echo "symlink check: 0 symlinks in snapshot tree"

# 4. Single orphan commit -> branch (temporary index; the real index/worktree
#    and HEAD are never touched, so this is safe to run mid-work).
export GIT_INDEX_FILE="$WORK/index"
git --work-tree="$EXPORT" add -Af
TREE_ID="$(git write-tree)"
unset GIT_INDEX_FILE

# Date pinned to the source commit's so rebuilding an unchanged tree yields
# the identical commit id (true idempotence).
SRC_DATE="$(git log -1 --format='%cI' "$COMMIT_ID")"
NEW_COMMIT="$(
  GIT_AUTHOR_NAME="$AUTHOR_NAME"    GIT_AUTHOR_EMAIL="$AUTHOR_EMAIL"    GIT_AUTHOR_DATE="$SRC_DATE" \
  GIT_COMMITTER_NAME="$AUTHOR_NAME" GIT_COMMITTER_EMAIL="$AUTHOR_EMAIL" GIT_COMMITTER_DATE="$SRC_DATE" \
  git commit-tree "$TREE_ID" -m "embedded-temporal $VERSION — public release snapshot

Single-commit sanitized snapshot of the tracked tree at $COMMIT_ID
(docs/history/ excluded, symlinks dereferenced). Built by
tools/release/build_snapshot.sh; verified by tools/release/verify_sanitized.sh."
)"

git branch -f "$BRANCH" "$NEW_COMMIT"
echo "branch $BRANCH -> $NEW_COMMIT (tree $TREE_ID, source $COMMIT_ID)"
echo "NOT pushed anywhere. Verify with: tools/release/verify_sanitized.sh $BRANCH"

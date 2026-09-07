#!/usr/bin/env bash
# tools/release/build_lib.sh — generate the single self-contained
# `embedded-temporal` library layout from this monorepo.
#
# One layout serves BOTH registries (PUBLISHING.md Paths B and C):
#   - PlatformIO Registry:      library.json      (pio pkg pack / publish)
#   - Arduino Library Manager:  library.properties (arduino-lint / registry PR)
#
# Contents: the device activity-worker subset — contracts headers, the
# JSON-lib-free core TUs (worker loop + activity registry), the payload codec,
# the ESP32 gRPC/h2 transport with its vendored nghttp2, and the nanopb
# activity device subset. No PSRAM requirement, no PB_FIELD_32BIT, no JSON
# library. This script only GENERATES and CHECKS; it publishes nothing.
#
# Usage: tools/release/build_lib.sh [OUT_DIR]
#   OUT_DIR defaults to ./out/embedded-temporal-lib (relative to the CWD).
#   The OUT_DIR is recreated from scratch on every run.
set -euo pipefail

VERSION="0.1.1"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OVERLAY="$REPO_ROOT/tools/release/lib"
OUT="${1:-$PWD/out/embedded-temporal-lib}"

say() { printf '==> %s\n' "$*"; }
die() { printf 'build_lib: ERROR: %s\n' "$*" >&2; exit 1; }

[ -f "$REPO_ROOT/contracts/include/mwf/contracts.h" ] || die "repo root not found at $REPO_ROOT"
[ -d "$OVERLAY" ] || die "overlay dir missing: $OVERLAY"

say "generating embedded-temporal $VERSION -> $OUT"
rm -rf "${OUT:?}"
mkdir -p "$OUT/src/mwf" "$OUT/src/mwf_core" "$OUT/src/mwf_codec" \
         "$OUT/src/nghttp2" "$OUT/src/nghttp2_lib" "$OUT/examples/BlinkActivity"

# ── 1. static overlay: manifests, umbrella header, README, example ──────────
cp "$OVERLAY/library.json"        "$OUT/library.json"
cp "$OVERLAY/library.properties"  "$OUT/library.properties"
cp "$OVERLAY/keywords.txt"        "$OUT/keywords.txt"
cp "$OVERLAY/README.md"           "$OUT/README.md"
cp "$OVERLAY/src/EmbeddedTemporal.h" "$OUT/src/EmbeddedTemporal.h"
cp "$OVERLAY/examples/BlinkActivity/BlinkActivity.ino" \
   "$OUT/examples/BlinkActivity/BlinkActivity.ino"
cp "$REPO_ROOT/LICENSE" "$OUT/LICENSE"

grep -q "\"version\": \"$VERSION\"" "$OUT/library.json" \
  || die "library.json version != $VERSION"
grep -q "^version=$VERSION\$" "$OUT/library.properties" \
  || die "library.properties version != $VERSION"

# ── 2. contracts headers ────────────────────────────────────────────────────
cp "$REPO_ROOT"/contracts/include/mwf/types.h \
   "$REPO_ROOT"/contracts/include/mwf/temporal_types.h \
   "$REPO_ROOT"/contracts/include/mwf/contracts.h \
   "$OUT/src/mwf/"

# ── 3. core: the device-portable, JSON-lib-free activity TUs only ───────────
cp "$REPO_ROOT"/core/include/mwf_core/worker_loop.h \
   "$REPO_ROOT"/core/include/mwf_core/activity_registry.h \
   "$OUT/src/mwf_core/"
cp "$REPO_ROOT"/core/src/worker_loop.cpp \
   "$REPO_ROOT"/core/src/activity_registry.cpp \
   "$OUT/src/mwf_core/"

# ── 4. codec ────────────────────────────────────────────────────────────────
cp "$REPO_ROOT"/codec/include/mwf_codec/payload_codec.h "$OUT/src/mwf_codec/"
cp "$REPO_ROOT"/codec/src/payload_codec.cpp             "$OUT/src/mwf_codec/"

# ── 5. proto: nanopb activity device subset (no workflow, no PB_FIELD_32BIT) ─
cp "$REPO_ROOT"/proto/gen/nanopb-device/mwf/device_activity.pb.h \
   "$REPO_ROOT"/proto/gen/nanopb-device/mwf/device_activity.pb.c \
   "$OUT/src/mwf/"
cp "$REPO_ROOT"/proto/gen/nanopb_worker_adapter.h \
   "$REPO_ROOT"/proto/gen/nanopb_worker_adapter.cpp \
   "$OUT/src/"
# nanopb runtime, from the canonical root files (NOT spm_headers — those are
# symlinks; cp follows and materializes nothing here, we take the real files).
cp "$REPO_ROOT"/proto/third_party/nanopb/pb.h \
   "$REPO_ROOT"/proto/third_party/nanopb/pb_common.h \
   "$REPO_ROOT"/proto/third_party/nanopb/pb_common.c \
   "$REPO_ROOT"/proto/third_party/nanopb/pb_decode.h \
   "$REPO_ROOT"/proto/third_party/nanopb/pb_decode.c \
   "$REPO_ROOT"/proto/third_party/nanopb/pb_encode.h \
   "$REPO_ROOT"/proto/third_party/nanopb/pb_encode.c \
   "$OUT/src/"

# ── 6. transport: esp gRPC/h2 + vendored nghttp2 ────────────────────────────
cp "$REPO_ROOT"/transport/esp/esp_transport.h \
   "$REPO_ROOT"/transport/esp/esp_transport.cpp \
   "$REPO_ROOT"/transport/esp/grpc_h2.h \
   "$REPO_ROOT"/transport/esp/grpc_h2.cpp \
   "$OUT/src/"
cp "$REPO_ROOT"/transport/esp/third_party/nghttp2/lib/includes/nghttp2/nghttp2.h \
   "$REPO_ROOT"/transport/esp/third_party/nghttp2/lib/includes/nghttp2/nghttp2ver.h \
   "$OUT/src/nghttp2/"
cp "$REPO_ROOT"/transport/esp/third_party/nghttp2/lib/*.c \
   "$REPO_ROOT"/transport/esp/third_party/nghttp2/lib/*.h \
   "$OUT/src/nghttp2_lib/"

# Arduino has no per-library build flags, so make the nghttp2 sources
# self-configuring: replace the autotools `#ifdef HAVE_CONFIG_H` /
# `#include <config.h>` guard with an unconditional include of a bundled
# config header (same macro set as transport/esp/third_party/nghttp2/config.h).
cat > "$OUT/src/nghttp2_lib/nghttp2_pkg_config.h" <<'EOF'
/* embedded-temporal packaging: bundled nghttp2 config (generated by
 * tools/release/build_lib.sh — replaces the autotools <config.h> guard so the
 * library builds with no -D flags, as the Arduino IDE requires).
 * Macro set mirrors the monorepo's vendored config.h: ESP32 (ESP-IDF/newlib +
 * lwIP) provides the POSIX trio below. */
#ifndef NGHTTP2_PKG_CONFIG_H
#define NGHTTP2_PKG_CONFIG_H

#ifndef HAVE_ARPA_INET_H
#define HAVE_ARPA_INET_H 1
#endif
#ifndef HAVE_NETINET_IN_H
#define HAVE_NETINET_IN_H 1
#endif
#ifndef HAVE_CLOCK_GETTIME
#define HAVE_CLOCK_GETTIME 1
#endif
/* Static library build: NGHTTP2_EXTERN must be plain extern. */
#ifndef NGHTTP2_STATICLIB
#define NGHTTP2_STATICLIB 1
#endif

#endif /* NGHTTP2_PKG_CONFIG_H */
EOF
if sed --version >/dev/null 2>&1; then SED_I=(sed -i); else SED_I=(sed -i ''); fi
for f in "$OUT"/src/nghttp2_lib/*.c "$OUT"/src/nghttp2_lib/*.h; do
  "${SED_I[@]}" \
    -e 's|^#ifdef HAVE_CONFIG_H$|#if 1 /* embedded-temporal: bundled config (generated rewrite) */|' \
    -e 's|^#  include <config.h>$|#  include "nghttp2_pkg_config.h"|' \
    -e 's|^#  include "config.h"$|#  include "nghttp2_pkg_config.h"|' \
    "$f"
done
# Verifier is deliberately looser than the sed patterns above: ANY surviving
# config.h include, whatever its spacing, fails the build.
grep -rlE '#[[:space:]]*include[[:space:]]*[<"]config\.h[>"]' "$OUT/src/nghttp2_lib" >/dev/null 2>&1 \
  && die "config.h rewrite incomplete" || true
# And the rewrite must have actually happened somewhere.
grep -rl 'nghttp2_pkg_config.h"' "$OUT"/src/nghttp2_lib/*.h >/dev/null 2>&1 \
  || die "config.h rewrite touched no headers (upstream include form changed?)"

# Copy-completeness: the hand-listed cp globs above must keep pace with the
# vendored trees. Compare file counts against the source of truth.
count() { find "$1" -maxdepth 1 -name "$2" | wc -l | tr -d ' '; }
src_nghttp2_c=$(count "$REPO_ROOT/transport/esp/third_party/nghttp2/lib" '*.c')
out_nghttp2_c=$(count "$OUT/src/nghttp2_lib" '*.c')
[ "$src_nghttp2_c" = "$out_nghttp2_c" ] \
  || die "nghttp2 .c count mismatch: repo=$src_nghttp2_c layout=$out_nghttp2_c"
src_nghttp2_h=$(count "$REPO_ROOT/transport/esp/third_party/nghttp2/lib" '*.h')
out_nghttp2_h=$(( $(count "$OUT/src/nghttp2_lib" '*.h') - 1 ))  # minus generated pkg_config
[ "$src_nghttp2_h" = "$out_nghttp2_h" ] \
  || die "nghttp2 .h count mismatch: repo=$src_nghttp2_h layout=$out_nghttp2_h"

# ── 7. third-party license texts (nanopb zlib, nghttp2 MIT) ─────────────────
{
  printf '# Third-party licenses\n\n'
  printf 'This library vendors two third-party components. Their license texts\n'
  printf 'follow verbatim; the source files also retain their original headers.\n\n'
  printf '## nanopb %s (zlib license)\n\n' "$(sed -n 's/.*NANOPB_VERSION "nanopb-\(.*\)".*/\1/p' "$OUT/src/pb.h")"
  printf 'Source: https://github.com/nanopb/nanopb — files src/pb*.h, src/pb*.c\n\n```\n'
  cat "$REPO_ROOT/proto/third_party/nanopb/LICENSE.txt"
  printf '```\n\n## nghttp2 %s (MIT license)\n\n' "$(sed -n 's/.*NGHTTP2_VERSION "\(.*\)".*/\1/p' "$OUT/src/nghttp2/nghttp2ver.h")"
  printf 'Source: https://github.com/nghttp2/nghttp2 — files src/nghttp2/, src/nghttp2_lib/\n\n```\n'
  cat "$REPO_ROOT/transport/esp/third_party/nghttp2/COPYING"
  printf '```\n'
} > "$OUT/THIRD_PARTY_LICENSES.md"

# ── 8. layout sanity checks (the DoD exclusions) ────────────────────────────
symlinks=$(find "$OUT" -type l | wc -l | tr -d ' ')
[ "$symlinks" = "0" ] || { find "$OUT" -type l; die "layout contains $symlinks symlink(s)"; }
for bad in build .pio .venv test tests third_party vendor; do
  [ -e "$OUT/$bad" ] && die "forbidden entry in layout root: $bad"
done
for required in library.json library.properties LICENSE README.md \
                THIRD_PARTY_LICENSES.md src/EmbeddedTemporal.h \
                examples/BlinkActivity/BlinkActivity.ino \
                src/mwf/contracts.h src/mwf_core/worker_loop.cpp \
                src/mwf_codec/payload_codec.cpp src/nanopb_worker_adapter.cpp \
                src/pb_decode.c src/esp_transport.cpp src/grpc_h2.cpp \
                src/nghttp2/nghttp2.h src/nghttp2_lib/nghttp2_session.c; do
  [ -f "$OUT/$required" ] || die "missing required file: $required"
done
# The activity-only subset must not drag in the workflow/JSON side.
for excluded in src/mwf/device_workflow.pb.c src/nanopb_workflow_adapter.cpp \
                src/mwf_core/replay_engine.cpp src/mwf_core/json_util.cpp; do
  [ -e "$OUT/$excluded" ] && die "excluded file leaked into layout: $excluded"
done
grep -rn 'PB_FIELD_32BIT' "$OUT/library.json" "$OUT/library.properties" >/dev/null 2>&1 \
  && die "PB_FIELD_32BIT must not appear in the activity-only manifests" || true

files=$(find "$OUT" -type f | wc -l | tr -d ' ')
say "OK: $files files, 0 symlinks"
say "next: pio pkg pack (Path B), arduino-lint (Path C), pio ci (compile proof)"

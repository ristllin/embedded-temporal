#!/usr/bin/env bash
# gen_nanopb.sh — generate the nanopb device stubs from the Temporal proto subset.
# The device (ESP32-S3) side of IProtoCodec. nanopb emits static-alloc C
# (no libprotobuf on the device); nanopb/mwf.options bounds every string/bytes/
# repeated/map field so the generated structs fit fixed buffers.
#
# Output: gen/nanopb/  (mirrors package dirs, *.pb.c/*.pb.h). Host-compiled by the
# CMake mwf_proto_nanopb_smoke target to prove the generated C builds; the real
# link target is the PlatformIO device build.
set -euo pipefail
cd "$(dirname "$0")"

PB_PREFIX="$(brew --prefix protobuf 2>/dev/null || echo /opt/homebrew/opt/protobuf)"
PROTOC="$(command -v protoc || echo "$PB_PREFIX/bin/protoc")"
PB_INC="$PB_PREFIX/include"   # resolves google/protobuf/field_mask.proto (not vendored)
NANOPB="third_party/nanopb"
PLUGIN="$NANOPB/generator/protoc-gen-nanopb"

# The generator plugin runs under protoc's own `python3` (shebang). Make BOTH the
# nanopb generator modules AND the venv's protobuf package importable to it.
VENV_SP="$(cd "$(dirname "$0")" && pwd)/.venv/lib/python3.13/site-packages"
export PYTHONPATH="$NANOPB/generator:${VENV_SP}:${PYTHONPATH:-}"

OUT="gen/nanopb"
rm -rf "$OUT" && mkdir -p "$OUT"

# Generate the FULL closure request_response.proto reaches (it aggregates every
# WorkflowService RPC, so its transitive imports are effectively the whole tree)
# — same file set as the host twin so the two codecs cover identical messages,
# plus the google well-known types (nanopb bundles only descriptor.proto).
TEMPORAL_MSGS=$(cd third_party/temporal-api && find temporal/api -name '*.proto' ! -name 'service.proto' \
                 | sed 's#^#third_party/temporal-api/#')
# google well-known types by canonical path (resolved via -I; field_mask lives
# only in the protobuf include dir, the rest are vendored under temporal-api).
GOOGLE_MSGS="google/protobuf/any.proto \
             google/protobuf/duration.proto \
             google/protobuf/empty.proto \
             google/protobuf/field_mask.proto \
             google/protobuf/timestamp.proto \
             google/protobuf/wrappers.proto \
             google/protobuf/struct.proto"

"$PROTOC" -I third_party/temporal-api -I proto -I "$PB_INC" \
  --plugin=protoc-gen-nanopb="$PLUGIN" \
  --nanopb_out="$OUT" \
  --nanopb_opt="--options-file=nanopb/mwf.options" \
  $GOOGLE_MSGS $TEMPORAL_MSGS

echo "gen_nanopb: generated $(find "$OUT" -name '*.pb.c' | wc -l | tr -d ' ') .pb.c into $OUT/"

# ── DEVICE subset: the workflow-task trim the ESP32 actually links ────────────
# proto/mwf/device_activity.proto is a wire-identical mirror of ONLY the four
# activity-worker RPC messages + their closure (see that file's header), capped
# by nanopb/device_activity.options. Small enough that no -DPB_FIELD_32BIT is
# needed. Output is COMMITTED (gen/nanopb-device) so the PlatformIO device
# build needs no protoc/venv; conformance vs the real temporal messages is
# asserted by test/test_nanopb_device_conformance.cpp.
DEV_OUT="gen/nanopb-device"
rm -rf "$DEV_OUT" && mkdir -p "$DEV_OUT"

"$PROTOC" -I proto -I "$PB_INC" \
  --plugin=protoc-gen-nanopb="$PLUGIN" \
  --nanopb_out="$DEV_OUT" \
  --nanopb_opt="--options-file=nanopb/device_activity.options" \
  mwf/device_activity.proto

# device_workflow.proto: the WORKFLOW-TASK trim. IMPORTS device_activity
# (Payload/Payloads/WorkflowExecution/…), so it is generated in a SECOND pass
# with its OWN caps (nanopb/device_workflow.options); nanopb references the
# already-generated device_activity types by name. NOTE: this subset exceeds
# 64 kB per message at the configured caps, so consumers MUST compile with
# -DPB_FIELD_32BIT (proto/CMakeLists.txt and library.json already do; the
# generated .pb.c enforces it with an #error).
"$PROTOC" -I proto -I "$PB_INC" \
  --plugin=protoc-gen-nanopb="$PLUGIN" \
  --nanopb_out="$DEV_OUT" \
  --nanopb_opt="--options-file=nanopb/device_workflow.options" \
  mwf/device_workflow.proto

echo "gen_nanopb: device subset -> $DEV_OUT/ ($(find "$DEV_OUT" -name '*.pb.c' | wc -l | tr -d ' ') .pb.c)"

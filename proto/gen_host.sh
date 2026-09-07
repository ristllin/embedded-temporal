#!/usr/bin/env bash
# gen_host.sh — generate the libprotobuf C++ twin from the Temporal proto subset.
# The host side of the IProtoCodec contract (CONTRACTS.md §2).
#
# We compile the full vendored Temporal message/enum closure (every *.proto under
# temporal/api EXCEPT the official service.proto, which pulls in google/api
# grpc-gateway annotations we deliberately avoid) plus our slim WorkflowService.
# This guarantees the §2 message closure — and everything it transitively
# references — is available, without hand-maintaining an import list.
#
# Output: gen/host/  (mirrors the proto package dirs). Compiled by CMake into the
# mwf_proto_host static library linking protobuf::libprotobuf.
set -euo pipefail
cd "$(dirname "$0")"

PB_PREFIX="$(brew --prefix protobuf 2>/dev/null || echo /opt/homebrew/opt/protobuf)"
PROTOC="$(command -v protoc || echo "$PB_PREFIX/bin/protoc")"
PB_INC="$PB_PREFIX/include"

OUT="gen/host"
rm -rf "$OUT" && mkdir -p "$OUT"

# All Temporal message/enum/request_response protos EXCEPT the official
# service.proto (google/api gateway annotations we avoid — our slim service
# below replaces it, wire-identical for the worker RPC subset).
MSGS=$(cd third_party/temporal-api && find temporal/api -name '*.proto' ! -name 'service.proto' \
        | sed 's#^#third_party/temporal-api/#')

"$PROTOC" -I third_party/temporal-api -I proto -I "$PB_INC" \
          --cpp_out="$OUT" $MSGS

# Our slim WorkflowService (C++ messages only — the transport layer owns the gRPC stubs).
"$PROTOC" -I third_party/temporal-api -I proto -I "$PB_INC" \
          --cpp_out="$OUT" proto/mwf/worker_service.proto

echo "gen_host: generated $(find "$OUT" -name '*.pb.cc' | wc -l | tr -d ' ') .pb.cc into $OUT/"

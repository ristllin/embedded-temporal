# proto

To talk to Mistral/Temporal, the worker speaks the Temporal gRPC wire protocol,
whose messages are protobuf. This directory turns a subset of the Temporal `.proto`
files into C++ encode/decode code — twice, because the two workers can't share one
protobuf runtime:

- **host** (`cookbook_worker`) uses **libprotobuf**, which is large but effortless.
- **device** (the ESP32-S3) uses **nanopb**, which generates fixed-size C structs
  with no heap or dynamic allocation — the only kind that fits on the board.

Both realize the same `IProtoCodec<Msg>` contract (typed message ⇄ protobuf bytes)
from [`../contracts/CONTRACTS.md`](../contracts/CONTRACTS.md) §2, so the worker core
never sees which runtime is underneath.

## Layout

```
proto/mwf/worker_service.proto     slim WorkflowService — the RPC subset a worker needs
proto/mwf/device_activity.proto    device nanopb trim: activity-worker messages
proto/mwf/device_workflow.proto    device nanopb trim: workflow-task messages
third_party/temporal-api/          vendored Temporal protos (the full message/enum closure)
third_party/nanopb/                vendored nanopb runtime + generator (pinned 0.4.9.1)
nanopb/*.options                   field caps (max_size / max_count) for each nanopb pass
gen_host.sh                        protoc → libprotobuf C++  → gen/host/       (git-ignored)
gen_nanopb.sh                      protoc → nanopb C         → gen/nanopb/ + gen/nanopb-device/
gen/proto_codec.h                  the host IProtoCodec<Msg> twin + type aliases (committed)
gen/nanopb-device/                 the committed device subset the ESP32 links (committed)
test/                              host round-trip + cross-runtime conformance
CMakeLists.txt                     builds the host lib and the test targets
```

## The two generators

Run these before configuring CMake — the build compiles what they emit.

```bash
brew install grpc protobuf                                   # protoc + libprotobuf 35.1
python3 -m venv .venv && .venv/bin/pip install protobuf grpcio-tools   # for the nanopb plugin
./gen_host.sh        # → gen/host/*.pb.cc   (libprotobuf, host)
./gen_nanopb.sh      # → gen/nanopb/*.pb.c  (nanopb, host smoke) + gen/nanopb-device/ (device)
```

**`gen_host.sh`** runs `protoc --cpp_out` over the *entire* vendored Temporal
closure — every `temporal/api/**.proto` except the official `service.proto` (which
drags in `google/api` grpc-gateway annotations we don't want) — plus our slim
[`worker_service.proto`](proto/mwf/worker_service.proto). Compiling the whole
closure means every message §2 references is present without hand-maintaining an
import list. The result is ~53 `.pb.cc` files that link into the `mwf_proto_host`
static library. A single template covers all of them, because every libprotobuf
message is a `MessageLite`:

```cpp
// gen/proto_codec.h
template <class Msg> struct ProtoCodec final : IProtoCodec<Msg> {
  Bytes encode(const Msg& m) override;              // SerializeToString
  bool  decode(const Bytes& b, Msg& out) override;  // ParseFromArray
};
```

**`gen_nanopb.sh`** runs the nanopb generator over the same file set (plus the
google well-known types, which nanopb doesn't bundle). It produces two things:

1. `gen/nanopb/` — the full closure, host-compiled by the `mwf_proto_nanopb_smoke`
   target only to prove the caps and codegen hold across every message. It is
   **not** what the device links; it's too big (see below).
2. `gen/nanopb-device/` — the two hand-trimmed subsets the ESP32 actually links,
   **committed to git** so the PlatformIO device build needs no protoc or venv.

## Committed vs. generated

`gen/host/` and `gen/nanopb/` are regenerated and git-ignored. What's committed:

- [`gen/proto_codec.h`](gen/proto_codec.h) — hand-written, the host codec twin.
- [`gen/nanopb-device/`](gen/nanopb-device/) — the device subset's generated
  `.pb.c` / `.pb.h`, plus the hand-written `nanopb_worker_adapter.*` and
  `nanopb_workflow_adapter.*` that wrap them as the core's `IWorkerProtoAdapter`.

Committing the device output is deliberate: the firmware build ([`library.json`](library.json))
pulls `gen/nanopb-device/*.pb.c` and the nanopb runtime straight into the image
with `-DPB_FIELD_32BIT`, so flashing a board never requires the codegen toolchain.
Re-run `gen_nanopb.sh` and commit the result whenever you change a device `.proto`
or its `.options`.

## The device subsets — why a trim, not the full closure

nanopb allocates statically, so a message's struct is sized to its worst case up
front. The full Temporal closure has messages that are impossible to statically
allocate on a microcontroller:

- **`oneof`** becomes a C union sized to its *largest* arm. `HistoryEvent` has 60+
  arms; one of them, `MarkerRecordedEventAttributes`, is a ~346 KB aggregate that
  blows past nanopb's 64 KB / 16-bit ceiling on its own.
- **maps** (`map<string, bytes>`, e.g. `Payload.metadata`) expand to a repeated
  synthetic `…Entry` submessage — fine once capped, but each cap multiplies.
- **recursion** (`Failure.cause` → `Failure`) has no finite static size, so nanopb
  turns it into a callback the caller must service.

So the device doesn't link the full closure. It links two wire-identical mirrors
that carry only the messages and fields its worker loops touch:

| file | RPCs it serves | imports |
|---|---|---|
| [`device_activity.proto`](proto/mwf/device_activity.proto) | `PollActivityTaskQueue`, `RespondActivityTaskCompleted`, `RespondActivityTaskFailed` | — |
| [`device_workflow.proto`](proto/mwf/device_workflow.proto) | `PollWorkflowTaskQueue`, `RespondWorkflowTaskCompleted`, `SignalWorkflowExecution` | `device_activity.proto` |

`device_workflow.proto` imports `device_activity.proto` for the shared closure
(`Payload`, `Payloads`, `WorkflowExecution`, `Workflow`/`ActivityType`, `TaskQueue`,
`Failure`) so there's exactly one on-device `Payload` struct.

These are byte-compatible with `temporal.api.*` because **protobuf carries field
numbers on the wire, not names** — the mirror keeps each field at its original
Temporal number and simply omits the fields the device doesn't read. Omitted
`oneof` arms (like MarkerRecorded) aren't in the descriptor, so on decode their
tags are skipped as unknown fields. `HistoryEvent.attributes` keeps 13 of the
original 60+ arms; `Command.attributes` keeps 4 of 18. Wire-identity is checked
against the real `temporal.api` messages and captured golden histories in
[`test/test_nanopb_device_conformance.cpp`](test/test_nanopb_device_conformance.cpp)
and [`test/test_nanopb_device_workflow_conformance.cpp`](test/test_nanopb_device_workflow_conformance.cpp).

**Why `-DPB_FIELD_32BIT`.** A full workflow history decodes to a large struct — a
`History` of up to 64 events, each event's union sized to a `Payloads` (4 × an
8 KB `Payload.data`) ≈ 39 KB, so `PollWorkflowTaskQueueResponse` reaches ~2.5 MB.
That's heap-allocated per poll on the ESP32's PSRAM, never the stack or statics.
The device build compiles the whole nanopb subset with `PB_FIELD_32BIT` (one
global `pb_size_t`, consistent across every linked nanopb translation unit).

## Caps — the `.options` files

nanopb needs an upper bound on every string, bytes, repeated, and map field to
size its structs. Those bounds live in the `.options` file for each generation
pass — [`nanopb/mwf.options`](nanopb/mwf.options) (full closure),
[`nanopb/device_activity.options`](nanopb/device_activity.options), and
[`nanopb/device_workflow.options`](nanopb/device_workflow.options).

Two rules worth knowing before you edit them:

- **Caps are policy, not protocol.** A payload larger than its cap fails to
  decode. Tune them to real traffic; they only bound on-device buffers.
- **The last matching line wins.** Wildcard defaults (`* max_size:…`) go at the
  top, specific overrides below.

One override is a scar worth reading before you lower a default: a real Mistral
history carries a **64-hex-character execution id** in `workflow_id`, and nanopb
reserves a byte for the NUL. The old `max_size:64` default held only 63 chars, so
a live poll aborted with a "string overflow" — while the golden fixtures (short
synthetic ids) never tripped it. The default is now `256`. The reasoning is
documented at the top of [`nanopb/device_workflow.options`](nanopb/device_workflow.options),
and the regression is pinned by the real-history probe in
[`test/test_nanopb_device_workflow_conformance.cpp`](test/test_nanopb_device_workflow_conformance.cpp).

> `device_activity.proto` and `device_workflow.proto` share types (`Payload`,
> `Failure`, the identity messages). nanopb computes each embedded field's width
> from the caps it sees *in that pass*, so the shared-type caps are duplicated in
> both `.options` files and **must stay identical** — mismatched widths fail to
> compile (`FIELDINFO_DOES_NOT_FIT`). That's why the two files repeat those lines.

## When you'd touch this — adding a Temporal RPC

1. **Host side.** If the RPC's request/response messages already exist in the
   vendored `temporal-api` closure (they usually do), add the `rpc` line to
   [`proto/mwf/worker_service.proto`](proto/mwf/worker_service.proto) and re-run
   `./gen_host.sh`. The gRPC method path is derived from package + service +
   method, so it's wire-identical to real Temporal automatically — no extra work.
2. **Device side.** Only if the ESP32 worker loop needs the RPC. Add its messages
   and fields to the relevant `device_*.proto` trim (or a new one), **at their
   original Temporal field numbers**; add caps for every string/bytes/repeated/map
   field to the matching `.options`; run `./gen_nanopb.sh` and commit the
   regenerated `gen/nanopb-device/*`.
3. **Prove it.** Add a case to the conformance test that encodes with one runtime
   and decodes with the other, so the trim can't silently drift from `temporal.api`.

## Build & test

```bash
cmake -B build -S . -DCMAKE_PREFIX_PATH="$(brew --prefix protobuf);$(brew --prefix abseil)"
cmake --build build -j
cd build && ctest --output-on-failure
```

protobuf 35 (Homebrew) is Abseil-based — use its CMake `protobuf` **CONFIG**
package, not the legacy `FindProtobuf` module, or the generated code can't find
the `absl/…` headers. nanopb is pinned to **0.4.9.1**: 1.0.0-dev emits a broken
self-referential `_Failure_size` macro that fails to compile wherever `Failure`
is embedded.

## Reference

Every worker RPC path is `/temporal.api.workflowservice.v1.WorkflowService/<Method>`
— e.g. `/…/PollWorkflowTaskQueue`, `/…/RespondActivityTaskCompleted`. The full set
is the `rpc`s in [`worker_service.proto`](proto/mwf/worker_service.proto); the
transport layer passes these as `ITransport::call`'s `fullMethod`.

Host C++ types live in the `temporal::api::*::v1` namespaces; the device nanopb
structs use the flattened `temporal_api_*_v1_*` names (and, for the trims, the
`mwf_device_v1_*` package). [`gen/proto_codec.h`](gen/proto_codec.h) re-exports the
host types under `mwf::proto::` and holds the full logical-name → type mapping for
the replay engine and transport layer to consume.

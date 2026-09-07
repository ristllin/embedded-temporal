# contracts

`core/` is one worker engine — it replays a workflow spec, runs activities, and
drives the poll loops. It has to run in two very different places: your desktop
and an ESP32-S3. It does that by never touching a concrete HTTP stack, protobuf
library, JSON library, or flash driver directly. It talks to a handful of small
C++ interfaces. **This directory holds those interfaces.**

Each interface has two implementations — one built with desktop libraries, one
built for the device — and `core/` is compiled against neither. Swap the
implementations, keep the engine.

| | desktop (`cookbook_worker`) | device (firmware) |
|---|---|---|
| gRPC transport | grpc++ generic stub, TLS | vendored nghttp2 + mbedTLS (PSRAM), ALPN `h2` |
| protobuf | libprotobuf | nanopb |
| REST control plane | libcurl | single-write HTTPS |
| JSON | nlohmann/json | ArduinoJson |
| durable store | in-memory | NVS + LittleFS |

## The headers here

C++17, no Arduino, no exceptions across a seam — errors come back as
[`Result<T>`](include/mwf/types.h). JSON crosses seams as raw bytes (`mwf::Bytes`)
so neither JSON library leaks into the interface.

- **[`include/mwf/types.h`](include/mwf/types.h)** — `Bytes`, `Metadata`,
  `Result<T>`, and the `mwf::json` alias (nlohmann on host, ArduinoJson on
  device; core code only ever names the alias).
- **[`include/mwf/temporal_types.h`](include/mwf/temporal_types.h)** —
  lib-agnostic value mirrors of the wire structs the seams pass around:
  `temporal::Payload` and the Mistral `WorkflowContext`. These are plain
  structs, *not* protobuf classes.
- **[`include/mwf/contracts.h`](include/mwf/contracts.h)** — the interfaces
  themselves.

## The seams

| interface | what it does | host impl | device impl |
|---|---|---|---|
| **`ITransport`** | one long-poll-aware gRPC unary call: `(fullMethod, request bytes) → (status, response bytes)`. The engine never sees HTTP/2 or TLS. | `DesktopTransport` — [`transport/src/desktop_transport.h`](../transport/src/desktop_transport.h) | `EspTransport` — [`transport/esp/esp_transport.h`](../transport/esp/esp_transport.h) |
| **`IProtoCodec<Msg>`** | typed Temporal message ⇄ protobuf wire bytes. Keeps the protobuf library out of `core/`. Lives next to the generated protos, not here. | `ProtoCodec<Msg>` (libprotobuf) — [`proto/gen/proto_codec.h`](../proto/gen/proto_codec.h) | nanopb `pb_encode`/`pb_decode`, generated from `proto/` |
| **`IPayloadCodec`** | splits/wraps the Mistral `json/wf_v1` envelope: `Payload ⇄ (argument JSON, WorkflowContext)`. Pure metadata-map + byte passthrough, so **one impl serves both sides**. | `PayloadCodecV1` — [`codec/include/mwf_codec/payload_codec.h`](../codec/include/mwf_codec/payload_codec.h) | *(same)* |
| **`IActivityRegistry`** | name → `ActivityFn`, and dispatch. Portable, so **one impl serves both sides**. | `ActivityRegistry` — [`core/include/mwf_core/activity_registry.h`](../core/include/mwf_core/activity_registry.h) | *(same)* |
| **`IDurableStore`** | key/value that survives a power cycle: in-flight replay state (`exec/<runId>`) + task tokens (`token/<runId>`). | `MemStore` fake (examples/tests) | `DurableStore` (NVS + LittleFS) — [`firmware/src/durable_store.h`](../firmware/src/durable_store.h) |
| **workflow-proto adapter** — `IWorkflowProtoAdapter` / `IWorkerProtoAdapter` | maps the poll loops' portable structs ⇄ the Temporal `Poll*` / `Respond*` messages, including translating a protobuf `History` into the engine's `mwf_core::History`. All protobuf knowledge lives here. Declared with the loops, not here. | `HostWorkflowAdapter` / `HostProtoAdapter` — [`transport/example/host_workflow_adapter.h`](../transport/example/host_workflow_adapter.h) | `Nanopb…Adapter` — [`proto/gen/nanopb_workflow_adapter.h`](../proto/gen/nanopb_workflow_adapter.h), [`nanopb_worker_adapter.h`](../proto/gen/nanopb_worker_adapter.h) |
| **`IClock` / `IRandom`** | the only sources of nondeterminism, both recorded to / replayed from history so a re-run reproduces exactly. | — | — |

Two of these seams (`IProtoCodec` and the workflow-proto adapters) deliberately
live *outside* `include/mwf/` — they carry protobuf types, and putting them here
would drag a protobuf library into the interface package. `contracts/` holds only
the library-agnostic value seams.

`IWorkflowSpec` from the grammar is not a vtable at all: a workflow is JSON
**data** the replay engine interprets (that is what makes replay deterministic
without a sandbox). Its grammar reference is
[`core/include/mwf_core/workflow_spec.h`](../core/include/mwf_core/workflow_spec.h).

## Why one core runs both

The two workers assemble the *same* `WorkflowLoop` and `WorkerLoop` from `core/`.
Only the objects behind the seams differ.

```cpp
// desktop — transport/example/sole_worker_main.cpp
mwf_transport::DesktopTransport  transport(target, bearer);   // grpc++ + TLS
mwf_codec::PayloadCodecV1        codec;
mwf_core::ActivityRegistry       registry;
mwf_example::HostWorkflowAdapter wfAdapter;                    // libprotobuf
MemStore                         store;                        // in-memory

mwf_core::WorkflowLoop wfLoop(transport, codec, wfAdapter, store, provider, wcfg);
```

```cpp
// device — firmware/src/main.cpp
auto transport = std::make_unique<mwf_transport::EspTransport>(target, bearer); // nghttp2 + mbedTLS
mwf_codec::PayloadCodecV1        codec;
mwf_core::ActivityRegistry       registry;
mwf_proto::NanopbWorkflowAdapter wfAdapter;                    // nanopb
mwf_device::DurableStore         store;                        // NVS + LittleFS

auto wfLoop = std::make_unique<mwf_core::WorkflowLoop>(
    *transport, codec, wfAdapter, store, provider, wcfg);
```

Same `WorkflowLoop`, same replay engine, same activity dispatch. The four objects
in front of it are the whole difference between "runs on my laptop" and "runs on
a $10 board."

An activity is the one seam you write yourself. Its signature never changes
across host and device ([`contracts.h`](include/mwf/contracts.h)):

```cpp
using ActivityFn = std::function<Result<Bytes>(const Bytes& arg_json)>;
```

See [docs/writing-a-workflow.md](../docs/writing-a-workflow.md) for how to
register one.

## Consuming this package

The device build pulls it in as a PlatformIO dependency (`symlink://../contracts`);
host builds add its include path via CMake. It is headers only.

## More

- **[docs/contracts.md](../docs/contracts.md)** — every seam in depth: exact
  method semantics, the two roles a device plays, versioning.
- **[docs/architecture.md](../docs/architecture.md)** — how the loops, replay
  engine, and these seams fit together end to end.
- **[spec/codec-wire-format.md](spec/codec-wire-format.md)** — the byte format
  `IPayloadCodec` implements (the Mistral `WorkflowContext` envelope).

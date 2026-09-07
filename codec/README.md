# codec

`PayloadCodecV1` — the step that packs an activity's JSON into Mistral's wire
format and unpacks it on the way back.

In this project an activity is a function that takes JSON and returns JSON (see
[../docs/writing-a-workflow.md](../docs/writing-a-workflow.md)). Before that JSON
can travel to Mistral's Temporal frontend it has to be wrapped in a Temporal
`Payload`, with a little routing context — which execution this belongs to —
riding alongside it. The codec is that wrapping step, and nothing more.

## Two operations

It implements `mwf::IPayloadCodec`
([../contracts/CONTRACTS.md](../contracts/CONTRACTS.md)), which is two methods:

| direction | method | does |
|---|---|---|
| inbound (server → worker) | `decodeActivityInput(Payload)` | split a `Payload` into the argument JSON + the `WorkflowContext` |
| outbound (worker → server) | `encodeActivityResult(resultJson, ctx, empty)` | wrap the result JSON back into a `Payload`, echoing that same context |

You rarely call it directly. The worker loops (`WorkerLoop`, `WorkflowLoop` in
[../core/](../core/)) own a codec and call it for you — you construct one and hand
it in. Direct use looks like this:

```cpp
#include "mwf_codec/payload_codec.h"

mwf_codec::PayloadCodecV1 codec;

// inbound: Temporal Payload -> (argument JSON, context)
auto decoded = codec.decodeActivityInput(payload);
if (!decoded) { /* decoded.error, e.g. missing/unsupported encoding */ }
mwf::Bytes argJson = decoded.value.argument_json;   // hand this to the activity

// ...the activity runs: argJson in, resultJson out...

// outbound: result JSON -> Temporal Payload, echoing the same context back
mwf::temporal::Payload out =
    codec.encodeActivityResult(resultJson, decoded.value.context, /*empty=*/false);
```

## What `json/wf_v1` is

`json/wf_v1` is Mistral's payload encoding. A Temporal `Payload` has two parts,
and the codec uses them like so:

| `Payload` part | carries |
|---|---|
| `data` | the activity argument or result — **raw JSON bytes, passed through untouched** |
| `metadata` | the `WorkflowContext` (`execution_id`, `namespace`, …) as a string→bytes map |

In v1 encryption, offload, and compression are all off, so the body transform is
identity: the exact bytes that arrive are the exact bytes handed to the activity,
and vice versa. Because of that the codec parses **no JSON** — the body crosses
the seam as `mwf::Bytes` — so it pulls in no JSON library and is the same code on
the ESP32 as on the desktop.

Encodings it handles:

| encoding | decode | encode |
|---|---|---|
| `json/wf_v1` | yes — rebuilds the context; **rejects a missing/empty `execution_id`** | yes — every result is emitted as this |
| `json/plain` | yes — stock Temporal payloads, no context (empty `WorkflowContext`) | no |
| `json/abraxas_v1` (legacy) / anything else | rejected | — |

## Files

```
include/mwf_codec/payload_codec.h   PayloadCodecV1 + the wire constants (kEncoding*, metadata keys)
src/payload_codec.cpp               the metadata-map transform + byte passthrough (~100 lines)
test/test_payload_codec.cpp         encode/decode + round-trip vectors
test/test_goldens.cpp               real captured Payloads (self-skips when absent)
```

## Build & test (host)

```bash
brew install nlohmann-json googletest
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Depends on [../contracts/](../contracts/) (for `mwf::IPayloadCodec` and the
Temporal types), `nlohmann/json` (the host `mwf::json` backing, used only by the
golden test), and GoogleTest.

## The byte-level spec

The wire is defined by the spec, not this code. Read
**[../docs/wire-format.md](../docs/wire-format.md)** for the byte-by-byte
walkthrough: every metadata key, the `empty_payload` `0x00` sentinel, `json/plain`
compat, and worked examples from real captures. The frozen byte authority it
derives from is
[../contracts/spec/codec-wire-format.md](../contracts/spec/codec-wire-format.md).

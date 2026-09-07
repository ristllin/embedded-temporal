# embedded-temporal

Run a **Mistral Workflows / Temporal activity worker on an ESP32**. The board
registers as a worker, long-polls its task queue for activity tasks over
gRPC/TLS, runs your handler, and reports the result — no cloud SDK, no host
machine in the loop.

This package is the self-contained **device activity-worker subset** of the
[embedded-temporal](https://github.com/ristllin/embedded-temporal) project,
generated from that monorepo (which also holds the host twin, the workflow
replay engine, and the conformance suite that validates this code against the
real Temporal/Mistral wire protocol).

## What's inside

- `mwf::` — the frozen portable contracts (`ITransport`, `IPayloadCodec`,
  `IActivityRegistry`, value structs). C++17, no Arduino types in the seams.
- `mwf_core::WorkerLoop` — the poll → decode → dispatch → respond loop.
  Portable; all protobuf knowledge sits behind an adapter seam.
- `mwf_codec::PayloadCodecV1` — the Mistral `WorkflowContext` envelope codec
  (`json/wf_v1`, with `json/plain` decode compatibility). No JSON library
  needed: arguments and results cross the seam as raw JSON bytes.
- `mwf_proto::NanopbWorkerAdapter` — the `temporal.api` activity-worker
  messages as a wire-identical nanopb device subset (bundled nanopb runtime).
- `mwf_transport::EspTransport` — gRPC-over-HTTP/2 on `WiFiClientSecure`
  (mbedTLS, ALPN `h2`) with a bundled nghttp2. One persistent connection is
  reused across calls; reconnects on GOAWAY/io errors.

## Requirements

- **arduino-esp32 core 3.x** (the transport uses `NetworkClientSecure`, which
  does not exist in core 2.x). This matters for how you install:
  - **Arduino IDE:** Boards Manager → "esp32 by Espressif Systems" **3.0.0 or
    newer**. Nothing else needed.
  - **PlatformIO:** the stock `platform = espressif32` still ships Arduino
    core 2.0.17 and will NOT compile this library. Use a platform build that
    carries core 3.x, e.g. the pioarduino fork this library is validated
    against:
    ```ini
    [env:esp32s3]
    platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip
    board = esp32-s3-devkitc-1
    framework = arduino
    lib_deps = ristllin/embedded-temporal@^0.1.1
    ```
- An ESP32 with TLS-capable network access to your Temporal / Mistral
  Workflows frontend (tested on ESP32-S3). The activity subset needs no PSRAM
  and no compile-time flags.

## Quick start

See `examples/BlinkActivity` — a complete worker in one sketch:

```cpp
#include <WiFi.h>
#include <EmbeddedTemporal.h>

mwf_transport::EspTransport   transport("wf-scheduler.mistral.ai:443", "api-key");
mwf_codec::PayloadCodecV1     codec;
mwf_proto::NanopbWorkerAdapter adapter;
mwf_core::ActivityRegistry    registry;

void setup() {
  // ... join WiFi ...
  registry.registerActivity("device.blink", [](const mwf::Bytes& arg) {
    // arg = the activity input as raw JSON bytes; return JSON bytes back
    std::string out = "{\"ok\":true}";
    return mwf::Result<mwf::Bytes>::success(mwf::Bytes(out.begin(), out.end()));
  });
}

void loop() {
  static mwf_core::WorkerLoop worker(transport, codec, registry, adapter, [] {
    mwf_core::WorkerConfig c;
    c.ns = "your-namespace";
    c.task_queue = "esp32-blink";
    c.identity = "esp32@device";
    c.call_metadata["temporal-namespace"] = c.ns;
    return c;
  }());
  worker.runOnce(70000);  // one long-poll + dispatch + respond cycle
}
```

Any Temporal or Mistral Workflows client can then schedule an activity named
`device.blink` on that task queue and the board will execute it.

## Install

- **PlatformIO:** `lib_deps = ristllin/embedded-temporal@^0.1.1`
- **Arduino IDE:** Library Manager, search "EmbeddedTemporal"
- **From git:** `lib_deps = https://github.com/ristllin/embedded-temporal.git`
  (the full monorepo; this package is its packaged device subset)

## Security note

`EspTransport` currently uses `setInsecure()` (no server certificate
verification), matching the upstream project's current device posture — the
bearer token still rides inside TLS, but the server is not authenticated.
Treat networks and namespaces accordingly; CA pinning is on the upstream
roadmap.

## License

MIT (c) 2026 Roy Darnell <contact@cumulo-nimbus.ai>. Bundles nanopb (zlib
license) and nghttp2 (MIT) — see `THIRD_PARTY_LICENSES.md`.

# transport

How the worker talks to Mistral. A worker does two things over the network — it
polls the Temporal data plane for work (gRPC) and it manages workflows on the
REST control plane (register, trigger, check status). This directory holds both
sides, plus the example binaries that use them.

The data plane hides behind one interface, `mwf::ITransport`, so
[`core/`](../core/) and the worker loops never see HTTP/2 or a protobuf library.
There are two implementations of that interface: a desktop one built on grpc++,
and an ESP32 one built on a vendored nghttp2 + mbedTLS. They're
interchangeable — the same worker code runs on either.

```
   worker loop  ──►  mwf::ITransport::call(method, bytes, metadata, deadline)
                          │
              ┌───────────┴───────────┐
        DesktopTransport          EspTransport
        (grpc++, desktop)     (nghttp2+mbedTLS, ESP32)
```

## The seam

`ITransport` is one blocking unary call. The worker loop owns the thread; the
transport just moves bytes.

```cpp
// contracts/include/mwf/contracts.h
struct GrpcResult {
  int         grpc_status = 0;   // 0 OK; 4 DEADLINE_EXCEEDED — empty long-poll, re-poll
  std::string message;           // the grpc-message trailer
  Bytes       response;          // response protobuf bytes; empty unless grpc_status == 0
};

struct ITransport {
  virtual GrpcResult call(std::string_view fullMethod, const Bytes& requestMsg,
                          const Metadata& metadata, int deadlineMs) = 0;
  virtual void close() = 0;
};
```

`fullMethod` is a Temporal method path, e.g.
`/temporal.api.workflowservice.v1.WorkflowService/PollActivityTaskQueue`. The
request/response bytes are opaque protobuf — [`proto/`](../proto/) encodes and
decodes them on the far side of this seam, so the transport links against **zero**
generated message code. `metadata` is a string map (the worker puts
`temporal-namespace` here); the API key rides in as `authorization: Bearer …`.

One rule the whole system depends on: **`grpc_status == 4` (`DEADLINE_EXCEEDED`)
on a poll is normal, not an error.** A long-poll that finds no work returns
empty; the loop just polls again.

## Two implementations

| impl | file | built by | stack |
|---|---|---|---|
| `DesktopTransport` | [`src/desktop_transport.cpp`](src/desktop_transport.cpp) | CMake (here) | grpc++ generic stub |
| `EspTransport` | [`esp/esp_transport.cpp`](esp/esp_transport.cpp) | PlatformIO (via [`firmware/`](../firmware/)) | vendored nghttp2 + mbedTLS |

Both satisfy `ITransport`, so `core` is transport-agnostic. The desktop one is
what you run while developing; the device one is what ships in the firmware.

## Build & run (desktop)

You need a C++17 toolchain, grpc/protobuf, curl, and a Mistral API key.

```bash
brew install cmake protobuf grpc abseil nlohmann-json curl openssl    # macOS
./proto/gen_host.sh     # from the repo root: generate the protobuf C++ (git-ignored)
cd transport
cmake -B build && cmake --build build --target cookbook_worker
```

> If CMake can't find grpc/protobuf, point it at them:
> `cmake -B build -DCMAKE_PREFIX_PATH="$(brew --prefix grpc);$(brew --prefix protobuf)"`.
> They must be found in **CONFIG** mode — grpc's own `find_dependency(Protobuf)`
> is CONFIG, and mixing in the legacy `FindProtobuf` module double-defines the
> `protobuf::*` targets.

`cookbook_worker` is the binary you actually run — it's the desktop worker
behind [`cookbooks/`](../cookbooks/). It registers a workflow, then runs both the
workflow loop and the activity loop on one queue:

```bash
export MISTRAL_API_KEY=...        # or a gitignored .env; never hardcode

# start the worker: register `greet` on queue greet-q, run until one completion
build/cookbook_worker --mode worker --spec ../greet.json \
  --workflow greet --queue greet-q --max-completions 1

# in another shell, trigger a run and wait for COMPLETED
build/cookbook_worker --mode trigger --workflow greet --queue greet-q \
  --input-json '{"name":"Ada"}'
```

Modes: `--mode worker` (register + run both loops), `--mode trigger` (execute +
poll to terminal), `--mode hitl` (trigger, prove the pause, send a signal — see
[cookbook 04](../cookbooks/04-human-in-the-loop/)), `--mode cleanup` (archive the
deployment). `--input-json` takes a raw JSON value; `--input-file` reads it from
disk. The cookbooks wrap this whole cycle in a `run_local.sh`.

## DesktopTransport

Uses grpc++'s **generic stub** (`grpc::GenericStub` + `grpc::ByteBuffer`), not a
generated service stub — that's the natural fit for `ITransport::call`, which is
already `(method, request bytes) → (status, response bytes)`.

```cpp
#include "desktop_transport.h"

// target, bearer, tls: TLS is ON by default (validating SslCredentials); pass
// tls=false only for a local plaintext dev frontend. Empty bearer = no
// Authorization header (e.g. a local dev server) — on an insecure channel the
// transport refuses to send a bearer at all.
mwf_transport::DesktopTransport t("wf-scheduler.mistral.ai:443", apiKey);

auto r = t.call("/temporal.api.workflowservice.v1.WorkflowService/PollActivityTaskQueue",
                requestBytes, {{"temporal-namespace", ns}}, /*deadlineMs=*/70000);
if (r.grpc_status == 0)  { /* got a task in r.response */ }
else if (r.grpc_status == 4) { /* empty poll — loop again */ }
```

The channel dials with `SslCredentials` unless TLS is explicitly opted out
(`InsecureChannelCredentials`, credential-free local dev only). Each unary call
runs synchronously over a private
completion queue; `deadlineMs` becomes the gRPC deadline (Temporal requires one
on every poll).

## RestClient

The control plane is plain HTTPS/JSON against `api.mistral.ai`, separate from the
gRPC data path. Desktop uses libcurl; the device reuses the same source over
mbedTLS. [`src/rest_client.h`](src/rest_client.h) is the full surface — the ones
the worker uses:

| method | HTTP | purpose |
|---|---|---|
| `whoami()` | `GET /v1/workflows/workers/whoami` | discover the gRPC scheduler + namespace |
| `registerWorkflow(...)` | `POST /v1/workflows/register` | deploy a workflow on a queue (declares its signals) |
| `workflowActive(name)` | `GET /v1/workflows/{name}` | is the deployment dispatchable yet |
| `executeWorkflow(...)` | `POST /v1/workflows/{name}/execute` | trigger a run → execution id |
| `getExecution(id)` | `GET /v1/workflows/executions/{id}` | status + result |
| `signalExecution(...)` | `POST /v1/workflows/executions/{id}/signals` | deliver a `wait_signal` payload |
| `archiveWorkflow(name)` | `PUT /v1/workflows/{name}/archive` | stop dispatch (there is no delete) |

`whoami()` returns exactly what the gRPC transport needs — `scheduler_url`
becomes the `DesktopTransport`/`EspTransport` target and `namespace_` becomes the
`temporal-namespace` metadata:

```cpp
mwf_transport::RestClient rest("https://api.mistral.ai", apiKey);
auto who = rest.whoami();
if (who) {
  // who.value.scheduler_url == "wf-scheduler.mistral.ai:443", who.value.tls == true
}
```

> A `wait_signal` workflow **must** declare its signal names in
> `registerWorkflow`, or the frontend rejects the signal with HTTP 422. The
> cookbook worker walks the spec and builds that list automatically
> (`buildSignalsJson` in [`example/cookbook_worker_main.cpp`](example/cookbook_worker_main.cpp)).

## The ESP32 stack (`esp/`)

The device speaks the same gRPC-over-HTTP/2, but there's no grpc++ on an ESP32,
so the framing is hand-built on nghttp2 over a `WiFiClientSecure` (mbedTLS)
stream. Three files, one framing core:

| file | role |
|---|---|
| [`esp/grpc_h2.{h,cpp}`](esp/grpc_h2.h) | portable core: nghttp2 session + gRPC framing (5-byte length prefix, `grpc-timeout`, trailers → `grpc_status`, GOAWAY handling) against abstract send/recv/now callbacks. No Arduino, no sockets, no TLS. |
| [`esp/esp_transport.{h,cpp}`](esp/esp_transport.h) | the device stream: `EspTransport` over `WiFiClientSecure` (ALPN `h2`), one persistent h2 connection reused across polls, task-watchdog feed inside the long recv window. |
| [`esp/host/h2_host_stream.{h,cpp}`](esp/host/h2_host_stream.h) | a host twin of that stream (POSIX TCP + OpenSSL) that drives the *same* `grpc_h2` core, so the framing can be exercised on desktop. |

`EspTransport` has the same constructor shape and the same `call()` as the
desktop twin — [`firmware/src/main.cpp`](../firmware/src/main.cpp) builds one and
hands it to both loops. Three device realities it lives with:

- **mbedTLS must be PSRAM-routed.** The handshake + nghttp2 session (~24–28 KB)
  won't fit in internal SRAM alongside the rest of the firmware; the firmware
  installs a PSRAM allocator (`mwf::installPsramMbedtls()`) before constructing
  the transport.
- **TLS is `setInsecure()`** (no cert validation), matching the device fleet's
  current posture. See [`SECURITY.md`](../SECURITY.md) before using a real key.
- **Long-poll timing:** Mistral's frontend holds `PollActivityTaskQueue` ~45 s
  then returns an empty OK (treat as idle, re-poll); the client deadline stays
  ~70 s. The recv loop yields and feeds the task watchdog each window so a
  blocking poll inside `loop()` can't trip it.

### Vendored nghttp2

[`esp/third_party/nghttp2/`](esp/third_party/nghttp2/) is nghttp2 **1.68.0**
`lib/` sources, vendored because no Arduino-framework PlatformIO package ships a
reachable nghttp2. It's pure C with no OS deps; the only build knobs are in a
hand-written `config.h` (autotools replacement). To upgrade: re-copy `lib/` from
a newer tag, regenerate `nghttp2ver.h`, rebuild. The desktop host twin compiles
this same vendored copy (not brew's), so host runs cover the exact sources the
device links.

## Example binaries

Built by [`CMakeLists.txt`](CMakeLists.txt). `cookbook_worker` is the one you run
for real work; the rest are probes and tests.

| target | what it does |
|---|---|
| `cookbook_worker` | the desktop worker — register / trigger / signal / cleanup (above) |
| `whoami` | one live `whoami()` call; prints scheduler + namespace. Needs `MISTRAL_API_KEY`. |
| `transport_probe` | `DesktopTransport` against a local `temporal server start-dev` |
| `h2_probe` | the `esp/grpc_h2` core over the host stream — local, or `--live` against Mistral |
| `mwf_sole_worker` | both loops with a hard-coded spec (the pre-cookbook worker) |
| `mwf_worker` / `mwf_worker_h2` | activity-only worker over the desktop / h2 core |
| `h2_security_test` | ctest: gRPC frame-length overflow + JSON deep-nesting guards |

The `mwf_worker*` / `mwf_sole_worker` / `cookbook_worker` targets need the
generated proto host code — run [`../proto/gen_host.sh`](../proto/) first, or CMake
skips them with a message.

## When you'd touch this code

- **Add a Temporal RPC the worker calls** — nothing here changes; pass the new
  method path to `call()`. Encoding lives in [`proto/`](../proto/).
- **Add a control-plane endpoint** — extend `RestClient`
  ([`src/rest_client.h`](src/rest_client.h)); it's shared source, so the method
  exists on both desktop and device.
- **Change TLS posture, connection reuse, or watchdog behavior on the device** —
  [`esp/esp_transport.cpp`](esp/esp_transport.cpp).
- **Change gRPC framing or the reconnect policy** — [`esp/grpc_h2.cpp`](esp/grpc_h2.cpp)
  (`H2Transport` owns reconnect-and-retry). It's the shared core, so verify with
  `h2_probe` on the host before flashing.

Depends on [`contracts/`](../contracts/) for the interface and types.

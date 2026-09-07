# esp/ — ESP32-S3 transport

The device side of `mwf::ITransport` (CONTRACTS.md §1): **unary gRPC over
HTTP/2** — nghttp2 framing on a WiFiClientSecure (mbedTLS) stream, TLS routed
to PSRAM by firmware's allocator install. Consumed by `firmware` via
`symlink://../transport` (see `../library.json` for the PlatformIO view);
there is no standalone CMake build of the *device* transport here, but the
**core is host-built and host-proven** (below).

## Layout — one framing core, two streams

| file | role |
|---|---|
| `grpc_h2.{h,cpp}` | **Portable core**: nghttp2 session driver + gRPC framing (5-byte length-prefix, grpc-timeout, trailers → `grpc_status`, percent-decoded `grpc-message`, GOAWAY handling) against abstract `H2IO` send/recv/now-ms callbacks. One in-flight unary call (the worker's single-poll pattern); h2 connection reuse across calls; `H2Transport` base class owns the reconnect-and-retry-once policy. |
| `esp_transport.{h,cpp}` | Device stream: `EspTransport` over `WiFiClientSecure` (ALPN **h2**, `setInsecure()` fleet posture), lwIP RST-close, optional TLS-arbiter hooks, task-watchdog feed inside the long-poll recv window. |
| `host/h2_host_stream.{h,cpp}` | **Host twin** stream: POSIX TCP (+ OpenSSL TLS, ALPN h2) feeding the *same* `grpc_h2` core — `H2HostTransport`. Built by the CMake project (`mwf_transport_h2_host`, `h2_probe`, `mwf_worker_h2`). |
| `third_party/nghttp2/` | **Vendored nghttp2 1.68.0** `lib/` sources (see below). |

## Why vendored nghttp2

Neither the Arduino framework package nor pioarduino 55.03.39 ships an nghttp2
component reachable from an Arduino-framework PlatformIO build (ESP-IDF's
`sh2lib` lives in idf-extra-components and needs an IDF component build).
nghttp2's `lib/` is pure C with no OS deps, so it vendors cleanly:

- Source: `github.com/nghttp2/nghttp2` tag **v1.68.0** (same release brew
  ships), `lib/*.{c,h}` + `lib/includes/nghttp2/nghttp2.h` + `COPYING` (MIT).
- `nghttp2ver.h` generated from the `.in` (version substituted).
- `config.h` hand-written (autotools replacement): the only macros `lib/`
  consults are `HAVE_ARPA_INET_H` / `HAVE_NETINET_IN_H` / `HAVE_CLOCK_GETTIME`
  — all true on both macOS and ESP-IDF (lwIP + newlib).
- The **host build compiles the vendored copy too** (not brew's), so the host
  proof covers the exact sources the device links.
- Upgrading: re-copy `lib/` from a newer tag, regenerate `nghttp2ver.h`,
  re-run `./build/h2_probe`.

## Verification (host-twin gates — all green 2026-07-18)

- `h2_probe` vs local `temporal server start-dev`: GetSystemInfo +
  DescribeNamespace **byte-wise identical** to DesktopTransport (grpc++) on the
  same server; 12 s PollActivityTaskQueue → taskless-OK Idle path; three calls
  reused one h2 connection.
- `h2_probe --live-poll` vs **wf-scheduler.mistral.ai:443** (TLS + ALPN h2,
  Bearer + `temporal-namespace`): GetSystemInfo → `grpc_status 0`; a 50 s
  PollActivityTaskQueue held ~45 s by the frontend and released **OK-taskless**
  (empty task_token) — the documented idle path, NOT a client deadline.
- `mwf_worker_h2` (the desktop activity worker over THIS core) passed
  `conformance/e2e/run_desktop_worker_e2e.sh`: two real activity tasks
  polled/dispatched/responded cross-language. Observed `task_token` = **121 B**
  (dev server) → the nanopb 256 B cap has 2× headroom.
- h2 session allocator footprint (counting `nghttp2_mem`): peak **~24 KB**
  local / **~28 KB** live TLS — PSRAM-routed on device via
  `heap_caps_malloc_extmem_enable`.

Long-poll timing contract: Mistral's frontend holds `PollActivityTaskQueue`
~45 s then returns OK-taskless — treat as Idle and re-poll; client deadline
stays ~70 s (`grpc-timeout: 70000m`). The local dev frontend additionally
rejects a poll context under its 2 s minimum ("Context timeout is too short"),
so never send sub-2 s poll deadlines.

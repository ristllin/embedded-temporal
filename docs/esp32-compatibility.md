# ESP32 compatibility & performance

Reference for fitting a workflow onto the device: the hardware you need, the RAM
and flash it costs, and the latency to expect. If you're new here, start with
[the README](../README.md) and [Writing a workflow](writing-a-workflow.md). The
figures below come from the `firmware/` worker running against Mistral cloud on
an ESP32-S3 (8 MB PSRAM / 16 MB flash).

## Minimum hardware

| Requirement | Why |
|---|---|
| **ESP32-S3 with PSRAM** (N8R8 / N16R8 class) | The workflow history decodes into a multi-MB buffer that must live in PSRAM; TLS + HTTP/2 also lean on PSRAM-routed mbedTLS. |
| ≥ 8 MB flash | Firmware ~1.3 MB; the default 16 MB partition table is used. |
| 2.4 GHz WiFi | The worker holds a long-poll gRPC/TLS connection to `wf-scheduler.mistral.ai:443`. |

**Classic ESP32 (no PSRAM) is out of scope.** A single workflow history can
decode to ~2.5 MB — impossible in the ~320 KB internal SRAM alongside TLS +
HTTP/2 session state. The S3's 8 MB PSRAM (with a PSRAM-routed mbedTLS allocator
and per-poll PSRAM buffers) is what makes this fit.

## Compile-time footprint

Reported by PlatformIO for `env:esp32s3` (the full sole-worker: both loops,
nanopb protos, mbedTLS, vendored nghttp2, nlohmann/json):

| Segment | Used | Of | % |
|---|---|---|---|
| Static RAM (`.bss`+`.data`) | **47,048 B** | 320 KB internal SRAM | **14.4 %** |
| Flash (application) | **1,316,175 B** | 6.25 MB app partition | **20.1 %** |

Nothing large lands in static RAM — the big buffers are heap/PSRAM allocated
per-use and freed, never permanent statics.

## Runtime memory

| Metric | Value |
|---|---|
| Free internal heap at boot | ~248 KB |
| Free internal heap resting (worker up) | **~255 KB**, stable across cycles |
| Free PSRAM resting | **~8,108 KB** |
| HTTP/2 + TLS session (internal heap) | ~24–28 KB |
| **Per-poll workflow-history buffer (PSRAM)** | up to ~2.56 MB, **allocated then freed each poll** |

The workflow-task decode buffer is the one large allocation. It is sized by the
nanopb caps (`History.events` × `Payload.data`) and lives entirely in PSRAM
(`heap_caps_malloc(MALLOC_CAP_SPIRAM)`), so internal SRAM is untouched by it —
which is why resting free heap stays flat regardless of history size.

## Performance

Device-side timings, per workflow the device both orchestrated and executed end
to end:

| Phase | Time |
|---|---|
| Workflow task → **schedule activity** (replay decision) | few ms |
| Schedule → **activity executed + responded** (device.echo, incl. gRPC respond) | ~240–340 ms |
| Activity done → **CompleteWorkflowExecution** (next replay decision) | ~350 ms |
| **Total on-device active work per workflow** | **~570–690 ms** |
| Boot → worker up (WiFi + whoami + assemble) | ~2.5 s |
| Boot → self-registered with Mistral | ~3.2 s |

End-to-end wall-clock (trigger → COMPLETED) was **0.9 s – 11 s** server-side,
dominated by the poll cadence, not compute (see below). The compute is
sub-second; the spread is polling.

## The latency model: round-robin polling

The device runs **two** poll loops on **one** task queue on a single thread —
`WorkflowLoop` (workflow tasks) and `WorkerLoop` (activity tasks) — alternating
with a ~10 s poll deadline each. Mistral's frontend holds an empty long-poll
~45 s but honors a shorter client deadline, so a full workflow
(WFT1 → schedule activity → activity task → WFT2 → complete) turns over in
roughly **one to a few 10 s poll cycles** — hence the ~0.9–11 s spread depending
on where in the round-robin the task lands.

**Tuning knobs / roadmap:** shorten the poll deadline for lower latency (bounded
below by Mistral's ~2 s minimum); or run the two loops as separate FreeRTOS
tasks with two TLS slots for concurrency (see [ROADMAP.md](../ROADMAP.md) →
"Two-task concurrency"). Long-lived workflows should adopt continue-as-new and
incremental replay to bound the history buffer (also on the roadmap).

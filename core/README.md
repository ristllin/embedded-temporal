# core

The portable engine that both workers are built from — the desktop
`cookbook_worker` binary and the ESP32 firmware. Pure C++17, no Arduino. It
parses a workflow spec, replays it against the execution history to decide the
next step, runs your activities, and drives the two poll loops that talk to
Mistral. The exact same `.cpp` runs on your laptop (backed by nlohmann/json) and
on the device (backed by ArduinoJson).

You usually don't call into `core` yourself — the desktop worker
([`transport/example/cookbook_worker_main.cpp`](../transport/example/cookbook_worker_main.cpp))
and the firmware ([`firmware/src/main.cpp`](../firmware/src/main.cpp)) wire it
up. You reach for these headers in two cases:

- **Writing an activity** — [`include/mwf_core/activity_registry.h`](include/mwf_core/activity_registry.h).
- **Understanding the spec grammar** — the grammar is documented at the top of
  [`include/mwf_core/workflow_spec.h`](include/mwf_core/workflow_spec.h).

If you haven't yet, read [docs/writing-a-workflow.md](../docs/writing-a-workflow.md)
first — it covers the whole model (spec + activities). This file is the map of
what lives under `include/mwf_core/`.

## The two you'll actually read

### Registering activities

An activity is a function `mwf::Result<mwf::Bytes>(const mwf::Bytes&)` — JSON
bytes in, a `Result` of JSON bytes out — registered under a name. The registry
is a name → function table the worker loop dispatches through.

```cpp
#include "mwf_core/activity_registry.h"

mwf_core::ActivityRegistry registry;

registry.registerActivity("say_hello", [](const mwf::Bytes& arg) {
  mwf::json in  = mwf::json::parse(arg.begin(), arg.end());   // {"name":"Ada"}
  mwf::json out = {{"greeting", "Hello " + in.value("name", "world")}};
  std::string s = out.dump();
  return mwf::Result<mwf::Bytes>::success(mwf::Bytes(s.begin(), s.end()));
});
```

Return `mwf::Result<mwf::Bytes>::failure("reason")` to fail the activity;
Mistral applies the retry policy. Activities are compiled in — to add one you
edit the worker's C++ and rebuild (desktop) or reflash (device).

### The spec grammar

A workflow is JSON, not code — a list of steps (`activity`, `sequence`,
`conditional`, `wait_signal`, `complete`) that read and write one bindings
document via [JSON Pointers](https://datatracker.ietf.org/doc/html/rfc6901):

```json
{ "input":   <the trigger's input>,
  "results": { "<step id>": <that step's output>, ... } }
```

The full grammar — every step type, the predicate operators, the source-ref
rules — is the header comment in
[`include/mwf_core/workflow_spec.h`](include/mwf_core/workflow_spec.h).
`parseWorkflowSpec(text)` turns spec JSON into a `WorkflowSpec` and never throws
(bad JSON comes back as a `Result` failure). Reserved but not yet accepted:
`parallel`, `timer`.

## The headers

Everything under [`include/mwf_core/`](include/mwf_core/):

| header | what it is |
|---|---|
| [`activity_registry.h`](include/mwf_core/activity_registry.h) | name → activity-function table; the thing you register your code on |
| [`workflow_spec.h`](include/mwf_core/workflow_spec.h) | the spec grammar (documented in-header) + `parseWorkflowSpec` |
| [`replay_engine.h`](include/mwf_core/replay_engine.h) | `decide(spec, history) -> commands`; the deterministic interpreter |
| [`workflow_loop.h`](include/mwf_core/workflow_loop.h) | polls the workflow queue, runs `decide`, responds with commands |
| [`worker_loop.h`](include/mwf_core/worker_loop.h) | polls the activity queue, dispatches to the registry, responds |
| [`client_ops.h`](include/mwf_core/client_ops.h) | start / signal / query a workflow (see note below) |
| [`history.h`](include/mwf_core/history.h) | the Temporal event history model + JSON loader |
| [`retry.h`](include/mwf_core/retry.h) | Temporal-shaped backoff (`nextBackoffMs`, `shouldRetry`) |
| [`detail/json_util.h`](include/mwf_core/detail/json_util.h) | the single JSON-library seam (nlohmann on host, ArduinoJson on device) |

## How the pieces fit

The engine and loops are the machinery `main.cpp` runs; you rarely touch them
directly, but here's the shape.

`ReplayEngine::decide` is a pure function of `(spec, history)` — no clock, no
random, no I/O. It walks the spec against the recorded history and returns the
next `Command`s (schedule an activity, complete, or fail). Same inputs always
produce the same commands, which is what makes a run survive a reboot: it also
returns an `EngineState` snapshot you persist and restore to resume mid-run.

Two loops turn `decide` into a live worker. Both are one `runOnce()` cycle you
call in a loop:

- **`WorkflowLoop`** polls the workflow task queue, runs `decide` over the
  delivered history, and responds with its commands.
- **`WorkerLoop`** polls the activity task queue, dispatches the task to your
  `ActivityRegistry`, and responds with the result.

On the device the same board runs both loops, so it is the whole worker. The
step-by-step version — a workflow tick end to end, the transport seam, durable
resume — is in [docs/architecture.md](../docs/architecture.md).

> **`ClientOps` note:** `signalWorkflow` is implemented — it's what delivers a
> human-in-the-loop approval to a paused `wait_signal` step. `startWorkflow` and
> `queryWorkflow` are declared but not wired yet; today you trigger a run over
> REST or with `cookbook_worker --mode trigger` (see
> [docs/writing-a-workflow.md](../docs/writing-a-workflow.md)). Tracking in
> [ROADMAP.md](../ROADMAP.md).

## Build and test (host)

`core` builds and its tests run on your desktop with CMake — no hardware.

```bash
cmake -B build && cmake --build build && ctest --test-dir build
```

Needs `nlohmann-json` (the host JSON backing) and the sibling `contracts/`
headers. The engine is written portable-first so the same sources build for the
device through PlatformIO with ArduinoJson swapped in behind the `json_util.h`
seam — see [`library.json`](library.json) for which translation units compile on
device today.

## The JSON seam

`mwf::json` / `mwf::Bytes` / `mwf::Result` come from
[`contracts/`](../contracts/include/mwf/). Every parse, serialize, and
pointer-walk in `core` goes through
[`detail/json_util.h`](include/mwf_core/detail/json_util.h) and nowhere else, so
porting the JSON library is a one-file change. No exceptions escape `core` —
parse errors come back as `Result` failures.

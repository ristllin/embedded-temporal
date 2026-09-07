# Writing a workflow

This guide takes you from nothing to a workflow running on real Mistral cloud —
first on your desktop, then on an ESP32. By the end you'll know the two things
that make up every workflow here: **the spec** (what happens) and **the
activities** (the code that does it).

If you just want to run an example first, see [cookbooks/](../cookbooks/). This
guide explains how to build your own.

---

## The mental model

A **workflow** is a JSON document describing a sequence of steps. It contains no
logic of its own — it *orchestrates*. Steps call **activities** (your code),
branch on their results, wait for signals, and finish with a result.

An **activity** is a plain function you register under a name. It takes a JSON
argument and returns a JSON result. Activities are where the real work happens: a
computation, an HTTP call, reading a sensor, blinking an LED.

A **worker** is the program that runs both: it polls Mistral for work, replays
the spec to decide what to do next, and executes the activities. In this project
the worker is a small C++ program — the desktop `cookbook_worker` binary, or the
ESP32 firmware. Both use the identical engine in [`core/`](../core/).

```
   Mistral cloud                     your worker (desktop or ESP32)
  ┌──────────────┐   poll for work   ┌───────────────────────────────┐
  │  durable      │ ◄──────────────── │  replay the spec → next step  │
  │  history,     │                   │  run the named activity       │
  │  retries,     │ ──────────────►   │  report result                │
  │  dashboard    │   here's a task   └───────────────────────────────┘
  └──────────────┘
```

You never write orchestration in C++. You write a JSON spec and some activities.

---

## Step 1 — Write the spec

Create `greet.json`:

```json
{
  "name": "greet",
  "steps": [
    { "type": "activity", "name": "say_hello", "id": "greeting", "args_from": "/input" },
    { "type": "complete", "result_from": "/results/greeting" }
  ]
}
```

- `name` is how you'll register and trigger it.
- `steps` run top to bottom.
- The `activity` step calls the activity named `say_hello`, passing it the
  workflow's input (`args_from: "/input"`), and stores its result under the id
  `greeting`.
- The `complete` step finishes the workflow, returning whatever is at
  `/results/greeting`.

### How data flows

The engine keeps one JSON document as it runs:

```json
{ "input":   <the trigger's input>,
  "results": { "greeting": <say_hello's output>, ... } }
```

Every `args_from`, `result_from`, and predicate `path` is a
[JSON Pointer](https://datatracker.ietf.org/doc/html/rfc6901) into that document —
`/input`, `/results/greeting`, `/results/parity/kind`. That's how a step's output
becomes the next step's input.

### Step types

| Step | What it does |
|---|---|
| `activity` | Call an activity by name; bind its result under `id`. Argument comes from `args_from` (a pointer) or `args` (a literal). |
| `conditional` | Evaluate a `predicate`; walk `true_steps` or `false_steps`. |
| `wait_signal` | **Pause** until a named signal arrives (e.g. a human approval), then bind its payload and continue. |
| `sequence` | Group steps (useful inside a branch). |
| `complete` | Finish, returning `result_from` (a pointer) or `result` (a literal). |

A **conditional** with a predicate:

```json
{ "type": "conditional",
  "predicate": { "path": "/results/classify/label", "op": "eq", "value": "positive" },
  "true_steps":  [ { "type": "activity", "name": "thank_them", "id": "reply", "args_from": "/input" } ],
  "false_steps": [ { "type": "activity", "name": "apologize",  "id": "reply", "args_from": "/input" } ] }
```

Predicate `op` is one of `eq`, `ne`, `lt`, `le`, `gt`, `ge`, `exists`. Predicates
are pure — they only read bound results, so the same history always decides the
same way (this is what makes replay deterministic).

A **wait_signal** step (human-in-the-loop):

```json
{ "type": "wait_signal", "signal": "approve", "id": "gate" }
```

The workflow blocks here until something sends the `approve` signal (a REST call,
or a button press on the device — see [cookbook 04](../cookbooks/04-human-in-the-loop/)).
The signal's payload binds under `/results/gate`.

> The full grammar, including edge cases, is documented at the top of
> [`core/include/mwf_core/workflow_spec.h`](../core/include/mwf_core/workflow_spec.h).
> Reserved but not yet implemented: `parallel`, `timer` (see [ROADMAP.md](../ROADMAP.md)).

---

## Step 2 — Write the activities

An activity is a function registered by name. Its signature is always the same:
JSON bytes in, a `Result` of JSON bytes out.

```cpp
#include "mwf_core/activity_registry.h"
#include "mwf/types.h"           // mwf::json, mwf::Bytes, mwf::Result

mwf_core::ActivityRegistry registry;

registry.registerActivity("say_hello", [](const mwf::Bytes& arg) {
  mwf::json in  = mwf::json::parse(arg.begin(), arg.end());  // e.g. {"name":"Ada"}
  mwf::json out = { {"greeting", "Hello " + in.value("name", "world") + "!"} };
  std::string s = out.dump();
  return mwf::Result<mwf::Bytes>::success(mwf::Bytes(s.begin(), s.end()));
});
```

Notes:

- The argument bytes are whatever the spec pointed `args_from` at, serialized as
  JSON. The return bytes become the step's result.
- Return `Result::failure("reason")` to fail the activity — Mistral applies the
  retry policy.
- Activities are **compiled in**. To add or change one you edit the worker's C++
  and rebuild (desktop) or reflash (device). There is no dynamic loading.

The desktop `cookbook_worker` ships a small fixed set of activities you can use
without writing any C++ — `echo`, `ai.chat`, `ai.classify` — see
[`transport/example/cookbook_worker_main.cpp`](../transport/example/cookbook_worker_main.cpp).
For your own activities, add them there (or copy that file into your own worker
binary).

---

## Step 3 — Run it on your desktop

The desktop worker registers the workflow, then polls and runs it. Point it at
your Mistral account (`MISTRAL_API_KEY` in your env or a gitignored `.env`).

```bash
# build once (gen_host.sh generates the git-ignored protobuf C++ first)
./proto/gen_host.sh
cd transport && cmake -B build && cmake --build build --target cookbook_worker

# run the worker: it registers `greet` and starts polling
build/cookbook_worker --mode worker --spec ../greet.json \
  --workflow greet --queue greet-q &

# trigger a run (in another shell, or via REST)
build/cookbook_worker --mode trigger --workflow greet --queue greet-q \
  --input-json '{"name":"Ada"}'
```

You can also trigger over plain REST — the worker doesn't have to be the trigger:

```bash
curl -H "Authorization: Bearer $MISTRAL_API_KEY" \
     -H 'Content-Type: application/json' \
     -X POST https://api.mistral.ai/v1/workflows/greet/execute \
     -d '{"input":{"name":"Ada"},"task_queue":"greet-q"}'
# -> {"execution_id":"...","status":"RUNNING",...}
```

The run appears in [Mistral Studio](https://console.mistral.ai) → Workflows,
under the same workspace your API key belongs to. Poll it with
`GET /v1/workflows/executions/{id}` until `COMPLETED`.

> The [cookbooks](../cookbooks/) wrap exactly this (build → register → trigger →
> verify → clean up) in a `run_local.sh`. Copying a cookbook is the fastest way
> to start.

---

## Step 4 — Put it on an ESP32

The device runs the *same* spec and *same* engine. The differences are:

1. The spec is an embedded C++ string instead of a file.
2. Your activities are registered in the firmware.
3. The firmware self-registers the workflow with Mistral on boot, then runs the
   workflow loop **and** the activity loop, so the board is the whole worker.

The wiring lives in
[`firmware/src/main.cpp`](../firmware/src/main.cpp). The shape is:

```cpp
// 1) your workflow, as a string
static const char kSpec[] = R"JSON({
  "name": "greet",
  "steps": [
    {"type":"activity","name":"say_hello","id":"greeting","args_from":"/input"},
    {"type":"complete","result_from":"/results/greeting"}
  ]
})JSON";

// 2) your activities
registry.registerActivity("say_hello", [](const mwf::Bytes& arg){ /* ... */ });

// 3) main.cpp already: connects Wi-Fi, discovers the Mistral endpoint (whoami),
//    registers the workflow, and drives WorkflowLoop + WorkerLoop each loop().
```

Build, flash, and provision (Wi-Fi + key go into NVS, never into git):

```bash
cd firmware
pio run -e esp32s3 -t upload
# provisioning + full flashing walkthrough:  firmware/README.md
```

Then trigger it exactly as in Step 3 (the device polls the queue it registered).
The result comes back from the board. See
[cookbook 04](../cookbooks/04-human-in-the-loop/) for a device workflow that also
sends a signal from a physical button.

Requirements and on-device footprint/latency:
[docs/esp32-compatibility.md](esp32-compatibility.md). (You need an ESP32-**S3
with PSRAM**.)

---

## Where to go next

- **[cookbooks/](../cookbooks/)** — copy a working workflow and adapt it.
- **[docs/architecture.md](architecture.md)** — how the engine, loops, and
  transport fit together.
- **[docs/limitations.md](limitations.md)** — what the grammar and engine do and
  don't support yet.
- **[docs/mistral-compatibility.md](mistral-compatibility.md)** — exactly which
  Mistral/Temporal surface this speaks.

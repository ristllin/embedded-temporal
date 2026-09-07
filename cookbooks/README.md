# Cookbooks — a guided path

Four runnable workflows, in order. Each one adds exactly one idea to the last, so
by the end you can write your own workflow and run it on an ESP32. Start at 01
even if you've read the [main guide](../docs/writing-a-workflow.md) — these are
the hands-on version of it.

Every cookbook is the same shape: a `spec.json` (the workflow), a `run_local.sh`
(build → register → trigger → check → clean up), and a README that walks through
both. They all run on the desktop `cookbook_worker` — no hardware needed until
you want it.

## Before you start

You need a Mistral API key and a C++ toolchain (see the
[root README quickstart](../README.md#quickstart-desktop) for the dependency
list). Put your key in a gitignored `.env` at the repo root:

```bash
echo "MISTRAL_API_KEY=sk-..." > .env        # already in .gitignore
```

Each `run_local.sh` builds `cookbook_worker` on first run, so you can go straight
to a cookbook. To build it once up front instead:

```bash
./proto/gen_host.sh        # once: protoc → proto/gen/host (git-ignored)
cd transport && cmake -B build && cmake --build build --target cookbook_worker
```

Then run any chapter:

```bash
cd cookbooks/01-hello-world && ./run_local.sh
```

## The path

### [01 — Hello World](01-hello-world/)

**Your first workflow, end to end.** A two-step [`spec.json`](01-hello-world/spec.json):
one `activity` step calls `echo`, one `complete` step returns its result. You'll
see the three things every workflow has — the JSON spec, a named activity, and the
worker that registers it, triggers it, and reads back the result. Nothing else
moving yet, on purpose.

### [02 — AI Workflow](02-ai-workflow/)

**Chaining and branching.** Builds on 01 by adding steps that feed each other:
`ai.classify` labels an input, a `conditional` branches on that label, and
`ai.chat` writes a reply — a real call out to a Mistral model from inside an
activity. This is where you see data flow step→step: each result binds under
`/results/<id>`, and the next step reads it back with a JSON pointer.

### [03 — Reproducibility](03-reproducibility/)

**Why it's durable and deterministic.** Builds on 02, but drops the AI so "the
same" can mean *byte-for-byte*. The spec is data, not code, so the worker replays
it the same way every time: the same input produces the same command stream and
the same result. That property — a workflow is a function of its history — is what
lets Temporal recover a run by replaying it, and what lets a tiny board run the
orchestration with no sandbox.

### [04 — Human-in-the-Loop](04-human-in-the-loop/)

**Pausing, signals, and the device.** Builds on 03 by adding a `wait_signal` step:
the workflow blocks until an `approve` signal arrives, then resumes with the
signal's payload. The pause lives entirely in the recorded history, so a worker
can reboot while paused and pick up where it left off. This is the first chapter
you run on real hardware — the approval can be a REST call *or* a BOOT-button
press on the ESP32.

## Core concepts recap

The five pieces you meet along the way. Full grammar and details:
[docs/writing-a-workflow.md](../docs/writing-a-workflow.md).

| Concept | What it is |
|---|---|
| **Spec** | A JSON document (`spec.json`) listing the steps. It orchestrates; it holds no logic of its own. |
| **Activity** | A C++ function `mwf::Result<mwf::Bytes>(const mwf::Bytes&)` registered by name. Where the real work happens. |
| **Worker** | The program that polls Mistral, replays the spec to pick the next step, and runs the activities — desktop `cookbook_worker` or the ESP32 firmware, same engine. |
| **Step types** | `activity`, `conditional`, `wait_signal`, `sequence`, `complete`. Grammar reference: [`core/include/mwf_core/workflow_spec.h`](../core/include/mwf_core/workflow_spec.h). |
| **Data flow** | One JSON document `{ "input": …, "results": { … } }`. Every `args_from` / `result_from` / predicate `path` is a JSON pointer into it, e.g. `/results/classify/label`. |

The desktop worker ships three activities you can use without writing any C++ —
`echo`, `ai.chat`, `ai.classify` (see
[`transport/example/cookbook_worker_main.cpp`](../transport/example/cookbook_worker_main.cpp)).
To add your own, edit that file and rebuild, or flash the firmware — see
[Step 4 of the guide](../docs/writing-a-workflow.md#step-4--put-it-on-an-esp32).

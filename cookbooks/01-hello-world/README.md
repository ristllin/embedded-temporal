# Cookbook 01 — Hello World

**What you'll learn:** the two pieces every workflow is made of — a JSON *spec*
of steps and a named C++ *activity* — by running the smallest one that exists:
call one activity, return its result. Start here; the next three cookbooks build
on this.

## What this workflow does

It takes a JSON input, hands it to a single activity called `echo`, and finishes
by returning whatever `echo` produced. Two steps, one activity, no branching.

The worker (here the desktop `cookbook_worker` binary) registers the spec with
Mistral, polls for work, replays the spec to decide the next step, runs the
`echo` activity, and reports the result back. Nothing runs "in" the JSON — the
spec only says *what* to do; the activity is the code that does it.

## The spec

[`spec.json`](spec.json):

```json
{
  "name": "cookbook-01-hello-world",
  "input_schema":  { "type": "object", "additionalProperties": true, "title": "hello_Input" },
  "output_schema": { "type": "object", "additionalProperties": true, "title": "hello_Output" },
  "steps": [
    { "type": "activity", "name": "echo", "id": "greeting", "args_from": "/input" },
    { "type": "complete", "result_from": "/results/greeting" }
  ]
}
```

- `steps` run top to bottom.
- The `activity` step calls the activity named `echo`, passes it the workflow's
  input (`args_from: "/input"`), and binds its result under the id `greeting`.
- The `complete` step finishes the run, returning whatever is at
  `/results/greeting`.
- `input_schema` / `output_schema` are optional structural hints for Mistral's
  frontend; the replay engine ignores them. The cookbooks keep them permissive.

Every `args_from` / `result_from` is a [JSON Pointer](https://datatracker.ietf.org/doc/html/rfc6901)
into the one document the engine accumulates as it runs:

```json
{ "input":   <the trigger input>,
  "results": { "greeting": <echo's output> } }
```

That document is how one step's output becomes another step's input. Full
grammar: [`core/include/mwf_core/workflow_spec.h`](../../core/include/mwf_core/workflow_spec.h).

## The activity

`echo` is a compiled-in C++ function registered by name in the desktop worker,
in [`transport/example/cookbook_worker_main.cpp`](../../transport/example/cookbook_worker_main.cpp):

```cpp
// echo — deterministic, no network. Wrap the arg with the engine/host brand.
registry.registerActivity("echo", [](const Bytes& arg) {
  json out;
  out["echo"] = parseArg(arg);   // the JSON that /input pointed at
  out["engine"] = "mwf-cpp";
  out["host"] = "desktop";
  return mwf::Result<Bytes>::success(toBytes(out.dump()));
});
```

Every activity has the same shape: JSON bytes in, a `mwf::Result<mwf::Bytes>`
out. `echo` returns its argument wrapped in an object so you can see the input
made the full round trip. Activities are compiled into the worker — to add or
change one you edit this file and rebuild (desktop) or reflash (device); there is
no dynamic loading.

## Run it

You need a `MISTRAL_API_KEY` (in your environment, or a gitignored `.env` at the
repo root) and a C++ toolchain — see the [Quickstart](../../README.md#quickstart-desktop).

```bash
./run_local.sh
```

The script builds `cookbook_worker` if needed, registers a uniquely-named copy of
this workflow, triggers it with a sample input, waits for it to reach
`COMPLETED`, prints the result, and cleans up. It exits non-zero if the run
doesn't complete.

The input it triggers with (edit this line in [`run_local.sh`](run_local.sh) to
change it):

```bash
INPUT='{"greeting":"hello-from-cookbook-01","name":"world"}'
```

### Reading the result

`echo` wraps the input, and `complete` returns it, so the run comes back as:

```json
{
  "echo": { "greeting": "hello-from-cookbook-01", "name": "world" },
  "engine": "mwf-cpp",
  "host": "desktop"
}
```

`run_local.sh` prints this and also writes it to `evidence/trigger.json`
(git-ignored). The same execution shows up in
[Mistral Studio](https://console.mistral.ai) → Workflows.

## Make it your own

- **Change the input.** Edit the `INPUT='...'` line in `run_local.sh` and rerun.
  Because `args_from` is `/input`, whatever object you pass shows up unchanged
  under `echo` in the result.
- **Change the activity.** Edit the `echo` lambda in
  [`cookbook_worker_main.cpp`](../../transport/example/cookbook_worker_main.cpp)
  — e.g. uppercase a field, add a timestamp, or copy it under a new name and
  point the spec's `"name": "echo"` at it — then rebuild
  (`cmake --build transport/build --target cookbook_worker`, or just rerun
  `./run_local.sh`, which rebuilds).

## Next

- **[02-ai-workflow](../02-ai-workflow/)** — chain multiple steps, flow data
  step→step through `/results`, add a conditional, and call the Mistral chat API
  from an activity.
- For the concepts in depth — step types, data flow, and putting a workflow on an
  ESP32 — read [docs/writing-a-workflow.md](../../docs/writing-a-workflow.md).

# Cookbook 03 — Reproducibility

**What you'll learn:** why running a workflow is repeatable. Chapter
[02](../02-ai-workflow/) chained activities and branched on their results; here
the branch is the same, but the point is different — *the same input always
replays to the same run.* That is Temporal's determinism guarantee, and it's why
a workflow can survive a crash.

## The idea

A workflow here is **data**, not code: a JSON spec of steps. The engine in
[`core/`](../../core/) doesn't *execute* your spec once and forget it — it
*replays* it against the recorded history of what has happened so far (the
input, and each activity's result) to decide what to do next. Two facts make
that replay deterministic:

- Every decision reads only from the bindings document — `/input` and
  `/results/*` — which comes only from recorded history.
- Predicates are pure: no clock, no randomness, no I/O (see the grammar in
  [`core/include/mwf_core/workflow_spec.h`](../../core/include/mwf_core/workflow_spec.h)).

So the same input can only produce the same decisions: the same command stream
(schedule activity, schedule activity, complete) and the same result.

**Why it matters:** if a worker dies mid-run, nothing is lost — a fresh worker
replays the history and arrives at the exact same place. That's what makes a
run *durable*, and it's what lets a board with ~50 KB of RAM be a real worker
instead of holding the whole run in memory. Chapter [04](../04-human-in-the-loop/)
makes that physical by rebooting a device mid-workflow.

## The spec

[`spec.json`](spec.json) — two `echo` activities with a conditional between
them. Nothing here calls the network, so "same result" is exact:

```json
{
  "name": "cookbook-03-reproducibility",
  "steps": [
    { "type": "activity", "name": "echo", "id": "capture", "args_from": "/input" },
    {
      "type": "conditional",
      "predicate": { "path": "/input/n", "op": "ge", "value": 3 },
      "true_steps": [
        { "type": "activity", "name": "echo", "id": "branch",
          "args": { "branch": "big", "reason": "n >= 3" } }
      ],
      "false_steps": [
        { "type": "activity", "name": "echo", "id": "branch",
          "args": { "branch": "small", "reason": "n < 3" } }
      ]
    },
    { "type": "complete", "result_from": "/results" }
  ]
}
```

- `echo` with `args_from: "/input"` copies the trigger input into
  `/results/capture`.
- The `conditional` reads `/input/n` and compares it to `3` with `ge`
  (`>=`). Both branches bind under the *same* id `branch`, so exactly one runs
  and `/results/branch` holds whichever did.
- `complete` returns the whole `/results` map.

(The spec also carries an `input_schema` requiring an integer `n`; the engine
treats schemas as opaque, but Mistral Studio uses them to render the trigger
form.)

## How the input decides the branch

As the run proceeds, the engine accumulates one document. With input
`{"n":4,"note":"determinism-demo"}` it looks like this by the time the
conditional evaluates:

```json
{ "input":   { "n": 4, "note": "determinism-demo" },
  "results": { "capture": { "echo": { "n": 4, "note": "determinism-demo" },
                            "engine": "mwf-cpp", "host": "desktop" } } }
```

The predicate `/input/n ge 3` reads `4`, so the `true_steps` branch runs.
Because that read touches only recorded data, re-running with `n=4` takes the
same branch every time. Flip the input to `n=1` and it deterministically takes
the other branch — different result, same rule.

## The activities

Just `echo`, the deterministic one — no AI, no network. It wraps its argument
and stamps the worker's identity, defined in
[`transport/example/cookbook_worker_main.cpp`](../../transport/example/cookbook_worker_main.cpp):

```cpp
registry.registerActivity("echo", [](const Bytes& arg) {
  json out;
  out["echo"]   = parseArg(arg);
  out["engine"] = "mwf-cpp";
  out["host"]   = "desktop";   // "esp32" when the same activity runs on the board
  return mwf::Result<Bytes>::success(toBytes(out.dump()));
});
```

Both steps call it — the first on `/input`, the second on a literal `args`
object — so the final result for `n=4` is:

```json
{ "capture": { "echo": { "n": 4, "note": "determinism-demo" }, "engine": "mwf-cpp", "host": "desktop" },
  "branch":  { "echo": { "branch": "big", "reason": "n >= 3" }, "engine": "mwf-cpp", "host": "desktop" } }
```

## Run it

```bash
./run_local.sh
```

It builds `cookbook_worker`, registers a uniquely-named deployment, starts one
worker that services three completions, and triggers the same workflow three
times:

| run | input | branch | how it's checked |
|---|---|---|---|
| A | `{"n":4}` | `big` | baseline |
| B | `{"n":4}` (identical) | `big` | must equal A |
| C | `{"n":1}` | `small` | must differ from A |

The reproducibility check is two comparisons: the **command stream** each run
emitted (parsed from the worker log) and the **result** each run returned
(pulled from the trigger evidence, normalized with `sort_keys`). A and B must
match on both; C must differ on the result. The comparison lands in
`evidence/determinism.txt`:

```
run A (n=4): command_stream=[SCHEDULE_ACTIVITY_TASK,SCHEDULE_ACTIVITY_TASK,COMPLETE_WORKFLOW_EXECUTION]
run B (n=4): command_stream=[SCHEDULE_ACTIVITY_TASK,SCHEDULE_ACTIVITY_TASK,COMPLETE_WORKFLOW_EXECUTION]

OK: result A == result B (byte-identical, same input -> same result)
OK: result C differs (n=1 took the 'small' branch)
```

All three runs share the same command *shape* — two schedules and a complete —
because the spec's structure never changes. What A and B additionally share is a
byte-identical result; C, from a different input, reaches the other branch. Same
engine, same spec, decisions driven only by the input.

> How it proves it, briefly: because a completed run's history is fixed, replay
> is a pure function of `(spec, history)`. The script exercises that by replaying
> the identical input twice and diffing — no special "determinism mode," just
> the ordinary worker run twice.

## Make it your own

Change the branch predicate and watch the decisions follow. Edit the
`conditional` in [`spec.json`](spec.json):

```json
"predicate": { "path": "/input/n", "op": "lt", "value": 10 }
```

Now `n < 10` takes the `true` branch — so the sample inputs (`n=4`, `n=1`) both
land in `big`, and A/B/C all match. Other things to try:

- Branch on the free-text field instead: `{ "path": "/input/note", "op": "eq", "value": "determinism-demo" }` (string equality).
- Use `"op": "exists"` to branch on whether a field is present at all (`value`
  is ignored).

The full predicate grammar — `eq`, `ne`, `lt`, `le`, `gt`, `ge`, `exists`, and
what each compares — is documented in
[`workflow_spec.h`](../../core/include/mwf_core/workflow_spec.h). Whatever you
pick stays deterministic, because it still reads only from `/input` and
`/results/*`.

## Next

Chapter [04 — Human-in-the-Loop](../04-human-in-the-loop/): a `wait_signal` step
pauses the workflow durably until an approval arrives — a REST call, or a
BOOT-button press on the ESP32 — and the run survives a reboot while paused. The
durability this chapter argued for, on real hardware.

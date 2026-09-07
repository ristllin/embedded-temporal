# Cookbook 04 — Human-in-the-Loop (pause, signal, resume)

**What you'll learn:** how a workflow *pauses* until a person approves it, then
resumes. Chapter 03 showed the run is a pure function of its history; here that
same property makes the pause **durable** — the worker can reboot while paused
and still finish once the approval arrives. You'll approve two ways: a REST
signal from any client, and a physical **BOOT-button press on the ESP32**.

This is the capstone: spec + activities + data flow + determinism, plus the one
step that lets a workflow stop and wait for the outside world.

## The workflow

Four steps: echo the request, **wait** for the `approve` signal, echo whatever
the approver sent, complete. The middle step is the whole point.

```jsonc
// spec.json
{
  "steps": [
    // 1. run the "echo" activity on the trigger input -> /results/request
    { "type": "activity", "name": "echo", "id": "request", "args_from": "/input" },

    // 2. BLOCK until an "approve" signal arrives; its payload binds -> /results/gate
    { "type": "wait_signal", "signal": "approve", "id": "gate" },

    // 3. run "echo" again, this time on the approver's payload -> /results/after
    { "type": "activity", "name": "echo", "id": "after", "args_from": "/results/gate" },

    // 4. finish, returning /results/after
    { "type": "complete", "result_from": "/results/after" }
  ]
}
```

Data flows exactly as in the earlier chapters — through JSON pointers into
`{input, results}`. The new piece is that `wait_signal` binds the *signal's*
payload under `/results/gate`, so step 3 sees what the approver sent.

### Why the pause is durable

A `wait_signal` step emits **no command**. The engine replays the history, walks
to the `wait_signal`, finds no matching `WorkflowExecutionSignaled` event yet,
and stops — it does not schedule anything, does not complete. Waiting is the
*absence* of a completion command, not a timer or a held connection.

Because nothing but the recorded history decides where the walk stops, a worker
that crashes or reboots while paused replays to the **same** blocked step and
keeps waiting. When the `approve` event finally appears in history, the next
replay walks past it and continues. The full rule is at the top of
[`core/include/mwf_core/workflow_spec.h`](../../core/include/mwf_core/workflow_spec.h)
(the `wait_signal` grammar).

## The activities

Both `echo` steps use one activity. On the desktop worker it's `echo`, defined in
[`transport/example/cookbook_worker_main.cpp`](../../transport/example/cookbook_worker_main.cpp):

```cpp
registry.registerActivity("echo", [](const Bytes& arg) {
  json out;
  out["echo"]   = parseArg(arg);   // whatever the spec pointed at
  out["engine"] = "mwf-cpp";
  out["host"]   = "desktop";
  return mwf::Result<Bytes>::success(toBytes(out.dump()));
});
```

So the completed result is the approver's payload, wrapped:

```json
{ "echo": { "approved": true, "by": "desktop" }, "engine": "mwf-cpp", "host": "desktop" }
```

The device firmware registers an equivalent `device.echo` (in
[`firmware/src/main.cpp`](../../firmware/src/main.cpp)) that brands the result
with `"device":"mwf-esp32s3-sole"` instead — same shape, different worker.

## Sending the `approve` signal

Two ways to deliver the same signal. Both target the running execution by id and
carry a JSON payload that becomes `/results/gate`.

**1. REST — any client.** `POST` to the execution's `/signals` endpoint with
`{"name": <signal>, "input": <payload>}`:

```bash
curl -H "Authorization: Bearer $MISTRAL_API_KEY" -H 'Content-Type: application/json' \
  -X POST https://api.mistral.ai/v1/workflows/executions/$EXECUTION_ID/signals \
  -d '{"name":"approve","input":{"approved":true,"by":"desktop"}}'
```

**2. On the device — the BOOT button.** A GPIO0 press calls
[`ClientOps::signalWorkflow`](../../core/include/mwf_core/client_ops.h) over the
device's own transport:

```cpp
// firmware/src/main.cpp — on a debounced BOOT-button press
client->signalWorkflow(ns, waitingWorkflowId, "approve",
                       bytes(R"({"approved":true,"by":"button"})"));
```

> **Declare the signal, or Mistral rejects it.** A workflow's registration must
> list the signal names it accepts; signaling a name that wasn't declared returns
> `HTTP 422 "Signal handler '<name>' not found"`. The desktop worker derives the
> list from the spec's `wait_signal` steps automatically (`buildSignalsJson`); the
> device declares it in `kWorkflowSignals`.

## Run it on your desktop

```bash
./run_local.sh
```

It builds `cookbook_worker`, registers a uniquely-named workflow with its
`approve` signal declared, then in one client process:

1. triggers the workflow,
2. polls the execution and confirms it stays `RUNNING` with no result (the pause),
3. delivers the `approve` signal over REST,
4. polls again and confirms it reaches `COMPLETED`, with the approver's payload
   in the result.

It prints `PASS`/`FAIL` and archives the deployment on exit. `MISTRAL_API_KEY`
comes from the environment or a local `.env` file (never printed).

## Run it on the ESP32

The same spec and replay engine run on the board, and the board is *both* the
worker and the approver: it serves the workflow to the pause, and a BOOT-button
press delivers the signal.

Build and flash the human-in-the-loop firmware (`-DMWF_COOKBOOK=4`, wired as its
own PlatformIO env):

```bash
cd ../../firmware
pio run -e esp32s3_cb4 -t upload      # plain `pio run -e esp32s3` is cookbook 01
```

Provision Wi-Fi + the Mistral key into NVS (see [firmware/README.md](../../firmware/README.md)),
then:

1. The board boots, self-registers `mwf_device_hitl` (with the `approve` signal),
   and starts polling its task queue (default `mwf-esp`).
2. Trigger the workflow from your laptop against that queue:

   ```bash
   curl -H "Authorization: Bearer $MISTRAL_API_KEY" -H 'Content-Type: application/json' \
     -X POST https://api.mistral.ai/v1/workflows/mwf_device_hitl/execute \
     -d '{"input":{"request":"ship it"},"task_queue":"mwf-esp"}'
   ```

3. The device runs `device.echo`, then blocks and logs
   `WAITING for button (workflow=<id> signal=approve)`.
4. **Press the BOOT button (GPIO0).** The device signals `approve` and its next
   poll unblocks and completes. (No physical access? Type `SIGNAL` on the serial
   console — same effect.)
5. Poll `GET /v1/workflows/executions/<id>` from your laptop until `COMPLETED`.

### Rebooting while paused

The pause is durable, so you can power-cycle the board while it is waiting (step
3) and the workflow stays paused; it completes once the `approve` signal arrives.

One caveat about the button specifically: the device learns *which* execution is
waiting from the task it just processed and holds that id **in RAM**. After a
reboot that in-memory target is gone, so the button has nothing to signal. The
workflow is still durable — send the signal from any client that carries the
execution id (the REST `POST .../signals` above works), and the rebooted board
picks up the task and finishes. Persisting the waiting id to NVS so the button
re-arms across reboots is a noted enhancement (see [ROADMAP.md](../../ROADMAP.md)).

## Make it your own — a second approver

Require two people. Add a second `wait_signal` with its own signal name and id,
and return both approvals:

```json
{ "type": "activity",    "name": "echo",     "id": "request", "args_from": "/input" },
{ "type": "wait_signal", "signal": "approve",  "id": "gate1" },
{ "type": "wait_signal", "signal": "approve2", "id": "gate2" },
{ "type": "complete",    "result_from": "/results" }
```

Now the workflow pauses twice — once per approver — and each approval binds under
its own id (`/results/gate1`, `/results/gate2`), so `complete` returning
`/results` carries both. On the desktop path this just works: the worker
re-derives the declared signal list from the spec, so it now advertises both
`approve` and `approve2`. On the device, add the second name to `kWorkflowSignals`
and send `approve2` from a second client.

To require the *same* signal twice instead, use two `wait_signal` steps with the
same `signal` name — successive same-name signals bind to them in history order.

## Next

There's no cookbook 05 — you've seen the whole grammar. To build your own from
here:

- **[docs/writing-a-workflow.md](../../docs/writing-a-workflow.md)** — write a
  spec and activities from scratch, desktop then device.
- **[docs/architecture.md](../../docs/architecture.md)** — how the replay engine,
  poll loops, and transport fit together.
- **[docs/limitations.md](../../docs/limitations.md)** and
  [ROADMAP.md](../../ROADMAP.md) — what the grammar does and doesn't do yet
  (`parallel` and `timer` are reserved, not implemented).

# Viewing this cookbook's runs in Mistral Studio

The reproducibility of this cookbook is **proven at the command-stream level** —
which is exactly Temporal's determinism guarantee. `run_local.sh` triggers the
same input twice and asserts the worker emits a **byte-identical command stream
and result** both times (see `../evidence/determinism.txt`, regenerated on every
run). A third run with different input deterministically takes the other branch.

```
run A (n=4): command_stream=[SCHEDULE_ACTIVITY_TASK, SCHEDULE_ACTIVITY_TASK, COMPLETE_WORKFLOW_EXECUTION]
run B (n=4): command_stream=[SCHEDULE_ACTIVITY_TASK, SCHEDULE_ACTIVITY_TASK, COMPLETE_WORKFLOW_EXECUTION]   ← identical to A
run C (n=1): result branch = "small"                                                                       ← different input, different branch
result A == result B   (byte-identical)
```

## Seeing it in Studio

Every execution this worker completes is a real Temporal execution recorded by
Mistral and visible in **[Mistral Studio](https://console.mistral.ai) →
Workflows → Executions**, with status `Completed`, start/end times, and
duration. To view your own runs live:

1. `./run_local.sh` (leave off the archive step, or re-register with a stable
   `--workflow` name so it stays listed).
2. Open **console.mistral.ai → Build → Workflows**. The registered workflow
   appears in **List**; its runs appear under **Executions**. Click a row to see
   the execution's event history — the same `ScheduleActivityTask …
   CompleteWorkflowExecution` sequence the worker emitted, now rendered by
   Studio. Re-running the same input produces an identical event history.

> Note: Studio's **Executions** view is a Preview feature; during automated
> capture for this repo it lagged in indexing just-created runs, so the live
> view is the reliable place to see them. The determinism guarantee itself does
> not depend on Studio — it is asserted directly on the worker's wire output in
> `../evidence/determinism.txt`.

## Why this is the real proof

Temporal (and therefore Mistral Workflows) guarantees determinism by replaying a
workflow's event history to reproduce the exact same **commands**. This worker's
replay engine is host-tested against real event histories with an
all-prefixes self-replay oracle (see `core/test/`), and here it is demonstrated
end-to-end against the live Mistral cloud: identical inputs → identical command
stream → identical result.

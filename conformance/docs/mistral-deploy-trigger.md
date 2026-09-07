# Mistral Workflows — live deploy/trigger flow

How a worker deploys workflows to the Mistral control plane and how runs are
triggered **server-side**, established two ways: reading `mistralai-workflows`
3.9.0 source, then **capturing the actual HTTP calls of a live run** with a
traced `httpx` transport (`e2e/evidence/http_trace*.jsonl`). Everything below
was observed against real Mistral cloud on 2026-07-18 unless marked
*source-only*.

Control plane: `https://api.mistral.ai` (Bearer auth). Data plane (Temporal
gRPC): `wf-scheduler.mistral.ai:443`, namespace `<customer-uuid>:<workspace-uuid>`
(from whoami), TLS, Bearer key as gRPC call credential.

## 1. Deploy — what `run_worker()` actually does

Source: `mistralai/workflows/core/worker.py::_run_worker` +
`worker_client/sdk.py` (`PrivateWorkerClient`). Observed sequence:

| step | call | notes |
|---|---|---|
| 1 | `GET /v1/workflows/workers/whoami` → 200 | config discovery: `{namespace, scheduler_url, tls}`; applied to the Temporal client config |
| 2 | Temporal gRPC connect | `wf-scheduler.mistral.ai:443`, namespace from whoami, api_key as bearer |
| 3 | `POST /v1/workflows/register` → 200 | **the deployment**: `{"definitions": [WorkflowSpecWithTaskQueue…], "deployment_name", "worker_name", "deployment_location"}` |
| 4 | Temporal workers start polling | workflow + activity task queues (task_queue == deployment_name) |
| 5 | `POST /v1/workflows/workers/heartbeat` every **10 s** → 200 | body `{"workflow_registration_refs": [{workflow_id, workflow_registration_id}…], deployment_name, worker_name}`; on 404/405 the SDK disables heartbeat and falls back to re-`POST /register` every 10 s |
| 6 | per run: `POST /v1/workflows/executions/register` → 200 | fired by `ExecutionRegistrationInterceptor` from inside workflow execution: `{temporal_workflow_id, temporal_run_id, workflow_name, task_queue, execution_token_hash, …}` |
| 7 | per lifecycle step: `POST /v1/workflows/events` → 200 | workflow/activity started/completed events (SSE-consumable via `GET /v1/workflows/events/stream?workflow_exec_id=…`) |

Registration details (observed body):

- Each definition carries `name`, `task_queue`, `input_schema` /
  `output_schema` (auto-derived from the entrypoint signature — a
  `run(self, name: str) -> str` entrypoint becomes
  `{"properties":{"name":{"type":"string"}},"required":["name"]}` in and
  `{"properties":{"result":{"type":"string"}}}` out), `signals/queries/updates`,
  `enforce_determinism`, `execution_timeout` (ISO-8601, `PT1H` default),
  `schedules`.
- **The SDK appends an internal workflow** `__parallel_execution__`
  (`ParallelExecutionWorkflow`) to every registration — 3 definitions were
  registered for our 2 workflows. Any worker that hosts SDK workflows must
  expect internal registrations to ride along.
- `deployment_name` **is** the task queue: both derive from `DEPLOYMENT_NAME`
  (env, required; `default` is refused when versioning is on). Registration is
  retried with exponential backoff (tenacity, 15 attempts) on transient errors;
  403 carries an `admin_panel_url` for the not-authorized-credential case.
- Registration response: `{workflow_registration_refs: [{workflow_id,
  workflow_registration_id}…], has_conflicts}` — refs in submission order.
- Optional (config, **off by default**, *source-only*): graph upload
  `POST /v1/workflows/{workflow_id}/graphs`.

There is **no worker/deployment delete endpoint** on this surface — a worker
"undeploys" by stopping (heartbeats lapse and `GET /v1/workflows/{name}`
flips `workflow.active` to false). The strongest cleanup is archiving the
workflow: `PUT /v1/workflows/{id-or-name}/archive` (and `/unarchive`).

## 2. Trigger — server-side execution (the non-local path)

⚠ `mistralai.workflows.execute_workflow()` outside a worker context silently
runs the workflow **locally in-process** — nothing reaches the server and
`/v1/workflows/runs` stays empty. The real server path is REST (this is also
what the SDK's own integration tests use — `testing/workflow_helpers.py`):

```
POST /v1/workflows/{name-or-id}/execute
     {"input": {…}, "task_queue": "<queue>"}       # task_queue optional
  →  {"execution_id": "<64-hex>", "status": "RUNNING", "workflow_name", "start_time", …}

GET  /v1/workflows/executions/{execution_id}
  →  {"status": "RUNNING|COMPLETED|FAILED|TERMINATED|TIMED_OUT|CANCELED",
      "result": …, "workflow_id", "deployment_name", "total_duration_ms", …}

GET  /v1/workflows/runs?limit=N                    # server of record, newest first
  →  {"runs":[{execution_id, workflow_name, deployment_name, status, run_id, …}…]}
```

Also live on the execution surface (source + partially exercised):
`POST /v1/workflows/executions/{id}/terminate` (→ 204, verified live) ·
`/cancel` · `/signals` `{"name", "input"}` · `/queries` · `/updates` ·
`GET /v1/workflows/{name}` (worker liveness via `workflow.active` — the
trigger gate) · `GET /v1/workflows` (list) · schedules:
`POST /v1/workflows/{id}/schedules` (*source-only*).

The `execution_id` is the **Temporal workflow id** (64 hex chars — observed in
`executions/register` as `temporal_workflow_id`); `run_id` is the Temporal run
uuid.

## 3. Live E2E evidence (2026-07-18, both server-side, one account)

Runner: `e2e/run_live_mistral_e2e.sh` (worker `e2e/live_worker.py`, trigger
`e2e/live_trigger.py`, evidence JSON in `e2e/evidence/`).

**E2E-1 — Python-only** (`mwf_e2e_py`, activity executed by the SDK worker):

- execution_id `3b06bb5d5f015e2d83d8581601899d34397794d5b2255e4880ba74b145baca22`
- status `COMPLETED` in 1 496 ms; result `{"result": "Hello nimbus from Python"}`
- listed in `GET /v1/workflows/runs` (entry in `e2e/evidence/py_e2e.json`)

**E2E-2 — C++ desktop worker executes the activity** (`mwf_e2e_cpp_live`):
the Python worker hosts only the workflow shell; the workflow schedules the
`greet` activity **by name** onto task queue `mwf-cpp-live`, polled exclusively
by `transport`'s `mwf_worker` binary against `wf-scheduler.mistral.ai:443`.

- execution_id `e8e3be47c90950a09cc280cb555da62744a0959d9afb558a909b17e6a16baa94`
- status `COMPLETED`; run result carries the C++ brand **and** the echoed
  wf_v1 envelope verbatim:
  `{"context": {"namespace": "00000000-…:11111111-…", "execution_id": "e8e3be47…", "execution_token": "…", …}, "payload": "Hello nimbus from C++", "empty": false}`
- C++ worker tick log: `completed activity=greet (wire=json/wf_v1
  token_bytes=168 result_bytes=23) elapsed_ms=3429`
- listed in `GET /v1/workflows/runs` (entry in `e2e/evidence/cpp_e2e.json`)

**Wire facts for the device:**

- Activity input from an SDK-driven run arrives as **`json/wf_v1`** (the
  Mistral WorkflowContext envelope) — exactly what the goldens predicted; the
  C++ codec's context echo is accepted by the server and the SDK workflow.
- **task_token = 168 bytes** on real Mistral cloud (nanopb caps: budget 256 B
  with headroom; the local dev-server tokens were shorter).
- **Non-default activity task queues are accepted** by the Mistral frontend
  (`mwf-cpp-live` worked first try) — no fallback to `default` was needed.
- Empty long-polls release at ~45 s with gRPC OK + empty body (not
  DEADLINE_EXCEEDED) — handle both idle shapes.

## 4. Quirks learned live

- **`@activity` is a decorator factory** — bare `@activity` (as in the SDK's
  own README) fails only at *call* time inside the workflow
  (`activity_not_module_level: got <class 'str'>`), which presents as a
  workflow-task retry loop: the execution sits `RUNNING` forever. Use
  `@activity()`.
- **The SDK pins activities to the worker's own task queue**
  (`core/activity.py` wrapper → `config.get_effective_task_queue()`); there is
  no per-call queue override. Cross-queue scheduling (our C++ topology) needs
  raw `temporalio.workflow.execute_activity("name", …,
  task_queue=…)` inside the workflow — which works fine under the SDK's
  sandbox/interceptors.
- **By-name activities skip the SDK's result unwrap**: the wf_v1 envelope comes
  back as a dict (`{"context", "payload", "empty"}`), not the bare payload —
  unwrap `["payload"]` in the workflow if you want a clean result (we kept the
  envelope: it is stronger evidence).
- The Temporal **workflow sandbox re-imports the workflow module** — module-
  level imports that touch `urllib.request` (e.g. `httpx`) must be inside
  `workflow.unsafe.imports_passed_through()` or worker startup fails
  validation.
- A stuck-RUNNING execution keeps consuming workflow-task retries until
  terminated — always `POST …/terminate` leftovers (cleanup:
  `e2e/live_cleanup.py`, verified 204).

## 5. Cleanup ledger

- Both E2E executions ended `COMPLETED`; one earlier stuck run
  (the bare-`@activity` casualty) was terminated (204).
- Workers stopped (Python + C++); registrations lapse via missed heartbeats.
- E2E workflows archived via `PUT /v1/workflows/{name}/archive`
  (`e2e/live_cleanup.py --archive`); `…/unarchive` restores them before a
  re-run (the runner's worker re-registers them regardless).
- No deployment delete endpoint exists — documented above.

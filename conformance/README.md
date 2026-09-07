# conformance

The test oracle. This is a small Python package that runs the **real** Mistral
Workflows and Temporal SDKs, captures what they put on the wire, and saves it as
fixed files (`goldens/`) that the C++ code is tested against. It also has the
end-to-end scripts that drive a whole workflow through a running worker.

**This is for contributors and testers, not end users.** If you want to write or
run a workflow, start at [../README.md](../README.md) and
[../docs/writing-a-workflow.md](../docs/writing-a-workflow.md). Nothing here ships
in the firmware.

## Why it exists

The C++ engine has to reproduce two byte-formats the SDKs define: Temporal event
histories (what the replay engine consumes) and the Mistral payload envelope
(what the codec reads and writes). Rather than diff the C++ against a live server
on every run, we capture those bytes once from the SDKs into `goldens/`, and the
C++ host tests assert against the captured files. When an SDK version changes, you
re-capture and the goldens move; the C++ tests catch anything that drifted.

The full cross-track rationale is in
[../contracts/CONTRACTS.md](../contracts/CONTRACTS.md) ("Integration currency").

## Setup

You need Python ≥ 3.10 and, for anything that captures histories, the Temporal
CLI (it bundles a local dev server):

```bash
brew install temporal          # provides `temporal server start-dev`
make venv                      # creates .venv and installs the pinned SDKs
```

`make venv` installs exactly the two SDK versions the committed goldens were
captured with ([requirements.txt](requirements.txt)):

| package | version | role |
|---|---|---|
| `mistralai-workflows` | 3.9.0 | the Mistral payload-envelope oracle |
| `temporalio` | 1.27.2 | the event-history oracle + local dev server |

The versions are pinned on purpose — a floating resolve would let a re-capture
silently diverge from the committed goldens. Bump them deliberately and re-run
`make goldens`.

You don't have to use `make venv`: the capture and e2e scripts create `.venv`
themselves on first run. To reuse an interpreter that already has the SDKs, set
`WFPY=/path/to/python`.

## Regenerate the goldens

```bash
make goldens        # local, offline, deterministic — boots a temporal dev server
make goldens-live   # + a read-only smoke against api.mistral.ai (needs a key)
```

Both call [`capture/run_all.sh`](capture/run_all.sh), which provisions the venv,
boots and tears down a local `temporal server start-dev` (SQLite, no cloud), and
runs the two captures:

- **[`capture/capture_history.py`](capture/capture_history.py)** runs the
  reference workflows in [`capture/workflows.py`](capture/workflows.py) on the dev
  server and writes each execution's full event history to
  `goldens/histories/`. Every file is re-parsed through
  `temporalio.client.WorkflowHistory.from_json` before it lands, so one that
  won't load back never gets committed.
- **[`capture/capture_payloads.py`](capture/capture_payloads.py)** drives the real
  SDK codec (`MistralWorkflowsPayloadCodec`) to produce the exact activity
  input/result payloads and writes them to `goldens/payloads/` with both the wire
  bytes and decoded views. Each is round-tripped back through the codec's decoder.

`make goldens-live` additionally runs
[`capture/live_smoke.py`](capture/live_smoke.py): it hits
`GET /v1/workflows/workers/whoami`, connects a `temporalio` client to the hosted
scheduler, and lists executions. It is **read-only** — it deploys and triggers
nothing (that would spin a paid, long-lived run). It reads `MISTRAL_API_KEY` from
the environment or a local `.env` and never prints it.

What lands where, and which C++ code each set feeds:

| goldens | captured from | tested against |
|---|---|---|
| [`goldens/histories/`](goldens/histories/) | real Temporal event histories | the core replay engine |
| [`goldens/payloads/`](goldens/payloads/) | the Mistral payload codec | the wire-format codec |

The histories embed run-specific ids and timestamps, so a fresh capture differs
from the committed one in those fields; the event *sequence* and payloads are
stable. The payloads are fully deterministic. See
[`goldens/README.md`](goldens/README.md) for the file-by-file breakdown and
[`goldens/NOTES.md`](goldens/NOTES.md) for the byte-level findings a codec
implementer should read first.

## End-to-end scripts

These drive a real worker through a whole workflow, front to back. Unlike the
goldens, they need built C++ binaries (from [`../transport`](../transport/); the
scripts build them if missing).

| script | server | what it proves | cost |
|---|---|---|---|
| [`e2e/run_desktop_worker_e2e.sh`](e2e/run_desktop_worker_e2e.sh) | local dev server | a Python workflow schedules activities onto a queue only the C++ `mwf_worker` polls; the result carries the C++ output | free, offline |
| [`e2e/run_live_mistral_e2e.sh`](e2e/run_live_mistral_e2e.sh) | api.mistral.ai | one Python-only run, then one where the C++ `mwf_worker` completes an activity on real cloud | **paid, live** |
| [`e2e/run_desktop_sole_worker_e2e.sh`](e2e/run_desktop_sole_worker_e2e.sh) | api.mistral.ai | `mwf_sole_worker` registers, runs, and completes a whole workflow as the only worker — no Python host anywhere | **paid, live** |

```bash
./e2e/run_desktop_worker_e2e.sh          # offline gate; needs the temporal CLI

MISTRAL_API_KEY=sk-... ./e2e/run_live_mistral_e2e.sh   # spins real executions
```

The live scripts read `MISTRAL_API_KEY` from the environment or a local `.env`,
never print it, and clean up the executions and deployments they create. They
write run ids, statuses, and logs to `e2e/evidence/` (committed). The Python
drivers are runnable on their own — [`e2e/live_trigger.py`](e2e/live_trigger.py)
triggers a registered workflow over REST and polls it to a terminal status; see
`--help`.

The hardware gate, [`e2e/device_e2e_worker.py`](e2e/device_e2e_worker.py), is the
same cross-queue trick but the activity queue is polled by an ESP32 instead of a
C++ desktop binary. It has no wrapper script — run it directly with the device on
the network:

```bash
DEPLOYMENT_NAME=mwf-esp32-shell MWF_DEVICE_TASK_QUEUE=mwf-esp32-e2e \
  MISTRAL_API_KEY=sk-... .venv/bin/python e2e/device_e2e_worker.py
```

The live control-plane calls these scripts depend on — register, heartbeat,
execute, poll — are documented in
[`docs/mistral-deploy-trigger.md`](docs/mistral-deploy-trigger.md).

## Layout

```
mwf_conformance/            the installable package (oracle helpers)
capture/workflows.py        reference workflows + activities (linear, conditional)
capture/capture_history.py  runs them on a local dev server -> goldens/histories/
capture/capture_payloads.py real SDK codec -> goldens/payloads/
capture/live_smoke.py       read-only smoke vs api.mistral.ai
capture/run_all.sh          one command: venv, boot dev server, both captures
goldens/                    the recorded vectors + README.md + NOTES.md
e2e/                        the end-to-end scripts + Python drivers + evidence/
docs/mistral-deploy-trigger.md   the live register/trigger REST flow
Makefile                    `make venv` / `make goldens` / `make goldens-live`
requirements.txt            the pinned oracle SDK versions
```

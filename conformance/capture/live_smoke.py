#!/usr/bin/env python3
"""live_smoke.py — read-mostly smoke against the REAL Mistral Workflows backend.

A secondary gate. Confirms the live control plane is reachable and
documents (does NOT execute) the deploy/trigger path. Deliberately cheap and
read-only: no workflow is deployed or triggered here (that spins a paid,
long-lived execution). The deterministic goldens come from the LOCAL dev server
(``capture_history.py`` / ``capture_payloads.py``); this script is the final
"the wire format we reproduce is the real one" check.

Steps:
  1. ``GET /v1/workflows/workers/whoami`` (Bearer key) -> scheduler_url,
     namespace, tls. The one call proven live this session.
  2. Connect a ``temporalio`` client to that scheduler with just
     ``api_key + namespace + tls`` (no mTLS) and list workflow executions
     (read-only ``ListWorkflowExecutions`` via ``client.list_workflows``).
  3. Print the documented deploy+trigger requirements (worker registration +
     StartWorkflowExecution) without performing them.

Key handling: ``MISTRAL_API_KEY`` is read from the env or `.env`
and NEVER printed.

Usage:  python capture/live_smoke.py
"""

from __future__ import annotations

import asyncio
import json
import os
import pathlib
import urllib.request

WHOAMI_URL = "https://api.mistral.ai/v1/workflows/workers/whoami"


def _load_key() -> str:
    key = os.environ.get("MISTRAL_API_KEY")
    if key:
        return key
    env = pathlib.Path(os.environ.get("MWF_ENV_FILE", ".env"))
    if env.exists():
        for line in env.read_text().splitlines():
            if line.startswith("MISTRAL_API_KEY="):
                return line.split("=", 1)[1].strip().strip('"').strip("'")
    raise SystemExit("MISTRAL_API_KEY not found (env or a local .env file)")


def whoami(key: str) -> dict:
    req = urllib.request.Request(WHOAMI_URL, headers={"Authorization": f"Bearer {key}"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read().decode())


async def list_executions(scheduler_url: str, namespace: str, key: str) -> None:
    from temporalio.client import Client

    # Hosted scheduler uses api_key + namespace + TLS, NO mTLS (confirmed:
    # whoami reports tls:true; plain temporalio client connects with just these).
    print(f"  connecting temporalio client -> {scheduler_url} ns={namespace} tls=True")
    try:
        client = await Client.connect(
            scheduler_url, namespace=namespace, api_key=key, tls=True
        )
    except Exception as e:  # noqa: BLE001 - smoke: report, don't crash
        print(f"  [connect] FAILED: {type(e).__name__}: {e}")
        return

    print("  connected. listing workflow executions (read-only)...")
    try:
        n = 0
        async for wf in client.list_workflows(page_size=10):
            n += 1
            print(f"    - {wf.id}  type={wf.workflow_type}  status={wf.status}")
            if n >= 10:
                break
        if n == 0:
            print("    (no executions in this namespace yet)")
        else:
            print(f"    listed {n} execution(s)")
    except Exception as e:  # noqa: BLE001
        print(f"  [list_workflows] {type(e).__name__}: {e}")


DEPLOY_NOTES = """
  Deploy + trigger (documented, NOT performed here):
    The Mistral Workflows backend IS a hosted Temporal frontend. There is no
    simple REST "register workflow" call in the v1 scope; deployment = running a
    Temporal WORKER against the scheduler:
      1. Connect: temporalio Client/Worker to `scheduler_url` (wf-scheduler.
         mistral.ai:443) with api_key + namespace + tls=True.
      2. Register: the worker polls a task queue and self-registers its workflow
         types (SDK: worker.py auto-register-as-current for local deployments;
         WF_1104 = 'credential not authorized for this deployment' if the key
         lacks rights).
      3. Trigger: StartWorkflowExecution (client.start_workflow) on that task
         queue. Payloads cross via MistralWorkflowsPayloadCodec => the json/wf_v1
         envelope captured in goldens/payloads/.
    Skipped here on purpose: a live execution is paid + long-lived, and the C++
    tracks validate against the local goldens, not a live run (CONTRACTS.md
    "Integration currency"). This is the final smoke gate, kept read-only.
"""


def main() -> None:
    key = _load_key()
    print("== live smoke: api.mistral.ai ==")
    print("1) whoami")
    info = whoami(key)
    # redact nothing sensitive here (namespace is a workspace id, not a secret),
    # but never print the key.
    print(f"   scheduler_url={info.get('scheduler_url')}")
    print(f"   namespace={info.get('namespace')}")
    print(f"   tls={info.get('tls')}")

    scheduler = info.get("scheduler_url")
    namespace = info.get("namespace")
    print("2) list executions on hosted scheduler")
    if scheduler and namespace:
        asyncio.run(list_executions(scheduler, namespace, key))
    else:
        print("   whoami missing scheduler_url/namespace; skipping list")

    print("3) deploy/trigger requirements")
    print(DEPLOY_NOTES)


if __name__ == "__main__":
    main()

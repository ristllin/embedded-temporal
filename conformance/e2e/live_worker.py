#!/usr/bin/env python3
"""live_worker.py — the live-deploy Python worker (real Mistral cloud).

Hosts two tiny workflows against api.mistral.ai / wf-scheduler.mistral.ai:

* ``mwf_e2e_py``       — greet activity executed by THIS worker (pure-Python E2E).
* ``mwf_e2e_cpp_live`` — the workflow SHELL only: its single activity is
  scheduled by NAME onto the ``mwf-cpp-live`` task queue, which only the C++
  ``mwf_worker`` binary polls. The Python worker never registers an activity
  implementation for that queue, so a completed run proves the C++ desktop
  worker executed the activity on the real Mistral cloud.

Every control-plane HTTP call the SDK makes (register, heartbeat, graphs, ...)
is traced to ``MWF_HTTP_TRACE`` (JSONL) via a patched ``httpx`` send, so the
deploy flow is captured from a live run, not inferred. Authorization headers
are never logged.

Usage (key in env, never printed)::

    DEPLOYMENT_NAME=mwf-e2e MISTRAL_API_KEY=... \
        MWF_HTTP_TRACE=e2e/evidence/http_trace.jsonl \
        .venv/bin/python e2e/live_worker.py
"""

from __future__ import annotations

import asyncio
import json
import os
import time
from datetime import timedelta

# httpx must be a pass-through import: the Temporal workflow sandbox re-imports
# this module per workflow, and httpx pulls in urllib.request (restricted).
from temporalio import workflow as _sandbox_marker  # noqa: E402

with _sandbox_marker.unsafe.imports_passed_through():
    import httpx

# ── httpx trace shim (installed before the SDK builds its clients) ────────── #
_TRACE_PATH = os.environ.get("MWF_HTTP_TRACE")


def _install_httpx_trace() -> None:
    if not _TRACE_PATH:
        return

    def _record(method: str, url: str, status: int, req_body: bytes | None) -> None:
        entry: dict = {
            "ts": round(time.time(), 3),
            "method": method,
            "url": url,
            "status": status,
        }
        # Bodies only for the workflows control-plane calls (they carry the
        # registration contract); cap size, never log headers.
        if "/v1/workflows" in url and req_body:
            entry["request_body"] = req_body[:4000].decode("utf-8", "replace")
        with open(_TRACE_PATH, "a") as fh:
            fh.write(json.dumps(entry) + "\n")

    orig_async_send = httpx.AsyncClient.send
    orig_sync_send = httpx.Client.send

    async def traced_async_send(self, request, **kw):  # type: ignore[no-untyped-def]
        resp = await orig_async_send(self, request, **kw)
        try:
            _record(request.method, str(request.url), resp.status_code, request.content)
        except Exception:
            pass
        return resp

    def traced_sync_send(self, request, **kw):  # type: ignore[no-untyped-def]
        resp = orig_sync_send(self, request, **kw)
        try:
            _record(request.method, str(request.url), resp.status_code, request.content)
        except Exception:
            pass
        return resp

    httpx.AsyncClient.send = traced_async_send  # type: ignore[method-assign]
    httpx.Client.send = traced_sync_send  # type: ignore[method-assign]


_install_httpx_trace()

# SDK imports AFTER the shim so its clients inherit the traced send.
import temporalio.common  # noqa: E402
import temporalio.workflow  # noqa: E402
from mistralai.workflows import activity, run_worker, workflow  # noqa: E402

CPP_TASK_QUEUE = os.environ.get("MWF_CPP_TASK_QUEUE", "mwf-cpp-live")


@activity()  # NB: decorator FACTORY — bare @activity silently breaks at call time
async def py_greet(name: str) -> str:
    """Executed by THIS Python worker (E2E-1)."""
    return f"Hello {name} from Python"


@workflow.define(name="mwf_e2e_py")
class PyE2EWorkflow:
    @workflow.entrypoint
    async def run(self, name: str) -> str:
        return await py_greet(name)


@workflow.define(name="mwf_e2e_cpp_live")
class CppLiveWorkflow:
    """Workflow shell in Python; the activity runs on the C++ worker's queue."""

    @workflow.entrypoint
    async def run(self, name: str) -> str:
        # By-name scheduling onto the C++ queue: bypasses the SDK's activity
        # wrapper (which pins activities to the worker's own task queue).
        greeting: str = await temporalio.workflow.execute_activity(
            "greet",
            name,
            task_queue=CPP_TASK_QUEUE,
            start_to_close_timeout=timedelta(seconds=60),
            schedule_to_close_timeout=timedelta(seconds=240),
            retry_policy=temporalio.common.RetryPolicy(maximum_attempts=2),
            result_type=str,
        )
        return greeting


async def main() -> None:
    await run_worker([PyE2EWorkflow, CppLiveWorkflow])


if __name__ == "__main__":
    asyncio.run(main())

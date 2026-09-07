#!/usr/bin/env python3
"""desktop_worker_e2e.py — the desktop worker E2E driver (Python workflow host, C++ activities).

A Python ``temporalio`` worker runs the WORKFLOW task queue; the workflow
schedules its activities onto a *different* task queue (``mwf-cpp-e2e``) that
only the C++ ``mwf_worker`` binary polls. The workflow result therefore proves
the C++ worker polled, decoded, dispatched, and responded on the real Temporal
wire protocol.

Run via ``run_desktop_worker_e2e.sh`` (which boots the dev server and the C++
worker); standalone it assumes both are already up::

    TEMPORAL_ADDRESS=127.0.0.1:7233 .venv/bin/python e2e/desktop_worker_e2e.py

Exit 0 iff the workflow completed AND the result carries the C++ brand.
"""

from __future__ import annotations

import asyncio
import os
import sys
import uuid
from datetime import timedelta

from temporalio import workflow
from temporalio.client import Client
from temporalio.common import RetryPolicy
from temporalio.worker import Worker

# The C++ worker's queue (mwf_worker default) and the Python workflow queue.
CPP_TASK_QUEUE = os.environ.get("MWF_CPP_TASK_QUEUE", "mwf-cpp-e2e")
PY_TASK_QUEUE = "mwf-py-e2e"

# Activities are referenced BY NAME (they are implemented in C++, not here).
_ACT_OPTS = dict(
    start_to_close_timeout=timedelta(seconds=30),
    retry_policy=RetryPolicy(maximum_attempts=3),
)


@workflow.defn
class CrossLangLinearWorkflow:
    """workflows.py LinearWorkflow shape — greet -> shout — but the activities
    are routed to the C++ worker's task queue."""

    @workflow.run
    async def run(self, name: str) -> str:
        greeting = await workflow.execute_activity(
            "greet", name, task_queue=CPP_TASK_QUEUE, **_ACT_OPTS
        )
        loud = await workflow.execute_activity(
            "shout", greeting, task_queue=CPP_TASK_QUEUE, **_ACT_OPTS
        )
        return loud


async def main() -> int:
    address = os.environ.get("TEMPORAL_ADDRESS", "127.0.0.1:7233")
    client = await Client.connect(address)
    print(f"[e2e] connected to {address}")

    async with Worker(
        client,
        task_queue=PY_TASK_QUEUE,
        workflows=[CrossLangLinearWorkflow],
    ):
        wf_id = f"mwf-m3-e2e-{uuid.uuid4().hex[:8]}"
        print(
            f"[e2e] starting {wf_id}: workflow on '{PY_TASK_QUEUE}', "
            f"activities on '{CPP_TASK_QUEUE}' (C++ worker)"
        )
        result = await asyncio.wait_for(
            client.execute_workflow(
                CrossLangLinearWorkflow.run,
                "world",
                id=wf_id,
                task_queue=PY_TASK_QUEUE,
            ),
            timeout=90,
        )
        print(f"[e2e] workflow result: {result!r}")

        expected = "HELLO WORLD FROM C++!"
        if result != expected:
            print(f"[e2e] FAIL: expected {expected!r}", file=sys.stderr)
            return 1
        print("[e2e] PASS: activity round-trip executed by the C++ worker")
        return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))

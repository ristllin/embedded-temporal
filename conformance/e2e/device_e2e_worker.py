#!/usr/bin/env python3
"""device_e2e_worker.py — the hardware gate. Python hosts a workflow SHELL whose
single activity ``device.echo`` is scheduled by name onto the task queue that
the **ESP32-S3** (firmware firmware) polls against wf-scheduler.mistral.ai:443.

A completed run whose result carries ``"device":"mwf-esp32s3"`` proves the
microcontroller executed a real Mistral Workflows activity on the live cloud.

The workflow itself runs on DEPLOYMENT_NAME's queue (polled by THIS Python
worker); the activity runs on MWF_DEVICE_TASK_QUEUE (polled ONLY by the device).
The Python worker registers no ``device.echo`` implementation, so it cannot
complete the activity — only the ESP32 can.

Usage (key in env, never printed)::

    DEPLOYMENT_NAME=mwf-esp32-shell MWF_DEVICE_TASK_QUEUE=mwf-esp32-e2e \
        MISTRAL_API_KEY=... .venv/bin/python e2e/device_e2e_worker.py
"""

from __future__ import annotations

import asyncio
import os
from datetime import timedelta
from typing import Any

from temporalio import workflow as _sandbox_marker

with _sandbox_marker.unsafe.imports_passed_through():
    import httpx  # noqa: F401  (sandbox import hygiene, mirrors live_worker.py)

import temporalio.common  # noqa: E402
import temporalio.workflow  # noqa: E402
from mistralai.workflows import run_worker, workflow  # noqa: E402

DEVICE_TASK_QUEUE = os.environ.get("MWF_DEVICE_TASK_QUEUE", "mwf-esp32-e2e")


@workflow.define(name="mwf_e2e_device")
class DeviceEchoWorkflow:
    """Shell only — the ``device.echo`` activity runs on the ESP32's queue."""

    @workflow.entrypoint
    async def run(self, name: str) -> Any:
        # By-name scheduling onto the device queue (bypasses the SDK's
        # own-queue activity pinning, same trick as the C++ live E2E).
        return await temporalio.workflow.execute_activity(
            "device.echo",
            name,
            task_queue=DEVICE_TASK_QUEUE,
            start_to_close_timeout=timedelta(seconds=90),
            schedule_to_close_timeout=timedelta(seconds=240),
            retry_policy=temporalio.common.RetryPolicy(maximum_attempts=2),
            result_type=dict,
        )


async def main() -> None:
    await run_worker([DeviceEchoWorkflow])


if __name__ == "__main__":
    asyncio.run(main())

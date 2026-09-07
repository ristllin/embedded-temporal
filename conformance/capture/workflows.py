#!/usr/bin/env python3
"""workflows.py — reference workflows + activities for golden capture.

Two representative shapes, matching the v1 ``IWorkflowSpec`` grammar
(activity + sequence + conditional + complete) in ``../contracts/CONTRACTS.md``:

* ``LinearWorkflow``      — activity -> activity -> complete (a sequence).
* ``ConditionalWorkflow`` — activity -> conditional branch -> complete.

These are plain ``temporalio`` workflows/activities. They are deliberately
deterministic (no wall-clock, no random, no external IO) so the captured event
history is stable across re-runs and can serve as a replay oracle for the
replay engine.

The module is imported by ``capture_history.py`` (which runs a worker against a
local ``temporal server start-dev`` frontend). It carries NO Mistral-specific
codec — the Mistral WorkflowContext envelope is exercised separately by
``capture_payloads.py`` against the real SDK codec.
"""

from __future__ import annotations

from datetime import timedelta

from temporalio import activity, workflow

# Single shared task queue for the capture worker.
TASK_QUEUE = "conformance-goldens"

# No retries: keep histories minimal + deterministic (a retry would inject
# ActivityTaskFailed/timer events that vary run-to-run).
_NO_RETRY = workflow.unsafe.imports_passed_through  # (marker; see RetryPolicy below)


# --------------------------------------------------------------------------- #
# Activities                                                                   #
# --------------------------------------------------------------------------- #
@activity.defn
async def greet(name: str) -> str:
    """First step of the linear workflow: build a greeting."""
    return f"hello, {name}"


@activity.defn
async def shout(text: str) -> str:
    """Second step of the linear workflow: transform the prior result."""
    return text.upper() + "!"


@activity.defn
async def parity(n: int) -> str:
    """Branch predicate input: classify a number (drives the conditional)."""
    return "even" if n % 2 == 0 else "odd"


@activity.defn
async def even_branch(n: int) -> str:
    """Taken when ``parity`` said 'even'."""
    return f"{n} is even; halved={n // 2}"


@activity.defn
async def odd_branch(n: int) -> str:
    """Taken when ``parity`` said 'odd'."""
    return f"{n} is odd; tripled+1={3 * n + 1}"


# --------------------------------------------------------------------------- #
# Workflows                                                                    #
# --------------------------------------------------------------------------- #
from temporalio.common import RetryPolicy  # noqa: E402

# maximum_attempts=1 => no automatic retries => deterministic, minimal history.
_ACT_OPTS = dict(
    start_to_close_timeout=timedelta(seconds=10),
    retry_policy=RetryPolicy(maximum_attempts=1),
)


@workflow.defn
class LinearWorkflow:
    """activity(greet) -> activity(shout) -> complete. A pure sequence."""

    @workflow.run
    async def run(self, name: str) -> str:
        greeting = await workflow.execute_activity(greet, name, **_ACT_OPTS)
        loud = await workflow.execute_activity(shout, greeting, **_ACT_OPTS)
        return loud


@workflow.defn
class ConditionalWorkflow:
    """activity(parity) -> conditional -> activity(branch) -> complete."""

    @workflow.run
    async def run(self, n: int) -> str:
        kind = await workflow.execute_activity(parity, n, **_ACT_OPTS)
        if kind == "even":
            result = await workflow.execute_activity(even_branch, n, **_ACT_OPTS)
        else:
            result = await workflow.execute_activity(odd_branch, n, **_ACT_OPTS)
        return result


ALL_ACTIVITIES = [greet, shout, parity, even_branch, odd_branch]
ALL_WORKFLOWS = [LinearWorkflow, ConditionalWorkflow]

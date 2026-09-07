#!/usr/bin/env python3
"""capture_history.py — record real Temporal workflow event histories → goldens.

Runs the reference workflows in ``workflows.py`` against a **local**
``temporal server start-dev`` frontend (SQLite, no cloud dependency), then dumps
each execution's full **event history** — the ordered ``HistoryEvent`` stream
that drives deterministic replay — to ``goldens/histories/<name>.json`` as
canonical Temporal history JSON (google.protobuf json, camelCase field names —
the same shape ``tctl``/the Temporal UI export).

These feed core: the ReplayEngine replays each history against the
matching WorkflowSpec and its emitted Commands are asserted against the oracle
(the replay-green-vs-oracle gate).

Prereq: a dev server on ``localhost:7233`` (started by ``run_all.sh``). Override
the address with ``TEMPORAL_ADDRESS``.

Verification: after writing each file it is re-parsed via
``temporalio.client.WorkflowHistory.from_json`` — the same parser the core's
Python oracle uses — so a golden that would not load back never lands on disk.
"""

from __future__ import annotations

import asyncio
import json
import os
import pathlib
import sys
import uuid

from google.protobuf.json_format import MessageToJson
from temporalio.api.history.v1 import History
from temporalio.client import Client, WorkflowHistory
from temporalio.worker import Worker

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from workflows import (  # noqa: E402
    ALL_ACTIVITIES,
    ALL_WORKFLOWS,
    TASK_QUEUE,
    ConditionalWorkflow,
    LinearWorkflow,
)

REPO = pathlib.Path(__file__).resolve().parents[1]
OUT_DIR = REPO / "goldens" / "histories"
ADDRESS = os.environ.get("TEMPORAL_ADDRESS", "127.0.0.1:7233")
NAMESPACE = os.environ.get("TEMPORAL_NAMESPACE", "default")


def _history_to_json(hist: WorkflowHistory) -> str:
    """Serialize a fetched history to canonical Temporal history JSON.

    Wraps the events in a ``History`` proto and renders with the protobuf JSON
    printer (camelCase, the shape the Temporal UI / tctl produce and that
    ``WorkflowHistory.from_json`` round-trips)."""
    proto = History(events=hist.events)
    return MessageToJson(proto, indent=2)


def _verify_reloads(workflow_id: str, js: str) -> int:
    """Round-trip the golden through the oracle's own parser. Returns event count."""
    reloaded = WorkflowHistory.from_json(workflow_id, json.loads(js))
    return len(list(reloaded.events))


async def _run_one(client: Client, name: str, wf, arg, filename: str) -> dict:
    workflow_id = f"{name}-{uuid.uuid4().hex[:8]}"
    result = await client.execute_workflow(
        wf.run, arg, id=workflow_id, task_queue=TASK_QUEUE
    )
    handle = client.get_workflow_handle(workflow_id)
    hist = await handle.fetch_history()
    js = _history_to_json(hist)

    n_events = _verify_reloads(workflow_id, js)
    event_types = [
        e.get("eventType", "?")
        for e in json.loads(js).get("events", [])
    ]

    out = OUT_DIR / filename
    out.write_text(js + "\n")
    print(f"  [{name}] result={result!r}")
    print(f"    workflow_id={workflow_id}  events={n_events}")
    print(f"    -> {out.relative_to(REPO)}")
    print(f"    types: {', '.join(t.replace('EVENT_TYPE_', '') for t in event_types)}")
    return {"name": name, "file": filename, "events": n_events, "result": result}


async def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    client = await Client.connect(ADDRESS, namespace=NAMESPACE)
    print(f"connected: {ADDRESS} ns={NAMESPACE}")

    async with Worker(
        client,
        task_queue=TASK_QUEUE,
        workflows=ALL_WORKFLOWS,
        activities=ALL_ACTIVITIES,
    ):
        captured = []
        captured.append(
            await _run_one(client, "linear", LinearWorkflow, "world", "linear.json")
        )
        # Conditional: capture BOTH branches so the replay engine can exercise
        # the true/false step lists of the conditional grammar node.
        captured.append(
            await _run_one(
                client, "conditional_even", ConditionalWorkflow, 8,
                "conditional_even.json",
            )
        )
        captured.append(
            await _run_one(
                client, "conditional_odd", ConditionalWorkflow, 7,
                "conditional_odd.json",
            )
        )

    print(f"\ncaptured {len(captured)} histories -> {OUT_DIR.relative_to(REPO)}/")


if __name__ == "__main__":
    asyncio.run(main())

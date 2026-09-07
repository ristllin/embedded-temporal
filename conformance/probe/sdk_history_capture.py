#!/usr/bin/env python3
"""sdk_history_capture.py — capture the REAL command stream a mistralai-workflows
worker emits, to ENUMERATE the optional __internal__ lifecycle overhead.

Spawns the repo's sandbox-clean SDK worker (e2e/live_worker.py) as a subprocess,
triggers its `mwf_e2e_py` workflow via REST, waits for COMPLETED, then pulls the
full Temporal history via raw gRPC. The history reveals every marker /
local-activity the SDK injects (register_execution, emit_workflow_started,
emit_workflow_completed, ...) that our raw sole-worker probe proved are NOT
required. Contrast probe/final_history_no_activity.json (5 events, zero internal)
with probe/sdk_history.json.

Key never printed. Cleanup: the spawned worker is terminated at the end.
"""
from __future__ import annotations

import asyncio
import json
import os
import signal
import subprocess
import sys
import time

from google.protobuf.json_format import MessageToDict

import temporalio.api.common.v1 as common
import temporalio.api.workflowservice.v1 as wsv1
from temporalio.client import Client

try:
    from ._common import load_key, rest_client, whoami
except ImportError:  # run as a plain script from the probe/ directory
    from _common import load_key, rest_client, whoami

PROBE_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(PROBE_DIR)
TERMINAL = {"COMPLETED", "FAILED", "TERMINATED", "TIMED_OUT", "CANCELED"}
WF_NAME = "mwf_e2e_py"
QUEUE = "mwf-e2e"


async def main() -> int:
    key = load_key()
    c = rest_client(key)
    who = whoami(c)
    namespace = who["namespace"]
    scheduler_url = who.get("scheduler_url") or "wf-scheduler.mistral.ai:443"

    env = dict(os.environ)
    env["MISTRAL_API_KEY"] = key
    env["DEPLOYMENT_NAME"] = QUEUE
    worker = subprocess.Popen(
        [os.path.join(REPO, ".venv", "bin", "python"), os.path.join(REPO, "e2e", "live_worker.py")],
        cwd=REPO,
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    print(f"[sdk] spawned SDK worker pid={worker.pid} on queue={QUEUE}")
    try:
        for _ in range(60):
            r = c.get(f"/v1/workflows/{WF_NAME}")
            if r.status_code == 200 and r.json().get("workflow", r.json()).get("active"):
                break
            await asyncio.sleep(1)
        print("[sdk] worker active; triggering run")

        r = c.post(f"/v1/workflows/{WF_NAME}/execute", json={"input": {"name": "nimbus"}, "task_queue": QUEUE})
        r.raise_for_status()
        execution_id = r.json()["execution_id"]
        print(f"[sdk] execution_id={execution_id}")

        status = None
        for _ in range(60):
            await asyncio.sleep(2)
            sr = c.get(f"/v1/workflows/executions/{execution_id}")
            if sr.status_code == 200:
                status = sr.json().get("status")
                if status in TERMINAL:
                    break
        print(f"[sdk] status={status}")

        client = await Client.connect(scheduler_url, namespace=namespace, api_key=key, tls=True)
        hist = await client.workflow_service.get_workflow_execution_history(
            wsv1.GetWorkflowExecutionHistoryRequest(
                namespace=namespace,
                execution=common.WorkflowExecution(workflow_id=execution_id),
            )
        )
        hd = MessageToDict(hist, preserving_proto_field_name=True)
        with open(os.path.join(PROBE_DIR, "sdk_history.json"), "w") as fh:
            json.dump(hd, fh, indent=2, default=str)

        events = hd.get("history", {}).get("events", [])
        print(f"[sdk] history events ({len(events)}):")
        markers = []
        scheduled_activities = []
        for e in events:
            et = e.get("event_type")
            detail = ""
            if et == "EVENT_TYPE_MARKER_RECORDED":
                attrs = e.get("marker_recorded_event_attributes", {})
                mname = attrs.get("marker_name")
                markers.append(mname)
                # local-activity markers stash the activity name in details/'data'
                la_name = None
                try:
                    det = attrs.get("details", {})
                    blob = json.dumps(det)
                    for cand in ("register_execution", "emit_workflow_started", "emit_workflow_completed",
                                 "emit_workflow_failed", "emit_task"):
                        if cand in blob:
                            la_name = cand
                            break
                except Exception:
                    pass
                detail = f"  marker={mname} local_activity~={la_name}"
            if et == "EVENT_TYPE_ACTIVITY_TASK_SCHEDULED":
                an = e.get("activity_task_scheduled_event_attributes", {}).get("activity_type", {}).get("name")
                scheduled_activities.append(an)
                detail = f"  activity={an}"
            print(f"   {e.get('event_id')} {et}{detail}")

        summary = {
            "execution_id": execution_id,
            "final_status": status,
            "n_events": len(events),
            "event_types": [e.get("event_type") for e in events],
            "marker_names": markers,
            "scheduled_activities": scheduled_activities,
            "captured_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "workflow": WF_NAME,
            "task_queue": QUEUE,
        }
        with open(os.path.join(PROBE_DIR, "sdk_history_summary.json"), "w") as fh:
            json.dump(summary, fh, indent=2)
        print(f"[sdk] wrote sdk_history.json + summary (markers={markers}, activities={scheduled_activities})")
        return 0
    finally:
        print("[sdk] terminating worker")
        try:
            os.killpg(os.getpgid(worker.pid), signal.SIGTERM)
        except Exception:
            worker.terminate()
        try:
            worker.wait(timeout=15)
        except Exception:
            try:
                os.killpg(os.getpgid(worker.pid), signal.SIGKILL)
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))

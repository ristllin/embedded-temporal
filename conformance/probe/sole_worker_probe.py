#!/usr/bin/env python3
"""sole_worker_probe.py — RISK PROBE for the mwf "true sole-worker" milestone.

Question: can a NON-mistralai-workflows worker (a raw temporalio poller, i.e. the
future ESP32 / any declarative replay engine) act as the *WORKFLOW* worker on real
Mistral — polling PollWorkflowTaskQueue, replaying a declarative workflow, and
responding with commands — WITHOUT the Python SDK and WITHOUT emitting the SDK's
`__internal__` lifecycle activities (register_execution, emit_workflow_started, ...)?

This script is the decisive test. It:
  1. whoami -> namespace + scheduler_url
  2. REST-registers a minimal workflow on a custom queue (NO Python worker)
  3. checks worker-active WITHOUT any heartbeat   (answers Probe 2)
  4. REST-triggers a run
  5. becomes the workflow worker with a RAW temporalio WorkflowService:
     PollWorkflowTaskQueue -> dump full history + task metadata ->
     RespondWorkflowTaskCompleted with a single hand-built
     CompleteWorkflowExecution command and ZERO __internal__ activity commands
  6. polls GET /executions/{id} -> is it COMPLETED with our result?

If step 6 reaches COMPLETED, pure declarative replay of the user workflow is
sufficient and the internal activities are NOT required by the data plane.

Variants:
  --with-activity : register a workflow whose single activity runs on the SAME
                    raw poller (device-realistic: schedule activity -> poll the
                    activity task queue -> complete activity -> complete workflow).

Everything is captured to probe/. The API key is never printed.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import time

import httpx
from google.protobuf.json_format import MessageToDict

import temporalio.api.command.v1 as cmd
import temporalio.api.common.v1 as common
import temporalio.api.enums.v1 as enums
import temporalio.api.taskqueue.v1 as taskqueue
import temporalio.api.workflowservice.v1 as wsv1
from temporalio.client import Client

try:
    from ._common import load_key, rest_client, whoami
except ImportError:  # run as a plain script from the probe/ directory
    from _common import load_key, rest_client, whoami

PROBE_DIR = os.path.dirname(os.path.abspath(__file__))
TERMINAL = {"COMPLETED", "FAILED", "TERMINATED", "TIMED_OUT", "CANCELED"}


def _dump(name: str, obj) -> str:
    path = os.path.join(PROBE_DIR, name)
    with open(path, "w") as fh:
        json.dump(obj, fh, indent=2, default=str)
    print(f"[probe]   wrote {name}")
    return path


def minimal_definition(name: str, task_queue: str) -> dict:
    """A minimal WorkflowSpecWithTaskQueue (shape lifted from a captured live register)."""
    return {
        "input_schema": {
            "additionalProperties": False,
            "properties": {"name": {"title": "Name", "type": "string"}},
            "required": ["name"],
            "title": "run_Input",
            "type": "object",
        },
        "name": name,
        "task_queue": task_queue,
        "output_schema": {
            "properties": {"result": {"title": "Result", "type": "string"}},
            "required": ["result"],
            "title": "run_Output",
            "type": "object",
        },
        "signals": [],
        "queries": [],
        "updates": [],
        "enforce_determinism": True,
        "on_behalf_of": False,
        "execution_timeout": "PT1H",
        "plugin_metadata": None,
        "display_name": None,
        "description": None,
        "is_technical": False,
        "schedules": [],
    }


def register(c: httpx.Client, name: str, task_queue: str, worker_name: str) -> dict:
    body = {
        "definitions": [minimal_definition(name, task_queue)],
        "deployment_name": task_queue,
        "worker_name": worker_name,
        "deployment_location": {"location_type": "local"},
    }
    r = c.post("/v1/workflows/register", json=body)
    print(f"[probe] POST /v1/workflows/register -> {r.status_code}")
    if r.status_code >= 400:
        print(f"[probe]   error body: {r.text[:1000]}")
        r.raise_for_status()
    return {"request": body, "status": r.status_code, "response": r.json()}


def check_active(c: httpx.Client, name: str) -> dict:
    r = c.get(f"/v1/workflows/{name}")
    data = r.json() if r.status_code == 200 else {"_status": r.status_code, "_body": r.text[:300]}
    wf = data.get("workflow", data) if isinstance(data, dict) else {}
    print(f"[probe] GET /v1/workflows/{name} -> {r.status_code} active={wf.get('active')}")
    return data


def trigger(c: httpx.Client, name: str, task_queue: str, input_obj: dict) -> dict:
    body = {"input": input_obj, "task_queue": task_queue}
    r = c.post(f"/v1/workflows/{name}/execute", json=body)
    print(f"[probe] POST /v1/workflows/{name}/execute -> {r.status_code}")
    if r.status_code >= 400:
        print(f"[probe]   error body: {r.text[:1000]}")
        r.raise_for_status()
    return r.json()


def json_plain_payload(value) -> common.Payload:
    return common.Payload(
        metadata={"encoding": b"json/plain"},
        data=json.dumps(value).encode("utf-8"),
    )


async def poll_one_workflow_task(svc, namespace: str, task_queue: str, identity: str, label: str):
    print(f"[probe] PollWorkflowTaskQueue queue={task_queue} (long-poll ~45s) ...")
    req = wsv1.PollWorkflowTaskQueueRequest(
        namespace=namespace,
        task_queue=taskqueue.TaskQueue(name=task_queue, kind=enums.TaskQueueKind.TASK_QUEUE_KIND_NORMAL),
        identity=identity,
    )
    resp = await svc.poll_workflow_task_queue(req)
    if not resp.task_token:
        print("[probe]   empty poll (no task)")
        return None
    d = MessageToDict(resp, preserving_proto_field_name=True)
    print(
        f"[probe]   GOT workflow task: type={d.get('workflow_type', {}).get('name')} "
        f"started_event_id={d.get('started_event_id')} "
        f"history_events={len(d.get('history', {}).get('events', []))} "
        f"task_token_bytes={len(resp.task_token)}"
    )
    _dump(f"workflow_task_{label}.json", d)
    return resp


async def poll_one_activity_task(svc, namespace: str, task_queue: str, identity: str, label: str):
    print(f"[probe] PollActivityTaskQueue queue={task_queue} (long-poll ~45s) ...")
    req = wsv1.PollActivityTaskQueueRequest(
        namespace=namespace,
        task_queue=taskqueue.TaskQueue(name=task_queue, kind=enums.TaskQueueKind.TASK_QUEUE_KIND_NORMAL),
        identity=identity,
    )
    resp = await svc.poll_activity_task_queue(req)
    if not resp.task_token:
        print("[probe]   empty activity poll")
        return None
    d = MessageToDict(resp, preserving_proto_field_name=True)
    print(f"[probe]   GOT activity task: type={d.get('activity_type', {}).get('name')}")
    _dump(f"activity_task_{label}.json", d)
    return resp


async def run(args) -> int:
    key = load_key()
    c = rest_client(key)

    who = whoami(c)
    namespace = who["namespace"]
    scheduler_url = who.get("scheduler_url") or who.get("schedulerUrl")
    who_redacted = {k: v for k, v in who.items() if "token" not in k.lower() and "key" not in k.lower()}
    print(f"[probe] whoami: namespace={namespace} scheduler_url={scheduler_url} tls={who.get('tls')}")

    name = args.workflow
    task_queue = args.task_queue
    worker_name = "mwf-sole-probe-worker"
    evidence: dict = {
        "probe": "sole_worker",
        "variant": "with_activity" if args.with_activity else "no_activity",
        "workflow": name,
        "task_queue": task_queue,
        "whoami": who_redacted,
        "captured_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }

    # 1. REST register (no Python worker, no heartbeat)
    reg = register(c, name, task_queue, worker_name)
    evidence["register"] = reg

    # 2. Probe 2: is it active WITHOUT any heartbeat? Snapshot immediately.
    evidence["active_after_register_no_heartbeat"] = check_active(c, name)

    # 3. Connect the RAW workflow worker (no mistralai-workflows, no interceptors)
    target = scheduler_url or "wf-scheduler.mistral.ai:443"
    print(f"[probe] connecting RAW temporalio WorkflowService to {target} ns={namespace} ...")
    client = await Client.connect(target, namespace=namespace, api_key=key, tls=True)
    svc = client.workflow_service

    # 4. REST trigger
    exec_resp = trigger(c, name, task_queue, {"name": args.input_name})
    evidence["trigger_response"] = exec_resp
    execution_id = exec_resp.get("execution_id")
    print(f"[probe] execution_id={execution_id} status={exec_resp.get('status')}")

    # 5. Become the workflow worker: poll the FIRST workflow task, dump history.
    wf_task = await poll_one_workflow_task(svc, namespace, task_queue, worker_name, "1")
    if wf_task is None:
        evidence["result"] = "NO_WORKFLOW_TASK_DISPATCHED"
        _dump(f"evidence_{evidence['variant']}.json", evidence)
        return 2

    responded_commands: list = []

    if not args.with_activity:
        # Minimal declarative replay: complete the workflow immediately with a
        # single command, ZERO __internal__ activity commands.
        result_val = {"result": f"Hello {args.input_name} from RAW sole-worker (no SDK)"}
        complete_cmd = cmd.Command(
            command_type=enums.CommandType.COMMAND_TYPE_COMPLETE_WORKFLOW_EXECUTION,
            complete_workflow_execution_command_attributes=cmd.CompleteWorkflowExecutionCommandAttributes(
                result=common.Payloads(payloads=[json_plain_payload(result_val)])
            ),
        )
        responded_commands = ["COMPLETE_WORKFLOW_EXECUTION"]
        print("[probe] RespondWorkflowTaskCompleted: [COMPLETE_WORKFLOW_EXECUTION] (0 internal activities)")
        await svc.respond_workflow_task_completed(
            wsv1.RespondWorkflowTaskCompletedRequest(
                task_token=wf_task.task_token, commands=[complete_cmd], identity=worker_name
            )
        )
    else:
        # Device-realistic: schedule ONE activity (device.echo) on the same queue,
        # then poll+complete it, then complete the workflow. Still ZERO __internal__.
        activity_id = "1"
        act_queue = task_queue  # same queue; raw poller does both
        schedule_cmd = cmd.Command(
            command_type=enums.CommandType.COMMAND_TYPE_SCHEDULE_ACTIVITY_TASK,
            schedule_activity_task_command_attributes=cmd.ScheduleActivityTaskCommandAttributes(
                activity_id=activity_id,
                activity_type=common.ActivityType(name="device.echo"),
                task_queue=taskqueue.TaskQueue(name=act_queue),
                input=common.Payloads(payloads=[json_plain_payload(args.input_name)]),
                schedule_to_close_timeout={"seconds": 120},
                start_to_close_timeout={"seconds": 60},
            ),
        )
        responded_commands = ["SCHEDULE_ACTIVITY_TASK"]
        print("[probe] RespondWorkflowTaskCompleted #1: [SCHEDULE_ACTIVITY_TASK] (0 internal activities)")
        await svc.respond_workflow_task_completed(
            wsv1.RespondWorkflowTaskCompletedRequest(
                task_token=wf_task.task_token, commands=[schedule_cmd], identity=worker_name
            )
        )
        # Poll + complete the activity.
        act_task = await poll_one_activity_task(svc, namespace, act_queue, worker_name, "1")
        if act_task is None:
            evidence["result"] = "NO_ACTIVITY_TASK_DISPATCHED"
            _dump(f"evidence_{evidence['variant']}.json", evidence)
            return 3
        act_result = {"device": "mwf-esp32s3", "echo": args.input_name, "via": "raw-sole-worker"}
        await svc.respond_activity_task_completed(
            wsv1.RespondActivityTaskCompletedRequest(
                task_token=act_task.task_token,
                result=common.Payloads(payloads=[json_plain_payload(act_result)]),
                identity=worker_name,
            )
        )
        print("[probe]   activity completed; polling next workflow task ...")
        wf_task2 = await poll_one_workflow_task(svc, namespace, task_queue, worker_name, "2")
        if wf_task2 is None:
            evidence["result"] = "NO_SECOND_WORKFLOW_TASK"
            _dump(f"evidence_{evidence['variant']}.json", evidence)
            return 4
        complete_cmd = cmd.Command(
            command_type=enums.CommandType.COMMAND_TYPE_COMPLETE_WORKFLOW_EXECUTION,
            complete_workflow_execution_command_attributes=cmd.CompleteWorkflowExecutionCommandAttributes(
                result=common.Payloads(payloads=[json_plain_payload(act_result)])
            ),
        )
        responded_commands.append("COMPLETE_WORKFLOW_EXECUTION")
        print("[probe] RespondWorkflowTaskCompleted #2: [COMPLETE_WORKFLOW_EXECUTION] (0 internal activities)")
        await svc.respond_workflow_task_completed(
            wsv1.RespondWorkflowTaskCompletedRequest(
                task_token=wf_task2.task_token, commands=[complete_cmd], identity=worker_name
            )
        )

    evidence["responded_commands"] = responded_commands
    evidence["internal_activity_commands_emitted"] = 0

    # 6. Did Mistral mark it COMPLETED?
    status_data: dict = {}
    deadline = time.time() + args.timeout_s
    while time.time() < deadline:
        time.sleep(2)
        sr = c.get(f"/v1/workflows/executions/{execution_id}")
        if sr.status_code != 200:
            print(f"[probe] GET /executions/{execution_id} -> {sr.status_code}: {sr.text[:200]}")
            continue
        status_data = sr.json()
        status = status_data.get("status")
        print(f"[probe] execution status={status}")
        if status in TERMINAL:
            break
    evidence["final_execution_status"] = status_data

    # Also grab the raw Temporal history end-state for the record.
    try:
        hist = await svc.get_workflow_execution_history(
            wsv1.GetWorkflowExecutionHistoryRequest(
                namespace=namespace,
                execution=common.WorkflowExecution(workflow_id=execution_id),
            )
        )
        hd = MessageToDict(hist, preserving_proto_field_name=True)
        _dump(f"final_history_{evidence['variant']}.json", hd)
        evidence["final_history_event_types"] = [
            e.get("event_type") for e in hd.get("history", {}).get("events", [])
        ]
    except Exception as exc:
        evidence["final_history_error"] = str(exc)

    verdict = status_data.get("status") == "COMPLETED"
    evidence["VERDICT_declarative_replay_sufficient"] = verdict
    _dump(f"evidence_{evidence['variant']}.json", evidence)

    print("\n[probe] ===== VERDICT =====")
    print(f"[probe] variant={evidence['variant']} internal_activity_commands=0")
    print(f"[probe] responded_commands={responded_commands}")
    print(f"[probe] final status={status_data.get('status')} result={json.dumps(status_data.get('result'))[:300]}")
    print(f"[probe] DECLARATIVE REPLAY SUFFICIENT: {verdict}")
    return 0 if verdict else 5


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--workflow", default="mwf_sole_probe")
    ap.add_argument("--task-queue", default="mwf-sole-probe")
    ap.add_argument("--input-name", default="nimbus")
    ap.add_argument("--with-activity", action="store_true")
    ap.add_argument("--timeout-s", type=int, default=90)
    args = ap.parse_args()
    return asyncio.run(run(args))


if __name__ == "__main__":
    raise SystemExit(main())

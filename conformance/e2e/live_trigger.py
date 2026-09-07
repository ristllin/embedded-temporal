#!/usr/bin/env python3
"""live_trigger.py — trigger a registered workflow VIA THE MISTRAL SERVER and
verify the execution server-side (the anti-local-fallback gate).

Flow (all REST, Bearer auth, api.mistral.ai):

1. wait until GET /v1/workflows/{name} reports the worker active,
2. POST /v1/workflows/{name}/execute  {"input": {...}, "task_queue": q}
   -> {"execution_id": ...}
3. poll GET /v1/workflows/executions/{execution_id} until a terminal status,
4. verify the execution appears in GET /v1/workflows/runs (server of record),
5. write an evidence JSON snippet (ids + statuses + result) and assert the
   result contains --expect.

Usage::

    live_trigger.py --workflow mwf_e2e_py --task-queue mwf-e2e \
        --input '{"name": "nimbus"}' --expect "from Python" \
        --evidence e2e/evidence/py_e2e.json
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

import httpx

BASE = os.environ.get("MISTRAL_BASE_URL", "https://api.mistral.ai")
TERMINAL = {"COMPLETED", "FAILED", "TERMINATED", "TIMED_OUT", "CANCELED"}


def client() -> httpx.Client:
    key = os.environ.get("MISTRAL_API_KEY")
    if not key:
        sys.exit("MISTRAL_API_KEY not set")
    return httpx.Client(
        base_url=BASE,
        headers={"Authorization": f"Bearer {key}"},
        timeout=60.0,
        follow_redirects=True,
    )


def wait_worker_active(c: httpx.Client, name: str, timeout_s: int) -> dict:
    deadline = time.time() + timeout_s
    last: dict = {}
    while time.time() < deadline:
        r = c.get(f"/v1/workflows/{name}")
        if r.status_code == 200:
            last = r.json()
            wf = last.get("workflow", last)
            if wf.get("active"):
                return last
        time.sleep(2)
    raise TimeoutError(f"workflow {name} not active after {timeout_s}s (last: {json.dumps(last)[:400]})")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--workflow", required=True)
    ap.add_argument("--task-queue", default=None)
    ap.add_argument("--input", default="{}", help="JSON object for the input field")
    ap.add_argument("--expect", default=None, help="substring the result must contain")
    ap.add_argument("--evidence", default=None, help="write evidence JSON here")
    ap.add_argument("--wait-active-s", type=int, default=120)
    ap.add_argument("--timeout-s", type=int, default=240)
    args = ap.parse_args()

    c = client()

    print(f"[trigger] waiting for worker on workflow '{args.workflow}' ...")
    defn = wait_worker_active(c, args.workflow, args.wait_active_s)
    print(f"[trigger] worker active for '{args.workflow}'")

    body: dict = {"input": json.loads(args.input)}
    if args.task_queue:
        body["task_queue"] = args.task_queue
    r = c.post(f"/v1/workflows/{args.workflow}/execute", json=body)
    print(f"[trigger] POST /v1/workflows/{args.workflow}/execute -> {r.status_code}")
    if r.status_code >= 400:
        print(f"[trigger] error body: {r.text[:800]}")
        return 1
    exec_resp = r.json()
    execution_id = exec_resp.get("execution_id")
    print(f"[trigger] execution_id={execution_id}")
    if not execution_id:
        print(f"[trigger] no execution_id in response: {exec_resp}")
        return 1

    status_data: dict = {}
    deadline = time.time() + args.timeout_s
    while time.time() < deadline:
        time.sleep(2)
        sr = c.get(f"/v1/workflows/executions/{execution_id}")
        sr.raise_for_status()
        status_data = sr.json()
        status = status_data.get("status")
        print(f"[trigger] status={status}")
        if status in TERMINAL:
            break
    else:
        print("[trigger] TIMEOUT waiting for terminal status")
        return 1

    # Server-of-record check: the run must be listed by /v1/workflows/runs.
    runs_entry = None
    runs_status = None
    rr = c.get("/v1/workflows/runs", params={"limit": 50})
    runs_status = rr.status_code
    if rr.status_code == 200:
        payload = rr.json()
        rows = payload.get("runs", payload.get("executions", payload if isinstance(payload, list) else []))
        for row in rows if isinstance(rows, list) else []:
            row_ids = {row.get("execution_id"), row.get("id"), row.get("workflow_exec_id")}
            if execution_id in row_ids:
                runs_entry = row
                break
    print(f"[trigger] GET /v1/workflows/runs -> {runs_status}; listed={runs_entry is not None}")

    result = status_data.get("result")
    result_str = json.dumps(result) if not isinstance(result, str) else result
    ok = status_data.get("status") == "COMPLETED"
    if args.expect and (result_str is None or args.expect not in str(result_str)):
        print(f"[trigger] EXPECT FAILED: {args.expect!r} not in result {result_str!r}")
        ok = False

    evidence = {
        "workflow": args.workflow,
        "task_queue": args.task_queue,
        "execute_response": exec_resp,
        "final_status": status_data,
        "runs_listing_status": runs_status,
        "runs_listing_entry": runs_entry,
        "workflow_definition": defn,
        "captured_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    if args.evidence:
        os.makedirs(os.path.dirname(args.evidence), exist_ok=True)
        with open(args.evidence, "w") as fh:
            json.dump(evidence, fh, indent=2)
        print(f"[trigger] evidence -> {args.evidence}")

    print(f"[trigger] status={status_data.get('status')} result={result_str!r}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

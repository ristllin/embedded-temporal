#!/usr/bin/env python3
"""live_cleanup.py — leave the Mistral account tidy after the live E2Es.

* terminates any non-terminal executions of the E2E workflows
  (POST /v1/workflows/executions/{id}/terminate),
* with --archive, archives the E2E workflow registrations
  (PUT /v1/workflows/{name}/archive) so they stop cluttering the listing.

There is NO deployment/worker delete endpoint on the public surface (the
worker registration simply expires when heartbeats stop); archiving the
workflow is the strongest available cleanup.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

import httpx

BASE = os.environ.get("MISTRAL_BASE_URL", "https://api.mistral.ai")
TERMINAL = {"COMPLETED", "FAILED", "TERMINATED", "TIMED_OUT", "CANCELED"}
E2E_WORKFLOWS = ["mwf_e2e_py", "mwf_e2e_cpp_live"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--archive", action="store_true", help="also archive the E2E workflows")
    args = ap.parse_args()

    key = os.environ.get("MISTRAL_API_KEY")
    if not key:
        sys.exit("MISTRAL_API_KEY not set")
    c = httpx.Client(base_url=BASE, headers={"Authorization": f"Bearer {key}"}, timeout=60.0)

    r = c.get("/v1/workflows/runs", params={"limit": 100})
    terminated = []
    if r.status_code == 200:
        payload = r.json()
        rows = payload.get("runs", payload.get("executions", []))
        for row in rows if isinstance(rows, list) else []:
            name = row.get("workflow_name") or row.get("name") or ""
            status = row.get("status")
            exec_id = row.get("execution_id") or row.get("id") or row.get("workflow_exec_id")
            if name in E2E_WORKFLOWS and status not in TERMINAL and exec_id:
                tr = c.post(f"/v1/workflows/executions/{exec_id}/terminate")
                print(f"[cleanup] terminate {exec_id} ({name}, was {status}) -> {tr.status_code}")
                terminated.append(exec_id)
    else:
        print(f"[cleanup] GET /v1/workflows/runs -> {r.status_code}: {r.text[:300]}")

    if not terminated:
        print("[cleanup] no lingering executions")

    if args.archive:
        for name in E2E_WORKFLOWS:
            ar = c.put(f"/v1/workflows/{name}/archive")
            print(f"[cleanup] archive {name} -> {ar.status_code} {ar.text[:200]}")

    return 0


if __name__ == "__main__":
    sys.exit(main())

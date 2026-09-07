#!/usr/bin/env python3
"""cleanup.py — terminate any lingering probe executions and archive the probe
workflows registered by this probe suite. Idempotent. Key never printed."""
from __future__ import annotations

import sys

try:
    from ._common import load_key, rest_client
except ImportError:  # run as a plain script from the probe/ directory
    from _common import load_key, rest_client

# Workflows this probe registered (mwf_e2e_* are pre-existing E2E ones we
# re-triggered; archive them to restore the documented cleaned-up state).
PROBE_WORKFLOWS = ["mwf_sole_probe", "mwf_sole_probe_act", "mwf_sole_sdk"]
E2E_WORKFLOWS = ["mwf_e2e_py", "mwf_e2e_cpp_live"]
TERMINAL = {"COMPLETED", "FAILED", "TERMINATED", "TIMED_OUT", "CANCELED"}


def main() -> int:
    key = load_key()
    c = rest_client(key)

    # 1. Terminate any non-terminal runs of our probe workflows.
    r = c.get("/v1/workflows/runs", params={"limit": 100})
    if r.status_code == 200:
        rows = r.json().get("runs", [])
        for row in rows:
            wf = row.get("workflow_name")
            eid = row.get("execution_id") or row.get("id")
            st = row.get("status")
            if wf in PROBE_WORKFLOWS + E2E_WORKFLOWS and st not in TERMINAL and eid:
                tr = c.post(f"/v1/workflows/executions/{eid}/terminate")
                print(f"[cleanup] terminate {wf} {eid} ({st}) -> {tr.status_code}")

    # 2. Archive the workflows.
    for name in PROBE_WORKFLOWS + E2E_WORKFLOWS:
        ar = c.put(f"/v1/workflows/{name}/archive")
        print(f"[cleanup] archive {name} -> {ar.status_code} {ar.text[:120] if ar.status_code>=400 else ''}")

    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Shared helpers for the sole-worker probes.

Loads MISTRAL_API_KEY from a local .env file (never printed), does whoami,
and exposes a REST client + a raw temporalio WorkflowService connector.
"""
from __future__ import annotations

import os
import pathlib
import re

import httpx

BASE = os.environ.get("MISTRAL_BASE_URL", "https://api.mistral.ai")


def load_key() -> str:
    key = os.environ.get("MISTRAL_API_KEY")
    if key:
        return key
    env = pathlib.Path(os.environ.get("MWF_ENV_FILE", ".env"))
    for line in env.read_text().splitlines():
        m = re.match(r"\s*(?:export\s+)?MISTRAL_API_KEY\s*=\s*(.+)\s*$", line)
        if m:
            val = m.group(1).strip().strip('"').strip("'")
            if val:
                return val
    raise SystemExit("MISTRAL_API_KEY not found in env or a local .env file")


def rest_client(key: str) -> httpx.Client:
    return httpx.Client(
        base_url=BASE,
        headers={"Authorization": f"Bearer {key}"},
        timeout=60.0,
        follow_redirects=True,
    )


def whoami(c: httpx.Client) -> dict:
    r = c.get("/v1/workflows/workers/whoami")
    r.raise_for_status()
    return r.json()

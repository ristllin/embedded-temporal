"""embedded-temporal-conformance — golden vectors + oracle tooling for the
embedded-temporal ESP32 Temporal / Mistral Workflows worker.

The integration currency of the project (CONTRACTS.md "Integration currency"):
cross-track integration flows through *recorded vectors*, not live handshakes.
This distribution ships:

  mwf_conformance.goldens   the recorded vectors — Temporal event histories
                            (replay-engine oracle) and Mistral payload
                            envelopes (codec oracle), captured from the real
                            pinned SDKs; see its README.md / NOTES.md.
  mwf_conformance.capture   the tooling that (re)captures the goldens from the
                            temporalio + mistralai-workflows SDKs.
  mwf_conformance.e2e       live end-to-end drivers: run a whole workflow
                            through a worker against a Temporal dev server or
                            the live Mistral frontend.
  mwf_conformance.probe     raw wire probes for the Mistral scheduler (whoami,
                            history capture) plus captured reference responses.

Golden-vector access (installed package): goldens_root(), list_goldens(),
load_golden(). In a repo checkout the same files live at conformance/goldens/.
"""
from __future__ import annotations

import json
from importlib.resources import files

__version__ = "0.1.1"


def goldens_root():
    """Traversable root of the packaged golden vectors."""
    return files("mwf_conformance.goldens")


def list_goldens() -> list[str]:
    """Relative paths of every packaged golden-vector JSON file."""
    root = goldens_root()
    out: list[str] = []
    for sub in ("payloads", "histories"):
        d = root / sub
        if d.is_dir():
            out.extend(sorted(f"{sub}/{f.name}" for f in d.iterdir()
                              if f.name.endswith(".json")))
    return out


def load_golden(relpath: str):
    """Parse one golden vector, e.g. load_golden("payloads/greet-in.json")."""
    return json.loads((goldens_root() / relpath).read_text(encoding="utf-8"))

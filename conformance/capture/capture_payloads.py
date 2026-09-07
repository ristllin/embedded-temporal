#!/usr/bin/env python3
"""capture_payloads.py — record custom Mistral Payloads → goldens.

Produces the **exact Temporal ``Payload``** (metadata map + inner data
bytes) that a Mistral Workflows worker puts on the wire for an activity input /
result, and dumps each to ``goldens/payloads/<name>-<in|out>.json``.

It drives the **real SDK code path** — no hand-rolled bytes — reproducing what
crosses the worker<->server boundary exactly:

    value
      -> pydantic_core.to_json(value)                       (raw JSON bytes)
      -> PayloadWithContext(payload=..., context=..., empty=...)
      -> WithContextJSONPayloadConverter.to_payload(...)    (encoding=json/wf_v1)
      -> MistralWorkflowsPayloadCodec(None,None,None).encode([...])   <-- WIRE

with encryption / offloading / compression all disabled (v1 default), so
``encode_payload_content`` is identity and the inner ``data`` is raw JSON.

This is the concrete realization of ``IPayloadCodec`` and feeds the C++ codec,
which must decode/encode these byte-for-byte per
``../contracts/spec/codec-wire-format.md``. Every capture is round-tripped
back through the SDK codec's ``decode`` and checked against the spec's metadata
table; discrepancies are written to ``goldens/NOTES.md`` (see ``run_all.sh``).

Fully offline — no network, no cloud, deterministic. The namespace/execution_id
below are a representative placeholder namespace (overridable via env); nothing
secret rides in a Payload.
"""

from __future__ import annotations

import asyncio
import base64
import json
import logging
import os
import pathlib

# Quiet the Mistral SDK's noisy import-time structlog config dump.
logging.disable(logging.INFO)
os.environ.setdefault("MISTRAL_WORKFLOWS_OTEL_ENABLED", "false")

from pydantic_core import to_json
from temporalio.api.common.v1 import Payload

from mistralai.workflows.core.temporal.payload_codec import MistralWorkflowsPayloadCodec
from mistralai.workflows.core.temporal.payload_converter import (
    WithContextJSONPayloadConverter,
)
from mistralai.workflows.core.encoding import build_info_from_payload_metadata
from mistralai.workflows.models import PayloadWithContext, WorkflowContext

REPO = pathlib.Path(__file__).resolve().parents[1]
OUT_DIR = REPO / "goldens" / "payloads"

# Representative workflow context echoed into every payload. The namespace is a
# representative placeholder ``<customer>:<workspace>`` value (not a secret);
# execution_id is a representative run id. Decode REQUIRES execution_id
# non-empty (the SDK codec raises otherwise).
NAMESPACE = os.environ.get(
    "MWF_NAMESPACE",
    "00000000-0000-4000-8000-000000000000:11111111-1111-4111-8111-111111111111",
)
EXECUTION_ID = os.environ.get("MWF_EXECUTION_ID", "linear-goldencapture-0001")

# The spec's metadata table (codec-wire-format.md). Keys that must ALWAYS be
# present on a v1 custom payload.
SPEC_ALWAYS_KEYS = {"encoding", "namespace", "execution_id", "encoding_options"}
SPEC_ENCODING = "json/wf_v1"

_converter = WithContextJSONPayloadConverter()
# All three configs None => encryption/offloading/compression OFF => identity.
_codec = MistralWorkflowsPayloadCodec(None, None, None)


def _ctx() -> WorkflowContext:
    return WorkflowContext(namespace=NAMESPACE, execution_id=EXECUTION_ID)


async def _wire_payload(value, empty: bool) -> Payload:
    """Run the real SDK outbound pipeline and return the on-the-wire Payload."""
    ctx = _ctx()
    inner = b"null" if empty else to_json(value)
    pwc = PayloadWithContext(payload=inner, context=ctx, empty=empty)
    intermediate = _converter.to_payload(pwc)  # encoding=json/wf_v1, data=PWC json
    wire = (await _codec.encode([intermediate]))[0]
    return wire


def _b64(b: bytes) -> str:
    return base64.b64encode(b).decode()


def _decode_meta(md: dict[str, bytes]) -> dict[str, str]:
    out = {}
    for k, v in md.items():
        try:
            out[k] = v.decode()
        except UnicodeDecodeError:
            out[k] = "b64:" + _b64(v)
    return out


async def _capture(name: str, direction: str, value, description: str, empty=False) -> dict:
    wire = await _wire_payload(value, empty)
    md = dict(wire.metadata)  # str -> bytes
    meta_dec = _decode_meta(md)

    # --- spec conformance checks (per codec-wire-format.md) ---
    findings = []
    if meta_dec.get("encoding") != SPEC_ENCODING:
        findings.append(f"encoding={meta_dec.get('encoding')!r} != {SPEC_ENCODING!r}")
    missing = SPEC_ALWAYS_KEYS - set(md.keys())
    if missing:
        findings.append(f"missing always-keys: {sorted(missing)}")
    if meta_dec.get("encoding_options", None) != "":
        findings.append(f"encoding_options={meta_dec.get('encoding_options')!r} (v1 expects empty string)")
    if meta_dec.get("namespace") != NAMESPACE:
        findings.append("namespace not echoed")
    if meta_dec.get("execution_id") != EXECUTION_ID:
        findings.append("execution_id not echoed")
    if empty and "empty_payload" not in md:
        findings.append("empty=True but empty_payload metadata key absent")

    # --- round-trip via the SDK codec's own decoder ---
    decoded = (await _codec.decode([Payload(metadata=md, data=wire.data)]))[0]
    pwc_back = PayloadWithContext.model_validate_json(decoded.data)
    ctx_back, opts_back, empty_back = build_info_from_payload_metadata(md)
    if ctx_back.execution_id != EXECUTION_ID:
        findings.append("decode: execution_id lost")
    if opts_back != []:
        findings.append(f"decode: encoding_options non-empty {opts_back}")
    if empty_back != empty:
        findings.append(f"decode: empty flag mismatch {empty_back} != {empty}")

    # inner data should be raw JSON of the value (v1 identity passthrough)
    data_text = wire.data.decode("utf-8", errors="replace")
    try:
        data_json = json.loads(data_text)
        data_is_json = True
    except ValueError:
        data_json = None
        data_is_json = False
    if not data_is_json:
        findings.append(f"inner data not valid JSON: {data_text!r}")

    golden = {
        "description": description,
        "direction": direction,  # "in" (activity input) | "out" (activity result)
        "encoding": meta_dec.get("encoding"),
        "metadata": {k: _b64(v) for k, v in md.items()},
        "metadata_decoded": meta_dec,
        "data_b64": _b64(wire.data),
        "data_utf8": data_text,
        "data_json": data_json,
        "empty": empty,
        "context_echo": {
            "namespace": NAMESPACE,
            "execution_id": EXECUTION_ID,
        },
        "spec_conformance": "OK" if not findings else findings,
    }

    fname = f"{name}-{direction}.json"
    (OUT_DIR / fname).write_text(json.dumps(golden, indent=2, sort_keys=False) + "\n")
    status = "OK" if not findings else f"DISCREPANCY: {findings}"
    print(f"  [{fname}] keys={sorted(md.keys())}")
    print(f"    data={data_text!r}  empty={empty}  spec={status}")
    return {"file": fname, "findings": findings, "metadata_keys": sorted(md.keys())}


async def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    print(f"namespace={NAMESPACE}")
    print(f"execution_id={EXECUTION_ID}\n")

    results = []
    # Activity INPUT: the argument crossing into an activity (greet("world")).
    results.append(await _capture(
        "greet", "in", "world",
        "Activity input: scalar JSON string argument to greet(name).",
    ))
    # Activity RESULT: scalar string result of greet.
    results.append(await _capture(
        "greet", "out", "hello, world",
        "Activity result: scalar JSON string returned by greet.",
    ))
    # Activity RESULT: structured (object) result — exercises nested JSON in data.
    results.append(await _capture(
        "struct", "out", {"n": 8, "kind": "even", "halved": 4},
        "Activity result: structured JSON object result (nested fields).",
    ))
    # Activity RESULT: empty payload — exercises the empty_payload metadata key.
    results.append(await _capture(
        "empty", "out", None,
        "Activity result: logically-empty payload (empty_payload flag set).",
        empty=True,
    ))

    n_disc = sum(1 for r in results if r["findings"])
    print(f"\ncaptured {len(results)} payloads -> {OUT_DIR.relative_to(REPO)}/")
    print(f"spec discrepancies: {n_disc}")
    # Emit a machine-readable summary for run_all.sh / NOTES.md generation.
    (OUT_DIR / "_summary.json").write_text(
        json.dumps({"payloads": results, "namespace": NAMESPACE}, indent=2) + "\n"
    )


if __name__ == "__main__":
    asyncio.run(main())

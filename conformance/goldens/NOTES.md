# Golden capture notes — spec conformance + discrepancies

Captured 2026-07-18 with the real SDKs: `mistralai-workflows` 3.9.0,
`temporalio` 1.27.2, Temporal CLI 1.8.0 (Server 1.31.2). Every payload golden
was round-tripped back through the SDK's own `MistralWorkflowsPayloadCodec.decode`
and checked against [`../../contracts/spec/codec-wire-format.md`](../../contracts/spec/codec-wire-format.md).

## Payload wire format — CONFIRMED

The spec's metadata table and "inner data = raw JSON" claim are **accurate**.
For a v1 custom payload (encryption/offloading/compression all `None`):

- `encoding` = `json/wf_v1` — confirmed (bytes `6a736f6e2f77665f7631`).
- `namespace`, `execution_id` — echoed verbatim from `WorkflowContext`.
- `encoding_options` — present and the **empty string** (`b""`), exactly as spec.
- Inner `data` = **raw JSON of the value**, identity passthrough. A scalar string
  result `"hello, world"` is on the wire as the 14 bytes `"hello, world"`
  (JSON-quoted); an int `8` as `8`; an object as its compact JSON. Confirmed the
  double-JSON path (`to_json(value)` -> stored in `PayloadWithContext.payload`
  as a JSON string -> codec re-parses -> emits the inner JSON) collapses to
  **exactly the value's raw JSON** on the wire. No wrapper survives to the wire.
- Optional keys (`root_workflow_exec_id`, `parent_workflow_exec_id`,
  `__internal_execution_token`, `__internal_extensions`, `__internal_on_behalf_of`)
  are **absent** when the context fields are unset — matches the "if set" column.
  (Not exercised as present in these goldens; the minimal-context case is the one
  a direct C++ worker must produce for a plain activity.)

## Discrepancies / refinements (READ before implementing the codec)

1. **`empty_payload` value is a single NUL byte `0x00`, not a truthy string.**
   The spec row says "truthy bytes". The SDK writes `bytes(True)` which in Python
   is `bytes(1)` = **`b"\x00"`** (a one-byte buffer of zero), and decode reads it
   as `empty = bool(metadata.get("empty_payload"))` — i.e. **presence of the key
   with any non-zero-length bytes = empty**. So:
   - A C++ encoder should emit the key with value `\x00` (1 byte) to match the SDK
     byte-for-byte, OR any non-empty byte string (decode only checks non-emptiness).
   - A C++ decoder MUST treat *the key being present* as `empty=true`. It must NOT
     compare the value to `"true"`/`"1"` — the value is a NUL byte and such a
     comparison would wrongly yield `empty=false`. See `empty-out.json`
     (`metadata.empty_payload` base64 `AA==` = `0x00`).

2. **Inner data for an empty payload is `null` (4 bytes), not absent.** The SDK
   sets `PayloadWithContext(payload=b"null", empty=True)`, so on the wire `data` =
   `null` AND `empty_payload` is set. The `empty` flag is the authority; `data`
   is still valid JSON (`null`). Don't assume empty => zero-length `data`.

3. **Metadata map has no guaranteed key order.** Observed emission order was
   `encoding_options, namespace, encoding, execution_id` (dict/Temporal map
   ordering), not the spec table's order. Temporal `Payload.metadata` is a
   `map<string,bytes>` — **order is not semantic**; a codec must key by name.

4. **Histories carry `json/plain`, not `json/wf_v1`.** The event histories in
   `histories/` were produced by a *local* dev worker using Temporal's **default**
   data converter (activity in/out payloads have `metadata.encoding = json/plain`,
   base64 `anNvbi9wbGFpbg==`). This is **by design**: histories are for the
   replay engine (pure Temporal `HistoryEvent` shape + Command sequencing) and do
   not need the Mistral envelope. The `json/wf_v1` envelope is captured separately
   and authoritatively in `payloads/` via the real SDK codec. A future capture
   could run the local worker *with* `MistralWorkflowsPayloadConverter` +
   `MistralWorkflowsPayloadCodec` to embed `json/wf_v1` payloads inside a history,
   but that is not required for the replay-vs-oracle gate.

## Live backend — CONFIRMED (read-only)

`live_smoke.py` connected a plain `temporalio` client to
`wf-scheduler.mistral.ai:443` with **only** `api_key + namespace + tls=True`
(no mTLS) and listed executions — corroborating the transport-layer `ITransport`
assumption that the ESP32 needs Bearer-token auth + TLS but not client certs.
`whoami` namespace form is `<customer-uuid>:<workspace-uuid>`.

# Mistral Workflows payload codec — wire format (reverse-engineered)

**Source of truth:** `mistralai-workflows` 3.9.0 SDK, files
`core/temporal/payload_codec.py`, `core/temporal/payload_converter.py`,
`core/encoding/{constants,utils}.py`, `models/payload.py`. RE'd 2026-07-17.

This is the exact byte-level contract a C++ worker must reproduce to talk
directly to Mistral's Temporal frontend (`wf-scheduler.mistral.ai:443`). It is
the concrete realization of `IPayloadCodec`.

## The key simplification (v1 scope)

With **encryption, offloading, and compression all DISABLED** (our v1 default —
worker constructed with all three configs `None`), `encoding_options` is the
**empty list** and `encode_payload_content(data, ctx)` / `decode_payload_content`
are **identity**. Therefore the inner activity payload on the wire is **raw
JSON bytes** — the activity's argument or result serialized with plain JSON.
There is **no crypto/RE burden** in v1. The "undocumented envelope" is entirely
the Temporal `Payload.metadata` map below.

## Encoding constants

- `NEW_ENCODING_FORMAT = CUSTOM_ENCODING_FORMAT = "json/wf_v1"`
- `LEGACY_ENCODING_FORMAT = "json/abraxas_v1"` (decode-only; never emit)
- Temporal's own plain encoding `"json/plain"` is still used for non-custom
  payloads (accepted for backward compat).

## On-the-wire custom Payload (worker ↔ Temporal server)

A Temporal `Payload` is `{ metadata: map<string,bytes>, data: bytes }`. A
Mistral custom-encoded payload sets:

| metadata key | value (bytes) | when |
|---|---|---|
| `encoding` | `json/wf_v1` | always |
| `namespace` | `ctx.namespace` | always |
| `execution_id` | `ctx.execution_id` | always (decode REQUIRES it non-empty) |
| `encoding_options` | comma-joined option values — **empty string in v1** | always |
| `root_workflow_exec_id` | `ctx.root_workflow_exec_id` | if set |
| `parent_workflow_exec_id` | `ctx.parent_workflow_exec_id` | if set |
| `__internal_execution_token` | `ctx.execution_token` | if set |
| `__internal_extensions` | `json.dumps(ctx.extensions)` | if non-empty |
| `__internal_on_behalf_of` | `true` / `false` | if not None |
| `empty_payload` | truthy bytes | if the payload is logically empty |

`data` = inner payload bytes (raw JSON in v1).

(`INTERNAL_METADATA_PREFIX = "__internal_"`.)

## WorkflowContext fields (the C++ struct)

```
struct WorkflowContext {
  std::string namespace_;                 // required on wire
  std::string execution_id;               // required; decode fails if empty
  std::optional<std::string> root_workflow_exec_id;
  std::optional<std::string> parent_workflow_exec_id;
  std::optional<std::string> execution_token;
  std::map<std::string,json> extensions;  // default empty
  std::optional<bool> on_behalf_of;
  // (base also has retention_ttl — present in the base model; not written into
  //  temporal metadata by build_temporal_payload_metadata, so ignore for wire.)
};
```

## Worker semantics

- **Inbound (activity input)**: server sends a custom Payload → C++ reads
  `data` as JSON (the activity argument) and reconstructs `WorkflowContext`
  from metadata (`build_info_from_payload_metadata`). Reject if `execution_id`
  empty.
- **Outbound (activity result)**: C++ serializes the handler's JSON result into
  `data`, builds metadata via `build_temporal_payload_metadata(ctx, [], empty)`
  echoing the **same execution context** (namespace/execution_id/etc.) received
  on input. `encoding_options` = empty.

## Notes / deferred

- The intermediate `PayloadWithContext` JSON form (`{context, payload, empty}`)
  is an INTERNAL worker representation between codec and converter; the actual
  Temporal wire form is the metadata form above. A direct C++ worker only needs
  the wire form.
- Legacy `json/abraxas_v1`: decode-only, out of v1 scope (only if we hit old
  data — we won't for fresh executions).
- Encryption (`binary/encrypted`, AES-GCM), offloading (blob storage), and
  compression are all v2+; each maps to an `EncodedPayloadOptions` value in
  `encoding_options` and a transform in `PayloadEncoder`. Deferred.

## `IPayloadCodec` (contract)

```cpp
struct DecodedActivityInput { json argument; WorkflowContext context; bool empty; };
struct IPayloadCodec {
  virtual DecodedActivityInput decodeActivityInput(const temporal::Payload&) = 0;
  virtual temporal::Payload   encodeActivityResult(const json& result,
                                                   const WorkflowContext& echoCtx,
                                                   bool empty) = 0;
};
```
The v1 impl (`codec`) is ~100 lines: metadata map read/write + JSON
passthrough. Host-tested against golden Payloads captured by `conformance`.

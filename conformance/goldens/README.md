# goldens — recorded conformance vectors

The **integration currency** of the mwf program (CONTRACTS.md "Integration
currency"): cross-track integration flows through these recorded vectors, not
live handshakes. Each C++ track goes "green" when its host tests pass against
them. Regenerate everything with `make goldens` (or `./capture/run_all.sh`).

See [`NOTES.md`](NOTES.md) for the spec-conformance findings and the byte-level
discrepancies a codec implementer MUST read first.

## `histories/` — Temporal event histories → feeds the **replay engine**

Full `HistoryEvent` streams from real workflow executions on a local
`temporal server start-dev` frontend, serialized as canonical Temporal history
JSON (google.protobuf JSON, camelCase — the shape `tctl` / the Temporal UI
export, and that `temporalio.client.WorkflowHistory.from_json` round-trips).
Each was verified to reload through that parser at capture time.

| file | workflow shape (v1 `IWorkflowSpec` grammar) | events | terminal result |
|---|---|---|---|
| `linear.json` | `activity(greet) -> activity(shout) -> complete` (sequence) | 17 | `"HELLO, WORLD!"` |
| `conditional_even.json` | `activity(parity) -> conditional[true] -> activity(even_branch) -> complete` | 17 | `"8 is even; halved=4"` |
| `conditional_odd.json` | `activity(parity) -> conditional[false] -> activity(odd_branch) -> complete` | 17 | `"7 is odd; tripled+1=22"` |

Both branches of the conditional are captured so the replay engine can exercise
the true/false step lists. Event sequence per run (the replay-vs-oracle gate):
`WorkflowExecutionStarted` → (`WorkflowTaskScheduled/Started/Completed` →
`ActivityTaskScheduled/Started/Completed`)×2 → `WorkflowTaskScheduled/Started/
Completed` → `WorkflowExecutionCompleted`.

Consumer: the mwf-core `ReplayEngine` replays each history against the matching
`WorkflowSpec` and asserts its emitted `Command`s against this oracle.

## `payloads/` — custom Mistral WorkflowContext Payloads → feeds the **codec**

The exact Temporal `Payload` (metadata map + inner `data` bytes) a Mistral
Workflows worker puts on the wire, produced through the **real SDK codec path**
(`PayloadWithContext` → `WithContextJSONPayloadConverter` →
`MistralWorkflowsPayloadCodec(None,None,None).encode`). Each file has the wire
bytes (`metadata` as `{key: base64}`, `data_b64`) plus decoded views
(`metadata_decoded`, `data_utf8`, `data_json`) and an inline `spec_conformance`
verdict. Every one was decoded back through the SDK codec at capture time.

| file | direction | value | exercises |
|---|---|---|---|
| `greet-in.json`  | activity **input**  | `"world"` | minimal custom envelope, scalar arg |
| `greet-out.json` | activity **result** | `"hello, world"` | scalar string result |
| `struct-out.json`| activity **result** | `{"n":8,"kind":"even","halved":4}` | nested-object JSON in `data` |
| `empty-out.json` | activity **result** | (empty) | `empty_payload` metadata key (value `0x00`), `data="null"` |
| `_summary.json`  | — | — | machine-readable capture summary (metadata keys + findings per file) |

All four are `encoding=json/wf_v1`, `encoding_options=""` (empty), with
`namespace` + `execution_id` echoed. Consumer: the mwf-codec `PayloadCodecV1`
must decode/encode these byte-for-byte per
[`../../contracts/spec/codec-wire-format.md`](../../contracts/spec/codec-wire-format.md).

## Regenerating

```bash
make goldens        # local, deterministic, offline (boots a temporal dev server)
make goldens-live   # + read-only live smoke vs api.mistral.ai (needs MISTRAL_API_KEY)
```

The histories embed run-specific ids (workflow_id suffix, timestamps, event
UUIDs) so a regenerated file differs from a committed one in those fields; the
event *sequence*, attributes, and payloads are stable. The payload goldens are
fully deterministic (fixed namespace/execution_id, no timestamps).

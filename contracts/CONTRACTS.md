# mwf frozen contracts

The seams that let the layers (proto, core, codec, transport, firmware,
conformance) build in parallel without cross-blocking. **Frozen before
fan-out.** Every layer codes against these interfaces, not another layer's
implementation. Changes here are a coordinated event (bump `CONTRACTS_VERSION`,
update all layers together).

`CONTRACTS_VERSION = 0.1.1` · repos consume this via `symlink://../contracts`
(device/PlatformIO) or CMake `add_subdirectory`/include path (host).

Language: portable C++17, no Arduino, no exceptions across the seam (return
`Result<T>`). JSON type = a thin `mwf::json` alias (nlohmann/json on host and,
vendored, on device — codec/core touch only the alias, never the concrete lib).

---

## 1. `ITransport` — gRPC-over-HTTP/2 unary call (transport)

Long-poll-aware unary gRPC. One impl on desktop (grpc++/raw h2), one on ESP32
(nghttp2+mbedTLS-PSRAM+framing). Core/worker never sees HTTP/2 or protobuf-lib.

```cpp
struct GrpcResult {
  int      grpc_status;      // 0 = OK; 4 = DEADLINE_EXCEEDED (empty long-poll)
  std::string message;       // grpc-message trailer, if any
  Bytes    response;         // response message bytes (protobuf), empty on non-OK
};
struct ITransport {
  // method e.g. "/temporal.api.workflowservice.v1.WorkflowService/PollActivityTaskQueue"
  // metadata carries authorization: Bearer <key>, temporal-namespace, etc.
  virtual GrpcResult call(std::string_view fullMethod,
                          const Bytes& requestMsg,
                          const Metadata& metadata,
                          int deadlineMs) = 0;
  virtual void close() = 0;
};
```
DEADLINE_EXCEEDED on a poll is normal (re-poll), not an error. Poll deadline
≈ 20–70s (see poc/ + sdk-go: server holds 60s, client ~70s).

## 2. `IProtoCodec` — typed msg ⇄ bytes (proto)

Core works on typed structs; this converts to/from protobuf wire bytes. nanopb
impl on device, libprotobuf twin on host, both generated from `proto`. One
method pair per used message; codegen'd. Core stays protobuf-lib-agnostic.

```cpp
template<class Msg> struct IProtoCodec {
  virtual Bytes encode(const Msg&) = 0;
  virtual bool  decode(const Bytes&, Msg& out) = 0;
};
```
Messages in scope (v1): PollActivityTaskQueueRequest/Response,
RespondActivityTaskCompleted/FailedRequest, RecordActivityTaskHeartbeatRequest,
PollWorkflowTaskQueueRequest/Response, RespondWorkflowTaskCompletedRequest,
StartWorkflowExecutionRequest/Response, SignalWorkflowExecutionRequest,
QueryWorkflowRequest/Response, GetWorkflowExecutionHistoryRequest/Response, and
the message closure they reference (Payload, Payloads, WorkflowExecution,
Command + command variants, HistoryEvent + event-attributes closure, TaskQueue,
WorkflowType, ActivityType, RetryPolicy, Failure, Header, Memo).

## 3. `IPayloadCodec` — Mistral WorkflowContext envelope (codec)

See [`spec/codec-wire-format.md`](spec/codec-wire-format.md) for the byte spec.
v1 = metadata map + raw-JSON passthrough (encryption/offload/compression off).

```cpp
struct DecodedActivityInput { json argument; WorkflowContext context; bool empty; };
struct IPayloadCodec {
  virtual Result<DecodedActivityInput> decodeActivityInput(const temporal::Payload&) = 0;
  virtual temporal::Payload encodeActivityResult(const json& result,
                                                 const WorkflowContext& echoCtx,
                                                 bool empty) = 0;
};
```

## 4. `IDurableStore` — no-SD durable state (firmware impl, core interface)

Persist in-flight execution state + task tokens across reboot. Device backing =
NVS journal + LittleFS. Host fake = in-memory /
temp-file. Append-or-replace by key; survives power cycle.

```cpp
struct IDurableStore {
  virtual bool put(std::string_view key, const Bytes& value) = 0;
  virtual std::optional<Bytes> get(std::string_view key) = 0;
  virtual bool erase(std::string_view key) = 0;
  virtual std::vector<std::string> keys(std::string_view prefix) = 0;
};
```
Keys: `exec/<runId>` = serialized replay state; `token/<runId>` = task token.

## 5. `IWorkflowSpec` — declarative workflow grammar (core)

**Determinism by construction.** No arbitrary C++ workflow code — a workflow is
DATA the replay engine interprets, so replay is deterministic without a sandbox.
Grammar (a bounded subset of the Mistral Workflows node vocabulary, v1):

- `activity`   — schedule activity by name with a JSON arg (from prior results / input); await result.
- `sequence`   — ordered steps.
- `conditional`— predicate over accumulated results → branch (true/false step lists).
- `parallel`   — fan out N activity branches, join all.
- `timer`      — durable sleep (StartTimer).
- `wait_signal`— block until a named signal arrives (wait_condition / human_input).
- `complete`   — CompleteWorkflowExecution with a JSON result (or FailWorkflowExecution).

Spec is JSON (`WorkflowSpec` = `{name, input_schema, output_schema, steps[]}`),
loaded from `IDurableStore`/flash. v1 supports activity+sequence+conditional+
complete; parallel/timer/wait_signal are the v1.1 grammar (interface reserves them).
Deferred (out of v1 grammar): `agent`, `memory_op`, `try_except`, `loop`, child
workflows, updates/queries handlers.

## 6. Determinism seams — replay engine inputs (core)

The engine feeds an event history in; these are the ONLY sources of nondeterminism,
all recorded to / replayed from history so a re-run reproduces exactly.

```cpp
struct IClock  { virtual uint64_t nowMs() = 0; };   // during replay: returns recorded event time
struct IRandom { virtual uint64_t next() = 0; };     // seeded; seed recorded in history
// side effects (activity results) come ONLY from history events, never live calls during replay.
```

## 7. `IActivityHandler` registry — device work (core + firmware)

Handler = `(name, jsonIn)→jsonOut`.

```cpp
using ActivityFn = std::function<Result<json>(const json& arg)>;
struct IActivityRegistry {
  virtual void registerActivity(std::string name, ActivityFn) = 0;
  virtual bool has(std::string_view name) = 0;
  virtual Result<json> invoke(std::string_view name, const json& arg) = 0;
};
```

---

## Integration currency

Cross-layer integration flows through **golden vectors** (`conformance/`),
not live handshakes: recorded Temporal event histories (feed the replay engine),
captured custom Payloads (feed the payload codec), and message wire captures
(feed the proto codecs). A layer is "green" when host tests pass against
goldens; live-Mistral smoke is the final gate.

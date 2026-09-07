// codec: PayloadCodecV1 — Mistral WorkflowContext envelope codec.
// Wire authority: ../contracts/spec/codec-wire-format.md.
//
// v1 scope: encryption / offloading / compression are OFF, so encode/decode of
// the inner payload content is IDENTITY. The inner Payload.data is therefore raw
// JSON bytes (the activity argument or result) and the WorkflowContext rides the
// Temporal Payload.metadata map. This file is a pure metadata-map transform plus
// a byte passthrough — it does NOT parse the JSON body (that crosses the seam as
// Bytes), so it pulls in no JSON library.
#include "mwf_codec/payload_codec.h"

namespace mwf_codec {
namespace {

// Look up a metadata key; returns nullptr when absent.
const std::string* find(const mwf::Metadata& m, const char* key) {
  auto it = m.find(key);
  return it == m.end() ? nullptr : &it->second;
}

// The "empty_payload" metadata value on the wire is TRUTHY BYTES. The real
// Mistral SDK writes a single 0x00 byte (verified against conformance's golden
// empty-out.json: base64 "AA=="), which is truthy in Python because it is a
// non-empty bytes object — content is irrelevant. We mirror both directions:
//   encode → a single 0x00 byte; decode → empty iff the key is present with a
//   non-empty value (i.e. Python bool(bytes)).
inline const std::string& emptyPayloadSentinel() {
  static const std::string s(1, '\0');  // one 0x00 byte
  return s;
}
bool truthy(const std::string& v) { return !v.empty(); }

}  // namespace

mwf::Result<mwf::DecodedActivityInput> PayloadCodecV1::decodeActivityInput(
    const mwf::temporal::Payload& payload) {
  using R = mwf::Result<mwf::DecodedActivityInput>;
  const mwf::Metadata& md = payload.metadata;

  const std::string* encoding = find(md, kKeyEncoding);
  if (!encoding) {
    return R::failure("payload metadata missing 'encoding'");
  }

  // Backward-compat: a plain Temporal payload carries no WorkflowContext. Treat
  // the whole data blob as the raw JSON argument with an empty context.
  if (*encoding == kEncodingPlain) {
    mwf::DecodedActivityInput out;
    out.argument_json = payload.data;
    out.context = mwf::WorkflowContext{};  // no context on the wire
    out.empty = false;
    return R::success(std::move(out));
  }

  // Legacy custom format is decode-only and out of v1 scope; anything else is
  // simply unrecognized.
  if (*encoding != kEncodingWfV1) {
    return R::failure("unsupported payload encoding '" + *encoding + "'");
  }

  // Custom "json/wf_v1" envelope: reconstruct the WorkflowContext from metadata.
  mwf::WorkflowContext ctx;
  if (const std::string* ns = find(md, kKeyNamespace)) ctx.ns = *ns;
  if (const std::string* eid = find(md, kKeyExecutionId)) ctx.execution_id = *eid;

  // decode REQUIRES execution_id non-empty.
  if (ctx.execution_id.empty()) {
    return R::failure("json/wf_v1 payload has empty execution_id");
  }

  if (const std::string* v = find(md, kKeyRootWorkflowExecId)) ctx.root_workflow_exec_id = *v;
  if (const std::string* v = find(md, kKeyParentWorkflowExecId)) ctx.parent_workflow_exec_id = *v;
  if (const std::string* v = find(md, kKeyExecutionToken)) ctx.execution_token = *v;
  if (const std::string* v = find(md, kKeyExtensions)) ctx.extensions_json = *v;
  if (const std::string* v = find(md, kKeyOnBehalfOf)) ctx.on_behalf_of = (*v == "true");
  // encoding_options is empty in v1 and carries no decode-time behavior — ignored.

  mwf::DecodedActivityInput out;
  out.argument_json = payload.data;  // v1 identity: raw JSON passthrough
  out.context = std::move(ctx);
  const std::string* emptyMd = find(md, kKeyEmptyPayload);
  out.empty = emptyMd && truthy(*emptyMd);
  return R::success(std::move(out));
}

mwf::temporal::Payload PayloadCodecV1::encodeActivityResult(
    const mwf::Bytes& result_json, const mwf::WorkflowContext& echoCtx, bool empty) {
  mwf::temporal::Payload p;
  mwf::Metadata& md = p.metadata;

  // Always-present keys (order in the map is irrelevant — it's keyed).
  md[kKeyEncoding] = kEncodingWfV1;
  md[kKeyNamespace] = echoCtx.ns;
  md[kKeyExecutionId] = echoCtx.execution_id;
  md[kKeyEncodingOptions] = "";  // empty in v1 (no encryption/offload/compression)

  // Optional keys — echo only when present on the context.
  if (echoCtx.root_workflow_exec_id) md[kKeyRootWorkflowExecId] = *echoCtx.root_workflow_exec_id;
  if (echoCtx.parent_workflow_exec_id)
    md[kKeyParentWorkflowExecId] = *echoCtx.parent_workflow_exec_id;
  if (echoCtx.execution_token) md[kKeyExecutionToken] = *echoCtx.execution_token;
  // __internal_extensions only when non-empty (matches "if ctx.extensions").
  if (echoCtx.extensions_json && !echoCtx.extensions_json->empty())
    md[kKeyExtensions] = *echoCtx.extensions_json;
  if (echoCtx.on_behalf_of) md[kKeyOnBehalfOf] = *echoCtx.on_behalf_of ? "true" : "false";
  if (empty) md[kKeyEmptyPayload] = emptyPayloadSentinel();  // single 0x00 byte

  p.data = result_json;  // v1 identity: raw JSON passthrough
  return p;
}

}  // namespace mwf_codec

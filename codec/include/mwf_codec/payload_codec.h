// codec: PayloadCodecV1 — the Mistral WorkflowContext envelope codec.
// Implements mwf::IPayloadCodec (../contracts/include/mwf/contracts.h §3).
// Wire authority: ../contracts/spec/codec-wire-format.md.
//
// v1 = metadata map + raw-JSON passthrough (encryption/offload/compression OFF):
// the inner Payload.data is raw JSON, the WorkflowContext rides Payload.metadata.
// No JSON library is needed on the core path — this is a metadata-map transform
// plus a byte passthrough (argument/result JSON crosses the seam as Bytes).
#pragma once
#include "mwf/contracts.h"

namespace mwf_codec {

// Encoding constants (mirror core/encoding/constants.py). Public so tests and
// callers can assert against the exact spec strings.
inline constexpr const char* kEncodingWfV1 = "json/wf_v1";   // NEW == CUSTOM format
inline constexpr const char* kEncodingPlain = "json/plain";  // Temporal plain (decode compat)
inline constexpr const char* kEncodingLegacy =
    "json/abraxas_v1";  // legacy; decode-only, out of v1 scope

// Metadata keys (mirror build_temporal_payload_metadata /
// build_info_from_payload_metadata). INTERNAL_METADATA_PREFIX = "__internal_".
inline constexpr const char* kKeyEncoding = "encoding";
inline constexpr const char* kKeyNamespace = "namespace";
inline constexpr const char* kKeyExecutionId = "execution_id";
inline constexpr const char* kKeyEncodingOptions = "encoding_options";
inline constexpr const char* kKeyRootWorkflowExecId = "root_workflow_exec_id";
inline constexpr const char* kKeyParentWorkflowExecId = "parent_workflow_exec_id";
inline constexpr const char* kKeyExecutionToken = "__internal_execution_token";
inline constexpr const char* kKeyExtensions = "__internal_extensions";
inline constexpr const char* kKeyOnBehalfOf = "__internal_on_behalf_of";
inline constexpr const char* kKeyEmptyPayload = "empty_payload";

class PayloadCodecV1 : public mwf::IPayloadCodec {
 public:
  PayloadCodecV1() = default;
  ~PayloadCodecV1() override = default;

  // Split a Temporal Payload into (argument JSON bytes, WorkflowContext).
  //   encoding == "json/wf_v1": custom envelope. Reconstruct the context from
  //     metadata; fail if execution_id is empty. argument_json = raw data bytes.
  //   encoding == "json/plain": backward-compat. Treat data as the raw JSON
  //     argument with an EMPTY WorkflowContext (no context on the wire).
  //   anything else (incl. legacy "json/abraxas_v1", or a missing key): failure.
  mwf::Result<mwf::DecodedActivityInput> decodeActivityInput(
      const mwf::temporal::Payload&) override;

  // Re-wrap an activity result JSON into a Temporal Payload, echoing the context
  // metadata back so the control plane can correlate the completion. Emits the
  // full "json/wf_v1" metadata map (encoding_options always "", optional keys
  // only when present, empty_payload only when `empty`). data = result_json.
  mwf::temporal::Payload encodeActivityResult(const mwf::Bytes& result_json,
                                              const mwf::WorkflowContext& echoCtx,
                                              bool empty) override;
};

}  // namespace mwf_codec

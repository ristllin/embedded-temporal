// contracts: portable mirrors of the Temporal/Mistral wire structs the seams pass around.
// These are lib-agnostic value types (NOT protobuf classes). The proto codecs map them <-> wire bytes.
#pragma once
#include "types.h"

namespace mwf {
namespace temporal {

// Mirror of temporal.api.common.v1.Payload
struct Payload {
  Metadata metadata;   // e.g. {"encoding":"json/wf_v1","execution_id":"...", ...}
  Bytes    data;       // inner payload bytes (raw JSON in v1)
};

}  // namespace temporal

// Mistral WorkflowContext (see spec/codec-wire-format.md). Wire-relevant fields only.
struct WorkflowContext {
  std::string ns;                                    // "namespace" (reserved word) — required on wire
  std::string execution_id;                          // required; decode fails if empty
  std::optional<std::string> root_workflow_exec_id;
  std::optional<std::string> parent_workflow_exec_id;
  std::optional<std::string> execution_token;
  std::optional<std::string> extensions_json;        // json.dumps(extensions); empty => omit
  std::optional<bool> on_behalf_of;
};

}  // namespace mwf

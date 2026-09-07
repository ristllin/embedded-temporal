// gen/proto_codec.h — the IProtoCodec<Msg> host twin (libprotobuf).
//
// CONTRACTS.md §2 defines IProtoCodec as a *generated* template (it is NOT in
// contracts/contracts.h by design — the comment there points here). This
// header realizes it for the host: every libprotobuf-generated message derives
// from google::protobuf::MessageLite, so ONE generic adapter covers the whole
// §2 closure. Core code sees only mwf::IProtoCodec<Msg> + mwf::Bytes; it never
// includes <google/protobuf/...> itself, keeping it protobuf-lib-agnostic.
//
// The device (nanopb) twin implements the same mwf::IProtoCodec<Msg> shape over
// pb_encode/pb_decode; see gen_nanopb.sh + README "nanopb twin".
#pragma once

#include <string>

#include "mwf/types.h"  // mwf::Bytes

// Generated Temporal message headers (concrete Msg types core/transport construct).
#include "temporal/api/common/v1/message.pb.h"
#include "temporal/api/history/v1/message.pb.h"
#include "temporal/api/command/v1/message.pb.h"
#include "temporal/api/failure/v1/message.pb.h"
#include "temporal/api/taskqueue/v1/message.pb.h"
#include "temporal/api/workflowservice/v1/request_response.pb.h"

namespace mwf {

// The §2 contract interface (typed msg <-> protobuf wire bytes).
template <class Msg>
struct IProtoCodec {
  virtual ~IProtoCodec() = default;
  virtual Bytes encode(const Msg&) = 0;
  virtual bool  decode(const Bytes&, Msg& out) = 0;
};

// Host realization over libprotobuf. Msg = any generated message type.
template <class Msg>
struct ProtoCodec final : IProtoCodec<Msg> {
  Bytes encode(const Msg& m) override {
    std::string s;
    (void)m.SerializeToString(&s);  // false only on >2GiB msg; not reachable here
    return Bytes(s.begin(), s.end());
  }
  bool decode(const Bytes& b, Msg& out) override {
    return out.ParseFromArray(b.data(), static_cast<int>(b.size()));
  }
};

// ── Concrete generated type aliases core/transport consume (lib-agnostic names) ─
// The fully-qualified libprotobuf type names live in the temporal::api::*::v1
// namespaces; these aliases are the boundary vocabulary. Device code uses the
// nanopb structs of the same logical names (see README type-name table).
namespace proto {
namespace common = ::temporal::api::common::v1;
namespace hist   = ::temporal::api::history::v1;
namespace cmd    = ::temporal::api::command::v1;
namespace tq     = ::temporal::api::taskqueue::v1;
namespace fail   = ::temporal::api::failure::v1;
namespace wsvc   = ::temporal::api::workflowservice::v1;

using Payload        = common::Payload;
using Payloads       = common::Payloads;
using WorkflowExec   = common::WorkflowExecution;
using WorkflowType   = common::WorkflowType;
using ActivityType   = common::ActivityType;
using RetryPolicy    = common::RetryPolicy;
using Header         = common::Header;
using Memo           = common::Memo;
using Failure        = fail::Failure;
using TaskQueue      = tq::TaskQueue;
using Command        = cmd::Command;
using HistoryEvent   = hist::HistoryEvent;
using History        = hist::History;

using PollActivityTaskQueueRequest    = wsvc::PollActivityTaskQueueRequest;
using PollActivityTaskQueueResponse   = wsvc::PollActivityTaskQueueResponse;
using RespondActivityTaskCompletedReq = wsvc::RespondActivityTaskCompletedRequest;
using RespondActivityTaskFailedReq    = wsvc::RespondActivityTaskFailedRequest;
using RecordActivityTaskHeartbeatReq  = wsvc::RecordActivityTaskHeartbeatRequest;
using PollWorkflowTaskQueueRequest    = wsvc::PollWorkflowTaskQueueRequest;
using PollWorkflowTaskQueueResponse   = wsvc::PollWorkflowTaskQueueResponse;
using RespondWorkflowTaskCompletedReq = wsvc::RespondWorkflowTaskCompletedRequest;
using StartWorkflowExecutionRequest   = wsvc::StartWorkflowExecutionRequest;
using StartWorkflowExecutionResponse  = wsvc::StartWorkflowExecutionResponse;
using SignalWorkflowExecutionRequest  = wsvc::SignalWorkflowExecutionRequest;
using QueryWorkflowRequest            = wsvc::QueryWorkflowRequest;
using QueryWorkflowResponse           = wsvc::QueryWorkflowResponse;
using GetHistoryRequest               = wsvc::GetWorkflowExecutionHistoryRequest;
using GetHistoryResponse              = wsvc::GetWorkflowExecutionHistoryResponse;
}  // namespace proto

}  // namespace mwf

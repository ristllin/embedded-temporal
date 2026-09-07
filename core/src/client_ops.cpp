// core: ClientOps bodies.
#include "mwf_core/client_ops.h"

#include <utility>

namespace mwf_core {
namespace {
constexpr int kSignalDeadlineMs = 10000;  // unary control call — quick
}  // namespace

ClientOps::ClientOps(mwf::ITransport& transport, IWorkflowProtoAdapter& proto,
                     std::string identity)
    : transport_(transport), proto_(proto), identity_(std::move(identity)) {}

mwf::Result<StartWorkflowResult> ClientOps::startWorkflow(const StartWorkflowArgs& /*args*/) {
  // Would encode StartWorkflowExecutionRequest → transport_.call → run_id.
  return mwf::Result<StartWorkflowResult>::failure(
      "startWorkflow is not implemented yet; start executions via the REST "
      "control plane (RestClient::executeWorkflow)");
}

mwf::Result<void> ClientOps::signalWorkflow(std::string_view ns,
                                            std::string_view workflow_id,
                                            std::string_view signal_name,
                                            const mwf::Bytes& payload_json) {
  SignalRequest req;
  req.ns = std::string(ns);
  req.workflow_id = std::string(workflow_id);
  // run_id intentionally left empty ⇒ the frontend targets the latest run.
  req.signal_name = std::string(signal_name);
  req.identity = identity_;
  // Wrap the raw-JSON argument as a json/plain Payload (the default Temporal
  // converter; the workflow's replay binds the inner data bytes verbatim).
  req.input.metadata["encoding"] = "json/plain";
  req.input.data = payload_json;

  mwf::Bytes wire = proto_.buildSignalWorkflowRequest(req);
  if (wire.empty()) {
    return mwf::Result<void>::failure(
        "signalWorkflow: failed to build SignalWorkflowExecutionRequest "
        "(over-cap field?)");
  }
  // Mirror the poll/respond calls: route/authorize by the temporal-namespace
  // header (not only the body's ns field) so the signal targets the right
  // namespace under header-based frontend routing / multi-namespace setups.
  mwf::Metadata md;  // transport adds authorization
  md["temporal-namespace"] = std::string(ns);
  mwf::GrpcResult r =
      transport_.call(kMethodSignalWorkflowExecution, wire, md, kSignalDeadlineMs);
  if (r.grpc_status != 0) {
    return mwf::Result<void>::failure(
        "SignalWorkflowExecution grpc_status=" + std::to_string(r.grpc_status) +
        " " + r.message);
  }
  return mwf::Result<void>::success();
}

mwf::Result<mwf::Bytes> ClientOps::queryWorkflow(std::string_view /*ns*/,
                                                 std::string_view /*workflow_id*/,
                                                 std::string_view /*query_type*/,
                                                 const mwf::Bytes& /*args_json*/) {
  // Would encode QueryWorkflowRequest → QueryWorkflowResponse result bytes.
  return mwf::Result<mwf::Bytes>::failure(
      "queryWorkflow is not implemented yet; inspect execution state via the "
      "REST control plane instead");
}

}  // namespace mwf_core

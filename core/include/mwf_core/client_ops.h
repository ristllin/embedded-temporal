// core: ClientOps — workflow client operations.
// start / signal / query a workflow execution over ITransport, using the
// generated IProtoCodec for the request/response messages. This is the
// "control" side (the device can also START workflows, not just serve them).
// Implemented today: signalWorkflow. startWorkflow / queryWorkflow return a
// clean not-implemented failure (see ROADMAP).
#pragma once
#include <string>
#include "mwf/contracts.h"
#include "mwf_core/workflow_proto_adapter.h"  // IWorkflowProtoAdapter + SignalRequest

namespace mwf_core {

struct StartWorkflowArgs {
  std::string ns;
  std::string workflow_id;
  std::string workflow_type;
  std::string task_queue;
  mwf::Bytes  input_json;   // raw-JSON argument (codec wraps into a Payload)
};

struct StartWorkflowResult {
  std::string run_id;       // populated on success
};

// Thin client over the transport + the workflow proto seam (the builder that
// serializes the request; keeps ClientOps protobuf-lib-free like the loops).
// deadline/metadata handling lives in the impl.
class ClientOps {
 public:
  ClientOps(mwf::ITransport& transport, IWorkflowProtoAdapter& proto,
            std::string identity = "mwf-client");

  mwf::Result<StartWorkflowResult> startWorkflow(const StartWorkflowArgs&);  // not implemented yet
  // Deliver a signal to a running workflow (SignalWorkflowExecution RPC). The
  // targeted workflow's replay unblocks its matching wait_signal step. run_id is
  // implicitly the latest run of workflow_id. `payload_json` is the raw-JSON
  // signal argument (wrapped as a json/plain Payload on the wire).
  mwf::Result<void> signalWorkflow(std::string_view ns, std::string_view workflow_id,
                                   std::string_view signal_name,
                                   const mwf::Bytes& payload_json);
  mwf::Result<mwf::Bytes> queryWorkflow(std::string_view ns, std::string_view workflow_id,
                                        std::string_view query_type,
                                        const mwf::Bytes& args_json);         // not implemented yet

 private:
  mwf::ITransport&       transport_;
  IWorkflowProtoAdapter& proto_;
  std::string            identity_;
};

}  // namespace mwf_core

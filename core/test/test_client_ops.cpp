// test_client_ops.cpp — ClientOps::signalWorkflow (the signal SENDER).
//
// Proves ClientOps assembles the SignalRequest from its args, drives the proto
// seam's builder, and posts the bytes to the SignalWorkflowExecution RPC —
// classifying the transport result. Uses a fake transport + a fake proto adapter
// that captures the SignalRequest (no protobuf here; the WIRE round-trip of the
// message lives in proto/test_nanopb_device_workflow_conformance).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mwf_core/client_ops.h"

namespace {

mwf::Bytes bytes(const std::string& s) { return mwf::Bytes(s.begin(), s.end()); }
std::string str(const mwf::Bytes& b) { return std::string(b.begin(), b.end()); }

struct RecordedCall {
  std::string method;
  mwf::Bytes request;
};

struct FakeTransport : mwf::ITransport {
  mwf::GrpcResult result{0, "", {}};  // default OK
  std::vector<RecordedCall> calls;
  mwf::GrpcResult call(std::string_view method, const mwf::Bytes& req,
                       const mwf::Metadata&, int) override {
    calls.push_back({std::string(method), req});
    return result;
  }
  void close() override {}
};

// Captures the SignalRequest ClientOps hands it; returns scripted bytes (or empty
// to simulate an over-cap build failure). Worker-poll methods are unused here.
struct FakeSignalAdapter : mwf_core::IWorkflowProtoAdapter {
  bool have_signal = false;
  mwf_core::SignalRequest last;
  mwf::Bytes built = bytes("SIGNAL-WIRE");

  mwf::Bytes buildPollWorkflowRequest(const mwf_core::WorkflowConfig&) override { return {}; }
  mwf::Result<mwf_core::WorkflowTaskInfo> parseWorkflowTask(const mwf::Bytes&) override {
    return mwf::Result<mwf_core::WorkflowTaskInfo>::failure("unused");
  }
  mwf::Bytes buildRespondWorkflowCompleted(
      const mwf_core::WorkflowConfig&, const mwf::Bytes&,
      const std::vector<mwf_core::ProtoCommand>&) override { return {}; }

  mwf::Bytes buildSignalWorkflowRequest(const mwf_core::SignalRequest& s) override {
    have_signal = true;
    last = s;
    return built;
  }
};

}  // namespace

TEST(ClientOpsSignal, BuildsRequestAndPostsToSignalRpc) {
  FakeTransport transport;
  FakeSignalAdapter adapter;
  mwf_core::ClientOps client(transport, adapter, "tester@host");

  auto r = client.signalWorkflow("ns-1", "wf-42", "approve", bytes(R"({"ok":true})"));
  ASSERT_TRUE(r.ok) << r.error;

  // The adapter saw the fields ClientOps assembled.
  ASSERT_TRUE(adapter.have_signal);
  EXPECT_EQ(adapter.last.ns, "ns-1");
  EXPECT_EQ(adapter.last.workflow_id, "wf-42");
  EXPECT_TRUE(adapter.last.run_id.empty());  // latest run
  EXPECT_EQ(adapter.last.signal_name, "approve");
  EXPECT_EQ(adapter.last.identity, "tester@host");
  EXPECT_EQ(adapter.last.input.metadata.at("encoding"), "json/plain");
  EXPECT_EQ(str(adapter.last.input.data), R"({"ok":true})");

  // The built bytes went to the SignalWorkflowExecution method.
  ASSERT_EQ(transport.calls.size(), 1u);
  EXPECT_EQ(transport.calls[0].method, mwf_core::kMethodSignalWorkflowExecution);
  EXPECT_EQ(str(transport.calls[0].request), "SIGNAL-WIRE");
}

TEST(ClientOpsSignal, BuildFailureIsReportedNotSent) {
  FakeTransport transport;
  FakeSignalAdapter adapter;
  adapter.built = {};  // simulate an over-cap field ⇒ empty build
  mwf_core::ClientOps client(transport, adapter);

  auto r = client.signalWorkflow("ns", "wf", "go", bytes("1"));
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("failed to build"), std::string::npos) << r.error;
  EXPECT_TRUE(transport.calls.empty());  // nothing posted on a build failure
}

TEST(ClientOpsSignal, TransportErrorPropagates) {
  FakeTransport transport;
  transport.result = {14, "unavailable", {}};
  FakeSignalAdapter adapter;
  mwf_core::ClientOps client(transport, adapter);

  auto r = client.signalWorkflow("ns", "wf", "go", bytes("1"));
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("grpc_status=14"), std::string::npos) << r.error;
}

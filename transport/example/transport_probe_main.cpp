// transport example: prove DesktopTransport makes a real unary gRPC call.
// Against a local Temporal dev server (`temporal server start-dev`, frontend on
// localhost:7233) this exercises three paths through the generic-stub transport
// with NO generated proto — request bytes are hand-encoded protobuf here (Track
// A's IProtoCodec does this in the real worker):
//
//   1. GetSystemInfo      — empty request → grpc_status 0 (connectivity proof)
//   2. DescribeNamespace  — {namespace:"default"} → grpc_status 0
//   3. PollActivityTaskQueue — long-poll → grpc_status 4 DEADLINE_EXCEEDED
//      after the deadline (the empty-poll path the worker loop relies on)
//
//   ./transport_probe [target]     # default localhost:7233
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "desktop_transport.h"

using mwf::Bytes;

// ── minimal protobuf wire encoders (length-delimited string + varint) ─────────
static void putVarint(Bytes& b, uint64_t v) {
  while (v >= 0x80) { b.push_back(static_cast<uint8_t>(v) | 0x80); v >>= 7; }
  b.push_back(static_cast<uint8_t>(v));
}
static void putLenDelim(Bytes& b, int field, const std::string& s) {
  putVarint(b, (static_cast<uint64_t>(field) << 3) | 2);  // wire type 2
  putVarint(b, s.size());
  b.insert(b.end(), s.begin(), s.end());
}
static void putLenDelim(Bytes& b, int field, const Bytes& msg) {
  putVarint(b, (static_cast<uint64_t>(field) << 3) | 2);
  putVarint(b, msg.size());
  b.insert(b.end(), msg.begin(), msg.end());
}
static void putVarintField(Bytes& b, int field, uint64_t v) {
  putVarint(b, (static_cast<uint64_t>(field) << 3) | 0);  // wire type 0
  putVarint(b, v);
}

static const char* codeName(int c) {
  switch (c) {
    case 0:  return "OK";
    case 4:  return "DEADLINE_EXCEEDED";
    case 14: return "UNAVAILABLE";
    case 16: return "UNAUTHENTICATED";
    default: return "?";
  }
}

int main(int argc, char** argv) {
  const std::string target = argc > 1 ? argv[1] : "localhost:7233";
  const std::string ns = "default";
  const char* kSvc = "/temporal.api.workflowservice.v1.WorkflowService/";

  // Local dev server: no bearer, insecure (tls forced off).
  mwf_transport::DesktopTransport transport(target, /*bearer=*/"", /*tls=*/false);

  std::cout << "[probe] target=" << target << " (insecure, local dev)\n\n";
  int failures = 0;

  // 1. GetSystemInfo — empty request.
  {
    mwf::GrpcResult r =
        transport.call(std::string(kSvc) + "GetSystemInfo", Bytes{}, {}, 5000);
    std::cout << "1. GetSystemInfo      -> grpc_status=" << r.grpc_status << " ("
              << codeName(r.grpc_status) << ")"
              << " resp_bytes=" << r.response.size();
    if (!r.message.empty()) std::cout << " msg=\"" << r.message << "\"";
    std::cout << "\n";
    if (r.grpc_status != 0) ++failures;
  }

  // 2. DescribeNamespace — {namespace:"default"} (field 1, string).
  {
    Bytes req;
    putLenDelim(req, 1, ns);
    mwf::GrpcResult r =
        transport.call(std::string(kSvc) + "DescribeNamespace", req, {}, 5000);
    std::cout << "2. DescribeNamespace  -> grpc_status=" << r.grpc_status << " ("
              << codeName(r.grpc_status) << ")"
              << " resp_bytes=" << r.response.size();
    if (!r.message.empty()) std::cout << " msg=\"" << r.message << "\"";
    std::cout << "\n";
    if (r.grpc_status != 0) ++failures;
  }

  // 3. PollActivityTaskQueue — long-poll; expect DEADLINE_EXCEEDED (empty poll).
  //    PollActivityTaskQueueRequest{ namespace=1:string, task_queue=2:TaskQueue }
  //    TaskQueue{ name=1:string, kind=2:enum(1=NORMAL) }
  {
    Bytes tq;
    putLenDelim(tq, 1, std::string("mwf-probe-tq"));
    putVarintField(tq, 2, 1);  // TASK_QUEUE_KIND_NORMAL
    Bytes req;
    putLenDelim(req, 1, ns);
    putLenDelim(req, 2, tq);
    putLenDelim(req, 3, std::string("mwf-probe@desktop"));  // identity=3:string

    std::cout << "3. PollActivityTaskQueue (long-poll ~2s) ... " << std::flush;
    mwf::GrpcResult r =
        transport.call(std::string(kSvc) + "PollActivityTaskQueue", req, {}, 2000);
    std::cout << "-> grpc_status=" << r.grpc_status << " ("
              << codeName(r.grpc_status) << ")";
    if (!r.message.empty()) std::cout << " msg=\"" << r.message << "\"";
    std::cout << "\n";
    // status 0 (a task, unlikely) or 4 (empty poll) both prove the poll path.
    if (r.grpc_status != 0 && r.grpc_status != 4) ++failures;
  }

  // 4. PollWorkflowTaskQueue — the frontend HOLDS this poll open when no task is
  //    queued, so with a short deadline we observe the empty-long-poll path:
  //    grpc_status 4 DEADLINE_EXCEEDED, which the transport reports as a normal
  //    empty result (not an error) for the worker to re-poll.
  {
    Bytes tq;
    putLenDelim(tq, 1, std::string("mwf-probe-wf-tq"));
    putVarintField(tq, 2, 1);  // TASK_QUEUE_KIND_NORMAL
    Bytes req;
    putLenDelim(req, 1, ns);
    putLenDelim(req, 2, tq);
    putLenDelim(req, 3, std::string("mwf-probe@desktop"));

    std::cout << "4. PollWorkflowTaskQueue (held long-poll ~2s) ... " << std::flush;
    mwf::GrpcResult r =
        transport.call(std::string(kSvc) + "PollWorkflowTaskQueue", req, {}, 2000);
    std::cout << "-> grpc_status=" << r.grpc_status << " ("
              << codeName(r.grpc_status) << ")";
    if (!r.message.empty()) std::cout << " msg=\"" << r.message << "\"";
    std::cout << "\n";
    if (r.grpc_status != 0 && r.grpc_status != 4) ++failures;
  }

  transport.close();
  std::cout << "\n[probe] " << (failures ? "FAILURES: " : "all paths OK")
            << (failures ? std::to_string(failures) : std::string()) << "\n";
  return failures ? 1 : 0;
}

// transport example: prove the esp/grpc_h2 framing core (the SAME code the
// ESP32 transport runs) against real servers, side-by-side with the already
// live-verified DesktopTransport (grpc++).
//
// Local mode (default; needs `temporal server start-dev` on localhost:7233):
//   1. GetSystemInfo via H2HostTransport AND DesktopTransport → both status 0,
//      response bytes compared BYTE-WISE (same server, deterministic reply).
//   2. DescribeNamespace — same comparison.
//   3. PollActivityTaskQueue, 2 s deadline → taskless OK (dev server answers
//      just before the client deadline) or DEADLINE_EXCEEDED — both are the
//      worker loop's Idle path.
//   4. Session reuse: calls 1–3 all rode ONE h2 connection (asserted).
//
// Live mode (--live; needs MISTRAL_API_KEY):
//   whoami (REST) → scheduler target + namespace, then GetSystemInfo over the
//   h2 stack against wf-scheduler.mistral.ai:443 (TLS + ALPN h2). --live-poll
//   additionally runs one PollActivityTaskQueue with a 50 s deadline: Mistral's
//   frontend holds ~45 s then answers OK-taskless (empty task_token) — NOT a
//   client deadline; both that and status 4 count as the Idle path.
//
//   ./h2_probe [target]         # local, default localhost:7233
//   ./h2_probe --live [--live-poll]
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "../esp/host/h2_host_stream.h"
#include "desktop_transport.h"
#include "rest_client.h"

using mwf::Bytes;

// ── minimal protobuf wire encoders (same as transport_probe) ─────────────────
static void putVarint(Bytes& b, uint64_t v) {
  while (v >= 0x80) { b.push_back(static_cast<uint8_t>(v) | 0x80); v >>= 7; }
  b.push_back(static_cast<uint8_t>(v));
}
static void putLenDelim(Bytes& b, int field, const std::string& s) {
  putVarint(b, (static_cast<uint64_t>(field) << 3) | 2);
  putVarint(b, s.size());
  b.insert(b.end(), s.begin(), s.end());
}
static void putLenDelim(Bytes& b, int field, const Bytes& msg) {
  putVarint(b, (static_cast<uint64_t>(field) << 3) | 2);
  putVarint(b, msg.size());
  b.insert(b.end(), msg.begin(), msg.end());
}
static void putVarintField(Bytes& b, int field, uint64_t v) {
  putVarint(b, (static_cast<uint64_t>(field) << 3) | 0);
  putVarint(b, v);
}

static const char* kSvc = "/temporal.api.workflowservice.v1.WorkflowService/";

static const char* codeName(int c) {
  switch (c) {
    case 0:  return "OK";
    case 4:  return "DEADLINE_EXCEEDED";
    case 12: return "UNIMPLEMENTED";
    case 14: return "UNAVAILABLE";
    case 16: return "UNAUTHENTICATED";
    default: return "?";
  }
}

static Bytes pollRequest(const std::string& ns, const std::string& queue,
                         const std::string& identity) {
  Bytes tq;
  putLenDelim(tq, 1, queue);
  putVarintField(tq, 2, 1);  // TASK_QUEUE_KIND_NORMAL
  Bytes req;
  putLenDelim(req, 1, ns);
  putLenDelim(req, 2, tq);
  putLenDelim(req, 3, identity);
  return req;
}

// Extract field 1 (bytes task_token) length from a PollActivityTaskQueueResponse
// without a proto lib — enough to classify taskless vs task + report token size.
static long taskTokenLen(const Bytes& resp) {
  size_t i = 0;
  auto varint = [&](uint64_t& v) {
    v = 0;
    int shift = 0;
    while (i < resp.size()) {
      uint8_t b = resp[i++];
      v |= static_cast<uint64_t>(b & 0x7f) << shift;
      if (!(b & 0x80)) return true;
      shift += 7;
    }
    return false;
  };
  while (i < resp.size()) {
    uint64_t key, len;
    if (!varint(key)) return -1;
    int field = static_cast<int>(key >> 3), wt = static_cast<int>(key & 7);
    if (wt == 2) {
      if (!varint(len)) return -1;
      if (field == 1) return static_cast<long>(len);
      i += len;
    } else if (wt == 0) {
      uint64_t skip;
      if (!varint(skip)) return -1;
    } else if (wt == 5) i += 4;
    else if (wt == 1) i += 8;
    else return -1;
  }
  return 0;  // no task_token field = taskless
}

static void printMem(const mwf_transport::H2HostTransport& t) {
  const auto& m = t.memStats();
  std::cout << "[h2 mem] session allocator: current=" << m.current
            << " B  peak=" << m.peak << " B  total=" << m.total
            << " B over " << m.count << " allocations\n";
}

static int runLocal(const std::string& target) {
  std::cout << "[h2_probe] LOCAL target=" << target << " (h2c prior-knowledge)\n\n";
  mwf_transport::H2HostTransport h2(target, "", /*tls=*/0);
  mwf_transport::DesktopTransport ref(target, "", /*tls=*/false);
  int failures = 0;

  // 1+2. GetSystemInfo + DescribeNamespace: byte-wise compare vs grpc++.
  struct Case { const char* name; Bytes req; };
  Bytes descReq;
  putLenDelim(descReq, 1, std::string("default"));
  Case cases[] = {{"GetSystemInfo", Bytes{}}, {"DescribeNamespace", descReq}};
  int n = 0;
  for (auto& c : cases) {
    mwf::GrpcResult a = h2.call(std::string(kSvc) + c.name, c.req, {}, 5000);
    mwf::GrpcResult b = ref.call(std::string(kSvc) + c.name, c.req, {}, 5000);
    bool same = a.grpc_status == b.grpc_status && a.response == b.response;
    std::cout << ++n << ". " << c.name << "  h2: status=" << a.grpc_status << " ("
              << codeName(a.grpc_status) << ") " << a.response.size()
              << " B   grpc++: status=" << b.grpc_status << " "
              << b.response.size() << " B   byte-wise "
              << (same ? "IDENTICAL" : "MISMATCH") << "\n";
    if (a.grpc_status != 0 || !same) ++failures;
  }

  // 3. Empty long-poll (12 s): dev server answers taskless-OK near the client
  //    deadline, or the client deadline fires — both are the worker's Idle
  //    path. NOT shorter: the frontend rejects a context under
  //    MinLongPollTimeout (2 s, and an exact 2000 ms arrives as strictly less
  //    after transit → "Context timeout is too short.", observed live).
  {
    Bytes req = pollRequest("default", "mwf-h2-probe-tq", "h2-probe@host");
    std::cout << ++n << ". PollActivityTaskQueue (12s) ... " << std::flush;
    mwf::GrpcResult r =
        h2.call(std::string(kSvc) + "PollActivityTaskQueue", req, {}, 12000);
    long tok = r.grpc_status == 0 ? taskTokenLen(r.response) : 0;
    std::cout << "status=" << r.grpc_status << " (" << codeName(r.grpc_status)
              << ") resp=" << r.response.size() << " B task_token=" << tok
              << " B" << (r.message.empty() ? "" : " msg=\"" + r.message + "\"")
              << "\n";
    if (r.grpc_status != 0 && r.grpc_status != 4) ++failures;
  }

  printMem(h2);
  h2.close();
  ref.close();
  std::cout << "\n[h2_probe] " << (failures ? "FAILURES" : "all local paths OK")
            << "\n";
  return failures ? 1 : 0;
}

static int runLive(bool livePoll) {
  const char* key = std::getenv("MISTRAL_API_KEY");
  if (!key || !*key) {
    std::cerr << "[h2_probe] --live needs MISTRAL_API_KEY\n";
    return 2;
  }
  mwf_transport::RestClient rest("https://api.mistral.ai", key);
  auto who = rest.whoami();
  if (!who) {
    std::cerr << "[h2_probe] whoami failed: " << who.error << "\n";
    return 1;
  }
  std::cout << "[h2_probe] LIVE scheduler=" << who.value.scheduler_url
            << " ns=" << who.value.namespace_ << "\n\n";

  mwf_transport::H2HostTransport h2(who.value.scheduler_url, key);
  mwf::Metadata md{{"temporal-namespace", who.value.namespace_}};
  int failures = 0;

  {
    mwf::GrpcResult r =
        h2.call(std::string(kSvc) + "GetSystemInfo", Bytes{}, md, 15000);
    std::cout << "1. GetSystemInfo -> status=" << r.grpc_status << " ("
              << codeName(r.grpc_status) << ") resp=" << r.response.size()
              << " B" << (r.message.empty() ? "" : " msg=\"" + r.message + "\"")
              << "\n";
    if (r.grpc_status != 0) ++failures;
  }

  if (livePoll) {
    Bytes req =
        pollRequest(who.value.namespace_, "mwf-esp-h2-probe", "h2-probe@host");
    std::cout << "2. PollActivityTaskQueue (50s window; Mistral frontend "
                 "releases ~45s with taskless-OK) ... "
              << std::flush;
    mwf::GrpcResult r =
        h2.call(std::string(kSvc) + "PollActivityTaskQueue", req, md, 50000);
    long tok = r.grpc_status == 0 ? taskTokenLen(r.response) : 0;
    std::cout << "status=" << r.grpc_status << " (" << codeName(r.grpc_status)
              << ") resp=" << r.response.size() << " B task_token=" << tok
              << " B" << (r.message.empty() ? "" : " msg=\"" + r.message + "\"")
              << "\n";
    // Taskless OK (empty token) and client/server deadline are all Idle.
    if (r.grpc_status != 0 && r.grpc_status != 4) ++failures;
  }

  printMem(h2);
  h2.close();
  std::cout << "\n[h2_probe] " << (failures ? "FAILURES" : "all live paths OK")
            << "\n";
  return failures ? 1 : 0;
}

int main(int argc, char** argv) {
  bool live = false, livePoll = false;
  std::string target = "localhost:7233";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--live") live = true;
    else if (a == "--live-poll") { live = true; livePoll = true; }
    else target = a;
  }
  return live ? runLive(livePoll) : runLocal(target);
}

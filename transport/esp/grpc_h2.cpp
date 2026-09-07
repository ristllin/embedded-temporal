// transport esp/: portable gRPC-over-HTTP/2 core bodies. See grpc_h2.h.
// Only this TU touches nghttp2. Uses the non-deprecated *2 API (nghttp2 >= 1.62;
// vendored 1.68.0) and a counting allocator so the session footprint is
// measurable on the host.
#include "grpc_h2.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include <nghttp2/nghttp2.h>

namespace mwf_transport {
namespace {

constexpr int kGrpcOk = 0;
constexpr int kGrpcDeadlineExceeded = 4;
constexpr int kGrpcInternal = 13;
constexpr int kGrpcUnavailable = 14;

// Read chunk per pump. Small on purpose: the device recv buffer lives on the
// caller's stack frame in pumpRecv; 2 KB balances syscall count vs stack use.
constexpr size_t kRecvChunk = 2048;

// Hard cap on the accumulated unary response. A malicious / compromised / MITM
// server can stream unbounded DATA frames; without a ceiling that walks the
// device heap (or PSRAM) straight into exhaustion. 4 MB sits well above the
// largest expected nanopb response struct (Temporal histories are paginated far
// below this), so a legitimate reply never trips it.
constexpr size_t kMaxRespBytes = 4u * 1024u * 1024u;

// ── counting allocator (routes nghttp2's mem through H2MemStats) ─────────────
// Each block is prefixed with its size so free/realloc can decrement the
// counter. The prefix is max_align_t-sized to keep payload alignment.
union AllocHeader {
  size_t size;
  max_align_t align;
};

void* countedAlloc(size_t size, H2MemStats* st) {
  // Defense-in-depth: the header addition must not wrap size_t (a wrapped total
  // would under-allocate, then the caller writes `size` bytes out of bounds).
  if (size > SIZE_MAX - sizeof(AllocHeader)) return nullptr;
  auto* h = static_cast<AllocHeader*>(std::malloc(sizeof(AllocHeader) + size));
  if (!h) return nullptr;
  h->size = size;
  st->current += size;
  st->total += size;
  st->count++;
  if (st->current > st->peak) st->peak = st->current;
  return h + 1;
}

void countedFree(void* ptr, H2MemStats* st) {
  if (!ptr) return;
  auto* h = static_cast<AllocHeader*>(ptr) - 1;
  st->current -= h->size;
  std::free(h);
}

void* mem_malloc(size_t size, void* ud) {
  return countedAlloc(size, static_cast<H2MemStats*>(ud));
}
void mem_free(void* ptr, void* ud) { countedFree(ptr, static_cast<H2MemStats*>(ud)); }
void* mem_calloc(size_t nmemb, size_t size, void* ud) {
  // Guard nmemb*size against wrap before it under-allocates + memsets OOB.
  if (size != 0 && nmemb > SIZE_MAX / size) return nullptr;
  size_t n = nmemb * size;
  void* p = countedAlloc(n, static_cast<H2MemStats*>(ud));
  if (p) std::memset(p, 0, n);
  return p;
}
void* mem_realloc(void* ptr, size_t size, void* ud) {
  auto* st = static_cast<H2MemStats*>(ud);
  if (!ptr) return countedAlloc(size, st);
  // Same header-addition wrap guard as countedAlloc (the realloc path skips it).
  if (size > SIZE_MAX - sizeof(AllocHeader)) return nullptr;
  auto* h = static_cast<AllocHeader*>(ptr) - 1;
  size_t old = h->size;
  auto* nh = static_cast<AllocHeader*>(std::realloc(h, sizeof(AllocHeader) + size));
  if (!nh) return nullptr;
  nh->size = size;
  st->current += size - old;
  st->total += (size > old) ? (size - old) : 0;
  st->count++;
  if (st->current > st->peak) st->peak = st->current;
  return nh + 1;
}

// ── nv helpers ───────────────────────────────────────────────────────────────
nghttp2_nv makeNv(std::string_view name, std::string_view value) {
  nghttp2_nv nv;
  nv.name = reinterpret_cast<uint8_t*>(const_cast<char*>(name.data()));
  nv.namelen = name.size();
  nv.value = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data()));
  nv.valuelen = value.size();
  nv.flags = NGHTTP2_NV_FLAG_NONE;
  return nv;
}

std::string toLower(std::string s) {
  for (auto& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Is this gRPC method safe to blind-retry after an idle-close reconnect? Only
// the long-poll READS are: PollWorkflowTaskQueue / PollActivityTaskQueue have no
// server-side effect, so re-issuing one is harmless. Signal* / Respond* DO carry
// side effects — a processed-but-reply-lost call also surfaces as UNAVAILABLE
// with an empty body, so a blind retry would double-fire it (double signal,
// double task completion). We key off the method's final path segment starting
// with "Poll" (all poll RPCs are reads), which excludes every mutating method.
bool isIdempotentMethod(std::string_view fullMethod) {
  const size_t slash = fullMethod.rfind('/');
  const std::string_view leaf = (slash == std::string_view::npos)
                                    ? fullMethod
                                    : fullMethod.substr(slash + 1);
  return leaf.substr(0, 4) == "Poll";
}

}  // namespace

// grpc-message is percent-encoded per the gRPC HTTP/2 spec.
std::string GrpcH2Session::percentDecode(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size()) {
      int hi = hexVal(in[i + 1]), lo = hexVal(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out += in[i];
  }
  return out;
}

// ── nghttp2 callbacks (static, session user_data = GrpcH2Session*) ──────────
struct H2Callbacks {
  static int onHeader(nghttp2_session*, const nghttp2_frame* frame,
                      const uint8_t* name, size_t namelen, const uint8_t* value,
                      size_t valuelen, uint8_t, void* user_data) {
    auto* s = static_cast<GrpcH2Session*>(user_data);
    if (frame->hd.stream_id != s->stream_id_) return 0;
    std::string_view n(reinterpret_cast<const char*>(name), namelen);
    std::string_view v(reinterpret_cast<const char*>(value), valuelen);
    if (n == ":status") {
      s->http_status_ = std::atoi(std::string(v).c_str());
    } else if (n == "grpc-status") {
      s->grpc_status_ = std::atoi(std::string(v).c_str());
    } else if (n == "grpc-message") {
      s->grpc_message_ = GrpcH2Session::percentDecode(v);
    }
    return 0;
  }

  static int onDataChunk(nghttp2_session*, uint8_t, int32_t stream_id,
                         const uint8_t* data, size_t len, void* user_data) {
    auto* s = static_cast<GrpcH2Session*>(user_data);
    if (stream_id != s->stream_id_) return 0;
    // Bound the accumulated response so a server streaming unlimited DATA can't
    // exhaust the heap/PSRAM. The `size() > cap - len` form can't wrap (the
    // `len > cap` short-circuit rules out cap - len underflowing). On breach we
    // fail the session (owner sees UNAVAILABLE) and abort nghttp2's recv.
    if (len > kMaxRespBytes || s->resp_data_.size() > kMaxRespBytes - len) {
      s->fail("response exceeds max size");
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    s->resp_data_.insert(s->resp_data_.end(), data, data + len);
    return 0;
  }

  static int onStreamClose(nghttp2_session*, int32_t stream_id, uint32_t,
                           void* user_data) {
    auto* s = static_cast<GrpcH2Session*>(user_data);
    if (stream_id == s->stream_id_) s->stream_done_ = true;
    return 0;
  }

  static int onFrameRecv(nghttp2_session*, const nghttp2_frame* frame,
                         void* user_data) {
    auto* s = static_cast<GrpcH2Session*>(user_data);
    if (frame->hd.type == NGHTTP2_GOAWAY) {
      // The server is retiring this connection. Any stream above
      // last_stream_id will never complete; either way, don't reuse.
      s->alive_ = false;
      if (s->fail_reason_.empty()) s->fail_reason_ = "server GOAWAY";
    }
    return 0;
  }

  static nghttp2_ssize readRequest(nghttp2_session*, int32_t, uint8_t* buf,
                                   size_t length, uint32_t* data_flags,
                                   nghttp2_data_source*, void* user_data) {
    auto* s = static_cast<GrpcH2Session*>(user_data);
    size_t remain = s->req_frame_.size() - s->req_sent_;
    size_t n = remain < length ? remain : length;
    if (n) std::memcpy(buf, s->req_frame_.data() + s->req_sent_, n);
    s->req_sent_ += n;
    if (s->req_sent_ >= s->req_frame_.size()) *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return static_cast<nghttp2_ssize>(n);
  }
};

GrpcH2Session::GrpcH2Session() = default;
GrpcH2Session::~GrpcH2Session() { shutdown(); }

bool GrpcH2Session::begin(const H2IO& io, std::string authority,
                          std::string scheme) {
  shutdown();
  io_ = io;
  authority_ = std::move(authority);
  scheme_ = std::move(scheme);
  mem_stats_ = H2MemStats{};

  nghttp2_session_callbacks* cbs = nullptr;
  if (nghttp2_session_callbacks_new(&cbs) != 0) return false;
  nghttp2_session_callbacks_set_on_header_callback(cbs, H2Callbacks::onHeader);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
      cbs, H2Callbacks::onDataChunk);
  nghttp2_session_callbacks_set_on_stream_close_callback(
      cbs, H2Callbacks::onStreamClose);
  nghttp2_session_callbacks_set_on_frame_recv_callback(cbs,
                                                       H2Callbacks::onFrameRecv);

  // Counting allocator → H2MemStats (footprint report + PSRAM routing on the
  // device happens beneath malloc via heap_caps_malloc_extmem_enable).
  auto* mem = new nghttp2_mem{&mem_stats_, mem_malloc, mem_free, mem_calloc,
                              mem_realloc};
  mem_holder_ = mem;

  int rv = nghttp2_session_client_new3(&session_, cbs, this, nullptr, mem);
  nghttp2_session_callbacks_del(cbs);
  if (rv != 0) {
    session_ = nullptr;
    return false;
  }

  // Client preface rides the first send. SETTINGS: no server push; one call in
  // flight, so defaults are otherwise fine.
  nghttp2_settings_entry sv[] = {{NGHTTP2_SETTINGS_ENABLE_PUSH, 0}};
  if (nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, sv, 1) != 0) {
    shutdown();
    return false;
  }
  alive_ = true;
  fail_reason_.clear();
  return pumpSend();
}

void GrpcH2Session::fail(const char* why) {
  if (fail_reason_.empty()) fail_reason_ = why;
  alive_ = false;
}

bool GrpcH2Session::pumpSend() {
  const uint8_t* data = nullptr;
  for (;;) {
    nghttp2_ssize n = nghttp2_session_mem_send2(session_, &data);
    if (n < 0) {
      fail("nghttp2 mem_send failed");
      return false;
    }
    if (n == 0) return true;
    size_t off = 0;
    while (off < static_cast<size_t>(n)) {
      int w = io_.send(io_.ctx, data + off, static_cast<size_t>(n) - off);
      if (w <= 0) {
        fail("io send failed");
        return false;
      }
      off += static_cast<size_t>(w);
    }
  }
}

bool GrpcH2Session::pumpRecv(uint64_t deadlineAt) {
  uint8_t buf[kRecvChunk];
  int r = io_.recv(io_.ctx, buf, sizeof(buf));
  if (r < 0) {
    fail("io recv failed / connection closed");
    return false;
  }
  if (r == 0) {
    // No data in the io's short poll window; caller re-checks the deadline.
    (void)deadlineAt;
    return true;
  }
  nghttp2_ssize n = nghttp2_session_mem_recv2(session_, buf, static_cast<size_t>(r));
  if (n < 0 || static_cast<size_t>(n) != static_cast<size_t>(r)) {
    fail("nghttp2 mem_recv failed");
    return false;
  }
  return true;
}

mwf::GrpcResult GrpcH2Session::call(std::string_view fullMethod,
                                    const mwf::Bytes& requestMsg,
                                    const mwf::Metadata& metadata,
                                    int deadlineMs) {
  mwf::GrpcResult result;
  if (!alive_ || !session_) {
    result.grpc_status = kGrpcUnavailable;
    result.message = fail_reason_.empty() ? "session not started" : fail_reason_;
    return result;
  }

  // ── per-call state ─────────────────────────────────────────────────────────
  stream_done_ = false;
  http_status_ = 0;
  grpc_status_ = -1;
  grpc_message_.clear();
  resp_data_.clear();
  req_sent_ = 0;

  // gRPC length-prefixed message: 1-byte compressed flag (0) + 4-byte BE len.
  req_frame_.clear();
  req_frame_.reserve(5 + requestMsg.size());
  req_frame_.push_back(0);
  uint32_t len = static_cast<uint32_t>(requestMsg.size());
  req_frame_.push_back(static_cast<uint8_t>(len >> 24));
  req_frame_.push_back(static_cast<uint8_t>(len >> 16));
  req_frame_.push_back(static_cast<uint8_t>(len >> 8));
  req_frame_.push_back(static_cast<uint8_t>(len));
  req_frame_.insert(req_frame_.end(), requestMsg.begin(), requestMsg.end());

  // ── headers ────────────────────────────────────────────────────────────────
  // grpc-timeout in milliseconds ("<n>m", n ≤ 8 digits per spec).
  char timeout[16];
  std::snprintf(timeout, sizeof(timeout), "%dm", deadlineMs > 0 ? deadlineMs : 70000);
  const std::string method(fullMethod);

  std::vector<nghttp2_nv> nva;
  nva.reserve(8 + metadata.size());
  nva.push_back(makeNv(":method", "POST"));
  nva.push_back(makeNv(":scheme", scheme_));
  nva.push_back(makeNv(":path", method));
  nva.push_back(makeNv(":authority", authority_));
  nva.push_back(makeNv("te", "trailers"));
  nva.push_back(makeNv("content-type", "application/grpc"));
  nva.push_back(makeNv("grpc-timeout", timeout));
  nva.push_back(makeNv("user-agent", "grpc-mwf-h2/0.1"));
  // Caller metadata (authorization, temporal-namespace, …). h2 requires
  // lowercase field names; keep the lowered strings alive through submit.
  std::vector<std::pair<std::string, const std::string*>> lowered;
  lowered.reserve(metadata.size());
  for (const auto& kv : metadata) lowered.emplace_back(toLower(kv.first), &kv.second);
  for (const auto& kv : lowered) nva.push_back(makeNv(kv.first, *kv.second));

  nghttp2_data_provider2 prd{};
  prd.read_callback = H2Callbacks::readRequest;

  stream_id_ = nghttp2_submit_request2(session_, nullptr, nva.data(), nva.size(),
                                       &prd, nullptr);
  if (stream_id_ < 0) {
    fail("nghttp2 submit_request failed");
    result.grpc_status = kGrpcUnavailable;
    result.message = fail_reason_;
    return result;
  }

  // ── pump until trailers or deadline ────────────────────────────────────────
  const uint64_t start = io_.nowMs(io_.ctx);
  // Client-side enforcement backstop slightly beyond the header value so a
  // server that honours grpc-timeout wins the race and we see ITS status.
  const uint64_t deadlineAt =
      start + static_cast<uint64_t>(deadlineMs > 0 ? deadlineMs : 70000) + 2000;

  while (!stream_done_ && alive_) {
    if (!pumpSend()) break;
    if (io_.nowMs(io_.ctx) >= deadlineAt) {
      // Local deadline: cancel the stream but KEEP the session for reuse.
      nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, stream_id_,
                                NGHTTP2_CANCEL);
      pumpSend();
      stream_id_ = -1;
      result.grpc_status = kGrpcDeadlineExceeded;
      result.message = "deadline exceeded (client)";
      return result;
    }
    if (!pumpRecv(deadlineAt)) break;
  }

  if (!alive_) {
    result.grpc_status = kGrpcUnavailable;
    result.message = fail_reason_.empty() ? "connection lost" : fail_reason_;
    stream_id_ = -1;
    return result;
  }

  stream_id_ = -1;

  // ── map to GrpcResult ──────────────────────────────────────────────────────
  if (grpc_status_ >= 0) {
    result.grpc_status = grpc_status_;
    result.message = grpc_message_;
  } else if (http_status_ != 0 && http_status_ != 200) {
    result.grpc_status = kGrpcUnavailable;
    result.message = "http status " + std::to_string(http_status_);
    return result;
  } else {
    result.grpc_status = kGrpcInternal;
    result.message = "response missing grpc-status trailer";
    return result;
  }

  if (result.grpc_status == kGrpcOk) {
    // Unwrap the gRPC frame: flag + 4-byte BE length + message. A unary OK
    // response carries exactly one frame; an empty body (0 bytes) is legal
    // for an empty response message only via a 0-length frame — but some
    // frontends answer trailers-only OK with no DATA at all: treat as empty.
    std::string msg;
    const int st = unwrapUnaryFrame(resp_data_, result.response, msg);
    if (st != kGrpcOk) {
      result.grpc_status = st;
      result.message = msg;
    }
  }
  return result;
}

int GrpcH2Session::unwrapUnaryFrame(const mwf::Bytes& frame, mwf::Bytes& out,
                                    std::string& message) {
  if (frame.empty()) return kGrpcOk;  // trailers-only OK ⇒ empty message
  if (frame.size() < 5) {
    message = "short gRPC frame (" + std::to_string(frame.size()) + " bytes)";
    return kGrpcInternal;
  }
  if (frame[0] != 0) {
    message = "compressed gRPC frame unsupported";
    return kGrpcInternal;
  }
  const uint32_t mlen = (static_cast<uint32_t>(frame[1]) << 24) |
                        (static_cast<uint32_t>(frame[2]) << 16) |
                        (static_cast<uint32_t>(frame[3]) << 8) |
                        static_cast<uint32_t>(frame[4]);
  // mlen is 4 attacker-controlled bytes. NEVER add it to anything: `5 + mlen`
  // wraps (size_t is 32-bit on the MCU), so with mlen=0xFFFFFFFF the old
  // `size() < 5 + mlen` guard computed `size() < 4`, passed, and the assign()
  // below ran ~4 GB out of bounds. size() >= 5 is guaranteed here, so size()-5
  // can't underflow — compare the untrusted length against the room that
  // actually exists.
  if (mlen > frame.size() - 5) {
    message = "truncated gRPC frame";
    return kGrpcInternal;
  }
  out.assign(frame.begin() + 5, frame.begin() + 5 + mlen);
  return kGrpcOk;
}

void GrpcH2Session::shutdown() {
  if (session_) {
    // Best-effort GOAWAY so the server can clean up; ignore io failures.
    nghttp2_session_terminate_session(session_, NGHTTP2_NO_ERROR);
    pumpSend();
    nghttp2_session_del(session_);
    session_ = nullptr;
  }
  if (mem_holder_) {
    delete static_cast<nghttp2_mem*>(mem_holder_);
    mem_holder_ = nullptr;
  }
  alive_ = false;
}

// ── H2Transport: reconnect policy over the session ───────────────────────────
bool H2Transport::ensureSession(std::string& err) {
  if (connected_ && session_.alive()) return true;
  closeStream();
  connected_ = false;
  H2IO io;
  std::string authority, scheme;
  if (!openStream(io, authority, scheme, err)) return false;
  if (!session_.begin(io, std::move(authority), std::move(scheme))) {
    err = "h2 session begin failed";
    closeStream();
    return false;
  }
  connected_ = true;
  return true;
}

mwf::GrpcResult H2Transport::call(std::string_view fullMethod,
                                  const mwf::Bytes& requestMsg,
                                  const mwf::Metadata& metadata, int deadlineMs) {
  mwf::GrpcResult result;
  std::string err;
  const bool wasConnected = connected_ && session_.alive();
  if (!ensureSession(err)) {
    result.grpc_status = 14;  // UNAVAILABLE
    result.message = "connect failed: " + err;
    return result;
  }

  result = session_.call(fullMethod, requestMsg, metadata, deadlineMs);

  // A REUSED connection may have been idle-closed by the server between calls
  // (result: UNAVAILABLE with no response bytes). Reconnect once and retry —
  // but ONLY for idempotent (Poll*) methods. An UNAVAILABLE+empty result does
  // NOT prove the call went unprocessed: a processed-but-reply-lost Signal* /
  // Respond* lands here too, and a blind retry would double-fire the side
  // effect (duplicate signal, duplicate task completion). Never retry on a
  // fresh connect either.
  if (result.grpc_status == 14 && wasConnected && result.response.empty() &&
      isIdempotentMethod(fullMethod)) {
    if (ensureSession(err)) {
      result = session_.call(fullMethod, requestMsg, metadata, deadlineMs);
    }
  }
  return result;
}

void H2Transport::close() {
  session_.shutdown();
  closeStream();
  connected_ = false;
}

}  // namespace mwf_transport

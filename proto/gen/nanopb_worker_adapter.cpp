// proto: NanopbWorkerAdapter bodies. The only TU that touches the nanopb
// device-subset types. See nanopb_worker_adapter.h.
#include "nanopb_worker_adapter.h"

#include <cstring>
#include <memory>

#include <pb_decode.h>
#include <pb_encode.h>

#include "mwf/device_activity.pb.h"

namespace mwf_proto {
namespace {

constexpr int kTaskQueueKindNormal = 1;  // temporal.api.enums.v1 TASK_QUEUE_KIND_NORMAL

// Bounded C-string copy into a nanopb char[N] field. Over-long input is a
// caller bug on requests (identity/ns are config); returns false so encode
// paths can refuse instead of silently truncating.
template <size_t N>
bool setStr(char (&dst)[N], const std::string& src) {
  if (src.size() >= N) return false;
  std::memcpy(dst, src.data(), src.size());
  dst[src.size()] = '\0';
  return true;
}

// Bounded bytes copy into a nanopb PB_BYTES_ARRAY_T field.
template <class PbBytes>
bool setBytes(PbBytes& dst, const uint8_t* src, size_t n) {
  if (n > sizeof(dst.bytes)) return false;
  dst.size = static_cast<pb_size_t>(n);
  if (n) std::memcpy(dst.bytes, src, n);
  return true;
}

// Encode a nanopb message into mwf::Bytes (size pass + encode pass). Empty
// result = encode failure (over-cap field, etc.).
mwf::Bytes encodeMsg(const pb_msgdesc_t* fields, const void* msg) {
  size_t size = 0;
  if (!pb_get_encoded_size(&size, fields, msg)) return {};
  mwf::Bytes out(size);
  pb_ostream_t os = pb_ostream_from_buffer(out.data(), out.size());
  if (!pb_encode(&os, fields, msg)) return {};
  out.resize(os.bytes_written);
  return out;
}

// mwf::temporal::Payload → nanopb Payload. False when metadata count / key /
// value / data exceed the device caps.
bool fillPayload(mwf_device_v1_Payload& dst, const mwf::temporal::Payload& src) {
  if (src.metadata.size() > (sizeof(dst.metadata) / sizeof(dst.metadata[0])))
    return false;
  pb_size_t i = 0;
  for (const auto& kv : src.metadata) {
    auto& e = dst.metadata[i++];
    if (!setStr(e.key, kv.first)) return false;
    if (!setBytes(e.value, reinterpret_cast<const uint8_t*>(kv.second.data()),
                  kv.second.size()))
      return false;
  }
  dst.metadata_count = i;
  return setBytes(dst.data, src.data.data(), src.data.size());
}

}  // namespace

mwf::Bytes NanopbWorkerAdapter::buildPollRequest(const mwf_core::WorkerConfig& c) {
  mwf_device_v1_PollActivityTaskQueueRequest req =
      mwf_device_v1_PollActivityTaskQueueRequest_init_zero;
  if (!setStr(req.namespace_, c.ns) || !setStr(req.identity, c.identity) ||
      !setStr(req.task_queue.name, c.task_queue))
    return {};
  req.has_task_queue = true;
  req.task_queue.kind = kTaskQueueKindNormal;
  return encodeMsg(mwf_device_v1_PollActivityTaskQueueRequest_fields, &req);
}

bool NanopbWorkerAdapter::parsePollResponse(const mwf::Bytes& bytes,
                                            mwf_core::PolledActivityTask& out) {
  // ~34 KB at the device caps — heap (PSRAM on device), never the stack.
  auto resp = std::make_unique<mwf_device_v1_PollActivityTaskQueueResponse>();
  *resp = mwf_device_v1_PollActivityTaskQueueResponse_init_zero;

  pb_istream_t is = pb_istream_from_buffer(bytes.data(), bytes.size());
  if (!pb_decode(&is, mwf_device_v1_PollActivityTaskQueueResponse_fields,
                 resp.get()))
    return false;

  out = mwf_core::PolledActivityTask{};
  if (resp->task_token.size == 0) {
    out.has_task = false;  // taskless long-poll release (idle)
    return true;
  }
  out.has_task = true;
  out.task_token.assign(resp->task_token.bytes,
                        resp->task_token.bytes + resp->task_token.size);
  if (resp->has_activity_type) out.activity_name = resp->activity_type.name;
  if (resp->has_workflow_execution) {
    out.workflow_id = resp->workflow_execution.workflow_id;
    out.run_id = resp->workflow_execution.run_id;
  }
  if (resp->has_input) {
    for (pb_size_t i = 0; i < resp->input.payloads_count; ++i) {
      const auto& p = resp->input.payloads[i];
      mwf::temporal::Payload mp;
      for (pb_size_t m = 0; m < p.metadata_count; ++m) {
        const auto& e = p.metadata[m];
        mp.metadata[e.key] = std::string(
            reinterpret_cast<const char*>(e.value.bytes), e.value.size);
      }
      mp.data.assign(p.data.bytes, p.data.bytes + p.data.size);
      out.inputs.push_back(std::move(mp));
    }
  }
  return true;
}

mwf::Bytes NanopbWorkerAdapter::buildRespondCompleted(
    const mwf_core::WorkerConfig& c, const mwf::Bytes& task_token,
    const mwf::temporal::Payload& result) {
  auto req = std::make_unique<mwf_device_v1_RespondActivityTaskCompletedRequest>();
  *req = mwf_device_v1_RespondActivityTaskCompletedRequest_init_zero;

  if (!setBytes(req->task_token, task_token.data(), task_token.size()) ||
      !setStr(req->identity, c.identity) || !setStr(req->namespace_, c.ns))
    return {};
  req->has_result = true;
  req->result.payloads_count = 1;
  if (!fillPayload(req->result.payloads[0], result)) return {};
  return encodeMsg(mwf_device_v1_RespondActivityTaskCompletedRequest_fields,
                   req.get());
}

mwf::Bytes NanopbWorkerAdapter::buildRespondFailed(const mwf_core::WorkerConfig& c,
                                                   const mwf::Bytes& task_token,
                                                   const std::string& message) {
  auto req = std::make_unique<mwf_device_v1_RespondActivityTaskFailedRequest>();
  *req = mwf_device_v1_RespondActivityTaskFailedRequest_init_zero;

  if (!setBytes(req->task_token, task_token.data(), task_token.size()) ||
      !setStr(req->identity, c.identity) || !setStr(req->namespace_, c.ns))
    return {};
  req->has_failure = true;
  // Failure.message cap: truncate rather than refuse — an over-long error text
  // must not turn a clean activity failure into a transport failure.
  std::string msg = message.substr(0, sizeof(req->failure.message) - 1);
  setStr(req->failure.message, msg);
  setStr(req->failure.source, std::string("mwf-esp-worker"));
  req->failure.has_application_failure_info = true;
  setStr(req->failure.application_failure_info.type,
         std::string("MwfActivityFailure"));
  return encodeMsg(mwf_device_v1_RespondActivityTaskFailedRequest_fields,
                   req.get());
}

}  // namespace mwf_proto

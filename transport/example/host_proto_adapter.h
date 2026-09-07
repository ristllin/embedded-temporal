// transport example: HostProtoAdapter — the desktop (libprotobuf) twin of
// core's IWorkerProtoAdapter seam. Maps the loop's portable
// structs <-> the generated Temporal messages via proto's ProtoCodec.
// The device twin will do the same over nanopb — the loop is shared.
#pragma once

#include <string>

#include "mwf_core/worker_loop.h"
#include "proto_codec.h"  // proto/gen: ProtoCodec<Msg> + mwf::proto aliases
#include "temporal/api/enums/v1/task_queue.pb.h"

namespace mwf_example {

class HostProtoAdapter final : public mwf_core::IWorkerProtoAdapter {
 public:
  mwf::Bytes buildPollRequest(const mwf_core::WorkerConfig& c) override {
    mwf::proto::PollActivityTaskQueueRequest req;
    req.set_namespace_(c.ns);
    req.mutable_task_queue()->set_name(c.task_queue);
    req.mutable_task_queue()->set_kind(
        ::temporal::api::enums::v1::TASK_QUEUE_KIND_NORMAL);
    req.set_identity(c.identity);
    return poll_req_.encode(req);
  }

  bool parsePollResponse(const mwf::Bytes& bytes,
                         mwf_core::PolledActivityTask& out) override {
    mwf::proto::PollActivityTaskQueueResponse resp;
    if (!poll_resp_.decode(bytes, resp)) return false;
    if (resp.task_token().empty()) {
      out.has_task = false;  // empty poll response body = no task
      return true;
    }
    out.has_task = true;
    out.task_token.assign(resp.task_token().begin(), resp.task_token().end());
    out.activity_name = resp.activity_type().name();
    out.workflow_id = resp.workflow_execution().workflow_id();
    out.run_id = resp.workflow_execution().run_id();
    for (const auto& p : resp.input().payloads()) {
      mwf::temporal::Payload mp;
      for (const auto& kv : p.metadata()) mp.metadata[kv.first] = kv.second;
      mp.data.assign(p.data().begin(), p.data().end());
      out.inputs.push_back(std::move(mp));
    }
    return true;
  }

  mwf::Bytes buildRespondCompleted(const mwf_core::WorkerConfig& c,
                                   const mwf::Bytes& task_token,
                                   const mwf::temporal::Payload& result) override {
    mwf::proto::RespondActivityTaskCompletedReq req;
    req.set_task_token(task_token.data(), task_token.size());
    req.set_identity(c.identity);
    req.set_namespace_(c.ns);
    auto* p = req.mutable_result()->add_payloads();
    for (const auto& kv : result.metadata)
      (*p->mutable_metadata())[kv.first] = kv.second;
    p->set_data(result.data.data(), result.data.size());
    return completed_.encode(req);
  }

  mwf::Bytes buildRespondFailed(const mwf_core::WorkerConfig& c,
                                const mwf::Bytes& task_token,
                                const std::string& message) override {
    mwf::proto::RespondActivityTaskFailedReq req;
    req.set_task_token(task_token.data(), task_token.size());
    req.set_identity(c.identity);
    req.set_namespace_(c.ns);
    auto* f = req.mutable_failure();
    f->set_message(message);
    f->set_source("mwf-cpp-worker");
    // ApplicationFailureInfo so SDK-side callers see a typed ApplicationError.
    f->mutable_application_failure_info()->set_type("MwfActivityFailure");
    return failed_.encode(req);
  }

 private:
  mwf::ProtoCodec<mwf::proto::PollActivityTaskQueueRequest> poll_req_;
  mwf::ProtoCodec<mwf::proto::PollActivityTaskQueueResponse> poll_resp_;
  mwf::ProtoCodec<mwf::proto::RespondActivityTaskCompletedReq> completed_;
  mwf::ProtoCodec<mwf::proto::RespondActivityTaskFailedReq> failed_;
};

}  // namespace mwf_example

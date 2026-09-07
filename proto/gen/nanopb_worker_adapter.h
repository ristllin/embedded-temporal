// proto: NanopbWorkerAdapter — the DEVICE twin of core's
// IWorkerProtoAdapter seam. Maps the worker loop's portable structs ⇄ the
// nanopb device-subset messages (gen/nanopb-device, from
// proto/mwf/device_activity.proto — a wire-identical trim of the temporal.api
// activity-worker messages; see that file's header for why a mirror).
//
// Portable C++17, no Arduino: host-compiled by the cross-lib conformance test
// (test/test_nanopb_device_conformance.cpp — nanopb encode → libprotobuf
// decode of the REAL temporal messages, and back) and device-compiled by
// firmware. All nanopb types stay in the .cpp; the big response/request
// structs (~34 KB at the device caps) are HEAP-allocated per call — on the
// device that heap is PSRAM (heap_caps_malloc_extmem_enable), never
// stack/statics.
#pragma once

#include "mwf_core/worker_loop.h"

namespace mwf_proto {

class NanopbWorkerAdapter final : public mwf_core::IWorkerProtoAdapter {
 public:
  // PollActivityTaskQueueRequest{namespace, task_queue{name, NORMAL}, identity}.
  mwf::Bytes buildPollRequest(const mwf_core::WorkerConfig&) override;

  // PollActivityTaskQueueResponse bytes → PolledActivityTask. false on decode
  // failure (incl. a payload above the device caps — ProtocolError upstream);
  // an empty/taskless response is success with has_task=false.
  bool parsePollResponse(const mwf::Bytes&, mwf_core::PolledActivityTask&) override;

  // RespondActivityTaskCompletedRequest{task_token, result{[payload]}, identity,
  // namespace}. Truncates nothing: over-cap data returns an empty Bytes (caller
  // sees an encode failure and fails the task cleanly).
  mwf::Bytes buildRespondCompleted(const mwf_core::WorkerConfig&,
                                   const mwf::Bytes& task_token,
                                   const mwf::temporal::Payload& result) override;

  // RespondActivityTaskFailedRequest{task_token, failure{message, source,
  // application_failure_info{type}}, identity, namespace}.
  mwf::Bytes buildRespondFailed(const mwf_core::WorkerConfig&,
                                const mwf::Bytes& task_token,
                                const std::string& message) override;
};

}  // namespace mwf_proto

// EmbeddedTemporal.h — umbrella header for the embedded-temporal library.
//
// One include gives you the full device activity-worker stack:
//   mwf::            frozen contracts (ITransport, IPayloadCodec, IActivityRegistry, ...)
//   mwf_core::       ActivityRegistry + WorkerLoop (the portable poll loop)
//   mwf_codec::      PayloadCodecV1 (Mistral WorkflowContext envelope codec)
//   mwf_proto::      NanopbWorkerAdapter (temporal.api wire subset via nanopb)
//   mwf_transport::  EspTransport (gRPC-over-HTTP/2 on WiFiClientSecure)
//
// Generated from the embedded-temporal monorepo by tools/release/build_lib.sh;
// see https://github.com/ristllin/embedded-temporal for sources and tests.
#pragma once

#include "mwf/contracts.h"
#include "mwf_core/activity_registry.h"
#include "mwf_core/worker_loop.h"
#include "mwf_codec/payload_codec.h"
#include "nanopb_worker_adapter.h"
#include "esp_transport.h"

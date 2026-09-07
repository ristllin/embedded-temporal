# proto — Temporal proto subset → C++ wire codecs.
#   host  : libprotobuf twin  (mwf_proto_host)          — realizes IProtoCodec<Msg>
#   device: nanopb twin       (mwf_proto_nanopb_smoke)  — host-compiles the generated C
#
# Run ./gen_host.sh (and ./gen_nanopb.sh) BEFORE configuring — this file compiles
# the already-generated sources under gen/. gen/ is .gitignore'd (regenerated).
cmake_minimum_required(VERSION 3.16)
project(mwf_proto LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# protobuf 35 (brew) is Abseil-based: use its CMake package config so the
# protobuf::libprotobuf imported target carries the transitive absl include/link
# deps (the legacy FindProtobuf module misses them → 'absl/...' not found).
find_package(protobuf CONFIG REQUIRED)

set(MWF_GEN_HOST   ${CMAKE_CURRENT_SOURCE_DIR}/gen/host)
set(MWF_GEN_NANOPB ${CMAKE_CURRENT_SOURCE_DIR}/gen/nanopb)
set(MWF_CONTRACTS  ${CMAKE_CURRENT_SOURCE_DIR}/../contracts/include)

# ── Host twin: libprotobuf ────────────────────────────────────────────────────
file(GLOB_RECURSE MWF_HOST_PB_CC CONFIGURE_DEPENDS ${MWF_GEN_HOST}/*.pb.cc)
if(NOT MWF_HOST_PB_CC)
  message(FATAL_ERROR "No generated .pb.cc under ${MWF_GEN_HOST}. Run ./gen_host.sh first.")
endif()

add_library(mwf_proto_host STATIC ${MWF_HOST_PB_CC})
target_include_directories(mwf_proto_host PUBLIC
  ${MWF_GEN_HOST}
  ${CMAKE_CURRENT_SOURCE_DIR}/gen        # proto_codec.h adapter
  ${MWF_CONTRACTS})
target_link_libraries(mwf_proto_host PUBLIC protobuf::libprotobuf)
# Generated Temporal code is not warning-clean under -Wall; keep our own warnings
# but don't fail the vendored generated TU.
target_compile_options(mwf_proto_host PRIVATE -w)

# ── Round-trip conformance test (host) ────────────────────────────────────────
enable_testing()
add_executable(test_roundtrip test/test_roundtrip.cpp)
target_link_libraries(test_roundtrip PRIVATE mwf_proto_host)
add_test(NAME roundtrip COMMAND test_roundtrip)

# ── Device twin: nanopb host-compile smoke ────────────────────────────────────
# Proves the generated nanopb .c/.h build against the nanopb runtime on the host.
# (Cannot link an ESP32 image here — this is a compile/link smoke, not a device run.)
set(MWF_NANOPB_RT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanopb)
if(EXISTS ${MWF_GEN_NANOPB} AND EXISTS ${MWF_NANOPB_RT}/pb_encode.c)
  file(GLOB_RECURSE MWF_NANOPB_PB_C CONFIGURE_DEPENDS ${MWF_GEN_NANOPB}/*.pb.c)
  add_executable(mwf_proto_nanopb_smoke
    test/nanopb_smoke.c
    ${MWF_NANOPB_PB_C}
    ${MWF_NANOPB_RT}/pb_encode.c
    ${MWF_NANOPB_RT}/pb_decode.c
    ${MWF_NANOPB_RT}/pb_common.c)
  target_include_directories(mwf_proto_nanopb_smoke PRIVATE
    ${MWF_NANOPB_RT} ${MWF_GEN_NANOPB})
  # The vendored Temporal closure contains a few very large aggregate messages
  # (history MarkerRecorded etc.); PB_FIELD_32BIT lifts nanopb's 16-bit field
  # offset/size ceiling so the generated descriptors for those still compile.
  # A real device build would trim to the workflow-task subset and drop this.
  target_compile_definitions(mwf_proto_nanopb_smoke PRIVATE PB_FIELD_32BIT)
  add_test(NAME nanopb_smoke COMMAND mwf_proto_nanopb_smoke)
else()
  message(STATUS "proto: nanopb twin skipped (run ./gen_nanopb.sh; needs third_party/nanopb runtime)")
endif()

# ── Device-subset cross-lib conformance ────────────────────────────────────────
# The nanopb worker adapter (gen/nanopb-device, the trim the ESP32 links)
# encodes; the REAL temporal.api messages (libprotobuf) decode — and back.
# Proves the hand-mirrored device_activity.proto is wire-identical.
set(MWF_GEN_NANOPB_DEV ${CMAKE_CURRENT_SOURCE_DIR}/gen/nanopb-device)
set(MWF_CORE_INC ${CMAKE_CURRENT_SOURCE_DIR}/../core/include)
if(EXISTS ${MWF_GEN_NANOPB_DEV}/mwf/device_activity.pb.c
   AND EXISTS ${MWF_NANOPB_RT}/pb_encode.c AND EXISTS ${MWF_CORE_INC})
  add_executable(test_nanopb_device_conformance
    test/test_nanopb_device_conformance.cpp
    gen/nanopb_worker_adapter.cpp
    ${MWF_GEN_NANOPB_DEV}/mwf/device_activity.pb.c
    ${MWF_NANOPB_RT}/pb_encode.c
    ${MWF_NANOPB_RT}/pb_decode.c
    ${MWF_NANOPB_RT}/pb_common.c)
  target_include_directories(test_nanopb_device_conformance PRIVATE
    ${MWF_NANOPB_RT} ${MWF_GEN_NANOPB_DEV} ${MWF_CORE_INC})
  target_link_libraries(test_nanopb_device_conformance PRIVATE mwf_proto_host)
  add_test(NAME nanopb_device_conformance COMMAND test_nanopb_device_conformance)
else()
  message(STATUS "proto: device conformance skipped (need gen/nanopb-device + ../core)")
endif()

# ── Device WORKFLOW-subset cross-lib conformance ──────────────────────────────
# The nanopb device_workflow types decode REAL conformance golden histories (packed
# into a PollWorkflowTaskQueueResponse via libprotobuf) and encode commands that
# libprotobuf re-decodes. Proves proto/mwf/device_workflow.proto is wire-identical
# to temporal.api. ⚠ A v1 History (64 events × ~40 KB Payloads) is > 64 KB, so the
# whole target (incl. the imported device_activity.pb.c + the nanopb runtime) is
# built with PB_FIELD_32BIT — REQUIRED for the workflow subset (the activity
# subset above stays 16-bit). The golden histories live in ../conformance;
# the round-trip is skipped (with a printed note) if that repo is absent.
set(MWF_GOLDENS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../conformance/goldens/histories)
if(EXISTS ${MWF_GEN_NANOPB_DEV}/mwf/device_workflow.pb.c
   AND EXISTS ${MWF_NANOPB_RT}/pb_encode.c)
  add_executable(test_nanopb_device_workflow_conformance
    test/test_nanopb_device_workflow_conformance.cpp
    gen/nanopb_workflow_adapter.cpp   # host-compile the DEVICE workflow adapter
    ${MWF_GEN_NANOPB_DEV}/mwf/device_activity.pb.c
    ${MWF_GEN_NANOPB_DEV}/mwf/device_workflow.pb.c
    ${MWF_NANOPB_RT}/pb_encode.c
    ${MWF_NANOPB_RT}/pb_decode.c
    ${MWF_NANOPB_RT}/pb_common.c)
  target_include_directories(test_nanopb_device_workflow_conformance PRIVATE
    ${MWF_NANOPB_RT} ${MWF_GEN_NANOPB_DEV}
    ${CMAKE_CURRENT_SOURCE_DIR}/gen           # nanopb_workflow_adapter.h
    ${MWF_CORE_INC} ${MWF_CONTRACTS})         # mwf_core/* + mwf/* (SignalRequest)
  target_compile_definitions(test_nanopb_device_workflow_conformance PRIVATE
    PB_FIELD_32BIT)
  if(EXISTS ${MWF_GOLDENS_DIR}/linear.json)
    target_compile_definitions(test_nanopb_device_workflow_conformance PRIVATE
      MWF_GOLDENS_DIR="${MWF_GOLDENS_DIR}")
  else()
    message(STATUS "proto: workflow goldens absent — round-trip test runs without them")
  endif()
  # REAL captured Mistral history (proto-JSON) — the field-overflow regression
  # that golden-only conformance missed (goldens use short synthetic ids; real
  # data carries a 64-hex-char workflow_id). Lives in ../conformance/probe.
  set(MWF_PROBE_JSON ${CMAKE_CURRENT_SOURCE_DIR}/../conformance/probe/workflow_task_1.json)
  if(EXISTS ${MWF_PROBE_JSON})
    target_compile_definitions(test_nanopb_device_workflow_conformance PRIVATE
      MWF_PROBE_JSON="${MWF_PROBE_JSON}")
  else()
    message(STATUS "proto: real-history probe absent — overflow regression skipped")
  endif()
  target_link_libraries(test_nanopb_device_workflow_conformance PRIVATE mwf_proto_host)
  add_test(NAME nanopb_device_workflow_conformance
    COMMAND test_nanopb_device_workflow_conformance)
else()
  message(STATUS "proto: workflow conformance skipped (run ./gen_nanopb.sh)")
endif()

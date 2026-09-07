# transport — the ITransport impls.
# Desktop target here: grpc++ generic-stub gRPC twin + the Mistral REST
# control-plane client (libcurl). The ESP32 nghttp2+mbedTLS impl lives under
# esp/ (PlatformIO, built by firmware — no CMake build here).
cmake_minimum_required(VERSION 3.16)
project(mwf_transport LANGUAGES C CXX)  # C: vendored nghttp2 (esp/third_party)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# brew grpc pulls protobuf too; both expected on the host toolchain.
# CONFIG mode for protobuf (not the legacy FindProtobuf module): grpc's
# find_dependency(Protobuf) is CONFIG, and mixing the two double-defines targets.
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(CURL REQUIRED)        # macOS SDK libcurl / brew curl

# ── the transport library ─────────────────────────────────────────────────────
add_library(mwf_transport_desktop STATIC
  src/desktop_transport.cpp
  src/rest_client.cpp)

target_include_directories(mwf_transport_desktop PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../contracts/include)

# gRPC::grpc++ (generic stub) for the data plane; CURL for the REST control
# plane. protobuf is transitively linked via gRPC but named for clarity.
target_link_libraries(mwf_transport_desktop PUBLIC
  gRPC::grpc++
  protobuf::libprotobuf
  CURL::libcurl)

# ── examples / live proofs ────────────────────────────────────────────────────
# whoami: live proof against the real Mistral control plane.
add_executable(whoami example/whoami_main.cpp)
target_link_libraries(whoami PRIVATE mwf_transport_desktop)

# transport_probe: real unary gRPC call against a local Temporal dev server.
add_executable(transport_probe example/transport_probe_main.cpp)
target_link_libraries(transport_probe PRIVATE mwf_transport_desktop)

message(STATUS "transport: desktop target + whoami/transport_probe examples")

# ── esp/grpc_h2 host twin (vendored nghttp2 + OpenSSL stream) ────────────────
# The SAME grpc_h2.cpp the ESP32 compiles, driven here over a POSIX/OpenSSL
# stream. Deliberately built against the VENDORED nghttp2 (esp/third_party),
# not brew's, so the host proof covers the exact sources the device links.
find_package(OpenSSL REQUIRED)

file(GLOB MWF_NGHTTP2_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/esp/third_party/nghttp2/lib/*.c)
add_library(mwf_nghttp2_vendored STATIC ${MWF_NGHTTP2_SOURCES})
target_include_directories(mwf_nghttp2_vendored PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/esp/third_party/nghttp2/lib/includes
  ${CMAKE_CURRENT_SOURCE_DIR}/esp/third_party/nghttp2)
target_compile_definitions(mwf_nghttp2_vendored PRIVATE HAVE_CONFIG_H NGHTTP2_STATICLIB)
target_compile_definitions(mwf_nghttp2_vendored INTERFACE NGHTTP2_STATICLIB)

add_library(mwf_transport_h2_host STATIC
  esp/grpc_h2.cpp
  esp/host/h2_host_stream.cpp)
target_include_directories(mwf_transport_h2_host PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/esp
  ${CMAKE_CURRENT_SOURCE_DIR}/../contracts/include)
target_link_libraries(mwf_transport_h2_host PUBLIC
  mwf_nghttp2_vendored
  OpenSSL::SSL OpenSSL::Crypto)

# h2_probe: framing proof — h2 stack vs DesktopTransport byte-wise, + live mode.
add_executable(h2_probe example/h2_probe_main.cpp)
target_link_libraries(h2_probe PRIVATE mwf_transport_h2_host mwf_transport_desktop)

# h2_security_test: server-free host unit checks for the transport/JSON
# security hardening — gRPC frame-length overflow (the 0xFFFFFFFF case), frame
# parse edge cases, and the json_util deep-nesting guard. Compiles the SAME
# grpc_h2 core and core/json_util.cpp the device links; registered as a ctest
# so a regression fails the build, not just the runtime.
enable_testing()
find_package(nlohmann_json REQUIRED)
add_executable(h2_security_test
  esp/host/h2_security_test.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../core/src/json_util.cpp)
target_include_directories(h2_security_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../contracts/include)
target_compile_definitions(h2_security_test PRIVATE MWF_JSON_NLOHMANN)
target_link_libraries(h2_security_test PRIVATE
  mwf_transport_h2_host
  nlohmann_json::nlohmann_json)
add_test(NAME h2_security_test COMMAND h2_security_test)

# desktop_tls_test: server-free checks that DesktopTransport is TLS-on by
# default and refuses to send bearer credentials over an insecure channel.
add_executable(desktop_tls_test test/desktop_tls_test.cpp)
target_link_libraries(desktop_tls_test PRIVATE mwf_transport_desktop)
add_test(NAME desktop_tls_test COMMAND desktop_tls_test)

# ── the desktop C++ activity worker (mwf_worker) ──────────────────────────────
# Wires DesktopTransport + proto's host codec + codec's PayloadCodecV1 +
# core's WorkerLoop/ActivityRegistry into one binary. Sibling repos are
# consumed via their public seams: proto as a subdirectory (its
# mwf_proto_host target needs gen/host — run ../proto/gen_host.sh first),
# core/codec as sources (the three TUs used are JSON-lib-free, so no
# nlohmann dependency leaks in here).
set(MWF_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../proto)
set(MWF_CORE_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../core)
set(MWF_CODEC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../codec)

if(EXISTS ${MWF_PROTO_DIR}/gen/host AND EXISTS ${MWF_CORE_DIR}/src/worker_loop.cpp
   AND EXISTS ${MWF_CODEC_DIR}/src/payload_codec.cpp)
  if(NOT TARGET mwf_proto_host)
    add_subdirectory(${MWF_PROTO_DIR} ${CMAKE_BINARY_DIR}/proto)
  endif()

  add_executable(mwf_worker
    example/worker_main.cpp
    ${MWF_CORE_DIR}/src/worker_loop.cpp
    ${MWF_CORE_DIR}/src/activity_registry.cpp
    ${MWF_CODEC_DIR}/src/payload_codec.cpp)
  target_include_directories(mwf_worker PRIVATE
    ${MWF_CORE_DIR}/include
    ${MWF_CODEC_DIR}/include)
  target_link_libraries(mwf_worker PRIVATE
    mwf_transport_desktop
    mwf_proto_host)
  message(STATUS "transport: mwf_worker assembly enabled")

  # mwf_worker_h2 — the SAME worker over the esp/grpc_h2 framing core
  # (host-twin proof: run the desktop_worker_e2e gate with this binary
  # instead of mwf_worker).
  add_executable(mwf_worker_h2
    example/worker_main.cpp
    ${MWF_CORE_DIR}/src/worker_loop.cpp
    ${MWF_CORE_DIR}/src/activity_registry.cpp
    ${MWF_CODEC_DIR}/src/payload_codec.cpp)
  target_compile_definitions(mwf_worker_h2 PRIVATE MWF_USE_H2_TRANSPORT)
  target_include_directories(mwf_worker_h2 PRIVATE
    ${MWF_CORE_DIR}/include
    ${MWF_CODEC_DIR}/include)
  target_link_libraries(mwf_worker_h2 PRIVATE
    mwf_transport_h2_host
    mwf_transport_desktop   # RestClient (whoami) still rides the desktop lib
    mwf_proto_host)

  # ── the desktop SOLE-WORKER (mwf_sole_worker) ───────────────────────────────
  # Runs BOTH the WorkflowLoop AND the WorkerLoop on ONE queue, one thread — the
  # device orchestrates the whole Mistral workflow itself, no Python host. Pulls
  # in the JSON-backed replay engine, so this target (unlike mwf_worker) needs
  # nlohmann + MWF_JSON_NLOHMANN. Payload codec + activity/worker TUs stay
  # JSON-lib-free; only the workflow-side TUs touch mwf::json.
  find_package(nlohmann_json REQUIRED)
  add_executable(mwf_sole_worker
    example/sole_worker_main.cpp
    ${MWF_CORE_DIR}/src/worker_loop.cpp
    ${MWF_CORE_DIR}/src/workflow_loop.cpp
    ${MWF_CORE_DIR}/src/replay_engine.cpp
    ${MWF_CORE_DIR}/src/workflow_spec.cpp
    ${MWF_CORE_DIR}/src/history.cpp
    ${MWF_CORE_DIR}/src/json_util.cpp
    ${MWF_CORE_DIR}/src/activity_registry.cpp
    ${MWF_CODEC_DIR}/src/payload_codec.cpp)
  target_compile_definitions(mwf_sole_worker PRIVATE MWF_JSON_NLOHMANN)
  target_include_directories(mwf_sole_worker PRIVATE
    ${MWF_CORE_DIR}/include
    ${MWF_CODEC_DIR}/include)
  target_link_libraries(mwf_sole_worker PRIVATE
    mwf_transport_desktop
    mwf_proto_host
    nlohmann_json::nlohmann_json)
  message(STATUS "transport: mwf_sole_worker assembly enabled")

  # ── cookbooks: the GENERIC desktop cookbook worker (cookbook_worker) ─────────
  # Same sole-worker mechanics (both loops, one queue) but the WorkflowSpec is
  # loaded from disk (--spec) and a FIXED cookbook activity set (echo / ai.chat /
  # ai.classify) is registered — the shared engine behind cookbooks/01..03.
  # ai.chat/ai.classify call Mistral /v1/chat/completions via libcurl (already
  # linked transitively through mwf_transport_desktop's CURL::libcurl).
  add_executable(cookbook_worker
    example/cookbook_worker_main.cpp
    ${MWF_CORE_DIR}/src/worker_loop.cpp
    ${MWF_CORE_DIR}/src/workflow_loop.cpp
    ${MWF_CORE_DIR}/src/replay_engine.cpp
    ${MWF_CORE_DIR}/src/workflow_spec.cpp
    ${MWF_CORE_DIR}/src/history.cpp
    ${MWF_CORE_DIR}/src/json_util.cpp
    ${MWF_CORE_DIR}/src/activity_registry.cpp
    ${MWF_CODEC_DIR}/src/payload_codec.cpp)
  target_compile_definitions(cookbook_worker PRIVATE MWF_JSON_NLOHMANN)
  target_include_directories(cookbook_worker PRIVATE
    ${MWF_CORE_DIR}/include
    ${MWF_CODEC_DIR}/include)
  target_link_libraries(cookbook_worker PRIVATE
    mwf_transport_desktop
    mwf_proto_host
    nlohmann_json::nlohmann_json)
  message(STATUS "transport: cookbook_worker assembly enabled")
else()
  message(STATUS "transport: mwf_worker skipped (need ../proto/gen/host + ../core + ../codec)")
endif()

# core — portable C++17 library: worker loop, client ops, replay engine,
# registries, retry. Portable (no Arduino) and host-tested; the device build
# compiles the same sources through PlatformIO.
cmake_minimum_required(VERSION 3.16)
project(mwf_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Host JSON backing for mwf::json.
find_package(nlohmann_json REQUIRED)

add_library(mwf_core STATIC
  src/worker_loop.cpp
  src/workflow_loop.cpp
  src/client_ops.cpp
  src/json_util.cpp
  src/workflow_spec.cpp
  src/history.cpp
  src/replay_engine.cpp
  src/activity_registry.cpp
  src/retry.cpp)

target_include_directories(mwf_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../contracts/include
  # proto generated headers (the IProtoCodec twin, from proto/gen_host.sh).
  ${CMAKE_CURRENT_SOURCE_DIR}/../proto/gen)

target_compile_definitions(mwf_core PUBLIC MWF_JSON_NLOHMANN)
target_link_libraries(mwf_core PUBLIC nlohmann_json::nlohmann_json)

enable_testing()
add_subdirectory(test)

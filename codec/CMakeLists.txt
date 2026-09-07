# codec — C++17 library implementing mwf::IPayloadCodec: the Mistral
# WorkflowContext envelope codec, host-built and host-tested.
cmake_minimum_required(VERSION 3.16)
project(mwf_codec LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Host JSON lib — the contracts' mwf::json aliases to nlohmann/json on host.
find_package(nlohmann_json REQUIRED)

add_library(mwf_codec STATIC src/payload_codec.cpp)
target_include_directories(mwf_codec PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../contracts/include)
target_compile_definitions(mwf_codec PUBLIC MWF_JSON_NLOHMANN)
target_link_libraries(mwf_codec PUBLIC nlohmann_json::nlohmann_json)

enable_testing()
add_subdirectory(test)

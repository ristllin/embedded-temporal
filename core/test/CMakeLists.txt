# core/test — host unit tests. gtest resolved via find_package
# (same approach as codec: the conda toolchain provides GTest + nlohmann;
# brew's build has an ABI mismatch with this compiler).

find_package(GTest REQUIRED)

add_executable(test_mwf_core
  test_workflow_spec.cpp
  test_history.cpp
  test_replay_engine.cpp
  test_wait_signal.cpp
  test_replay_bugfixes.cpp
  test_client_ops.cpp
  test_activity_registry.cpp
  test_worker_loop.cpp
  test_workflow_loop.cpp
  test_workflow_loop_waiting.cpp
  # The payload codec is compiled in so the workflow-loop tests exercise the
  # REAL PayloadCodecV1 (wf_v1 envelope) on the outbound path, not a fake.
  ${CMAKE_CURRENT_SOURCE_DIR}/../../codec/src/payload_codec.cpp)
target_include_directories(test_mwf_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../codec/include)
target_link_libraries(test_mwf_core PRIVATE
  mwf_core
  GTest::gtest_main)

# Point the golden suite at conformance's recorded Temporal histories.
target_compile_definitions(test_mwf_core PRIVATE
  MWF_HISTORIES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../conformance/goldens/histories")

# If GTest resolved to a shared lib (e.g. a conda install), bake its directory
# into the test binary's rpath so it loads at runtime without DYLD_LIBRARY_PATH.
get_target_property(_gtest_loc GTest::gtest LOCATION)
if(_gtest_loc)
  get_filename_component(_gtest_libdir "${_gtest_loc}" DIRECTORY)
  set_target_properties(test_mwf_core PROPERTIES
    BUILD_RPATH "${_gtest_libdir}")
endif()

add_test(NAME test_mwf_core COMMAND test_mwf_core)

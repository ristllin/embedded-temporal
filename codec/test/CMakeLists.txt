# codec/test — host unit tests for PayloadCodecV1.
find_package(GTest REQUIRED)

add_executable(test_payload_codec
  test_payload_codec.cpp
  test_goldens.cpp)
target_link_libraries(test_payload_codec PRIVATE
  mwf_codec
  GTest::gtest_main
  nlohmann_json::nlohmann_json)

# Point the golden suite at conformance's capture dir (self-skips when empty).
target_compile_definitions(test_payload_codec PRIVATE
  MWF_GOLDENS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../conformance/goldens/payloads")

# If GTest resolved to a shared lib (e.g. a conda install), bake its directory
# into the test binary's rpath so it loads at runtime without DYLD_LIBRARY_PATH.
get_target_property(_gtest_loc GTest::gtest LOCATION)
if(_gtest_loc)
  get_filename_component(_gtest_libdir "${_gtest_loc}" DIRECTORY)
  set_target_properties(test_payload_codec PROPERTIES
    BUILD_RPATH "${_gtest_libdir}")
endif()

add_test(NAME test_payload_codec COMMAND test_payload_codec)

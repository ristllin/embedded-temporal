// contracts: shared portable types. C++17, no Arduino, no exceptions across seams.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// JSON alias: both host AND device builds define MWF_JSON_NLOHMANN today (the
// ESP32 build vendors nlohmann — see docs/esp32-compatibility.md). The
// MWF_JSON_ARDUINO branch is an unused, untested escape hatch for a future
// ArduinoJson port. Codec/core touch ONLY mwf::json, never the concrete lib type.
#if defined(MWF_JSON_NLOHMANN)
  #include <nlohmann/json.hpp>
namespace mwf { using json = nlohmann::json; }
#elif defined(MWF_JSON_ARDUINO)
  #include <ArduinoJson.h>
namespace mwf { using json = ArduinoJson::JsonDocument; }
#else
  // No JSON lib selected yet (e.g. pure interface consumers). Forward-declare a tag.
namespace mwf { struct json; }
#endif

namespace mwf {

using Bytes    = std::vector<uint8_t>;
using Metadata = std::map<std::string, std::string>;  // gRPC/HTTP-2 headers, string values

// Minimal Result: ok(value) or err(message). No exceptions across the seam.
template <class T>
struct Result {
  bool ok = false;
  T value{};
  std::string error;
  static Result success(T v) { return Result{true, std::move(v), {}}; }
  static Result failure(std::string e) { return Result{false, {}, std::move(e)}; }
  explicit operator bool() const { return ok; }
};

// Specialization: Result<void> — ok/error only, no value member.
template <>
struct Result<void> {
  bool ok = false;
  std::string error;
  static Result success() { return Result{true, {}}; }
  static Result failure(std::string e) { return Result{false, std::move(e)}; }
  explicit operator bool() const { return ok; }
};

constexpr const char* CONTRACTS_VERSION = "0.1.1";

}  // namespace mwf

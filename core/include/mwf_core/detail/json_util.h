// core: the ONE json-library seam.
//
// Every place core parses, serializes, or pointer-walks JSON goes through
// these helpers and nothing else — so swapping the concrete library under the
// mwf::json alias (nlohmann on host, ArduinoJson on device) touches ONLY
// src/json_util.cpp. No exceptions escape (parse errors -> Result failure).
#pragma once
#include <string>
#include <string_view>
#include "mwf/types.h"

namespace mwf_core {
namespace jsonutil {

// Parse UTF-8 JSON text. Failure carries a short reason, never throws.
mwf::Result<mwf::json> parse(std::string_view text);
mwf::Result<mwf::json> parse(const mwf::Bytes& bytes);

// Compact, canonical (object keys sorted) serialization — stable across runs,
// suitable for hashing and for command/args equality in tests.
std::string dump(const mwf::json& j);

// RFC 6901 JSON Pointer resolution ("" = root, "/a/0/b", ~0=~ ~1=/ escapes).
// Returns nullptr when the pointer does not resolve. Never throws.
const mwf::json* pointerGet(const mwf::json& root, std::string_view pointer);

// FNV-1a 64-bit over a byte string (spec hashing).
uint64_t fnv1a64(std::string_view data);

}  // namespace jsonutil
}  // namespace mwf_core

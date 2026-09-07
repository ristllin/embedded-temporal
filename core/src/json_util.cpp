// core: json_util — the one place that touches the concrete JSON library.
#include "mwf_core/detail/json_util.h"

namespace mwf_core {
namespace jsonutil {

namespace {
// Max container-nesting depth we will parse. Server-sourced JSON (Temporal
// histories fetched by the desktop worker) is untrusted; a pathologically
// nested document ("[[[[…]]]]") would recurse deep enough to overflow the stack
// — nlohmann builds the value iteratively but DESTROYS it recursively, so even
// the temporary tree's destructor can blow the stack. Real histories nest ~8
// deep, so 64 is generous headroom while still bounded far below any risk.
constexpr int kMaxJsonDepth = 64;

// Cheap, string-aware pre-scan: reject input whose bracket/brace nesting exceeds
// the limit BEFORE the DOM parser ever builds (or destroys) the deep tree. Only
// the max depth matters here — structural validity is still the parser's job.
bool jsonDepthWithinLimit(std::string_view text, int limit) {
  int depth = 0;
  bool in_string = false, escaped = false;
  for (char ch : text) {
    if (in_string) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') in_string = false;
      continue;  // brackets inside a string literal don't nest
    }
    switch (ch) {
      case '"': in_string = true; break;
      case '{':
      case '[':
        if (++depth > limit) return false;
        break;
      case '}':
      case ']':
        if (depth > 0) --depth;
        break;
      default: break;
    }
  }
  return true;
}
}  // namespace

#if defined(MWF_JSON_NLOHMANN)

mwf::Result<mwf::json> parse(std::string_view text) {
  if (!jsonDepthWithinLimit(text, kMaxJsonDepth)) {
    return mwf::Result<mwf::json>::failure("json nesting too deep");
  }
  mwf::json j = mwf::json::parse(text.begin(), text.end(), nullptr,
                                 /*allow_exceptions=*/false);
  if (j.is_discarded()) {
    return mwf::Result<mwf::json>::failure("json parse error");
  }
  return mwf::Result<mwf::json>::success(std::move(j));
}

mwf::Result<mwf::json> parse(const mwf::Bytes& bytes) {
  return parse(std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                bytes.size()));
}

std::string dump(const mwf::json& j) {
  // nlohmann object keys are std::map-ordered => compact dump is canonical.
  return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

#else
#error "core host build requires MWF_JSON_NLOHMANN (device swap: reimplement this file over ArduinoJson)"
#endif

const mwf::json* pointerGet(const mwf::json& root, std::string_view pointer) {
  if (pointer.empty()) return &root;
  if (pointer[0] != '/') return nullptr;
  const mwf::json* cur = &root;
  size_t i = 1;
  for (;;) {
    size_t j = pointer.find('/', i);
    const size_t end = (j == std::string_view::npos) ? pointer.size() : j;
    std::string tok;
    tok.reserve(end - i);
    for (size_t k = i; k < end; ++k) {
      if (pointer[k] == '~' && k + 1 < end) {
        if (pointer[k + 1] == '0') { tok += '~'; ++k; continue; }
        if (pointer[k + 1] == '1') { tok += '/'; ++k; continue; }
      }
      tok += pointer[k];
    }
    if (cur->is_object()) {
      auto it = cur->find(tok);
      if (it == cur->end()) return nullptr;
      cur = &it.value();
    } else if (cur->is_array()) {
      if (tok.empty() || tok.find_first_not_of("0123456789") != std::string::npos)
        return nullptr;
      const unsigned long idx = std::strtoul(tok.c_str(), nullptr, 10);
      if (idx >= cur->size()) return nullptr;
      cur = &(*cur)[idx];
    } else {
      return nullptr;
    }
    if (j == std::string_view::npos) break;
    i = j + 1;
  }
  return cur;
}

uint64_t fnv1a64(std::string_view data) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : data) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

}  // namespace jsonutil
}  // namespace mwf_core

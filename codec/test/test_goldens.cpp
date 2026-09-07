// test_goldens.cpp — validate PayloadCodecV1 against REAL captured Payloads.
//
// conformance captures custom Mistral Payloads into
// goldens/payloads/<name>.json. Golden schema (observed from the real captures):
//   {
//     "encoding": "json/wf_v1",
//     "metadata":         { "<key>": "<base64 of the raw wire byte value>", ... },
//     "metadata_decoded": { "<key>": "<utf8/decoded value>", ... },
//     "data_b64":  "<base64 of the inner payload bytes>",
//     "data_utf8": "<utf8 view of the same>",
//     "empty": <bool>,
//     "direction": "in" | "out", ...
//   }
// Temporal Payload.metadata VALUES are bytes; we reconstruct the wire Payload by
// base64-decoding `metadata` and `data_b64`, decode it, and assert the codec
// recovers the context + body, then re-encode and assert the metadata map + data
// reproduce the golden byte-for-byte (per ../contracts/spec/codec-wire-format.md).
//
// Self-skips (GTEST_SKIP) when no *.json goldens are present (conformance capture pending).
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mwf_codec/payload_codec.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string b64decode(const std::string& in) {
  static const std::string tbl =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0, bits = -8;
  for (unsigned char c : in) {
    if (c == '=') break;
    auto pos = tbl.find(c);
    if (pos == std::string::npos) continue;  // skip whitespace/newlines
    val = (val << 6) + static_cast<int>(pos);
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<char>((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

std::vector<fs::path> goldenDirCandidates() {
  std::vector<fs::path> dirs;
#ifdef MWF_GOLDENS_DIR
  dirs.emplace_back(MWF_GOLDENS_DIR);
#endif
  dirs.emplace_back("../../../conformance/goldens/payloads");
  dirs.emplace_back("../conformance/goldens/payloads");
  return dirs;
}

std::vector<fs::path> collectGoldens() {
  std::vector<fs::path> found;
  for (const auto& dir : goldenDirCandidates()) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      // Skip the aggregate summary — it is not a captured Payload.
      if (e.path().extension() == ".json" && e.path().filename() != "_summary.json")
        found.push_back(e.path());
    }
    if (!found.empty()) break;
  }
  return found;
}

// Reconstruct the on-wire Temporal Payload from a golden (bytes via base64).
mwf::temporal::Payload loadGolden(const json& j) {
  mwf::temporal::Payload p;
  for (auto it = j.at("metadata").begin(); it != j.at("metadata").end(); ++it) {
    p.metadata[it.key()] = b64decode(it.value().get<std::string>());
  }
  const std::string data = b64decode(j.at("data_b64").get<std::string>());
  p.data.assign(data.begin(), data.end());
  return p;
}

TEST(GoldenPayloads, DecodeReEncodeMatchesCapturesByteForByte) {
  const auto goldens = collectGoldens();
  if (goldens.empty()) {
    GTEST_SKIP() << "no goldens/payloads/*.json yet (conformance capture pending) — "
                    "synthetic-vector suite covers the wire format meanwhile";
  }

  mwf_codec::PayloadCodecV1 codec;
  int checked = 0;
  for (const auto& gpath : goldens) {
    SCOPED_TRACE(gpath.string());
    std::ifstream in(gpath);
    json j;
    ASSERT_TRUE((in >> j)) << "failed to parse golden JSON";

    auto payload = loadGolden(j);
    auto decoded = codec.decodeActivityInput(payload);
    ASSERT_TRUE(decoded.ok) << decoded.error;

    // Context matches the golden's decoded metadata.
    const auto& md = j.at("metadata_decoded");
    EXPECT_EQ(decoded.value.context.ns, md.at("namespace").get<std::string>());
    EXPECT_EQ(decoded.value.context.execution_id, md.at("execution_id").get<std::string>());

    // Body passthrough + empty flag.
    EXPECT_EQ(std::string(decoded.value.argument_json.begin(), decoded.value.argument_json.end()),
              j.at("data_utf8").get<std::string>());
    EXPECT_EQ(decoded.value.empty, j.at("empty").get<bool>());

    // Re-encode from the reconstructed context and assert byte-for-byte parity
    // with the captured Payload (metadata map + data). The metadata builder is
    // direction-agnostic, so this holds for both "in" and "out" captures.
    auto re = codec.encodeActivityResult(decoded.value.argument_json, decoded.value.context,
                                         decoded.value.empty);
    EXPECT_EQ(re.metadata, payload.metadata) << "re-encoded metadata != captured metadata";
    EXPECT_EQ(re.data, payload.data) << "re-encoded data != captured data";
    ++checked;
  }
  EXPECT_GT(checked, 0);
}

}  // namespace

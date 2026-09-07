// core/test: shared fixtures — golden loading, the specs matching the
// golden workflows, and the determinism-seam / durable-store fakes.
#pragma once
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "mwf/contracts.h"
#include "mwf_core/history.h"
#include "mwf_core/replay_engine.h"
#include "mwf_core/workflow_spec.h"

namespace mwf_test {

inline std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

inline mwf_core::History loadGolden(const std::string& name) {
  const std::string path = std::string(MWF_HISTORIES_DIR) + "/" + name + ".json";
  const std::string text = readFile(path);
  auto h = mwf_core::loadHistoryJson(text);
  if (!h) throw std::runtime_error("failed to load golden " + path + ": " + h.error);
  return std::move(h.value);
}

// The spec matching goldens/histories/linear.json:
//   activity(greet) -> activity(shout) -> complete   (README + event stream)
inline const char* kLinearSpec = R"({
  "name": "LinearWorkflow",
  "steps": [
    {"type": "activity", "name": "greet", "args_from": "/input"},
    {"type": "activity", "name": "shout", "args_from": "/results/greet"},
    {"type": "complete", "result_from": "/results/shout"}
  ]
})";

// The spec matching conditional_even.json / conditional_odd.json:
//   activity(parity) -> conditional on its result -> even_branch | odd_branch
//   -> complete. Both branch activities take the workflow input and bind the
//   shared id "branch" so the trailing complete reads whichever ran.
inline const char* kConditionalSpec = R"({
  "name": "ConditionalWorkflow",
  "steps": [
    {"type": "activity", "name": "parity", "args_from": "/input"},
    {"type": "conditional",
     "predicate": {"path": "/results/parity", "op": "eq", "value": "even"},
     "true_steps":  [{"type": "activity", "id": "branch", "name": "even_branch", "args_from": "/input"}],
     "false_steps": [{"type": "activity", "id": "branch", "name": "odd_branch",  "args_from": "/input"}]},
    {"type": "complete", "result_from": "/results/branch"}
  ]
})";

inline mwf_core::WorkflowSpec parseSpecOrDie(const char* text) {
  auto s = mwf_core::parseWorkflowSpec(std::string_view(text));
  if (!s) throw std::runtime_error("spec parse failed: " + s.error);
  return std::move(s.value);
}

// §6 determinism-seam fakes. The v1 walk must never consult them — these abort
// the test if it does.
struct BombClock : mwf::IClock {
  uint64_t nowMs() override {
    throw std::runtime_error("v1 replay consulted IClock — nondeterminism");
  }
};
struct BombRandom : mwf::IRandom {
  uint64_t next() override {
    throw std::runtime_error("v1 replay consulted IRandom — nondeterminism");
  }
};

// In-memory IDurableStore fake (host twin of the NVS+LittleFS device store).
struct MemStore : mwf::IDurableStore {
  std::map<std::string, mwf::Bytes, std::less<>> kv;
  bool put(std::string_view key, const mwf::Bytes& value) override {
    kv[std::string(key)] = value;
    return true;
  }
  std::optional<mwf::Bytes> get(std::string_view key) override {
    auto it = kv.find(key);
    if (it == kv.end()) return std::nullopt;
    return it->second;
  }
  bool erase(std::string_view key) override { return kv.erase(std::string(key)) > 0; }
  std::vector<std::string> keys(std::string_view prefix) override {
    std::vector<std::string> out;
    for (const auto& [k, v] : kv) {
      (void)v;
      if (k.rfind(std::string(prefix), 0) == 0) out.push_back(k);
    }
    return out;
  }
};

}  // namespace mwf_test

// firmware: DurableStoreLogic — the PORTABLE key-routing core of the durable
// store. No Arduino: this is the host-testable brain behind the device
// `mwf::IDurableStore` impl, mirroring the portable-core discipline (the decisions
// live in portable code with an injected backend; the Arduino glue is a thin shell).
//
// The KEY DECISIONS captured here:
//   * ROUTING BY SIZE — small values (task tokens) go to the NVS-backed key/value
//     store; larger blobs (serialized replay state) go to the LittleFS-backed file
//     store. Threshold = kInlineThreshold. NVS is precious + has a small per-value
//     ceiling; LittleFS is the place for anything that can grow.
//   * SINGLE-SOURCE INVARIANT — a put() writes ONE backend and erases the key from
//     the other, so a value that grows/shrinks across the threshold MOVES rather
//     than leaving a stale duplicate that get() might return.
//   * get() checks both backends; erase() removes from both; keys(prefix) merges +
//     filters + dedupes across both.
//
// Backends are injected as IKeyValueBackend, so the whole thing is exercised under
// [env:native] with an in-memory fake (test/test_durable_store/).
#pragma once
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mwf/types.h"  // mwf::Bytes

namespace mwf_device {

// Abstract storage backend keyed by an arbitrary LOGICAL string. Each concrete
// backend hides its own medium constraints (NVS 15-char keys + an enumeration
// index; LittleFS path sanitization). Host tests supply an in-memory fake.
struct IKeyValueBackend {
  virtual ~IKeyValueBackend() = default;
  virtual bool put(const std::string& key, const mwf::Bytes& value) = 0;
  virtual std::optional<mwf::Bytes> get(const std::string& key) = 0;
  virtual bool erase(const std::string& key) = 0;
  virtual std::vector<std::string> keys() = 0;  // all logical keys currently held
};

class DurableStoreLogic {
 public:
  // Values <= threshold -> NVS kv; larger -> LittleFS file. 512 B keeps task
  // tokens + tiny records in NVS while serialized replay state spills to flash
  // (well under NVS's per-blob ceiling either way).
  static constexpr std::size_t kInlineThreshold = 512;

  DurableStoreLogic(IKeyValueBackend* kv, IKeyValueBackend* file)
      : kv_(kv), file_(file) {}

  bool put(std::string_view key, const mwf::Bytes& value) {
    std::string k(key);
    if (value.size() <= kInlineThreshold) {
      file_->erase(k);  // drop any larger prior copy (moved below the threshold)
      return kv_->put(k, value);
    }
    kv_->erase(k);  // drop any smaller prior copy (moved above the threshold)
    return file_->put(k, value);
  }

  std::optional<mwf::Bytes> get(std::string_view key) {
    std::string k(key);
    if (auto v = kv_->get(k)) return v;
    return file_->get(k);
  }

  bool erase(std::string_view key) {
    std::string k(key);
    bool a = kv_->erase(k);
    bool b = file_->erase(k);
    return a || b;  // true if it existed in either backend
  }

  std::vector<std::string> keys(std::string_view prefix) {
    std::vector<std::string> out;
    auto take = [&](std::vector<std::string> src) {
      for (auto& s : src) {
        if (s.size() >= prefix.size() &&
            s.compare(0, prefix.size(), prefix.data(), prefix.size()) == 0) {
          out.push_back(std::move(s));
        }
      }
    };
    take(kv_->keys());
    take(file_->keys());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

 private:
  IKeyValueBackend* kv_;
  IKeyValueBackend* file_;
};

}  // namespace mwf_device

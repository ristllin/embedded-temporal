// firmware: DurableStore — device impl of mwf::IDurableStore.
// No-SD durable state: in-flight execution state + task tokens survive reboot
// (CONTRACTS.md §4). Keys: exec/<runId> = serialized replay state (large -> LittleFS);
// token/<runId> = task token (small -> NVS).
//
// Structure follows a proven prior design: NVS (Preferences) for the small
// critical journal, LittleFS for the larger blobs. The routing/merge brain is the
// portable DurableStoreLogic (host-tested); this file is the Arduino shell that gives
// it two concrete backends:
//   * NvsKvStore     — Preferences namespace; hashes arbitrary logical keys down to
//                      NVS's 15-char limit and keeps an index blob for enumeration.
//   * LittleFsFileStore — one file per key under /data/mwf/, name = percent-encoded key.
#pragma once
#include <memory>

#include "durable_store_logic.h"
#include "mwf/contracts.h"

namespace mwf_device {

// --- NVS (Preferences) key/value backend -------------------------------------
// NVS keys are capped at 15 chars, so we can't store under the raw logical key
// ("token/<runId>" is longer). We hash the logical key to a short NVS key and keep
// a newline-joined index of logical keys (under a reserved key) so keys()/enumeration
// works without relying on NVS iteration (awkward across core versions).
class NvsKvStore : public IKeyValueBackend {
 public:
  bool begin();  // open the Preferences namespace + load the index (idempotent)
  bool put(const std::string& key, const mwf::Bytes& value) override;
  std::optional<mwf::Bytes> get(const std::string& key) override;
  bool erase(const std::string& key) override;
  std::vector<std::string> keys() override;

 private:
  bool open_ = false;
  std::vector<std::string> index_;  // logical keys currently held
  void loadIndex();
  void saveIndex();
};

// --- LittleFS file backend ---------------------------------------------------
class LittleFsFileStore : public IKeyValueBackend {
 public:
  bool begin();  // mount LittleFS + ensure the /data/mwf dir exists (idempotent)
  bool put(const std::string& key, const mwf::Bytes& value) override;
  std::optional<mwf::Bytes> get(const std::string& key) override;
  bool erase(const std::string& key) override;
  std::vector<std::string> keys() override;

 private:
  bool open_ = false;
};

// --- The IDurableStore the worker loop consumes ------------------------------
class DurableStore : public mwf::IDurableStore {
 public:
  DurableStore() : logic_(&kv_, &file_) {}
  ~DurableStore() override = default;

  bool begin();  // bring up both backends; false if either fails to mount

  bool put(std::string_view key, const mwf::Bytes& value) override {
    return logic_.put(key, value);
  }
  std::optional<mwf::Bytes> get(std::string_view key) override {
    return logic_.get(key);
  }
  bool erase(std::string_view key) override { return logic_.erase(key); }
  std::vector<std::string> keys(std::string_view prefix) override {
    return logic_.keys(prefix);
  }

 private:
  NvsKvStore kv_;
  LittleFsFileStore file_;
  DurableStoreLogic logic_;
};

}  // namespace mwf_device

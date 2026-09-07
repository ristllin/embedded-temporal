// firmware: DurableStore device backends.
// Layout proven in earlier internal firmware (LittleFS blob path +
// Preferences NVS journal). External couplings stripped: no custom logger
// (log via ESP_LOG), no store:: namespace — this store is self-contained.
#include "durable_store.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_log.h>

#include <algorithm>
#include <cstdio>

namespace mwf_device {

static const char* kTag = "mwf.durable";

// ── NvsKvStore ───────────────────────────────────────────────────────────────
static Preferences s_prefs;
static const char* kNs      = "mwfstore";
static const char* kIdxKey  = "_idx";   // reserved: newline-joined logical keys

// FNV-1a 64-bit -> a <=15-char NVS-safe value key ("v" + 12 hex chars).
static std::string nvsKeyFor(const std::string& logical) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : logical) { h ^= c; h *= 1099511628211ULL; }
  char buf[16];
  std::snprintf(buf, sizeof(buf), "v%012llx", (unsigned long long)(h & 0xFFFFFFFFFFFFULL));
  return std::string(buf);
}

bool NvsKvStore::begin() {
  if (open_) return true;
  open_ = s_prefs.begin(kNs, /*readOnly=*/false);
  if (!open_) { ESP_LOGE(kTag, "NVS namespace open failed"); return false; }
  loadIndex();
  return true;
}

void NvsKvStore::loadIndex() {
  index_.clear();
  String blob = s_prefs.getString(kIdxKey, "");
  int start = 0;
  while (start < (int)blob.length()) {
    int nl = blob.indexOf('\n', start);
    if (nl < 0) nl = blob.length();
    if (nl > start) index_.emplace_back(blob.substring(start, nl).c_str());
    start = nl + 1;
  }
}

void NvsKvStore::saveIndex() {
  String blob;
  for (auto& k : index_) { blob += k.c_str(); blob += '\n'; }
  s_prefs.putString(kIdxKey, blob);
}

bool NvsKvStore::put(const std::string& key, const mwf::Bytes& value) {
  if (!open_) return false;
  std::string nk = nvsKeyFor(key);
  size_t wrote = s_prefs.putBytes(nk.c_str(), value.data(), value.size());
  if (wrote != value.size()) { ESP_LOGE(kTag, "NVS putBytes short write"); return false; }
  if (std::find(index_.begin(), index_.end(), key) == index_.end()) {
    index_.push_back(key);
    saveIndex();
  }
  return true;
}

std::optional<mwf::Bytes> NvsKvStore::get(const std::string& key) {
  if (!open_) return std::nullopt;
  std::string nk = nvsKeyFor(key);
  size_t n = s_prefs.getBytesLength(nk.c_str());
  if (n == 0) return std::nullopt;
  mwf::Bytes out(n);
  size_t got = s_prefs.getBytes(nk.c_str(), out.data(), n);
  if (got != n) return std::nullopt;
  return out;
}

bool NvsKvStore::erase(const std::string& key) {
  if (!open_) return false;
  auto it = std::find(index_.begin(), index_.end(), key);
  if (it == index_.end()) return false;  // not held here
  std::string nk = nvsKeyFor(key);
  s_prefs.remove(nk.c_str());
  index_.erase(it);
  saveIndex();
  return true;
}

std::vector<std::string> NvsKvStore::keys() { return index_; }

// ── LittleFsFileStore ────────────────────────────────────────────────────────
static const char* kDir = "/data/mwf";

// Reversible file-name mapping: percent-encode everything but [A-Za-z0-9._-], so a
// logical key like "exec/run-42" round-trips through a flat filename.
static std::string encodeName(const std::string& key) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : key) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
      out.push_back((char)c);
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}

static std::string decodeName(const std::string& name) {
  auto unhex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  std::string out;
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '%' && i + 2 < name.size()) {
      int hi = unhex(name[i + 1]), lo = unhex(name[i + 2]);
      if (hi >= 0 && lo >= 0) { out.push_back((char)((hi << 4) | lo)); i += 2; continue; }
    }
    out.push_back(name[i]);
  }
  return out;
}

bool LittleFsFileStore::begin() {
  if (open_) return true;
  if (!LittleFS.begin(/*formatOnFail=*/true)) {
    ESP_LOGE(kTag, "LittleFS mount failed");
    return false;
  }
  if (!LittleFS.exists("/data")) LittleFS.mkdir("/data");
  if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);
  open_ = true;
  return true;
}

bool LittleFsFileStore::put(const std::string& key, const mwf::Bytes& value) {
  if (!open_) return false;
  // NOTE (load-bearing): this write is NOT crash-atomic (truncate-then-write). A
  // brownout mid-write leaves a corrupt exec/<runId> blob — which is SAFE here
  // only because Temporal's history is the source of truth: restoreState fails to
  // parse a corrupt blob, loadState returns failure, and the replay engine
  // rebuilds exact state from the full server history (see WorkflowLoop). Do NOT
  // start trusting local state without that full-history fallback. Making the
  // snapshot crash-atomic (temp file + rename) is a ROADMAP item.
  std::string path = std::string(kDir) + "/" + encodeName(key);
  File f = LittleFS.open(path.c_str(), FILE_WRITE);
  if (!f) { ESP_LOGE(kTag, "LittleFS open-for-write failed: %s", path.c_str()); return false; }
  size_t wrote = f.write(value.data(), value.size());
  f.close();
  return wrote == value.size();
}

std::optional<mwf::Bytes> LittleFsFileStore::get(const std::string& key) {
  if (!open_) return std::nullopt;
  std::string path = std::string(kDir) + "/" + encodeName(key);
  File f = LittleFS.open(path.c_str(), FILE_READ);
  if (!f) return std::nullopt;
  size_t n = f.size();
  mwf::Bytes out(n);
  size_t got = n ? f.read(out.data(), n) : 0;
  f.close();
  if (got != n) return std::nullopt;
  return out;
}

bool LittleFsFileStore::erase(const std::string& key) {
  if (!open_) return false;
  std::string path = std::string(kDir) + "/" + encodeName(key);
  if (!LittleFS.exists(path.c_str())) return false;
  return LittleFS.remove(path.c_str());
}

std::vector<std::string> LittleFsFileStore::keys() {
  std::vector<std::string> out;
  if (!open_) return out;
  File dir = LittleFS.open(kDir);
  if (!dir || !dir.isDirectory()) return out;
  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    if (e.isDirectory()) continue;
    std::string name = e.name();  // basename on this core
    auto slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    out.push_back(decodeName(name));
  }
  return out;
}

// ── DurableStore ─────────────────────────────────────────────────────────────
bool DurableStore::begin() {
  bool a = file_.begin();
  bool b = kv_.begin();
  if (!a || !b) ESP_LOGE(kTag, "durable store degraded (fs=%d nvs=%d)", a, b);
  return a && b;
}

}  // namespace mwf_device

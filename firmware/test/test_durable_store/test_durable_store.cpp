// Host test for the PORTABLE durable-store core, run under `pio test -e
// native`. Exercises DurableStoreLogic's key decisions — size-based routing between
// the NVS-ish kv backend and the LittleFS-ish file backend, the single-source
// move-across-threshold invariant, get/erase across both, and prefix listing — via
// an in-memory fake backend (no Arduino, no hardware).
#include <unity.h>

#include <map>
#include <string>

#include "durable_store_logic.h"

using mwf::Bytes;
using mwf_device::DurableStoreLogic;
using mwf_device::IKeyValueBackend;

// In-memory fake standing in for both NVS and LittleFS.
struct FakeBackend : IKeyValueBackend {
  std::map<std::string, Bytes> data;
  bool put(const std::string& key, const Bytes& value) override {
    data[key] = value;
    return true;
  }
  std::optional<Bytes> get(const std::string& key) override {
    auto it = data.find(key);
    if (it == data.end()) return std::nullopt;
    return it->second;
  }
  bool erase(const std::string& key) override { return data.erase(key) > 0; }
  std::vector<std::string> keys() override {
    std::vector<std::string> out;
    for (auto& kv : data) out.push_back(kv.first);
    return out;
  }
};

static FakeBackend* g_kv;
static FakeBackend* g_file;
static DurableStoreLogic* g_logic;

void setUp() {
  g_kv = new FakeBackend();
  g_file = new FakeBackend();
  g_logic = new DurableStoreLogic(g_kv, g_file);
}
void tearDown() {
  delete g_logic;
  delete g_kv;
  delete g_file;
}

static Bytes bytesOf(const std::string& s) { return Bytes(s.begin(), s.end()); }
static Bytes bigBytes(size_t n) { return Bytes(n, 0xAB); }

// Small value (a task token) routes to NVS kv, not the file store.
void test_small_value_routes_to_kv() {
  TEST_ASSERT_TRUE(g_logic->put("token/run-1", bytesOf("tok-abc")));
  TEST_ASSERT_EQUAL_UINT(1, g_kv->data.count("token/run-1"));
  TEST_ASSERT_EQUAL_UINT(0, g_file->data.count("token/run-1"));
  auto v = g_logic->get("token/run-1");
  TEST_ASSERT_TRUE(v.has_value());
  TEST_ASSERT_EQUAL_STRING("tok-abc", std::string(v->begin(), v->end()).c_str());
}

// Large value (serialized replay state) routes to the LittleFS file store.
void test_large_value_routes_to_file() {
  Bytes big = bigBytes(DurableStoreLogic::kInlineThreshold + 1);
  TEST_ASSERT_TRUE(g_logic->put("exec/run-1", big));
  TEST_ASSERT_EQUAL_UINT(0, g_kv->data.count("exec/run-1"));
  TEST_ASSERT_EQUAL_UINT(1, g_file->data.count("exec/run-1"));
  auto v = g_logic->get("exec/run-1");
  TEST_ASSERT_TRUE(v.has_value());
  TEST_ASSERT_EQUAL_UINT(big.size(), v->size());
}

// A value at exactly the threshold stays in kv (<= is inline).
void test_threshold_boundary_stays_kv() {
  Bytes edge = bigBytes(DurableStoreLogic::kInlineThreshold);
  TEST_ASSERT_TRUE(g_logic->put("exec/edge", edge));
  TEST_ASSERT_EQUAL_UINT(1, g_kv->data.count("exec/edge"));
  TEST_ASSERT_EQUAL_UINT(0, g_file->data.count("exec/edge"));
}

// Growing a value across the threshold MOVES it (single-source invariant): the old
// kv copy must be gone, only the file copy remains, and get() returns the new value.
void test_grow_across_threshold_moves_and_no_duplicate() {
  g_logic->put("exec/run-2", bytesOf("small"));
  TEST_ASSERT_EQUAL_UINT(1, g_kv->data.count("exec/run-2"));

  Bytes big = bigBytes(DurableStoreLogic::kInlineThreshold + 10);
  g_logic->put("exec/run-2", big);
  TEST_ASSERT_EQUAL_UINT(0, g_kv->data.count("exec/run-2"));   // old copy erased
  TEST_ASSERT_EQUAL_UINT(1, g_file->data.count("exec/run-2"));
  TEST_ASSERT_EQUAL_UINT(big.size(), g_logic->get("exec/run-2")->size());
}

// Shrinking back across the threshold moves it the other way.
void test_shrink_across_threshold_moves_back() {
  g_logic->put("exec/run-3", bigBytes(DurableStoreLogic::kInlineThreshold + 10));
  TEST_ASSERT_EQUAL_UINT(1, g_file->data.count("exec/run-3"));
  g_logic->put("exec/run-3", bytesOf("tiny"));
  TEST_ASSERT_EQUAL_UINT(0, g_file->data.count("exec/run-3"));  // old copy erased
  TEST_ASSERT_EQUAL_UINT(1, g_kv->data.count("exec/run-3"));
}

void test_get_missing_returns_nullopt() {
  TEST_ASSERT_FALSE(g_logic->get("nope").has_value());
}

// erase() removes from whichever backend holds the key; returns false when absent.
void test_erase_across_both() {
  g_logic->put("token/a", bytesOf("x"));
  g_logic->put("exec/b", bigBytes(DurableStoreLogic::kInlineThreshold + 1));
  TEST_ASSERT_TRUE(g_logic->erase("token/a"));
  TEST_ASSERT_TRUE(g_logic->erase("exec/b"));
  TEST_ASSERT_FALSE(g_logic->get("token/a").has_value());
  TEST_ASSERT_FALSE(g_logic->get("exec/b").has_value());
  TEST_ASSERT_FALSE(g_logic->erase("token/a"));  // already gone
}

// keys(prefix) merges both backends, filters by prefix, and dedupes.
void test_keys_prefix_merge() {
  g_logic->put("token/r1", bytesOf("t1"));                                  // kv
  g_logic->put("token/r2", bytesOf("t2"));                                  // kv
  g_logic->put("exec/r1", bigBytes(DurableStoreLogic::kInlineThreshold + 1));  // file
  g_logic->put("exec/r2", bigBytes(DurableStoreLogic::kInlineThreshold + 1));  // file

  auto tokens = g_logic->keys("token/");
  TEST_ASSERT_EQUAL_UINT(2, tokens.size());
  TEST_ASSERT_EQUAL_STRING("token/r1", tokens[0].c_str());  // sorted
  TEST_ASSERT_EQUAL_STRING("token/r2", tokens[1].c_str());

  auto execs = g_logic->keys("exec/");
  TEST_ASSERT_EQUAL_UINT(2, execs.size());

  auto all = g_logic->keys("");
  TEST_ASSERT_EQUAL_UINT(4, all.size());
}

// A key present in BOTH backends (shouldn't happen via put, but keys() must be
// robust) is deduped in the merged listing.
void test_keys_dedupes_cross_backend() {
  g_kv->data["exec/dup"] = bytesOf("a");
  g_file->data["exec/dup"] = bytesOf("b");
  auto keys = g_logic->keys("exec/");
  TEST_ASSERT_EQUAL_UINT(1, keys.size());
  TEST_ASSERT_EQUAL_STRING("exec/dup", keys[0].c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_small_value_routes_to_kv);
  RUN_TEST(test_large_value_routes_to_file);
  RUN_TEST(test_threshold_boundary_stays_kv);
  RUN_TEST(test_grow_across_threshold_moves_and_no_duplicate);
  RUN_TEST(test_shrink_across_threshold_moves_back);
  RUN_TEST(test_get_missing_returns_nullopt);
  RUN_TEST(test_erase_across_both);
  RUN_TEST(test_keys_prefix_merge);
  RUN_TEST(test_keys_dedupes_cross_backend);
  return UNITY_END();
}

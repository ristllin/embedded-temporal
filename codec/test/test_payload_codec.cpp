// test_payload_codec.cpp — PayloadCodecV1 conformance.
//
// Synthetic vectors built from the wire spec
// (../contracts/spec/codec-wire-format.md). Golden-payload validation
// against real captures (conformance/goldens/payloads/) lives in
// test_goldens.cpp, which self-skips until conformance produces captures.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "mwf_codec/payload_codec.h"

namespace {

using mwf_codec::PayloadCodecV1;

// bytes <-> string helpers (JSON crosses the seam as raw bytes).
mwf::Bytes bytesOf(const std::string& s) { return mwf::Bytes(s.begin(), s.end()); }
std::string strOf(const mwf::Bytes& b) { return std::string(b.begin(), b.end()); }

// A fully-populated custom context for round-trip coverage.
mwf::WorkflowContext fullCtx() {
  mwf::WorkflowContext c;
  c.ns = "mistral-prod";
  c.execution_id = "wf-exec-abc123";
  c.root_workflow_exec_id = "wf-root-000";
  c.parent_workflow_exec_id = "wf-parent-111";
  c.execution_token = "tok-deadbeef";
  c.extensions_json = R"({"feature":"beta","n":7})";
  c.on_behalf_of = true;
  return c;
}

// ── encode: exact metadata keys/values per the spec table ──────────────────────

TEST(Encode, MetadataKeysMatchSpecByteForByte) {
  PayloadCodecV1 codec;
  mwf::WorkflowContext ctx;
  ctx.ns = "ns1";
  ctx.execution_id = "wf-1";

  auto p = codec.encodeActivityResult(bytesOf(R"({"result":42})"), ctx, /*empty=*/false);

  // Always-present keys.
  EXPECT_EQ(p.metadata.at("encoding"), "json/wf_v1");
  EXPECT_EQ(p.metadata.at("namespace"), "ns1");
  EXPECT_EQ(p.metadata.at("execution_id"), "wf-1");
  ASSERT_TRUE(p.metadata.count("encoding_options"));
  EXPECT_EQ(p.metadata.at("encoding_options"), "");  // empty string in v1

  // Data is the raw JSON body, untouched.
  EXPECT_EQ(strOf(p.data), R"({"result":42})");
}

TEST(Encode, OptionalKeysOmittedWhenUnset) {
  PayloadCodecV1 codec;
  mwf::WorkflowContext ctx;
  ctx.ns = "ns1";
  ctx.execution_id = "wf-1";

  auto p = codec.encodeActivityResult(bytesOf("{}"), ctx, /*empty=*/false);

  EXPECT_FALSE(p.metadata.count("root_workflow_exec_id"));
  EXPECT_FALSE(p.metadata.count("parent_workflow_exec_id"));
  EXPECT_FALSE(p.metadata.count("__internal_execution_token"));
  EXPECT_FALSE(p.metadata.count("__internal_extensions"));
  EXPECT_FALSE(p.metadata.count("__internal_on_behalf_of"));
  EXPECT_FALSE(p.metadata.count("empty_payload"));
  // exactly the 4 always-present keys
  EXPECT_EQ(p.metadata.size(), 4u);
}

TEST(Encode, OptionalKeysPresentWhenSet) {
  PayloadCodecV1 codec;
  auto p = codec.encodeActivityResult(bytesOf("{}"), fullCtx(), /*empty=*/false);

  EXPECT_EQ(p.metadata.at("root_workflow_exec_id"), "wf-root-000");
  EXPECT_EQ(p.metadata.at("parent_workflow_exec_id"), "wf-parent-111");
  EXPECT_EQ(p.metadata.at("__internal_execution_token"), "tok-deadbeef");
  EXPECT_EQ(p.metadata.at("__internal_extensions"), R"({"feature":"beta","n":7})");
  EXPECT_EQ(p.metadata.at("__internal_on_behalf_of"), "true");
}

TEST(Encode, OnBehalfOfFalseSerializesAsStringFalse) {
  PayloadCodecV1 codec;
  mwf::WorkflowContext ctx;
  ctx.ns = "ns1";
  ctx.execution_id = "wf-1";
  ctx.on_behalf_of = false;

  auto p = codec.encodeActivityResult(bytesOf("{}"), ctx, /*empty=*/false);
  ASSERT_TRUE(p.metadata.count("__internal_on_behalf_of"));
  EXPECT_EQ(p.metadata.at("__internal_on_behalf_of"), "false");
}

TEST(Encode, EmptyExtensionsStringIsOmitted) {
  PayloadCodecV1 codec;
  mwf::WorkflowContext ctx;
  ctx.ns = "ns1";
  ctx.execution_id = "wf-1";
  ctx.extensions_json = "";  // present but empty => omit (matches "if extensions")

  auto p = codec.encodeActivityResult(bytesOf("{}"), ctx, /*empty=*/false);
  EXPECT_FALSE(p.metadata.count("__internal_extensions"));
}

TEST(Encode, EmptyPayloadKeySetWhenEmpty) {
  PayloadCodecV1 codec;
  mwf::WorkflowContext ctx;
  ctx.ns = "ns1";
  ctx.execution_id = "wf-1";

  auto p = codec.encodeActivityResult(mwf::Bytes{}, ctx, /*empty=*/true);
  ASSERT_TRUE(p.metadata.count("empty_payload"));
  // Wire truthy-bytes sentinel is a single 0x00 byte (matches the real SDK /
  // conformance's golden empty-out.json base64 "AA==").
  EXPECT_EQ(p.metadata.at("empty_payload"), std::string(1, '\0'));
}

// ── decode: validation + reconstruction ────────────────────────────────────────

TEST(Decode, RejectsMissingEncoding) {
  PayloadCodecV1 codec;
  mwf::temporal::Payload p;
  p.metadata["execution_id"] = "wf-1";
  auto r = codec.decodeActivityInput(p);
  EXPECT_FALSE(r.ok);
}

TEST(Decode, RejectsEmptyExecutionId) {
  PayloadCodecV1 codec;
  mwf::temporal::Payload p;
  p.metadata["encoding"] = "json/wf_v1";
  p.metadata["namespace"] = "ns1";
  p.metadata["execution_id"] = "";  // empty => reject
  p.metadata["encoding_options"] = "";
  auto r = codec.decodeActivityInput(p);
  EXPECT_FALSE(r.ok);
}

TEST(Decode, RejectsMissingExecutionId) {
  PayloadCodecV1 codec;
  mwf::temporal::Payload p;
  p.metadata["encoding"] = "json/wf_v1";
  p.metadata["namespace"] = "ns1";
  auto r = codec.decodeActivityInput(p);
  EXPECT_FALSE(r.ok);
}

TEST(Decode, RejectsLegacyAbraxasEncoding) {
  PayloadCodecV1 codec;
  mwf::temporal::Payload p;
  p.metadata["encoding"] = "json/abraxas_v1";
  p.metadata["execution_id"] = "wf-1";
  auto r = codec.decodeActivityInput(p);
  EXPECT_FALSE(r.ok);  // decode-only legacy is out of v1 scope
}

TEST(Decode, ReconstructsFullContextAndBody) {
  PayloadCodecV1 codec;
  mwf::temporal::Payload p;
  p.metadata["encoding"] = "json/wf_v1";
  p.metadata["namespace"] = "mistral-prod";
  p.metadata["execution_id"] = "wf-exec-abc123";
  p.metadata["encoding_options"] = "";
  p.metadata["root_workflow_exec_id"] = "wf-root-000";
  p.metadata["parent_workflow_exec_id"] = "wf-parent-111";
  p.metadata["__internal_execution_token"] = "tok-deadbeef";
  p.metadata["__internal_extensions"] = R"({"feature":"beta"})";
  p.metadata["__internal_on_behalf_of"] = "true";
  p.data = bytesOf(R"({"arg":"value"})");

  auto r = codec.decodeActivityInput(p);
  ASSERT_TRUE(r.ok) << r.error;
  const auto& d = r.value;
  EXPECT_EQ(d.context.ns, "mistral-prod");
  EXPECT_EQ(d.context.execution_id, "wf-exec-abc123");
  ASSERT_TRUE(d.context.root_workflow_exec_id.has_value());
  EXPECT_EQ(*d.context.root_workflow_exec_id, "wf-root-000");
  ASSERT_TRUE(d.context.parent_workflow_exec_id.has_value());
  EXPECT_EQ(*d.context.parent_workflow_exec_id, "wf-parent-111");
  ASSERT_TRUE(d.context.execution_token.has_value());
  EXPECT_EQ(*d.context.execution_token, "tok-deadbeef");
  ASSERT_TRUE(d.context.extensions_json.has_value());
  EXPECT_EQ(*d.context.extensions_json, R"({"feature":"beta"})");
  ASSERT_TRUE(d.context.on_behalf_of.has_value());
  EXPECT_TRUE(*d.context.on_behalf_of);
  EXPECT_EQ(strOf(d.argument_json), R"({"arg":"value"})");
  EXPECT_FALSE(d.empty);
}

TEST(Decode, OptionalKeysAbsentLeaveContextUnset) {
  PayloadCodecV1 codec;
  mwf::temporal::Payload p;
  p.metadata["encoding"] = "json/wf_v1";
  p.metadata["namespace"] = "ns1";
  p.metadata["execution_id"] = "wf-1";
  p.metadata["encoding_options"] = "";
  p.data = bytesOf("{}");

  auto r = codec.decodeActivityInput(p);
  ASSERT_TRUE(r.ok) << r.error;
  const auto& d = r.value;
  EXPECT_FALSE(d.context.root_workflow_exec_id.has_value());
  EXPECT_FALSE(d.context.parent_workflow_exec_id.has_value());
  EXPECT_FALSE(d.context.execution_token.has_value());
  EXPECT_FALSE(d.context.extensions_json.has_value());
  EXPECT_FALSE(d.context.on_behalf_of.has_value());
}

TEST(Decode, OnBehalfOfFalseParses) {
  PayloadCodecV1 codec;
  mwf::temporal::Payload p;
  p.metadata["encoding"] = "json/wf_v1";
  p.metadata["namespace"] = "ns1";
  p.metadata["execution_id"] = "wf-1";
  p.metadata["__internal_on_behalf_of"] = "false";
  p.data = bytesOf("{}");

  auto r = codec.decodeActivityInput(p);
  ASSERT_TRUE(r.ok) << r.error;
  ASSERT_TRUE(r.value.context.on_behalf_of.has_value());
  EXPECT_FALSE(*r.value.context.on_behalf_of);
}

// ── backward-compat: json/plain ────────────────────────────────────────────────

TEST(Decode, PlainEncodingTreatsDataAsArgWithEmptyContext) {
  PayloadCodecV1 codec;
  mwf::temporal::Payload p;
  p.metadata["encoding"] = "json/plain";
  p.data = bytesOf(R"({"just":"an arg"})");

  auto r = codec.decodeActivityInput(p);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(strOf(r.value.argument_json), R"({"just":"an arg"})");
  EXPECT_TRUE(r.value.context.ns.empty());
  EXPECT_TRUE(r.value.context.execution_id.empty());
  EXPECT_FALSE(r.value.empty);
}

// ── round-trips ────────────────────────────────────────────────────────────────

TEST(RoundTrip, EncodeThenDecodePreservesContextAndBody) {
  PayloadCodecV1 codec;
  const std::string body = R"({"result":{"ok":true,"items":[1,2,3]}})";
  auto encoded = codec.encodeActivityResult(bytesOf(body), fullCtx(), /*empty=*/false);

  auto r = codec.decodeActivityInput(encoded);
  ASSERT_TRUE(r.ok) << r.error;
  const auto& d = r.value;
  const auto in = fullCtx();
  EXPECT_EQ(d.context.ns, in.ns);
  EXPECT_EQ(d.context.execution_id, in.execution_id);
  EXPECT_EQ(d.context.root_workflow_exec_id, in.root_workflow_exec_id);
  EXPECT_EQ(d.context.parent_workflow_exec_id, in.parent_workflow_exec_id);
  EXPECT_EQ(d.context.execution_token, in.execution_token);
  EXPECT_EQ(d.context.extensions_json, in.extensions_json);
  EXPECT_EQ(d.context.on_behalf_of, in.on_behalf_of);
  EXPECT_EQ(strOf(d.argument_json), body);
  EXPECT_FALSE(d.empty);
}

TEST(RoundTrip, EmptyPayloadFlagSurvives) {
  PayloadCodecV1 codec;
  mwf::WorkflowContext ctx;
  ctx.ns = "ns1";
  ctx.execution_id = "wf-1";

  auto encoded = codec.encodeActivityResult(mwf::Bytes{}, ctx, /*empty=*/true);
  auto r = codec.decodeActivityInput(encoded);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_TRUE(r.value.empty);
  EXPECT_TRUE(r.value.argument_json.empty());
}

TEST(RoundTrip, MinimalContextNoOptionalKeys) {
  PayloadCodecV1 codec;
  mwf::WorkflowContext ctx;
  ctx.ns = "";  // namespace may be empty; only execution_id is required
  ctx.execution_id = "wf-min";

  auto encoded = codec.encodeActivityResult(bytesOf("null"), ctx, /*empty=*/false);
  auto r = codec.decodeActivityInput(encoded);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.value.context.execution_id, "wf-min");
  EXPECT_EQ(strOf(r.value.argument_json), "null");
}

}  // namespace

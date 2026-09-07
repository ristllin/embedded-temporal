/* nanopb_smoke.c — device-twin smoke test.
 *
 * Proves the generated nanopb C compiles + links against the nanopb runtime on
 * the host, and that a representative statically-allocated message encodes and
 * decodes with pb_encode/pb_decode. This is NOT an ESP32 link (no toolchain
 * here) — it's the "the generated stubs build and round-trip" gate. The real
 * device link is the PlatformIO build.
 */
#include <stdio.h>
#include <string.h>

#include "pb_encode.h"
#include "pb_decode.h"
#include "temporal/api/common/v1/message.pb.h"
#include "temporal/api/history/v1/message.pb.h"

static int failures = 0;
#define CHECK(cond, msg)                                        \
  do {                                                          \
    if (!(cond)) {                                              \
      fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
      ++failures;                                               \
    }                                                           \
  } while (0)

int main(void) {
  puts("proto nanopb smoke:");

  /* Build a Payload: one metadata entry + JSON-ish data bytes. All static. */
  temporal_api_common_v1_Payload out = temporal_api_common_v1_Payload_init_zero;
  out.metadata_count = 1;
  strcpy(out.metadata[0].key, "encoding");
  const char* enc = "json/plain";
  out.metadata[0].value.size = (pb_size_t)strlen(enc);
  memcpy(out.metadata[0].value.bytes, enc, strlen(enc));
  const char* body = "{\"hello\":\"world\"}";
  out.data.size = (pb_size_t)strlen(body);
  memcpy(out.data.bytes, body, strlen(body));

  uint8_t buf[512];
  pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
  CHECK(pb_encode(&os, temporal_api_common_v1_Payload_fields, &out),
        "pb_encode(Payload) failed");
  size_t n = os.bytes_written;
  CHECK(n > 0, "Payload encoded to 0 bytes");

  temporal_api_common_v1_Payload in = temporal_api_common_v1_Payload_init_zero;
  pb_istream_t is = pb_istream_from_buffer(buf, n);
  CHECK(pb_decode(&is, temporal_api_common_v1_Payload_fields, &in),
        "pb_decode(Payload) failed");
  CHECK(in.metadata_count == 1, "metadata count lost");
  CHECK(strcmp(in.metadata[0].key, "encoding") == 0, "metadata key lost");
  CHECK(in.data.size == strlen(body), "data size lost");
  CHECK(memcmp(in.data.bytes, body, in.data.size) == 0, "data bytes lost");
  printf("  ok: Payload round-trip (%zu bytes)\n", n);

  /* HistoryEvent: exercise a oneof (which_attributes) + nested attributes. */
  temporal_api_history_v1_HistoryEvent ev = temporal_api_history_v1_HistoryEvent_init_zero;
  ev.event_id = 5;
  ev.task_id = 99;
  ev.which_attributes =
      temporal_api_history_v1_HistoryEvent_activity_task_started_event_attributes_tag;
  ev.attributes.activity_task_started_event_attributes.scheduled_event_id = 4;
  ev.attributes.activity_task_started_event_attributes.attempt = 1;
  strcpy(ev.attributes.activity_task_started_event_attributes.identity, "nimbus-worker");

  uint8_t ebuf[512];
  pb_ostream_t eos = pb_ostream_from_buffer(ebuf, sizeof(ebuf));
  CHECK(pb_encode(&eos, temporal_api_history_v1_HistoryEvent_fields, &ev),
        "pb_encode(HistoryEvent) failed");

  temporal_api_history_v1_HistoryEvent ev2 = temporal_api_history_v1_HistoryEvent_init_zero;
  pb_istream_t eis = pb_istream_from_buffer(ebuf, eos.bytes_written);
  CHECK(pb_decode(&eis, temporal_api_history_v1_HistoryEvent_fields, &ev2),
        "pb_decode(HistoryEvent) failed");
  CHECK(ev2.event_id == 5, "event_id lost");
  CHECK(ev2.which_attributes ==
            temporal_api_history_v1_HistoryEvent_activity_task_started_event_attributes_tag,
        "oneof discriminant lost");
  CHECK(strcmp(ev2.attributes.activity_task_started_event_attributes.identity,
               "nimbus-worker") == 0,
        "nested oneof attributes lost");
  printf("  ok: HistoryEvent oneof round-trip (%zu bytes)\n", eos.bytes_written);

  if (failures) {
    fprintf(stderr, "\n%d nanopb check(s) FAILED\n", failures);
    return 1;
  }
  puts("\nnanopb generated stubs build + round-trip green");
  return 0;
}

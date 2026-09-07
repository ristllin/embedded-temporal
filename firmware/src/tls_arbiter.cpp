// firmware: TLS work-slot arbiter impl (adapted from earlier internal firmware).
// External couplings stripped: the tlsSlots NVS knob is gone; slots hardcoded to 2
// (with mbedTLS calloc routed to PSRAM, two concurrent work-TLS sessions fit — the
// per-session internal cost is lwIP sockets + task-stack transients).
#include "tls_arbiter.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace mwf {
namespace arbiter {

static SemaphoreHandle_t g_sem = nullptr;
static constexpr int kSlots = 2;

void begin() {
  if (g_sem) return;  // idempotent
  g_sem = xSemaphoreCreateCounting(kSlots, kSlots);
}

bool acquireWork(uint32_t timeoutMs) {
  if (!g_sem) return true;  // arbiter not started => no contention
  return xSemaphoreTake(g_sem, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void releaseWork() {
  if (g_sem) xSemaphoreGive(g_sem);
}

int slots() { return g_sem ? kSlots : 0; }

}  // namespace arbiter
}  // namespace mwf

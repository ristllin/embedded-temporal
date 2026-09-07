// firmware: PSRAM-backed mbedTLS allocator install (adapted from earlier
// internal firmware). External couplings stripped: no custom logger (ESP_LOG instead).
#include "psram_tls.h"

#include <Arduino.h>
#include <esp_heap_caps.h>     // heap_caps_calloc / heap_caps_malloc_extmem_enable
#include <esp_log.h>
#include <mbedtls/platform.h>  // mbedtls_platform_set_calloc_free

namespace mwf {

void installPsramMbedtls() {
  if (ESP.getPsramSize() == 0) {
    ESP_LOGW("mwf.psram", "no PSRAM -> mbedTLS stays on internal heap");
    return;
  }
  // Big TLS buffers -> PSRAM.
  mbedtls_platform_set_calloc_free(
      [](size_t n, size_t sz) -> void* { return heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM); },
      free);
  // ...and route the general allocation churn AROUND a network call (String growth,
  // JSON nodes, HTTP header buffers) to PSRAM too: allocations >= 128 B go external,
  // tiny ones stay internal for hot-path speed. DMA/ISR allocs use explicit
  // MALLOC_CAP_DMA/INTERNAL and bypass this entirely (WiFi/lwIP buffers unaffected).
  heap_caps_malloc_extmem_enable(128);
  ESP_LOGI("mwf.psram", "mbedTLS + churn -> PSRAM (%u KB free)",
           (unsigned)(ESP.getFreePsram() / 1024));
}

}  // namespace mwf

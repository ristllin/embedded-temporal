// firmware: PSRAM-backed mbedTLS allocator (adapted from earlier internal firmware).
// The S3 has 8 MB PSRAM but it is NOT in the default malloc pool, and a single TLS
// handshake wants ~40 KB contiguous — which the scarce ~200 KB internal SRAM can't
// spare once WiFi + the worker + codecs are resident (previously measured: mbedTLS -0x7F00
// ALLOC_FAILED at ~50 KB). TLS record/session buffers are CPU-processed, never DMA,
// so external RAM is fine. Call ONCE at boot, before the first outbound gRPC/TLS.
// No-op if PSRAM isn't present.
#pragma once

namespace mwf {

// Route mbedTLS calloc/free to PSRAM and spill general >=128 B allocation churn
// (String/JSON/HTTP header assembly) off the internal heap too. Safe to call once.
void installPsramMbedtls();

}  // namespace mwf

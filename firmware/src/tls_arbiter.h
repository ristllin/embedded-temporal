// firmware: TLS work-slot arbiter (a pattern hardened across prior internal
// firmware generations). External couplings stripped: the NVS `tlsSlots` knob +
// store:: dependency are dropped — slot count is hardcoded to 2.
//
// A counting semaphore bounding concurrent outbound TLS handshakes so the single
// mbedTLS arena never has to hold too many resident sessions. Take a slot before
// opening a WiFiClientSecure; release it right after the connection closes.
//
//   if (!mwf::arbiter::acquireWork(10000)) { /* timeout, retry later */ }
//   // ... open client, do the gRPC call, close client ...
//   mwf::arbiter::releaseWork();
#pragma once
#include <cstdint>

namespace mwf {
namespace arbiter {

void begin();  // idempotent; creates the 2-slot counting semaphore
bool acquireWork(uint32_t timeoutMs = 30000);
void releaseWork();
int  slots();  // configured slot count (0 = not started)

}  // namespace arbiter
}  // namespace mwf

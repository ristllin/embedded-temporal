// firmware: abortive TLS close (a pattern hardened across prior internal
// firmware generations). External couplings stripped: the CA-bundle / TLS-verify
// helper is dropped — this file is just the RST-close seam the esp transport uses.
#pragma once
#include <WiFiClientSecure.h>
#include <lwip/sockets.h>

namespace mwf {

// Set SO_LINGER with l_linger=0 so close() sends a RST instead of a FIN — leaving NO
// TIME_WAIT PCB behind. We open/close TLS FREQUENTLY (every gRPC unary call + every
// long-poll), and each normal active-close parks a ~1.5-2 KB TIME_WAIT PCB for ~1-2
// min; under load these accumulate faster than they drain and starve the FIXED lwIP
// PCB pool (independent of PSRAM) until even DNS fails. RST-close skips TIME_WAIT.
// Safe when the response is fully read before closing — the RST discards nothing needed.
inline void tlsClose(WiFiClientSecure& c) {
  int fd = c.fd();
  if (fd >= 0) {
    struct linger so;
    so.l_onoff  = 1;
    so.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &so, sizeof(so));
  }
  c.stop();
}

}  // namespace mwf

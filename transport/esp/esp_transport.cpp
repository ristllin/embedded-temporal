// transport esp/: EspTransport bodies. Device-only TU (Arduino/ESP-IDF):
// WiFiClientSecure (mbedTLS, ALPN h2) + lwIP socket options. See esp_transport.h.
#include "esp_transport.h"

#include <NetworkClientSecure.h>
#include <esp_task_wdt.h>
#include <lwip/sockets.h>

namespace mwf_transport {
namespace {

// ALPN list for WiFiClientSecure (NULL-terminated).
const char* kAlpnH2[] = {"h2", nullptr};

constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint32_t kRecvPollMs = 20;  // yield window per H2IO recv when idle

// Optional TLS-arbiter hooks (firmware wires these to mwf::arbiter). Keeping
// them as injected seams avoids a transport→device dependency cycle.
bool (*g_acquireSlot)(uint32_t timeoutMs) = nullptr;
void (*g_releaseSlot)() = nullptr;

// RST-close: SO_LINGER l_linger=0 so close() sends a RST — no TIME_WAIT PCB
// parked in lwIP's fixed pool. Safe here: we only close after a response is
// fully read or the connection is already broken.
void rstClose(NetworkClientSecure& c) {
  int fd = c.fd();
  if (fd >= 0) {
    struct linger so;
    so.l_onoff = 1;
    so.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &so, sizeof(so));
  }
  c.stop();
}

}  // namespace

void espTransportSetArbiterHooks(bool (*acquire)(uint32_t), void (*release)()) {
  g_acquireSlot = acquire;
  g_releaseSlot = release;
}

EspTransport::EspTransport(std::string target, std::string bearer)
    : bearer_(std::move(bearer)) {
  auto colon = target.rfind(':');
  if (colon != std::string::npos) {
    host_ = target.substr(0, colon);
    port_ = std::atoi(target.c_str() + colon + 1);
  } else {
    host_ = std::move(target);
    port_ = 443;
  }
}

EspTransport::~EspTransport() { close(); }

mwf::GrpcResult EspTransport::call(std::string_view fullMethod,
                                   const mwf::Bytes& requestMsg,
                                   const mwf::Metadata& metadata,
                                   int deadlineMs) {
  if (!bearer_.empty() && metadata.count("authorization") == 0) {
    mwf::Metadata md = metadata;
    md["authorization"] = "Bearer " + bearer_;
    return H2Transport::call(fullMethod, requestMsg, md, deadlineMs);
  }
  return H2Transport::call(fullMethod, requestMsg, metadata, deadlineMs);
}

bool EspTransport::openStream(H2IO& io, std::string& authority,
                              std::string& scheme, std::string& err) {
  closeStream();

  if (g_acquireSlot && !g_acquireSlot(10000)) {
    err = "tls arbiter slot timeout";
    return false;
  }
  holding_slot_ = g_acquireSlot != nullptr;

  client_ = new NetworkClientSecure();
  // Fleet TLS posture (see header): no cert validation yet — CA bundle is the
  // documented follow-up shared with the rest of the device fleet.
  client_->setInsecure();
  client_->setAlpnProtocols(kAlpnH2);
  client_->setHandshakeTimeout(kConnectTimeoutMs / 1000);
  if (!client_->connect(host_.c_str(), static_cast<uint16_t>(port_),
                        static_cast<int32_t>(kConnectTimeoutMs))) {
    err = "TLS connect to " + host_ + ":" + std::to_string(port_) + " failed";
    closeStream();
    return false;
  }

  io.ctx = this;
  io.send = &EspTransport::ioSend;
  io.recv = &EspTransport::ioRecv;
  io.nowMs = &EspTransport::ioNowMs;
  authority = host_ + ":" + std::to_string(port_);
  scheme = "https";
  return true;
}

void EspTransport::closeStream() {
  if (client_) {
    rstClose(*client_);
    delete client_;
    client_ = nullptr;
  }
  if (holding_slot_) {
    if (g_releaseSlot) g_releaseSlot();
    holding_slot_ = false;
  }
}

int EspTransport::ioSend(void* ctx, const uint8_t* data, size_t len) {
  auto* t = static_cast<EspTransport*>(ctx);
  if (!t->client_ || !t->client_->connected()) return -1;
  size_t n = t->client_->write(data, len);
  return n > 0 ? static_cast<int>(n) : -1;
}

int EspTransport::ioRecv(void* ctx, uint8_t* buf, size_t len) {
  auto* t = static_cast<EspTransport*>(ctx);
  if (!t->client_) return -1;
  // Feed the task watchdog on EVERY recv chunk — not just the idle branch below.
  // A large history streaming continuously (avail>0 on every poll) would never
  // reach the idle feed, so a >8 s download inside loop() would trip the 8 s WDT
  // mid-stream. This is the one call the pump loop makes per received chunk.
  esp_task_wdt_reset();
  int avail = t->client_->available();
  if (avail > 0) {
    int want = avail < static_cast<int>(len) ? avail : static_cast<int>(len);
    int n = t->client_->read(buf, static_cast<size_t>(want));
    return n > 0 ? n : -1;
  }
  if (!t->client_->connected()) return -1;  // drained + closed
  // Idle inside a long-poll: yield to the scheduler (watchdog already fed above)
  // so a 70 s blocking poll in loop() can't trip the 8 s WDT.
  delay(kRecvPollMs);
  return 0;
}

uint64_t EspTransport::ioNowMs(void*) { return static_cast<uint64_t>(millis()); }

}  // namespace mwf_transport

// transport esp/host/: host twin stream bodies. POSIX TCP + OpenSSL TLS
// (ALPN h2). See h2_host_stream.h.
#include "h2_host_stream.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace mwf_transport {
namespace {

// One-time OpenSSL init (1.1+ auto-inits, this is belt-and-braces).
void ensureOpenSsl() {
  static bool once = [] {
    SSL_library_init();
    SSL_load_error_strings();
    return true;
  }();
  (void)once;
}

constexpr int kRecvPollMs = 100;  // H2IO recv short-poll window

}  // namespace

bool HostH2Stream::connect(const std::string& host, int port, bool tls,
                           std::string& err) {
  close();

  // ── TCP ────────────────────────────────────────────────────────────────────
  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  const std::string portStr = std::to_string(port);
  int rc = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
  if (rc != 0) {
    err = "getaddrinfo: " + std::string(gai_strerror(rc));
    return false;
  }
  for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
    int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      fd_ = fd;
      break;
    }
    ::close(fd);
  }
  freeaddrinfo(res);
  if (fd_ < 0) {
    err = "tcp connect to " + host + ":" + portStr + " failed";
    return false;
  }
  int one = 1;
  setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  // Short receive timeout implements the H2IO recv poll-window semantics.
  struct timeval tv{0, kRecvPollMs * 1000};
  setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (!tls) return true;

  // ── TLS + ALPN h2 ──────────────────────────────────────────────────────────
  ensureOpenSsl();
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    err = "SSL_CTX_new failed";
    close();
    return false;
  }
  SSL_CTX_set_default_verify_paths(ctx);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
  static const unsigned char kAlpn[] = {2, 'h', '2'};
  SSL_CTX_set_alpn_protos(ctx, kAlpn, sizeof(kAlpn));

  SSL* ssl = SSL_new(ctx);
  if (!ssl) {
    err = "SSL_new failed";
    SSL_CTX_free(ctx);
    close();
    return false;
  }
  SSL_set_tlsext_host_name(ssl, host.c_str());          // SNI
  SSL_set1_host(ssl, host.c_str());                     // hostname verification
  SSL_set_fd(ssl, fd_);
  if (SSL_connect(ssl) != 1) {
    unsigned long e = ERR_get_error();
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    err = "TLS handshake failed: " + std::string(buf);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close();
    return false;
  }
  const unsigned char* alpn = nullptr;
  unsigned int alpnLen = 0;
  SSL_get0_alpn_selected(ssl, &alpn, &alpnLen);
  if (alpnLen != 2 || std::memcmp(alpn, "h2", 2) != 0) {
    err = "ALPN did not negotiate h2";
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close();
    return false;
  }
  ssl_ = ssl;
  ssl_ctx_ = ctx;
  return true;
}

void HostH2Stream::close() {
  if (ssl_) {
    SSL_shutdown(static_cast<SSL*>(ssl_));
    SSL_free(static_cast<SSL*>(ssl_));
    ssl_ = nullptr;
  }
  if (ssl_ctx_) {
    SSL_CTX_free(static_cast<SSL_CTX*>(ssl_ctx_));
    ssl_ctx_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

int HostH2Stream::ioSend(void* ctx, const uint8_t* data, size_t len) {
  auto* s = static_cast<HostH2Stream*>(ctx);
  if (s->ssl_) {
    int n = SSL_write(static_cast<SSL*>(s->ssl_), data, static_cast<int>(len));
    return n > 0 ? n : -1;
  }
  ssize_t n = ::send(s->fd_, data, len, 0);
  return n > 0 ? static_cast<int>(n) : -1;
}

int HostH2Stream::ioRecv(void* ctx, uint8_t* buf, size_t len) {
  auto* s = static_cast<HostH2Stream*>(ctx);
  if (s->ssl_) {
    SSL* ssl = static_cast<SSL*>(s->ssl_);
    int n = SSL_read(ssl, buf, static_cast<int>(len));
    if (n > 0) return n;
    int e = SSL_get_error(ssl, n);
    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return 0;
    if (e == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK))
      return 0;  // SO_RCVTIMEO expired — the H2IO poll-window case
    return -1;   // closed or fatal
  }
  ssize_t n = ::recv(s->fd_, buf, len, 0);
  if (n > 0) return static_cast<int>(n);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
  return -1;  // 0 = orderly close, <0 = error
}

uint64_t HostH2Stream::ioNowMs(void*) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

H2IO HostH2Stream::io() {
  H2IO io;
  io.ctx = this;
  io.send = &HostH2Stream::ioSend;
  io.recv = &HostH2Stream::ioRecv;
  io.nowMs = &HostH2Stream::ioNowMs;
  return io;
}

// ── H2HostTransport ──────────────────────────────────────────────────────────
H2HostTransport::H2HostTransport(std::string target, std::string bearer, int tls)
    : bearer_(std::move(bearer)) {
  auto colon = target.rfind(':');
  if (colon != std::string::npos) {
    host_ = target.substr(0, colon);
    port_ = std::atoi(target.c_str() + colon + 1);
  } else {
    host_ = std::move(target);
    port_ = 443;
  }
  // Unset (-1) infers TLS from port 443 — a convenience for this host test
  // twin only. The production desktop transport does no inference: it is
  // TLS-on unless explicitly opted out.
  tls_ = tls >= 0 ? (tls != 0) : (port_ == 443);
}

mwf::GrpcResult H2HostTransport::call(std::string_view fullMethod,
                                      const mwf::Bytes& requestMsg,
                                      const mwf::Metadata& metadata,
                                      int deadlineMs) {
  // Match DesktopTransport: attach the ctor bearer unless the caller set one.
  if (!bearer_.empty() && metadata.count("authorization") == 0) {
    mwf::Metadata md = metadata;
    md["authorization"] = "Bearer " + bearer_;
    return H2Transport::call(fullMethod, requestMsg, md, deadlineMs);
  }
  return H2Transport::call(fullMethod, requestMsg, metadata, deadlineMs);
}

bool H2HostTransport::openStream(H2IO& io, std::string& authority,
                                 std::string& scheme, std::string& err) {
  if (!stream_.connect(host_, port_, tls_, err)) return false;
  io = stream_.io();
  authority = host_ + ":" + std::to_string(port_);
  scheme = tls_ ? "https" : "http";
  return true;
}

void H2HostTransport::closeStream() { stream_.close(); }

}  // namespace mwf_transport

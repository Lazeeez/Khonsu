#include "net.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#if KHONSU_WITH_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

namespace khonsu::native {

namespace {

void set_timeouts(int fd, int timeout_ms) {
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  const int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // stream tokens without Nagle delay
}

class TcpStream final : public Stream {
public:
  explicit TcpStream(int fd) : fd_(fd) {}
  ~TcpStream() override {
    if (fd_ >= 0) ::close(fd_);
  }
  long read(void* buf, std::size_t n) override {
    for (;;) {
      const ssize_t r = ::recv(fd_, buf, n, 0);
      if (r < 0 && errno == EINTR) continue;
      return static_cast<long>(r);
    }
  }
  bool write_all(const void* buf, std::size_t n) override {
    auto* p = static_cast<const char*>(buf);
    while (n > 0) {
      const ssize_t w = ::send(fd_, p, n, MSG_NOSIGNAL);
      if (w < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      p += w;
      n -= static_cast<std::size_t>(w);
    }
    return true;
  }
  void shutdown_write() override { ::shutdown(fd_, SHUT_WR); }
  [[nodiscard]] int fd() const { return fd_; }

private:
  int fd_;
};

// Non-blocking connect with a deadline, then back to blocking with I/O timeouts.
int connect_fd(const Endpoint& e, int timeout_ms, int io_timeout_ms, std::string* error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port = std::to_string(e.port);
  if (const int rc = ::getaddrinfo(e.host.c_str(), port.c_str(), &hints, &res); rc != 0) {
    if (error) *error = "resolve " + e.host + ": " + ::gai_strerror(rc);
    return -1;
  }
  int fd = -1;
  std::string last = "no addresses";
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0) continue;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
      pollfd p{fd, POLLOUT, 0};
      rc = ::poll(&p, 1, timeout_ms) == 1 ? 0 : -1;
      int soerr = 0;
      socklen_t len = sizeof soerr;
      if (rc == 0 && (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0 || soerr != 0)) {
        errno = soerr;
        rc = -1;
      } else if (rc != 0) {
        errno = ETIMEDOUT;
      }
    }
    if (rc == 0) {
      ::fcntl(fd, F_SETFL, flags);
      set_timeouts(fd, io_timeout_ms);
      break;
    }
    last = std::strerror(errno);
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);
  if (fd < 0 && error) *error = "connect " + e.host + ":" + port + ": " + last;
  return fd;
}

#if KHONSU_WITH_TLS
SSL_CTX* tls_ctx() {
  static std::once_flag once;
  static SSL_CTX* ctx = nullptr;
  std::call_once(once, [] {
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_default_verify_paths(ctx);
    // A statically linked OpenSSL only knows the CA directory of the system it was built on.
    // Also load the distribution's bundle, so the prebuilt binary verifies TLS everywhere.
    if (!std::getenv("SSL_CERT_FILE"))
      for (const char* bundle : {"/etc/ssl/certs/ca-certificates.crt",   // Debian, Ubuntu, Arch, WSL
                                 "/etc/pki/tls/certs/ca-bundle.crt",     // Fedora, RHEL
                                 "/etc/ssl/ca-bundle.pem",               // openSUSE
                                 "/etc/ssl/cert.pem"})                   // Alpine
        if (::access(bundle, R_OK) == 0 && SSL_CTX_load_verify_locations(ctx, bundle, nullptr) == 1) break;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
  });
  return ctx;
}

std::string ssl_error() {
  char buf[256];
  ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
  return buf;
}

class TlsStream final : public Stream {
public:
  TlsStream(int fd, SSL* ssl) : tcp_(fd), ssl_(ssl) {}
  ~TlsStream() override {
    SSL_shutdown(ssl_);
    SSL_free(ssl_);
  }
  long read(void* buf, std::size_t n) override {
    const int r = SSL_read(ssl_, buf, static_cast<int>(n > 1 << 30 ? 1 << 30 : n));
    if (r > 0) return r;
    return SSL_get_error(ssl_, r) == SSL_ERROR_ZERO_RETURN ? 0 : -1;
  }
  bool write_all(const void* buf, std::size_t n) override {
    auto* p = static_cast<const char*>(buf);
    while (n > 0) {
      const int w = SSL_write(ssl_, p, static_cast<int>(n > 1 << 30 ? 1 << 30 : n));
      if (w <= 0) return false;
      p += w;
      n -= static_cast<std::size_t>(w);
    }
    return true;
  }

private:
  TcpStream tcp_;  // owns and closes the socket after SSL_free
  SSL* ssl_;
};
#endif

}  // namespace

void ignore_sigpipe() { std::signal(SIGPIPE, SIG_IGN); }

bool tls_available() {
#if KHONSU_WITH_TLS
  return tls_ctx() != nullptr;
#else
  return false;
#endif
}

bool parse_url(std::string_view url, Endpoint& out, std::string* error) {
  out = {};
  if (url.substr(0, 7) == "http://") {
    url.remove_prefix(7);
    out.port = 80;
  } else if (url.substr(0, 8) == "https://") {
    url.remove_prefix(8);
    out.tls = true;
    out.port = 443;
  } else {
    if (error) *error = "URL must start with http:// or https://";
    return false;
  }
  const auto slash = url.find('/');
  std::string_view hostport = url.substr(0, slash);
  std::string_view path = slash == std::string_view::npos ? std::string_view() : url.substr(slash);
  while (!path.empty() && path.back() == '/') path.remove_suffix(1);
  out.base_path = std::string(path);
  std::size_t colon = std::string_view::npos;
  if (!hostport.empty() && hostport.front() == '[') {  // [::1]:8080
    const auto close = hostport.find(']');
    if (close == std::string_view::npos) {
      if (error) *error = "bad IPv6 host";
      return false;
    }
    out.host = std::string(hostport.substr(1, close - 1));
    if (close + 1 < hostport.size() && hostport[close + 1] == ':') colon = close + 1;
  } else {
    colon = hostport.rfind(':');
    out.host = std::string(hostport.substr(0, colon));
  }
  if (colon != std::string_view::npos) {
    const auto p = hostport.substr(colon + 1);
    unsigned v = 0;
    if (std::from_chars(p.data(), p.data() + p.size(), v).ec != std::errc() || v == 0 || v > 65535) {
      if (error) *error = "bad port";
      return false;
    }
    out.port = static_cast<std::uint16_t>(v);
  }
  if (out.host.empty()) {
    if (error) *error = "missing host";
    return false;
  }
  return true;
}

std::string to_string(const Endpoint& e) {
  const bool default_port = (e.tls && e.port == 443) || (!e.tls && e.port == 80);
  return std::string(e.tls ? "https://" : "http://") + e.host + (default_port ? "" : ":" + std::to_string(e.port)) +
         e.base_path;
}

std::unique_ptr<Stream> connect(const Endpoint& e, int timeout_ms, std::string* error, int io_timeout_ms) {
  const int fd = connect_fd(e, timeout_ms, io_timeout_ms < 0 ? timeout_ms : io_timeout_ms, error);
  if (fd < 0) return nullptr;
  if (!e.tls) return std::make_unique<TcpStream>(fd);
#if KHONSU_WITH_TLS
  SSL_CTX* ctx = tls_ctx();
  SSL* ssl = ctx ? SSL_new(ctx) : nullptr;
  if (!ssl) {
    ::close(fd);
    if (error) *error = "TLS init failed";
    return nullptr;
  }
  SSL_set_fd(ssl, fd);
  SSL_set_tlsext_host_name(ssl, e.host.c_str());  // SNI
  SSL_set1_host(ssl, e.host.c_str());             // hostname verification
  if (SSL_connect(ssl) != 1) {
    if (error) *error = "TLS handshake with " + e.host + " failed: " + ssl_error();
    SSL_free(ssl);
    ::close(fd);
    return nullptr;
  }
  return std::make_unique<TlsStream>(fd, ssl);
#else
  ::close(fd);
  if (error) *error = "this build has no TLS support (rebuild with OpenSSL)";
  return nullptr;
#endif
}

Listener::~Listener() {
  shutdown();
  close();
}

bool Listener::listen(std::uint16_t port, std::string* error, const std::string& host) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    if (error) *error = std::strerror(errno);
    return false;
  }
  fd_ = fd;
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
    if (error) *error = "bad bind address " + host;
    return false;
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 128) != 0) {
    if (error) *error = "bind " + host + ":" + std::to_string(port) + ": " + std::strerror(errno);
    return false;
  }
  socklen_t len = sizeof a;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
  port_ = ntohs(a.sin_port);
  return true;
}

std::unique_ptr<Stream> Listener::accept() {
  const int lfd = fd_.load();
  while (lfd >= 0 && !shut_.load()) {
    const int fd = ::accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd >= 0) {
      if (shut_.load()) {
        ::close(fd);
        break;
      }
      set_timeouts(fd, 600'000);
      return std::make_unique<TcpStream>(fd);
    }
    if (errno != EINTR && errno != ECONNABORTED) break;
  }
  return nullptr;
}

void Listener::shutdown() {
  shut_ = true;
  if (const int fd = fd_.load(); fd >= 0) ::shutdown(fd, SHUT_RDWR);  // wakes a blocked accept()
}

void Listener::close() {
  if (const int fd = fd_.exchange(-1); fd >= 0) ::close(fd);
}

bool BufReader::fill() {
  if (pos_ > 0) {  // compact consumed bytes so long streams don't grow the buffer
    buf_.erase(0, pos_);
    pos_ = 0;
  }
  char tmp[16 * 1024];
  const long r = s_.read(tmp, sizeof tmp);
  if (r <= 0) return false;
  buf_.append(tmp, static_cast<std::size_t>(r));
  return true;
}

bool BufReader::line(std::string& out, std::size_t max) {
  out.clear();
  for (;;) {
    const auto nl = buf_.find('\n', pos_);
    if (nl != std::string::npos) {
      out.assign(buf_, pos_, nl - pos_);
      pos_ = nl + 1;
      if (!out.empty() && out.back() == '\r') out.pop_back();
      return true;
    }
    if (buf_.size() - pos_ > max) return false;
    if (!fill()) return false;
  }
}

bool BufReader::exact(std::size_t n, std::string& out) {
  while (buf_.size() - pos_ < n)
    if (!fill()) return false;
  out.append(buf_, pos_, n);
  pos_ += n;
  return true;
}

bool BufReader::some(std::string& out, std::size_t max) {
  if (pos_ == buf_.size() && !fill()) return false;
  const std::size_t n = std::min(max, buf_.size() - pos_);
  out.append(buf_, pos_, n);
  pos_ += n;
  return true;
}

}  // namespace khonsu::native

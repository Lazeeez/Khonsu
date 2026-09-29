// Khonsu — blocking sockets with optional TLS (OpenSSL), and a buffered reader.
//
// The proxy uses one thread per connection (see docs/adr/0002). Agents keep at
// most tens of connections open and every exchange is dominated by model
// latency, so an event loop would add complexity without measurable benefit.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace khonsu::native {

class Stream {
public:
  virtual ~Stream() = default;
  // Reads up to n bytes; returns 0 on orderly EOF, -1 on error or timeout.
  virtual long read(void* buf, std::size_t n) = 0;
  virtual bool write_all(const void* buf, std::size_t n) = 0;
  bool write_all(std::string_view s) { return write_all(s.data(), s.size()); }
  virtual void shutdown_write() {}
};

struct Endpoint {
  bool tls = false;
  std::string host;
  std::uint16_t port = 0;
  std::string base_path;  // e.g. "/v1" (no trailing slash)
};

// Parses http[s]://host[:port][/base]. Returns false on malformed input.
bool parse_url(std::string_view url, Endpoint& out, std::string* error);
[[nodiscard]] std::string to_string(const Endpoint& e);

// Connects (with TLS + certificate/hostname verification when e.tls).
std::unique_ptr<Stream> connect(const Endpoint& e, int timeout_ms, std::string* error);
[[nodiscard]] bool tls_available();

class Listener {
public:
  ~Listener();
  // Binds host:port (0 = ephemeral). Loopback by default; the recording proxy
  // never listens elsewhere, the shared store server may (explicit --bind).
  bool listen(std::uint16_t port, std::string* error, const std::string& host = "127.0.0.1");
  // Blocks; returns nullptr after shutdown().
  std::unique_ptr<Stream> accept();
  // Wakes a thread blocked in accept(). Safe to call from any thread.
  void shutdown();
  // Releases the socket. Call only once no thread can be inside accept()
  // (closing an fd another thread is blocked on races with fd-number reuse).
  void close();
  [[nodiscard]] std::uint16_t port() const { return port_; }

private:
  std::atomic<int> fd_{-1};
  std::atomic<bool> shut_{false};
  std::uint16_t port_ = 0;
};

// Line/length-oriented reader over a Stream.
class BufReader {
public:
  explicit BufReader(Stream& s) : s_(s) {}
  // Reads through "\n"; strips "\r\n". False on EOF/error or if the line exceeds max.
  bool line(std::string& out, std::size_t max = 64 * 1024);
  // Reads exactly n bytes, appended to out.
  bool exact(std::size_t n, std::string& out);
  // Reads whatever is available (at least 1 byte), appended to out. False at EOF.
  bool some(std::string& out, std::size_t max = 64 * 1024);

private:
  bool fill();
  Stream& s_;
  std::string buf_;
  std::size_t pos_ = 0;
};

void ignore_sigpipe();

}  // namespace khonsu::native

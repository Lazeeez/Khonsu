// Khonsu — minimal HTTP/1.1 for the recording proxy.
//
// Supports what OpenAI-compatible clients and servers actually send:
// Content-Length and chunked bodies, server-sent-event streams, and
// read-until-close responses. One request per connection (Connection: close).
#pragma once

#include "net.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace khonsu::native {

struct Headers {
  std::vector<std::pair<std::string, std::string>> items;
  [[nodiscard]] std::string_view get(std::string_view name) const;  // case-insensitive, first match
  void set(std::string name, std::string value);
};

struct HttpRequest {
  std::string method, target, version;
  Headers headers;
  std::string body;
};

struct ResponseHead {
  int status = 0;
  std::string reason;
  Headers headers;
};

// Limits guard the proxy against runaway or hostile peers.
inline constexpr std::size_t kMaxBody = 256u << 20;

bool read_request(BufReader& in, HttpRequest& out, std::string* error);
bool read_response_head(BufReader& in, ResponseHead& out, std::string* error);

// Streams a message body regardless of framing (Content-Length, chunked, or close).
class BodyReader {
public:
  BodyReader(BufReader& in, const Headers& h, bool no_body = false);
  // Appends the next piece of body to `out`. Returns false at the end of the body
  // (check error() to tell a clean end from a failure).
  bool next(std::string& out);
  [[nodiscard]] bool error() const { return error_; }

private:
  enum class Mode { None, Length, Chunked, Close } mode_;
  BufReader& in_;
  std::size_t remaining_ = 0;
  bool done_ = false, error_ = false;
};

// Writes a response to a client, either with a known length or chunked.
class ResponseWriter {
public:
  explicit ResponseWriter(Stream& s) : s_(s) {}
  bool head(int status, std::string_view content_type, const Headers& extra, bool chunked, std::size_t length = 0);
  bool chunk(std::string_view data);  // chunked mode: one HTTP chunk; length mode: raw bytes
  bool finish();

private:
  Stream& s_;
  bool chunked_ = false;
};

[[nodiscard]] std::string_view reason_phrase(int status);
bool write_json_error(Stream& s, int status, std::string_view type, std::string_view message);

}  // namespace khonsu::native

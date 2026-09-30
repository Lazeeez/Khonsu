#include "http.hpp"

#include "khonsu/json.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace khonsu::native {

namespace {

bool iequals(std::string_view a, std::string_view b) {
  // ASCII case-insensitive; only letters fold (x | 0x20 alone would also equate '@' and '`').
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c | 0x20) : c; };
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return lower(x) == lower(y); });
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

bool read_headers(BufReader& in, Headers& h, std::string* error) {
  std::string line;
  for (int n = 0;; ++n) {
    if (n > 200) {
      if (error) *error = "too many headers";
      return false;
    }
    if (!in.line(line, 16 * 1024)) {
      if (error) *error = "connection closed while reading headers";
      return false;
    }
    if (line.empty()) return true;
    const auto colon = line.find(':');
    if (colon == std::string::npos || colon == 0) {
      if (error) *error = "malformed header line";
      return false;
    }
    h.items.emplace_back(std::string(trim(std::string_view(line).substr(0, colon))),
                         std::string(trim(std::string_view(line).substr(colon + 1))));
  }
}

bool parse_size(std::string_view s, std::size_t& out, int base = 10) {
  s = trim(s);
  if (const auto semi = s.find(';'); semi != std::string_view::npos) s = trim(s.substr(0, semi));  // chunk ext
  if (s.empty()) return false;
  std::uint64_t v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return false;
  out = static_cast<std::size_t>(v);
  return true;
}

}  // namespace

std::string_view Headers::get(std::string_view name) const {
  for (const auto& [k, v] : items)
    if (iequals(k, name)) return v;
  return {};
}

void Headers::set(std::string name, std::string value) {
  for (auto& [k, v] : items)
    if (iequals(k, name)) {
      v = std::move(value);
      return;
    }
  items.emplace_back(std::move(name), std::move(value));
}

bool read_request(BufReader& in, HttpRequest& out, std::string* error) {
  std::string line;
  if (!in.line(line, 16 * 1024)) {
    if (error) *error = "";  // idle close, not an error worth reporting
    return false;
  }
  const auto sp1 = line.find(' ');
  const auto sp2 = line.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) {
    if (error) *error = "malformed request line";
    return false;
  }
  out.method = line.substr(0, sp1);
  out.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  out.version = line.substr(sp2 + 1);
  if (!read_headers(in, out.headers, error)) return false;
  // RFC 9112 §6.3: a request without Content-Length or Transfer-Encoding has no body.
  const bool no_body = out.headers.get("content-length").empty() && out.headers.get("transfer-encoding").empty();
  BodyReader body(in, out.headers, no_body);
  while (body.next(out.body))
    if (out.body.size() > kMaxBody) {
      if (error) *error = "request body too large";
      return false;
    }
  if (body.error()) {
    if (error) *error = "malformed request body";
    return false;
  }
  return true;
}

bool read_response_head(BufReader& in, ResponseHead& out, std::string* error) {
  std::string line;
  if (!in.line(line, 16 * 1024)) {
    if (error) *error = "upstream closed the connection before responding";
    return false;
  }
  // HTTP/1.1 200 OK
  const auto sp1 = line.find(' ');
  std::size_t status = 0;
  if (line.substr(0, 5) != "HTTP/" || sp1 == std::string::npos ||
      !parse_size(std::string_view(line).substr(sp1 + 1, 3), status)) {
    if (error) *error = "malformed status line from upstream";
    return false;
  }
  out.status = static_cast<int>(status);
  out.reason = sp1 + 5 <= line.size() ? line.substr(sp1 + 5) : "";
  if (!read_headers(in, out.headers, error)) return false;
  if (out.status >= 100 && out.status < 200 && out.status != 101) {  // skip interim responses (100, 103, ...)
    out = {};
    return read_response_head(in, out, error);
  }
  return true;
}

BodyReader::BodyReader(BufReader& in, const Headers& h, bool no_body) : mode_(Mode::None), in_(in) {
  if (no_body) {
    done_ = true;
    return;
  }
  if (iequals(h.get("transfer-encoding"), "chunked")) {
    mode_ = Mode::Chunked;
  } else if (const auto cl = h.get("content-length"); !cl.empty()) {
    mode_ = Mode::Length;
    if (!parse_size(cl, remaining_)) {
      error_ = done_ = true;
      return;
    }
    done_ = remaining_ == 0;
  } else {
    mode_ = Mode::Close;
  }
}

bool BodyReader::next(std::string& out) {
  if (done_) return false;
  switch (mode_) {
    case Mode::None:
      done_ = true;
      return false;
    case Mode::Length: {
      const std::size_t before = out.size();
      if (!in_.some(out, std::min<std::size_t>(remaining_, 64 * 1024))) {
        error_ = done_ = true;
        return false;
      }
      remaining_ -= out.size() - before;
      done_ = remaining_ == 0;
      return true;
    }
    case Mode::Close: {
      if (!in_.some(out)) {
        done_ = true;  // EOF terminates the body
        return false;
      }
      return true;
    }
    case Mode::Chunked: {
      std::string line;
      std::size_t n = 0;
      if (!in_.line(line) || !parse_size(line, n, 16) || n > kMaxBody) {  // never buffer an absurd chunk
        error_ = done_ = true;
        return false;
      }
      if (n == 0) {  // last chunk; drain trailers
        while (in_.line(line) && !line.empty()) {
        }
        done_ = true;
        return false;
      }
      if (!in_.exact(n, out) || !in_.line(line) || !line.empty()) {
        error_ = done_ = true;
        return false;
      }
      return true;
    }
  }
  return false;
}

bool ResponseWriter::head(int status, std::string_view content_type, const Headers& extra, bool chunked,
                          std::size_t length) {
  chunked_ = chunked;
  std::string h = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason_phrase(status)) + "\r\n";
  if (!content_type.empty()) h += "Content-Type: " + std::string(content_type) + "\r\n";
  for (const auto& [k, v] : extra.items) h += k + ": " + v + "\r\n";
  h += chunked ? "Transfer-Encoding: chunked\r\n" : "Content-Length: " + std::to_string(length) + "\r\n";
  h += "Connection: close\r\n\r\n";
  return s_.write_all(h);
}

bool ResponseWriter::chunk(std::string_view data) {
  if (data.empty()) return true;
  if (!chunked_) return s_.write_all(data);
  char size[24];
  const int n = std::snprintf(size, sizeof size, "%zx\r\n", data.size());
  return s_.write_all(size, static_cast<std::size_t>(n)) && s_.write_all(data) && s_.write_all("\r\n", 2);
}

bool ResponseWriter::finish() { return chunked_ ? s_.write_all("0\r\n\r\n", 5) : true; }

std::string_view reason_phrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Status";
  }
}

bool write_json_error(Stream& s, int status, std::string_view type, std::string_view message) {
  Json j;
  j.begin_object().key("error").begin_object().field("type", type).field("message", message).end_object().end_object();
  const std::string body = j.take();
  ResponseWriter w(s);
  return w.head(status, "application/json", {}, false, body.size()) && w.chunk(body) && w.finish();
}

}  // namespace khonsu::native

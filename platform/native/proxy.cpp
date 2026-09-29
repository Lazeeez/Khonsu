#include "proxy.hpp"

#include "http.hpp"

#include "khonsu/json.hpp"

#include <algorithm>
#include <cstdio>

namespace khonsu::native {

namespace {

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return (x | 0x20) == (y | 0x20);
         });
}

// Headers forwarded to the upstream model API. Everything else (hop-by-hop,
// SDK telemetry such as x-stainless-*) is dropped.
constexpr std::string_view kForward[] = {
    "authorization", "api-key",     "x-api-key",       "openai-organization", "openai-project",
    "openai-beta",   "anthropic-version", "anthropic-beta", "content-type",   "accept",
    "user-agent",
};
// Headers whose *values* are secrets: recorded only as "<redacted>".
constexpr std::string_view kSecret[] = {"authorization", "api-key", "x-api-key", "openai-organization",
                                        "openai-project"};
// Headers recorded verbatim (they can change the response and are not secrets).
constexpr std::string_view kRecord[] = {"content-type", "accept", "openai-beta", "anthropic-version",
                                        "anthropic-beta"};

template <std::size_t N>
bool in_list(std::string_view name, const std::string_view (&list)[N]) {
  return std::any_of(std::begin(list), std::end(list), [&](std::string_view x) { return iequals(x, name); });
}

std::string recorded_headers(const Headers& h) {
  Json j;
  j.begin_object();
  for (const auto& [k, v] : h.items) {
    if (in_list(k, kSecret)) j.field(k, "<redacted>");
    else if (in_list(k, kRecord)) j.field(k, v);
  }
  j.end_object();
  return j.take();
}

std::string host_header(const Endpoint& e) {
  const bool default_port = (e.tls && e.port == 443) || (!e.tls && e.port == 80);
  const bool v6 = e.host.find(':') != std::string::npos;
  return (v6 ? "[" + e.host + "]" : e.host) + (default_port ? "" : ":" + std::to_string(e.port));
}

}  // namespace

struct Incoming {
  HttpRequest http;
  rec::Request rec;
  std::uint64_t t_us = 0;
};

// =========================================================================================
// RecordingWriter
// =========================================================================================

void RecordingWriter::append(FrameType t, std::uint16_t actor, std::uint64_t t_us,
                             const std::vector<std::byte>& payload, bool sync) {
  std::lock_guard lock(mu_);
  log_.append(t, actor, t_us, payload);
  chain_ = rec::chain_step(chain_, log_.last_crc());
  if (sync) log_.sync();
}

void RecordingWriter::run_start(const rec::RunInfo& info) { append(FrameType::RunStart, 0, 0, rec::encode(info), true); }

void RecordingWriter::request(const rec::Request& r, std::uint64_t t_us) {
  append(FrameType::HttpRequest, 1, t_us, rec::encode(r), false);  // made durable with its response
}

void RecordingWriter::response(const rec::Response& r, std::uint64_t t_us) {
  append(FrameType::HttpResponse, 1, t_us, rec::encode(r), true);
  exchanges_.fetch_add(1);
}

void RecordingWriter::run_end(rec::RunEnd end, std::uint64_t t_us) {
  std::lock_guard lock(mu_);
  end.chain = chain_;
  end.exchanges = exchanges_.load();
  log_.append(FrameType::RunEnd, 0, t_us, rec::encode(end));
  chain_ = rec::chain_step(chain_, log_.last_crc());
  log_.sync();
}

// =========================================================================================
// Proxy
// =========================================================================================

Proxy::Proxy(ProxyConfig cfg) : cfg_(std::move(cfg)), t0_(std::chrono::steady_clock::now()) {
  if (cfg_.replay) index_ = std::make_unique<rec::ReplayIndex>(*cfg_.replay);
}

Proxy::~Proxy() { stop(); }

std::uint64_t Proxy::elapsed_us() const {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0_).count());
}

void Proxy::log(const std::string& line) const {
  if (!cfg_.quiet) std::fprintf(stderr, "\033[2m[khonsu]\033[0m %s\n", line.c_str());
}

bool Proxy::start(std::uint16_t port, std::string* error) {
  ignore_sigpipe();
  if (!listener_.listen(port, error)) return false;
  acceptor_ = std::thread([this] { accept_loop(); });
  return true;
}

void Proxy::stop() {
  {
    std::lock_guard lock(mu_);
    if (stopping_) return;
    stopping_ = true;
  }
  listener_.shutdown();
  if (acceptor_.joinable()) acceptor_.join();
  listener_.close();
  std::unique_lock lock(mu_);
  idle_.wait(lock, [this] { return active_ == 0; });
}

std::vector<Divergence> Proxy::divergences() const {
  std::lock_guard lock(mu_);
  return divergences_;
}

void Proxy::accept_loop() {
  while (auto s = listener_.accept()) {
    {
      std::lock_guard lock(mu_);
      if (stopping_) break;
      ++active_;
    }
    std::thread([this, conn = std::move(s)]() mutable {
      handle(std::move(conn));
      std::lock_guard lock(mu_);
      if (--active_ == 0) idle_.notify_all();
    }).detach();
  }
}

void Proxy::handle(std::unique_ptr<Stream> s) {
  BufReader reader(*s);
  Incoming in;
  std::string err;
  if (!read_request(reader, in.http, &err)) {
    if (!err.empty()) write_json_error(*s, 400, "khonsu_bad_request", err);
    return;
  }
  in.t_us = elapsed_us();
  stats_.requests.fetch_add(1);

  if (in.http.target.rfind(cfg_.route, 0) != 0) {
    write_json_error(*s, 404, "khonsu_no_route",
                     "khonsu proxies " + cfg_.route + "/*; got " + in.http.method + " " + in.http.target);
    return;
  }

  // Identify the request: match key over the canonical body, plus its occurrence number.
  rec::Request& r = in.rec;
  r.seq = seq_.fetch_add(1);
  r.t_us = in.t_us;
  r.method = in.http.method;
  r.path = in.http.target;
  r.body = rec::canonical_body(in.http.body);
  r.key = rec::match_key(r.method, r.path, r.body);
  r.headers_json = recorded_headers(in.http.headers);
  {
    std::lock_guard lock(mu_);
    r.occurrence = occurrences_[r.key]++;
  }

  bool after_miss = false;
  if (cfg_.mode == Mode::Replay) {
    rec::ReplayIndex::Miss miss;
    if (const rec::Exchange* ex = index_->take(r.key, r.body, &miss)) {
      stats_.replayed.fetch_add(1);
      serve_recorded(*s, *ex);
      if (cfg_.writer) {  // a replay can itself be recorded (e.g. as the base of a fork)
        rec::Response copy = *ex->resp;
        copy.seq = r.seq;
        copy.from_recording = true;
        cfg_.writer->request(r, in.t_us);
        cfg_.writer->response(copy, elapsed_us());
      }
      return;
    }
    stats_.divergences.fetch_add(1);
    {
      std::lock_guard lock(mu_);
      divergences_.push_back({r.seq, r.path, miss});
    }
    std::string msg = "divergence at request #" + std::to_string(r.seq) + " (" + r.method + " " + r.path + ")";
    if (!miss.field_path.empty()) {
      msg += ": recording expected request #" + std::to_string(miss.expected_seq) + ", first difference at `" +
             miss.field_path + "`";
    }
    log("\033[33m" + msg + "\033[0m");
    if (!miss.field_path.empty() && !miss.expected_value.empty()) {
      log("  recorded: " + miss.expected_value);
      log("  now:      " + miss.actual_value);
    }
    if (cfg_.on_miss == OnMiss::Fail || !cfg_.upstream) {
      write_json_error(*s, 409, "khonsu_divergence",
                       msg + (cfg_.on_miss == OnMiss::Live ? " (no upstream configured for live fallback)"
                                                           : " (rerun with --on-miss live to continue against the model)"));
      return;
    }
    after_miss = true;
  }
  forward_live(*s, in, after_miss);
}

void Proxy::serve_recorded(Stream& client, const rec::Exchange& ex) {
  const rec::Response& resp = *ex.resp;
  ResponseWriter w(client);
  Headers extra;
  extra.set("X-Khonsu", "replayed seq=" + std::to_string(ex.req.seq));
  if (!resp.streamed) {
    w.head(static_cast<int>(resp.status), resp.content_type, extra, false, resp.body.size());
    w.chunk(resp.body);
    w.finish();
    return;
  }
  // Re-emit the stream with the recorded chunk boundaries (and timing, if asked).
  w.head(static_cast<int>(resp.status), resp.content_type, extra, true);
  const auto start = std::chrono::steady_clock::now();
  std::size_t prev = 0;
  for (const rec::Chunk& c : resp.chunks) {
    if (cfg_.realtime) std::this_thread::sleep_until(start + std::chrono::microseconds(c.t_us));
    const std::size_t end = std::min<std::size_t>(c.end, resp.body.size());
    if (end > prev && !w.chunk(std::string_view(resp.body).substr(prev, end - prev))) return;
    prev = std::max(prev, end);
  }
  if (prev < resp.body.size()) w.chunk(std::string_view(resp.body).substr(prev));
  w.finish();
}

void Proxy::forward_live(Stream& client, const Incoming& in, bool after_miss) {
  if (!cfg_.upstream) {
    write_json_error(client, 502, "khonsu_no_upstream", "no upstream configured");
    return;
  }
  const Endpoint& up = *cfg_.upstream;
  std::string err;
  auto upstream = connect(up, cfg_.upstream_timeout_ms, &err);
  if (!upstream) {
    stats_.errors.fetch_add(1);
    log("\033[31mupstream error: " + err + "\033[0m");
    write_json_error(client, 502, "khonsu_upstream_unreachable", err);
    return;
  }

  // Rewrite /v1/<rest> onto the upstream's base path.
  const std::string rest = in.http.target.substr(cfg_.route.size());
  std::string req = in.http.method + " " + up.base_path + rest + " HTTP/1.1\r\nHost: " + host_header(up) + "\r\n";
  for (const auto& [k, v] : in.http.headers.items)
    if (in_list(k, kForward)) req += k + ": " + v + "\r\n";
  req += "Accept-Encoding: identity\r\n";  // recordings must be inspectable, not gzip
  req += "Content-Length: " + std::to_string(in.http.body.size()) + "\r\nConnection: close\r\n\r\n";
  if (cfg_.writer) cfg_.writer->request(in.rec, in.t_us);
  if (!upstream->write_all(req) || !upstream->write_all(in.http.body)) {
    stats_.errors.fetch_add(1);
    write_json_error(client, 502, "khonsu_upstream_write_failed", "failed to send request upstream");
    return;
  }

  BufReader ur(*upstream);
  ResponseHead head;
  if (!read_response_head(ur, head, &err)) {
    stats_.errors.fetch_add(1);
    write_json_error(client, 502, "khonsu_upstream_bad_response", err);
    return;
  }

  rec::Response resp;
  resp.seq = in.rec.seq;
  resp.status = static_cast<std::uint32_t>(head.status);
  resp.content_type = std::string(head.headers.get("content-type"));
  resp.streamed = resp.content_type.rfind("text/event-stream", 0) == 0;

  const bool known_length = !head.headers.get("content-length").empty() &&
                            head.headers.get("transfer-encoding").empty() && !resp.streamed;
  Headers extra;
  if (const auto id = head.headers.get("x-request-id"); !id.empty()) extra.set("X-Request-Id", std::string(id));
  extra.set("X-Khonsu", after_miss ? "live-after-divergence" : "live");

  BodyReader body(ur, head.headers, head.status == 204 || head.status == 304);
  ResponseWriter w(client);
  // Durability contract: the response frame is fsync'd *before* the agent can
  // observe a complete response (before the body for buffered responses, before
  // the terminating chunk for streams). So if the agent saw it, it's recorded.
  auto commit = [&] {
    resp.latency_us = elapsed_us() - in.t_us;
    if (body.error()) {
      stats_.errors.fetch_add(1);
      log("\033[31mupstream stream ended abnormally for request #" + std::to_string(in.rec.seq) + "\033[0m");
    }
    stats_.live.fetch_add(1);
    if (cfg_.writer) {
      cfg_.writer->response(resp, elapsed_us());
      stats_.recorded.fetch_add(1);
    }
  };
  if (known_length) {
    // Buffer so the client sees a normal Content-Length response.
    while (body.next(resp.body)) {
    }
    commit();
    (void)(w.head(head.status, resp.content_type, extra, false, resp.body.size()) && w.chunk(resp.body) &&
           w.finish());
  } else {
    bool client_ok = w.head(head.status, resp.content_type, extra, true);
    const auto t_req = in.t_us;
    std::string piece;
    for (;;) {
      piece.clear();
      if (!body.next(piece)) break;
      resp.body += piece;
      resp.chunks.push_back({static_cast<std::uint32_t>(resp.body.size()), elapsed_us() - t_req});
      if (client_ok) client_ok = w.chunk(piece);  // keep draining upstream even if the client left
    }
    commit();
    if (client_ok) w.finish();
  }
}

}  // namespace khonsu::native

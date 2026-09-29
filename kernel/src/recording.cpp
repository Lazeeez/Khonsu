#include "khonsu/recording.hpp"

#include <algorithm>
#include <charconv>

namespace khonsu::rec {

namespace {

std::string trunc(std::string_view s, std::size_t n) {
  if (s.size() <= n) return std::string(s);
  // don't cut a UTF-8 sequence in half
  std::size_t cut = n;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  return std::string(s.substr(0, cut)) + "…";
}

std::uint64_t to_u64(std::string_view s) {
  std::uint64_t v = 0;
  std::from_chars(s.data(), s.data() + s.size(), v);
  return v;
}

// Resolves a first_difference() path such as "messages[3].content".
const jsonv::Value* resolve(const jsonv::Value& root, std::string_view path) {
  const jsonv::Value* cur = &root;
  std::size_t i = 0;
  while (cur && i < path.size()) {
    if (path[i] == '.') {
      ++i;
    } else if (path[i] == '[') {
      const auto close = path.find(']', i);
      if (close == std::string_view::npos) return nullptr;
      cur = cur->at(static_cast<std::size_t>(to_u64(path.substr(i + 1, close - i - 1))));
      i = close + 1;
    } else {
      auto end = path.find_first_of(".[", i);
      if (end == std::string_view::npos) end = path.size();
      cur = cur->get(path.substr(i, end - i));
      i = end;
    }
  }
  return cur;
}

std::string render(const jsonv::Value* v) {
  if (!v) return "(absent)";
  if (v->kind() == jsonv::Kind::String) return trunc(v->as_string(), 240);
  return trunc(jsonv::canonical(*v), 240);
}

std::string text_of_content(const jsonv::Value* c) {
  if (!c) return {};
  if (c->kind() == jsonv::Kind::String) return c->as_string();
  std::string out;
  if (c->kind() == jsonv::Kind::Array)
    for (const auto& part : c->as_array())
      if (part.str("type") == "text") out += std::string(part.str("text"));
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------------

std::vector<std::byte> encode(const RunInfo& v) {
  Writer w;
  w.str(v.format).str(v.tool_version).str(v.mode).str(v.command).str(v.cwd).str(v.upstream).str(v.parent);
  w.varint(v.started_unix_ms).str(v.workspace);
  return w.bytes();
}

std::vector<std::byte> encode(const Request& v) {
  Writer w;
  w.varint(v.seq).digest(v.key).varint(v.occurrence).varint(v.t_us).str(v.method).str(v.path).str(v.headers_json);
  w.str(v.body);
  return w.bytes();
}

std::vector<std::byte> encode(const Response& v) {
  Writer w;
  w.varint(v.seq).varint(v.status).str(v.content_type).varint(v.latency_us);
  w.u8(static_cast<std::uint8_t>((v.streamed ? 1 : 0) | (v.from_recording ? 2 : 0)));
  w.str(v.body).varint(v.chunks.size());
  for (const auto& c : v.chunks) w.varint(c.end).varint(c.t_us);
  return w.bytes();
}

std::vector<std::byte> encode(const RunEnd& v) {
  Writer w;
  // zig-zag the exit code so negative (signal) codes stay compact
  const auto zz = static_cast<std::uint64_t>((v.exit_code << 1) ^ (v.exit_code >> 63));
  w.varint(zz).varint(v.duration_us).varint(v.exchanges).digest(v.chain);
  return w.bytes();
}

std::optional<RunInfo> decode_run(std::span<const std::byte> p) {
  Reader r(p);
  RunInfo v;
  v.format = std::string(r.str());
  v.tool_version = std::string(r.str());
  v.mode = std::string(r.str());
  v.command = std::string(r.str());
  v.cwd = std::string(r.str());
  v.upstream = std::string(r.str());
  v.parent = std::string(r.str());
  v.started_unix_ms = r.varint();
  if (!r.ok()) return std::nullopt;
  if (!r.done()) v.workspace = std::string(r.str());
  return v;
}

std::optional<Request> decode_request(std::span<const std::byte> p) {
  Reader r(p);
  Request v;
  v.seq = r.varint();
  v.key = r.digest();
  v.occurrence = static_cast<std::uint32_t>(r.varint());
  v.t_us = r.varint();
  v.method = std::string(r.str());
  v.path = std::string(r.str());
  v.headers_json = std::string(r.str());
  v.body = std::string(r.str());
  if (!r.ok()) return std::nullopt;
  return v;
}

std::optional<Response> decode_response(std::span<const std::byte> p) {
  Reader r(p);
  Response v;
  v.seq = r.varint();
  v.status = static_cast<std::uint32_t>(r.varint());
  v.content_type = std::string(r.str());
  v.latency_us = r.varint();
  const auto flags = r.u8();
  v.streamed = flags & 1;
  v.from_recording = flags & 2;
  v.body = std::string(r.str());
  const auto n = r.varint();
  if (!r.ok() || n > v.body.size() + 1) return std::nullopt;
  v.chunks.reserve(static_cast<std::size_t>(n));
  for (std::uint64_t i = 0; i < n && r.ok(); ++i) {
    Chunk c;
    c.end = static_cast<std::uint32_t>(r.varint());
    c.t_us = r.varint();
    v.chunks.push_back(c);
  }
  if (!r.ok()) return std::nullopt;
  return v;
}

std::optional<RunEnd> decode_end(std::span<const std::byte> p) {
  Reader r(p);
  RunEnd v;
  const auto zz = r.varint();
  v.exit_code = static_cast<std::int64_t>(zz >> 1) ^ -static_cast<std::int64_t>(zz & 1);
  v.duration_us = r.varint();
  v.exchanges = r.varint();
  v.chain = r.digest();
  if (!r.ok()) return std::nullopt;
  return v;
}

std::string canonical_body(std::string_view body) {
  if (auto v = jsonv::parse(body)) return jsonv::canonical(*v);
  return std::string(body);
}

Digest128 match_key(std::string_view method, std::string_view path, std::string_view canonical) {
  DigestBuilder h(domain::kRequest);
  h.bytes(as_bytes(method)).bytes(as_bytes(path)).bytes(as_bytes(canonical));
  return h.finish();
}

Digest128 chain_step(const Digest128& prev, std::uint32_t crc) {
  DigestBuilder h(domain::kChain);
  h.digest(prev).u64(crc);
  return h.finish();
}

// ---------------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------------

std::optional<Recording> Recording::load(std::span<const std::byte> bytes, std::string* error) {
  Recording rec;
  const EventLog log = EventLog::parse(bytes, &rec.recovery);
  if (log.size() == 0 || log.frame(0).header.type != FrameType::RunStart) {
    if (error) *error = "not a Khonsu recording (" + (rec.recovery.reason.empty() ? "no RunStart frame" : rec.recovery.reason) + ")";
    return std::nullopt;
  }
  auto run = decode_run(log.frame(0).payload);
  if (!run || run->format != kFormatVersion) {
    if (error) *error = "unsupported recording format";
    return std::nullopt;
  }
  rec.run = std::move(*run);

  Digest128 chain;
  std::unordered_map<std::uint64_t, std::size_t> by_seq;
  for (std::uint64_t l = 0; l < log.size(); ++l) {
    const FrameView f = log.frame(l);
    switch (f.header.type) {
      case FrameType::HttpRequest:
        if (auto q = decode_request(f.payload)) {
          by_seq[q->seq] = rec.exchanges.size();
          rec.exchanges.push_back({std::move(*q), std::nullopt});
        }
        break;
      case FrameType::HttpResponse:
        if (auto s = decode_response(f.payload)) {
          const auto it = by_seq.find(s->seq);
          if (it != by_seq.end()) rec.exchanges[it->second].resp = std::move(*s);
        }
        break;
      case FrameType::Object:
        if (auto o = ws::decode_object(f.payload)) rec.objects.put(o->kind, o->id, std::move(o->bytes));
        break;
      case FrameType::Snapshot:
        if (auto s = ws::decode_snapshot(f.payload)) rec.snapshots.push_back(*s);
        break;
      case FrameType::RunEnd:
        if (auto e = decode_end(f.payload)) {
          rec.chain_ok = e->chain == chain;
          rec.end = *e;
        }
        break;
      default:
        break;
    }
    chain = chain_step(chain, f.header.crc32c);
  }
  std::stable_sort(rec.exchanges.begin(), rec.exchanges.end(),
                   [](const Exchange& a, const Exchange& b) { return a.req.seq < b.req.seq; });
  rec.digest = chain;
  return rec;
}

// ---------------------------------------------------------------------------------
// Summaries (OpenAI-compatible chat completions; tolerant of anything else)
// ---------------------------------------------------------------------------------

ExchangeSummary summarize(const Exchange& ex, std::size_t max_chars) {
  ExchangeSummary s;
  if (ex.req.path == kToolPath) {  // @khonsu.tool call: {"name","args"} -> {"ok","result"|"error"}
    if (auto call = jsonv::parse(ex.req.body)) {
      s.tool_calls = std::string(call->str("name"));
      s.model = "tool " + s.tool_calls;
      if (const auto* a = call->get("args")) s.last_user_message = trunc(jsonv::canonical(*a), max_chars);
    }
    if (ex.resp)
      if (auto out = jsonv::parse(ex.resp->body)) {
        const auto* v = out->get("result");
        if (!v) v = out->get("error");
        if (v) s.output_text = trunc(v->kind() == jsonv::Kind::String ? v->as_string() : jsonv::canonical(*v), max_chars);
      }
    return s;
  }
  if (auto req = jsonv::parse(ex.req.body)) {
    s.model = std::string(req->str("model"));
    if (const auto* msgs = req->get("messages"); msgs && msgs->kind() == jsonv::Kind::Array) {
      for (auto it = msgs->as_array().rbegin(); it != msgs->as_array().rend(); ++it)
        if (it->str("role") == "user") {
          s.last_user_message = trunc(text_of_content(it->get("content")), max_chars);
          break;
        }
    } else if (const auto p = req->str("prompt"); !p.empty()) {
      s.last_user_message = trunc(p, max_chars);
    }
  }
  if (!ex.resp) return s;
  std::string text;
  std::vector<std::string> tools;
  auto take_usage = [&](const jsonv::Value& v) {
    if (const auto* u = v.get("usage"); u && u->kind() == jsonv::Kind::Object) {
      if (const auto* pt = u->get("prompt_tokens")) s.prompt_tokens = to_u64(pt->as_string());
      if (const auto* ct = u->get("completion_tokens")) s.completion_tokens = to_u64(ct->as_string());
    }
  };
  auto take_tools = [&](const jsonv::Value* calls) {
    if (!calls || calls->kind() != jsonv::Kind::Array) return;
    for (const auto& c : calls->as_array())
      if (const auto* fn = c.get("function"))
        if (auto n = fn->str("name"); !n.empty() && std::find(tools.begin(), tools.end(), n) == tools.end())
          tools.emplace_back(n);
  };
  if (ex.resp->streamed) {
    std::string_view body = ex.resp->body;
    std::size_t pos = 0;
    while (pos < body.size()) {
      auto nl = body.find('\n', pos);
      if (nl == std::string_view::npos) nl = body.size();
      std::string_view line = body.substr(pos, nl - pos);
      pos = nl + 1;
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (line.substr(0, 5) != "data:") continue;
      line.remove_prefix(5);
      while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
      if (line == "[DONE]") continue;
      if (auto ev = jsonv::parse(line)) {
        if (const auto* ch = ev->get("choices"); ch && ch->at(0))
          if (const auto* d = ch->at(0)->get("delta")) {
            text += text_of_content(d->get("content"));
            take_tools(d->get("tool_calls"));
          }
        take_usage(*ev);
      }
    }
  } else if (auto resp = jsonv::parse(ex.resp->body)) {
    if (const auto* ch = resp->get("choices"); ch && ch->at(0)) {
      if (const auto* m = ch->at(0)->get("message")) {
        text = text_of_content(m->get("content"));
        take_tools(m->get("tool_calls"));
      } else {
        text = std::string(ch->at(0)->str("text"));
      }
    }
    take_usage(*resp);
  }
  s.output_text = trunc(text, max_chars);
  for (std::size_t i = 0; i < tools.size(); ++i) s.tool_calls += (i ? ", " : "") + tools[i];
  return s;
}

// ---------------------------------------------------------------------------------
// Replay matching
// ---------------------------------------------------------------------------------

ReplayIndex::ReplayIndex(const Recording& r, std::uint64_t live_from) : rec_(r), used_(r.exchanges.size(), false) {
  for (std::size_t i = 0; i < r.exchanges.size(); ++i) {
    if (r.exchanges[i].resp && r.exchanges[i].req.seq < live_from)
      by_key_[r.exchanges[i].req.key].push_back(i);  // already in seq order
    else {
      used_[i] = true;  // never expected: not part of the replayable prefix
      excluded_[r.exchanges[i].req.key] = true;
    }
  }
}

bool ReplayIndex::excluded(const Digest128& key) const {
  std::lock_guard lock(mu_);
  return excluded_.contains(key);
}

const ws::Snapshot* Recording::snapshot_before(std::uint64_t seq) const {
  for (const auto& s : snapshots)
    if (s.when == ws::When::BeforeRequest && s.seq == seq) return &s;
  return nullptr;
}

const ws::Snapshot* Recording::snapshot_at(ws::When when) const {
  for (auto it = snapshots.rbegin(); it != snapshots.rend(); ++it)
    if (it->when == when) return &*it;
  return nullptr;
}

const Exchange* Recording::exchange(std::uint64_t seq) const {
  for (const auto& e : exchanges)
    if (e.req.seq == seq) return &e;
  return nullptr;
}

const Exchange* ReplayIndex::take(const Digest128& key, std::string_view canonical, Miss* miss) {
  std::lock_guard lock(mu_);
  if (const auto it = by_key_.find(key); it != by_key_.end()) {
    std::size_t& n = next_[key];
    if (n < it->second.size()) {
      const std::size_t idx = it->second[n++];
      used_[idx] = true;
      ++served_;
      return &rec_.exchanges[idx];
    }
  }
  if (miss) {
    *miss = {};
    // The recording's expectation: the earliest exchange not yet served.
    const auto first = std::find(used_.begin(), used_.end(), false);
    if (first != used_.end()) {
      const Exchange& exp = rec_.exchanges[static_cast<std::size_t>(first - used_.begin())];
      miss->expected_seq = exp.req.seq;
      const auto a = jsonv::parse(exp.req.body);
      const auto b = jsonv::parse(canonical);
      if (a && b) {
        miss->field_path = jsonv::first_difference(*a, *b);
        miss->expected_value = render(resolve(*a, miss->field_path));
        miss->actual_value = render(resolve(*b, miss->field_path));
      }
    } else {
      miss->field_path = "(recording exhausted: the agent made more requests than were recorded)";
      miss->exhausted = true;
    }
  }
  return nullptr;
}

std::size_t ReplayIndex::served() const {
  std::lock_guard lock(mu_);
  return served_;
}

}  // namespace khonsu::rec

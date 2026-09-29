#include "check.hpp"
#include "khonsu/recording.hpp"

using namespace khonsu;

namespace {

rec::Request make_req(std::uint64_t seq, std::string_view body, std::uint32_t occ = 0) {
  rec::Request r;
  r.seq = seq;
  r.method = "POST";
  r.path = "/v1/chat/completions";
  r.body = rec::canonical_body(body);
  r.key = rec::match_key(r.method, r.path, r.body);
  r.occurrence = occ;
  return r;
}

rec::Response make_resp(std::uint64_t seq, std::string body, bool streamed = false) {
  rec::Response r;
  r.seq = seq;
  r.status = 200;
  r.content_type = streamed ? "text/event-stream" : "application/json";
  r.streamed = streamed;
  r.body = std::move(body);
  if (streamed) r.chunks = {{10, 1000}, {static_cast<std::uint32_t>(r.body.size()), 2000}};
  return r;
}

// Builds a recording the way the proxy writes one (frames + running chain).
std::vector<std::byte> build(const std::vector<std::pair<rec::Request, rec::Response>>& xs, bool end = true) {
  EventLog log;
  Digest128 chain;
  auto add = [&](FrameType t, const std::vector<std::byte>& p) {
    log.append(t, 0, 0, p);
    chain = rec::chain_step(chain, log.frame(log.size() - 1).header.crc32c);
  };
  rec::RunInfo info;
  info.mode = "record";
  info.command = "python agent.py";
  add(FrameType::RunStart, rec::encode(info));
  for (const auto& [q, s] : xs) {
    add(FrameType::HttpRequest, rec::encode(q));
    add(FrameType::HttpResponse, rec::encode(s));
  }
  if (end) {
    rec::RunEnd e;
    e.exit_code = -9;
    e.exchanges = xs.size();
    e.chain = chain;
    add(FrameType::RunEnd, rec::encode(e));
  }
  return log.serialize();
}

constexpr std::string_view kBodyA = R"({"model":"m","messages":[{"role":"user","content":"hello"}],"temperature":0.7})";
constexpr std::string_view kBodyB = R"({"model":"m","messages":[{"role":"user","content":"again"}],"temperature":0.7})";

}  // namespace

TEST("recording: round-trip, hash chain, field fidelity") {
  const auto bytes = build({{make_req(0, kBodyA), make_resp(0, R"({"choices":[{"message":{"content":"hi"}}]})")},
                            {make_req(1, kBodyB), make_resp(1, "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\ndata: [DONE]\n\n", true)}});
  std::string err;
  const auto r = rec::Recording::load(bytes, &err);
  REQUIRE(r.has_value());
  CHECK(r->run.command == "python agent.py");
  REQUIRE(r->exchanges.size() == 2);
  CHECK(r->end.has_value() && r->end->exit_code == -9);
  CHECK(r->chain_ok);
  CHECK(r->exchanges[1].resp->streamed);
  CHECK(r->exchanges[1].resp->chunks.size() == 2);
  CHECK(rec::summarize(r->exchanges[0]).output_text == "hi");
  CHECK(rec::summarize(r->exchanges[1]).output_text == "a");
  CHECK(rec::summarize(r->exchanges[0]).last_user_message == "hello");
}

TEST("recording: whitespace and key order do not change the match key") {
  const auto a = rec::canonical_body(R"({"model":"m","temperature":0.7,"messages":[]})");
  const auto b = rec::canonical_body("{ \"messages\" : [ ], \"temperature\": 0.7, \"model\":\"m\" }");
  CHECK(rec::match_key("POST", "/v1/x", a) == rec::match_key("POST", "/v1/x", b));
  CHECK(!(rec::match_key("POST", "/v1/x", a) == rec::match_key("POST", "/v1/y", a)));
}

TEST("recording: replay serves identical requests by occurrence, in order") {
  const auto bytes = build({{make_req(0, kBodyA, 0), make_resp(0, "first")},
                            {make_req(1, kBodyA, 1), make_resp(1, "second")},
                            {make_req(2, kBodyB, 0), make_resp(2, "third")}});
  const auto r = rec::Recording::load(bytes);
  REQUIRE(r.has_value());
  rec::ReplayIndex idx(*r);
  const auto ka = rec::match_key("POST", "/v1/chat/completions", rec::canonical_body(kBodyA));
  const auto kb = rec::match_key("POST", "/v1/chat/completions", rec::canonical_body(kBodyB));
  rec::ReplayIndex::Miss miss;
  const auto* b = idx.take(kb, "", &miss);  // out of order is fine: matching is by key
  const auto* a1 = idx.take(ka, "", &miss);
  const auto* a2 = idx.take(ka, "", &miss);
  REQUIRE(a1 && a2 && b);
  CHECK(a1->resp->body == "first");
  CHECK(a2->resp->body == "second");
  CHECK(b->resp->body == "third");
  CHECK(idx.served() == 3);
  CHECK(idx.take(ka, rec::canonical_body(kBodyA), &miss) == nullptr);  // a 3rd identical call was never recorded
  CHECK(miss.field_path.find("exhausted") != std::string::npos);
}

TEST("recording: a changed prompt is a divergence pinpointed to the JSON field") {
  const auto bytes = build({{make_req(0, kBodyA), make_resp(0, "ok")}});
  const auto r = rec::Recording::load(bytes);
  REQUIRE(r.has_value());
  rec::ReplayIndex idx(*r);
  const std::string changed =
      rec::canonical_body(R"({"model":"m","messages":[{"role":"user","content":"hello!"}],"temperature":0.7})");
  rec::ReplayIndex::Miss miss;
  CHECK(idx.take(rec::match_key("POST", "/v1/chat/completions", changed), changed, &miss) == nullptr);
  CHECK(miss.expected_seq == 0);
  CHECK(miss.field_path == "messages[0].content");
  CHECK(miss.expected_value == "hello");
  CHECK(miss.actual_value == "hello!");
}

TEST("recording: crash mid-run keeps every complete frame, flags missing RunEnd") {
  auto bytes = build({{make_req(0, kBodyA), make_resp(0, "one")}, {make_req(1, kBodyB), make_resp(1, "two")}}, false);
  bytes.resize(bytes.size() - 7);  // torn final response frame
  const auto r = rec::Recording::load(bytes);
  REQUIRE(r.has_value());
  CHECK(!r->end.has_value());
  REQUIRE(r->exchanges.size() == 2);
  CHECK(r->exchanges[0].resp.has_value());
  CHECK(!r->exchanges[1].resp.has_value());  // request survived, response was torn
  CHECK(r->recovery.reason.find("torn") != std::string::npos);
}

TEST("recording: rewriting history breaks the chain") {
  auto bytes = build({{make_req(0, kBodyA), make_resp(0, "one")}});
  EventLog log = EventLog::parse(bytes);
  // Re-create the log with a different response but the old RunEnd (whose chain no longer matches).
  EventLog forged;
  for (std::uint64_t l = 0; l < log.size(); ++l) {
    const auto f = log.frame(l);
    if (f.header.type == FrameType::HttpResponse) forged.append(f.header.type, 0, 0, rec::encode(make_resp(0, "forged")));
    else forged.append(f.header.type, f.header.actor, f.header.vtime_us, f.payload);
  }
  const auto r = rec::Recording::load(forged.serialize());
  REQUIRE(r.has_value());
  CHECK(r->recovery.reason.empty());  // every CRC is valid...
  CHECK(!r->chain_ok);                // ...but the chain exposes the rewrite
}

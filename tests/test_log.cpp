#include "check.hpp"
#include "khonsu/log.hpp"

using namespace khonsu;

TEST("log: crc32c known-answer vectors") {
  CHECK(crc32c(as_bytes("123456789")) == 0xE3069283u);  // RFC 3720 check value
  CHECK(crc32c(as_bytes("")) == 0u);
  std::string zeros(32, '\0');
  CHECK(crc32c(as_bytes(zeros)) == 0x8A9136AAu);
}

TEST("log: append / read / verify") {
  EventLog log;
  for (int i = 0; i < 100; ++i)
    log.append(FrameType::StepBegin, static_cast<std::uint16_t>(i % 5), i * 10u, "payload-" + std::to_string(i));
  CHECK(log.size() == 100);
  const FrameView f = log.frame(42);
  CHECK(f.header.lsn == 42);
  CHECK(f.header.actor == 2);
  CHECK(f.header.vtime_us == 420);
  CHECK(std::string_view(reinterpret_cast<const char*>(f.payload.data()), f.payload.size()) == "payload-42");
  for (std::uint64_t i = 0; i < log.size(); ++i) CHECK(log.verify(i));
}

TEST("log: serialize/parse round-trip and torn-tail recovery") {
  EventLog log;
  for (int i = 0; i < 50; ++i) log.append(FrameType::OracleResponse, 1, i, std::string(i * 3, 'x'));
  auto bytes = log.serialize();

  RecoveryReport rep;
  const EventLog back = EventLog::parse(bytes, &rep);
  CHECK(back.size() == 50);
  CHECK(rep.reason.empty());

  bytes.resize(bytes.size() - 5);  // torn write in the last frame
  const EventLog torn = EventLog::parse(bytes, &rep);
  CHECK(torn.size() == 49);
  CHECK(rep.reason.find("torn") != std::string::npos);
}

TEST("log: bit-rot is caught by CRC and recovery stops at the bad frame") {
  EventLog log;
  for (int i = 0; i < 20; ++i) log.append(FrameType::StepCommit, 0, i, "commit-" + std::to_string(i));
  log.corrupt_for_test(7, 3);
  CHECK(!log.verify(7));
  CHECK(log.verify(6));
  RecoveryReport rep;
  const EventLog back = EventLog::parse(log.serialize(), &rep);
  CHECK(back.size() == 7);
  CHECK(rep.reason == "crc mismatch at lsn 7");
}

TEST("log: payload varint/str codec") {
  Writer w;
  w.varint(0).varint(127).varint(128).varint(~std::uint64_t{0}).str("hello").digest({1, 2});
  Reader r(w.span());
  CHECK(r.varint() == 0);
  CHECK(r.varint() == 127);
  CHECK(r.varint() == 128);
  CHECK(r.varint() == ~std::uint64_t{0});
  CHECK(r.str() == "hello");
  CHECK((r.digest() == Digest128{1, 2}));
  CHECK(r.ok() && r.done());
  Reader bad(std::span<const std::byte>(w.span().data(), 2));
  bad.varint();
  bad.varint();
  bad.str();
  CHECK(!bad.ok());
}

#include "khonsu/json.hpp"

TEST("json: nesting, commas and escaping") {
  Json j;
  j.begin_object().field("a", 1).key("b").begin_array().value("x\"y\n").value(2.5).begin_object().end_object().end_array();
  j.field("c", true).key("d").null().end_object();
  CHECK(j.str() == R"({"a":1,"b":["x\"y\n",2.5,{}],"c":true,"d":null})");
}

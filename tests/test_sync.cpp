#include "check.hpp"
#include "khonsu/crdt.hpp"
#include "khonsu/log.hpp"
#include "khonsu/pack.hpp"
#include "khonsu/recording.hpp"
#include "khonsu/snapshot.hpp"

#include <map>
#include <unordered_map>

using namespace khonsu;

namespace {

// A recording laid out exactly as FileLog/RecordingWriter write it.
std::vector<std::byte> make_recording(const std::vector<std::string>& files, const std::string& prompt) {
  std::vector<std::byte> out = EventLog::file_header();
  std::uint64_t lsn = 0;
  auto frame = [&](FrameType t, std::uint16_t actor, std::uint64_t vt, const std::vector<std::byte>& p) {
    const auto f = EventLog::encode_frame(lsn++, t, actor, vt, p);
    out.insert(out.end(), f.begin(), f.end());
  };
  rec::RunInfo info;
  info.command = "agent";
  frame(FrameType::RunStart, 0, 0, rec::encode(info));
  std::vector<ws::Entry> entries;
  for (const auto& c : files) {
    std::vector<std::byte> b(reinterpret_cast<const std::byte*>(c.data()), reinterpret_cast<const std::byte*>(c.data()) + c.size());
    const auto id = ws::blob_id(b);
    frame(FrameType::Object, 2, 0, ws::encode_object(ws::ObjectKind::Blob, id, b));
    entries.push_back({"f" + std::to_string(entries.size()), ws::EntryType::File, 0644, c.size(), id});
  }
  const auto tree = ws::encode_tree(entries);
  const auto tid = ws::tree_id(tree);
  frame(FrameType::Object, 2, 0, ws::encode_object(ws::ObjectKind::Tree, tid, tree));
  ws::Snapshot s;
  s.root = tid;
  frame(FrameType::Snapshot, 2, 0, ws::encode(s));
  rec::Request q;
  q.method = "POST";
  q.path = "/v1/chat/completions";
  q.body = rec::canonical_body(R"({"messages":[{"role":"user","content":")" + prompt + R"("}]})");
  q.key = rec::match_key(q.method, q.path, q.body);
  frame(FrameType::HttpRequest, 1, 5, rec::encode(q));
  rec::Response r;
  r.status = 200;
  r.body = "{}";
  frame(FrameType::HttpResponse, 1, 9, rec::encode(r));
  return out;
}

std::unordered_map<Digest128, std::vector<std::byte>, ws::DigestHasher> store_of(const pack::Packed& p) {
  std::unordered_map<Digest128, std::vector<std::byte>, ws::DigestHasher> m;
  for (const auto& o : p.objects) m[o.id] = o.bytes;
  return m;
}

}  // namespace

TEST("pack: unpack rebuilds the recording byte for byte") {
  const auto rec = make_recording({std::string(5000, 'a'), "small", std::string(70000, 'z')}, "hi");
  std::string err;
  const auto p = pack::pack(rec, &err, 1024);
  REQUIRE(p.has_value());
  auto store = store_of(*p);
  const auto back = pack::unpack(p->manifest_id, [&](const Digest128& id) -> std::optional<std::vector<std::byte>> {
    const auto it = store.find(id);
    return it == store.end() ? std::nullopt : std::optional(it->second);
  }, &err);
  REQUIRE(back.has_value());
  CHECK(*back == rec);
  const auto loaded = rec::Recording::load(*back);
  REQUIRE(loaded.has_value());
  CHECK(loaded->objects.size() == 4);  // 3 blobs + 1 tree survive the round trip
}

TEST("pack: workspace objects are shared between recordings") {
  const std::string repo(200000, 'r');
  const auto a = pack::pack(make_recording({repo, "x"}, "first"), nullptr);
  const auto b = pack::pack(make_recording({repo, "x"}, "second"), nullptr);
  REQUIRE(a && b);
  auto sa = store_of(*a);
  std::size_t new_bytes = 0;
  for (const auto& o : b->objects)
    if (!sa.contains(o.id)) new_bytes += o.bytes.size();
  CHECK(new_bytes < 2000);  // the 200 KB repo blob is not re-sent; only the new run's chunk + manifest
}

TEST("pack: forged or corrupt objects are rejected") {
  const auto p = pack::pack(make_recording({"content"}, "q"), nullptr);
  REQUIRE(p.has_value());
  for (const auto& o : p->objects) CHECK(pack::verify(o.id, o.bytes));
  auto bad = p->objects.back().bytes;
  bad.back() ^= std::byte{1};
  CHECK(!pack::verify(p->objects.back().id, bad));
  auto damaged = make_recording({"content"}, "q");
  damaged[damaged.size() / 2] ^= std::byte{1};
  std::string err;
  CHECK(!pack::pack(damaged, &err).has_value());
}

TEST("pack: Merkle summary differs only in the buckets that differ") {
  std::vector<Digest128> ids;
  Xoshiro256 rng(5);
  for (int i = 0; i < 5000; ++i) ids.push_back({rng.next(), rng.next()});
  auto more = ids;
  more.push_back({1, 0xAB00000000000000ULL});
  const auto s1 = pack::summarize(ids);
  const auto s2 = pack::summarize(more);
  // Every id must be accounted for in its bucket (regression: a lo-major sort skipped most).
  bool all_counted = true;
  for (const auto& id : ids) all_counted &= !s1.buckets[pack::bucket_of(id)].empty();
  CHECK(all_counted);
  CHECK(!(pack::summarize({}).root == s1.root));
  CHECK(!(pack::summarize({ids[0]}).root == pack::summarize({ids[0], ids[1]}).root));
  CHECK(!(s1.root == s2.root));
  int differing = 0;
  for (int b = 0; b < 256; ++b) differing += !(s1.buckets[b] == s2.buckets[b]);
  CHECK(differing == 1);
  CHECK(pack::summarize(ids).root == s1.root);  // deterministic, order-independent input
  bool ok = false;
  CHECK(pack::parse_hex(ids[7].hex(), &ok) == ids[7] && ok);
}

TEST("crdt: replicas converge under any merge order (property test)") {
  Xoshiro256 rng(99);
  for (int trial = 0; trial < 60; ++trial) {
    crdt::State r[3];
    crdt::Clock clocks[3] = {crdt::Clock(11), crdt::Clock(22), crdt::Clock(33)};
    std::uint64_t wall = 1000;
    for (int op = 0; op < 40; ++op) {
      const int i = static_cast<int>(rng.below(3));
      wall += rng.below(3);
      switch (rng.below(5)) {
        case 0:
        case 1: {
          crdt::Ref ref{"m" + std::to_string(rng.below(1000)), clocks[i].now(wall), rng.below(4) == 0, 1};
          r[i].put_ref("run" + std::to_string(rng.below(4)), ref);
          break;
        }
        case 2:
        case 3:
          r[i].add_annotation({"run" + std::to_string(rng.below(4)), static_cast<std::int64_t>(rng.below(5)), "bug",
                               "note " + std::to_string(op), "n", clocks[i].now(wall)},
                              clocks[i].node());
          break;
        case 4: {
          const auto anns = r[i].annotations();
          if (!anns.empty()) r[i].remove_annotation(anns[rng.below(anns.size())].first);
          break;
        }
      }
      if (rng.below(4) == 0) {  // occasional partial sync
        const int j = static_cast<int>(rng.below(3));
        r[j].merge(r[i]);
        clocks[j].observe(r[i].max_hlc());
      }
    }
    // Full sync in two different orders must give identical states.
    crdt::State a = r[0], b = r[2];
    a.merge(r[1]);
    a.merge(r[2]);
    b.merge(r[0]);
    b.merge(r[1]);
    CHECK(a.encode() == b.encode());
    crdt::State c = a;
    c.merge(a);  // idempotent
    CHECK(c.encode() == a.encode());
    c.merge(r[1]);  // absorbing an already-merged replica changes nothing
    CHECK(c.encode() == a.encode());
  }
}

TEST("crdt: a concurrent add survives a remove that did not observe it (OR-Set)") {
  crdt::State x, y;
  const auto t1 = x.add_annotation({"run", 3, "bug", "first", "ann", {1, 0, 1}}, 1);
  y.merge(x);
  y.remove_annotation(t1);                                       // y removes what it saw…
  x.add_annotation({"run", 3, "bug", "second", "ann", {2, 0, 1}}, 1);  // …while x adds another
  x.merge(y);
  y.merge(x);
  CHECK(x.encode() == y.encode());
  const auto left = x.annotations("run");
  REQUIRE(left.size() == 1);
  CHECK(left[0].second.text == "second");
}

TEST("crdt: refs are last-writer-wins by hybrid logical clock; deletes are tombstones") {
  crdt::State a, b;
  crdt::Clock ca(1), cb(2);
  a.put_ref("main", {"old", ca.now(100), false, 1});
  b.merge(a);
  cb.observe(a.max_hlc());
  b.put_ref("main", {"", cb.now(50), true, 0});  // b's wall clock is behind, but it saw a's write
  a.merge(b);
  CHECK(a.ref("main") == nullptr);  // the causally later delete wins despite clock skew
  a.put_ref("main", {"new", ca.now(101), false, 1});
  b.merge(a);
  REQUIRE(b.ref("main") != nullptr);
  CHECK(b.ref("main")->manifest == "new");
}

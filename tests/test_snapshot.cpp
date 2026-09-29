#include "check.hpp"
#include "khonsu/snapshot.hpp"

#include <map>

using namespace khonsu;

namespace {

// Builds a snapshot tree from {"dir/file": contents} into `m`, returning the root id.
Digest128 build(ws::ObjectMap& m, const std::map<std::string, std::string>& files) {
  struct Node {
    std::map<std::string, Node> dirs;
    std::map<std::string, std::string> files;
  } root;
  for (const auto& [path, content] : files) {
    Node* n = &root;
    std::size_t start = 0;
    for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', start)) {
      n = &n->dirs[path.substr(start, slash - start)];
      start = slash + 1;
    }
    n->files[path.substr(start)] = content;
  }
  std::function<Digest128(const Node&)> rec = [&](const Node& n) {
    std::vector<ws::Entry> es;
    for (const auto& [name, d] : n.dirs) es.push_back({name, ws::EntryType::Dir, 0755, 0, rec(d)});
    for (const auto& [name, c] : n.files) {
      std::vector<std::byte> b(reinterpret_cast<const std::byte*>(c.data()), reinterpret_cast<const std::byte*>(c.data()) + c.size());
      const auto id = ws::blob_id(b);
      m.put(ws::ObjectKind::Blob, id, b);
      es.push_back({name, ws::EntryType::File, 0644, c.size(), id});
    }
    auto t = ws::encode_tree(es);
    const auto id = ws::tree_id(t);
    m.put(ws::ObjectKind::Tree, id, std::move(t));
    return id;
  };
  return rec(root);
}

}  // namespace

TEST("snapshot: identical content gives identical roots; objects are deduplicated") {
  ws::ObjectMap m;
  const auto a = build(m, {{"src/a.py", "print(1)"}, {"src/b.py", "print(2)"}, {"README", "hi"}});
  const std::size_t objects = m.size();
  const auto b = build(m, {{"README", "hi"}, {"src/b.py", "print(2)"}, {"src/a.py", "print(1)"}});
  CHECK(a == b);
  CHECK(m.size() == objects);  // nothing new stored
}

TEST("snapshot: diff reports adds, deletes and modifications with full paths") {
  ws::ObjectMap m;
  const auto a = build(m, {{"src/a.py", "print(1)"}, {"src/deep/x.txt", "x"}, {"docs/old.md", "old"}, {"keep", "k"}});
  const auto b = build(m, {{"src/a.py", "print(1)  # edited"}, {"src/deep/x.txt", "x"}, {"src/new.py", "n"}, {"keep", "k"}});
  const auto d = ws::diff(m, a, m, b);
  REQUIRE(d.size() == 3);
  CHECK(d[0].kind == 'D' && d[0].path == "docs/old.md");
  CHECK(d[1].kind == 'M' && d[1].path == "src/a.py");
  CHECK(d[2].kind == 'A' && d[2].path == "src/new.py");
  CHECK(ws::diff(m, a, m, a).empty());
}

TEST("snapshot: list_files walks the tree in sorted order") {
  ws::ObjectMap m;
  const auto r = build(m, {{"b/2", "2"}, {"a", "1"}, {"b/1", "1"}});
  const auto files = ws::list_files(m, r);
  REQUIRE(files.size() == 3);
  CHECK(files[0].path == "a");
  CHECK(files[1].path == "b/1");
  CHECK(files[2].path == "b/2");
}

TEST("snapshot: object frames are self-verifying (content addressing)") {
  const std::string c = "hello";
  std::vector<std::byte> b(reinterpret_cast<const std::byte*>(c.data()), reinterpret_cast<const std::byte*>(c.data()) + c.size());
  const auto id = ws::blob_id(b);
  auto payload = ws::encode_object(ws::ObjectKind::Blob, id, b);
  const auto ok = ws::decode_object(payload);
  REQUIRE(ok.has_value());
  CHECK(ok->bytes == b);
  payload.back() ^= std::byte{1};  // corrupt the content
  CHECK(!ws::decode_object(payload).has_value());
}

TEST("snapshot: snapshot record round-trips") {
  ws::Snapshot s{ws::When::BeforeRequest, 7, {1, 2}, 10, 4096, 3, 512, 99};
  const auto d = ws::decode_snapshot(ws::encode(s));
  REQUIRE(d.has_value());
  CHECK(d->seq == 7 && d->files == 10 && d->new_bytes == 512 && d->root == s.root && d->when == ws::When::BeforeRequest);
}

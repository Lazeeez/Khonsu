#include "check.hpp"
#include "khonsu/store.hpp"

#include <map>
#include <string>
#include <vector>

using namespace khonsu;

namespace {
std::map<std::string, std::string> dump(const NodeStore& s, NodeRef root) {
  std::map<std::string, std::string> out;
  s.for_each(root, [&](std::string_view k, NodeRef v) {
    out[std::string(k)] = v.tag() == Tag::File ? "F:" + s.file_read_all(v) : "B:" + std::string(s.blob(v));
  });
  return out;
}

std::string rand_text(Xoshiro256& rng, std::size_t max_len) {
  std::string s(static_cast<std::size_t>(rng.below(max_len + 1)), '\0');
  for (auto& c : s) c = static_cast<char>('a' + rng.below(26));
  return s;
}
}  // namespace

TEST("store: put/get/erase basics") {
  NodeStore s;
  NodeRef r = kEmpty;
  r = s.put_text(r, "alpha", "1");
  r = s.put_text(r, "beta", "2");
  CHECK(s.get_text(r, "alpha") == "1");
  CHECK(s.get_text(r, "beta") == "2");
  CHECK(!s.get_text(r, "gamma"));
  const NodeRef before = r;
  r = s.erase(r, "alpha");
  CHECK(!s.get_text(r, "alpha"));
  CHECK(s.get_text(before, "alpha") == "1");  // old root is an intact snapshot
  CHECK(s.put_text(r, "beta", "2") == r);    // idempotent write shares everything
}

TEST("store: model-based property test with persistent snapshots") {
  NodeStore s;
  Xoshiro256 rng(0xC0FFEE);
  NodeRef root = kEmpty;
  std::map<std::string, std::string> model;
  std::vector<std::pair<NodeRef, std::map<std::string, std::string>>> snaps;

  for (int op = 0; op < 4000; ++op) {
    const std::string key = "k" + std::to_string(rng.below(300));
    switch (rng.below(4)) {
      case 0:
      case 1: {
        const std::string v = rand_text(rng, 12);
        root = s.put_text(root, key, v);
        model[key] = "B:" + v;
        break;
      }
      case 2:
        root = s.erase(root, key);
        model.erase(key);
        break;
      case 3: {
        const std::string v = rand_text(rng, 9000);  // spans up to 3 pages
        root = s.put_file(root, key, v);
        model[key] = "F:" + v;
        break;
      }
    }
    if (op % 97 == 0) snaps.emplace_back(root, model);
    if (op % 500 == 0) CHECK(dump(s, root) == model);
  }
  CHECK(dump(s, root) == model);
  CHECK(s.size(root) == model.size());
  for (const auto& [r, m] : snaps) CHECK(dump(s, r) == m);  // every historical root still valid
}

TEST("store: canonical structure => history-independent digests") {
  NodeStore s;
  NodeRef a = kEmpty, b = kEmpty;
  for (int i = 0; i < 500; ++i) a = s.put_text(a, "key" + std::to_string(i), "v" + std::to_string(i));
  for (int i = 999; i >= 0; --i) b = s.put_text(b, "key" + std::to_string(i), "v" + std::to_string(i));
  for (int i = 500; i < 1000; ++i) b = s.erase(b, "key" + std::to_string(i));
  CHECK(a != b);                       // different nodes...
  CHECK(s.digest(a) == s.digest(b));   // ...same content, same Merkle root
  const NodeRef c = s.put_text(a, "key7", "changed");
  CHECK(s.digest(a) != s.digest(c));
}

TEST("store: diff reports exactly the changed keys") {
  NodeStore s;
  NodeRef base = kEmpty;
  for (int i = 0; i < 200; ++i) base = s.put_text(base, "f" + std::to_string(i), "x");
  NodeRef other = s.put_text(base, "f3", "y");
  other = s.erase(other, "f10");
  other = s.put_text(other, "new", "z");
  const auto d = s.diff(base, other);
  REQUIRE(d.size() == 3);
  CHECK(d[0].key == "f10" && d[0].kind == ChangeKind::Removed);
  CHECK(d[1].key == "f3" && d[1].kind == ChangeKind::Modified);
  CHECK(d[2].key == "new" && d[2].kind == ChangeKind::Added);
  CHECK(s.diff(base, base).empty());
}

TEST("store: file writes spanning pages are not truncated (v1 bug regression)") {
  NodeStore s;
  std::string data(3 * kPageSize + 123, 'q');
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>('A' + i % 23);
  NodeRef r = s.write_file(kEmpty, "/f", 100, data);
  const auto got = s.read_file(r, "/f");
  REQUIRE(got.has_value());
  CHECK(got->size() == 100 + data.size());
  CHECK(got->substr(100) == data);
  CHECK(got->substr(0, 100) == std::string(100, '\0'));
}

TEST("store: copy-on-write shares untouched pages across a fork") {
  NodeStore s;
  std::string content(16 * kPageSize, 'a');
  const NodeRef parent = s.put_file(kEmpty, "/big", content);
  const NodeRef child = s.write_file(parent, "/big", 5 * kPageSize + 7, "Z");  // "fork" = reuse root

  const auto pp = s.file_pages(s.get(parent, "/big"));
  const auto cp = s.file_pages(s.get(child, "/big"));
  REQUIRE(pp.size() == 16 && cp.size() == 16);
  int shared = 0;
  for (std::size_t i = 0; i < 16; ++i) shared += pp[i].page == cp[i].page;
  CHECK(shared == 15);  // exactly one page was copied
  CHECK(s.read_file(parent, "/big") == content);
  CHECK(s.read_file(child, "/big")->at(5 * kPageSize + 7) == 'Z');

  // Rewriting a file with mostly-identical content keeps identical pages shared.
  std::string edited = content;
  edited[kPageSize * 9 + 1] = 'b';
  const NodeRef r2 = s.put_file(parent, "/big", edited);
  const auto p2 = s.file_pages(s.get(r2, "/big"));
  shared = 0;
  for (std::size_t i = 0; i < 16; ++i) shared += pp[i].page == p2[i].page;
  CHECK(shared == 15);
}

TEST("store: sparse and deep integer keys (deep HAMT joins)") {
  NodeStore s;
  NodeRef f = s.empty_file();
  const std::uint64_t far = (std::uint64_t{1} << 40) * kPageSize;  // page index 2^40
  f = s.file_write(f, 0, as_bytes("head"));
  f = s.file_write(f, far, as_bytes("tail"));
  const auto pages = s.file_pages(f);
  REQUIRE(pages.size() == 2);
  CHECK(pages[1].index == (std::uint64_t{1} << 40));
  f = s.file_truncate(f, 4);
  CHECK(s.file_pages(f).size() == 1);
  CHECK(s.file_read_all(f) == "head");
}

TEST("store: garbage collection frees unreachable nodes and keeps roots valid") {
  NodeStore s;
  NodeRef keep = kEmpty;
  for (int i = 0; i < 100; ++i) keep = s.put_file(keep, "/k" + std::to_string(i), std::string(5000, 'k'));
  NodeRef junk = keep;
  for (int i = 0; i < 100; ++i) junk = s.put_file(junk, "/k" + std::to_string(i), std::string(5000, 'j'));
  const auto before = s.stats();
  const NodeRef roots[] = {keep};
  const std::size_t freed = s.collect(roots);
  const auto after = s.stats();
  CHECK(freed > 0);
  CHECK(after.pages < before.pages);
  for (int i = 0; i < 100; ++i) CHECK(s.read_file(keep, "/k" + std::to_string(i)) == std::string(5000, 'k'));
  // Freed slots are reused.
  NodeRef again = keep;
  for (int i = 0; i < 100; ++i) again = s.put_file(again, "/k" + std::to_string(i), std::string(5000, 'x'));
  CHECK(s.stats().pages <= before.pages);
  CHECK(s.read_file(keep, "/k5") == std::string(5000, 'k'));
}

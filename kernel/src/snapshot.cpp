#include "khonsu/snapshot.hpp"

#include "khonsu/log.hpp"

#include <algorithm>

namespace khonsu::ws {

Digest128 blob_id(std::span<const std::byte> content) { return digest_bytes(content, domain::kBlob); }

Digest128 tree_id(std::span<const std::byte> encoded) { return digest_bytes(encoded, domain::kBranch); }

std::vector<std::byte> encode_tree(std::vector<Entry> entries) {
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
  Writer w;
  w.varint(entries.size());
  for (const auto& e : entries)
    w.str(e.name).u8(static_cast<std::uint8_t>(e.type)).varint(e.mode).varint(e.size).digest(e.id);
  return w.bytes();
}

std::optional<std::vector<Entry>> decode_tree(std::span<const std::byte> bytes) {
  Reader r(bytes);
  const auto n = r.varint();
  if (!r.ok() || n > bytes.size()) return std::nullopt;
  std::vector<Entry> out;
  out.reserve(static_cast<std::size_t>(n));
  for (std::uint64_t i = 0; i < n; ++i) {
    Entry e;
    e.name = std::string(r.str());
    e.type = static_cast<EntryType>(r.u8());
    e.mode = static_cast<std::uint32_t>(r.varint());
    e.size = r.varint();
    e.id = r.digest();
    if (!r.ok()) return std::nullopt;
    out.push_back(std::move(e));
  }
  return out;
}

bool ObjectMap::put(ObjectKind kind, const Digest128& id, std::vector<std::byte> bytes) {
  if (objects_.contains(id)) return false;
  bytes_ += bytes.size();
  objects_.emplace(id, Obj{kind, std::move(bytes)});
  return true;
}

const std::vector<std::byte>* ObjectMap::get(const Digest128& id) const {
  const auto it = objects_.find(id);
  return it == objects_.end() ? nullptr : &it->second.bytes;
}

std::vector<std::byte> encode_object(ObjectKind kind, const Digest128& id, std::span<const std::byte> bytes) {
  Writer w;
  w.u8(static_cast<std::uint8_t>(kind)).digest(id).varint(bytes.size());
  std::vector<std::byte> out = w.bytes();
  out.insert(out.end(), bytes.begin(), bytes.end());
  return out;
}

std::optional<DecodedObject> decode_object(std::span<const std::byte> payload) {
  Reader r(payload);
  DecodedObject o;
  o.kind = static_cast<ObjectKind>(r.u8());
  o.id = r.digest();
  const auto n = r.varint();
  if (!r.ok() || n > payload.size()) return std::nullopt;
  const std::size_t header = payload.size() - static_cast<std::size_t>(n);
  if (header > payload.size()) return std::nullopt;
  o.bytes.assign(payload.begin() + static_cast<std::ptrdiff_t>(header), payload.end());
  // Content addressing is self-verifying: a corrupt object can't masquerade as another.
  const Digest128 actual = o.kind == ObjectKind::Blob ? blob_id(o.bytes) : tree_id(o.bytes);
  if (!(actual == o.id)) return std::nullopt;
  return o;
}

std::vector<std::byte> encode(const Snapshot& s) {
  Writer w;
  w.u8(static_cast<std::uint8_t>(s.when)).varint(s.seq).digest(s.root).varint(s.files).varint(s.bytes);
  w.varint(s.new_objects).varint(s.new_bytes).varint(s.took_us);
  return w.bytes();
}

std::optional<Snapshot> decode_snapshot(std::span<const std::byte> p) {
  Reader r(p);
  Snapshot s;
  s.when = static_cast<When>(r.u8());
  s.seq = r.varint();
  s.root = r.digest();
  s.files = r.varint();
  s.bytes = r.varint();
  s.new_objects = r.varint();
  s.new_bytes = r.varint();
  s.took_us = r.varint();
  if (!r.ok()) return std::nullopt;
  return s;
}

namespace {

std::vector<Entry> tree_at(const ObjectMap& m, const Digest128& id) {
  if (id.empty()) return {};
  const auto* bytes = m.get(id);
  if (!bytes) return {};
  return decode_tree(*bytes).value_or(std::vector<Entry>{});
}

void collect(const ObjectMap& m, const Entry& e, const std::string& path, char kind, bool before,
             std::vector<FileChange>& out) {
  if (e.type == EntryType::Dir) {
    for (const auto& c : tree_at(m, e.id)) collect(m, c, path + "/" + c.name, kind, before, out);
    return;
  }
  FileChange fc;
  fc.kind = kind;
  fc.path = path;
  fc.type = e.type;
  (before ? fc.before : fc.after) = e.id;
  out.push_back(std::move(fc));
}

void diff_rec(const ObjectMap& am, const Digest128& a, const ObjectMap& bm, const Digest128& b,
              const std::string& prefix, std::vector<FileChange>& out) {
  if (a == b) return;  // identical subtree: skip without reading it
  const auto x = tree_at(am, a);
  const auto y = tree_at(bm, b);
  std::size_t i = 0, j = 0;
  auto join = [&](const std::string& n) { return prefix.empty() ? n : prefix + "/" + n; };
  while (i < x.size() || j < y.size()) {
    if (j == y.size() || (i < x.size() && x[i].name < y[j].name)) {
      collect(am, x[i], join(x[i].name), 'D', true, out);
      ++i;
    } else if (i == x.size() || y[j].name < x[i].name) {
      collect(bm, y[j], join(y[j].name), 'A', false, out);
      ++j;
    } else {
      const Entry& ea = x[i];
      const Entry& eb = y[j];
      if (ea.type == EntryType::Dir && eb.type == EntryType::Dir) {
        diff_rec(am, ea.id, bm, eb.id, join(ea.name), out);
      } else if (ea.type == EntryType::Dir || eb.type == EntryType::Dir) {
        collect(am, ea, join(ea.name), 'D', true, out);
        collect(bm, eb, join(eb.name), 'A', false, out);
      } else if (!(ea.id == eb.id) || ea.mode != eb.mode || ea.type != eb.type) {
        out.push_back({'M', join(ea.name), ea.id, eb.id, eb.type});
      }
      ++i;
      ++j;
    }
  }
}

void list_rec(const ObjectMap& m, const Digest128& id, const std::string& prefix, std::vector<FileEntry>& out) {
  for (const auto& e : tree_at(m, id)) {
    const std::string p = prefix.empty() ? e.name : prefix + "/" + e.name;
    if (e.type == EntryType::Dir) list_rec(m, e.id, p, out);
    else out.push_back({p, e});
  }
}

}  // namespace

std::vector<FileChange> diff(const ObjectMap& a_objects, const Digest128& a_root, const ObjectMap& b_objects,
                             const Digest128& b_root) {
  std::vector<FileChange> out;
  diff_rec(a_objects, a_root, b_objects, b_root, "", out);
  return out;
}

std::vector<FileEntry> list_files(const ObjectMap& objects, const Digest128& root) {
  std::vector<FileEntry> out;
  list_rec(objects, root, "", out);
  return out;
}

}  // namespace khonsu::ws

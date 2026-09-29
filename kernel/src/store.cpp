#include "khonsu/store.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace khonsu {

namespace {
constexpr unsigned kBits = 5;
constexpr std::uint32_t kFanoutMask = (1u << kBits) - 1;

constexpr std::uint32_t slot_of(std::uint64_t key, unsigned shift) noexcept {
  return static_cast<std::uint32_t>((key >> shift) & kFanoutMask);
}
constexpr std::uint32_t rank_of(std::uint32_t bitmap, std::uint32_t bit) noexcept {
  return static_cast<std::uint32_t>(std::popcount(bitmap & (bit - 1)));
}

// Fixed-capacity scratch for child arrays: nodes are immutable, so every edit
// builds a new child list; this keeps that off the heap.
struct Kids {
  std::array<NodeRef, 65> v{};
  std::uint32_t n = 0;
  void assign(std::span<const NodeRef> s) {
    n = static_cast<std::uint32_t>(s.size());
    std::copy(s.begin(), s.end(), v.begin());
  }
  void insert_at(std::uint32_t pos, NodeRef r) {
    std::copy_backward(v.begin() + pos, v.begin() + n, v.begin() + n + 1);
    v[pos] = r;
    ++n;
  }
  void erase_at(std::uint32_t pos) {
    std::copy(v.begin() + pos + 1, v.begin() + n, v.begin() + pos);
    --n;
  }
  [[nodiscard]] std::span<const NodeRef> span() const { return {v.data(), n}; }
};
}  // namespace

// ---------------------------------------------------------------------------------
// Pools
// ---------------------------------------------------------------------------------

template <class T, class C>
template <class... Args>
std::uint32_t NodeStore::Pool<T, C>::alloc(Args&&... args) {
  std::uint32_t i;
  if (!free_list.empty()) {
    i = free_list.back();
    free_list.pop_back();
    if constexpr (sizeof...(Args) > 0) items[i] = T{std::forward<Args>(args)...};
  } else {
    i = static_cast<std::uint32_t>(items.size());
    if (i > NodeRef::kIndexMask) throw std::length_error("khonsu: node pool exhausted");
    items.emplace_back(T{std::forward<Args>(args)...});
    alive.push_back(0);
    mark.push_back(0);
  }
  alive[i] = 1;
  ++live;
  return i;
}

template <class T, class C>
void NodeStore::Pool<T, C>::release(std::uint32_t i) {
  alive[i] = 0;
  free_list.push_back(i);
  --live;
}

NodeStore::NodeStore() = default;
NodeStore::~NodeStore() = default;

std::span<const NodeRef> NodeStore::children(const Branch& b) const {
  const Slab& s = slabs_[b.count];
  return {s.data.data() + static_cast<std::size_t>(b.slot) * b.count, b.count};
}

NodeRef NodeStore::new_branch(std::uint32_t bitmap, std::span<const NodeRef> kids, bool collision) {
  if (kids.empty()) return kEmpty;
  if (kids.size() > kMaxChildren) throw std::length_error("khonsu: HAMT collision bucket overflow");
  const auto count = static_cast<std::uint32_t>(kids.size());
  Slab& s = slabs_[count];
  std::uint32_t slot;
  if (!s.free_slots.empty()) {
    slot = s.free_slots.back();
    s.free_slots.pop_back();
  } else {
    slot = static_cast<std::uint32_t>(s.data.size() / count);
    s.data.resize(s.data.size() + count);
  }
  std::copy(kids.begin(), kids.end(), s.data.begin() + static_cast<std::ptrdiff_t>(slot) * count);
  const auto i = branches_.alloc(Branch{bitmap, slot, static_cast<std::uint8_t>(count),
                                        static_cast<std::uint8_t>(collision ? 1 : 0), {}});
  return NodeRef::make(Tag::Branch, i);
}

NodeRef NodeStore::new_entry(std::uint64_t key, NodeRef name, NodeRef value) {
  return NodeRef::make(Tag::Entry, entries_.alloc(Entry{key, name, value, {}}));
}

NodeRef NodeStore::make_blob(std::string_view bytes) {
  return NodeRef::make(Tag::Blob, blobs_.alloc(Blob{std::string(bytes), {}}));
}

std::string_view NodeStore::blob(NodeRef b) const {
  if (b.tag() != Tag::Blob) return {};
  return blobs_.items[b.index()].bytes;
}

NodeRef NodeStore::new_page(const std::byte* src) {
  const auto i = pages_.alloc();
  auto& dst = pages_.items[i].bytes;
  if (src) {
    std::memcpy(dst.data(), src, kPageSize);
    page_bytes_copied_ += kPageSize;
  } else {
    std::memset(dst.data(), 0, kPageSize);
  }
  if (page_digest_.size() <= i) page_digest_.resize(static_cast<std::size_t>(i) + 1);
  page_digest_[i] = {};
  ++pages_allocated_;
  return NodeRef::make(Tag::Page, i);
}

NodeRef NodeStore::new_file(std::uint64_t size, NodeRef pages) {
  return NodeRef::make(Tag::File, files_.alloc(File{size, pages, {}}));
}

// ---------------------------------------------------------------------------------
// Generic HAMT
// ---------------------------------------------------------------------------------

std::string_view NodeStore::entry_name(const Entry& e) const { return blob(e.name); }

bool NodeStore::same_key(const Entry& e, std::uint64_t key, std::string_view name) const {
  return e.key == key && entry_name(e) == name;
}

NodeRef NodeStore::lookup(NodeRef node, std::uint64_t key, std::string_view name) const {
  unsigned shift = 0;
  for (;;) {
    switch (node.tag()) {
      case Tag::Empty:
        return kEmpty;
      case Tag::Entry:
        return same_key(entry(node), key, name) ? node : kEmpty;
      case Tag::Branch: {
        const Branch& b = branch(node);
        if (b.collision) {
          for (NodeRef c : children(b))
            if (same_key(entry(c), key, name)) return c;
          return kEmpty;
        }
        const std::uint32_t bit = 1u << slot_of(key, shift);
        if (!(b.bitmap & bit)) return kEmpty;
        node = children(b)[rank_of(b.bitmap, bit)];
        shift += kBits;
        break;
      }
      default:
        return kEmpty;
    }
  }
}

NodeRef NodeStore::make_collision(std::vector<NodeRef> es) {
  // Canonical order so equal key sets produce equal digests.
  std::sort(es.begin(), es.end(),
            [&](NodeRef a, NodeRef b) { return entry_name(entry(a)) < entry_name(entry(b)); });
  return new_branch(0, es, /*collision=*/true);
}

NodeRef NodeStore::join(NodeRef e1, NodeRef e2, unsigned shift) {
  if (shift >= 64) return make_collision({e1, e2});
  const std::uint32_t i1 = slot_of(entry(e1).key, shift);
  const std::uint32_t i2 = slot_of(entry(e2).key, shift);
  if (i1 == i2) {
    const NodeRef sub = join(e1, e2, shift + kBits);
    return new_branch(1u << i1, std::span<const NodeRef>(&sub, 1));
  }
  const NodeRef pair[2] = {i1 < i2 ? e1 : e2, i1 < i2 ? e2 : e1};
  return new_branch((1u << i1) | (1u << i2), pair);
}

NodeRef NodeStore::insert_rec(NodeRef node, std::uint64_t key, std::string_view name, NodeRef nameref,
                              NodeRef value, unsigned shift) {
  const Branch b = branch(node);  // copy: allocations below may move pool storage
  Kids kids;
  kids.assign(children(b));

  if (b.collision) {
    std::vector<NodeRef> es(kids.span().begin(), kids.span().end());
    for (auto& c : es) {
      if (same_key(entry(c), key, name)) {
        if (entry(c).value == value) return node;
        c = new_entry(key, entry(c).name, value);
        return make_collision(std::move(es));
      }
    }
    es.push_back(new_entry(key, nameref.empty() && !name.empty() ? make_blob(name) : nameref, value));
    return make_collision(std::move(es));
  }

  const std::uint32_t bit = 1u << slot_of(key, shift);
  const std::uint32_t pos = rank_of(b.bitmap, bit);
  auto make_name = [&] { return nameref.empty() && !name.empty() ? make_blob(name) : nameref; };

  if (!(b.bitmap & bit)) {
    kids.insert_at(pos, new_entry(key, make_name(), value));
    return new_branch(b.bitmap | bit, kids.span());
  }

  const NodeRef child = kids.v[pos];
  if (child.tag() == Tag::Entry) {
    const Entry e = entry(child);
    if (same_key(e, key, name)) {
      if (e.value == value) return node;
      kids.v[pos] = new_entry(key, e.name, value);  // reuse the name blob
    } else {
      kids.v[pos] = join(child, new_entry(key, make_name(), value), shift + kBits);
    }
  } else {
    const NodeRef updated = insert_rec(child, key, name, nameref, value, shift + kBits);
    if (updated == child) return node;
    kids.v[pos] = updated;
  }
  return new_branch(b.bitmap, kids.span());
}

NodeRef NodeStore::insert(NodeRef root, std::uint64_t key, std::string_view name, NodeRef value) {
  if (root.empty()) {
    const NodeRef e = new_entry(key, name.empty() ? kEmpty : make_blob(name), value);
    return new_branch(1u << slot_of(key, 0), std::span<const NodeRef>(&e, 1));
  }
  return insert_rec(root, key, name, kEmpty, value, 0);
}

NodeRef NodeStore::remove_rec(NodeRef node, std::uint64_t key, std::string_view name, unsigned shift,
                              bool is_root) {
  const Branch b = branch(node);
  Kids kids;
  kids.assign(children(b));

  if (b.collision) {
    std::vector<NodeRef> es(kids.span().begin(), kids.span().end());
    const auto it = std::find_if(es.begin(), es.end(), [&](NodeRef c) { return same_key(entry(c), key, name); });
    if (it == es.end()) return node;
    es.erase(it);
    if (es.size() == 1) return es.front();
    return make_collision(std::move(es));
  }

  const std::uint32_t bit = 1u << slot_of(key, shift);
  if (!(b.bitmap & bit)) return node;
  const std::uint32_t pos = rank_of(b.bitmap, bit);
  std::uint32_t bitmap = b.bitmap;
  const NodeRef child = kids.v[pos];

  if (child.tag() == Tag::Entry) {
    if (!same_key(entry(child), key, name)) return node;
    kids.erase_at(pos);
    bitmap &= ~bit;
  } else {
    const NodeRef r = remove_rec(child, key, name, shift + kBits, false);
    if (r == child) return node;
    if (r.empty()) {
      kids.erase_at(pos);
      bitmap &= ~bit;
    } else {
      kids.v[pos] = r;
    }
  }

  if (kids.n == 0) return kEmpty;
  // Canonical form: a non-root branch holding a single entry collapses into it.
  if (!is_root && kids.n == 1 && kids.v[0].tag() == Tag::Entry) return kids.v[0];
  return new_branch(bitmap, kids.span());
}

NodeRef NodeStore::remove(NodeRef root, std::uint64_t key, std::string_view name) {
  if (root.empty()) return root;
  return remove_rec(root, key, name, 0, true);
}

void NodeStore::walk_entries(NodeRef node, const std::function<void(const Entry&)>& fn) const {
  switch (node.tag()) {
    case Tag::Entry:
      fn(entry(node));
      break;
    case Tag::Branch:
      for (NodeRef c : children(branch(node))) walk_entries(c, fn);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------------
// World map
// ---------------------------------------------------------------------------------

NodeRef NodeStore::get(NodeRef root, std::string_view key) const {
  const NodeRef e = lookup(root, hash64(key), key);
  return e.empty() ? kEmpty : entry(e).value;
}

NodeRef NodeStore::put(NodeRef root, std::string_view key, NodeRef value) {
  if (key.empty()) throw std::invalid_argument("khonsu: empty key");
  if (value.empty()) return erase(root, key);
  return insert(root, hash64(key), key, value);
}

NodeRef NodeStore::erase(NodeRef root, std::string_view key) { return remove(root, hash64(key), key); }

void NodeStore::for_each(NodeRef root, const std::function<void(std::string_view, NodeRef)>& fn) const {
  walk_entries(root, [&](const Entry& e) { fn(entry_name(e), e.value); });
}

std::size_t NodeStore::size(NodeRef root) const {
  std::size_t n = 0;
  walk_entries(root, [&](const Entry&) { ++n; });
  return n;
}

NodeRef NodeStore::put_text(NodeRef root, std::string_view key, std::string_view text) {
  const NodeRef cur = get(root, key);
  if (cur.tag() == Tag::Blob && blob(cur) == text) return root;  // no-op writes share everything
  return put(root, key, make_blob(text));
}

std::optional<std::string_view> NodeStore::get_text(NodeRef root, std::string_view key) const {
  const NodeRef v = get(root, key);
  if (v.tag() != Tag::Blob) return std::nullopt;
  return blob(v);
}

// ---------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------

NodeRef NodeStore::empty_file() { return new_file(0, kEmpty); }

std::uint64_t NodeStore::file_size(NodeRef f) const { return f.tag() == Tag::File ? file(f).size : 0; }

NodeRef NodeStore::file_write(NodeRef fref, std::uint64_t offset, std::span<const std::byte> src) {
  if (fref.tag() != Tag::File) fref = empty_file();
  const File f = file(fref);
  NodeRef pages = f.pages;
  std::size_t pos = 0;
  while (pos < src.size()) {
    const std::uint64_t abs = offset + pos;
    const std::uint64_t pi = abs / kPageSize;
    const std::size_t po = static_cast<std::size_t>(abs % kPageSize);
    const std::size_t n = std::min(src.size() - pos, kPageSize - po);

    const NodeRef existing = lookup(pages, pi, {});
    const NodeRef old_page = existing.empty() ? kEmpty : entry(existing).value;
    if (!old_page.empty() && std::memcmp(page(old_page).bytes.data() + po, src.data() + pos, n) == 0) {
      pos += n;  // identical bytes: keep sharing the page
      continue;
    }
    NodeRef np;
    if (n == kPageSize) {
      np = new_page(src.data() + pos);
    } else {
      np = new_page(old_page.empty() ? nullptr : page(old_page).bytes.data());  // copy-on-write
      std::memcpy(pages_.items[np.index()].bytes.data() + po, src.data() + pos, n);
    }
    pages = insert(pages, pi, {}, np);
    pos += n;
  }
  const std::uint64_t size = std::max<std::uint64_t>(f.size, offset + src.size());
  if (pages == f.pages && size == f.size) return fref;
  return new_file(size, pages);
}

NodeRef NodeStore::file_truncate(NodeRef fref, std::uint64_t size) {
  if (fref.tag() != Tag::File) fref = empty_file();
  const File f = file(fref);
  if (size == f.size) return fref;
  NodeRef pages = f.pages;
  const std::uint64_t keep_pages = (size + kPageSize - 1) / kPageSize;
  for (const PageSlot& s : file_pages(fref))
    if (s.index >= keep_pages) pages = remove(pages, s.index, {});
  if (size < f.size && size % kPageSize != 0) {
    const std::uint64_t last = size / kPageSize;
    const NodeRef e = lookup(pages, last, {});
    if (!e.empty()) {
      const NodeRef np = new_page(page(entry(e).value).bytes.data());
      const std::size_t tail = static_cast<std::size_t>(size % kPageSize);
      std::memset(pages_.items[np.index()].bytes.data() + tail, 0, kPageSize - tail);
      pages = insert(pages, last, {}, np);
    }
  }
  return new_file(size, pages);
}

std::vector<PageSlot> NodeStore::file_pages(NodeRef f) const {
  std::vector<PageSlot> out;
  if (f.tag() != Tag::File) return out;
  walk_entries(file(f).pages, [&](const Entry& e) { out.push_back({e.key, e.value}); });
  std::sort(out.begin(), out.end(), [](const PageSlot& a, const PageSlot& b) { return a.index < b.index; });
  return out;
}

NodeRef NodeStore::read_page(NodeRef f, std::uint64_t index, std::byte* out) const {
  const NodeRef e = f.tag() == Tag::File ? lookup(file(f).pages, index, {}) : kEmpty;
  if (e.empty()) {
    std::memset(out, 0, kPageSize);
    return kEmpty;
  }
  const NodeRef p = entry(e).value;
  std::memcpy(out, page(p).bytes.data(), kPageSize);
  return p;
}

NodeRef NodeStore::page_at(NodeRef f, std::uint64_t index) const {
  const NodeRef e = f.tag() == Tag::File ? lookup(file(f).pages, index, {}) : kEmpty;
  return e.empty() ? kEmpty : entry(e).value;
}

std::string NodeStore::file_read_all(NodeRef f) const {
  if (f.tag() != Tag::File) return {};
  const File& fl = file(f);
  std::string out(static_cast<std::size_t>(fl.size), '\0');
  for (const PageSlot& s : file_pages(f)) {
    const std::uint64_t start = s.index * kPageSize;
    if (start >= fl.size) continue;
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kPageSize, fl.size - start));
    std::memcpy(out.data() + start, page(s.page).bytes.data(), n);
  }
  return out;
}

NodeRef NodeStore::write_file(NodeRef root, std::string_view path, std::uint64_t offset, std::string_view data) {
  const NodeRef f = file_write(get(root, path), offset, as_bytes(data));
  return put(root, path, f);
}

NodeRef NodeStore::put_file(NodeRef root, std::string_view path, std::string_view contents) {
  NodeRef f = get(root, path);
  if (f.tag() != Tag::File) f = empty_file();
  f = file_write(f, 0, as_bytes(contents));  // unchanged pages are kept shared
  f = file_truncate(f, contents.size());
  return put(root, path, f);
}

std::optional<std::string> NodeStore::read_file(NodeRef root, std::string_view path) const {
  const NodeRef f = get(root, path);
  if (f.tag() != Tag::File) return std::nullopt;
  return file_read_all(f);
}

// ---------------------------------------------------------------------------------
// Merkle digests and diff
// ---------------------------------------------------------------------------------

Digest128 NodeStore::digest(NodeRef n) const {
  switch (n.tag()) {
    case Tag::Empty:
      return {};
    case Tag::Blob: {
      const Blob& b = blobs_.items[n.index()];
      if (b.digest.empty()) b.digest = digest_bytes(b.bytes, domain::kBlob);
      return b.digest;
    }
    case Tag::Page: {
      Digest128& d = page_digest_[n.index()];
      if (d.empty()) d = digest_bytes(page(n).bytes, domain::kPage);
      return d;
    }
    case Tag::Entry: {
      const Entry& e = entry(n);
      if (e.digest.empty()) {
        DigestBuilder h(domain::kEntry);
        h.u64(e.key).digest(digest(e.name)).digest(digest(e.value));
        e.digest = h.finish();
      }
      return e.digest;
    }
    case Tag::Branch: {
      const Branch& b = branch(n);
      if (b.digest.empty()) {
        DigestBuilder h(domain::kBranch);
        h.u64(b.bitmap).u64(b.collision);
        for (NodeRef c : children(b)) h.digest(digest(c));
        b.digest = h.finish();
      }
      return b.digest;
    }
    case Tag::File: {
      const File& f = file(n);
      if (f.digest.empty()) {
        DigestBuilder h(domain::kFile);
        h.u64(f.size).digest(digest(f.pages));
        f.digest = h.finish();
      }
      return f.digest;
    }
  }
  return {};
}

void NodeStore::diff_fallback(NodeRef a, NodeRef b, std::vector<Change>& out) const {
  std::vector<const Entry*> ea, eb;
  walk_entries(a, [&](const Entry& e) { ea.push_back(&e); });
  walk_entries(b, [&](const Entry& e) { eb.push_back(&e); });
  auto key_less = [&](const Entry* x, const Entry* y) {
    return x->key != y->key ? x->key < y->key : entry_name(*x) < entry_name(*y);
  };
  std::sort(ea.begin(), ea.end(), key_less);
  std::sort(eb.begin(), eb.end(), key_less);
  std::size_t i = 0, j = 0;
  while (i < ea.size() || j < eb.size()) {
    if (j == eb.size() || (i < ea.size() && key_less(ea[i], eb[j]))) {
      out.push_back({ChangeKind::Removed, std::string(entry_name(*ea[i])), ea[i]->value, kEmpty});
      ++i;
    } else if (i == ea.size() || key_less(eb[j], ea[i])) {
      out.push_back({ChangeKind::Added, std::string(entry_name(*eb[j])), kEmpty, eb[j]->value});
      ++j;
    } else {
      if (ea[i]->value != eb[j]->value && digest(ea[i]->value) != digest(eb[j]->value))
        out.push_back({ChangeKind::Modified, std::string(entry_name(*ea[i])), ea[i]->value, eb[j]->value});
      ++i;
      ++j;
    }
  }
}

void NodeStore::diff_rec(NodeRef a, NodeRef b, unsigned shift, std::vector<Change>& out) const {
  if (a == b) return;
  if (!a.empty() && !b.empty() && digest(a) == digest(b)) return;
  if (a.tag() == Tag::Branch && b.tag() == Tag::Branch && !branch(a).collision && !branch(b).collision) {
    const Branch& ba = branch(a);
    const Branch& bb = branch(b);
    const auto ka = children(ba);
    const auto kb = children(bb);
    for (std::uint32_t slot = 0; slot < 32; ++slot) {
      const std::uint32_t bit = 1u << slot;
      const NodeRef ca = (ba.bitmap & bit) ? ka[rank_of(ba.bitmap, bit)] : kEmpty;
      const NodeRef cb = (bb.bitmap & bit) ? kb[rank_of(bb.bitmap, bit)] : kEmpty;
      if (ca != cb) diff_rec(ca, cb, shift + kBits, out);
    }
    return;
  }
  diff_fallback(a, b, out);
}

std::vector<Change> NodeStore::diff(NodeRef a, NodeRef b) const {
  std::vector<Change> out;
  diff_rec(a, b, 0, out);
  std::sort(out.begin(), out.end(), [](const Change& x, const Change& y) { return x.key < y.key; });
  return out;
}

// ---------------------------------------------------------------------------------
// Garbage collection
// ---------------------------------------------------------------------------------

std::size_t NodeStore::collect(std::span<const NodeRef> roots) {
  auto clear = [](auto& pool) { std::fill(pool.mark.begin(), pool.mark.end(), std::uint8_t{0}); };
  clear(branches_);
  clear(entries_);
  clear(blobs_);
  clear(files_);
  clear(pages_);

  std::vector<NodeRef> stack(roots.begin(), roots.end());
  auto mark = [](auto& pool, std::uint32_t i) {
    if (pool.mark[i]) return false;
    pool.mark[i] = 1;
    return true;
  };
  while (!stack.empty()) {
    const NodeRef n = stack.back();
    stack.pop_back();
    switch (n.tag()) {
      case Tag::Branch:
        if (mark(branches_, n.index())) {
          const auto kids = children(branch(n));
          stack.insert(stack.end(), kids.begin(), kids.end());
        }
        break;
      case Tag::Entry:
        if (mark(entries_, n.index())) {
          stack.push_back(entry(n).name);
          stack.push_back(entry(n).value);
        }
        break;
      case Tag::File:
        if (mark(files_, n.index())) stack.push_back(file(n).pages);
        break;
      case Tag::Blob:
        mark(blobs_, n.index());
        break;
      case Tag::Page:
        mark(pages_, n.index());
        break;
      case Tag::Empty:
        break;
    }
  }

  std::size_t freed = 0;
  for (std::uint32_t i = 0; i < branches_.alive.size(); ++i) {
    if (branches_.alive[i] && !branches_.mark[i]) {
      const Branch& b = branches_.items[i];
      slabs_[b.count].free_slots.push_back(b.slot);
      branches_.release(i);
      ++freed;
    }
  }
  auto sweep = [&](auto& pool, auto&& on_free) {
    for (std::uint32_t i = 0; i < pool.alive.size(); ++i) {
      if (pool.alive[i] && !pool.mark[i]) {
        on_free(i);
        pool.release(i);
        ++freed;
      }
    }
  };
  sweep(entries_, [](std::uint32_t) {});
  sweep(files_, [](std::uint32_t) {});
  sweep(pages_, [](std::uint32_t) {});
  sweep(blobs_, [&](std::uint32_t i) { std::string().swap(blobs_.items[i].bytes); });
  return freed;
}

StoreStats NodeStore::stats() const {
  StoreStats s;
  s.branches = branches_.live;
  s.entries = entries_.live;
  s.blobs = blobs_.live;
  s.pages = pages_.live;
  s.files = files_.live;
  std::uint64_t slab_bytes = 0;
  for (std::size_t c = 1; c < slabs_.size(); ++c)
    slab_bytes += (slabs_[c].data.size() - slabs_[c].free_slots.size() * c) * sizeof(NodeRef);
  std::uint64_t blob_bytes = 0;
  for (std::uint32_t i = 0; i < blobs_.alive.size(); ++i)
    if (blobs_.alive[i]) blob_bytes += blobs_.items[i].bytes.size();
  s.bytes = s.branches * sizeof(Branch) + slab_bytes + s.entries * sizeof(Entry) + s.blobs * sizeof(Blob) +
            blob_bytes + s.files * sizeof(File) + s.pages * (kPageSize + sizeof(Digest128));
  s.pages_allocated = pages_allocated_;
  s.page_bytes_copied = page_bytes_copied_;
  return s;
}

}  // namespace khonsu

// Khonsu — persistent, content-addressed node store.
//
// The whole agent world at a step is one immutable tree ("world map"): a 32-way
// bitmap-compressed HAMT from string keys to values. Values are Blobs (agent
// memory) or Files. A File is itself a u64-keyed HAMT from page index to a 4 KiB
// Page. Updates path-copy O(log32 n) nodes; the previous root stays valid, which
// is what makes fork() a single handle copy.
//
// Nodes are addressed by 32-bit handles (4-bit tag | 28-bit index) into typed
// pools — no shared_ptr, no atomic refcounts. Memory is reclaimed by mark/sweep
// from the roots the kernel retains. The HAMT is kept canonical (single-entry
// sub-branches collapse), so equal key sets have equal structure and equal
// Merkle digests regardless of edit history.
#pragma once

#include "khonsu/common.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace khonsu {

inline constexpr std::size_t kPageSize = 4096;

enum class Tag : std::uint8_t { Empty = 0, Branch = 1, Entry = 2, Blob = 3, Page = 4, File = 5 };

struct NodeRef {
  std::uint32_t raw = 0;

  static constexpr std::uint32_t kIndexBits = 28;
  static constexpr std::uint32_t kIndexMask = (1u << kIndexBits) - 1;

  [[nodiscard]] static constexpr NodeRef make(Tag t, std::uint32_t index) noexcept {
    return NodeRef{(static_cast<std::uint32_t>(t) << kIndexBits) | (index & kIndexMask)};
  }
  [[nodiscard]] constexpr Tag tag() const noexcept { return static_cast<Tag>(raw >> kIndexBits); }
  [[nodiscard]] constexpr std::uint32_t index() const noexcept { return raw & kIndexMask; }
  [[nodiscard]] constexpr bool empty() const noexcept { return raw == 0; }
  friend constexpr bool operator==(NodeRef, NodeRef) = default;
};

inline constexpr NodeRef kEmpty{};

struct StoreStats {
  std::uint64_t branches = 0, entries = 0, blobs = 0, pages = 0, files = 0;
  std::uint64_t bytes = 0;            // approximate resident bytes of live nodes
  std::uint64_t pages_allocated = 0;  // lifetime counter (CoW copies show up here)
  std::uint64_t page_bytes_copied = 0;
};

enum class ChangeKind : std::uint8_t { Added, Removed, Modified };

struct Change {
  ChangeKind kind;
  std::string key;
  NodeRef before, after;
};

// One page slot of a file, used by the CoW page inspector.
struct PageSlot {
  std::uint64_t index;
  NodeRef page;
};

class NodeStore {
public:
  NodeStore();
  ~NodeStore();
  NodeStore(const NodeStore&) = delete;
  NodeStore& operator=(const NodeStore&) = delete;

  // ---- World map (string-keyed) ----------------------------------------------
  [[nodiscard]] NodeRef get(NodeRef root, std::string_view key) const;
  [[nodiscard]] NodeRef put(NodeRef root, std::string_view key, NodeRef value);
  [[nodiscard]] NodeRef erase(NodeRef root, std::string_view key);
  void for_each(NodeRef root, const std::function<void(std::string_view key, NodeRef value)>& fn) const;
  [[nodiscard]] std::size_t size(NodeRef root) const;

  // Convenience: blob values.
  [[nodiscard]] NodeRef put_text(NodeRef root, std::string_view key, std::string_view text);
  [[nodiscard]] std::optional<std::string_view> get_text(NodeRef root, std::string_view key) const;

  // ---- Blobs -------------------------------------------------------------------
  [[nodiscard]] NodeRef make_blob(std::string_view bytes);
  [[nodiscard]] std::string_view blob(NodeRef b) const;

  // ---- Files (4 KiB CoW pages) -------------------------------------------------
  [[nodiscard]] NodeRef empty_file();
  [[nodiscard]] NodeRef file_write(NodeRef file, std::uint64_t offset, std::span<const std::byte> src);
  [[nodiscard]] NodeRef file_truncate(NodeRef file, std::uint64_t size);
  [[nodiscard]] std::uint64_t file_size(NodeRef file) const;
  [[nodiscard]] std::string file_read_all(NodeRef file) const;
  [[nodiscard]] std::vector<PageSlot> file_pages(NodeRef file) const;  // sorted by index

  // World-level helpers for files.
  [[nodiscard]] NodeRef write_file(NodeRef root, std::string_view path, std::uint64_t offset, std::string_view data);
  [[nodiscard]] NodeRef put_file(NodeRef root, std::string_view path, std::string_view contents);
  [[nodiscard]] std::optional<std::string> read_file(NodeRef root, std::string_view path) const;

  // ---- Merkle ------------------------------------------------------------------
  [[nodiscard]] Digest128 digest(NodeRef n) const;  // memoized; O(new nodes) amortized
  // Structural diff of two world maps. Skips identical subtrees by handle and digest.
  [[nodiscard]] std::vector<Change> diff(NodeRef a, NodeRef b) const;

  // ---- Memory management ---------------------------------------------------------
  // Frees every node unreachable from `roots`. Returns number of nodes freed.
  std::size_t collect(std::span<const NodeRef> roots);
  [[nodiscard]] StoreStats stats() const;

private:
  struct Branch {
    std::uint32_t bitmap = 0;  // occupied slots (regular) — unused for collision nodes
    std::uint32_t slot = 0;    // offset (in units of `count`) into children slab `count`
    std::uint8_t count = 0;
    std::uint8_t collision = 0;
    mutable Digest128 digest;
  };
  struct Entry {
    std::uint64_t key = 0;  // hash64(name) for world maps, page index for files
    NodeRef name;           // Blob with the key string, or kEmpty for integer-keyed maps
    NodeRef value;
    mutable Digest128 digest;
  };
  struct Blob {
    std::string bytes;
    mutable Digest128 digest;
  };
  struct File {
    std::uint64_t size = 0;
    NodeRef pages;  // u64-keyed HAMT: page index -> Page
    mutable Digest128 digest;
  };
  struct PageData {
    alignas(64) std::array<std::byte, kPageSize> bytes;
  };

  template <class T, class Container>
  struct Pool {
    Container items;
    std::vector<std::uint32_t> free_list;
    std::vector<std::uint8_t> alive, mark;
    std::uint64_t live = 0;

    template <class... Args>
    std::uint32_t alloc(Args&&... args);
    void release(std::uint32_t i);
  };

  // Children slabs: slab[c] holds nodes with exactly c children, contiguously.
  struct Slab {
    std::vector<NodeRef> data;
    std::vector<std::uint32_t> free_slots;
  };
  static constexpr std::size_t kMaxChildren = 64;  // 32 for regular nodes; collision nodes may exceed

  // Allocation.
  NodeRef new_branch(std::uint32_t bitmap, std::span<const NodeRef> children, bool collision = false);
  NodeRef new_entry(std::uint64_t key, NodeRef name, NodeRef value);
  NodeRef new_page(const std::byte* src);  // src==nullptr -> zero page
  NodeRef new_file(std::uint64_t size, NodeRef pages);

  // Accessors.
  const Branch& branch(NodeRef r) const { return branches_.items[r.index()]; }
  const Entry& entry(NodeRef r) const { return entries_.items[r.index()]; }
  const File& file(NodeRef r) const { return files_.items[r.index()]; }
  const PageData& page(NodeRef r) const { return pages_.items[r.index()]; }
  std::span<const NodeRef> children(const Branch& b) const;

  // Generic HAMT (key + optional name).
  bool same_key(const Entry& e, std::uint64_t key, std::string_view name) const;
  NodeRef lookup(NodeRef root, std::uint64_t key, std::string_view name) const;
  NodeRef insert(NodeRef root, std::uint64_t key, std::string_view name, NodeRef value);
  NodeRef remove(NodeRef root, std::uint64_t key, std::string_view name);
  NodeRef insert_rec(NodeRef node, std::uint64_t key, std::string_view name, NodeRef nameref, NodeRef value,
                     unsigned shift);
  NodeRef remove_rec(NodeRef node, std::uint64_t key, std::string_view name, unsigned shift, bool is_root);
  NodeRef join(NodeRef e1, NodeRef e2, unsigned shift);
  NodeRef make_collision(std::vector<NodeRef> entries);
  void walk_entries(NodeRef node, const std::function<void(const Entry&)>& fn) const;
  void diff_rec(NodeRef a, NodeRef b, unsigned shift, std::vector<Change>& out) const;
  void diff_fallback(NodeRef a, NodeRef b, std::vector<Change>& out) const;
  std::string_view entry_name(const Entry& e) const;

  Pool<Branch, std::vector<Branch>> branches_;
  Pool<Entry, std::vector<Entry>> entries_;
  Pool<Blob, std::deque<Blob>> blobs_;
  Pool<File, std::vector<File>> files_;
  Pool<PageData, std::deque<PageData>> pages_;
  mutable std::vector<Digest128> page_digest_;
  std::array<Slab, kMaxChildren + 1> slabs_{};
  std::uint64_t pages_allocated_ = 0, page_bytes_copied_ = 0;
};

}  // namespace khonsu

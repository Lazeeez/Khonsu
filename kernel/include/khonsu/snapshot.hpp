// Khonsu — workspace snapshots as content-addressed Merkle trees.
//
// A snapshot of the agent's working directory is a tree of objects, like git:
//   blob  = file contents               id = H(domain::kBlob, bytes)
//   tree  = sorted directory entries    id = H(domain::kBranch, encoded entries)
// Unchanged files and whole unchanged subtrees keep their ids, so a new snapshot
// only adds the objects that changed, and diffing two snapshots descends only
// into subtrees whose ids differ: O(changes), not O(files).
//
// Objects are stored inside the recording (Object frames), which keeps a .khn
// file self-contained: it can be checked out, shared, or opened in the browser.
#pragma once

#include "khonsu/common.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace khonsu::ws {

enum class EntryType : std::uint8_t {
  File = 1,
  Dir = 2,
  Symlink = 3,   // blob holds the link target
  Omitted = 4,   // too large to store; size and id are still tracked (changes are detected)
};

struct Entry {
  std::string name;
  EntryType type = EntryType::File;
  std::uint32_t mode = 0644;
  std::uint64_t size = 0;
  Digest128 id;
};

enum class ObjectKind : std::uint8_t { Blob = 1, Tree = 2 };

[[nodiscard]] Digest128 blob_id(std::span<const std::byte> content);
[[nodiscard]] std::vector<std::byte> encode_tree(std::vector<Entry> entries);  // sorts by name
[[nodiscard]] std::optional<std::vector<Entry>> decode_tree(std::span<const std::byte> bytes);
[[nodiscard]] Digest128 tree_id(std::span<const std::byte> encoded);

struct DigestHasher {
  std::size_t operator()(const Digest128& d) const noexcept { return static_cast<std::size_t>(d.lo ^ (d.hi >> 3)); }
};

// Objects by id: the in-memory view of a recording's Object frames.
class ObjectMap {
public:
  bool put(ObjectKind kind, const Digest128& id, std::vector<std::byte> bytes);  // false if already present
  [[nodiscard]] const std::vector<std::byte>* get(const Digest128& id) const;
  [[nodiscard]] bool contains(const Digest128& id) const { return objects_.contains(id); }
  [[nodiscard]] std::size_t size() const { return objects_.size(); }
  [[nodiscard]] std::uint64_t bytes() const { return bytes_; }

private:
  struct Obj {
    ObjectKind kind;
    std::vector<std::byte> bytes;
  };
  std::unordered_map<Digest128, Obj, DigestHasher> objects_;
  std::uint64_t bytes_ = 0;
};

// Object frame payload: kind · id · bytes.
[[nodiscard]] std::vector<std::byte> encode_object(ObjectKind kind, const Digest128& id, std::span<const std::byte> bytes);
struct DecodedObject {
  ObjectKind kind;
  Digest128 id;
  std::vector<std::byte> bytes;
};
[[nodiscard]] std::optional<DecodedObject> decode_object(std::span<const std::byte> payload);

enum class When : std::uint8_t { RunStart = 0, BeforeRequest = 1, RunEnd = 2 };

struct Snapshot {
  When when = When::BeforeRequest;
  std::uint64_t seq = 0;        // request seq this snapshot precedes (BeforeRequest)
  Digest128 root;               // root tree id
  std::uint64_t files = 0, bytes = 0;            // workspace size
  std::uint64_t new_objects = 0, new_bytes = 0;  // what this snapshot added to the recording
  std::uint64_t took_us = 0;
};
[[nodiscard]] std::vector<std::byte> encode(const Snapshot& s);
[[nodiscard]] std::optional<Snapshot> decode_snapshot(std::span<const std::byte> p);

struct FileChange {
  char kind = 'M';  // 'A' added, 'M' modified, 'D' deleted
  std::string path;
  Digest128 before, after;
  EntryType type = EntryType::File;
};

// Structural diff between two roots. Subtrees with equal ids are skipped.
// `a_objects` / `b_objects` may be the same map.
[[nodiscard]] std::vector<FileChange> diff(const ObjectMap& a_objects, const Digest128& a_root,
                                           const ObjectMap& b_objects, const Digest128& b_root);

// Every file under a root, depth-first with sorted names ("dir/file").
struct FileEntry {
  std::string path;
  Entry entry;
};
[[nodiscard]] std::vector<FileEntry> list_files(const ObjectMap& objects, const Digest128& root);

}  // namespace khonsu::ws

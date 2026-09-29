// Khonsu — recordings as content-addressed objects, and Merkle summaries of
// object sets for anti-entropy sync.
//
// pack():   a recording becomes a manifest plus objects. Workspace Object frames
//           are stored under their own content ids (so a repo snapshot shared by
//           many runs is stored and transferred once); all other frames are
//           grouped into chunks. unpack() rebuilds the file byte for byte, so the
//           recording's hash chain still verifies after a round trip.
// Summary:  256 buckets by the first byte of the object id; each bucket's hash
//           covers its sorted ids, the root covers the buckets. Two stores
//           compare roots, then buckets, then exchange only differing buckets'
//           id lists: work proportional to the difference, not the store size.
#pragma once

#include "khonsu/common.hpp"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace khonsu::pack {

// Stored objects are prefixed with a type byte so a receiver can verify the id.
enum class ObjType : char { Chunk = 'C', Blob = 'B', Tree = 'T', Manifest = 'M' };

struct Object {
  Digest128 id;
  std::vector<std::byte> bytes;  // type byte + content
};

// Recomputes the id from the bytes. False if the object is corrupt or forged.
[[nodiscard]] bool verify(const Digest128& id, std::span<const std::byte> bytes);

struct Packed {
  Digest128 manifest_id;
  std::vector<Object> objects;  // manifest first; duplicates removed
  std::uint64_t recording_bytes = 0;
};

// Splits a serialized recording. Chunks are capped at `chunk_bytes`.
[[nodiscard]] std::optional<Packed> pack(std::span<const std::byte> recording, std::string* error,
                                         std::size_t chunk_bytes = 256 * 1024);

// Rebuilds the recording from its manifest, fetching objects through `get`.
using Getter = std::function<std::optional<std::vector<std::byte>>(const Digest128&)>;
[[nodiscard]] std::optional<std::vector<std::byte>> unpack(const Digest128& manifest_id, const Getter& get,
                                                           std::string* error);
// Every object id a manifest references (for reachability and sync).
[[nodiscard]] std::optional<std::vector<Digest128>> references(std::span<const std::byte> manifest_object);

// ---- Merkle summary of a set of ids ------------------------------------------------------
struct Summary {
  std::array<Digest128, 256> buckets{};
  Digest128 root;
  std::size_t count = 0;
};
[[nodiscard]] Summary summarize(std::vector<Digest128> ids);
[[nodiscard]] std::uint8_t bucket_of(const Digest128& id);
[[nodiscard]] Digest128 parse_hex(std::string_view hex, bool* ok = nullptr);

}  // namespace khonsu::pack

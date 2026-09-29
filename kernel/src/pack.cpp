#include "khonsu/pack.hpp"

#include "khonsu/log.hpp"
#include "khonsu/snapshot.hpp"

#include <algorithm>
#include <unordered_set>

namespace khonsu::pack {

namespace {

constexpr std::uint64_t kChunkDomain = 0x4b484e2d43484e4bULL;     // "KHN-CHNK"
constexpr std::uint64_t kManifestDomain = 0x4b484e2d4d414e49ULL;  // "KHN-MANI"
constexpr std::uint64_t kBucketDomain = 0x4b484e2d424b5431ULL;    // "KHN-BKT1"
constexpr std::string_view kManifestMagic = "khonsu-manifest/1";
constexpr std::size_t kFrameHeader = 32;
constexpr std::size_t kFileHeader = 16;

std::size_t padded(std::size_t n) { return (n + 7) & ~std::size_t{7}; }

Digest128 id_of(ObjType t, std::span<const std::byte> content) {
  switch (t) {
    case ObjType::Chunk: return digest_bytes(content, kChunkDomain);
    case ObjType::Manifest: return digest_bytes(content, kManifestDomain);
    case ObjType::Blob: return ws::blob_id(content);
    case ObjType::Tree: return ws::tree_id(content);
  }
  return {};
}

std::vector<std::byte> with_type(ObjType t, std::span<const std::byte> content) {
  std::vector<std::byte> out;
  out.reserve(content.size() + 1);
  out.push_back(static_cast<std::byte>(t));
  out.insert(out.end(), content.begin(), content.end());
  return out;
}

}  // namespace

bool verify(const Digest128& id, std::span<const std::byte> bytes) {
  if (bytes.empty()) return false;
  const auto t = static_cast<ObjType>(bytes[0]);
  if (t != ObjType::Chunk && t != ObjType::Blob && t != ObjType::Tree && t != ObjType::Manifest) return false;
  return id_of(t, bytes.subspan(1)) == id;
}

std::optional<Packed> pack(std::span<const std::byte> rec, std::string* error, std::size_t chunk_bytes) {
  auto fail = [&](const char* m) -> std::optional<Packed> {
    if (error) *error = m;
    return std::nullopt;
  };
  if (rec.size() < kFileHeader) return fail("not a recording");
  // Validate the whole file first: we pack exactly what parses.
  RecoveryReport rr;
  const EventLog log = EventLog::parse(rec, &rr);
  if (!rr.reason.empty()) return fail("recording is damaged (verify it first); refusing to pack a partial file");

  Packed p;
  p.recording_bytes = rec.size();
  std::unordered_set<Digest128, ws::DigestHasher> seen;
  auto add = [&](ObjType t, std::span<const std::byte> content) {
    const Digest128 id = id_of(t, content);
    if (seen.insert(id).second) p.objects.push_back({id, with_type(t, content)});
    return id;
  };

  Writer m;
  m.str(kManifestMagic).varint(log.size());
  std::vector<std::pair<char, Digest128>> entries;
  std::vector<std::byte> chunk;
  auto flush = [&] {
    if (chunk.empty()) return;
    entries.emplace_back('c', add(ObjType::Chunk, chunk));
    chunk.clear();
  };
  std::size_t off = kFileHeader;
  for (std::uint64_t l = 0; l < log.size(); ++l) {
    const FrameView f = log.frame(l);
    const std::size_t total = kFrameHeader + padded(f.header.payload_len);
    const auto bytes = rec.subspan(off, total);
    off += total;
    // Object frames written by the recorder (actor 2, vtime 0) are stored by content id
    // and re-framed on unpack; anything else is kept verbatim inside a chunk.
    if (f.header.type == FrameType::Object && f.header.actor == 2 && f.header.vtime_us == 0 && f.header.flags == 0) {
      if (auto o = ws::decode_object(f.payload)) {
        flush();
        const ObjType t = o->kind == ws::ObjectKind::Blob ? ObjType::Blob : ObjType::Tree;
        entries.emplace_back('o', add(t, o->bytes));
        continue;
      }
    }
    if (chunk.size() + bytes.size() > chunk_bytes) flush();
    chunk.insert(chunk.end(), bytes.begin(), bytes.end());
  }
  flush();
  m.varint(entries.size());
  for (const auto& [k, id] : entries) m.u8(static_cast<std::uint8_t>(k)).digest(id);
  const auto manifest = m.bytes();
  p.manifest_id = id_of(ObjType::Manifest, manifest);
  p.objects.insert(p.objects.begin(), {p.manifest_id, with_type(ObjType::Manifest, manifest)});
  return p;
}

std::optional<std::vector<Digest128>> references(std::span<const std::byte> manifest_object) {
  if (manifest_object.empty() || static_cast<ObjType>(manifest_object[0]) != ObjType::Manifest) return std::nullopt;
  Reader r(manifest_object.subspan(1));
  if (r.str() != kManifestMagic) return std::nullopt;
  r.varint();  // frame count
  const auto n = r.varint();
  std::vector<Digest128> ids;
  for (std::uint64_t i = 0; i < n && r.ok(); ++i) {
    r.u8();
    ids.push_back(r.digest());
  }
  if (!r.ok()) return std::nullopt;
  return ids;
}

std::optional<std::vector<std::byte>> unpack(const Digest128& manifest_id, const Getter& get, std::string* error) {
  auto fail = [&](std::string m) -> std::optional<std::vector<std::byte>> {
    if (error) *error = std::move(m);
    return std::nullopt;
  };
  const auto mo = get(manifest_id);
  if (!mo || !verify(manifest_id, *mo)) return fail("manifest " + manifest_id.hex() + " missing or corrupt");
  Reader r(std::span<const std::byte>(*mo).subspan(1));
  if (r.str() != kManifestMagic) return fail("unsupported manifest");
  const auto frames = r.varint();
  const auto n = r.varint();
  std::vector<std::byte> out = EventLog::file_header();
  std::uint64_t lsn = 0;
  for (std::uint64_t i = 0; i < n && r.ok(); ++i) {
    const char kind = static_cast<char>(r.u8());
    const Digest128 id = r.digest();
    const auto obj = get(id);
    if (!obj || !verify(id, *obj)) return fail("object " + id.hex() + " missing or corrupt");
    const auto content = std::span<const std::byte>(*obj).subspan(1);
    if (kind == 'c') {
      out.insert(out.end(), content.begin(), content.end());
      // count the frames inside the chunk to keep LSNs for re-framed objects
      std::size_t off = 0;
      while (off + kFrameHeader <= content.size()) {
        FrameHeader h;
        std::memcpy(&h, content.data() + off, sizeof h);
        off += kFrameHeader + padded(h.payload_len);
        ++lsn;
      }
    } else {
      const auto kindw = static_cast<ObjType>((*obj)[0]) == ObjType::Blob ? ws::ObjectKind::Blob : ws::ObjectKind::Tree;
      const auto frame =
          EventLog::encode_frame(lsn++, FrameType::Object, 2, 0, ws::encode_object(kindw, id, content));
      out.insert(out.end(), frame.begin(), frame.end());
    }
  }
  if (!r.ok() || lsn != frames) return fail("manifest does not describe a complete recording");
  return out;
}

std::uint8_t bucket_of(const Digest128& id) { return static_cast<std::uint8_t>(id.hi >> 56); }

Summary summarize(std::vector<Digest128> ids) {
  // Bucket order is the id's *high* byte, so sort hi-major. (Digest128's default
  // ordering compares `lo` first; walking buckets over that order silently skipped
  // most ids. Caught by the sync integration test, now pinned by a unit test.)
  std::sort(ids.begin(), ids.end(),
            [](const Digest128& a, const Digest128& b) { return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo; });
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  Summary s;
  s.count = ids.size();
  std::size_t i = 0;
  DigestBuilder root(kBucketDomain);
  for (unsigned b = 0; b < 256; ++b) {
    DigestBuilder h(kBucketDomain);
    h.u64(b);
    std::size_t n = 0;
    while (i < ids.size() && bucket_of(ids[i]) == b) {
      h.digest(ids[i++]);
      ++n;
    }
    s.buckets[b] = n ? h.finish() : Digest128{};
    root.digest(s.buckets[b]);
  }
  s.root = root.finish();
  return s;
}

Digest128 parse_hex(std::string_view hex, bool* ok) {
  Digest128 d;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  bool good = hex.size() == 32;
  for (std::size_t i = 0; good && i < 32; ++i) {
    const int v = nib(hex[i]);
    if (v < 0) {
      good = false;
      break;
    }
    std::uint64_t& w = i < 16 ? d.hi : d.lo;
    w = (w << 4) | static_cast<std::uint64_t>(v);
  }
  if (ok) *ok = good;
  return good ? d : Digest128{};
}

}  // namespace khonsu::pack

#include "khonsu/log.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

namespace khonsu {

// ---------------------------------------------------------------------------------
// CRC32C (Castagnoli). Hardware instruction when available, slicing-by-8 otherwise
// (WebAssembly has no CRC or carry-less multiply instruction).
// ---------------------------------------------------------------------------------

namespace {
constexpr std::uint32_t kPoly = 0x82f63b78u;  // reflected Castagnoli polynomial

constexpr std::array<std::array<std::uint32_t, 256>, 8> make_tables() {
  std::array<std::array<std::uint32_t, 256>, 8> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ ((c & 1) ? kPoly : 0);
    t[0][i] = c;
  }
  for (std::uint32_t i = 0; i < 256; ++i)
    for (std::size_t s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xff];
  return t;
}
constexpr auto kTables = make_tables();

std::uint32_t load_le32(const std::byte* p) {
  return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
         static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
}  // namespace

std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t crc) noexcept {
  crc = ~crc;
  const std::byte* p = data.data();
  std::size_t n = data.size();
#if defined(__SSE4_2__) && defined(__x86_64__)
  while (n >= 8) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    crc = static_cast<std::uint32_t>(_mm_crc32_u64(crc, v));
    p += 8;
    n -= 8;
  }
#else
  while (n >= 8) {
    const std::uint32_t lo = load_le32(p) ^ crc;
    const std::uint32_t hi = load_le32(p + 4);
    crc = kTables[7][lo & 0xff] ^ kTables[6][(lo >> 8) & 0xff] ^ kTables[5][(lo >> 16) & 0xff] ^
          kTables[4][lo >> 24] ^ kTables[3][hi & 0xff] ^ kTables[2][(hi >> 8) & 0xff] ^
          kTables[1][(hi >> 16) & 0xff] ^ kTables[0][hi >> 24];
    p += 8;
    n -= 8;
  }
#endif
  while (n--) crc = (crc >> 8) ^ kTables[0][(crc ^ static_cast<std::uint32_t>(*p++)) & 0xff];
  return ~crc;
}

std::string_view frame_type_name(FrameType t) noexcept {
  switch (t) {
    case FrameType::Genesis: return "Genesis";
    case FrameType::StepBegin: return "StepBegin";
    case FrameType::StepCommit: return "StepCommit";
    case FrameType::OracleRequest: return "OracleRequest";
    case FrameType::OracleResponse: return "OracleResponse";
    case FrameType::Fork: return "Fork";
    case FrameType::UserEdit: return "UserEdit";
    case FrameType::Fault: return "Fault";
  }
  return "Unknown";
}

// ---------------------------------------------------------------------------------
// EventLog
// ---------------------------------------------------------------------------------

namespace {
constexpr std::size_t kHeader = sizeof(FrameHeader);
constexpr std::size_t kCrcStart = 8;  // magic_ver(4) + crc32c(4)
constexpr std::array<char, 8> kFileMagic = {'K', 'H', 'N', 'L', 'O', 'G', '\0', '\1'};

constexpr std::size_t padded(std::size_t n) { return (n + 7) & ~std::size_t{7}; }

FrameHeader read_header(const std::byte* p) {
  FrameHeader h;
  std::memcpy(&h, p, sizeof h);  // never reinterpret_cast an arbitrary buffer
  return h;
}
static_assert(std::endian::native == std::endian::little, "big-endian hosts need byteswapping in read/write");
}  // namespace

std::uint64_t EventLog::append(FrameType type, std::uint16_t actor, std::uint64_t vtime_us,
                               std::span<const std::byte> payload, std::uint8_t flags) {
  if (payload.size() > 0xffffffffu) throw std::length_error("khonsu: frame payload too large");
  const std::uint64_t lsn = index_.size();
  const std::size_t off = data_.size();
  const std::size_t total = kHeader + padded(payload.size());
  data_.resize(off + total, std::byte{0});

  FrameHeader h{kFrameMagic, 0, lsn, vtime_us, static_cast<std::uint32_t>(payload.size()), type, flags, actor};
  std::memcpy(data_.data() + off, &h, sizeof h);
  if (!payload.empty()) std::memcpy(data_.data() + off + kHeader, payload.data(), payload.size());
  h.crc32c = crc32c({data_.data() + off + kCrcStart, kHeader - kCrcStart + payload.size()});
  std::memcpy(data_.data() + off + 4, &h.crc32c, 4);

  index_.push_back(off);
  return lsn;
}

FrameView EventLog::frame(std::uint64_t lsn) const {
  if (lsn >= index_.size()) throw std::out_of_range("khonsu: lsn out of range");
  const std::byte* p = data_.data() + offset_of(lsn);
  const FrameHeader h = read_header(p);
  return {h, {p + kHeader, h.payload_len}};
}

bool EventLog::verify(std::uint64_t lsn) const {
  const FrameView f = frame(lsn);
  const std::byte* p = data_.data() + offset_of(lsn);
  return f.header.magic_ver == kFrameMagic &&
         crc32c({p + kCrcStart, kHeader - kCrcStart + f.header.payload_len}) == f.header.crc32c;
}

std::vector<std::byte> EventLog::serialize() const {
  std::vector<std::byte> out(16 + data_.size());
  std::memcpy(out.data(), kFileMagic.data(), 8);
  const std::uint64_t n = index_.size();
  std::memcpy(out.data() + 8, &n, 8);
  if (!data_.empty()) std::memcpy(out.data() + 16, data_.data(), data_.size());
  return out;
}

EventLog EventLog::parse(std::span<const std::byte> bytes, RecoveryReport* report) {
  EventLog log;
  RecoveryReport r;
  if (bytes.size() < 16 || std::memcmp(bytes.data(), kFileMagic.data(), 8) != 0) {
    r.reason = "bad file header";
    r.bytes_discarded = bytes.size();
    if (report) *report = r;
    return log;
  }
  std::size_t off = 16;
  while (off < bytes.size()) {
    if (bytes.size() - off < kHeader) {
      r.reason = "torn frame header";
      break;
    }
    const FrameHeader h = read_header(bytes.data() + off);
    if (h.magic_ver != kFrameMagic) {
      r.reason = "bad frame magic at lsn " + std::to_string(log.size());
      break;
    }
    if (h.lsn != log.size()) {
      r.reason = "lsn gap at lsn " + std::to_string(log.size());
      break;
    }
    const std::size_t total = kHeader + padded(h.payload_len);
    if (bytes.size() - off < total) {
      r.reason = "torn frame payload at lsn " + std::to_string(h.lsn);
      break;
    }
    if (crc32c({bytes.data() + off + kCrcStart, kHeader - kCrcStart + h.payload_len}) != h.crc32c) {
      r.reason = "crc mismatch at lsn " + std::to_string(h.lsn);
      break;
    }
    log.index_.push_back(log.data_.size());
    log.data_.insert(log.data_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(off),
                     bytes.begin() + static_cast<std::ptrdiff_t>(off + total));
    off += total;
  }
  r.frames_recovered = log.size();
  r.bytes_discarded = bytes.size() - std::min(bytes.size(), off);
  if (report) *report = r;
  return log;
}

void EventLog::corrupt_for_test(std::uint64_t lsn, std::size_t payload_offset) {
  const FrameView f = frame(lsn);
  if (f.header.payload_len == 0) return;
  auto& b = data_[offset_of(lsn) + kHeader + (payload_offset % f.header.payload_len)];
  b ^= std::byte{0x01};
}

void EventLog::tamper_for_test(std::uint64_t lsn, std::size_t payload_offset) {
  const FrameView f = frame(lsn);
  if (f.header.payload_len == 0) return;
  const std::size_t off = offset_of(lsn);
  data_[off + kHeader + (payload_offset % f.header.payload_len)] ^= std::byte{0x01};
  const std::uint32_t crc = crc32c({data_.data() + off + kCrcStart, kHeader - kCrcStart + f.header.payload_len});
  std::memcpy(data_.data() + off + 4, &crc, 4);
}

// ---------------------------------------------------------------------------------
// Payload encoding
// ---------------------------------------------------------------------------------

Writer& Writer::varint(std::uint64_t v) {
  while (v >= 0x80) {
    buf_.push_back(static_cast<std::byte>((v & 0x7f) | 0x80));
    v >>= 7;
  }
  buf_.push_back(static_cast<std::byte>(v));
  return *this;
}

Writer& Writer::str(std::string_view s) {
  varint(s.size());
  const auto b = as_bytes(s);
  buf_.insert(buf_.end(), b.begin(), b.end());
  return *this;
}

Writer& Writer::digest(const Digest128& d) {
  for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<std::byte>(d.lo >> (8 * i)));
  for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<std::byte>(d.hi >> (8 * i)));
  return *this;
}

std::uint8_t Reader::u8() {
  if (pos_ >= b_.size()) {
    ok_ = false;
    return 0;
  }
  return static_cast<std::uint8_t>(b_[pos_++]);
}

std::uint64_t Reader::varint() {
  std::uint64_t v = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    const std::uint8_t b = u8();
    if (!ok_) return 0;
    v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
    if (!(b & 0x80)) return v;
  }
  ok_ = false;
  return 0;
}

std::string_view Reader::str() {
  const std::uint64_t n = varint();
  if (!ok_ || n > b_.size() - pos_) {
    ok_ = false;
    return {};
  }
  std::string_view s(reinterpret_cast<const char*>(b_.data() + pos_), static_cast<std::size_t>(n));
  pos_ += static_cast<std::size_t>(n);
  return s;
}

Digest128 Reader::digest() {
  Digest128 d;
  for (int i = 0; i < 8; ++i) d.lo |= static_cast<std::uint64_t>(u8()) << (8 * i);
  for (int i = 0; i < 8; ++i) d.hi |= static_cast<std::uint64_t>(u8()) << (8 * i);
  return d;
}

}  // namespace khonsu

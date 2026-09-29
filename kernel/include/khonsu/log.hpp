// Khonsu — event log: the source of truth for a timeline (and its durability WAL).
//
// Frames are a 32-byte header + payload padded to 8 bytes. Every frame carries a
// CRC32C over [crc field end, payload end). StepCommit frames additionally carry
// the Merkle state digest and a running hash chain over all prior frames, so both
// bit-rot (CRC) and history rewriting (chain) are detectable.
#pragma once

#include "khonsu/common.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <string>
#include <string_view>
#include <vector>

namespace khonsu {

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t crc = 0) noexcept;

enum class FrameType : std::uint8_t {
  Genesis = 1,
  StepBegin = 2,
  StepCommit = 3,
  OracleRequest = 4,
  OracleResponse = 5,
  Fork = 6,
  UserEdit = 7,
  Fault = 8,
};

[[nodiscard]] std::string_view frame_type_name(FrameType t) noexcept;

inline constexpr std::uint32_t kFrameMagic = 0x014e484bu;  // 'K' 'H' 'N' v1 (little-endian)

struct FrameHeader {
  std::uint32_t magic_ver;
  std::uint32_t crc32c;       // covers bytes [8, 32 + payload_len)
  std::uint64_t lsn;          // dense and monotonic within one log
  std::uint64_t vtime_us;     // virtual time; wall clock only enters via OracleResponse
  std::uint32_t payload_len;  // payload follows, zero-padded to a multiple of 8
  FrameType type;
  std::uint8_t flags;
  std::uint16_t actor;
};
static_assert(sizeof(FrameHeader) == 32);
static_assert(std::is_trivially_copyable_v<FrameHeader>);

struct FrameView {
  FrameHeader header;
  std::span<const std::byte> payload;
};

struct RecoveryReport {
  std::uint64_t frames_recovered = 0;
  std::uint64_t bytes_discarded = 0;
  std::string reason;  // empty when the whole log was intact
};

class EventLog {
public:
  // Appends a frame and returns its LSN.
  std::uint64_t append(FrameType type, std::uint16_t actor, std::uint64_t vtime_us, std::span<const std::byte> payload,
                       std::uint8_t flags = 0);
  std::uint64_t append(FrameType type, std::uint16_t actor, std::uint64_t vtime_us, std::string_view payload) {
    return append(type, actor, vtime_us, as_bytes(payload));
  }

  [[nodiscard]] std::uint64_t size() const noexcept { return index_.size(); }
  [[nodiscard]] std::uint64_t bytes() const noexcept { return data_.size(); }
  [[nodiscard]] FrameView frame(std::uint64_t lsn) const;
  [[nodiscard]] bool verify(std::uint64_t lsn) const;  // re-check CRC of one frame

  // On-disk format: 16-byte file header ("KHNLOG\0\1" + frame count) + frames.
  [[nodiscard]] std::vector<std::byte> serialize() const;
  // Loads frames up to the first torn or corrupt frame (like WAL recovery).
  static EventLog parse(std::span<const std::byte> bytes, RecoveryReport* report = nullptr);

  // Test hook: flip one byte inside a frame's payload (simulates bit-rot).
  void corrupt_for_test(std::uint64_t lsn, std::size_t payload_offset);
  // Test hook: an adversary who edits a payload byte *and* recomputes the CRC.
  // CRC cannot catch this; replay digests and the StepCommit hash chain do.
  void tamper_for_test(std::uint64_t lsn, std::size_t payload_offset);

private:
  std::vector<std::byte> data_;
  std::vector<std::size_t> index_;  // lsn -> byte offset (dense index; seek is O(1))
  [[nodiscard]] std::size_t offset_of(std::uint64_t lsn) const { return index_[static_cast<std::size_t>(lsn)]; }
};

// Little binary writer/reader for payloads (LE varints + length-prefixed strings).
class Writer {
public:
  Writer& u8(std::uint8_t v) { buf_.push_back(static_cast<std::byte>(v)); return *this; }
  Writer& varint(std::uint64_t v);
  Writer& str(std::string_view s);
  Writer& digest(const Digest128& d);
  [[nodiscard]] const std::vector<std::byte>& bytes() const { return buf_; }
  [[nodiscard]] std::span<const std::byte> span() const { return buf_; }

private:
  std::vector<std::byte> buf_;
};

class Reader {
public:
  explicit Reader(std::span<const std::byte> b) : b_(b) {}
  std::uint8_t u8();
  std::uint64_t varint();
  std::string_view str();
  Digest128 digest();
  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] bool done() const { return pos_ == b_.size(); }

private:
  std::span<const std::byte> b_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace khonsu

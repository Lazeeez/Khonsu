// Khonsu — shared primitives: digests, hashing, deterministic PRNG.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace khonsu {

// 128-bit content digest (XXH3-128). All-zero means "not yet computed".
struct Digest128 {
  std::uint64_t lo = 0, hi = 0;
  [[nodiscard]] constexpr bool empty() const noexcept { return lo == 0 && hi == 0; }
  friend constexpr auto operator<=>(const Digest128&, const Digest128&) = default;
  [[nodiscard]] std::string hex() const;
  [[nodiscard]] std::string short_hex() const { return hex().substr(0, 8); }
};

[[nodiscard]] Digest128 digest_bytes(std::span<const std::byte> bytes, std::uint64_t domain) noexcept;
[[nodiscard]] Digest128 digest_bytes(std::string_view s, std::uint64_t domain) noexcept;
[[nodiscard]] std::uint64_t hash64(std::string_view s) noexcept;

// Incremental digest builder for composite nodes (branches, entries, roots).
class DigestBuilder {
public:
  explicit DigestBuilder(std::uint64_t domain) noexcept;
  ~DigestBuilder();
  DigestBuilder(const DigestBuilder&) = delete;
  DigestBuilder& operator=(const DigestBuilder&) = delete;

  DigestBuilder& u64(std::uint64_t v) noexcept;
  DigestBuilder& digest(const Digest128& d) noexcept;
  DigestBuilder& bytes(std::span<const std::byte> b) noexcept;
  [[nodiscard]] Digest128 finish() noexcept;  // never returns the all-zero digest

private:
  alignas(64) unsigned char state_[576];  // opaque XXH3_state_t storage
};

// Domain separators so that e.g. a blob and a page with equal bytes never collide.
namespace domain {
inline constexpr std::uint64_t kBlob = 0x4b484e2d424c4f42ULL;    // "KHN-BLOB"
inline constexpr std::uint64_t kPage = 0x4b484e2d50414745ULL;    // "KHN-PAGE"
inline constexpr std::uint64_t kBranch = 0x4b484e2d42524e43ULL;  // "KHN-BRNC"
inline constexpr std::uint64_t kEntry = 0x4b484e2d454e5452ULL;   // "KHN-ENTR"
inline constexpr std::uint64_t kFile = 0x4b484e2d46494c45ULL;    // "KHN-FILE"
inline constexpr std::uint64_t kChain = 0x4b484e2d43484149ULL;   // "KHN-CHAI"
inline constexpr std::uint64_t kRequest = 0x4b484e2d52455153ULL; // "KHN-REQS"
}  // namespace domain

// SplitMix64: seed derivation. Xoshiro256**: per-timeline deterministic streams.
[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t x) noexcept {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

class Xoshiro256 {
public:
  constexpr explicit Xoshiro256(std::uint64_t seed) noexcept {
    for (auto& w : s_) w = seed = splitmix64(seed);
  }
  constexpr std::uint64_t next() noexcept {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0]; s_[3] ^= s_[1]; s_[1] ^= s_[2]; s_[0] ^= s_[3];
    s_[2] ^= t; s_[3] = rotl(s_[3], 45);
    return result;
  }
  // Unbiased bounded integer in [0, bound) (Lemire's method without division in the fast path).
  constexpr std::uint64_t below(std::uint64_t bound) noexcept {
    if (bound <= 1) return 0;
    const std::uint64_t threshold = (0 - bound) % bound;
    for (;;) {
      const std::uint64_t r = next();
      if (r >= threshold) return r % bound;
    }
  }

private:
  static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
  std::uint64_t s_[4]{};
};

[[nodiscard]] inline std::span<const std::byte> as_bytes(std::string_view s) noexcept {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

}  // namespace khonsu

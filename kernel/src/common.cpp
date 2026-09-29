#include "khonsu/common.hpp"

// On wasm, xxHash reaches SIMD128 through Emscripten's NEON-compat (SIMDe) header,
// which must be included from C++ linkage before xxhash.h opens its extern "C" block.
#if defined(__wasm_simd128__) && __has_include(<arm_neon.h>)
#include <arm_neon.h>
#endif
#define XXH_INLINE_ALL
#include "xxhash.h"

#include <new>

namespace khonsu {

namespace {
constexpr char kHex[] = "0123456789abcdef";

Digest128 from_xxh(XXH128_hash_t h) noexcept {
  Digest128 d{h.low64, h.high64};
  if (d.empty()) d.lo = 1;  // reserve all-zero for "not computed"
  return d;
}

XXH3_state_t* state_of(unsigned char* storage) noexcept {
  return std::launder(reinterpret_cast<XXH3_state_t*>(storage));
}
}  // namespace

std::string Digest128::hex() const {
  std::string out(32, '0');
  for (int i = 0; i < 16; ++i) {
    const std::uint64_t word = i < 8 ? hi : lo;
    const unsigned byte = static_cast<unsigned>((word >> (8 * (7 - (i % 8)))) & 0xff);
    out[2 * i] = kHex[byte >> 4];
    out[2 * i + 1] = kHex[byte & 0xf];
  }
  return out;
}

Digest128 digest_bytes(std::span<const std::byte> bytes, std::uint64_t domain) noexcept {
  return from_xxh(XXH3_128bits_withSeed(bytes.data(), bytes.size(), domain));
}

Digest128 digest_bytes(std::string_view s, std::uint64_t domain) noexcept {
  return digest_bytes(as_bytes(s), domain);
}

std::uint64_t hash64(std::string_view s) noexcept { return XXH3_64bits(s.data(), s.size()); }

static_assert(sizeof(XXH3_state_t) <= 576, "enlarge DigestBuilder::state_");
static_assert(alignof(XXH3_state_t) <= 64, "DigestBuilder::state_ alignment");

DigestBuilder::DigestBuilder(std::uint64_t domain) noexcept {
  // Reset with the *default* secret, then absorb the domain as the first word.
  // (XXH3_*_reset_withSeed on an un-zeroed, stack-emplaced state may skip secret
  // derivation when the garbage `seed` field happens to match — hashes then depend
  // on stale stack bytes. Found by verify() diverging only at -O2.)
  auto* st = new (state_) XXH3_state_t;
  XXH3_128bits_reset(st);
  u64(domain);
}

DigestBuilder::~DigestBuilder() { state_of(state_)->~XXH3_state_t(); }

DigestBuilder& DigestBuilder::u64(std::uint64_t v) noexcept {
  unsigned char le[8];
  for (int i = 0; i < 8; ++i) le[i] = static_cast<unsigned char>(v >> (8 * i));  // endian-stable
  XXH3_128bits_update(state_of(state_), le, sizeof le);
  return *this;
}

DigestBuilder& DigestBuilder::digest(const Digest128& d) noexcept { return u64(d.lo).u64(d.hi); }

DigestBuilder& DigestBuilder::bytes(std::span<const std::byte> b) noexcept {
  u64(b.size());
  XXH3_128bits_update(state_of(state_), b.data(), b.size());
  return *this;
}

Digest128 DigestBuilder::finish() noexcept { return from_xxh(XXH3_128bits_digest(state_of(state_))); }

}  // namespace khonsu

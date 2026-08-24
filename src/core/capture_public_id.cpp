#include "core/capture_public_id.h"

#include <chrono>

namespace cambang {

namespace {

// Crockford base32: no I, L, O or U, so an id read aloud or retyped from a log
// cannot become a different id.
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr std::size_t kPrefixLength = 3;   // "dc_" / "rc_"
constexpr std::size_t kTimestampChars = 10; // 48 bits, left-padded into 50
constexpr std::size_t kEntropyChars = 16;   // 80 bits
constexpr std::size_t kBodyChars = kTimestampChars + kEntropyChars;
constexpr std::size_t kTotalChars = kPrefixLength + kBodyChars;

// The 80-bit entropy is held as (hi16, lo64). Shifting it right as one value is
// the only fiddly part of the encoding, so it lives in one place.
std::uint64_t shift_right_80(std::uint64_t hi16, std::uint64_t lo64, unsigned n) noexcept {
  if (n == 0) return lo64;
  if (n < 64) return (lo64 >> n) | (hi16 << (64 - n));
  return hi16 >> (n - 64);
}

bool is_alphabet_char(char c) noexcept {
  for (std::size_t i = 0; i < 32; ++i) {
    if (kAlphabet[i] == c) return true;
  }
  return false;
}

std::uint64_t wall_clock_unix_ms() noexcept {
  // Wall clock, NOT the steady clock Core times with. Sortability across
  // sessions is the entire point, and a monotonic-since-boot clock cannot give
  // it: two sessions would both start near zero.
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

} // namespace

const char* capture_public_id_prefix(CapturePublicIdSpace space) noexcept {
  return space == CapturePublicIdSpace::RigCapture ? "rc_" : "dc_";
}

std::optional<CapturePublicIdSpace> capture_public_id_space(
    const std::string& text) noexcept {
  if (text.size() != kTotalChars) {
    return std::nullopt;
  }
  CapturePublicIdSpace space{};
  if (text.compare(0, kPrefixLength, "dc_") == 0) {
    space = CapturePublicIdSpace::DeviceCapture;
  } else if (text.compare(0, kPrefixLength, "rc_") == 0) {
    space = CapturePublicIdSpace::RigCapture;
  } else {
    return std::nullopt;
  }
  // Alphabet too, not just the prefix: a truncated or corrupted id that happens
  // to begin "dc_" is not a Device Capture Id, and accepting it would hand the
  // caller a lookup miss instead of the malformed-input answer.
  for (std::size_t i = kPrefixLength; i < text.size(); ++i) {
    if (!is_alphabet_char(text[i])) {
      return std::nullopt;
    }
  }
  return space;
}

std::string encode_capture_public_id(CapturePublicIdSpace space,
                                     std::uint64_t unix_ms,
                                     std::uint64_t entropy_hi16,
                                     std::uint64_t entropy_lo64) {
  std::string out;
  out.reserve(kTotalChars);
  out.append(capture_public_id_prefix(space));

  // Timestamp: 48 bits into 10 characters, most significant first, so
  // lexicographic order follows time order.
  const std::uint64_t ms = unix_ms & 0x0000FFFFFFFFFFFFull;
  for (std::size_t i = 0; i < kTimestampChars; ++i) {
    const unsigned shift = static_cast<unsigned>(45 - 5 * i);
    out.push_back(kAlphabet[(ms >> shift) & 0x1Full]);
  }

  // Entropy: 80 bits into 16 characters, same order, so two ids minted in the
  // same millisecond order by their entropy -- which the minter increments
  // rather than redraws.
  const std::uint64_t hi = entropy_hi16 & 0xFFFFull;
  for (std::size_t i = 0; i < kEntropyChars; ++i) {
    const unsigned shift = static_cast<unsigned>(75 - 5 * i);
    out.push_back(kAlphabet[shift_right_80(hi, entropy_lo64, shift) & 0x1Full]);
  }
  return out;
}

CapturePublicIdMinter::CapturePublicIdMinter() : rng_(std::random_device{}()) {}

CapturePublicIdMinter::CapturePublicIdMinter(std::uint64_t seed) noexcept : rng_(seed) {}

void CapturePublicIdMinter::advance_entropy_locked_(std::uint64_t unix_ms) {
  if (has_minted_ && unix_ms == last_ms_) {
    // Same millisecond: increment the 80-bit entropy as one number. Redrawing
    // it here would order ids minted in one instant at random, which is exactly
    // the case a rig capture produces.
    if (++entropy_lo64_ == 0) {
      entropy_hi16_ = (entropy_hi16_ + 1) & 0xFFFFull;
    }
    return;
  }
  // New millisecond -- or a backwards clock step, which the timestamp itself
  // already fails to order. Fresh entropy.
  entropy_lo64_ = rng_();
  entropy_hi16_ = rng_() & 0xFFFFull;
  last_ms_ = unix_ms;
  has_minted_ = true;
}

std::string CapturePublicIdMinter::mint_at(CapturePublicIdSpace space,
                                           std::uint64_t unix_ms) {
  advance_entropy_locked_(unix_ms);
  return encode_capture_public_id(space, unix_ms, entropy_hi16_, entropy_lo64_);
}

std::string CapturePublicIdMinter::mint(CapturePublicIdSpace space) {
  return mint_at(space, wall_clock_unix_ms());
}

} // namespace cambang

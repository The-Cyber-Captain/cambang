#pragma once

#include <cstdint>
#include <optional>
#include <random>
#include <string>

namespace cambang {

// Durable public capture ids (capture_identity_and_lifecycle.md 2.2).
//
// A session-local uint64 collides on first reload and is therefore unfit as a
// public identity: capture results get serialised, stored, and read back in a
// later session or on another machine. The public form is an opaque,
// type-prefixed, lexicographically sortable string -- dc_<ulid> for a Device
// Capture, rc_<ulid> for a Rig Capture. The uint64 remains the internal
// identity for Core keying and hot-path lookups, unchanged.
//
// This lives in src/core/ rather than src/godot/ despite being minted at the
// boundary: it is a pure function of a clock and an entropy source, with no
// Godot types, and putting it here is what lets the host-native verifiers test
// it at all.

enum class CapturePublicIdSpace : std::uint8_t {
  DeviceCapture = 0,
  RigCapture = 1,
};

// "dc_" / "rc_". The prefix is load-bearing rather than decorative: 2.2
// requires that a Rig Capture Id can never be silently accepted where a Device
// Capture Id is expected, so a caller validates the space and REFUSES a
// mismatch instead of merely failing to find it.
const char* capture_public_id_prefix(CapturePublicIdSpace space) noexcept;

// The space a public id belongs to, or nullopt when the text is not a
// well-formed public capture id at all. Length and alphabet are both checked:
// a truncated id that happens to start with "dc_" is not a Device Capture Id.
std::optional<CapturePublicIdSpace> capture_public_id_space(
    const std::string& text) noexcept;

// Crockford base32 ULID body: a 48-bit millisecond timestamp followed by 80
// bits of entropy, 26 characters, so lexicographic order is mint order.
// Exposed for tests, which need to encode a chosen instant rather than "now".
std::string encode_capture_public_id(CapturePublicIdSpace space,
                                     std::uint64_t unix_ms,
                                     std::uint64_t entropy_hi16,
                                     std::uint64_t entropy_lo64);

// Mints public ids, monotonically.
//
// MONOTONIC WITHIN A MILLISECOND, deliberately: a rig's members are minted in
// one instant, and fresh entropy per member would order them at random. Minting
// again inside the same millisecond increments the previous entropy instead of
// redrawing it, so members sort in the order they were minted.
//
// Ordering across a BACKWARDS CLOCK STEP is not guaranteed -- the high bits are
// wall clock, which is what makes ids from different sessions comparable at
// all. Sortability is "in mint order under a sane clock", not a hard invariant.
class CapturePublicIdMinter {
public:
  CapturePublicIdMinter();
  // Deterministic entropy, for verifiers that need a repeatable sequence.
  explicit CapturePublicIdMinter(std::uint64_t seed) noexcept;

  std::string mint(CapturePublicIdSpace space);
  // Mint against a supplied instant rather than the wall clock. The monotonic
  // rule applies to this too, which is how the same-millisecond behaviour is
  // testable without racing a real clock.
  std::string mint_at(CapturePublicIdSpace space, std::uint64_t unix_ms);

private:
  void advance_entropy_locked_(std::uint64_t unix_ms);

  std::mt19937_64 rng_;
  std::uint64_t last_ms_ = 0;
  std::uint64_t entropy_hi16_ = 0;
  std::uint64_t entropy_lo64_ = 0;
  bool has_minted_ = false;
};

} // namespace cambang

#pragma once

#include <cstdint>

namespace cambang {

// Choosing a frame rate from what a backend advertises, and saying honestly
// what the caller actually got.
//
// Why this exists
// ---------------
// Every provider faces the same question: the caller asked for a rate, the
// backend offers a fixed set of options, which one serves the request -- and,
// separately, what does choosing it actually promise? Answering that per
// provider would make the same CaptureProfile mean different things per
// platform, which is the failure this seam exists to prevent. It is a pure
// function of the request and the advertised set, so it lives here and is
// exercised host-native, exactly as acquisition_seam_claims.h is and for the
// same reason: the providers that use it are platform-only and unreachable
// from any deterministic verifier.
//
// TWO SHAPES OF BACKEND, ONE QUESTION.
//
//   * RANGE backends accept a [min,max] and let auto-exposure move within it:
//     android_camera2's CONTROL_AE_TARGET_FPS_RANGE, apple_avfoundation's
//     activeVideoMin/MaxFrameDuration, web_getusermedia's frameRate
//     constraint.
//   * DISCRETE backends accept exactly one option chosen from an enumerated
//     list: windows_winrt, where the rate is a property of the
//     MediaFrameFormat you select, and linux_v4l2's frame intervals.
//
// Both are expressed here as candidate [min,max] pairs; a discrete option is
// simply a candidate whose min equals its max. One decision rather than two
// that could drift apart.
//
// WHY THE OUTCOME IS NOT A BOOLEAN. Asking Camera2 for 15 and being handed the
// advertised range [7,30] is not a satisfied request: 15 is inside it, but
// nothing makes AE sit there, and reporting that as success would republish a
// set-point as though it were truth. Exact and Satisfied are therefore distinct
// from each other and from Unserviceable, and the caller is told which it got
// rather than left to assume the request was honoured.
//
// WHAT THIS DOES NOT DO. It does not measure. A selection says what was asked
// of the backend, never what the sensor delivered -- realized rate is observed
// from frame arrival, not inferred from here.

// A rate interval the backend advertises, in whole frames per second.
// A discrete option has min_fps == max_fps.
struct FrameRateRange final {
  uint32_t min_fps = 0;
  uint32_t max_fps = 0;
};

// What the caller asked for, taken from CaptureProfile::target_fps_min/max.
// Either bound may be zero, meaning "unbounded on that side"; both zero means
// the caller expressed no preference at all, which is not the same as asking
// for zero and must not be treated as a request.
struct FrameRateRequest final {
  uint32_t min_fps = 0;
  uint32_t max_fps = 0;

  constexpr bool expressed() const noexcept { return min_fps != 0 || max_fps != 0; }
};

enum class FrameRateSelectionOutcome : uint8_t {
  // The caller asked for nothing. Core selects from what the backend reports,
  // exactly as it selects a pixel format the caller did not name.
  NotRequested = 0,

  // The chosen candidate is a single fixed rate satisfying the request. The
  // backend has nowhere else to go, so this is the only outcome that promises
  // a rate rather than a bound.
  Exact = 1,

  // The chosen candidate lies wholly within the request but spans more than one
  // rate. Every rate the backend may pick honours what was asked; which one it
  // picks is the backend's business.
  Satisfied = 2,

  // The backend reports rate capability and NOTHING it advertises satisfies the
  // request. The effective configuration is invalid, and it is left as the
  // caller stated it: nothing is substituted, exactly as an unavailable width is
  // never substituted. start_stream then fails deterministically (brief 6).
  //
  // This replaced a Clamped outcome that chose the nearest unsatisfying
  // candidate. A frame rate is specified configuration, not a preference -- we
  // would not serve 1024 pixels wide to a caller that asked for 1080 -- and
  // clamping put the provider in the business of inventing a value Core had not
  // materialized.
  Unserviceable = 3,

  // The backend reports NO rate capability at all. Distinct from Unserviceable
  // in the way NOT_THIS_PROVIDER is distinct from CANNOT_ENUMERATE: nothing has
  // been refused, we simply have no basis to select or validate. Core leaves the
  // request untouched and the provider decides at start.
  NotReported = 4,
};

struct FrameRateSelection final {
  FrameRateSelectionOutcome outcome = FrameRateSelectionOutcome::NotRequested;
  // Meaningful only for NotRequested, Exact and Satisfied. Unserviceable and
  // NotReported select nothing, and leave the caller's request to stand.
  FrameRateRange chosen{};
};

constexpr const char* frame_rate_selection_outcome_name(
    FrameRateSelectionOutcome o) noexcept {
  switch (o) {
    case FrameRateSelectionOutcome::NotRequested: return "not_requested";
    case FrameRateSelectionOutcome::Exact:        return "exact";
    case FrameRateSelectionOutcome::Satisfied:    return "satisfied";
    case FrameRateSelectionOutcome::Unserviceable: return "unserviceable";
    case FrameRateSelectionOutcome::NotReported:  return "not_reported";
  }
  return "unknown";
}

namespace detail {

// The request as a concrete interval. An omitted bound is not a bound.
constexpr FrameRateRange normalized_request(const FrameRateRequest& req) noexcept {
  const uint32_t lo = req.min_fps != 0 ? req.min_fps : 1u;
  const uint32_t hi = req.max_fps != 0 ? req.max_fps : UINT32_MAX;
  // A caller that inverted the bounds gets them read the way round it must have
  // meant; the boundary parser already refuses max < min, so this is belt only.
  return lo <= hi ? FrameRateRange{lo, hi} : FrameRateRange{hi, lo};
}

}  // namespace detail

// Pick the advertised candidate that satisfies the request.
//
// A candidate satisfies the request only when it lies WHOLLY INSIDE it, because
// only then does every rate the backend may choose honour what was asked. A
// range merely CONTAINING the request does not: handing Camera2 [7-30] when 15
// was asked for leaves auto-exposure free to sit anywhere in that span.
//
// Among satisfying candidates the highest ceiling wins -- a caller asking for a
// range wants frames, not the slowest option that technically qualifies -- and a
// fixed candidate breaks a tie, being the only kind that promises anything. That
// tie-break is uncontroversial precisely because every option it chooses between
// already honours the request.
//
// There is deliberately no nearest-candidate fallback. When nothing satisfies
// the request the answer is Unserviceable and the caller's request stands
// unaltered; substituting a rate the caller did not ask for is the clamp this
// header used to perform and no longer does.
//
// `candidates` may be null when `count` is zero. Nothing here allocates, throws,
// or touches a backend: brief section 2 forbids I/O in a capability decision,
// and Core consults this during create_stream on its own thread.
inline FrameRateSelection select_frame_rate(const FrameRateRequest& req,
                                            const FrameRateRange* candidates,
                                            size_t count) noexcept {
  FrameRateSelection out{};
  if (candidates == nullptr || count == 0) {
    // No basis to choose or to refuse. Says nothing about the request.
    out.outcome = FrameRateSelectionOutcome::NotReported;
    return out;
  }

  const FrameRateRange want = req.expressed()
      ? detail::normalized_request(req)
      : FrameRateRange{1u, UINT32_MAX};

  const FrameRateRange* best = nullptr;
  for (size_t i = 0; i < count; ++i) {
    const FrameRateRange& c = candidates[i];
    if (c.min_fps == 0 || c.max_fps == 0 || c.min_fps > c.max_fps) {
      continue;  // Not a usable advertisement; ignore rather than reason about it.
    }
    if (c.min_fps < want.min_fps || c.max_fps > want.max_fps) {
      continue;  // Does not lie wholly inside the request.
    }
    if (best == nullptr || c.max_fps > best->max_fps ||
        (c.max_fps == best->max_fps &&
         (c.max_fps - c.min_fps) < (best->max_fps - best->min_fps))) {
      best = &c;
    }
  }

  if (best == nullptr) {
    // An unexpressed request cannot be unserviceable -- it asked for nothing --
    // but a backend advertising only malformed ranges leaves nothing to select.
    out.outcome = req.expressed() ? FrameRateSelectionOutcome::Unserviceable
                                  : FrameRateSelectionOutcome::NotReported;
    return out;
  }

  out.chosen = *best;
  if (!req.expressed()) {
    out.outcome = FrameRateSelectionOutcome::NotRequested;
    return out;
  }
  out.outcome = (best->min_fps == best->max_fps)
                    ? FrameRateSelectionOutcome::Exact
                    : FrameRateSelectionOutcome::Satisfied;
  return out;
}

}  // namespace cambang

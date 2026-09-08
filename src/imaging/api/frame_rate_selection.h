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
// from each other and from Clamped, and a provider is expected to log which one
// it got rather than to assume it asked successfully.
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
  // The caller asked for nothing. The provider keeps whatever default it would
  // have used, and must not synthesise a request on the caller's behalf.
  NotRequested = 0,

  // The chosen candidate is a single fixed rate inside the request. The backend
  // has nowhere else to go, so this is the only outcome that promises a rate.
  Exact = 1,

  // The chosen candidate lies entirely within the request, but spans more than
  // one rate. Every rate the backend may pick honours the request; which one it
  // picks is the backend's business.
  Satisfied = 2,

  // Nothing advertised fits the request. The nearest was chosen, and the caller
  // is getting a rate it did not ask for -- permitted, because clamping is
  // honest so long as the realized rate is reported correctly, but never silent.
  //
  // KNOWN CONFLATION. This covers two cases a caller might want told apart: a
  // candidate that OVERLAPS the request and may yet yield it (asking 2 of a
  // device advertising [1-15]), and one that cannot possibly (asking 120 of a
  // device topping out at 30). `chosen` distinguishes them and the outcome does
  // not. Left as one value deliberately -- a fourth outcome earns its place only
  // when a caller acts on the difference, and none does today.
  Clamped = 3,

  // The backend advertised nothing to choose from. Distinct from NotRequested:
  // the caller asked and could not be served.
  Unavailable = 4,
};

struct FrameRateSelection final {
  FrameRateSelectionOutcome outcome = FrameRateSelectionOutcome::NotRequested;
  // Meaningful only for Exact, Satisfied and Clamped.
  FrameRateRange chosen{};
};

constexpr const char* frame_rate_selection_outcome_name(
    FrameRateSelectionOutcome o) noexcept {
  switch (o) {
    case FrameRateSelectionOutcome::NotRequested: return "not_requested";
    case FrameRateSelectionOutcome::Exact:        return "exact";
    case FrameRateSelectionOutcome::Satisfied:    return "satisfied";
    case FrameRateSelectionOutcome::Clamped:      return "clamped";
    case FrameRateSelectionOutcome::Unavailable:  return "unavailable";
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

// How far a candidate sits outside the request, zero when it overlaps at all.
constexpr uint32_t distance_outside(const FrameRateRange& c,
                                    const FrameRateRange& want) noexcept {
  if (c.max_fps < want.min_fps) return want.min_fps - c.max_fps;
  if (c.min_fps > want.max_fps) return c.min_fps - want.max_fps;
  return 0u;
}

}  // namespace detail

// Pick the advertised candidate that best serves the request.
//
// Preference order, and the reasoning for each:
//   1. A candidate lying WHOLLY INSIDE the request, because only then does every
//      rate the backend may choose honour what was asked. Among those, the
//      highest ceiling wins -- a caller asking for a range wants frames, not the
//      slowest option that technically qualifies -- and a fixed candidate breaks
//      a tie, being the only kind that promises anything.
//   2. Failing that, the nearest candidate, measured as distance outside the
//      requested interval. This is the Clamped case and the caller is owed a
//      log line saying so.
//
// `candidates` may be null when `count` is zero. Nothing here allocates, throws,
// or touches a backend: brief 2 forbids I/O in a capability decision, and this
// is consulted at stream start on the core thread's call path.
inline FrameRateSelection select_frame_rate(const FrameRateRequest& req,
                                            const FrameRateRange* candidates,
                                            size_t count) noexcept {
  FrameRateSelection out{};
  if (!req.expressed()) {
    out.outcome = FrameRateSelectionOutcome::NotRequested;
    return out;
  }
  if (candidates == nullptr || count == 0) {
    out.outcome = FrameRateSelectionOutcome::Unavailable;
    return out;
  }

  const FrameRateRange want = detail::normalized_request(req);

  const FrameRateRange* best_inside = nullptr;
  const FrameRateRange* best_near = nullptr;
  uint32_t best_near_distance = UINT32_MAX;

  for (size_t i = 0; i < count; ++i) {
    const FrameRateRange& c = candidates[i];
    if (c.min_fps == 0 || c.max_fps == 0 || c.min_fps > c.max_fps) {
      continue;  // Not a usable advertisement; ignore rather than reason about it.
    }
    if (c.min_fps >= want.min_fps && c.max_fps <= want.max_fps) {
      if (best_inside == nullptr || c.max_fps > best_inside->max_fps ||
          (c.max_fps == best_inside->max_fps &&
           (c.max_fps - c.min_fps) < (best_inside->max_fps - best_inside->min_fps))) {
        best_inside = &c;
      }
      continue;
    }
    const uint32_t d = detail::distance_outside(c, want);
    // Equal distance is broken by the TIGHTER span, not the higher ceiling.
    // Both matter and they disagree: asking 2 of a device advertising [1-15]
    // and [1-30], the ceiling rule takes [1-30], which is the looser promise
    // and the one less likely to yield 2. The tighter range is nearer the
    // request in every sense that matters to a caller. A higher ceiling still
    // breaks a tie between equally tight candidates, where it means more frames
    // at no cost to the promise.
    const uint32_t span = c.max_fps - c.min_fps;
    const uint32_t best_span =
        best_near != nullptr ? (best_near->max_fps - best_near->min_fps) : 0u;
    if (best_near == nullptr || d < best_near_distance ||
        (d == best_near_distance &&
         (span < best_span || (span == best_span && c.max_fps > best_near->max_fps)))) {
      best_near = &c;
      best_near_distance = d;
    }
  }

  if (best_inside != nullptr) {
    out.chosen = *best_inside;
    out.outcome = (best_inside->min_fps == best_inside->max_fps)
                      ? FrameRateSelectionOutcome::Exact
                      : FrameRateSelectionOutcome::Satisfied;
    return out;
  }
  if (best_near != nullptr) {
    out.chosen = *best_near;
    out.outcome = FrameRateSelectionOutcome::Clamped;
    return out;
  }
  out.outcome = FrameRateSelectionOutcome::Unavailable;
  return out;
}

}  // namespace cambang

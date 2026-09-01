#pragma once

// Deriving the delivered-image calibration from a sensor-domain one.
//
// Every provider whose native calibration is anchored to something other than
// the delivered image has to do exactly this arithmetic to publish
// `intrinsics_delivered`, and the shape of the problem is the same on all of
// them: the delivered image covers some rectangle of the reference frame, and
// is scaled to its own resolution. Camera2 and libcamera report that rectangle
// as a crop; WinRT and AVFoundation report a reference frame the image covers
// whole, which is the same thing with the crop set to the full frame.
//
// It lives here, in the provider-facing API layer rather than in Core, because
// Core does not convert between coordinate domains -- that remains true, and
// nothing in Core calls this. It is a tool offered to provider authors so the
// derivation is written once and tested once, instead of re-derived per backend
// where the two would quietly drift apart.
//
// WHAT ORIGIN TO ATTACH TO THE RESULT
//
// `FactOrigin` records who made an assertion, not who supplied the inputs to
// it. Anything this header returns is therefore CamBANG's assertion and takes
// `CORE_DERIVED`, even where the arithmetic turned out to be an identity: the
// claim that these numbers describe the delivered image, in the delivered-image
// domain, is still ours. `NATIVE_REPORTED` belongs only on a value the platform
// itself stated, in the frame it stated it in.
//
// For a region this cuts both ways, and the distinction is easy to lose:
//   - the platform's crop region, published unchanged, IS a native report;
//   - the same region after `region_cropped_to_aspect()` narrowed it is NOT --
//     the device never named that rectangle;
//   - a whole-frame region invented because the platform reports no region at
//     all (see `derive_delivered_calibration_scaled`) is NOT.
// Compare the returned region against the one you passed in when you need to
// tell the first case from the second.

#include <optional>

#include "core/camera_fact_types.h"

namespace cambang {

// The delivered-image calibration implied by `native` and the region of its
// reference frame that the delivered image covers.
//
//   scale       = delivered_size / region_size
//   focal      *= scale
//   principal   = (principal - region_origin) * scale
//
// The principal point may legitimately fall OUTSIDE the delivered image (a
// negative or beyond-bounds result) when the region is a heavy off-centre crop.
// That is a truthful answer about an off-axis view, not an error, so it is
// returned rather than rejected.
//
// Returns nullopt when the arithmetic cannot be performed at all -- a zero
// dimension anywhere -- rather than substituting a guess.
inline std::optional<Intrinsics> delivered_intrinsics_from_region(
    const Intrinsics& native,
    const DeliveredImageRegion& region,
    uint32_t delivered_width,
    uint32_t delivered_height) {
  if (region.width() == 0 || region.height() == 0 || delivered_width == 0 ||
      delivered_height == 0) {
    return std::nullopt;
  }
  const double sx =
      static_cast<double>(delivered_width) / static_cast<double>(region.width());
  const double sy =
      static_cast<double>(delivered_height) / static_cast<double>(region.height());
  return Intrinsics::create(
      native.focal_length_x_px() * sx,
      native.focal_length_y_px() * sy,
      (native.principal_point_x_px() - static_cast<double>(region.left())) * sx,
      (native.principal_point_y_px() - static_cast<double>(region.top())) * sy,
      native.skew_px() ? std::optional<double>(*native.skew_px() * sx) : std::nullopt,
      delivered_width,
      delivered_height,
      CoordinateDomain{CoordinateDomainDeliveredImage{}});
}

// Narrows a readout region to the aspect ratio of the delivered image.
//
// A platform that reports "the region of the sensor I read out" does not
// necessarily report the region that ends up in YOUR stream: where the readout
// and the output differ in aspect, the device crops further, and both Camera2
// and libcamera document the same rule -- take the largest sub-rectangle of the
// output aspect, CENTRED within the readout region. Camera2 states it as
// "any additional per-stream cropping will be done to maximize the final pixel
// area of the stream ... These additional crops will be centered within the
// crop region"; libcamera describes automatic letter-boxing for the same case.
//
// Two output streams of different aspects therefore see different parts of the
// scene from one readout region, which is why this is per delivered image and
// not per device or per capture.
//
// Returns the region unchanged when the aspects already agree.
inline std::optional<DeliveredImageRegion> region_cropped_to_aspect(
    const DeliveredImageRegion& region,
    uint32_t delivered_width,
    uint32_t delivered_height) {
  if (region.width() == 0 || region.height() == 0 || delivered_width == 0 ||
      delivered_height == 0) {
    return std::nullopt;
  }
  const double region_aspect =
      static_cast<double>(region.width()) / static_cast<double>(region.height());
  const double out_aspect =
      static_cast<double>(delivered_width) / static_cast<double>(delivered_height);

  uint32_t w = region.width();
  uint32_t h = region.height();
  if (region_aspect > out_aspect) {
    // Readout is wider than the output: narrow it (pillarbox).
    w = static_cast<uint32_t>(static_cast<double>(region.height()) * out_aspect + 0.5);
  } else if (region_aspect < out_aspect) {
    // Readout is taller than the output: shorten it (letterbox).
    h = static_cast<uint32_t>(static_cast<double>(region.width()) / out_aspect + 0.5);
  }
  if (w == 0 || h == 0 || w > region.width() || h > region.height()) {
    return std::nullopt;
  }
  return DeliveredImageRegion::create(
      region.left() + (region.width() - w) / 2u,
      region.top() + (region.height() - h) / 2u,
      w, h, region.coordinate_domain());
}
// Convenience for the no-crop case: the delivered image covers the whole
// reference frame and differs only in scale. This is what a provider whose
// calibration is anchored to a format reports, and when the reference frame and
// the delivered geometry are equal it yields the native values unchanged --
// the unity property a caller relies on when moving between the two surfaces.
inline bool derive_delivered_calibration_scaled(
    const Intrinsics& native,
    uint32_t delivered_width,
    uint32_t delivered_height,
    std::optional<Intrinsics>& out_delivered,
    std::optional<DeliveredImageRegion>& out_region) {
  out_delivered.reset();
  out_region.reset();
  const auto region = DeliveredImageRegion::create(
      0u, 0u, native.reference_width_px(), native.reference_height_px(),
      native.coordinate_domain());
  if (!region) {
    return false;
  }
  auto delivered = delivered_intrinsics_from_region(
      native, *region, delivered_width, delivered_height);
  if (!delivered) {
    return false;
  }
  out_region = region;
  out_delivered = std::move(delivered);
  return true;
}

}  // namespace cambang

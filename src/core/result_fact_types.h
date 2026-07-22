#pragma once

#include <cstdint>

namespace cambang {

enum class ResultFactProvenance : uint8_t {
  HARDWARE_REPORTED = 0,
  PROVIDER_DERIVED = 1,
  RUNTIME_INJECTED = 2,
  USER_DEFAULT = 3,
  USER_OVERRIDE = 4,
  UNKNOWN = 5,
};

struct ResultImagePropertiesFacts {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  int32_t orientation = 0;
  uint32_t bit_depth = 0;
};

struct ResultImagePropertiesProvenance {
  ResultFactProvenance width = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance height = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance format = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance orientation = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance bit_depth = ResultFactProvenance::UNKNOWN;
};

// Optical-calibration truth lives in the resolved per-member camera facts
// (CoreResolvedCaptureImageFacts / get_image_member camera_facts); location
// truth lives in the capture-admission context (get_geolocation()). The former
// flattened optical-calibration, location, and capture-attributes fact groups
// were writer-less duplicates and were removed.
//
// The five quantities the retired capture-attributes group covered — focus
// distance, exposure time, ISO, aperture, and physical focal length — now live
// in the camera-fact model proper (camera_facts.focus_state, .exposure_time,
// .sensor_sensitivity_iso, .aperture_f_number, .focal_length_mm), each with a
// device-constant tier and a per-capture tier. Note focal_length_mm is lens
// metadata, distinct from the calibrated intrinsics.focal_length_x_px.

} // namespace cambang

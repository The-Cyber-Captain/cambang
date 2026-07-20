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

struct ResultCaptureAttributesFacts {
  double exposure_time_ns = 0.0;
  double aperture_f_number = 0.0;
  double focal_length_mm = 0.0;
  double focus_distance_m = 0.0;
  double sensor_sensitivity_iso_equivalent = 0.0;
};

struct ResultCaptureAttributesProvenance {
  ResultFactProvenance exposure_time_ns = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance aperture_f_number = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance focal_length_mm = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance focus_distance_m = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance sensor_sensitivity_iso_equivalent = ResultFactProvenance::UNKNOWN;
};

// Optical-calibration truth lives in the resolved per-member camera facts
// (CoreResolvedCaptureImageFacts / get_image_member camera_facts); location
// truth lives in the capture-admission context (get_geolocation()). The
// former flattened optical-calibration and location fact groups were
// writer-less duplicates and were removed.

} // namespace cambang

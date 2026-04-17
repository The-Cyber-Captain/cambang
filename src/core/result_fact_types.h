#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

struct ResultLocationAttributesFacts {
  double latitude = 0.0;
  double longitude = 0.0;
  double altitude_m = 0.0;
};

struct ResultLocationAttributesProvenance {
  ResultFactProvenance latitude = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance longitude = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance altitude_m = ResultFactProvenance::UNKNOWN;
};

struct ResultOpticalCalibrationFacts {
  double principal_point_x = 0.0;
  double principal_point_y = 0.0;
  double focal_length_x = 0.0;
  double focal_length_y = 0.0;
  std::string distortion_model;
  std::vector<double> distortion_coefficients;
};

struct ResultOpticalCalibrationProvenance {
  ResultFactProvenance principal_point_x = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance principal_point_y = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance focal_length_x = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance focal_length_y = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance distortion_model = ResultFactProvenance::UNKNOWN;
  ResultFactProvenance distortion_coefficients = ResultFactProvenance::UNKNOWN;
};

} // namespace cambang

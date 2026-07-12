#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

namespace cambang {

class CaptureDateTimeUtc final {
public:
  static CaptureDateTimeUtc from_unix_epoch_nanoseconds(int64_t value) noexcept {
    return CaptureDateTimeUtc(value);
  }

  int64_t unix_epoch_nanoseconds() const noexcept { return unix_epoch_nanoseconds_; }

private:
  explicit CaptureDateTimeUtc(int64_t value) noexcept : unix_epoch_nanoseconds_(value) {}
  int64_t unix_epoch_nanoseconds_ = 0;
};

class CaptureGeolocation final {
public:
  static std::optional<CaptureGeolocation> create(
      double latitude_degrees,
      double longitude_degrees,
      std::optional<double> altitude_meters = std::nullopt) noexcept {
    if (!std::isfinite(latitude_degrees) || !std::isfinite(longitude_degrees) ||
        (altitude_meters && !std::isfinite(*altitude_meters))) {
      return std::nullopt;
    }
    return CaptureGeolocation(latitude_degrees, longitude_degrees, altitude_meters);
  }

  double latitude_degrees() const noexcept { return latitude_degrees_; }
  double longitude_degrees() const noexcept { return longitude_degrees_; }
  const std::optional<double>& altitude_meters() const noexcept { return altitude_meters_; }

private:
  CaptureGeolocation(double latitude_degrees, double longitude_degrees,
                     std::optional<double> altitude_meters) noexcept
      : latitude_degrees_(latitude_degrees), longitude_degrees_(longitude_degrees),
        altitude_meters_(altitude_meters) {}

  double latitude_degrees_ = 0.0;
  double longitude_degrees_ = 0.0;
  std::optional<double> altitude_meters_{};
};

struct CaptureAdmissionContext {
  CaptureDateTimeUtc capture_date_time = CaptureDateTimeUtc::from_unix_epoch_nanoseconds(0);
  std::optional<CaptureGeolocation> geolocation{};
};

} // namespace cambang

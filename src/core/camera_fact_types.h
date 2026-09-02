#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <vector>
#include <string>
#include <utility>
#include <variant>

namespace cambang {

// Origin identifies who supplied or authored a present fact. It never implies
// precedence, trust, or effective resolution authority.
enum class FactOrigin : uint8_t {
  NATIVE_REPORTED = 0,
  USER_SUPPLIED = 1,
  // A non-Core source, such as a provider or external document, derived it.
  DERIVED = 2,
  VIRTUAL_CAMERA_AUTHORED = 3,
  // A CamBANG input seam supplied runtime capture context.
  RUNTIME_INJECTED = 4,
  // CamBANG Core itself derived it.
  CORE_DERIVED = 5,
  UNKNOWN = 6,
};

template <typename T>
struct SourcedFact {
  T value;
  FactOrigin origin;
};

enum class CameraFacing : uint8_t {
  FRONT = 0,
  BACK = 1,
  EXTERNAL = 2,
  UNKNOWN = 3,
};

enum class CameraNature : uint8_t {
  PHYSICAL = 0,
  VIRTUAL = 1,
  HYBRID = 2,
  UNKNOWN = 3,
};

enum class SensorOrientationDegrees : uint16_t {
  DEGREES_0 = 0,
  DEGREES_90 = 90,
  DEGREES_180 = 180,
  DEGREES_270 = 270,
};

struct CoordinateDomainAndroidSensorPreCorrectionActiveArray {};
struct CoordinateDomainAndroidSensorActiveArray {};
struct CoordinateDomainDeliveredImage {};

class CoordinateDomainPlatformDefined {
 public:
  static std::optional<CoordinateDomainPlatformDefined> create(std::string token) {
    if (token.empty()) {
      return std::nullopt;
    }
    return CoordinateDomainPlatformDefined(std::move(token));
  }

  const std::string& token() const noexcept { return token_; }

 private:
  explicit CoordinateDomainPlatformDefined(std::string token) : token_(std::move(token)) {}

  std::string token_;
};

using CoordinateDomain = std::variant<
    CoordinateDomainAndroidSensorPreCorrectionActiveArray,
    CoordinateDomainAndroidSensorActiveArray,
    CoordinateDomainDeliveredImage,
    CoordinateDomainPlatformDefined>;

// Where a delivered image sits inside the reference domain that the
// sensor-domain calibration is expressed in.
//
// This is what makes sensor-domain intrinsics usable against delivered pixels,
// and what makes the delivered-image intrinsics auditable rather than opaque.
// A delivered image is generally a CROP of the sensor reference frame, scaled
// to the output resolution, so the reference frame's width is not the delivered
// image's width and the naive per-axis rescale of a focal length is wrong
// whenever the two aspects differ.
//
// Semantics, and the whole of them: the delivered image, at its full width and
// height, covers exactly this rectangle of `coordinate_domain`.
//
//   reference_x = left + delivered_x * (width  / delivered_width)
//   reference_y = top  + delivered_y * (height / delivered_height)
//
// A rectangle rather than a scale/offset pair because that is the shape the
// platforms report, it validates against the reference array bounds, and naming
// the fields avoids the (left, top, width, height) versus
// (left, top, right, bottom) ambiguity that Android's own documentation uses
// both of.
//
// It maps position only. It says nothing about rotation or mirroring -- that is
// RealizedImageTransform -- and nothing about lens distortion.
class DeliveredImageRegion {
 public:
  static std::optional<DeliveredImageRegion> create(
      uint32_t left,
      uint32_t top,
      uint32_t width,
      uint32_t height,
      CoordinateDomain coordinate_domain) {
    if (width == 0 || height == 0) {
      return std::nullopt;
    }
    return DeliveredImageRegion(left, top, width, height,
                                std::move(coordinate_domain));
  }

  uint32_t left() const noexcept { return left_; }
  uint32_t top() const noexcept { return top_; }
  uint32_t width() const noexcept { return width_; }
  uint32_t height() const noexcept { return height_; }
  const CoordinateDomain& coordinate_domain() const noexcept {
    return coordinate_domain_;
  }

 private:
  DeliveredImageRegion(uint32_t left,
                       uint32_t top,
                       uint32_t width,
                       uint32_t height,
                       CoordinateDomain coordinate_domain)
      : left_(left),
        top_(top),
        width_(width),
        height_(height),
        coordinate_domain_(std::move(coordinate_domain)) {}

  uint32_t left_ = 0;
  uint32_t top_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  CoordinateDomain coordinate_domain_{};
};
class Intrinsics {
 public:
  static std::optional<Intrinsics> create(
      double focal_length_x_px,
      double focal_length_y_px,
      double principal_point_x_px,
      double principal_point_y_px,
      std::optional<double> skew_px,
      uint32_t reference_width_px,
      uint32_t reference_height_px,
      CoordinateDomain coordinate_domain) {
    if (reference_width_px == 0 || reference_height_px == 0 ||
        !std::isfinite(focal_length_x_px) || !std::isfinite(focal_length_y_px) ||
        !std::isfinite(principal_point_x_px) || !std::isfinite(principal_point_y_px) ||
        (skew_px && !std::isfinite(*skew_px))) {
      return std::nullopt;
    }
    return Intrinsics(
        focal_length_x_px, focal_length_y_px, principal_point_x_px,
        principal_point_y_px, skew_px, reference_width_px, reference_height_px,
        std::move(coordinate_domain));
  }

  double focal_length_x_px() const noexcept { return focal_length_x_px_; }
  double focal_length_y_px() const noexcept { return focal_length_y_px_; }
  double principal_point_x_px() const noexcept { return principal_point_x_px_; }
  double principal_point_y_px() const noexcept { return principal_point_y_px_; }
  const std::optional<double>& skew_px() const noexcept { return skew_px_; }
  uint32_t reference_width_px() const noexcept { return reference_width_px_; }
  uint32_t reference_height_px() const noexcept { return reference_height_px_; }
  const CoordinateDomain& coordinate_domain() const noexcept { return coordinate_domain_; }

 private:
  Intrinsics(
      double focal_length_x_px,
      double focal_length_y_px,
      double principal_point_x_px,
      double principal_point_y_px,
      std::optional<double> skew_px,
      uint32_t reference_width_px,
      uint32_t reference_height_px,
      CoordinateDomain coordinate_domain)
      : focal_length_x_px_(focal_length_x_px),
        focal_length_y_px_(focal_length_y_px),
        principal_point_x_px_(principal_point_x_px),
        principal_point_y_px_(principal_point_y_px),
        skew_px_(skew_px),
        reference_width_px_(reference_width_px),
        reference_height_px_(reference_height_px),
        coordinate_domain_(std::move(coordinate_domain)) {}

  double focal_length_x_px_;
  double focal_length_y_px_;
  double principal_point_x_px_;
  double principal_point_y_px_;
  std::optional<double> skew_px_;
  uint32_t reference_width_px_;
  uint32_t reference_height_px_;
  CoordinateDomain coordinate_domain_;
};

enum class DistortionImageState : uint8_t {
  DISTORTED = 0,
  RECTIFIED = 1,
  UNKNOWN = 2,
};

class BrownConrady5Distortion {
 public:
  static std::optional<BrownConrady5Distortion> create(
      double radial_k1,
      double radial_k2,
      double radial_k3,
      double tangential_p1,
      double tangential_p2,
      uint32_t reference_width_px,
      uint32_t reference_height_px,
      CoordinateDomain coordinate_domain,
      DistortionImageState image_state) {
    if (reference_width_px == 0 || reference_height_px == 0 ||
        !std::isfinite(radial_k1) || !std::isfinite(radial_k2) ||
        !std::isfinite(radial_k3) || !std::isfinite(tangential_p1) ||
        !std::isfinite(tangential_p2)) {
      return std::nullopt;
    }
    return BrownConrady5Distortion(
        radial_k1, radial_k2, radial_k3, tangential_p1, tangential_p2,
        reference_width_px, reference_height_px, std::move(coordinate_domain), image_state);
  }

  double radial_k1() const noexcept { return radial_k1_; }
  double radial_k2() const noexcept { return radial_k2_; }
  double radial_k3() const noexcept { return radial_k3_; }
  double tangential_p1() const noexcept { return tangential_p1_; }
  double tangential_p2() const noexcept { return tangential_p2_; }
  uint32_t reference_width_px() const noexcept { return reference_width_px_; }
  uint32_t reference_height_px() const noexcept { return reference_height_px_; }
  const CoordinateDomain& coordinate_domain() const noexcept { return coordinate_domain_; }
  DistortionImageState image_state() const noexcept { return image_state_; }

 private:
  BrownConrady5Distortion(
      double radial_k1,
      double radial_k2,
      double radial_k3,
      double tangential_p1,
      double tangential_p2,
      uint32_t reference_width_px,
      uint32_t reference_height_px,
      CoordinateDomain coordinate_domain,
      DistortionImageState image_state)
      : radial_k1_(radial_k1),
        radial_k2_(radial_k2),
        radial_k3_(radial_k3),
        tangential_p1_(tangential_p1),
        tangential_p2_(tangential_p2),
        reference_width_px_(reference_width_px),
        reference_height_px_(reference_height_px),
        coordinate_domain_(std::move(coordinate_domain)),
        image_state_(image_state) {}

  double radial_k1_;
  double radial_k2_;
  double radial_k3_;
  double tangential_p1_;
  double tangential_p2_;
  uint32_t reference_width_px_;
  uint32_t reference_height_px_;
  CoordinateDomain coordinate_domain_;
  DistortionImageState image_state_;
};

struct NoDistortion {
  DistortionImageState image_state;
};

using Distortion = std::variant<BrownConrady5Distortion, NoDistortion>;

struct Vec3Meters {
  double x;
  double y;
  double z;
};

struct QuaternionXyzw {
  double x;
  double y;
  double z;
  double w;
};

class PoseReferenceCamera {
 public:
  static std::optional<PoseReferenceCamera> create(std::string camera_id) {
    if (camera_id.empty()) {
      return std::nullopt;
    }
    return PoseReferenceCamera(std::move(camera_id));
  }

  const std::string& camera_id() const noexcept { return camera_id_; }

 private:
  explicit PoseReferenceCamera(std::string camera_id) : camera_id_(std::move(camera_id)) {}

  std::string camera_id_;
};

struct PoseReferencePrimaryCamera {};
struct PoseReferenceDeviceMotionSensor {};
struct PoseReferenceAutomotive {};

class PoseReferenceCustom {
 public:
  static std::optional<PoseReferenceCustom> create(std::string reference_id) {
    if (reference_id.empty()) {
      return std::nullopt;
    }
    return PoseReferenceCustom(std::move(reference_id));
  }

  const std::string& reference_id() const noexcept { return reference_id_; }

 private:
  explicit PoseReferenceCustom(std::string reference_id) : reference_id_(std::move(reference_id)) {}

  std::string reference_id_;
};

class PoseReferencePlatformDefined {
 public:
  static std::optional<PoseReferencePlatformDefined> create(std::string reference_token) {
    if (reference_token.empty()) {
      return std::nullopt;
    }
    return PoseReferencePlatformDefined(std::move(reference_token));
  }

  const std::string& reference_token() const noexcept { return reference_token_; }

 private:
  explicit PoseReferencePlatformDefined(std::string reference_token)
      : reference_token_(std::move(reference_token)) {}

  std::string reference_token_;
};

struct PoseReferenceUnknown {};

using PoseReference = std::variant<
    PoseReferenceCamera,
    PoseReferencePrimaryCamera,
    PoseReferenceDeviceMotionSensor,
    PoseReferenceAutomotive,
    PoseReferenceCustom,
    PoseReferencePlatformDefined,
    PoseReferenceUnknown>;

struct PoseConventionAndroidCamera2 {};
struct PoseConventionCameraOpticalFrame {};

class PoseConventionPlatformDefined {
 public:
  static std::optional<PoseConventionPlatformDefined> create(std::string convention_token) {
    if (convention_token.empty()) {
      return std::nullopt;
    }
    return PoseConventionPlatformDefined(std::move(convention_token));
  }

  const std::string& convention_token() const noexcept { return convention_token_; }

 private:
  explicit PoseConventionPlatformDefined(std::string convention_token)
      : convention_token_(std::move(convention_token)) {}

  std::string convention_token_;
};

using PoseConvention = std::variant<
    PoseConventionAndroidCamera2,
    PoseConventionCameraOpticalFrame,
    PoseConventionPlatformDefined>;

class CameraPose {
 public:
  static std::optional<CameraPose> create(
      PoseReference reference,
      PoseConvention convention,
      Vec3Meters translation_m,
      QuaternionXyzw rotation_xyzw) {
    if (!is_finite(translation_m) || !is_finite(rotation_xyzw) ||
        (rotation_xyzw.x == 0.0 && rotation_xyzw.y == 0.0 &&
         rotation_xyzw.z == 0.0 && rotation_xyzw.w == 0.0)) {
      return std::nullopt;
    }
    return CameraPose(
        std::move(reference), std::move(convention), translation_m, rotation_xyzw);
  }

  const PoseReference& reference() const noexcept { return reference_; }
  const PoseConvention& convention() const noexcept { return convention_; }
  const Vec3Meters& translation_m() const noexcept { return translation_m_; }
  const QuaternionXyzw& rotation_xyzw() const noexcept { return rotation_xyzw_; }

 private:
  CameraPose(
      PoseReference reference,
      PoseConvention convention,
      Vec3Meters translation_m,
      QuaternionXyzw rotation_xyzw)
      : reference_(std::move(reference)),
        convention_(std::move(convention)),
        translation_m_(translation_m),
        rotation_xyzw_(rotation_xyzw) {}

  static bool is_finite(const Vec3Meters& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
  }

  static bool is_finite(const QuaternionXyzw& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
  }

  PoseReference reference_;
  PoseConvention convention_;
  Vec3Meters translation_m_;
  QuaternionXyzw rotation_xyzw_;
};

enum class AltitudeReference : uint8_t {
  ELLIPSOID = 0,
  MEAN_SEA_LEVEL = 1,
  UNKNOWN = 2,
};

struct AbsoluteUtcDateTime {
  int64_t unix_ms;
};

class GeodeticAltitude {
 public:
  static std::optional<GeodeticAltitude> create(double value_m, AltitudeReference reference) {
    if (!std::isfinite(value_m)) {
      return std::nullopt;
    }
    return GeodeticAltitude(value_m, reference);
  }

  double value_m() const noexcept { return value_m_; }
  AltitudeReference reference() const noexcept { return reference_; }

 private:
  GeodeticAltitude(double value_m, AltitudeReference reference)
      : value_m_(value_m), reference_(reference) {}

  double value_m_;
  AltitudeReference reference_;
};

class Geolocation {
 public:
  static std::optional<Geolocation> create(
      double latitude_degrees,
      double longitude_degrees,
      std::optional<GeodeticAltitude> altitude,
      std::optional<double> horizontal_accuracy_m,
      std::optional<double> vertical_accuracy_m,
      std::optional<AbsoluteUtcDateTime> sample_datetime_utc) {
    if (!std::isfinite(latitude_degrees) || !std::isfinite(longitude_degrees) ||
        (horizontal_accuracy_m && !std::isfinite(*horizontal_accuracy_m)) ||
        (vertical_accuracy_m && !std::isfinite(*vertical_accuracy_m))) {
      return std::nullopt;
    }
    return Geolocation(
        latitude_degrees, longitude_degrees, std::move(altitude), horizontal_accuracy_m,
        vertical_accuracy_m, sample_datetime_utc);
  }

  double latitude_degrees() const noexcept { return latitude_degrees_; }
  double longitude_degrees() const noexcept { return longitude_degrees_; }
  const std::optional<GeodeticAltitude>& altitude() const noexcept { return altitude_; }
  const std::optional<double>& horizontal_accuracy_m() const noexcept {
    return horizontal_accuracy_m_;
  }
  const std::optional<double>& vertical_accuracy_m() const noexcept {
    return vertical_accuracy_m_;
  }
  const std::optional<AbsoluteUtcDateTime>& sample_datetime_utc() const noexcept {
    return sample_datetime_utc_;
  }

 private:
  Geolocation(
      double latitude_degrees,
      double longitude_degrees,
      std::optional<GeodeticAltitude> altitude,
      std::optional<double> horizontal_accuracy_m,
      std::optional<double> vertical_accuracy_m,
      std::optional<AbsoluteUtcDateTime> sample_datetime_utc)
      : latitude_degrees_(latitude_degrees),
        longitude_degrees_(longitude_degrees),
        altitude_(std::move(altitude)),
        horizontal_accuracy_m_(horizontal_accuracy_m),
        vertical_accuracy_m_(vertical_accuracy_m),
        sample_datetime_utc_(sample_datetime_utc) {}

  double latitude_degrees_;
  double longitude_degrees_;
  std::optional<GeodeticAltitude> altitude_;
  std::optional<double> horizontal_accuracy_m_;
  std::optional<double> vertical_accuracy_m_;
  std::optional<AbsoluteUtcDateTime> sample_datetime_utc_;
};

enum class CaptureDateTimeReferenceEvent : uint8_t {
  CAPTURE_ADMISSION = 0,
};

struct CaptureDateTime {
  AbsoluteUtcDateTime utc;
  CaptureDateTimeReferenceEvent reference_event;
};

enum class ImageAcquisitionClockDomain : uint8_t {
  PROVIDER_MONOTONIC = 0,
  CORE_MONOTONIC = 1,
  DOMAIN_OPAQUE = 2,
};

enum class ImageAcquisitionReferenceEvent : uint8_t {
  EXPOSURE_START = 0,
  EXPOSURE_MIDPOINT = 1,
  SENSOR_READOUT_START = 2,
  FRAME_AVAILABLE = 3,
  PROVIDER_OBSERVED = 4,
  UNKNOWN = 5,
};

enum class ImageAcquisitionComparability : uint8_t {
  SAME_IMAGE_ONLY = 0,
  SAME_DEVICE = 1,
  SAME_PROVIDER = 2,
  CROSS_DEVICE_SYNCHRONIZED = 3,
  CORE_TIMELINE = 4,
  ORDERING_ONLY = 5,
};

class TickPeriod {
 public:
  static std::optional<TickPeriod> create(int64_t numerator_ns, int64_t denominator) {
    if (numerator_ns <= 0 || denominator <= 0) {
      return std::nullopt;
    }
    const int64_t divisor = std::gcd(numerator_ns, denominator);
    return TickPeriod(numerator_ns / divisor, denominator / divisor);
  }

  int64_t numerator_ns() const noexcept { return numerator_ns_; }
  int64_t denominator() const noexcept { return denominator_; }

 private:
  TickPeriod(int64_t numerator_ns, int64_t denominator)
      : numerator_ns_(numerator_ns), denominator_(denominator) {}

  int64_t numerator_ns_;
  int64_t denominator_;
};

class ImageAcquisitionTiming {
 public:
  static std::optional<ImageAcquisitionTiming> create(
      int64_t acquisition_mark,
      TickPeriod tick_period,
      ImageAcquisitionClockDomain clock_domain,
      ImageAcquisitionReferenceEvent reference_event,
      ImageAcquisitionComparability comparability) {
    if (acquisition_mark < 0 ||
        clock_domain > ImageAcquisitionClockDomain::DOMAIN_OPAQUE ||
        reference_event > ImageAcquisitionReferenceEvent::UNKNOWN ||
        comparability > ImageAcquisitionComparability::ORDERING_ONLY) {
      return std::nullopt;
    }
    return ImageAcquisitionTiming(
        acquisition_mark,
        std::move(tick_period),
        clock_domain,
        reference_event,
        comparability);
  }

  static std::optional<int64_t> checked_mark_from_unsigned(uint64_t acquisition_mark) noexcept {
    if (acquisition_mark > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<int64_t>(acquisition_mark);
  }

  int64_t acquisition_mark() const noexcept { return acquisition_mark_; }
  const TickPeriod& tick_period() const noexcept { return tick_period_; }
  ImageAcquisitionClockDomain clock_domain() const noexcept { return clock_domain_; }
  ImageAcquisitionReferenceEvent reference_event() const noexcept { return reference_event_; }
  ImageAcquisitionComparability comparability() const noexcept { return comparability_; }

 private:
  ImageAcquisitionTiming(
      int64_t acquisition_mark,
      TickPeriod tick_period,
      ImageAcquisitionClockDomain clock_domain,
      ImageAcquisitionReferenceEvent reference_event,
      ImageAcquisitionComparability comparability)
      : acquisition_mark_(acquisition_mark),
        tick_period_(std::move(tick_period)),
        clock_domain_(clock_domain),
        reference_event_(reference_event),
        comparability_(comparability) {}

  int64_t acquisition_mark_;
  TickPeriod tick_period_;
  ImageAcquisitionClockDomain clock_domain_;
  ImageAcquisitionReferenceEvent reference_event_;
  ImageAcquisitionComparability comparability_;
};

class FocusAtDistance {
 public:
  static std::optional<FocusAtDistance> create(double distance_m) {
    if (!std::isfinite(distance_m)) {
      return std::nullopt;
    }
    return FocusAtDistance(distance_m);
  }

  double distance_m() const noexcept { return distance_m_; }

 private:
  explicit FocusAtDistance(double distance_m) : distance_m_(distance_m) {}

  double distance_m_;
};

struct FocusAtInfinity {};
struct FocusStateUnknown {};

using FocusState = std::variant<FocusAtDistance, FocusAtInfinity, FocusStateUnknown>;

// Realized optical/exposure quantities. Each is device-constant on some
// hardware (fixed iris, prime lens, locked auto-exposure) and genuinely
// per-capture on other hardware (variable aperture, zoom lens, hunting
// auto-exposure), so each carries both a CameraStaticFacts tier and a
// per-capture tier, resolved external > provider-per-image > provider-static.
class ExposureTime {
 public:
  static std::optional<ExposureTime> create(double nanoseconds) {
    if (!std::isfinite(nanoseconds) || nanoseconds <= 0.0) {
      return std::nullopt;
    }
    return ExposureTime(nanoseconds);
  }

  double nanoseconds() const noexcept { return nanoseconds_; }

 private:
  explicit ExposureTime(double nanoseconds) : nanoseconds_(nanoseconds) {}

  double nanoseconds_;
};

class SensorSensitivityIso {
 public:
  static std::optional<SensorSensitivityIso> create(double iso_equivalent) {
    if (!std::isfinite(iso_equivalent) || iso_equivalent <= 0.0) {
      return std::nullopt;
    }
    return SensorSensitivityIso(iso_equivalent);
  }

  double iso_equivalent() const noexcept { return iso_equivalent_; }

 private:
  explicit SensorSensitivityIso(double iso_equivalent) : iso_equivalent_(iso_equivalent) {}

  double iso_equivalent_;
};

class ApertureFNumber {
 public:
  static std::optional<ApertureFNumber> create(double f_number) {
    if (!std::isfinite(f_number) || f_number <= 0.0) {
      return std::nullopt;
    }
    return ApertureFNumber(f_number);
  }

  double f_number() const noexcept { return f_number_; }

 private:
  explicit ApertureFNumber(double f_number) : f_number_(f_number) {}

  double f_number_;
};

// Physical lens focal length. This is lens metadata (what EXIF carries), a
// distinct quantity from Intrinsics::focal_length_x_px() — the two are related
// only through sensor pixel pitch, which this model does not carry, and a
// camera may report either without the other.
class FocalLengthMm {
 public:
  static std::optional<FocalLengthMm> create(double millimetres) {
    if (!std::isfinite(millimetres) || millimetres <= 0.0) {
      return std::nullopt;
    }
    return FocalLengthMm(millimetres);
  }

  double millimetres() const noexcept { return millimetres_; }

 private:
  explicit FocalLengthMm(double millimetres) : millimetres_(millimetres) {}

  double millimetres_;
};

enum class ImageRotationDegrees : uint16_t {
  DEGREES_0 = 0,
  DEGREES_90 = 90,
  DEGREES_180 = 180,
  DEGREES_270 = 270,
};

struct RealizedImageTransform {
  ImageRotationDegrees rotation;
  bool mirrored;
  // True when the delivered pixels already incorporate this transform.
  bool pixels_already_transformed;
};

// These containers deliberately separate static camera description, Core
// admission context, and image-time facts. No provider or interchange shape is
// implied by this source-neutral Core model.
struct CameraStaticFacts {
  std::optional<SourcedFact<CameraFacing>> facing;
  std::optional<SourcedFact<CameraNature>> nature;
  std::optional<SourcedFact<SensorOrientationDegrees>> sensor_orientation;
  std::optional<SourcedFact<CameraPose>> pose;
  // Device-constant assertions for otherwise per-capture quantities. A camera
  // whose hardware genuinely fixes these (fixed-focus lens, fixed iris, prime
  // lens, locked exposure) can have them supplied here — typically via external
  // camera-description ingestion, since such hardware usually exposes no API to
  // read them at all.
  std::optional<SourcedFact<FocusState>> focus_state;
  std::optional<SourcedFact<ExposureTime>> exposure_time;
  std::optional<SourcedFact<SensorSensitivityIso>> sensor_sensitivity_iso;
  std::optional<SourcedFact<ApertureFNumber>> aperture_f_number;
  std::optional<SourcedFact<FocalLengthMm>> focal_length_mm;
  // Intrinsics and distortion sit here, with the assertions, rather than with
  // the device-scoped facts above. Both platform providers source them per
  // image and anchored to a format, so there is no such thing as *the*
  // intrinsics of a device: a device-scoped value carries no format and cannot
  // be applied to a frame. What a device-keyed source can honestly say is that
  // this camera holds them constant -- an authored virtual camera, or an
  // ingested description standing in for hardware that exposes no API to read
  // them. That is an assertion, and it resolves into CaptureImageFacts, never
  // into a device-scoped fact.
  std::optional<SourcedFact<Intrinsics>> intrinsics;
  std::optional<SourcedFact<Distortion>> distortion;
  // Overridable independently of each other and of the sensor-domain pair: a
  // camera that misreports its principal point does so in whichever frame it
  // reports, and a description correcting one must not silently correct or
  // contradict the other.
  std::optional<SourcedFact<Intrinsics>> intrinsics_delivered;
  std::optional<SourcedFact<DeliveredImageRegion>> delivered_image_region;
};

// The RESOLVED device-scoped facts: what CamBANG concluded about the camera
// itself, after applying source precedence. Deliberately a distinct type from
// CameraStaticFacts, which is the device-keyed SOURCE carrier and also holds
// device-constant assertions of per-image quantities (intrinsics, distortion,
// exposure, focus). Those resolve into CaptureImageFacts and are never
// device-scoped facts.
//
// One type served both roles until 2026-08-29. A reader could then ask a
// resolved device record for intrinsics, compile clean, and receive nullopt
// forever -- which is exactly what happened to provider_compliance_verify. The
// split makes that a compile error instead.
struct ResolvedCameraDeviceFacts {
  std::optional<SourcedFact<CameraFacing>> facing;
  std::optional<SourcedFact<CameraNature>> nature;
  std::optional<SourcedFact<SensorOrientationDegrees>> sensor_orientation;
  std::optional<SourcedFact<CameraPose>> pose;
};

struct CaptureAdmissionFacts {
  std::optional<SourcedFact<Geolocation>> geolocation;
  std::optional<SourcedFact<CaptureDateTime>> capture_datetime;
};

struct CaptureImageFacts {
  std::optional<SourcedFact<ImageAcquisitionTiming>> acquisition_timing;
  // Image-scoped: the calibration that applies to THIS image, in the
  // coordinate domain and reference frame it was measured in.
  std::optional<SourcedFact<Intrinsics>> intrinsics;
  std::optional<SourcedFact<Distortion>> distortion;

  // The calibration expressed in DELIVERED-IMAGE pixels: what a caller needs to
  // build a projection or a frustum for the image actually handed to them.
  //
  // Independent of `intrinsics` above rather than derived from it here, because
  // providers differ in which one they can measure: a backend whose calibration
  // is format-anchored reports this one natively and has no sensor-domain
  // answer, while a sensor-anchored backend reports the other and derives this.
  // Requiring either direction would shape the contract around one platform.
  //
  // Where the sensor-domain calibration is ALREADY in delivered-image
  // coordinates, the two are the same values and must be reported as equal.
  std::optional<SourcedFact<Intrinsics>> intrinsics_delivered;

  // Where this delivered image sits inside the sensor reference frame. The
  // derivation linking the two intrinsics above, and the route back to sensor
  // space for a caller that needs it.
  std::optional<SourcedFact<DeliveredImageRegion>> delivered_image_region;
  std::optional<SourcedFact<FocusState>> focus_state;
  std::optional<SourcedFact<ExposureTime>> exposure_time;
  std::optional<SourcedFact<SensorSensitivityIso>> sensor_sensitivity_iso;
  std::optional<SourcedFact<ApertureFNumber>> aperture_f_number;
  std::optional<SourcedFact<FocalLengthMm>> focal_length_mm;
  std::optional<SourcedFact<RealizedImageTransform>> realized_image_transform;
};

// Provider ingress names the authority without changing the source-neutral
// record shapes. These are retained independently of external configuration.
struct ProviderCameraFacts {
  CameraStaticFacts static_facts;
};

// One configuration a device will accept, in the exact shape a caller hands
// back to create_stream()/set_still_capture_profile(). Geometry and format
// only: bracket capability is provider policy, not a device configuration.
//
// width/height/format_fourcc IDENTIFY the entry. max_fps is informational and
// deliberately not part of that identity -- it is derived (from a minimum
// frame duration on at least one backend), so it can move with a driver update
// while the geometry is unchanged. A caller matching a remembered choice
// matches on the three integers.
struct ProviderProfileEntry {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format_fourcc = 0;
  // Absent when the provider cannot derive a frame rate for this entry.
  std::optional<double> max_fps;
};

// A provider has three distinct things to say about an endpoint, and collapsing
// any two of them loses information a caller acts on.
//
// The distinction that matters most is NOT_THIS_PROVIDER vs CANNOT_ENUMERATE.
// An ingested description may supply a catalog for a camera the provider owns
// but cannot enumerate -- a backend needing the camera open to list its formats,
// for instance. It must NOT supply one for an endpoint that does not exist, or
// the surface reports configurations for absent hardware. With a single boolean
// those two cases were indistinguishable, and the second happened.
enum class ProfileCatalogAvailability : uint8_t {
  // Default. Not an endpoint this provider owns; no description may stand in for
  // it. Also what a provider that has not implemented enumeration reports, which
  // is the conservative direction: no catalog, rather than a catalog for
  // something that may not be there.
  NOT_THIS_PROVIDER = 0,
  // The provider owns this endpoint but cannot list its configurations. A
  // description may supply them.
  CANNOT_ENUMERATE,
  // The provider owns this endpoint and `entries` is what it advertises. An
  // empty list here means it offers nothing, which is a real answer.
  ENUMERATED,
};

// What a device advertises it will accept.
struct ProviderProfileCatalog {
  ProfileCatalogAvailability availability = ProfileCatalogAvailability::NOT_THIS_PROVIDER;
  std::vector<ProviderProfileEntry> entries;
};

// A catalog after resolution: what the device advertises, narrowed by any
// ingested description, with the provenance of that outcome.
//
// origin is native_reported when the provider's enumeration stands unchanged,
// core_derived when a description narrowed it (neither source alone produced the
// result), and user_supplied when the provider could not enumerate and the
// description supplied the catalog outright.
struct ResolvedProfileCatalog {
  bool enumerated = false;
  FactOrigin origin = FactOrigin::UNKNOWN;
  std::vector<ProviderProfileEntry> entries;
};

struct ProviderCaptureImageFacts {
  std::optional<SourcedFact<Intrinsics>> intrinsics;
  std::optional<SourcedFact<Distortion>> distortion;
  // A provider supplies whichever of these it can measure and omits the other;
  // neither is required, and neither is derived from the other by Core.
  std::optional<SourcedFact<Intrinsics>> intrinsics_delivered;
  std::optional<SourcedFact<DeliveredImageRegion>> delivered_image_region;
  std::optional<SourcedFact<CameraPose>> pose;
  // Acquisition timing travels only on the accepted FrameView. These remaining
  // image facts are independently reported per capture member.
  std::optional<SourcedFact<FocusState>> focus_state;
  std::optional<SourcedFact<ExposureTime>> exposure_time;
  std::optional<SourcedFact<SensorSensitivityIso>> sensor_sensitivity_iso;
  std::optional<SourcedFact<ApertureFNumber>> aperture_f_number;
  std::optional<SourcedFact<FocalLengthMm>> focal_length_mm;
  std::optional<SourcedFact<RealizedImageTransform>> realized_image_transform;
};

} // namespace cambang

#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
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
  static std::optional<TickPeriod> create(uint64_t numerator_ns, uint64_t denominator) {
    if (numerator_ns == 0 || denominator == 0) {
      return std::nullopt;
    }
    return TickPeriod(numerator_ns, denominator);
  }

  uint64_t numerator_ns() const noexcept { return numerator_ns_; }
  uint64_t denominator() const noexcept { return denominator_; }

 private:
  TickPeriod(uint64_t numerator_ns, uint64_t denominator)
      : numerator_ns_(numerator_ns), denominator_(denominator) {}

  uint64_t numerator_ns_;
  uint64_t denominator_;
};

struct ImageAcquisitionTiming {
  // Zero remains a valid mark when the declared clock domain provides it.
  uint64_t acquisition_mark;
  TickPeriod tick_period;
  ImageAcquisitionClockDomain clock_domain;
  ImageAcquisitionReferenceEvent reference_event;
  ImageAcquisitionComparability comparability;
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
  std::optional<SourcedFact<Intrinsics>> intrinsics;
  std::optional<SourcedFact<Distortion>> distortion;
  std::optional<SourcedFact<CameraPose>> pose;
};

struct CaptureAdmissionFacts {
  std::optional<SourcedFact<Geolocation>> geolocation;
  std::optional<SourcedFact<CaptureDateTime>> capture_datetime;
};

struct CaptureImageFacts {
  std::optional<SourcedFact<ImageAcquisitionTiming>> acquisition_timing;
  std::optional<SourcedFact<FocusState>> focus_state;
  std::optional<SourcedFact<RealizedImageTransform>> realized_image_transform;
};

// Provider ingress names the authority without changing the source-neutral
// record shapes. These are retained independently of external configuration.
struct ProviderCameraFacts {
  CameraStaticFacts static_facts;
};

struct ProviderCaptureImageFacts {
  std::optional<SourcedFact<Intrinsics>> intrinsics;
  std::optional<SourcedFact<Distortion>> distortion;
  std::optional<SourcedFact<CameraPose>> pose;
  CaptureImageFacts image;
};

} // namespace cambang

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/camera_fact_types.h"
#include "core/core_result_store.h"

using namespace cambang;

namespace {

template <typename T, typename = void>
struct has_effective_authority : std::false_type {};

template <typename T>
struct has_effective_authority<T, std::void_t<decltype(std::declval<T>().authority)>>
    : std::true_type {};

void verify_camera_fact_types() {
  static_assert(std::is_same_v<
      decltype(CameraStaticFacts{}.facing),
      std::optional<SourcedFact<CameraFacing>>>);
  static_assert(std::is_same_v<
      decltype(CaptureImageFacts{}.realized_image_transform),
      std::optional<SourcedFact<RealizedImageTransform>>>);
  static_assert(!has_effective_authority<SourcedFact<CameraFacing>>::value);
  static_assert(!std::is_same_v<SensorOrientationDegrees, ImageRotationDegrees>);
  static_assert(!std::is_same_v<CaptureDateTime, ImageAcquisitionTiming>);
  static_assert(!std::is_same_v<CameraStaticFacts, CaptureAdmissionFacts>);
  static_assert(!std::is_same_v<CaptureAdmissionFacts, CaptureImageFacts>);
  static_assert(!std::is_default_constructible_v<Intrinsics>);
  static_assert(!std::is_default_constructible_v<BrownConrady5Distortion>);
  static_assert(!std::is_default_constructible_v<FocusAtDistance>);
  static_assert(!std::is_default_constructible_v<GeodeticAltitude>);
  static_assert(!std::is_default_constructible_v<Geolocation>);

  CameraStaticFacts description{};
  CaptureAdmissionFacts admission{};
  CaptureImageFacts image{};
  assert(!description.facing);
  assert(!description.nature);
  assert(!description.sensor_orientation);
  assert(!description.intrinsics);
  assert(!description.distortion);
  assert(!description.pose);
  assert(!admission.geolocation);
  assert(!admission.capture_datetime);
  assert(!image.acquisition_timing);
  assert(!image.focus_state);
  assert(!image.realized_image_transform);

  description.facing = SourcedFact<CameraFacing>{
      CameraFacing::UNKNOWN,
      FactOrigin::UNKNOWN};
  assert(description.facing);
  assert(description.facing->value == CameraFacing::UNKNOWN);
  assert(description.facing->origin == FactOrigin::UNKNOWN);
  const SourcedFact<CameraFacing> provider_derived_facing{
      CameraFacing::BACK, FactOrigin::DERIVED};
  const SourcedFact<CameraFacing> core_derived_facing{
      CameraFacing::BACK, FactOrigin::CORE_DERIVED};
  assert(provider_derived_facing.value == core_derived_facing.value);
  assert(provider_derived_facing.origin != core_derived_facing.origin);
  description.nature = SourcedFact<CameraNature>{
      CameraNature::VIRTUAL,
      FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  description.sensor_orientation = SourcedFact<SensorOrientationDegrees>{
      SensorOrientationDegrees::DEGREES_0,
      FactOrigin::USER_SUPPLIED};
  assert(description.sensor_orientation);
  assert(description.sensor_orientation->value == SensorOrientationDegrees::DEGREES_0);
  assert(description.nature->origin == FactOrigin::VIRTUAL_CAMERA_AUTHORED);

  const CoordinateDomain known_domain = CoordinateDomainDeliveredImage{};
  const auto intrinsics = Intrinsics::create(
      3120.4, 3118.9, 2014.3, 1508.7, std::nullopt, 4032, 3024, known_domain);
  assert(intrinsics);
  description.intrinsics = SourcedFact<Intrinsics>{
      *intrinsics, FactOrigin::NATIVE_REPORTED};
  assert(!description.intrinsics->value.skew_px());
  const auto zero_skew_intrinsics = Intrinsics::create(
      3120.4, 3118.9, 2014.3, 1508.7, 0.0, 4032, 3024, known_domain);
  assert(zero_skew_intrinsics);
  assert(zero_skew_intrinsics->skew_px());
  assert(*zero_skew_intrinsics->skew_px() == 0.0);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double positive_infinity = std::numeric_limits<double>::infinity();
  assert(!Intrinsics::create(3120.4, 3118.9, 2014.3, 1508.7, std::nullopt, 0, 3024, known_domain));
  assert(!Intrinsics::create(3120.4, 3118.9, 2014.3, 1508.7, std::nullopt, 4032, 0, known_domain));
  assert(!Intrinsics::create(nan, 3118.9, 2014.3, 1508.7, std::nullopt, 4032, 3024, known_domain));
  assert(!Intrinsics::create(3120.4, positive_infinity, 2014.3, 1508.7, std::nullopt, 4032, 3024, known_domain));
  assert(!Intrinsics::create(3120.4, 3118.9, nan, 1508.7, std::nullopt, 4032, 3024, known_domain));
  assert(!Intrinsics::create(3120.4, 3118.9, 2014.3, positive_infinity, std::nullopt, 4032, 3024, known_domain));
  assert(!Intrinsics::create(3120.4, 3118.9, 2014.3, 1508.7, nan, 4032, 3024, known_domain));
  assert(!Intrinsics::create(3120.4, 3118.9, 2014.3, 1508.7, positive_infinity, 4032, 3024, known_domain));

  assert(std::holds_alternative<CoordinateDomainDeliveredImage>(known_domain));
  assert(!CoordinateDomainPlatformDefined::create(""));
  const auto platform_domain = CoordinateDomainPlatformDefined::create("synthetic-native");
  assert(platform_domain);
  const CoordinateDomain configured_domain = *platform_domain;
  assert(std::holds_alternative<CoordinateDomainPlatformDefined>(configured_domain));
  assert(std::get<CoordinateDomainPlatformDefined>(configured_domain).token() == "synthetic-native");

  description.distortion = SourcedFact<Distortion>{
      NoDistortion{DistortionImageState::RECTIFIED},
      FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  assert(std::holds_alternative<NoDistortion>(description.distortion->value));
  const auto brown_distortion = BrownConrady5Distortion::create(
      0.0, 0.0, 0.0, 0.0, 0.0, 4032, 3024, known_domain,
      DistortionImageState::DISTORTED);
  assert(brown_distortion);
  description.distortion = SourcedFact<Distortion>{
      *brown_distortion,
      FactOrigin::DERIVED};
  assert(std::holds_alternative<BrownConrady5Distortion>(description.distortion->value));
  const auto& brown = std::get<BrownConrady5Distortion>(description.distortion->value);
  assert(brown.radial_k1() == 0.0);
  assert(brown.tangential_p2() == 0.0);
  assert(!BrownConrady5Distortion::create(0.0, 0.0, 0.0, 0.0, 0.0, 0, 3024, known_domain, DistortionImageState::DISTORTED));
  assert(!BrownConrady5Distortion::create(0.0, 0.0, 0.0, 0.0, 0.0, 4032, 0, known_domain, DistortionImageState::DISTORTED));
  assert(!BrownConrady5Distortion::create(nan, 0.0, 0.0, 0.0, 0.0, 4032, 3024, known_domain, DistortionImageState::DISTORTED));
  assert(!BrownConrady5Distortion::create(0.0, positive_infinity, 0.0, 0.0, 0.0, 4032, 3024, known_domain, DistortionImageState::DISTORTED));
  assert(!BrownConrady5Distortion::create(0.0, 0.0, -positive_infinity, 0.0, 0.0, 4032, 3024, known_domain, DistortionImageState::DISTORTED));
  assert(description.distortion->origin == FactOrigin::DERIVED);

  assert(!PoseReferenceCamera::create(""));
  assert(!PoseReferenceCustom::create(""));
  assert(!PoseReferencePlatformDefined::create(""));
  assert(!PoseConventionPlatformDefined::create(""));
  const auto camera_reference_value = PoseReferenceCamera::create("Camera A ");
  const auto custom_reference_value = PoseReferenceCustom::create("synthetic-rig");
  const auto platform_reference_value = PoseReferencePlatformDefined::create("platform-rig");
  const auto platform_convention_value = PoseConventionPlatformDefined::create("native-pose");
  assert(camera_reference_value && custom_reference_value && platform_reference_value && platform_convention_value);
  assert(camera_reference_value->camera_id() == "Camera A ");
  assert(custom_reference_value->reference_id() == "synthetic-rig");
  assert(platform_reference_value->reference_token() == "platform-rig");
  assert(platform_convention_value->convention_token() == "native-pose");
  const PoseReference camera_reference = *camera_reference_value;
  const PoseReference custom_reference = *custom_reference_value;
  const PoseReference platform_reference = *platform_reference_value;
  assert(std::holds_alternative<PoseReferenceCamera>(camera_reference));
  assert(std::holds_alternative<PoseReferenceCustom>(custom_reference));
  assert(std::holds_alternative<PoseReferencePlatformDefined>(platform_reference));
  const PoseConvention platform_convention = *platform_convention_value;
  assert(std::holds_alternative<PoseConventionPlatformDefined>(platform_convention));

  const auto valid_pose = CameraPose::create(
      *custom_reference_value,
      PoseConventionCameraOpticalFrame{},
      Vec3Meters{0.0, 0.0, 0.0},
      QuaternionXyzw{0.0, 0.0, 0.0, 2.0});
  assert(valid_pose);
  assert(!CameraPose::create(
      *camera_reference_value,
      PoseConventionAndroidCamera2{},
      Vec3Meters{0.0, 0.0, 0.0},
      QuaternionXyzw{0.0, 0.0, 0.0, 0.0}));
  assert(std::holds_alternative<PoseReferenceCustom>(valid_pose->reference()));
  assert(std::holds_alternative<PoseConventionCameraOpticalFrame>(valid_pose->convention()));
  assert(valid_pose->translation_m().x == 0.0);

  description.pose = SourcedFact<CameraPose>{
      *valid_pose, FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  assert(std::holds_alternative<PoseReferenceCustom>(description.pose->value.reference()));
  assert(std::holds_alternative<PoseConventionCameraOpticalFrame>(description.pose->value.convention()));

  const auto zero_geolocation = Geolocation::create(
      0.0, 0.0, std::nullopt, 4.5, 8.0, AbsoluteUtcDateTime{1780000000000});
  assert(zero_geolocation);
  admission.geolocation = SourcedFact<Geolocation>{*zero_geolocation, FactOrigin::USER_SUPPLIED};
  assert(admission.geolocation);
  assert(admission.geolocation->value.latitude_degrees() == 0.0);
  assert(admission.geolocation->value.longitude_degrees() == 0.0);
  assert(!admission.geolocation->value.altitude());
  assert(admission.geolocation->value.sample_datetime_utc());
  const auto altitude = GeodeticAltitude::create(47.0, AltitudeReference::MEAN_SEA_LEVEL);
  assert(altitude);
  const auto geolocation_with_altitude = Geolocation::create(
      0.0, 0.0, *altitude, std::nullopt, std::nullopt, std::nullopt);
  assert(geolocation_with_altitude);
  assert(geolocation_with_altitude->altitude());
  assert(geolocation_with_altitude->altitude()->reference() == AltitudeReference::MEAN_SEA_LEVEL);
  assert(!Geolocation::create(nan, 0.0, std::nullopt, std::nullopt, std::nullopt, std::nullopt));
  assert(!Geolocation::create(0.0, positive_infinity, std::nullopt, std::nullopt, std::nullopt, std::nullopt));
  assert(!GeodeticAltitude::create(nan, AltitudeReference::UNKNOWN));
  assert(!Geolocation::create(0.0, 0.0, std::nullopt, nan, std::nullopt, std::nullopt));
  assert(!Geolocation::create(0.0, 0.0, std::nullopt, std::nullopt, -positive_infinity, std::nullopt));
  admission.capture_datetime = SourcedFact<CaptureDateTime>{
      CaptureDateTime{AbsoluteUtcDateTime{1780000000001},
                      CaptureDateTimeReferenceEvent::CAPTURE_ADMISSION},
      FactOrigin::RUNTIME_INJECTED};
  assert(admission.capture_datetime->value.utc.unix_ms == 1780000000001);

  const auto tick_period = TickPeriod::create(1, 1);
  assert(tick_period);
  assert(!TickPeriod::create(0, 1));
  assert(!TickPeriod::create(1, 0));
  image.acquisition_timing = SourcedFact<ImageAcquisitionTiming>{
      ImageAcquisitionTiming{0, *tick_period, ImageAcquisitionClockDomain::PROVIDER_MONOTONIC,
                             ImageAcquisitionReferenceEvent::EXPOSURE_MIDPOINT,
                             ImageAcquisitionComparability::SAME_DEVICE},
      FactOrigin::NATIVE_REPORTED};
  const auto zero_focus = FocusAtDistance::create(0.0);
  const auto finite_focus = FocusAtDistance::create(-1.0);
  assert(zero_focus && finite_focus);
  assert(!FocusAtDistance::create(nan));
  assert(!FocusAtDistance::create(positive_infinity));
  assert(!FocusAtDistance::create(-positive_infinity));
  image.focus_state = SourcedFact<FocusState>{FocusAtInfinity{}, FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  image.realized_image_transform = SourcedFact<RealizedImageTransform>{
      RealizedImageTransform{ImageRotationDegrees::DEGREES_0, false, true},
      FactOrigin::CORE_DERIVED};
  assert(std::holds_alternative<FocusAtInfinity>(image.focus_state->value));
  image.focus_state = SourcedFact<FocusState>{FocusStateUnknown{}, FactOrigin::UNKNOWN};
  assert(std::holds_alternative<FocusStateUnknown>(image.focus_state->value));
  image.focus_state = SourcedFact<FocusState>{*zero_focus, FactOrigin::DERIVED};
  assert(std::holds_alternative<FocusAtDistance>(image.focus_state->value));
  assert(std::get<FocusAtDistance>(image.focus_state->value).distance_m() == 0.0);
  assert(image.acquisition_timing->value.clock_domain ==
         ImageAcquisitionClockDomain::PROVIDER_MONOTONIC);
  assert(image.acquisition_timing->value.acquisition_mark == 0);
  assert(image.acquisition_timing->value.tick_period.numerator_ns() == 1);
  assert(image.acquisition_timing->value.tick_period.denominator() == 1);
  assert(image.acquisition_timing->value.reference_event ==
         ImageAcquisitionReferenceEvent::EXPOSURE_MIDPOINT);
  assert(image.acquisition_timing->value.comparability ==
         ImageAcquisitionComparability::SAME_DEVICE);
  assert(image.realized_image_transform->value.rotation == ImageRotationDegrees::DEGREES_0);
  assert(image.realized_image_transform->value.pixels_already_transformed);
  assert(image.realized_image_transform->origin == FactOrigin::CORE_DERIVED);
  assert(FactOrigin::DERIVED != FactOrigin::CORE_DERIVED);
}

} // namespace

int main() {
  verify_camera_fact_types();

  CoreResultStore store;

  assert(kResultAccessCheapWithinBestMultiplier == 2);
  uint64_t single_candidate_costs[] = {10};
  assert(classify_supported_non_ready_result_access_from_normalized_costs(
             ResultCapability::CHEAP, single_candidate_costs, 1) == ResultCapability::CHEAP);
  assert(classify_supported_non_ready_result_access_from_normalized_costs(
             ResultCapability::EXPENSIVE, single_candidate_costs, 1) == ResultCapability::EXPENSIVE);
  uint64_t multi_candidate_costs[] = {100, 40};
  assert(classify_supported_non_ready_result_access_from_normalized_costs(
             ResultCapability::EXPENSIVE, multi_candidate_costs, 2) == ResultCapability::CHEAP);
  assert(classify_supported_non_ready_result_access_from_normalized_costs(
             ResultCapability::READY, multi_candidate_costs, 2) == ResultCapability::READY);
  assert(classify_supported_non_ready_result_access_from_normalized_costs(
             ResultCapability::UNSUPPORTED, multi_candidate_costs, 2) == ResultCapability::UNSUPPORTED);

  auto refined_record = std::make_shared<CoreResultAccessClassificationRecord>();
  assert(resolve_result_access_classification(
             ResultCapability::EXPENSIVE,
             refined_record,
             CoreResultAccessOperation::TO_IMAGE) == ResultCapability::EXPENSIVE);
  refine_result_access_classification(
      refined_record,
      CoreResultAccessOperation::TO_IMAGE,
      ResultCapability::CHEAP);
  assert(resolve_result_access_classification(
             ResultCapability::EXPENSIVE,
             refined_record,
             CoreResultAccessOperation::TO_IMAGE) == ResultCapability::CHEAP);
  refine_result_access_classification(
      refined_record,
      CoreResultAccessOperation::DISPLAY_VIEW,
      ResultCapability::EXPENSIVE);
  assert(resolve_result_access_classification(
             ResultCapability::READY,
             refined_record,
             CoreResultAccessOperation::DISPLAY_VIEW) == ResultCapability::READY);
  assert(resolve_result_access_classification(
             ResultCapability::UNSUPPORTED,
             refined_record,
             CoreResultAccessOperation::TO_IMAGE) == ResultCapability::UNSUPPORTED);

  std::vector<uint8_t> px(2 * 2 * 4, 7);
  FrameView stream_frame{};
  stream_frame.device_instance_id = 10;
  stream_frame.stream_id = 20;
  stream_frame.width = 2;
  stream_frame.height = 2;
  stream_frame.format_fourcc = FOURCC_RGBA;
  stream_frame.data = px.data();
  stream_frame.size_bytes = px.size();
  stream_frame.stride_bytes = 0;
  constexpr uint64_t kStreamEpochA = 100;
  constexpr uint64_t kStreamEpochB = 101;
  constexpr uint64_t kCaptureEpochA = 200;
  CoreRetainedProductionPlan requested_cpu{};
  requested_cpu.valid = true;
  requested_cpu.posture = CoreProductionPostureShape::CpuPrimary;
  CoreRetainedProductionPlan requested_gpu_no_sidecar{};
  requested_gpu_no_sidecar.valid = true;
  requested_gpu_no_sidecar.posture = CoreProductionPostureShape::GpuPrimaryNoCpuSidecar;
  CoreRetainedProductionPlan requested_gpu_with_sidecar{};
  requested_gpu_with_sidecar.valid = true;
  requested_gpu_with_sidecar.posture = CoreProductionPostureShape::GpuPrimaryWithCpuSidecar;
  assert(store.retain_frame(stream_frame, StreamIntent::VIEWFINDER, 1234, kStreamEpochA, 0, requested_cpu));
  FrameView mismatched_cpu_request = stream_frame;
  mismatched_cpu_request.stream_id = 120;
  mismatched_cpu_request.primary_backing_kind = ProducerBackingKind::GPU;
  mismatched_cpu_request.primary_backing_artifact = std::make_shared<int>(120);
  assert(!store.retain_frame(mismatched_cpu_request, StreamIntent::VIEWFINDER, 1234, kStreamEpochA, 0, requested_cpu));
  FrameView provider_echo_only_request = stream_frame;
  provider_echo_only_request.stream_id = 121;
  provider_echo_only_request.requested_retained_plan = requested_cpu;
  assert(!store.retain_frame(provider_echo_only_request, StreamIntent::VIEWFINDER, 1234, kStreamEpochA, 0));
  assert(!store.get_latest_stream_result(120));
  assert(!store.get_latest_stream_result(121));

  auto stream_result = store.get_latest_stream_result(20);
  assert(stream_result);
  assert(stream_result->stream_id == 20);
  assert(stream_result->payload.width == 2);
  assert(stream_result->payload_kind == ResultPayloadKind::CPU_PACKED);
  assert(stream_result->retained_access_truth.display_view == ResultCapability::CHEAP);
  assert(stream_result->retained_access_truth.to_image == ResultCapability::CHEAP);
  assert(stream_result->retained_access_truth.encoded_bytes == ResultCapability::UNSUPPORTED);
  assert(stream_result->access_posture.posture_id != 0);
  assert(stream_result->access_posture.payload_kind == ResultPayloadKind::CPU_PACKED);
  assert(stream_result->access_posture.has_retained_cpu_payload);
  assert(!stream_result->access_posture.has_retained_gpu_backing);
  const uint64_t cpu_stream_posture_id = stream_result->access_posture.posture_id;

  store.retain_frame(stream_frame, StreamIntent::VIEWFINDER, 1235, kStreamEpochA, 0, requested_cpu);
  auto repeated_cpu_stream_result = store.get_latest_stream_result(20);
  assert(repeated_cpu_stream_result);
  assert(repeated_cpu_stream_result->access_posture.posture_id == cpu_stream_posture_id);

  store.retain_frame(stream_frame, StreamIntent::VIEWFINDER, 1236, kStreamEpochB, 0, requested_cpu);
  auto restarted_cpu_stream_result = store.get_latest_stream_result(20);
  assert(restarted_cpu_stream_result);
  assert(restarted_cpu_stream_result->access_posture.posture_id != cpu_stream_posture_id);

  FrameView gpu_only_stream_frame = stream_frame;
  gpu_only_stream_frame.stream_id = 21;
  gpu_only_stream_frame.primary_backing_kind = ProducerBackingKind::GPU;
  gpu_only_stream_frame.primary_backing_artifact = std::make_shared<int>(42);
  gpu_only_stream_frame.retain_cpu_sidecar = false;
  assert(store.retain_frame(gpu_only_stream_frame, StreamIntent::VIEWFINDER, 1236, kStreamEpochA, 0, requested_gpu_no_sidecar));

  auto gpu_only_stream_result = store.get_latest_stream_result(21);
  assert(gpu_only_stream_result);
  assert(gpu_only_stream_result->payload_kind == ResultPayloadKind::GPU_SURFACE);
  assert(gpu_only_stream_result->retained_gpu_backing);
  assert(gpu_only_stream_result->retained_access_truth.display_view == ResultCapability::READY);
  assert(gpu_only_stream_result->retained_access_truth.to_image == ResultCapability::UNSUPPORTED);
  assert(gpu_only_stream_result->retained_access_truth.encoded_bytes == ResultCapability::UNSUPPORTED);
  assert(resolve_result_access_classification(
             gpu_only_stream_result->retained_access_truth.display_view,
             gpu_only_stream_result->access_classification,
             CoreResultAccessOperation::DISPLAY_VIEW) == ResultCapability::READY);
  assert(resolve_result_access_classification(
             gpu_only_stream_result->retained_access_truth.to_image,
             gpu_only_stream_result->access_classification,
             CoreResultAccessOperation::TO_IMAGE) == ResultCapability::UNSUPPORTED);

  FrameView gpu_materializable_stream_frame = gpu_only_stream_frame;
  gpu_materializable_stream_frame.stream_id = 23;
  gpu_materializable_stream_frame.retained_gpu_backing_descriptor.valid = true;
  gpu_materializable_stream_frame.retained_gpu_backing_descriptor.materialization_available = true;
  store.retain_frame(gpu_materializable_stream_frame, StreamIntent::VIEWFINDER, 1238, kStreamEpochA, 0, requested_gpu_no_sidecar);

  auto gpu_materializable_stream_result = store.get_latest_stream_result(23);
  assert(gpu_materializable_stream_result);
  assert(gpu_materializable_stream_result->payload_kind == ResultPayloadKind::GPU_SURFACE);
  assert(gpu_materializable_stream_result->retained_gpu_backing);
  assert(gpu_materializable_stream_result->retained_access_truth.display_view == ResultCapability::READY);
  assert(gpu_materializable_stream_result->retained_access_truth.to_image == ResultCapability::EXPENSIVE);
  assert(gpu_materializable_stream_result->retained_access_truth.encoded_bytes == ResultCapability::UNSUPPORTED);
  refine_result_access_classification(
      gpu_materializable_stream_result->access_classification,
      CoreResultAccessOperation::TO_IMAGE,
      classify_supported_non_ready_result_access_from_normalized_costs(
          gpu_materializable_stream_result->retained_access_truth.to_image,
          single_candidate_costs,
          1));
  assert(resolve_result_access_classification(
             gpu_materializable_stream_result->retained_access_truth.to_image,
             gpu_materializable_stream_result->access_classification,
             CoreResultAccessOperation::TO_IMAGE) == ResultCapability::EXPENSIVE);
  assert(gpu_materializable_stream_result->access_posture.payload_kind == ResultPayloadKind::GPU_SURFACE);
  assert(!gpu_materializable_stream_result->access_posture.has_retained_cpu_payload);
  assert(gpu_materializable_stream_result->access_posture.has_retained_gpu_backing);
  assert(gpu_materializable_stream_result->access_posture.gpu_materialization_available);
  const uint64_t gpu_materializable_posture_id = gpu_materializable_stream_result->access_posture.posture_id;

  gpu_materializable_stream_frame.primary_backing_artifact = std::make_shared<int>(45);
  store.retain_frame(gpu_materializable_stream_frame, StreamIntent::VIEWFINDER, 1239, kStreamEpochA, 0, requested_gpu_no_sidecar);
  auto repeated_gpu_materializable_stream_result = store.get_latest_stream_result(23);
  assert(repeated_gpu_materializable_stream_result);
  assert(repeated_gpu_materializable_stream_result->access_posture.posture_id == gpu_materializable_posture_id);

  gpu_materializable_stream_frame.retained_gpu_backing_descriptor.materialization_available = false;
  store.retain_frame(gpu_materializable_stream_frame, StreamIntent::VIEWFINDER, 1240, kStreamEpochA, 0, requested_gpu_no_sidecar);
  auto transitioned_gpu_stream_result = store.get_latest_stream_result(23);
  assert(transitioned_gpu_stream_result);
  assert(transitioned_gpu_stream_result->access_posture.posture_id != gpu_materializable_posture_id);
  assert(resolve_result_access_classification(
             transitioned_gpu_stream_result->retained_access_truth.to_image,
             transitioned_gpu_stream_result->access_classification,
             CoreResultAccessOperation::TO_IMAGE) == ResultCapability::UNSUPPORTED);

  FrameView gpu_stream_frame = stream_frame;
  gpu_stream_frame.stream_id = 22;
  gpu_stream_frame.primary_backing_kind = ProducerBackingKind::GPU;
  gpu_stream_frame.primary_backing_artifact = std::make_shared<int>(43);
  gpu_stream_frame.retain_cpu_sidecar = true;
  assert(store.retain_frame(gpu_stream_frame, StreamIntent::VIEWFINDER, 1237, kStreamEpochA, 0, requested_gpu_with_sidecar));

  auto gpu_stream_result = store.get_latest_stream_result(22);
  assert(gpu_stream_result);
  assert(gpu_stream_result->payload_kind == ResultPayloadKind::GPU_SURFACE);
  assert(gpu_stream_result->retained_gpu_backing);
  assert(gpu_stream_result->retained_access_truth.display_view == ResultCapability::READY);
  assert(gpu_stream_result->retained_access_truth.to_image == ResultCapability::CHEAP);
  assert(gpu_stream_result->retained_access_truth.encoded_bytes == ResultCapability::UNSUPPORTED);

  store.mark_stream_display_demand(20, 1'000'000'000ull);
  assert(store.is_stream_display_demand_active(20, 1'150'000'000ull));
  assert(!store.is_stream_display_demand_active(20, 1'260'000'001ull));

  // Demand marks for unknown streams are ignored/evicted to bound the map.
  store.mark_stream_display_demand(999, 2'000'000'000ull);
  assert(!store.is_stream_display_demand_active(999, 2'000'000'000ull));

  store.remove_stream_result(20);
  assert(!store.get_latest_stream_result(20));
  assert(!store.is_stream_display_demand_active(20, 1'150'000'000ull));

  store.retain_frame(stream_frame, StreamIntent::VIEWFINDER, 1235, kStreamEpochA, 0, requested_cpu);
  assert(store.get_latest_stream_result(20));
  store.mark_stream_display_demand(20, 3'000'000'000ull);
  assert(store.is_stream_display_demand_active(20, 3'010'000'000ull));

  FrameView capture_a = stream_frame;
  capture_a.stream_id = 0;
  capture_a.capture_id = 77;
  capture_a.device_instance_id = 100;
  assert(store.retain_frame(capture_a, std::nullopt, 2000, 0, kCaptureEpochA, {}, requested_cpu));

  FrameView capture_b = stream_frame;
  capture_b.stream_id = 0;
  capture_b.capture_id = 77;
  capture_b.device_instance_id = 101;
  store.retain_frame(capture_b, std::nullopt, 2001, 0, kCaptureEpochA, {}, requested_cpu);

  auto capture_result = store.get_capture_result(77, 100);
  assert(capture_result);
  assert(capture_result->capture_id == 77);
  assert(capture_result->device_instance_id == 100);
  assert(capture_result->default_image.image_member_index == 0);
  assert(capture_result->default_image.role == CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED);
  assert(capture_result->default_image.payload.width == 2);
  assert(capture_result->default_image.retained_access_truth.display_view == ResultCapability::CHEAP);
  assert(capture_result->default_image.retained_access_truth.to_image == ResultCapability::CHEAP);
  assert(capture_result->default_image.retained_access_truth.encoded_bytes == ResultCapability::UNSUPPORTED);
  assert(capture_result->default_image.access_posture.posture_id != 0);
  assert(capture_result->default_image.access_posture.device_instance_id == 100);
  assert(capture_result->default_image.access_posture.has_retained_cpu_payload);
  assert(capture_result->additional_images.empty());
  const uint64_t capture_default_posture_id = capture_result->default_image.access_posture.posture_id;

  FrameView capture_a_second = capture_a;
  capture_a_second.capture_id = 79;
  store.retain_frame(capture_a_second, std::nullopt, 2008, 0, kCaptureEpochA, {}, requested_cpu);
  auto capture_result_second = store.get_capture_result(79, 100);
  assert(capture_result_second);
  assert(capture_result_second->default_image.access_posture.posture_id == capture_default_posture_id);

  FrameView capture_a_reconfigured = capture_a;
  capture_a_reconfigured.capture_id = 80;
  store.retain_frame(capture_a_reconfigured, std::nullopt, 2009, 0, kCaptureEpochA + 1, {}, requested_cpu);
  auto capture_result_reconfigured = store.get_capture_result(80, 100);
  assert(capture_result_reconfigured);
  assert(capture_result_reconfigured->default_image.access_posture.posture_id != capture_default_posture_id);

  FrameView gpu_capture = capture_a;
  gpu_capture.capture_id = 78;
  gpu_capture.data = nullptr;
  gpu_capture.size_bytes = 0;
  gpu_capture.primary_backing_kind = ProducerBackingKind::GPU;
  gpu_capture.primary_backing_artifact = std::make_shared<int>(44);
  gpu_capture.retain_cpu_sidecar = false;
  gpu_capture.retained_gpu_backing_descriptor.valid = true;
  gpu_capture.retained_gpu_backing_descriptor.materialization_available = true;
  assert(store.retain_frame(gpu_capture, std::nullopt, 2007, 0, kCaptureEpochA, {}, requested_gpu_no_sidecar));
  FrameView mismatched_capture_request = gpu_capture;
  mismatched_capture_request.capture_id = 178;
  assert(!store.retain_frame(mismatched_capture_request, std::nullopt, 2007, 0, kCaptureEpochA, {}, requested_cpu));
  auto gpu_capture_result = store.get_capture_result(78, 100);
  assert(gpu_capture_result);
  assert(gpu_capture_result->payload_kind == ResultPayloadKind::GPU_SURFACE);
  assert(gpu_capture_result->default_image.payload_kind == ResultPayloadKind::GPU_SURFACE);
  assert(gpu_capture_result->default_image.payload.empty());
  assert(gpu_capture_result->default_image.retained_gpu_backing);
  assert(gpu_capture_result->default_image.retained_access_truth.to_image == ResultCapability::EXPENSIVE);
  assert(gpu_capture_result->default_image.retained_access_truth.encoded_bytes == ResultCapability::UNSUPPORTED);
  assert(gpu_capture_result->default_image.access_posture.posture_id != 0);
  assert(gpu_capture_result->default_image.access_posture.has_retained_gpu_backing);
  assert(gpu_capture_result->default_image.access_posture.gpu_materialization_available);

  const uint64_t capture_id_before = capture_result->capture_id;
  const uint64_t device_id_before = capture_result->device_instance_id;
  const uint32_t width_before = capture_result->image_width;
  const uint32_t height_before = capture_result->image_height;
  const uint32_t format_before = capture_result->image_format_fourcc;
  const auto payload_kind_before = capture_result->payload_kind;
  const uint64_t default_ts_before = capture_result->default_image.capture_timestamp_ns;
  const size_t default_bytes_before = capture_result->default_image.payload.size_bytes();

  CoreCaptureResultData::ImageMemberData bracket{};
  bracket.image_member_index = 1;
  bracket.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
  bracket.capture_timestamp_ns = 2002;
  bracket.payload = capture_result->default_image.payload;
  assert(store.append_additional_capture_image(77, 100, bracket, kCaptureEpochA, requested_cpu));

  auto capture_result_with_bracket = store.get_capture_result(77, 100);
  assert(capture_result_with_bracket);
  assert(capture_result_with_bracket->default_image.capture_timestamp_ns == default_ts_before);
  assert(capture_result_with_bracket->default_image.payload.size_bytes() == default_bytes_before);
  assert(capture_result_with_bracket->additional_images.size() == 1);
  assert(capture_result_with_bracket->additional_images[0].role == CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET);
  assert(capture_result_with_bracket->additional_images[0].image_member_index == 1);
  assert(capture_result_with_bracket->additional_images[0].retained_access_truth.display_view == ResultCapability::CHEAP);
  assert(capture_result_with_bracket->additional_images[0].retained_access_truth.to_image == ResultCapability::CHEAP);
  assert(capture_result_with_bracket->additional_images[0].retained_access_truth.encoded_bytes == ResultCapability::UNSUPPORTED);
  assert(capture_result_with_bracket->additional_images[0].access_posture.posture_id != 0);
  assert(capture_result_with_bracket->additional_images[0].access_posture.device_instance_id == 100);
  assert(capture_result_with_bracket->additional_images[0].access_posture.has_retained_cpu_payload);
  assert(capture_result_with_bracket->additional_images[0].access_posture.posture_id == capture_default_posture_id);
  assert(capture_result_with_bracket->capture_id == capture_id_before);
  assert(capture_result_with_bracket->device_instance_id == device_id_before);
  assert(capture_result_with_bracket->image_width == width_before);
  assert(capture_result_with_bracket->image_height == height_before);
  assert(capture_result_with_bracket->image_format_fourcc == format_before);
  assert(capture_result_with_bracket->payload_kind == payload_kind_before);

  CoreCaptureResultData::ImageMemberData bad_role{};
  bad_role.role = CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED;
  bad_role.capture_timestamp_ns = 2003;
  bad_role.payload = capture_result_with_bracket->default_image.payload;
  assert(!store.append_additional_capture_image(77, 100, bad_role, kCaptureEpochA, requested_cpu));

  CoreCaptureResultData::ImageMemberData bad_payload{};
  bad_payload.image_member_index = 2;
  bad_payload.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
  bad_payload.capture_timestamp_ns = 2004;
  bad_payload.payload = CoreResultPayloadCpuPacked{};
  assert(!store.append_additional_capture_image(77, 100, bad_payload, kCaptureEpochA, requested_cpu));
  CoreCaptureResultData::ImageMemberData out_of_order{};
  out_of_order.image_member_index = 3;
  out_of_order.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
  out_of_order.capture_timestamp_ns = 2005;
  out_of_order.payload = capture_result_with_bracket->default_image.payload;
  assert(!store.append_additional_capture_image(77, 100, out_of_order, kCaptureEpochA, requested_cpu));
  CoreCaptureResultData::ImageMemberData duplicate_index{};
  duplicate_index.image_member_index = 1;
  duplicate_index.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
  duplicate_index.capture_timestamp_ns = 2006;
  duplicate_index.payload = capture_result_with_bracket->default_image.payload;
  assert(!store.append_additional_capture_image(77, 100, duplicate_index, kCaptureEpochA, requested_cpu));
  assert(!store.append_additional_capture_image(999, 100, bracket, kCaptureEpochA, requested_cpu));

  auto capture_set = store.get_capture_result_set(77);
  assert(capture_set.size() == 2);

  store.clear();
  assert(!store.get_latest_stream_result(20));
  assert(!store.is_stream_display_demand_active(20, 3'010'000'000ull));

  // mailbox/result independence smoke proxy: result path exists without a sink.
  CoreResultStore no_mailbox_store;
  no_mailbox_store.retain_frame(stream_frame, StreamIntent::PREVIEW, 111, 1, 0, requested_cpu);
  assert(no_mailbox_store.get_latest_stream_result(20));

  std::cout << "PASS core_result_path_smoke\n";
  return 0;
}

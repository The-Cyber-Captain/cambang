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

#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
namespace cambang {
struct CoreResultStoreSmokeAccess {
  static void set_next_retained_frame_id(CoreResultStore& store, uint64_t next_retained_frame_id) {
    store.next_retained_frame_id_ = next_retained_frame_id;
  }
};
} // namespace cambang
#endif

namespace {

FrameView make_cpu_rgba_frame(uint64_t device_instance_id,
                              uint64_t stream_id,
                              uint64_t capture_id,
                              std::vector<uint8_t>& bytes) {
  FrameView frame{};
  frame.device_instance_id = device_instance_id;
  frame.stream_id = stream_id;
  frame.capture_id = capture_id;
  frame.width = 2;
  frame.height = 2;
  frame.format_fourcc = FOURCC_RGBA;
  frame.data = bytes.data();
  frame.size_bytes = bytes.size();
  frame.stride_bytes = 0;
  return frame;
}

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
  const auto reduced_period = TickPeriod::create(10, 4);
  assert(reduced_period);
  assert(reduced_period->numerator_ns() == 5);
  assert(reduced_period->denominator() == 2);
  assert(!TickPeriod::create(0, 1));
  assert(!TickPeriod::create(-1, 1));
  assert(!TickPeriod::create(1, 0));
  assert(!TickPeriod::create(1, -1));
  const auto zero_mark_timing = ImageAcquisitionTiming::create(
      0,
      *tick_period,
      ImageAcquisitionClockDomain::PROVIDER_MONOTONIC,
      ImageAcquisitionReferenceEvent::EXPOSURE_MIDPOINT,
      ImageAcquisitionComparability::SAME_DEVICE);
  const auto max_mark_timing = ImageAcquisitionTiming::create(
      std::numeric_limits<int64_t>::max(),
      *tick_period,
      ImageAcquisitionClockDomain::PROVIDER_MONOTONIC,
      ImageAcquisitionReferenceEvent::EXPOSURE_MIDPOINT,
      ImageAcquisitionComparability::SAME_DEVICE);
  assert(zero_mark_timing);
  assert(max_mark_timing);
  assert(!ImageAcquisitionTiming::create(
      -1,
      *tick_period,
      ImageAcquisitionClockDomain::PROVIDER_MONOTONIC,
      ImageAcquisitionReferenceEvent::EXPOSURE_MIDPOINT,
      ImageAcquisitionComparability::SAME_DEVICE));
  assert(!ImageAcquisitionTiming::create(
      0,
      *tick_period,
      static_cast<ImageAcquisitionClockDomain>(std::numeric_limits<uint8_t>::max()),
      ImageAcquisitionReferenceEvent::EXPOSURE_MIDPOINT,
      ImageAcquisitionComparability::SAME_DEVICE));
  assert(!ImageAcquisitionTiming::create(
      0,
      *tick_period,
      ImageAcquisitionClockDomain::PROVIDER_MONOTONIC,
      static_cast<ImageAcquisitionReferenceEvent>(std::numeric_limits<uint8_t>::max()),
      ImageAcquisitionComparability::SAME_DEVICE));
  assert(!ImageAcquisitionTiming::create(
      0,
      *tick_period,
      ImageAcquisitionClockDomain::PROVIDER_MONOTONIC,
      ImageAcquisitionReferenceEvent::EXPOSURE_MIDPOINT,
      static_cast<ImageAcquisitionComparability>(std::numeric_limits<uint8_t>::max())));
  const auto max_checked_mark = ImageAcquisitionTiming::checked_mark_from_unsigned(
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
  assert(max_checked_mark && *max_checked_mark == std::numeric_limits<int64_t>::max());
  assert(!ImageAcquisitionTiming::checked_mark_from_unsigned(
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1u));
  image.acquisition_timing =
      SourcedFact<ImageAcquisitionTiming>{*zero_mark_timing, FactOrigin::NATIVE_REPORTED};
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
  assert(image.acquisition_timing->value.clock_domain() ==
         ImageAcquisitionClockDomain::PROVIDER_MONOTONIC);
  assert(image.acquisition_timing->value.acquisition_mark() == 0);
  assert(image.acquisition_timing->value.tick_period().numerator_ns() == 1);
  assert(image.acquisition_timing->value.tick_period().denominator() == 1);
  assert(image.acquisition_timing->value.reference_event() ==
         ImageAcquisitionReferenceEvent::EXPOSURE_MIDPOINT);
  assert(image.acquisition_timing->value.comparability() ==
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
  assert(store.retain_frame(stream_frame, StreamIntent::VIEWFINDER, kStreamEpochA, 0, requested_cpu));
  FrameView mismatched_cpu_request = stream_frame;
  mismatched_cpu_request.stream_id = 120;
  mismatched_cpu_request.primary_backing_kind = ProducerBackingKind::GPU;
  mismatched_cpu_request.primary_backing_artifact = std::make_shared<int>(120);
  assert(!store.retain_frame(mismatched_cpu_request, StreamIntent::VIEWFINDER, kStreamEpochA, 0, requested_cpu));
  FrameView provider_echo_only_request = stream_frame;
  provider_echo_only_request.stream_id = 121;
  provider_echo_only_request.requested_retained_plan = requested_cpu;
  assert(!store.retain_frame(provider_echo_only_request, StreamIntent::VIEWFINDER, kStreamEpochA, 0));
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
  assert(stream_result->retained_frame_id != 0);
  assert(!stream_result->image_facts.acquisition_timing);
  const uint64_t cpu_stream_posture_id = stream_result->access_posture.posture_id;

  const auto integral_period = TickPeriod::create(5, 2);
  const auto reducible_period = TickPeriod::create(10, 4);
  assert(integral_period && reducible_period);
  const auto timing = [](int64_t mark, TickPeriod period,
                         ImageAcquisitionClockDomain domain,
                         ImageAcquisitionComparability comparability) {
    const auto created = ImageAcquisitionTiming::create(
        mark,
        period,
        domain,
        ImageAcquisitionReferenceEvent::EXPOSURE_MIDPOINT,
        comparability);
    assert(created);
    return SourcedFact<ImageAcquisitionTiming>{
        *created,
        FactOrigin::NATIVE_REPORTED};
  };
  assert(!TickPeriod::create(1, 0));
  assert(reducible_period->numerator_ns() == 5);
  assert(reducible_period->denominator() == 2);

  stream_frame.acquisition_timing = timing(
      0, *integral_period, ImageAcquisitionClockDomain::DOMAIN_OPAQUE,
      ImageAcquisitionComparability::SAME_IMAGE_ONLY);

  store.retain_frame(stream_frame, StreamIntent::VIEWFINDER, kStreamEpochA, 0, requested_cpu);
  auto repeated_cpu_stream_result = store.get_latest_stream_result(20);
  assert(repeated_cpu_stream_result);
  assert(repeated_cpu_stream_result->access_posture.posture_id == cpu_stream_posture_id);
  assert(repeated_cpu_stream_result->retained_frame_id != stream_result->retained_frame_id);
  assert(repeated_cpu_stream_result->image_facts.acquisition_timing);
  assert(repeated_cpu_stream_result->image_facts.acquisition_timing->value.acquisition_mark() == 0);
  assert(repeated_cpu_stream_result->image_facts.acquisition_timing->value.tick_period().numerator_ns() ==
         integral_period->numerator_ns());
  assert(repeated_cpu_stream_result->image_facts.acquisition_timing->value.tick_period().denominator() ==
         integral_period->denominator());
  assert(repeated_cpu_stream_result->image_facts.acquisition_timing->value.clock_domain() ==
         ImageAcquisitionClockDomain::DOMAIN_OPAQUE);
  assert(repeated_cpu_stream_result->image_facts.acquisition_timing->value.reference_event() ==
         ImageAcquisitionReferenceEvent::EXPOSURE_MIDPOINT);
  assert(repeated_cpu_stream_result->image_facts.acquisition_timing->value.comparability() ==
         ImageAcquisitionComparability::SAME_IMAGE_ONLY);

  store.retain_frame(stream_frame, StreamIntent::VIEWFINDER, kStreamEpochB, 0, requested_cpu);
  auto restarted_cpu_stream_result = store.get_latest_stream_result(20);
  assert(restarted_cpu_stream_result);
  assert(restarted_cpu_stream_result->access_posture.posture_id != cpu_stream_posture_id);

  FrameView gpu_only_stream_frame = stream_frame;
  gpu_only_stream_frame.stream_id = 21;
  gpu_only_stream_frame.primary_backing_kind = ProducerBackingKind::GPU;
  gpu_only_stream_frame.primary_backing_artifact = std::make_shared<int>(42);
  gpu_only_stream_frame.retain_cpu_sidecar = false;
  assert(store.retain_frame(gpu_only_stream_frame, StreamIntent::VIEWFINDER, kStreamEpochA, 0, requested_gpu_no_sidecar));

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
  store.retain_frame(gpu_materializable_stream_frame, StreamIntent::VIEWFINDER, kStreamEpochA, 0, requested_gpu_no_sidecar);

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
  store.retain_frame(gpu_materializable_stream_frame, StreamIntent::VIEWFINDER, kStreamEpochA, 0, requested_gpu_no_sidecar);
  auto repeated_gpu_materializable_stream_result = store.get_latest_stream_result(23);
  assert(repeated_gpu_materializable_stream_result);
  assert(repeated_gpu_materializable_stream_result->access_posture.posture_id == gpu_materializable_posture_id);

  gpu_materializable_stream_frame.retained_gpu_backing_descriptor.materialization_available = false;
  store.retain_frame(gpu_materializable_stream_frame, StreamIntent::VIEWFINDER, kStreamEpochA, 0, requested_gpu_no_sidecar);
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
  assert(store.retain_frame(gpu_stream_frame, StreamIntent::VIEWFINDER, kStreamEpochA, 0, requested_gpu_with_sidecar));

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

  store.retain_frame(stream_frame, StreamIntent::VIEWFINDER, kStreamEpochA, 0, requested_cpu);
  assert(store.get_latest_stream_result(20));
  store.mark_stream_display_demand(20, 3'000'000'000ull);
  assert(store.is_stream_display_demand_active(20, 3'010'000'000ull));

  FrameView capture_a = stream_frame;
  capture_a.stream_id = 0;
  capture_a.capture_id = 77;
  capture_a.device_instance_id = 100;
  assert(store.retain_frame(capture_a, std::nullopt, 0, kCaptureEpochA, {}, requested_cpu));

  FrameView capture_b = stream_frame;
  capture_b.stream_id = 0;
  capture_b.capture_id = 77;
  capture_b.device_instance_id = 101;
  store.retain_frame(capture_b, std::nullopt, 0, kCaptureEpochA, {}, requested_cpu);

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
  store.retain_frame(capture_a_second, std::nullopt, 0, kCaptureEpochA, {}, requested_cpu);
  auto capture_result_second = store.get_capture_result(79, 100);
  assert(capture_result_second);
  assert(capture_result_second->default_image.access_posture.posture_id == capture_default_posture_id);

  FrameView capture_a_reconfigured = capture_a;
  capture_a_reconfigured.capture_id = 80;
  store.retain_frame(capture_a_reconfigured, std::nullopt, 0, kCaptureEpochA + 1, {}, requested_cpu);
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
  assert(store.retain_frame(gpu_capture, std::nullopt, 0, kCaptureEpochA, {}, requested_gpu_no_sidecar));
  FrameView mismatched_capture_request = gpu_capture;
  mismatched_capture_request.capture_id = 178;
  assert(!store.retain_frame(mismatched_capture_request, std::nullopt, 0, kCaptureEpochA, {}, requested_cpu));
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
  const uint64_t default_frame_id_before = capture_result->default_image.retained_frame_id;
  const size_t default_bytes_before = capture_result->default_image.payload.size_bytes();

  CoreCaptureResultData::ImageMemberData bracket{};
  bracket.image_member_index = 1;
  bracket.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
  bracket.payload = capture_result->default_image.payload;
  assert(store.append_additional_capture_image(77, 100, bracket, kCaptureEpochA, requested_cpu));

  auto capture_result_with_bracket = store.get_capture_result(77, 100);
  assert(capture_result_with_bracket);
  assert(capture_result_with_bracket->default_image.retained_frame_id == default_frame_id_before);
  assert(capture_result_with_bracket->default_image.payload.size_bytes() == default_bytes_before);
  assert(capture_result_with_bracket->additional_images.size() == 1);
  assert(capture_result_with_bracket->additional_images[0].retained_frame_id != 0);
  assert(capture_result_with_bracket->additional_images[0].retained_frame_id !=
         capture_result_with_bracket->default_image.retained_frame_id);
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
  bad_role.payload = capture_result_with_bracket->default_image.payload;
  assert(!store.append_additional_capture_image(77, 100, bad_role, kCaptureEpochA, requested_cpu));

  CoreCaptureResultData::ImageMemberData bad_payload{};
  bad_payload.image_member_index = 2;
  bad_payload.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
  bad_payload.payload = CoreResultPayloadCpuPacked{};
  assert(!store.append_additional_capture_image(77, 100, bad_payload, kCaptureEpochA, requested_cpu));
  CoreCaptureResultData::ImageMemberData out_of_order{};
  out_of_order.image_member_index = 3;
  out_of_order.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
  out_of_order.payload = capture_result_with_bracket->default_image.payload;
  assert(!store.append_additional_capture_image(77, 100, out_of_order, kCaptureEpochA, requested_cpu));
  CoreCaptureResultData::ImageMemberData duplicate_index{};
  duplicate_index.image_member_index = 1;
  duplicate_index.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
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
  no_mailbox_store.retain_frame(stream_frame, StreamIntent::PREVIEW, 1, 0, requested_cpu);
  assert(no_mailbox_store.get_latest_stream_result(20));

  {
    CoreResultStore identity_store;
    std::vector<uint8_t> identity_a(2 * 2 * 4, 11);
    std::vector<uint8_t> identity_b(2 * 2 * 4, 12);
    FrameView identical_timing_a = make_cpu_rgba_frame(1, 501, 0, identity_a);
    FrameView identical_timing_b = make_cpu_rgba_frame(1, 502, 0, identity_b);
    identical_timing_a.acquisition_timing = timing(
        77, *integral_period, ImageAcquisitionClockDomain::PROVIDER_MONOTONIC,
        ImageAcquisitionComparability::SAME_DEVICE);
    identical_timing_b.acquisition_timing = identical_timing_a.acquisition_timing;
    assert(identity_store.retain_frame(
        identical_timing_a, StreamIntent::VIEWFINDER, 1, 0, requested_cpu));
    assert(identity_store.retain_frame(
        identical_timing_b, StreamIntent::VIEWFINDER, 1, 0, requested_cpu));
    const auto first = identity_store.get_latest_stream_result(501);
    const auto second = identity_store.get_latest_stream_result(502);
    assert(first && second);
    assert(first->retained_frame_id != 0);
    assert(second->retained_frame_id != 0);
    assert(first->retained_frame_id != second->retained_frame_id);
    assert(first->image_facts.acquisition_timing);
    assert(second->image_facts.acquisition_timing);
    assert(first->image_facts.acquisition_timing->value.acquisition_mark() ==
           second->image_facts.acquisition_timing->value.acquisition_mark());
    assert(first->image_facts.acquisition_timing->value.tick_period().numerator_ns() ==
           second->image_facts.acquisition_timing->value.tick_period().numerator_ns());
    assert(first->image_facts.acquisition_timing->value.tick_period().denominator() ==
           second->image_facts.acquisition_timing->value.tick_period().denominator());
  }

  {
    CoreResultStore shared_identity_store;
    std::vector<uint8_t> dual_bytes(2 * 2 * 4, 13);
    FrameView dual_frame = make_cpu_rgba_frame(2, 601, 701, dual_bytes);
    dual_frame.primary_backing_kind = ProducerBackingKind::GPU;
    dual_frame.primary_backing_artifact = std::make_shared<int>(601701);
    dual_frame.retain_cpu_sidecar = true;
    dual_frame.retained_gpu_backing_descriptor.valid = true;
    dual_frame.retained_gpu_backing_descriptor.materialization_available = true;
    dual_frame.retained_gpu_backing_descriptor.backing_id = 41;
    dual_frame.capture_image.routing = CaptureImageRouting::DEFAULT_METERED;
    dual_frame.capture_image.image_member_index = 0;
    assert(shared_identity_store.retain_frame(
        dual_frame,
        StreamIntent::VIEWFINDER,
        1,
        1,
        requested_gpu_with_sidecar,
        requested_gpu_with_sidecar));
    const auto stream = shared_identity_store.get_latest_stream_result(601);
    const auto capture = shared_identity_store.get_capture_result(701, 2);
    assert(stream && capture);
    assert(stream->retained_frame_id != 0);
    assert(stream->payload_retained_frame_id == stream->retained_frame_id);
    assert(capture->default_image.retained_frame_id == stream->retained_frame_id);
    assert(capture->default_image.payload_kind == ResultPayloadKind::GPU_SURFACE);
    assert(stream->payload_kind == ResultPayloadKind::GPU_SURFACE);
    assert(!stream->payload.empty());
    assert(!capture->default_image.payload.empty());
  }

  {
    CoreResultStore backing_store;
    std::vector<uint8_t> gpu_bytes(2 * 2 * 4, 14);
    FrameView gpu_frame = make_cpu_rgba_frame(3, 801, 0, gpu_bytes);
    gpu_frame.primary_backing_kind = ProducerBackingKind::GPU;
    gpu_frame.primary_backing_artifact = std::make_shared<int>(8001);
    gpu_frame.retain_cpu_sidecar = false;
    gpu_frame.retained_gpu_backing_descriptor.valid = true;
    gpu_frame.retained_gpu_backing_descriptor.materialization_available = true;
    gpu_frame.retained_gpu_backing_descriptor.backing_id = 9001;
    assert(backing_store.retain_frame(
        gpu_frame, StreamIntent::VIEWFINDER, 1, 0, requested_gpu_no_sidecar));
    const auto first = backing_store.get_latest_stream_result(801);
    assert(first);
    gpu_frame.primary_backing_artifact = std::make_shared<int>(8002);
    assert(backing_store.retain_frame(
        gpu_frame, StreamIntent::VIEWFINDER, 1, 0, requested_gpu_no_sidecar));
    const auto second = backing_store.get_latest_stream_result(801);
    assert(second);
    assert(first->retained_gpu_backing_descriptor.backing_id ==
           second->retained_gpu_backing_descriptor.backing_id);
    assert(first->retained_gpu_backing_descriptor.backing_id == 9001);
    assert(first->retained_frame_id != second->retained_frame_id);
  }

  {
    CoreResultStore continuity_store;
    std::vector<uint8_t> continuity_bytes(2 * 2 * 4, 15);
    FrameView first_frame = make_cpu_rgba_frame(4, 901, 0, continuity_bytes);
    assert(continuity_store.retain_frame(
        first_frame, StreamIntent::VIEWFINDER, 1, 0, requested_cpu));
    const auto first = continuity_store.get_latest_stream_result(901);
    assert(first && first->retained_frame_id != 0);
    continuity_store.clear();
    FrameView second_frame = make_cpu_rgba_frame(4, 902, 0, continuity_bytes);
    assert(continuity_store.retain_frame(
        second_frame, StreamIntent::VIEWFINDER, 1, 0, requested_cpu));
    const auto second = continuity_store.get_latest_stream_result(902);
    assert(second);
    assert(second->retained_frame_id == first->retained_frame_id + 1);
  }

  {
    CoreResultStore rejected_stream_store;
    std::vector<uint8_t> stream_bytes(2 * 2 * 4, 16);
    FrameView accepted_stream = make_cpu_rgba_frame(5, 1001, 0, stream_bytes);
    assert(rejected_stream_store.retain_frame(
        accepted_stream, StreamIntent::VIEWFINDER, 1, 0, requested_cpu));
    const auto first = rejected_stream_store.get_latest_stream_result(1001);
    assert(first);
    FrameView rejected_stream = accepted_stream;
    rejected_stream.stream_id = 1002;
    rejected_stream.primary_backing_kind = ProducerBackingKind::GPU;
    rejected_stream.primary_backing_artifact = std::make_shared<int>(1002);
    assert(!rejected_stream_store.retain_frame(
        rejected_stream, StreamIntent::VIEWFINDER, 1, 0, requested_cpu));
    assert(!rejected_stream_store.get_latest_stream_result(1002));
    FrameView accepted_after_rejection = accepted_stream;
    accepted_after_rejection.stream_id = 1003;
    assert(rejected_stream_store.retain_frame(
        accepted_after_rejection, StreamIntent::VIEWFINDER, 1, 0, requested_cpu));
    const auto second = rejected_stream_store.get_latest_stream_result(1003);
    assert(second);
    assert(second->retained_frame_id == first->retained_frame_id + 1);
  }

  {
    CoreResultStore rejected_default_capture_store;
    std::vector<uint8_t> capture_bytes(2 * 2 * 4, 17);
    FrameView accepted_capture = make_cpu_rgba_frame(6, 0, 1101, capture_bytes);
    accepted_capture.capture_image.routing = CaptureImageRouting::DEFAULT_METERED;
    accepted_capture.capture_image.image_member_index = 0;
    assert(rejected_default_capture_store.retain_frame(
        accepted_capture, std::nullopt, 0, 1, {}, requested_cpu));
    const auto first = rejected_default_capture_store.get_capture_result(1101, 6);
    assert(first);
    FrameView rejected_capture = accepted_capture;
    rejected_capture.capture_id = 1102;
    rejected_capture.capture_image.applied_exposure_compensation_milli_ev = 1;
    assert(!rejected_default_capture_store.retain_frame(
        rejected_capture, std::nullopt, 0, 1, {}, requested_cpu));
    assert(!rejected_default_capture_store.get_capture_result(1102, 6));
    FrameView accepted_after_rejection = accepted_capture;
    accepted_after_rejection.capture_id = 1103;
    assert(rejected_default_capture_store.retain_frame(
        accepted_after_rejection, std::nullopt, 0, 1, {}, requested_cpu));
    const auto second = rejected_default_capture_store.get_capture_result(1103, 6);
    assert(second);
    assert(second->default_image.retained_frame_id ==
           first->default_image.retained_frame_id + 1);
  }

  {
    CoreResultStore additional_member_store;
    std::vector<uint8_t> member_bytes(2 * 2 * 4, 18);
    FrameView default_capture = make_cpu_rgba_frame(7, 0, 1201, member_bytes);
    default_capture.capture_image.routing = CaptureImageRouting::DEFAULT_METERED;
    default_capture.capture_image.image_member_index = 0;
    assert(additional_member_store.retain_frame(
        default_capture, std::nullopt, 0, 1, {}, requested_cpu));
    const auto base = additional_member_store.get_capture_result(1201, 7);
    assert(base);
    const uint64_t default_id = base->default_image.retained_frame_id;

    CoreCaptureResultData::ImageMemberData rejected_member{};
    rejected_member.image_member_index = 1;
    rejected_member.role = CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED;
    rejected_member.payload = base->default_image.payload;
    assert(!additional_member_store.append_additional_capture_image(
        1201, 7, rejected_member, 1, requested_cpu));

    CoreCaptureResultData::ImageMemberData member_one{};
    member_one.image_member_index = 1;
    member_one.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
    member_one.payload = base->default_image.payload;
    assert(additional_member_store.append_additional_capture_image(
        1201, 7, member_one, 1, requested_cpu));
    const auto with_member_one = additional_member_store.get_capture_result(1201, 7);
    assert(with_member_one);
    assert(with_member_one->additional_images.size() == 1);
    const uint64_t member_one_id = with_member_one->additional_images[0].retained_frame_id;
    assert(member_one_id != 0);
    assert(member_one_id == default_id + 1);
    assert(member_one_id != default_id);

    CoreCaptureResultData::ImageMemberData rejected_member_two{};
    rejected_member_two.image_member_index = 2;
    rejected_member_two.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
    assert(!additional_member_store.append_additional_capture_image(
        1201, 7, rejected_member_two, 1, requested_cpu));

    CoreCaptureResultData::ImageMemberData member_two{};
    member_two.image_member_index = 2;
    member_two.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
    member_two.payload = base->default_image.payload;
    assert(additional_member_store.append_additional_capture_image(
        1201, 7, member_two, 1, requested_cpu));
    const auto with_member_two = additional_member_store.get_capture_result(1201, 7);
    assert(with_member_two);
    assert(with_member_two->additional_images.size() == 2);
    assert(with_member_two->additional_images[1].retained_frame_id == member_one_id + 1);
  }

#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
  {
    CoreResultStore exhaustion_store;
    CoreResultStoreSmokeAccess::set_next_retained_frame_id(
        exhaustion_store, std::numeric_limits<uint64_t>::max() - 1);
    std::vector<uint8_t> bytes(2 * 2 * 4, 19);
    FrameView first_frame = make_cpu_rgba_frame(8, 1301, 0, bytes);
    FrameView second_frame = make_cpu_rgba_frame(8, 1302, 0, bytes);
    FrameView failed_frame = make_cpu_rgba_frame(8, 1303, 0, bytes);
    assert(exhaustion_store.retain_frame(
        first_frame, StreamIntent::VIEWFINDER, 1, 0, requested_cpu));
    assert(exhaustion_store.retain_frame(
        second_frame, StreamIntent::VIEWFINDER, 1, 0, requested_cpu));
    const auto first = exhaustion_store.get_latest_stream_result(1301);
    const auto second = exhaustion_store.get_latest_stream_result(1302);
    assert(first && second);
    assert(first->retained_frame_id == std::numeric_limits<uint64_t>::max() - 1);
    assert(second->retained_frame_id == std::numeric_limits<uint64_t>::max());
    assert(!exhaustion_store.retain_frame(
        failed_frame, StreamIntent::VIEWFINDER, 1, 0, requested_cpu));
    assert(!exhaustion_store.get_latest_stream_result(1303));
    assert(exhaustion_store.get_latest_stream_result(1301)->retained_frame_id ==
           std::numeric_limits<uint64_t>::max() - 1);
    assert(exhaustion_store.get_latest_stream_result(1302)->retained_frame_id ==
           std::numeric_limits<uint64_t>::max());
  }
#endif

  std::cout << "PASS core_result_path_smoke\n";
  return 0;
}

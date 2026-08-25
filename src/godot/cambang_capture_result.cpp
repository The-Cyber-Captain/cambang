#include "godot/cambang_capture_result.h"

#include "godot/cambang_server.h"
#include "godot/cambang_result_convert.h"
#include "godot/capture_compute_texture.h"
#include "godot/godot_gpu_display_service.h"
#include "godot/result_access_cost_evidence.h"

#include <chrono>
#include <mutex>
#include <type_traits>
#include <variant>

#include <godot_cpp/variant/array.hpp>

namespace cambang {

namespace {

// Capture materialisation is reached from two places, and the access-cost
// evidence records both under one route, which hid the fact that every member
// is converted twice: once by the application through to_image_member(), and
// once by calibrate_capture_result() probing the real cost so it can classify
// the access CHEAP vs EXPENSIVE.
//
// That was invisible and free while capture payloads were packed (~1.3ms per
// conversion). With planar payloads it is ~24ms, so the probe alone accounted
// for ~5s of Godot-thread work in an 87s soak. Splitting the counts is what
// made it findable; keeping them split is what will make a regression here
// findable again.
std::mutex g_capture_materialization_counter_mutex;
uint64_t g_capture_materializations_application = 0;
uint64_t g_capture_materializations_calibration = 0;

void note_capture_materialization(bool calibration) {
  std::lock_guard<std::mutex> lock(g_capture_materialization_counter_mutex);
  if (calibration) {
    ++g_capture_materializations_calibration;
  } else {
    ++g_capture_materializations_application;
  }
}
uint64_t result_access_now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Usable by a capture access path that may convert. Delegates to the shared
// predicate rather than re-deriving the format rule: this was the seventh
// place in this area deciding independently what "usable CPU payload" means,
// and every previous one drifted from the capability that reports it.
bool capture_member_has_cpu_payload(const CoreCaptureResultData::ImageMemberData& member) {
  return member.payload.width != 0 && member.payload.height != 0 &&
         retained_cpu_payload_is_convertible(member.payload);
}

// Directly readable packed bytes, for evidence routing that distinguishes a
// straight copy from a full-frame conversion.
bool capture_member_has_packed_cpu_payload(const CoreCaptureResultData::ImageMemberData& member) {
  return retained_cpu_payload_is_packed_readable(member.payload);
}

const char* capture_to_image_evidence_route(const CoreCaptureResultData::ImageMemberData* member) {
  if (!member) return result_access_cost_evidence::kRouteCaptureAccessUnsupported;
  if (capture_member_has_packed_cpu_payload(*member)) {
    if (member->payload_kind == ResultPayloadKind::GPU_SURFACE) {
      return result_access_cost_evidence::kRouteCaptureToImageGpuPrimaryCpuSidecar;
    }
    return result_access_cost_evidence::kRouteCaptureToImageCpuPacked;
  }
  if (capture_member_has_cpu_payload(*member)) {
    return result_access_cost_evidence::kRouteCaptureToImageCpuPlanarConvert;
  }
  if (member->payload_kind == ResultPayloadKind::GPU_SURFACE && member->retained_gpu_backing &&
      member->retained_gpu_backing_descriptor.valid &&
      member->retained_gpu_backing_descriptor.materialization_available) {
    return result_access_cost_evidence::kRouteCaptureToImageGpuPrimaryNoCpuSidecarMaterializer;
  }
  return result_access_cost_evidence::kRouteCaptureAccessUnsupported;
}

const char* capture_gpu_materializer_evidence_route_for_posture(
    const CoreCaptureResultData::ImageMemberData* member) {
  if (!member) {
    return result_access_cost_evidence::kRouteCaptureAccessUnsupported;
  }
  return member->access_posture.has_retained_cpu_payload
      ? result_access_cost_evidence::kRouteCaptureToImageGpuPrimaryCpuSidecarMaterializer
      : result_access_cost_evidence::kRouteCaptureToImageGpuPrimaryNoCpuSidecarMaterializer;
}

const char* color_range_name(ColorRange value) {
  switch (value) {
    case ColorRange::LIMITED: return "limited";
    case ColorRange::FULL: return "full";
    case ColorRange::UNSPECIFIED: return "unspecified";
  }
  return "unspecified";
}

const char* color_matrix_name(ColorMatrix value) {
  switch (value) {
    case ColorMatrix::BT601: return "bt601";
    case ColorMatrix::BT709: return "bt709";
    case ColorMatrix::BT2020_NCL: return "bt2020_ncl";
    case ColorMatrix::UNSPECIFIED: return "unspecified";
  }
  return "unspecified";
}

const char* color_transfer_name(ColorTransfer value) {
  switch (value) {
    case ColorTransfer::BT709: return "bt709";
    case ColorTransfer::SRGB: return "srgb";
    case ColorTransfer::PQ: return "pq";
    case ColorTransfer::HLG: return "hlg";
    case ColorTransfer::UNSPECIFIED: return "unspecified";
  }
  return "unspecified";
}

const char* color_primaries_name(ColorPrimaries value) {
  switch (value) {
    case ColorPrimaries::BT709: return "bt709";
    case ColorPrimaries::BT2020: return "bt2020";
    case ColorPrimaries::P3_D65: return "p3_d65";
    case ColorPrimaries::UNSPECIFIED: return "unspecified";
  }
  return "unspecified";
}

// Colour interpretation of a member's retained payload, reported verbatim.
//
// "unspecified" is truthful absence, not a value -- the same stance the
// provider contract takes on PayloadColorimetry. Nothing here substitutes a
// default, because a caller writing its own Y'CbCr maths needs to know whether
// CamBANG was told the colour space or is simply unaware of it. What CamBANG's
// own CPU conversion assumes when told nothing is documented in
// pixel_payload_and_result_contract.md 11.6.1; it is deliberately not reported
// here as though the provider had declared it.
godot::Dictionary colorimetry_to_dict(const PayloadColorimetry& colorimetry) {
  godot::Dictionary out;
  out["range"] = godot::String(color_range_name(colorimetry.range));
  out["matrix"] = godot::String(color_matrix_name(colorimetry.matrix));
  out["transfer"] = godot::String(color_transfer_name(colorimetry.transfer));
  out["primaries"] = godot::String(color_primaries_name(colorimetry.primaries));
  out["declared"] = colorimetry.range != ColorRange::UNSPECIFIED ||
                    colorimetry.matrix != ColorMatrix::UNSPECIFIED ||
                    colorimetry.transfer != ColorTransfer::UNSPECIFIED ||
                    colorimetry.primaries != ColorPrimaries::UNSPECIFIED;
  return out;
}

const char* fact_origin_name(FactOrigin origin) {
  switch (origin) {
    case FactOrigin::NATIVE_REPORTED: return "native_reported";
    case FactOrigin::USER_SUPPLIED: return "user_supplied";
    case FactOrigin::DERIVED: return "derived";
    case FactOrigin::VIRTUAL_CAMERA_AUTHORED: return "virtual_camera_authored";
    case FactOrigin::RUNTIME_INJECTED: return "runtime_injected";
    case FactOrigin::CORE_DERIVED: return "core_derived";
    case FactOrigin::UNKNOWN: return "unknown";
  }
  return "unknown";
}

const char* camera_facing_name(CameraFacing value) {
  switch (value) {
    case CameraFacing::FRONT: return "front";
    case CameraFacing::BACK: return "back";
    case CameraFacing::EXTERNAL: return "external";
    case CameraFacing::UNKNOWN: return "unknown";
  }
  return "unknown";
}

const char* camera_nature_name(CameraNature value) {
  switch (value) {
    case CameraNature::PHYSICAL: return "physical";
    case CameraNature::VIRTUAL: return "virtual";
    case CameraNature::HYBRID: return "hybrid";
    case CameraNature::UNKNOWN: return "unknown";
  }
  return "unknown";
}

const char* coordinate_domain_name(const CoordinateDomain& domain) {
  if (std::holds_alternative<CoordinateDomainAndroidSensorPreCorrectionActiveArray>(domain)) {
    return "android_sensor_pre_correction_active_array";
  }
  if (std::holds_alternative<CoordinateDomainAndroidSensorActiveArray>(domain)) {
    return "android_sensor_active_array";
  }
  if (std::holds_alternative<CoordinateDomainDeliveredImage>(domain)) {
    return "delivered_image";
  }
  return "platform_defined";
}

void add_coordinate_domain(godot::Dictionary& out, const CoordinateDomain& domain) {
  out["coordinate_domain"] = godot::String(coordinate_domain_name(domain));
  if (const auto* platform = std::get_if<CoordinateDomainPlatformDefined>(&domain)) {
    out["platform_defined_coordinate_domain"] = godot::String(platform->token().c_str());
  }
}

godot::Array to_array(const Vec3Meters& value) {
  godot::Array out;
  out.push_back(value.x);
  out.push_back(value.y);
  out.push_back(value.z);
  return out;
}

godot::Array to_array(const QuaternionXyzw& value) {
  godot::Array out;
  out.push_back(value.x);
  out.push_back(value.y);
  out.push_back(value.z);
  out.push_back(value.w);
  return out;
}

godot::Dictionary to_dict(const SourcedFact<Intrinsics>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  out["focal_length_x_px"] = fact.value.focal_length_x_px();
  out["focal_length_y_px"] = fact.value.focal_length_y_px();
  out["principal_point_x_px"] = fact.value.principal_point_x_px();
  out["principal_point_y_px"] = fact.value.principal_point_y_px();
  if (fact.value.skew_px()) out["skew_px"] = *fact.value.skew_px();
  out["reference_width_px"] = static_cast<int64_t>(fact.value.reference_width_px());
  out["reference_height_px"] = static_cast<int64_t>(fact.value.reference_height_px());
  add_coordinate_domain(out, fact.value.coordinate_domain());
  return out;
}

godot::Dictionary to_dict(const SourcedFact<Distortion>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  if (const auto* brown = std::get_if<BrownConrady5Distortion>(&fact.value)) {
    out["model"] = "brown_conrady_5";
    out["radial_k1"] = brown->radial_k1();
    out["radial_k2"] = brown->radial_k2();
    out["radial_k3"] = brown->radial_k3();
    out["tangential_p1"] = brown->tangential_p1();
    out["tangential_p2"] = brown->tangential_p2();
    out["reference_width_px"] = static_cast<int64_t>(brown->reference_width_px());
    out["reference_height_px"] = static_cast<int64_t>(brown->reference_height_px());
    add_coordinate_domain(out, brown->coordinate_domain());
    out["image_state"] = brown->image_state() == DistortionImageState::DISTORTED ? "distorted" :
        brown->image_state() == DistortionImageState::RECTIFIED ? "rectified" : "unknown";
  } else {
    const auto& none = std::get<NoDistortion>(fact.value);
    out["model"] = "none";
    out["image_state"] = none.image_state == DistortionImageState::DISTORTED ? "distorted" :
        none.image_state == DistortionImageState::RECTIFIED ? "rectified" : "unknown";
  }
  return out;
}

godot::Dictionary to_dict(const SourcedFact<CameraPose>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  const PoseReference& reference = fact.value.reference();
  if (const auto* camera = std::get_if<PoseReferenceCamera>(&reference)) {
    out["reference_kind"] = "camera";
    out["reference_camera_id"] = godot::String(camera->camera_id().c_str());
  } else if (std::holds_alternative<PoseReferencePrimaryCamera>(reference)) {
    out["reference_kind"] = "primary_camera";
  } else if (std::holds_alternative<PoseReferenceDeviceMotionSensor>(reference)) {
    out["reference_kind"] = "device_motion_sensor";
  } else if (std::holds_alternative<PoseReferenceAutomotive>(reference)) {
    out["reference_kind"] = "automotive";
  } else if (const auto* custom = std::get_if<PoseReferenceCustom>(&reference)) {
    out["reference_kind"] = "custom_reference";
    out["reference_id"] = godot::String(custom->reference_id().c_str());
  } else if (const auto* platform = std::get_if<PoseReferencePlatformDefined>(&reference)) {
    out["reference_kind"] = "platform_defined";
    out["platform_defined_reference"] = godot::String(platform->reference_token().c_str());
  } else {
    out["reference_kind"] = "unknown";
  }
  const PoseConvention& convention = fact.value.convention();
  if (std::holds_alternative<PoseConventionAndroidCamera2>(convention)) {
    out["coordinate_convention"] = "android_camera2";
  } else if (std::holds_alternative<PoseConventionCameraOpticalFrame>(convention)) {
    out["coordinate_convention"] = "camera_optical_frame";
  } else {
    out["coordinate_convention"] = "platform_defined";
    out["platform_defined_convention"] = godot::String(
        std::get<PoseConventionPlatformDefined>(convention).convention_token().c_str());
  }
  out["translation_m"] = to_array(fact.value.translation_m());
  out["rotation_xyzw"] = to_array(fact.value.rotation_xyzw());
  return out;
}

godot::Dictionary to_dict(const SourcedFact<FocusState>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  std::visit(
      [&out](const auto& focus) {
        using T = std::decay_t<decltype(focus)>;
        if constexpr (std::is_same_v<T, FocusAtDistance>) {
          out["state"] = "at_distance";
          out["distance_m"] = focus.distance_m();
        } else if constexpr (std::is_same_v<T, FocusAtInfinity>) {
          out["state"] = "infinity";
        } else {
          out["state"] = "unknown";
        }
      },
      fact.value);
  return out;
}

godot::Dictionary to_dict(const SourcedFact<ExposureTime>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  out["nanoseconds"] = fact.value.nanoseconds();
  return out;
}

godot::Dictionary to_dict(const SourcedFact<SensorSensitivityIso>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  out["iso_equivalent"] = fact.value.iso_equivalent();
  return out;
}

godot::Dictionary to_dict(const SourcedFact<ApertureFNumber>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  out["f_number"] = fact.value.f_number();
  return out;
}

godot::Dictionary to_dict(const SourcedFact<FocalLengthMm>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  out["millimetres"] = fact.value.millimetres();
  return out;
}

godot::Dictionary to_dict(const SourcedFact<RealizedImageTransform>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  out["rotation_degrees"] = static_cast<int64_t>(fact.value.rotation);
  out["mirrored"] = fact.value.mirrored;
  out["pixels_already_transformed"] = fact.value.pixels_already_transformed;
  return out;
}

godot::Dictionary camera_facts_to_dict(const CoreResolvedCaptureImageFacts& facts) {
  godot::Dictionary out;
  if (facts.camera.facing) {
    godot::Dictionary value;
    value["value"] = godot::String(camera_facing_name(facts.camera.facing->value));
    value["origin"] = godot::String(fact_origin_name(facts.camera.facing->origin));
    out["facing"] = value;
  }
  if (facts.camera.nature) {
    godot::Dictionary value;
    value["value"] = godot::String(camera_nature_name(facts.camera.nature->value));
    value["origin"] = godot::String(fact_origin_name(facts.camera.nature->origin));
    out["camera_nature"] = value;
  }
  if (facts.camera.sensor_orientation) {
    godot::Dictionary value;
    value["value"] = static_cast<int64_t>(facts.camera.sensor_orientation->value);
    value["origin"] = godot::String(fact_origin_name(facts.camera.sensor_orientation->origin));
    out["sensor_orientation_degrees"] = value;
  }
  if (facts.camera.intrinsics) out["intrinsics"] = to_dict(*facts.camera.intrinsics);
  if (facts.camera.distortion) out["distortion"] = to_dict(*facts.camera.distortion);
  if (facts.camera.pose) out["pose"] = to_dict(*facts.camera.pose);
  add_acquisition_timing_camera_fact(out, facts.image.acquisition_timing);
  if (facts.image.focus_state) out["focus_state"] = to_dict(*facts.image.focus_state);
  if (facts.image.exposure_time) out["exposure_time"] = to_dict(*facts.image.exposure_time);
  if (facts.image.sensor_sensitivity_iso) {
    out["sensor_sensitivity_iso"] = to_dict(*facts.image.sensor_sensitivity_iso);
  }
  if (facts.image.aperture_f_number) {
    out["aperture_f_number"] = to_dict(*facts.image.aperture_f_number);
  }
  if (facts.image.focal_length_mm) {
    out["focal_length_mm"] = to_dict(*facts.image.focal_length_mm);
  }
  if (facts.image.realized_image_transform) {
    out["realized_image_transform"] = to_dict(*facts.image.realized_image_transform);
  }
  return out;
}
} // namespace

uint32_t CamBANGCaptureResult::get_width() const { return data_ ? data_->image_width : 0; }
uint32_t CamBANGCaptureResult::get_height() const { return data_ ? data_->image_height : 0; }
uint32_t CamBANGCaptureResult::get_format() const { return data_ ? data_->image_format_fourcc : 0; }
int CamBANGCaptureResult::get_payload_kind() const {
  return data_ ? static_cast<int>(data_->payload_kind) : static_cast<int>(ResultPayloadKind::CPU_PACKED);
}
int64_t CamBANGCaptureResult::get_capture_datetime_unix_nanoseconds() const {
  return data_ && data_->has_admission_context
      ? data_->admission_context.capture_date_time.unix_epoch_nanoseconds()
      : 0;
}
uint64_t CamBANGCaptureResult::get_device_instance_id() const { return data_ ? data_->device_instance_id : 0; }
godot::Dictionary CamBANGCaptureResult::get_capture_identity() const {
  godot::Dictionary out;
  const uint64_t device_capture_id = data_ ? data_->capture_id : 0;

  // Every key present in every case, including the empty one: a caller that
  // has to test for a key's existence before reading it has two shapes to
  // handle, and the second one only shows up in the field.
  out["capture_origin"] = CAPTURE_ORIGIN_DEVICE;
  out["device_capture_id"] =
      server_ ? server_->device_capture_public_id(device_capture_id) : godot::String();
  out["rig_capture_id"] = godot::String();
  out["rig_member_hardware_id"] = godot::String();
  out["rig_member_index"] = -1;
  out["device_instance_id"] =
      static_cast<uint64_t>(data_ ? data_->device_instance_id : 0);
  if (!server_ || device_capture_id == 0) {
    return out;
  }

  const auto participation = server_->rig_participation_for_device_capture(device_capture_id);
  if (!participation) {
    // No cohort claims this capture, so it was device-triggered. The absence
    // IS the answer; it is not a failed lookup to be reported.
    return out;
  }
  out["capture_origin"] = CAPTURE_ORIGIN_RIG;
  out["rig_capture_id"] = server_->rig_capture_public_id(participation->rig_capture_id);
  out["rig_member_hardware_id"] = godot::String(participation->hardware_id.c_str());
  out["rig_member_index"] = static_cast<int64_t>(participation->member_index);
  return out;
}

bool CamBANGCaptureResult::has_geolocation() const {
  return data_ && data_->has_admission_context && data_->admission_context.geolocation.has_value();
}
godot::Dictionary CamBANGCaptureResult::get_geolocation() const {
  if (!has_geolocation()) return godot::Dictionary();
  const CaptureGeolocation& location = *data_->admission_context.geolocation;
  godot::Dictionary out;
  out["latitude_degrees"] = location.latitude_degrees();
  out["longitude_degrees"] = location.longitude_degrees();
  if (location.altitude_meters()) out["altitude_meters"] = *location.altitude_meters();
  return out;
}

bool CamBANGCaptureResult::has_image_properties() const { return data_ && data_->facts.has_image_properties; }

godot::Dictionary CamBANGCaptureResult::get_image_properties() const {
  return has_image_properties() ? to_dict(data_->facts.image_properties) : godot::Dictionary();
}

godot::Dictionary CamBANGCaptureResult::get_image_properties_provenance() const {
  return has_image_properties() ? to_dict(data_->facts.image_properties_provenance) : godot::Dictionary();
}

int CamBANGCaptureResult::can_get_display_view() const {
  if (!data_) {
    return CAPABILITY_UNSUPPORTED;
  }
  return static_cast<int>(resolve_result_access_classification(
      data_->default_image.retained_access_truth.display_view,
      data_->default_image.access_classification,
      CoreResultAccessOperation::DISPLAY_VIEW));
}

int CamBANGCaptureResult::can_to_image() const {
  // CaptureResult.can_to_image() delegates to
  // CaptureResult.can_to_image_member(index). These are capability/cost
  // classification APIs, not readiness/progress APIs. Future lower-level
  // materialization infrastructure can be shared with StreamResult.to_image(),
  // while CaptureResult.to_image() and
  // CaptureResult.to_image_member(index) must retain capture/member identity
  // validation.
  return can_to_image_member(0);
}

int CamBANGCaptureResult::get_image_count() const {
  return data_ ? static_cast<int>(data_->image_member_count()) : 0;
}

bool CamBANGCaptureResult::has_additional_images() const {
  return data_ && data_->has_additional_images();
}

godot::Dictionary CamBANGCaptureResult::get_image_member(int image_member_index) const {
  if (!data_ || image_member_index < 0) {
    return godot::Dictionary();
  }
  const auto* member = data_->image_member_at(static_cast<uint32_t>(image_member_index));
  if (!member) {
    return godot::Dictionary();
  }
  godot::Dictionary out;
  const int role = static_cast<int>(member->role);
  out["image_member_index"] = static_cast<int64_t>(member->image_member_index);
  out["role"] = role;
  out["role_name"] = (member->role == CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED)
      ? godot::String("DEFAULT_METERED")
      : godot::String("ADDITIONAL_BRACKET");
  out["applied_exposure_compensation_milli_ev"] = static_cast<int64_t>(member->applied_exposure_compensation_milli_ev);
  out["has_realized_exposure_compensation_milli_ev"] = member->has_realized_exposure_compensation_milli_ev;
  out["realized_exposure_compensation_milli_ev"] = static_cast<int64_t>(member->realized_exposure_compensation_milli_ev);
  out["is_default"] = (member->role == CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED);
  out["is_additional_bracket"] = (member->role == CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET);
  const godot::Dictionary camera_facts = camera_facts_to_dict(member->resolved_image_facts);
  if (!camera_facts.is_empty()) out["camera_facts"] = camera_facts;
  // Colour interpretation of this member's planes. Present whenever the member
  // retains CPU bytes, because a caller reading compute-texture planes needs it
  // to write correct Y'CbCr maths, and absence would be indistinguishable from
  // "unspecified".
  if (!member->payload.empty()) {
    out["colorimetry"] = colorimetry_to_dict(member->payload.colorimetry);
  }
  return out;
}

int CamBANGCaptureResult::can_to_image_member(int image_member_index) const {
  if (!data_ || image_member_index < 0) {
    return CAPABILITY_UNSUPPORTED;
  }
  const auto* member = data_->image_member_at(static_cast<uint32_t>(image_member_index));
  if (!member) {
    return CAPABILITY_UNSUPPORTED;
  }
  return static_cast<int>(resolve_result_access_classification(
      member->retained_access_truth.to_image,
      member->access_classification,
      CoreResultAccessOperation::TO_IMAGE));
}

godot::Ref<godot::Image> perform_capture_to_image_member_access(const SharedCaptureResultData& data, int image_member_index) {
  if (!data || image_member_index < 0) {
    const uint64_t begin_ns = result_access_now_ns();
    godot::Ref<godot::Image> image;
    result_access_cost_evidence::record_capture_member_access(
        result_access_cost_evidence::kRouteCaptureAccessUnsupported,
        data,
        nullptr,
        result_access_now_ns() - begin_ns,
        false,
        ResultCapability::UNSUPPORTED);
    return image;
  }
  const auto* member = data->image_member_at(static_cast<uint32_t>(image_member_index));
  if (!member) {
    const uint64_t begin_ns = result_access_now_ns();
    godot::Ref<godot::Image> image;
    result_access_cost_evidence::record_capture_member_access(
        result_access_cost_evidence::kRouteCaptureAccessUnsupported,
        data,
        nullptr,
        result_access_now_ns() - begin_ns,
        false,
        ResultCapability::UNSUPPORTED);
    return image;
  }
  const char* evidence_route = capture_to_image_evidence_route(member);
  const ResultCapability reported_capability = resolve_result_access_classification(
      member->retained_access_truth.to_image,
      member->access_classification,
      CoreResultAccessOperation::TO_IMAGE);
  const uint64_t begin_ns = result_access_now_ns();
  godot::Ref<godot::Image> image;
  if (capture_member_has_cpu_payload(*member)) {
    note_capture_materialization(/*calibration=*/false);
    image = payload_to_image(member->payload);
  } else if (member->payload_kind == ResultPayloadKind::GPU_SURFACE &&
             member->retained_gpu_backing) {
    image = godot_gpu_display_materialize_to_image(
        member->retained_gpu_backing_descriptor,
        member->retained_gpu_backing);
  }
  result_access_cost_evidence::record_capture_member_access(
      evidence_route,
      data,
      member,
      result_access_now_ns() - begin_ns,
      image.is_valid(),
      reported_capability);
  return image;
}

godot::Ref<godot::Image> perform_capture_to_image_member_cpu_payload_access(
    const SharedCaptureResultData& data,
    int image_member_index) {
  const uint64_t begin_ns = result_access_now_ns();
  godot::Ref<godot::Image> image;
  if (!data || image_member_index < 0) {
    result_access_cost_evidence::record_capture_member_access(
        result_access_cost_evidence::kRouteCaptureAccessUnsupported,
        data,
        nullptr,
        result_access_now_ns() - begin_ns,
        false,
        ResultCapability::UNSUPPORTED);
    return image;
  }
  const auto* member = data->image_member_at(static_cast<uint32_t>(image_member_index));
  if (!member || !capture_member_has_cpu_payload(*member)) {
    result_access_cost_evidence::record_capture_member_access(
        result_access_cost_evidence::kRouteCaptureAccessUnsupported,
        data,
        member,
        result_access_now_ns() - begin_ns,
        false,
        member ? member->retained_access_truth.to_image : ResultCapability::UNSUPPORTED);
    return image;
  }
  note_capture_materialization(/*calibration=*/true);
  image = payload_to_image(member->payload);
  result_access_cost_evidence::record_capture_member_access(
      capture_to_image_evidence_route(member),
      data,
      member,
      result_access_now_ns() - begin_ns,
      image.is_valid(),
      member->retained_access_truth.to_image);
  return image;
}

godot::Ref<godot::Image> perform_capture_to_image_member_gpu_materializer_access(
    const SharedCaptureResultData& data,
    int image_member_index) {
  const uint64_t begin_ns = result_access_now_ns();
  godot::Ref<godot::Image> image;
  if (!data || image_member_index < 0) {
    result_access_cost_evidence::record_capture_member_access(
        result_access_cost_evidence::kRouteCaptureAccessUnsupported,
        data,
        nullptr,
        result_access_now_ns() - begin_ns,
        false,
        ResultCapability::UNSUPPORTED);
    return image;
  }
  const auto* member = data->image_member_at(static_cast<uint32_t>(image_member_index));
  const char* route = capture_gpu_materializer_evidence_route_for_posture(member);
  if (!member ||
      member->payload_kind != ResultPayloadKind::GPU_SURFACE ||
      !member->retained_gpu_backing ||
      !member->retained_gpu_backing_descriptor.valid ||
      !member->retained_gpu_backing_descriptor.materialization_available) {
    result_access_cost_evidence::record_capture_member_access(
        result_access_cost_evidence::kRouteCaptureAccessUnsupported,
        data,
        member,
        result_access_now_ns() - begin_ns,
        false,
        member ? member->retained_access_truth.to_image : ResultCapability::UNSUPPORTED);
    return image;
  }
  image = godot_gpu_display_materialize_to_image(
      member->retained_gpu_backing_descriptor,
      member->retained_gpu_backing);
  result_access_cost_evidence::record_capture_member_access(
      route,
      data,
      member,
      result_access_now_ns() - begin_ns,
      image.is_valid(),
      member->retained_access_truth.to_image);
  return image;
}

godot::Ref<godot::Image> CamBANGCaptureResult::to_image_member(int image_member_index) const {
  godot::Ref<godot::Image> image =
      perform_capture_to_image_member_access(data_, image_member_index);
  if (server_ && data_ && image_member_index >= 0) {
    server_->report_capture_result_member_observation(
        data_, static_cast<uint32_t>(image_member_index));
  }
  return image;
}

godot::Ref<godot::Image> CamBANGCaptureResult::calibrate_to_image_member_for_retained_access(
    const SharedCaptureResultData& data,
    uint32_t image_member_index) {
  return perform_capture_to_image_member_access(data, static_cast<int>(image_member_index));
}

godot::Ref<godot::Image> CamBANGCaptureResult::calibrate_to_image_member_cpu_payload_for_retained_access(
    const SharedCaptureResultData& data,
    uint32_t image_member_index) {
  return perform_capture_to_image_member_cpu_payload_access(data, static_cast<int>(image_member_index));
}

godot::Ref<godot::Image> CamBANGCaptureResult::calibrate_to_image_member_gpu_materializer_for_retained_access(
    const SharedCaptureResultData& data,
    uint32_t image_member_index) {
  return perform_capture_to_image_member_gpu_materializer_access(data, static_cast<int>(image_member_index));
}

int CamBANGCaptureResult::can_get_encoded_bytes() const {
  if (!data_) {
    return CAPABILITY_UNSUPPORTED;
  }
  return static_cast<int>(resolve_result_access_classification(
      data_->default_image.retained_access_truth.encoded_bytes,
      data_->default_image.access_classification,
      CoreResultAccessOperation::ENCODED_BYTES));
}

int CamBANGCaptureResult::can_get_compute_texture_member(int image_member_index) const {
  if (image_member_index < 0) {
    return CAPABILITY_UNSUPPORTED;
  }
  return static_cast<int>(capture_compute_texture_support(
      data_, static_cast<uint32_t>(image_member_index)));
}

int CamBANGCaptureResult::get_compute_texture_plane_count(int image_member_index) const {
  if (image_member_index < 0) {
    return 0;
  }
  return static_cast<int>(capture_compute_texture_plane_count(
      data_, static_cast<uint32_t>(image_member_index)));
}

godot::Ref<godot::Texture2D> CamBANGCaptureResult::get_compute_texture_plane(
    int image_member_index,
    int plane_index) const {
  if (image_member_index < 0 || plane_index < 0) {
    return {};
  }
  return capture_compute_texture_plane(
      data_,
      static_cast<uint32_t>(image_member_index),
      static_cast<uint32_t>(plane_index));
}

godot::Variant CamBANGCaptureResult::get_display_view() const {
  return to_image();
}

godot::Ref<godot::Image> CamBANGCaptureResult::to_image() const {
  return to_image_member(0);
}

godot::PackedByteArray CamBANGCaptureResult::get_encoded_bytes() const {
  return godot::PackedByteArray();
}

void CamBANGCaptureResult::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_width"), &CamBANGCaptureResult::get_width);
  godot::ClassDB::bind_method(godot::D_METHOD("get_height"), &CamBANGCaptureResult::get_height);
  godot::ClassDB::bind_method(godot::D_METHOD("get_format"), &CamBANGCaptureResult::get_format);
  godot::ClassDB::bind_method(godot::D_METHOD("get_payload_kind"), &CamBANGCaptureResult::get_payload_kind);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_datetime_unix_nanoseconds"), &CamBANGCaptureResult::get_capture_datetime_unix_nanoseconds);
  godot::ClassDB::bind_method(godot::D_METHOD("get_device_instance_id"), &CamBANGCaptureResult::get_device_instance_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_identity"), &CamBANGCaptureResult::get_capture_identity);
  godot::ClassDB::bind_method(godot::D_METHOD("has_geolocation"), &CamBANGCaptureResult::has_geolocation);
  godot::ClassDB::bind_method(godot::D_METHOD("get_geolocation"), &CamBANGCaptureResult::get_geolocation);

  godot::ClassDB::bind_method(godot::D_METHOD("has_image_properties"), &CamBANGCaptureResult::has_image_properties);

  godot::ClassDB::bind_method(godot::D_METHOD("get_image_properties"), &CamBANGCaptureResult::get_image_properties);

  godot::ClassDB::bind_method(godot::D_METHOD("get_image_properties_provenance"), &CamBANGCaptureResult::get_image_properties_provenance);

  godot::ClassDB::bind_method(godot::D_METHOD("can_get_display_view"), &CamBANGCaptureResult::can_get_display_view);
  godot::ClassDB::bind_method(godot::D_METHOD("can_to_image"), &CamBANGCaptureResult::can_to_image);
  godot::ClassDB::bind_method(godot::D_METHOD("get_image_count"), &CamBANGCaptureResult::get_image_count);
  godot::ClassDB::bind_method(godot::D_METHOD("has_additional_images"), &CamBANGCaptureResult::has_additional_images);
  godot::ClassDB::bind_method(godot::D_METHOD("get_image_member", "image_member_index"), &CamBANGCaptureResult::get_image_member);
  godot::ClassDB::bind_method(godot::D_METHOD("can_to_image_member", "image_member_index"), &CamBANGCaptureResult::can_to_image_member);
  godot::ClassDB::bind_method(godot::D_METHOD("to_image_member", "image_member_index"), &CamBANGCaptureResult::to_image_member);
  godot::ClassDB::bind_method(godot::D_METHOD("can_get_encoded_bytes"), &CamBANGCaptureResult::can_get_encoded_bytes);

  godot::ClassDB::bind_method(godot::D_METHOD("can_get_compute_texture_member", "image_member_index"), &CamBANGCaptureResult::can_get_compute_texture_member);
  godot::ClassDB::bind_method(godot::D_METHOD("get_compute_texture_plane_count", "image_member_index"), &CamBANGCaptureResult::get_compute_texture_plane_count);
  godot::ClassDB::bind_method(godot::D_METHOD("get_compute_texture_plane", "image_member_index", "plane_index"), &CamBANGCaptureResult::get_compute_texture_plane);

  godot::ClassDB::bind_method(godot::D_METHOD("get_display_view"), &CamBANGCaptureResult::get_display_view);
  godot::ClassDB::bind_method(godot::D_METHOD("to_image"), &CamBANGCaptureResult::to_image);
  godot::ClassDB::bind_method(godot::D_METHOD("get_encoded_bytes"), &CamBANGCaptureResult::get_encoded_bytes);

  BIND_CONSTANT(CAPABILITY_READY);
  BIND_CONSTANT(CAPABILITY_CHEAP);
  BIND_CONSTANT(CAPABILITY_EXPENSIVE);
  BIND_CONSTANT(CAPABILITY_UNSUPPORTED);
  BIND_CONSTANT(CAPTURE_ORIGIN_DEVICE);
  BIND_CONSTANT(CAPTURE_ORIGIN_RIG);
  BIND_CONSTANT(IMAGE_ROLE_DEFAULT_METERED);
  BIND_CONSTANT(IMAGE_ROLE_ADDITIONAL_BRACKET);
}

godot::Dictionary capture_materialization_stats() {
  std::lock_guard<std::mutex> lock(g_capture_materialization_counter_mutex);
  godot::Dictionary d;
  d["application"] = static_cast<uint64_t>(g_capture_materializations_application);
  d["calibration"] = static_cast<uint64_t>(g_capture_materializations_calibration);
  return d;
}

} // namespace cambang

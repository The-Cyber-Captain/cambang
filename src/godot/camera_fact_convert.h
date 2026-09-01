#pragma once

// Camera-fact -> Godot Dictionary conversion of the device-scoped facts,
// shared by CamBANGCaptureResult and CamBANGStreamResult.
//
// Shared rather than duplicated for the reason 11.2 of the payload contract
// already had to fix once: two independently re-derived projections of the same
// fact drift, and a caller then sees the same camera described two ways
// depending on which surface it asked. Origin travels with every value here,
// because camera_fact_model.md 8 requires the public projection to expose fact
// origin -- a value without its origin cannot be told from an ingested override.

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include "core/camera_fact_types.h"
#include "core/core_result_store.h"
#include "godot/cambang_result_convert_timing.h"

namespace cambang {
namespace camera_fact_convert {

inline const char* fact_origin_name(FactOrigin origin) {
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

inline const char* camera_facing_name(CameraFacing value) {
  switch (value) {
    case CameraFacing::FRONT: return "front";
    case CameraFacing::BACK: return "back";
    case CameraFacing::EXTERNAL: return "external";
    case CameraFacing::UNKNOWN: return "unknown";
  }
  return "unknown";
}

inline const char* camera_nature_name(CameraNature value) {
  switch (value) {
    case CameraNature::PHYSICAL: return "physical";
    case CameraNature::VIRTUAL: return "virtual";
    case CameraNature::HYBRID: return "hybrid";
    case CameraNature::UNKNOWN: return "unknown";
  }
  return "unknown";
}

inline godot::Array to_array(const Vec3Meters& value) {
  godot::Array out;
  out.push_back(value.x);
  out.push_back(value.y);
  out.push_back(value.z);
  return out;
}

inline godot::Array to_array(const QuaternionXyzw& value) {
  godot::Array out;
  out.push_back(value.x);
  out.push_back(value.y);
  out.push_back(value.z);
  out.push_back(value.w);
  return out;
}

inline godot::Dictionary to_dict(const SourcedFact<CameraPose>& fact) {
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

// The device-scoped four: facts a provider establishes when the device opens
// and does not re-report per image. Intrinsics and distortion are absent
// because they are not device-scoped facts at all -- both providers source
// them per image and anchored to a format, so they resolve into
// CaptureImageFacts. ResolvedCameraDeviceFacts cannot carry them, so this
// function cannot silently omit one: there is nothing to omit.
inline godot::Dictionary camera_device_facts_to_dict(
    const ResolvedCameraDeviceFacts& facts) {
  godot::Dictionary out;
  if (facts.facing) {
    godot::Dictionary value;
    value["value"] = godot::String(camera_facing_name(facts.facing->value));
    value["origin"] = godot::String(fact_origin_name(facts.facing->origin));
    out["facing"] = value;
  }
  if (facts.nature) {
    godot::Dictionary value;
    value["value"] = godot::String(camera_nature_name(facts.nature->value));
    value["origin"] = godot::String(fact_origin_name(facts.nature->origin));
    out["camera_nature"] = value;
  }
  if (facts.sensor_orientation) {
    godot::Dictionary value;
    value["value"] = static_cast<int64_t>(facts.sensor_orientation->value);
    value["origin"] = godot::String(fact_origin_name(facts.sensor_orientation->origin));
    out["sensor_orientation_degrees"] = value;
  }
  if (facts.pose) out["pose"] = to_dict(*facts.pose);
  return out;
}

inline const char* coordinate_domain_name(const CoordinateDomain& domain) {
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

inline void add_coordinate_domain(godot::Dictionary& out, const CoordinateDomain& domain) {
  out["coordinate_domain"] = godot::String(coordinate_domain_name(domain));
  if (const auto* platform = std::get_if<CoordinateDomainPlatformDefined>(&domain)) {
    out["platform_defined_coordinate_domain"] = godot::String(platform->token().c_str());
  }
}

inline godot::Dictionary to_dict(const SourcedFact<Intrinsics>& fact) {
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

inline godot::Dictionary to_dict(const SourcedFact<DeliveredImageRegion>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  out["left"] = static_cast<int64_t>(fact.value.left());
  out["top"] = static_cast<int64_t>(fact.value.top());
  out["width"] = static_cast<int64_t>(fact.value.width());
  out["height"] = static_cast<int64_t>(fact.value.height());
  add_coordinate_domain(out, fact.value.coordinate_domain());
  return out;
}
inline godot::Dictionary to_dict(const SourcedFact<Distortion>& fact) {
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

inline godot::Dictionary to_dict(const SourcedFact<FocusState>& fact) {
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

inline godot::Dictionary to_dict(const SourcedFact<ExposureTime>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  out["nanoseconds"] = fact.value.nanoseconds();
  return out;
}

inline godot::Dictionary to_dict(const SourcedFact<SensorSensitivityIso>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  out["iso_equivalent"] = fact.value.iso_equivalent();
  return out;
}

inline godot::Dictionary to_dict(const SourcedFact<ApertureFNumber>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  out["f_number"] = fact.value.f_number();
  return out;
}

inline godot::Dictionary to_dict(const SourcedFact<FocalLengthMm>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  out["millimetres"] = fact.value.millimetres();
  return out;
}

inline godot::Dictionary to_dict(const SourcedFact<RealizedImageTransform>& fact) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(fact.origin));
  out["rotation_degrees"] = static_cast<int64_t>(fact.value.rotation);
  out["mirrored"] = fact.value.mirrored;
  out["pixels_already_transformed"] = fact.value.pixels_already_transformed;
  return out;
}

inline godot::Dictionary camera_facts_to_dict(const CoreResolvedImageFacts& facts) {
  // The device-scoped facts come from the shared projection, which is also
  // what CamBANGStreamResult uses. They are resolved from one source already;
  // projecting them from one place too is what stops a key rename or a new
  // enum member reaching only one of the two surfaces.
  godot::Dictionary out =
      camera_fact_convert::camera_device_facts_to_dict(facts.camera);
  // Image-scoped, and so capture-only: both providers source these per image
  // and anchored to a format. They read from the image tier for that reason --
  // a device-scoped copy would carry no format and could not be applied here.
  // The delivered-image calibration first: it is what a caller building a
  // projection or frustum for the image in their hands needs, and using it
  // requires no knowledge of sensor coordinate domains at all.
  if (facts.image.intrinsics_delivered) {
    out["intrinsics_delivered"] = to_dict(*facts.image.intrinsics_delivered);
  }
  // The sensor-domain calibration, and the region linking it to the delivered
  // image: the advanced surface, for a caller working in sensor space.
  if (facts.image.intrinsics) out["intrinsics"] = to_dict(*facts.image.intrinsics);
  if (facts.image.delivered_image_region) {
    out["delivered_image_region"] = to_dict(*facts.image.delivered_image_region);
  }
  if (facts.image.distortion) out["distortion"] = to_dict(*facts.image.distortion);
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

} // namespace camera_fact_convert
} // namespace cambang

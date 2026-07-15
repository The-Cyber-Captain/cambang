#include "godot/cambang_result_convert_timing.h"

namespace cambang {
namespace {

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

const char* acquisition_clock_domain_name(ImageAcquisitionClockDomain value) {
  switch (value) {
    case ImageAcquisitionClockDomain::PROVIDER_MONOTONIC: return "provider_monotonic";
    case ImageAcquisitionClockDomain::CORE_MONOTONIC: return "core_monotonic";
    case ImageAcquisitionClockDomain::DOMAIN_OPAQUE: return "domain_opaque";
  }
  return "domain_opaque";
}

const char* acquisition_reference_event_name(ImageAcquisitionReferenceEvent value) {
  switch (value) {
    case ImageAcquisitionReferenceEvent::EXPOSURE_START: return "exposure_start";
    case ImageAcquisitionReferenceEvent::EXPOSURE_MIDPOINT: return "exposure_midpoint";
    case ImageAcquisitionReferenceEvent::SENSOR_READOUT_START: return "sensor_readout_start";
    case ImageAcquisitionReferenceEvent::FRAME_AVAILABLE: return "frame_available";
    case ImageAcquisitionReferenceEvent::PROVIDER_OBSERVED: return "provider_observed";
    case ImageAcquisitionReferenceEvent::UNKNOWN: return "unknown";
  }
  return "unknown";
}

const char* acquisition_comparability_name(ImageAcquisitionComparability value) {
  switch (value) {
    case ImageAcquisitionComparability::SAME_IMAGE_ONLY: return "same_image_only";
    case ImageAcquisitionComparability::SAME_DEVICE: return "same_device";
    case ImageAcquisitionComparability::SAME_PROVIDER: return "same_provider";
    case ImageAcquisitionComparability::CROSS_DEVICE_SYNCHRONIZED:
      return "cross_device_synchronized";
    case ImageAcquisitionComparability::CORE_TIMELINE: return "core_timeline";
    case ImageAcquisitionComparability::ORDERING_ONLY: return "ordering_only";
  }
  return "ordering_only";
}

}  // namespace

godot::Dictionary to_dict(const SourcedFact<ImageAcquisitionTiming>& v) {
  godot::Dictionary out;
  out["origin"] = godot::String(fact_origin_name(v.origin));
  out["acquisition_mark"] = v.value.acquisition_mark();
  out["tick_period_numerator_ns"] = v.value.tick_period().numerator_ns();
  out["tick_period_denominator"] = v.value.tick_period().denominator();
  out["clock_domain"] =
      godot::String(acquisition_clock_domain_name(v.value.clock_domain()));
  out["reference_event"] =
      godot::String(acquisition_reference_event_name(v.value.reference_event()));
  out["comparability"] =
      godot::String(acquisition_comparability_name(v.value.comparability()));
  return out;
}

void add_acquisition_timing_camera_fact(
    godot::Dictionary& out,
    const std::optional<SourcedFact<ImageAcquisitionTiming>>& v) {
  if (!v) {
    return;
  }
  out["acquisition_timing"] = to_dict(*v);
}

}  // namespace cambang

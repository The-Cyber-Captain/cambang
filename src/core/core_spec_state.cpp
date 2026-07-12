#include "core/core_spec_state.h"

namespace cambang {

void CoreSpecState::reset_for_generation(uint64_t imaging_spec_version) {
  camera_spec_versions_.clear();
  set_imaging_spec_version(imaging_spec_version);
}

void CoreSpecState::set_camera_spec_version(const std::string& hardware_id, uint64_t camera_spec_version) {
  if (hardware_id.empty()) {
    return;
  }
  camera_spec_versions_[hardware_id] = camera_spec_version;
}

uint64_t CoreSpecState::camera_spec_version(const std::string& hardware_id) const noexcept {
  if (hardware_id.empty()) {
    return 0;
  }
  const auto it = camera_spec_versions_.find(hardware_id);
  if (it == camera_spec_versions_.end()) {
    return 0;
  }
  return it->second;
}

void CoreSpecState::set_imaging_spec_version(uint64_t imaging_spec_version) noexcept {
  imaging_spec_version_ = imaging_spec_version;
  imaging_spec_payload_.clear();
  imaging_spec_retention_kind_ = ImagingSpecRetentionKind::None;
  imaging_spec_interpretation_ = {};
}

void CoreSpecState::set_imaging_spec_concurrency(
    uint64_t imaging_spec_version,
    camera_concurrency::Truth camera_concurrency) noexcept {
  set_imaging_spec_version(imaging_spec_version);
  imaging_spec_interpretation_.camera_concurrency = std::move(camera_concurrency);
}

bool CoreSpecState::retain_imaging_spec_replace(
    uint64_t imaging_spec_version,
    SpecPatchView effective_spec) {
  return retain_imaging_spec_payload_(
      imaging_spec_version,
      effective_spec,
      ImagingSpecRetentionKind::Replace);
}

bool CoreSpecState::retain_imaging_spec_patch(
    uint64_t imaging_spec_version,
    SpecPatchView effective_spec) {
  return retain_imaging_spec_payload_(
      imaging_spec_version,
      effective_spec,
      ImagingSpecRetentionKind::Patch);
}

SpecPatchView CoreSpecState::imaging_spec_payload() const noexcept {
  SpecPatchView out{};
  if (imaging_spec_payload_.empty()) {
    return out;
  }
  out.data = imaging_spec_payload_.data();
  out.size_bytes = imaging_spec_payload_.size();
  return out;
}

std::vector<uint8_t> CoreSpecState::imaging_spec_payload_copy() const {
  return imaging_spec_payload_;
}

CoreSpecState::ImagingSpecInterpretation
CoreSpecState::interpret_imaging_spec() const noexcept {
  return imaging_spec_interpretation_;
}

bool CoreSpecState::retain_imaging_spec_payload_(
    uint64_t imaging_spec_version,
    SpecPatchView effective_spec,
    ImagingSpecRetentionKind retention_kind) {
  if (effective_spec.size_bytes != 0 && effective_spec.data == nullptr) {
    return false;
  }

  ImagingSpecInterpretation next_interpretation{};
  std::vector<uint8_t> next_payload{};
  if (effective_spec.size_bytes != 0) {
    const camera_concurrency::LoadResult load =
        camera_concurrency::load_truth_from_adc_json_payload(effective_spec);
    if (!load.ok) {
      return false;
    }
    next_interpretation.camera_concurrency = load.truth;
    const auto* bytes = static_cast<const uint8_t*>(effective_spec.data);
    next_payload.assign(bytes, bytes + effective_spec.size_bytes);
  }

  imaging_spec_version_ = imaging_spec_version;
  imaging_spec_retention_kind_ = retention_kind;
  imaging_spec_payload_ = std::move(next_payload);
  imaging_spec_interpretation_ = std::move(next_interpretation);
  return true;
}

} // namespace cambang

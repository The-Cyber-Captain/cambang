#include "core/provider_camera_fact_state.h"

#include <chrono>
#include <future>
#include <memory>
#include <type_traits>
#include <variant>

#include "core/core_runtime.h"

namespace cambang {
namespace {

bool valid(FactOrigin value) noexcept {
  return value <= FactOrigin::UNKNOWN;
}

bool valid(CameraFacing value) noexcept { return value <= CameraFacing::UNKNOWN; }
bool valid(CameraNature value) noexcept { return value <= CameraNature::UNKNOWN; }
bool valid(SensorOrientationDegrees value) noexcept {
  return value == SensorOrientationDegrees::DEGREES_0 ||
         value == SensorOrientationDegrees::DEGREES_90 ||
         value == SensorOrientationDegrees::DEGREES_180 ||
         value == SensorOrientationDegrees::DEGREES_270;
}
bool valid(DistortionImageState value) noexcept {
  return value <= DistortionImageState::UNKNOWN;
}
bool valid(ImageAcquisitionClockDomain value) noexcept {
  return value <= ImageAcquisitionClockDomain::DOMAIN_OPAQUE;
}
bool valid(ImageAcquisitionReferenceEvent value) noexcept {
  return value <= ImageAcquisitionReferenceEvent::UNKNOWN;
}
bool valid(ImageAcquisitionComparability value) noexcept {
  return value <= ImageAcquisitionComparability::ORDERING_ONLY;
}
bool valid(ImageRotationDegrees value) noexcept {
  return value == ImageRotationDegrees::DEGREES_0 ||
         value == ImageRotationDegrees::DEGREES_90 ||
         value == ImageRotationDegrees::DEGREES_180 ||
         value == ImageRotationDegrees::DEGREES_270;
}

template <typename T, typename Validator>
bool valid_sourced(const std::optional<SourcedFact<T>>& fact, Validator validator) noexcept {
  return !fact || (valid(fact->origin) && validator(fact->value));
}

bool valid_intrinsics(const Intrinsics&) noexcept { return true; }
bool valid_pose(const CameraPose&) noexcept { return true; }
bool valid_distortion(const Distortion& value) noexcept {
  return std::visit(
      [](const auto& distortion) noexcept {
        using T = std::decay_t<decltype(distortion)>;
        if constexpr (std::is_same_v<T, NoDistortion>) {
          return valid(distortion.image_state);
        } else {
          return valid(distortion.image_state());
        }
      },
      value);
}

bool valid_acquisition_timing(const ImageAcquisitionTiming& value) noexcept {
  return value.acquisition_mark() >= 0 && value.tick_period().numerator_ns() > 0 &&
         value.tick_period().denominator() > 0 && valid(value.clock_domain()) &&
         valid(value.reference_event()) && valid(value.comparability());
}

bool valid_focus_state(const FocusState& value) noexcept {
  if (value.valueless_by_exception()) {
    return false;
  }
  return std::visit(
      [](const auto& focus) noexcept {
        using T = std::decay_t<decltype(focus)>;
        if constexpr (std::is_same_v<T, FocusAtDistance>) {
          return std::isfinite(focus.distance_m());
        }
        return true;
      },
      value);
}

bool valid_realized_image_transform(const RealizedImageTransform& value) noexcept {
  return valid(value.rotation);
}

bool valid_capture_image(const CaptureImageFacts& facts) noexcept {
  return valid_sourced(facts.focus_state, valid_focus_state) &&
         valid_sourced(facts.realized_image_transform, valid_realized_image_transform);
}

bool valid_static(const CameraStaticFacts& facts) noexcept {
  return valid_sourced(facts.facing, [](CameraFacing value) { return valid(value); }) &&
         valid_sourced(facts.nature, [](CameraNature value) { return valid(value); }) &&
         valid_sourced(
             facts.sensor_orientation,
             [](SensorOrientationDegrees value) { return valid(value); }) &&
         valid_sourced(facts.intrinsics, valid_intrinsics) &&
         valid_sourced(facts.distortion, valid_distortion) &&
         valid_sourced(facts.pose, valid_pose);
}
} // namespace

bool ProviderCameraFactState::replace_static(
    uint64_t device_instance_id, ProviderCameraFacts facts) {
  if (device_instance_id == 0 || !valid(facts)) return false;
  static_by_device_[device_instance_id] = std::move(facts);
  return true;
}

bool ProviderCameraFactState::replace_capture_image(
    CaptureImageKey key, ProviderCaptureImageFacts facts) {
  if (key.capture_id == 0 || key.device_instance_id == 0 || !valid(facts)) return false;
  capture_images_[key] = std::move(facts);
  return true;
}

void ProviderCameraFactState::erase_device(uint64_t device_instance_id) noexcept {
  static_by_device_.erase(device_instance_id);
  for (auto it = capture_images_.begin(); it != capture_images_.end();) {
    if (it->first.device_instance_id == device_instance_id) {
      it = capture_images_.erase(it);
    } else {
      ++it;
    }
  }
}

void ProviderCameraFactState::clear() noexcept {
  static_by_device_.clear();
  capture_images_.clear();
}

const ProviderCameraFacts* ProviderCameraFactState::find_static(
    uint64_t device_instance_id) const noexcept {
  const auto it = static_by_device_.find(device_instance_id);
  return it == static_by_device_.end() ? nullptr : &it->second;
}

const ProviderCaptureImageFacts* ProviderCameraFactState::find_capture_image(
    CaptureImageKey key) const noexcept {
  const auto it = capture_images_.find(key);
  return it == capture_images_.end() ? nullptr : &it->second;
}

bool ProviderCameraFactState::valid(const ProviderCameraFacts& facts) noexcept {
  return valid_static(facts.static_facts);
}

bool ProviderCameraFactState::valid(const ProviderCaptureImageFacts& facts) noexcept {
  return valid_sourced(facts.intrinsics, valid_intrinsics) &&
         valid_sourced(facts.distortion, valid_distortion) &&
         valid_sourced(facts.pose, valid_pose) &&
         valid_sourced(facts.focus_state, valid_focus_state) &&
         valid_sourced(facts.realized_image_transform, valid_realized_image_transform);
}

#if defined(CAMBANG_INTERNAL_SMOKE)
std::optional<ProviderCameraFacts> CoreRuntime::provider_camera_facts_for_smoke(
    uint64_t device_instance_id) const {
  if (device_instance_id == 0) return std::nullopt;
  if (core_thread_.is_core_thread()) {
    const auto* facts = provider_camera_fact_state_.find_static(device_instance_id);
    return facts ? std::optional<ProviderCameraFacts>(*facts) : std::nullopt;
  }
  auto completion = std::make_shared<std::promise<std::optional<ProviderCameraFacts>>>();
  std::future<std::optional<ProviderCameraFacts>> completed = completion->get_future();
  CoreRuntime* self = const_cast<CoreRuntime*>(this);
  if (self->try_post([this, device_instance_id, completion]() {
        const auto* facts = provider_camera_fact_state_.find_static(device_instance_id);
        completion->set_value(facts ? std::optional<ProviderCameraFacts>(*facts) : std::nullopt);
      }) != CoreThread::PostResult::Enqueued ||
      completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return std::nullopt;
  }
  return completed.get();
}

std::optional<ProviderCaptureImageFacts>
CoreRuntime::provider_capture_image_facts_for_smoke(
    uint64_t capture_id,
    uint64_t device_instance_id,
    uint32_t image_member_index) const {
  if (capture_id == 0 || device_instance_id == 0) return std::nullopt;
  const ProviderCameraFactState::CaptureImageKey key{
      capture_id, device_instance_id, image_member_index};
  if (core_thread_.is_core_thread()) {
    const auto* facts = provider_camera_fact_state_.find_capture_image(key);
    return facts ? std::optional<ProviderCaptureImageFacts>(*facts) : std::nullopt;
  }
  auto completion = std::make_shared<std::promise<std::optional<ProviderCaptureImageFacts>>>();
  std::future<std::optional<ProviderCaptureImageFacts>> completed = completion->get_future();
  CoreRuntime* self = const_cast<CoreRuntime*>(this);
  if (self->try_post([this, key, completion]() {
        const auto* facts = provider_camera_fact_state_.find_capture_image(key);
        completion->set_value(
            facts ? std::optional<ProviderCaptureImageFacts>(*facts) : std::nullopt);
      }) != CoreThread::PostResult::Enqueued ||
      completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return std::nullopt;
  }
  return completed.get();
}
#endif

} // namespace cambang

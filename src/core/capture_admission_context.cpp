#include "core/core_runtime.h"

#include <cassert>

namespace cambang {

CaptureAdmissionContext CoreRuntime::make_capture_admission_context_() const {
  assert(core_thread_.is_core_thread());
  capture_admission_clock_sample_count_.fetch_add(1, std::memory_order_relaxed);
  CaptureAdmissionContext context{};
  const int64_t override_ns = capture_admission_clock_override_ns_.load(std::memory_order_acquire);
  if (override_ns != INT64_MIN) {
    context.capture_date_time = CaptureDateTimeUtc::from_unix_epoch_nanoseconds(override_ns);
  } else {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    context.capture_date_time = CaptureDateTimeUtc::from_unix_epoch_nanoseconds(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  }
  {
    const std::lock_guard<std::mutex> lock(configured_capture_geolocation_mutex_);
    context.geolocation = active_capture_geolocation_;
  }
  return context;
}

CoreRuntime::ReplaceCaptureGeolocationStatus
CoreRuntime::replace_capture_geolocation_for_server(
    std::optional<CaptureGeolocation> replacement) {
  std::lock_guard<std::mutex> lock(configured_capture_geolocation_mutex_);
  configured_capture_geolocation_ = replacement;
  if (state_.load(std::memory_order_acquire) == CoreRuntimeState::LIVE) {
    active_capture_geolocation_ = std::move(replacement);
  }
  return ReplaceCaptureGeolocationStatus::Ok;
}

#if defined(CAMBANG_INTERNAL_SMOKE)
CoreRuntime::ReplaceCaptureGeolocationStatus CoreRuntime::smoke_replace_capture_geolocation(
    double latitude_degrees, double longitude_degrees, std::optional<double> altitude_meters) {
  const CoreRuntimeState state = state_.load(std::memory_order_acquire);
  if (state == CoreRuntimeState::LIVE || state == CoreRuntimeState::TEARING_DOWN) return ReplaceCaptureGeolocationStatus::Busy;
  const auto replacement = CaptureGeolocation::create(latitude_degrees, longitude_degrees, altitude_meters);
  if (!replacement) return ReplaceCaptureGeolocationStatus::Invalid;
  return replace_capture_geolocation_for_server(*replacement);
}

CoreRuntime::ReplaceCaptureGeolocationStatus CoreRuntime::smoke_clear_capture_geolocation() {
  const CoreRuntimeState state = state_.load(std::memory_order_acquire);
  if (state == CoreRuntimeState::LIVE || state == CoreRuntimeState::TEARING_DOWN) return ReplaceCaptureGeolocationStatus::Busy;
  return replace_capture_geolocation_for_server(std::nullopt);
}

bool CoreRuntime::smoke_set_capture_datetime_utc_nanoseconds(int64_t value) {
  capture_admission_clock_override_ns_.store(value, std::memory_order_release);
  return true;
}

uint64_t CoreRuntime::smoke_capture_admission_clock_sample_count() const noexcept {
  return capture_admission_clock_sample_count_.load(std::memory_order_relaxed);
}

void CoreRuntime::smoke_reset_capture_admission_clock_sample_count() noexcept {
  capture_admission_clock_sample_count_.store(0, std::memory_order_relaxed);
}

std::optional<CaptureAdmissionContext> CoreRuntime::smoke_capture_admission_context(
    uint64_t capture_id, uint64_t device_instance_id) const {
  const auto assembly = capture_assembly_registry_.find_for_smoke(capture_id, device_instance_id);
  return assembly && assembly->has_admission_context ? std::optional<CaptureAdmissionContext>(assembly->admission_context) : std::nullopt;
}
#endif

} // namespace cambang

#include "imaging/broker/provider_broker.h"

#include <mutex>
#include <string>
#include <utility>

#include "imaging/api/provider_error_string.h"

// (No broker-level pattern switching; picture is stream-scoped.)

#if defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
  #include "imaging/stub/provider.h"
#endif

// Synthetic provider (optional compile).
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  #include "imaging/synthetic/provider.h"
  #include "imaging/synthetic/config.h"
  #include "imaging/synthetic/builtin_scenario_library.h"
#endif

namespace cambang {

namespace {

ProviderResult err_not_initialized() {
  return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
}

ProviderResult err_no_active() {
  return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
}



ProviderError provider_error_from_access_status(ProviderAccessStatus status) noexcept {
  switch (status.code) {
    case ProviderAccessCode::Ready:
      return ProviderError::OK;
    case ProviderAccessCode::PermissionRequired:
    case ProviderAccessCode::PermissionDenied:
    case ProviderAccessCode::AccessUnavailable:
      return ProviderError::ERR_PLATFORM_CONSTRAINT;
    case ProviderAccessCode::CheckFailed:
      return ProviderError::ERR_PROVIDER_FAILED;
    default:
      return ProviderError::ERR_PROVIDER_FAILED;
  }
}

} // namespace

ProviderBroker::ProviderBroker() = default;

ProviderBroker::~ProviderBroker() {
  // Best-effort: ensure deterministic shutdown when broker is destroyed.
  // Core should call shutdown(), but destruction should still clean up safely.
  //
  // Mirrors shutdown()'s locking discipline below (see that method's doc
  // comment for the full rationale): take ownership of active_ and clear it
  // under the lock, then call the provider's own shutdown() -- which is not
  // guaranteed prompt -- without the lock held. Previously this destructor
  // held active_provider_mutex_ for the full duration of active_->shutdown(),
  // inconsistent with shutdown()'s own pattern; any other thread reading a
  // latched accessor concurrently would block for however long a real
  // provider's teardown takes, purely because destruction happened to be the
  // path taken instead of an explicit shutdown() call.
  std::unique_ptr<ICameraProvider> provider_to_shutdown;
  {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    if (initialized_ && active_) {
      provider_to_shutdown = std::move(active_);
    }
    initialized_ = false;
    callbacks_ = nullptr;
  }
  if (provider_to_shutdown) {
    (void)provider_to_shutdown->shutdown();
  }
}

const char* ProviderBroker::provider_name() const {
  // The broker is the core-bound facade; report active backend for logs.
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (active_) {
    return active_->provider_name();
  }
  return "broker(uninitialized)";
}

ProviderKind ProviderBroker::provider_kind() const noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  return mode_latched_ == RuntimeMode::synthetic ? ProviderKind::synthetic
                                                  : ProviderKind::platform_backed;
}

StreamTemplate ProviderBroker::stream_template() const {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (active_) {
    return active_->stream_template();
  }
  return StreamTemplate{};
}

CaptureTemplate ProviderBroker::capture_template() const {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (active_) {
    return active_->capture_template();
  }
  return CaptureTemplate{};
}

bool ProviderBroker::supports_stream_picture_updates() const noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  return active_ ? active_->supports_stream_picture_updates() : false;
}

bool ProviderBroker::supports_capture_picture_updates() const noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  return active_ ? active_->supports_capture_picture_updates() : false;
}

bool ProviderBroker::supports_multi_image_still_sequence() const noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  return active_ ? active_->supports_multi_image_still_sequence() : false;
}

ProducerBackingCapabilities ProviderBroker::stream_backing_capabilities(
    const CaptureProfile& profile,
    const PictureConfig& picture) const noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  return active_ ? active_->stream_backing_capabilities(profile, picture)
                 : ProducerBackingCapabilities{false, false};
}

ProducerBackingCapabilities ProviderBroker::capture_backing_capabilities(
    const CaptureRequest& req) const noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  return active_ ? active_->capture_backing_capabilities(req)
                 : ProducerBackingCapabilities{false, false};
}

ProducerBackingCapabilities ProviderBroker::stream_parent_context_backing_capabilities(
    uint64_t device_instance_id,
    uint64_t stream_id,
    StreamIntent intent,
    const CaptureProfile& profile,
    const PictureConfig& picture) noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  return active_ ? active_->stream_parent_context_backing_capabilities(
                       device_instance_id, stream_id, intent, profile, picture)
                 : ProducerBackingCapabilities{false, false};
}

ProducerBackingCapabilities ProviderBroker::capture_parent_context_backing_capabilities(
    uint64_t device_instance_id,
    const CaptureRequest& req) noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  return active_ ? active_->capture_parent_context_backing_capabilities(
                       device_instance_id, req)
                 : ProducerBackingCapabilities{false, false};
}

uint64_t ProviderBroker::stream_backing_plan_evaluation_settle_delay_ns() const noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  return active_ ? active_->stream_backing_plan_evaluation_settle_delay_ns() : 0;
}

uint64_t ProviderBroker::capture_backing_plan_evaluation_settle_delay_ns() const noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  return active_ ? active_->capture_backing_plan_evaluation_settle_delay_ns() : 0;
}

ProviderResult ProviderBroker::update_stream_retained_production_plan(
    uint64_t stream_id,
    CoreRetainedProductionPlan requested_retained_plan) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (!active_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return active_->update_stream_retained_production_plan(
      stream_id, requested_retained_plan);
}

ProviderResult ProviderBroker::check_mode_supported_in_build(RuntimeMode mode) noexcept {
  switch (mode) {
    case RuntimeMode::platform_backed: {
#if defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
      return ProviderResult::success();
#else
      return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
#endif
    }
    case RuntimeMode::synthetic: {
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
      return ProviderResult::success();
#else
      return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
#endif
    }
    default:
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
}

ProviderAccessStatus ProviderBroker::check_mode_access_readiness(RuntimeMode mode) noexcept {
  const ProviderResult cap = check_mode_supported_in_build(mode);
  if (!cap.ok()) {
    return ProviderAccessStatus::failure(
        ProviderAccessCode::AccessUnavailable,
        "provider_mode_not_supported_in_build",
        false);
  }

  switch (mode) {
    case RuntimeMode::platform_backed: {
#if defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
      return StubProvider::check_access_readiness();
#else
      return ProviderAccessStatus::failure(
          ProviderAccessCode::AccessUnavailable,
          "no_platform_backed_provider_compiled",
          false);
#endif
    }
    case RuntimeMode::synthetic: {
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
      return SyntheticProvider::check_access_readiness();
#else
      return ProviderAccessStatus::failure(
          ProviderAccessCode::AccessUnavailable,
          "synthetic_provider_not_compiled",
          false);
#endif
    }
    default:
      return ProviderAccessStatus::failure(
          ProviderAccessCode::CheckFailed,
          "invalid_runtime_mode",
          false);
  }
}

ProviderResult ProviderBroker::set_runtime_mode_requested(RuntimeMode mode) noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }
  ProviderResult cap = check_mode_supported_in_build(mode);
  if (!cap.ok()) {
    return cap;
  }
  mode_requested_ = mode;
  return ProviderResult::success();
}

ProviderResult ProviderBroker::set_synthetic_role_requested(SyntheticRole role) noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }
  synthetic_role_requested_ = role;
  return ProviderResult::success();
}

ProviderResult ProviderBroker::set_synthetic_timing_driver_requested(TimingDriver timing_driver) noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }
  timing_driver_requested_ = timing_driver;
  return ProviderResult::success();
}

ProviderResult ProviderBroker::set_synthetic_timeline_reconciliation_requested(TimelineReconciliation reconciliation) noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }
  // Applicable only to synthetic timeline + virtual_time.
  if (mode_requested_ != RuntimeMode::synthetic ||
      synthetic_role_requested_ != SyntheticRole::Timeline ||
      timing_driver_requested_ != TimingDriver::VirtualTime) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  timeline_reconciliation_requested_ = reconciliation;
  return ProviderResult::success();
}

ProviderResult ProviderBroker::set_synthetic_producer_output_form_mode_requested(
    SyntheticProducerOutputFormMode mode) noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }
  producer_output_form_mode_requested_ = mode;
  return ProviderResult::success();
}

ProviderResult ProviderBroker::set_synthetic_stream_capability_downgrade_conditions_requested(
    std::vector<SyntheticStreamCapabilityDowngradeCondition> conditions) noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }
  stream_capability_downgrade_conditions_requested_ = std::move(conditions);
  return ProviderResult::success();
}

ProviderResult ProviderBroker::set_synthetic_capture_capability_downgrade_conditions_requested(
    std::vector<SyntheticCaptureCapabilityDowngradeCondition> conditions) noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }
  capture_capability_downgrade_conditions_requested_ = std::move(conditions);
  return ProviderResult::success();
}

void ProviderBroker::dispatch_synthetic_timeline_request_(const SyntheticScheduledEvent& ev) {
  std::function<void(const SyntheticScheduledEvent&)> hook;
  {
    std::lock_guard<std::mutex> lock(synthetic_timeline_dispatch_mutex_);
    if (deferring_synthetic_timeline_dispatches_) {
      deferred_synthetic_timeline_dispatches_.push_back(ev);
      return;
    }
    hook = synthetic_timeline_request_dispatch_hook_;
  }
  if (hook) {
    hook(ev);
  }
}

void ProviderBroker::install_synthetic_timeline_request_dispatch_hook_locked_() {
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
    syn->set_timeline_request_dispatch_hook_for_host(
        [this](const SyntheticScheduledEvent& ev) { dispatch_synthetic_timeline_request_(ev); });
  }
#endif
}

void ProviderBroker::set_synthetic_timeline_request_dispatch_hook(
    std::function<void(const SyntheticScheduledEvent&)> hook) {
  {
    std::lock_guard<std::mutex> lock(synthetic_timeline_dispatch_mutex_);
    synthetic_timeline_request_dispatch_hook_ = std::move(hook);
  }

  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  install_synthetic_timeline_request_dispatch_hook_locked_();
}

ProviderResult ProviderBroker::initialize(IProviderCallbacks* callbacks) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (initialized_) {
    return ProviderResult::success();
  }
  callbacks_ = callbacks;
  if (!callbacks_) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  // Mode selection is explicit and latched per runtime session.
  // (Server provides the requested mode; broker does not consult env/CLI.)
  mode_latched_ = mode_requested_;
  synthetic_role_latched_ = synthetic_role_requested_;
  timing_driver_latched_ = timing_driver_requested_;
  timeline_reconciliation_latched_ = timeline_reconciliation_requested_;
  producer_output_form_mode_latched_ = producer_output_form_mode_requested_;
  stream_capability_downgrade_conditions_latched_ =
      stream_capability_downgrade_conditions_requested_;
  capture_capability_downgrade_conditions_latched_ =
      capture_capability_downgrade_conditions_requested_;

  // Defensive: re-check build support (mirrors server-side validation).
  ProviderResult cap = check_mode_supported_in_build(mode_latched_);
  if (!cap.ok()) {
    return cap;
  }

  const ProviderAccessStatus access = check_mode_access_readiness(mode_latched_);
  if (!access.ok()) {
    return ProviderResult::failure(provider_error_from_access_status(access));
  }

  // Construct backend.
  if (mode_latched_ == RuntimeMode::synthetic) {
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
    SyntheticProviderConfig cfg{};
    cfg.synthetic_role = synthetic_role_latched_;
    cfg.timing_driver = timing_driver_latched_;
    cfg.timeline_reconciliation = timeline_reconciliation_latched_;
    cfg.producer_output_form_mode = producer_output_form_mode_latched_;
    cfg.verification_stream_capability_downgrade_conditions =
        stream_capability_downgrade_conditions_latched_;
    cfg.verification_capture_capability_downgrade_conditions =
        capture_capability_downgrade_conditions_latched_;
    auto syn = std::make_unique<SyntheticProvider>(cfg);
    active_ = std::move(syn);
    install_synthetic_timeline_request_dispatch_hook_locked_();
#endif
  }

  if (!active_) {
    // Platform-backed selection is compile-time (SCons provider=...).
#if defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
    active_ = std::make_unique<StubProvider>();
#else
    // No platform-backed provider compiled in.
    active_.reset();
#endif
  }

  if (!active_) {
    return err_no_active();
  }

  ProviderResult pr = active_->initialize(callbacks_);
  if (!pr.ok()) {
    active_.reset();
    return pr;
  }
  initialized_ = true;
  return ProviderResult::success();
}

ProviderResult ProviderBroker::ensure_initialized_or_err_() const {
  if (!initialized_) {
    return err_not_initialized();
  }
  return ProviderResult::success();
}

ProviderResult ProviderBroker::ensure_active_or_err_() const {
  ProviderResult pr = ensure_initialized_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  if (!active_) {
    return err_no_active();
  }
  return ProviderResult::success();
}

ProviderResult ProviderBroker::enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->enumerate_endpoints(out_endpoints);
}

ProviderResult ProviderBroker::open_device(
    const std::string& hardware_id,
    uint64_t device_instance_id,
    uint64_t root_id) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->open_device(hardware_id, device_instance_id, root_id);
}

ProviderResult ProviderBroker::close_device(uint64_t device_instance_id) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->close_device(device_instance_id);
}

ProviderResult ProviderBroker::create_stream(const StreamRequest& req) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->create_stream(req);
}

ProviderResult ProviderBroker::destroy_stream(uint64_t stream_id) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->destroy_stream(stream_id);
}

ProviderResult ProviderBroker::start_stream(
    uint64_t stream_id,
    const CaptureProfile& profile,
    const PictureConfig& picture) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->start_stream(stream_id, profile, picture);
}

ProviderResult ProviderBroker::stop_stream(uint64_t stream_id) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->stop_stream(stream_id);
}

ProviderResult ProviderBroker::set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->set_stream_picture_config(stream_id, picture);
}

ProviderResult ProviderBroker::set_capture_picture_config(uint64_t device_instance_id, const PictureConfig& picture) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->set_capture_picture_config(device_instance_id, picture);
}

ProviderResult ProviderBroker::sync_capture_parent_priming(
    const CaptureRequest& req) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->sync_capture_parent_priming(req);
}

ProviderResult ProviderBroker::release_capture_parent_priming(
    uint64_t device_instance_id) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->release_capture_parent_priming(device_instance_id);
}

ProviderResult ProviderBroker::trigger_capture(const CaptureRequest& req) {
  std::unique_lock<std::mutex> lock(active_provider_mutex_);
  ProviderResult out = ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    out = pr;
  } else {
    out = active_->trigger_capture(req);
  }
  lock.unlock();
  return out;
}

ProviderResult ProviderBroker::trigger_capture_submission(const CaptureSubmission& submission) {
  std::unique_lock<std::mutex> lock(active_provider_mutex_);
  ProviderResult out = ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    out = pr;
  } else {
    out = active_->trigger_capture_submission(submission);
  }
  lock.unlock();
  return out;
}

ProviderResult ProviderBroker::abort_capture(uint64_t capture_id) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->abort_capture(capture_id);
}

ProviderResult ProviderBroker::apply_camera_spec_patch(
    const std::string& hardware_id,
    uint64_t new_camera_spec_version,
    SpecPatchView patch) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->apply_camera_spec_patch(hardware_id, new_camera_spec_version, patch);
}

ProviderResult ProviderBroker::apply_imaging_spec_patch(
    uint64_t new_imaging_spec_version,
    SpecPatchView patch) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->apply_imaging_spec_patch(new_imaging_spec_version, patch);
}

ProviderResult ProviderBroker::shutdown() {
  // A provider's shutdown() is not guaranteed prompt (docs/provider_architecture.md
  // §8.1: "must be prompt, bounded... must not perform long backend drains
  // while public/Godot callers are synchronously blocked"; the current
  // SyntheticProvider::shutdown() joins outstanding capture threads and
  // flushes/stops its strand). Take exclusive ownership of active_ and clear
  // it under the lock so any other thread calling into the broker concurrently
  // observes "not initialized" immediately (a fast, deterministic failure)
  // instead of blocking on active_provider_mutex_ for however long teardown
  // takes. The actual (possibly slow) provider->shutdown() call then runs
  // without the lock held; no other code path can reach this exact provider
  // instance any more since active_ was cleared before releasing the lock.
  std::unique_ptr<ICameraProvider> provider_to_shutdown;
  {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    ProviderResult pr = ensure_initialized_or_err_();
    if (!pr.ok()) {
      return pr;
    }
    provider_to_shutdown = std::move(active_);
    initialized_ = false;
    callbacks_ = nullptr;
  }

  if (!provider_to_shutdown) {
    return ProviderResult::success();
  }
  return provider_to_shutdown->shutdown();
}

#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
ProviderResult ProviderBroker::install_active_provider_for_smoke(
    std::unique_ptr<ICameraProvider> provider,
    IProviderCallbacks* callbacks) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (initialized_ || active_ || provider == nullptr || callbacks == nullptr) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  callbacks_ = callbacks;
  active_ = std::move(provider);
  ProviderResult pr = active_->initialize(callbacks_);
  if (!pr.ok()) {
    active_.reset();
    callbacks_ = nullptr;
    return pr;
  }
  initialized_ = true;
  install_synthetic_timeline_request_dispatch_hook_locked_();
  return ProviderResult::success();
}
#endif

bool ProviderBroker::try_tick_virtual_time(uint64_t dt_ns) {
  std::vector<SyntheticScheduledEvent> deferred_dispatches;
  std::function<void(const SyntheticScheduledEvent&)> hook;

  bool tick_result = false;
  {
    std::unique_lock<std::mutex> lock(active_provider_mutex_);
    if (!initialized_ || !active_) {
      lock.unlock();
      return false;
    }

    // Synthetic virtual_time driver.
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
    if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
      {
        std::lock_guard<std::mutex> dispatch_lock(synthetic_timeline_dispatch_mutex_);
        deferring_synthetic_timeline_dispatches_ = true;
        deferred_synthetic_timeline_dispatches_.clear();
      }
      syn->advance(dt_ns);
      {
        std::lock_guard<std::mutex> dispatch_lock(synthetic_timeline_dispatch_mutex_);
        deferred_dispatches.swap(deferred_synthetic_timeline_dispatches_);
        deferring_synthetic_timeline_dispatches_ = false;
        hook = synthetic_timeline_request_dispatch_hook_;
      }
      tick_result = true;
    } else
#endif

    // Stub heartbeat driver.
#if defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
    if (auto* stub = dynamic_cast<StubProvider*>(active_.get())) {
      stub->advance(dt_ns);
      tick_result = true;
    } else
#endif
    {
      tick_result = false;
    }
    lock.unlock();
    if (!tick_result) {
      return false;
    }
  }

  if (hook) {
    for (const auto& ev : deferred_dispatches) {
      hook(ev);
    }
  }
  return true;
}

ProviderResult ProviderBroker::set_timeline_scenario_for_host(const SyntheticTimelineScenario& scenario) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
    return syn->set_timeline_scenario_for_host(scenario);
  }
#endif
  (void)scenario;
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult ProviderBroker::set_timeline_canonical_scenario_for_host(const SyntheticCanonicalScenario& scenario) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
    return syn->set_timeline_scenario_for_host(scenario);
  }
#endif
  (void)scenario;
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult ProviderBroker::select_timeline_builtin_scenario_for_host(const std::string& scenario_name) {
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  SyntheticBuiltinScenarioLibraryId library_id{};
  if (scenario_name == "stream_lifecycle_versions") {
    library_id = SyntheticBuiltinScenarioLibraryId::StreamLifecycleVersions;
  } else if (scenario_name == "topology_change_versions") {
    library_id = SyntheticBuiltinScenarioLibraryId::TopologyChangeVersions;
  } else if (scenario_name == "publication_coalescing") {
    library_id = SyntheticBuiltinScenarioLibraryId::PublicationCoalescing;
  } else if (scenario_name == "stream_inspection_live") {
    library_id = SyntheticBuiltinScenarioLibraryId::StreamInspectionLive;
  } else {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  CaptureProfile profile{};
  {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    ProviderResult pr = ensure_active_or_err_();
    if (!pr.ok()) {
      return pr;
    }
    profile = active_->stream_template().profile;
  }

  SyntheticCanonicalScenario canonical{};
  std::string error;
  if (!build_synthetic_builtin_scenario_library_canonical_scenario(
          library_id,
          profile,
          canonical,
          &error)) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
    return syn->set_timeline_scenario_for_host(canonical);
  }
#else
  (void)scenario_name;
#endif
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult ProviderBroker::load_timeline_canonical_scenario_from_json_text_for_host(
    const std::string& text,
    std::string* error) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
    return syn->load_timeline_canonical_scenario_from_json_text_for_host(text, error);
  }
#endif
  (void)text;
  if (error) {
    *error = "active provider does not support synthetic timeline json loading";
  }
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult ProviderBroker::load_timeline_canonical_scenario_from_json_file_for_host(
    const std::string& path,
    std::string* error) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
    return syn->load_timeline_canonical_scenario_from_json_file_for_host(path, error);
  }
#endif
  (void)path;
  if (error) {
    *error = "active provider does not support synthetic timeline json loading";
  }
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult ProviderBroker::start_timeline_scenario_for_host() {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
    return syn->start_timeline_scenario_for_host();
  }
#endif
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult ProviderBroker::stop_timeline_scenario_for_host() {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
    return syn->stop_timeline_scenario_for_host();
  }
#endif
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult ProviderBroker::set_timeline_scenario_paused_for_host(bool paused) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
    return syn->set_timeline_scenario_paused_for_host(paused);
  }
#endif
  (void)paused;
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult ProviderBroker::advance_timeline_for_host(uint64_t dt_ns) {
  constexpr uint32_t kMaxCurrentTimeDrainPasses = 16;
  ProviderResult result = ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  uint64_t step_dt_ns = dt_ns;

  for (uint32_t pass = 0; pass < kMaxCurrentTimeDrainPasses; ++pass) {
    std::vector<SyntheticScheduledEvent> deferred_dispatches;
    std::function<void(const SyntheticScheduledEvent&)> hook;

    {
      std::lock_guard<std::mutex> lock(active_provider_mutex_);
      ProviderResult pr = ensure_active_or_err_();
      if (!pr.ok()) {
        return pr;
      }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
      if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
        {
          std::lock_guard<std::mutex> dispatch_lock(synthetic_timeline_dispatch_mutex_);
          deferring_synthetic_timeline_dispatches_ = true;
          deferred_synthetic_timeline_dispatches_.clear();
        }
        result = syn->advance_timeline_for_host(step_dt_ns);
        {
          std::lock_guard<std::mutex> dispatch_lock(synthetic_timeline_dispatch_mutex_);
          deferred_dispatches.swap(deferred_synthetic_timeline_dispatches_);
          deferring_synthetic_timeline_dispatches_ = false;
          hook = synthetic_timeline_request_dispatch_hook_;
        }
      }
#endif
    }

    if (!hook || deferred_dispatches.empty()) {
      return result;
    }
    for (const auto& ev : deferred_dispatches) {
      hook(ev);
    }
    step_dt_ns = 0;
  }

  return result;
}

ProviderResult ProviderBroker::set_timeline_reconciliation_for_host(TimelineReconciliation reconciliation) {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
    return syn->set_timeline_reconciliation_for_host(reconciliation);
  }
#endif
  (void)reconciliation;
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

bool ProviderBroker::get_synthetic_metrics_snapshot_for_host(SyntheticMetricsSnapshot& out) const {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
    out = syn->get_metrics_snapshot_for_host();
    return true;
  }
#endif
  (void)out;
  return false;
}

bool ProviderBroker::get_synthetic_staged_rig_topology_for_host(std::vector<SyntheticStagedRigTopology>& out) const {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
    out = syn->get_staged_rig_topology_for_host();
    return true;
  }
#endif
  (void)out;
  return false;
}

} // namespace cambang

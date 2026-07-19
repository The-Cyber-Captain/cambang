#include "imaging/broker/provider_broker.h"

#include <mutex>
#include <string>
#include <utility>

#include "imaging/api/provider_error_string.h"

// (No broker-level pattern switching; picture is stream-scoped.)

#if defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
  #include "imaging/stub/provider.h"
#endif

// Windows platform provider (windows_winrt family, compile-time selection).
#if defined(CAMBANG_PROVIDER_WINDOWS_WINRT) && CAMBANG_PROVIDER_WINDOWS_WINRT
  #include "imaging/platform/windows/winrt_camera_provider.h"
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

// Non-owning and valid only for the dynamic extent of a leased provider call.
// This prevents recursive provider/lifecycle entry from waiting on its own
// serialization lease.
thread_local const ProviderBroker* current_provider_call_broker = nullptr;

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

ProviderBroker::ActiveProviderCall::~ActiveProviderCall() {
  if (broker_) {
    broker_->release_active_provider_call_(*this);
  }
}

ProviderResult ProviderBroker::acquire_active_provider_call_(
    ActiveProviderCall& call) const {
  if (current_provider_call_broker == this) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }

  {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    if (provider_lifecycle_state_ == ProviderLifecycleState::ShuttingDown) {
      return ProviderResult::failure(ProviderError::ERR_SHUTTING_DOWN);
    }
    if (provider_lifecycle_state_ != ProviderLifecycleState::Active) {
      return err_not_initialized();
    }
    if (provider_call_admission_closed_) {
      return ProviderResult::failure(ProviderError::ERR_SHUTTING_DOWN);
    }
    if (!active_) {
      return err_no_active();
    }

    ++active_provider_call_count_;
    call.broker_ = this;
    call.provider_ = active_.get();
  }

  try {
    call.provider_call_lock_ =
        std::unique_lock<std::mutex>(provider_call_mutex_);
  } catch (...) {
    release_active_provider_call_(call);
    throw;
  }
  call.previous_provider_call_broker_ = current_provider_call_broker;
  current_provider_call_broker = this;
  return ProviderResult::success();
}

void ProviderBroker::release_active_provider_call_(
    ActiveProviderCall& call) const noexcept {
  if (call.provider_call_lock_.owns_lock()) {
    current_provider_call_broker = call.previous_provider_call_broker_;
    call.provider_call_lock_.unlock();
  }

  {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    if (active_provider_call_count_ > 0) {
      --active_provider_call_count_;
    }
    call.broker_ = nullptr;
    call.previous_provider_call_broker_ = nullptr;
    call.provider_ = nullptr;
    if (active_provider_call_count_ == 0) {
      active_provider_calls_drained_.notify_all();
    }
  }
}

ProviderResult ProviderBroker::close_and_drain_active_provider_(
    std::unique_ptr<ICameraProvider>& provider_to_shutdown) {
  if (current_provider_call_broker == this) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }

  std::unique_lock<std::mutex> lock(active_provider_mutex_);
  if (provider_lifecycle_state_ == ProviderLifecycleState::ShuttingDown) {
    return ProviderResult::failure(ProviderError::ERR_SHUTTING_DOWN);
  }
  if (provider_lifecycle_state_ == ProviderLifecycleState::Initializing) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }
  if (provider_lifecycle_state_ != ProviderLifecycleState::Active) {
    return err_not_initialized();
  }

  provider_lifecycle_state_ = ProviderLifecycleState::ShuttingDown;
  provider_call_admission_closed_ = true;
  active_provider_calls_drained_.wait(
      lock, [this] { return active_provider_call_count_ == 0; });

  provider_to_shutdown = std::move(active_);
  callbacks_ = nullptr;
  return ProviderResult::success();
}

ProviderBroker::~ProviderBroker() {
  std::unique_ptr<ICameraProvider> provider_to_shutdown;
  (void)close_and_drain_active_provider_(provider_to_shutdown);
  if (provider_to_shutdown) {
    try {
      (void)provider_to_shutdown->shutdown();
    } catch (...) {
      // Provider failures cannot escape this noexcept destructor boundary.
    }
  }
}

const char* ProviderBroker::provider_name() const {
  // The broker is the core-bound facade; report active backend for logs.
  ActiveProviderCall call;
  if (acquire_active_provider_call_(call).ok()) {
    return call.provider()->provider_name();
  }
  return "broker(uninitialized)";
}

ProviderKind ProviderBroker::provider_kind() const noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  return mode_latched_ == RuntimeMode::synthetic ? ProviderKind::synthetic
                                                  : ProviderKind::platform_backed;
}

StreamTemplate ProviderBroker::stream_template() const {
  ActiveProviderCall call;
  if (acquire_active_provider_call_(call).ok()) {
    return call.provider()->stream_template();
  }
  return StreamTemplate{};
}

CaptureTemplate ProviderBroker::capture_template() const {
  ActiveProviderCall call;
  if (acquire_active_provider_call_(call).ok()) {
    return call.provider()->capture_template();
  }
  return CaptureTemplate{};
}

bool ProviderBroker::supports_stream_picture_updates() const noexcept {
  ActiveProviderCall call;
  return acquire_active_provider_call_(call).ok()
             ? call.provider()->supports_stream_picture_updates()
             : false;
}

bool ProviderBroker::supports_capture_picture_updates() const noexcept {
  ActiveProviderCall call;
  return acquire_active_provider_call_(call).ok()
             ? call.provider()->supports_capture_picture_updates()
             : false;
}

bool ProviderBroker::supports_multi_image_still_sequence() const noexcept {
  ActiveProviderCall call;
  return acquire_active_provider_call_(call).ok()
             ? call.provider()->supports_multi_image_still_sequence()
             : false;
}

ProducerBackingCapabilities ProviderBroker::stream_backing_capabilities(
    const CaptureProfile& profile,
    const PictureConfig& picture) const noexcept {
  ActiveProviderCall call;
  return acquire_active_provider_call_(call).ok()
             ? call.provider()->stream_backing_capabilities(profile, picture)
             : ProducerBackingCapabilities{false, false};
}

ProducerBackingCapabilities ProviderBroker::capture_backing_capabilities(
    const CaptureRequest& req) const noexcept {
  ActiveProviderCall call;
  return acquire_active_provider_call_(call).ok()
             ? call.provider()->capture_backing_capabilities(req)
             : ProducerBackingCapabilities{false, false};
}

ProducerBackingCapabilities ProviderBroker::stream_parent_context_backing_capabilities(
    uint64_t device_instance_id,
    uint64_t stream_id,
    StreamIntent intent,
    const CaptureProfile& profile,
    const PictureConfig& picture) noexcept {
  ActiveProviderCall call;
  return acquire_active_provider_call_(call).ok()
             ? call.provider()->stream_parent_context_backing_capabilities(
                   device_instance_id, stream_id, intent, profile, picture)
             : ProducerBackingCapabilities{false, false};
}

ProducerBackingCapabilities ProviderBroker::capture_parent_context_backing_capabilities(
    uint64_t device_instance_id,
    const CaptureRequest& req) noexcept {
  ActiveProviderCall call;
  return acquire_active_provider_call_(call).ok()
             ? call.provider()->capture_parent_context_backing_capabilities(
                   device_instance_id, req)
             : ProducerBackingCapabilities{false, false};
}

uint64_t ProviderBroker::stream_backing_plan_evaluation_settle_delay_ns() const noexcept {
  ActiveProviderCall call;
  return acquire_active_provider_call_(call).ok()
             ? call.provider()->stream_backing_plan_evaluation_settle_delay_ns()
             : 0;
}

uint64_t ProviderBroker::capture_backing_plan_evaluation_settle_delay_ns() const noexcept {
  ActiveProviderCall call;
  return acquire_active_provider_call_(call).ok()
             ? call.provider()->capture_backing_plan_evaluation_settle_delay_ns()
             : 0;
}

uint64_t ProviderBroker::capture_admission_watchdog_timeout_ns() const noexcept {
  ActiveProviderCall call;
  return acquire_active_provider_call_(call).ok()
             ? call.provider()->capture_admission_watchdog_timeout_ns()
             : kDefaultCaptureAdmissionWatchdogTimeoutNs;
}

ProviderResult ProviderBroker::update_stream_retained_production_plan(
    uint64_t stream_id,
    CoreRetainedProductionPlan requested_retained_plan) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->update_stream_retained_production_plan(
      stream_id, requested_retained_plan);
}

ProviderResult ProviderBroker::check_mode_supported_in_build(RuntimeMode mode) noexcept {
  switch (mode) {
    case RuntimeMode::platform_backed: {
#if (defined(CAMBANG_PROVIDER_WINDOWS_WINRT) && CAMBANG_PROVIDER_WINDOWS_WINRT) || \
    (defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB)
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
#if defined(CAMBANG_PROVIDER_WINDOWS_WINRT) && CAMBANG_PROVIDER_WINDOWS_WINRT
      return WinrtCameraProvider::check_access_readiness();
#elif defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
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
  if (provider_lifecycle_state_ != ProviderLifecycleState::Uninitialized) {
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
  if (provider_lifecycle_state_ != ProviderLifecycleState::Uninitialized) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }
  synthetic_role_requested_ = role;
  return ProviderResult::success();
}

ProviderResult ProviderBroker::set_synthetic_timing_driver_requested(TimingDriver timing_driver) noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (provider_lifecycle_state_ != ProviderLifecycleState::Uninitialized) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }
  timing_driver_requested_ = timing_driver;
  return ProviderResult::success();
}

ProviderResult ProviderBroker::set_synthetic_timeline_reconciliation_requested(TimelineReconciliation reconciliation) noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (provider_lifecycle_state_ != ProviderLifecycleState::Uninitialized) {
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
  if (provider_lifecycle_state_ != ProviderLifecycleState::Uninitialized) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }
  producer_output_form_mode_requested_ = mode;
  return ProviderResult::success();
}

ProviderResult ProviderBroker::set_synthetic_stream_capability_downgrade_conditions_requested(
    std::vector<SyntheticStreamCapabilityDowngradeCondition> conditions) noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (provider_lifecycle_state_ != ProviderLifecycleState::Uninitialized) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }
  stream_capability_downgrade_conditions_requested_ = std::move(conditions);
  return ProviderResult::success();
}

ProviderResult ProviderBroker::set_synthetic_capture_capability_downgrade_conditions_requested(
    std::vector<SyntheticCaptureCapabilityDowngradeCondition> conditions) noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  if (provider_lifecycle_state_ != ProviderLifecycleState::Uninitialized) {
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

void ProviderBroker::install_synthetic_timeline_request_dispatch_hook_(
    ICameraProvider* provider) {
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(provider)) {
    syn->set_timeline_request_dispatch_hook_for_host(
        [this](const SyntheticScheduledEvent& ev) { dispatch_synthetic_timeline_request_(ev); });
  }
#else
  (void)provider;
#endif
}

void ProviderBroker::set_synthetic_timeline_request_dispatch_hook(
    std::function<void(const SyntheticScheduledEvent&)> hook) {
  {
    std::lock_guard<std::mutex> lock(synthetic_timeline_dispatch_mutex_);
    synthetic_timeline_request_dispatch_hook_ = std::move(hook);
  }

  ActiveProviderCall call;
  if (acquire_active_provider_call_(call).ok()) {
    install_synthetic_timeline_request_dispatch_hook_(call.provider());
  }
}

ProviderResult ProviderBroker::initialize(IProviderCallbacks *callbacks) {
  if (!callbacks) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    if (provider_lifecycle_state_ == ProviderLifecycleState::Active) {
      return ProviderResult::success();
    }
    if (provider_lifecycle_state_ != ProviderLifecycleState::Uninitialized) {
      return ProviderResult::failure(ProviderError::ERR_BUSY);
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
    provider_lifecycle_state_ = ProviderLifecycleState::Initializing;
    provider_call_admission_closed_ = true;
  }

  auto fail_initialization = [this](ProviderResult result) {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    callbacks_ = nullptr;
    provider_lifecycle_state_ = ProviderLifecycleState::Uninitialized;
    provider_call_admission_closed_ = true;
    return result;
  };

  try {
    // Defensive: re-check build support (mirrors server-side validation).
    ProviderResult cap = check_mode_supported_in_build(mode_latched_);
    if (!cap.ok()) {
      return fail_initialization(cap);
    }

    const ProviderAccessStatus access =
        check_mode_access_readiness(mode_latched_);
    if (!access.ok()) {
      return fail_initialization(
          ProviderResult::failure(provider_error_from_access_status(access)));
    }

    std::unique_ptr<ICameraProvider> candidate;
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
      candidate = std::make_unique<SyntheticProvider>(cfg);
#endif
    }

    if (!candidate) {
      // Platform-backed selection is compile-time (SCons platform=...).
      // GDE builds compile exactly one platform family; maintainer-tool
      // builds keep the deterministic StubProvider.
#if defined(CAMBANG_PROVIDER_WINDOWS_WINRT) && CAMBANG_PROVIDER_WINDOWS_WINRT
      candidate = std::make_unique<WinrtCameraProvider>();
#elif defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
      candidate = std::make_unique<StubProvider>();
#endif
    }

    if (!candidate) {
      return fail_initialization(err_no_active());
    }

    install_synthetic_timeline_request_dispatch_hook_(candidate.get());
    ProviderResult pr = candidate->initialize(callbacks);
    if (!pr.ok()) {
      return fail_initialization(pr);
    }

    {
      std::lock_guard<std::mutex> lock(active_provider_mutex_);
      active_ = std::move(candidate);
      callbacks_ = callbacks;
      provider_lifecycle_state_ = ProviderLifecycleState::Active;
      provider_call_admission_closed_ = false;
    }
    return ProviderResult::success();
  } catch (...) {
    return fail_initialization(
        ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED));
  }
}

ProviderResult ProviderBroker::enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->enumerate_endpoints(out_endpoints);
}

ProviderResult ProviderBroker::open_device(
    const std::string& hardware_id,
    uint64_t device_instance_id,
    uint64_t root_id) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->open_device(hardware_id, device_instance_id, root_id);
}

ProviderResult ProviderBroker::close_device(uint64_t device_instance_id) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->close_device(device_instance_id);
}

ProviderResult ProviderBroker::create_stream(const StreamRequest& req) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->create_stream(req);
}

ProviderResult ProviderBroker::destroy_stream(uint64_t stream_id) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->destroy_stream(stream_id);
}

ProviderResult ProviderBroker::start_stream(
    uint64_t stream_id,
    const CaptureProfile& profile,
    const PictureConfig& picture) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->start_stream(stream_id, profile, picture);
}

ProviderResult ProviderBroker::stop_stream(uint64_t stream_id) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->stop_stream(stream_id);
}

ProviderResult ProviderBroker::set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->set_stream_picture_config(stream_id, picture);
}

ProviderResult ProviderBroker::set_capture_picture_config(uint64_t device_instance_id, const PictureConfig& picture) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->set_capture_picture_config(device_instance_id, picture);
}

ProviderResult ProviderBroker::sync_capture_parent_priming(
    const CaptureRequest& req) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->sync_capture_parent_priming(req);
}

ProviderResult ProviderBroker::release_capture_parent_priming(
    uint64_t device_instance_id) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->release_capture_parent_priming(device_instance_id);
}

ProviderResult ProviderBroker::trigger_capture(const CaptureRequest& req) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->trigger_capture(req);
}

ProviderResult ProviderBroker::trigger_capture_submission(const CaptureSubmission& submission) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->trigger_capture_submission(submission);
}

ProviderResult ProviderBroker::abort_capture(uint64_t capture_id) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->abort_capture(capture_id);
}

ProviderResult ProviderBroker::apply_camera_spec_patch(
    const std::string& hardware_id,
    uint64_t new_camera_spec_version,
    SpecPatchView patch) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->apply_camera_spec_patch(
      hardware_id, new_camera_spec_version, patch);
}

ProviderResult ProviderBroker::apply_imaging_spec_patch(
    uint64_t new_imaging_spec_version,
    SpecPatchView patch) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  return call.provider()->apply_imaging_spec_patch(
      new_imaging_spec_version, patch);
}

ProviderResult ProviderBroker::shutdown() {
  // Close admission before waiting. The condition-variable wait releases the
  // lifecycle mutex, so state queries remain prompt while admitted calls drain.
  // Provider shutdown and destruction both occur after the drain and outside
  // broker locks; initialization remains closed until both are complete.
  std::unique_ptr<ICameraProvider> provider_to_shutdown;
  ProviderResult close_result =
      close_and_drain_active_provider_(provider_to_shutdown);
  if (!close_result.ok()) {
    return close_result;
  }

  if (!provider_to_shutdown) {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    provider_lifecycle_state_ = ProviderLifecycleState::Uninitialized;
    return ProviderResult::success();
  }
  ProviderResult shutdown_result =
      ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  try {
    shutdown_result = provider_to_shutdown->shutdown();
  } catch (...) {
    shutdown_result =
        ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }
  provider_to_shutdown.reset();
  {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    provider_lifecycle_state_ = ProviderLifecycleState::Uninitialized;
  }
  return shutdown_result;
}

#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
ProviderResult ProviderBroker::install_active_provider_for_smoke(
    std::unique_ptr<ICameraProvider> provider,
    IProviderCallbacks* callbacks) {
  if (!provider || !callbacks) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    if (provider_lifecycle_state_ != ProviderLifecycleState::Uninitialized ||
        active_) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    provider_lifecycle_state_ = ProviderLifecycleState::Initializing;
    provider_call_admission_closed_ = true;
  }

  ProviderResult pr = ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  try {
    install_synthetic_timeline_request_dispatch_hook_(provider.get());
    pr = provider->initialize(callbacks);
  } catch (...) {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    provider_lifecycle_state_ = ProviderLifecycleState::Uninitialized;
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }
  if (!pr.ok()) {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    provider_lifecycle_state_ = ProviderLifecycleState::Uninitialized;
    return pr;
  }

  {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    active_ = std::move(provider);
    callbacks_ = callbacks;
    provider_lifecycle_state_ = ProviderLifecycleState::Active;
    provider_call_admission_closed_ = false;
  }
  return ProviderResult::success();
}

bool ProviderBroker::provider_call_admission_closed_for_smoke() const noexcept {
  std::lock_guard<std::mutex> lock(active_provider_mutex_);
  return provider_call_admission_closed_;
}
#endif

bool ProviderBroker::try_tick_virtual_time(uint64_t dt_ns, bool flush_strand) {
  std::vector<SyntheticScheduledEvent> deferred_dispatches;
  std::function<void(const SyntheticScheduledEvent&)> hook;

  bool tick_result = false;
  {
    ActiveProviderCall call;
    if (!acquire_active_provider_call_(call).ok()) {
      return false;
    }

    // Synthetic virtual_time driver.
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
    if (auto* syn = dynamic_cast<SyntheticProvider*>(call.provider())) {
      {
        std::lock_guard<std::mutex> dispatch_lock(synthetic_timeline_dispatch_mutex_);
        deferring_synthetic_timeline_dispatches_ = true;
        deferred_synthetic_timeline_dispatches_.clear();
      }
      syn->advance(dt_ns, /*allow_paused_timeline_step=*/false, flush_strand);
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
    if (auto* stub = dynamic_cast<StubProvider*>(call.provider())) {
      stub->advance(dt_ns);
      tick_result = true;
    } else
#endif
    {
      tick_result = false;
    }
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
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(call.provider())) {
    return syn->set_timeline_scenario_for_host(scenario);
  }
#endif
  (void)scenario;
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult ProviderBroker::set_timeline_canonical_scenario_for_host(const SyntheticCanonicalScenario& scenario) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(call.provider())) {
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
    ActiveProviderCall call;
    ProviderResult pr = acquire_active_provider_call_(call);
    if (!pr.ok()) {
      return pr;
    }
    profile = call.provider()->stream_template().profile;
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

  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
  if (auto* syn = dynamic_cast<SyntheticProvider*>(call.provider())) {
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
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(call.provider())) {
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
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(call.provider())) {
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
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(call.provider())) {
    return syn->start_timeline_scenario_for_host();
  }
#endif
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult ProviderBroker::stop_timeline_scenario_for_host() {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(call.provider())) {
    return syn->stop_timeline_scenario_for_host();
  }
#endif
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult ProviderBroker::set_timeline_scenario_paused_for_host(bool paused) {
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(call.provider())) {
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
      ActiveProviderCall call;
      ProviderResult pr = acquire_active_provider_call_(call);
      if (!pr.ok()) {
        return pr;
      }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
      if (auto* syn = dynamic_cast<SyntheticProvider*>(call.provider())) {
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
  ActiveProviderCall call;
  ProviderResult pr = acquire_active_provider_call_(call);
  if (!pr.ok()) {
    return pr;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(call.provider())) {
    return syn->set_timeline_reconciliation_for_host(reconciliation);
  }
#endif
  (void)reconciliation;
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

bool ProviderBroker::get_synthetic_metrics_snapshot_for_host(SyntheticMetricsSnapshot& out) const {
  ActiveProviderCall call;
  if (!acquire_active_provider_call_(call).ok()) {
    return false;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(call.provider())) {
    out = syn->get_metrics_snapshot_for_host();
    return true;
  }
#endif
  (void)out;
  return false;
}

bool ProviderBroker::get_synthetic_staged_rig_topology_for_host(std::vector<SyntheticStagedRigTopology>& out) const {
  ActiveProviderCall call;
  if (!acquire_active_provider_call_(call).ok()) {
    return false;
  }
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(call.provider())) {
    out = syn->get_staged_rig_topology_for_host();
    return true;
  }
#endif
  (void)out;
  return false;
}

} // namespace cambang

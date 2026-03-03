#include "imaging/broker/provider_broker.h"

#include <cstdlib>
#include <cstring>

#include "imaging/api/provider_error_string.h"

#include "pixels/pattern/active_pattern_config.h"

// Platform-backed providers compiled into this artifact.
#if defined(CAMBANG_PROVIDER_WINDOWS_MF) && CAMBANG_PROVIDER_WINDOWS_MF
  #include "imaging/platform/windows/provider.h"
#endif

#if defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
  #include "imaging/stub/provider.h"
#endif

// Synthetic provider (optional compile).
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  #include "imaging/synthetic/provider.h"
  #include "imaging/synthetic/config.h"
#endif

namespace cambang {

namespace {

ProviderResult err_not_initialized() {
  return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
}

ProviderResult err_no_active() {
  return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
}

} // namespace

ProviderBroker::ProviderBroker() = default;

ProviderBroker::~ProviderBroker() {
  // Best-effort: ensure deterministic shutdown when broker is destroyed.
  // Core should call shutdown(), but dev scaffolding may drop the provider early.
  if (initialized_ && active_) {
    (void)active_->shutdown();
  }
}

const char* ProviderBroker::provider_name() const {
  // The broker is the core-bound facade; report active backend for logs.
  if (active_) {
    return active_->provider_name();
  }
  return "broker(uninitialized)";
}

RuntimeMode ProviderBroker::read_provider_mode_from_env_() {
  // Canonical env var:
  //   CAMBANG_PROVIDER_MODE = platform_backed | synthetic
  // Default is platform_backed.
  const char* v = std::getenv("CAMBANG_PROVIDER_MODE");
  if (!v || !*v) {
    return RuntimeMode::platform_backed;
  }
  if (std::strcmp(v, "platform_backed") == 0) {
    return RuntimeMode::platform_backed;
  }
  if (std::strcmp(v, "synthetic") == 0) {
    return RuntimeMode::synthetic;
  }
  // Unknown value: be deterministic and fall back to platform_backed.
  return RuntimeMode::platform_backed;
}

ProviderResult ProviderBroker::initialize(IProviderCallbacks* callbacks) {
  if (initialized_) {
    return ProviderResult::success();
  }
  callbacks_ = callbacks;
  if (!callbacks_) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  mode_latched_ = read_provider_mode_from_env_();

  // Construct backend.
  if (mode_latched_ == RuntimeMode::synthetic) {
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
    SyntheticProviderConfig cfg{};
    // Defaults: nominal role, virtual_time driver (first landing).
    cfg.synthetic_role = SyntheticRole::Nominal;
    cfg.timing_driver = TimingDriver::VirtualTime;
    active_ = std::make_unique<SyntheticProvider>(cfg);
#else
    // Synthetic requested but not compiled: deterministic fallback.
    mode_latched_ = RuntimeMode::platform_backed;
#endif
  }

  if (!active_) {
    // Platform-backed selection is compile-time (SCons provider=...).
#if defined(CAMBANG_PROVIDER_WINDOWS_MF) && CAMBANG_PROVIDER_WINDOWS_MF
    active_ = std::make_unique<WindowsProvider>();
#elif defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
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
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->open_device(hardware_id, device_instance_id, root_id);
}

ProviderResult ProviderBroker::close_device(uint64_t device_instance_id) {
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->close_device(device_instance_id);
}

ProviderResult ProviderBroker::create_stream(const StreamRequest& req) {
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->create_stream(req);
}

ProviderResult ProviderBroker::destroy_stream(uint64_t stream_id) {
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->destroy_stream(stream_id);
}

ProviderResult ProviderBroker::start_stream(uint64_t stream_id) {
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->start_stream(stream_id);
}

ProviderResult ProviderBroker::stop_stream(uint64_t stream_id) {
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->stop_stream(stream_id);
}

ProviderResult ProviderBroker::trigger_capture(const CaptureRequest& req) {
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->trigger_capture(req);
}

ProviderResult ProviderBroker::abort_capture(uint64_t capture_id) {
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
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->apply_camera_spec_patch(hardware_id, new_camera_spec_version, patch);
}

ProviderResult ProviderBroker::apply_imaging_spec_patch(
    uint64_t new_imaging_spec_version,
    SpecPatchView patch) {
  ProviderResult pr = ensure_active_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  return active_->apply_imaging_spec_patch(new_imaging_spec_version, patch);
}

ProviderResult ProviderBroker::shutdown() {
  ProviderResult pr = ensure_initialized_or_err_();
  if (!pr.ok()) {
    return pr;
  }
  if (active_) {
    ProviderResult spr = active_->shutdown();
    active_.reset();
    initialized_ = false;
    callbacks_ = nullptr;
    return spr;
  }
  initialized_ = false;
  callbacks_ = nullptr;
  return ProviderResult::success();
}

bool ProviderBroker::try_tick_virtual_time(uint64_t dt_ns) {
  if (!initialized_ || !active_) {
    return false;
  }

  // Synthetic virtual_time driver.
#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
    syn->advance(dt_ns);
    return true;
  }
#endif

  // Stub heartbeat driver.
#if defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
  if (auto* stub = dynamic_cast<StubProvider*>(active_.get())) {
    stub->advance(dt_ns);
    return true;
  }
#endif

  return false;
}


bool ProviderBroker::try_set_active_pattern(const ActivePatternConfig& cfg) {
  if (!initialized_ || !active_) {
    return false;
  }

#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* syn = dynamic_cast<SyntheticProvider*>(active_.get())) {
    syn->set_active_pattern_config(cfg);
    return true;
  }
#endif

#if defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
  if (auto* stub = dynamic_cast<StubProvider*>(active_.get())) {
    stub->set_active_pattern_config(cfg);
    return true;
  }
#endif

  return false;
}

} // namespace cambang

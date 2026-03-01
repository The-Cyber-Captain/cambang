#include "imaging/broker/provider_broker.h"

#include "imaging/api/provider_contract_datatypes.h"

#if defined(CAMBANG_PROVIDER_WINDOWS_MF) && CAMBANG_PROVIDER_WINDOWS_MF
  #include "imaging/platform/windows/provider.h"
#endif

#if defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
  #include "imaging/stub/provider.h"
#endif

#if defined(CAMBANG_ENABLE_SYNTHETIC)
  #include "imaging/synthetic/provider.h"
#endif

#include <godot_cpp/variant/utility_functions.hpp>

namespace cambang {

ProviderBroker::ProviderBroker() = default;

ProviderBroker::~ProviderBroker() {
  // Best-effort deterministic shutdown.
  (void)shutdown();
}

const char* ProviderBroker::provider_name() const {
  // Keep it stable; underlying provider name is still accessible via logs.
  return name_;
}

ProviderResult ProviderBroker::select_provider_(RuntimeMode mode) {
  // Always destroy any existing provider before selecting another.
  active_.reset();
  initialized_ = false;
  active_mode_ = mode;

  if (mode == RuntimeMode::synthetic) {
#if defined(CAMBANG_ENABLE_SYNTHETIC)
    active_ = std::make_unique<SyntheticProvider>();
    return ProviderResult::success();
#else
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
#endif
  }

  // platform_backed
#if defined(CAMBANG_PROVIDER_WINDOWS_MF) && CAMBANG_PROVIDER_WINDOWS_MF
  active_ = std::make_unique<WindowsProvider>();
  return ProviderResult::success();
#elif defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
  active_ = std::make_unique<StubProvider>();
  return ProviderResult::success();
#else
  // No platform provider compiled in for this build.
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
#endif
}

ProviderResult ProviderBroker::ensure_selected_and_initialized_(IProviderCallbacks* callbacks)
{
  if (initialized_) {
    return ProviderResult::success();
  }

  const RuntimeMode mode = resolve_runtime_mode();
  ProviderResult pr = select_provider_(mode);
  if (!pr.ok()) {
    return pr;
  }
  if (!active_) {
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }

  pr = active_->initialize(callbacks);
  if (!pr.ok()) {
    active_.reset();
    return pr;
  }

  // Startup log: exactly once per core run (this function returns early if initialized_ is true).
  const char* mode_str = (active_mode_ == RuntimeMode::synthetic) ? "synthetic" : "platform_backed";
  const char* impl_str = active_ ? active_->provider_name() : "null";
  godot::UtilityFunctions::print("[CamBANG] provider mode = ", mode_str, " (", impl_str, ")");

  initialized_ = true;
  return ProviderResult::success();
}

ProviderResult ProviderBroker::initialize(IProviderCallbacks* callbacks) {
  if (!callbacks) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  // Resolve mode and initialize active provider. This freezes mode for this core run.
  return ensure_selected_and_initialized_(callbacks);
}

ProviderResult ProviderBroker::enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) {
  if (!active_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return active_->enumerate_endpoints(out_endpoints);
}

ProviderResult ProviderBroker::open_device(const std::string& hardware_id, uint64_t device_instance_id, uint64_t root_id) {
  if (!active_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return active_->open_device(hardware_id, device_instance_id, root_id);
}

ProviderResult ProviderBroker::close_device(uint64_t device_instance_id) {
  if (!active_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return active_->close_device(device_instance_id);
}

ProviderResult ProviderBroker::create_stream(const StreamRequest& req) {
  if (!active_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return active_->create_stream(req);
}

ProviderResult ProviderBroker::destroy_stream(uint64_t stream_id) {
  if (!active_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return active_->destroy_stream(stream_id);
}

ProviderResult ProviderBroker::start_stream(uint64_t stream_id) {
  if (!active_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return active_->start_stream(stream_id);
}

ProviderResult ProviderBroker::stop_stream(uint64_t stream_id) {
  if (!active_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return active_->stop_stream(stream_id);
}

ProviderResult ProviderBroker::trigger_capture(const CaptureRequest& req) {
  if (!active_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return active_->trigger_capture(req);
}

ProviderResult ProviderBroker::abort_capture(uint64_t capture_id) {
  if (!active_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return active_->abort_capture(capture_id);
}

ProviderResult ProviderBroker::apply_camera_spec_patch(const std::string& hardware_id,
                                                      uint64_t new_camera_spec_version,
                                                      SpecPatchView patch) {
  if (!active_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return active_->apply_camera_spec_patch(hardware_id, new_camera_spec_version, patch);
}

ProviderResult ProviderBroker::apply_imaging_spec_patch(uint64_t new_imaging_spec_version,
                                                       SpecPatchView patch) {
  if (!active_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return active_->apply_imaging_spec_patch(new_imaging_spec_version, patch);
}

ProviderResult ProviderBroker::shutdown() {
  if (active_) {
    ProviderResult pr = active_->shutdown();
    active_.reset();
    initialized_ = false;
    return pr;
  }
  return ProviderResult::success();
}

#if defined(CAMBANG_ENABLE_DEV_NODES)
ProviderResult ProviderBroker::dev_emit_test_frames(uint64_t stream_id, uint32_t count) {
#if defined(CAMBANG_PROVIDER_STUB) && CAMBANG_PROVIDER_STUB
  if (!active_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  // StubProvider defines this method; other providers do not.
  auto* stub = dynamic_cast<StubProvider*>(active_.get());
  if (!stub) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  stub->emit_test_frames(stream_id, count);
  return ProviderResult::success();
#else
  (void)stream_id;
  (void)count;
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
#endif
}
#endif

} // namespace cambang

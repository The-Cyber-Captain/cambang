#include "provider/stub/stub_camera_provider.h"

namespace cambang {

const char* StubCameraProvider::provider_name() const {
  return "StubCameraProvider";
}

bool StubCameraProvider::is_known_hardware_id(const std::string& hardware_id) const {
  return hardware_id == kStubHardwareId;
}

ProviderResult StubCameraProvider::initialize(IProviderCallbacks* callbacks) {
  if (initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (!callbacks) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  callbacks_ = callbacks;
  initialized_ = true;
  shutting_down_ = false;
  return ProviderResult::success();
}

ProviderResult StubCameraProvider::enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  out_endpoints.clear();
  CameraEndpoint ep;
  ep.hardware_id = kStubHardwareId;
  ep.name = "Stub Camera 0";
  out_endpoints.push_back(ep);
  return ProviderResult::success();
}

ProviderResult StubCameraProvider::open_device(
    const std::string& hardware_id,
    uint64_t device_instance_id,
    uint64_t root_id) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (!is_known_hardware_id(hardware_id) || device_instance_id == 0 || root_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  auto& dev = devices_[device_instance_id];
  if (dev.open) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }

  dev.hardware_id = hardware_id;
  dev.root_id = root_id;
  dev.open = true;
  dev.stream_id = 0;

  callbacks_->on_device_opened(device_instance_id);
  return ProviderResult::success();
}

ProviderResult StubCameraProvider::close_device(uint64_t device_instance_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  auto it = devices_.find(device_instance_id);
  if (it == devices_.end() || !it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  // If a stream exists, require it to be destroyed first (core should do this).
  if (it->second.stream_id != 0) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  it->second.open = false;
  callbacks_->on_device_closed(device_instance_id);
  return ProviderResult::success();
}

ProviderResult StubCameraProvider::create_stream(const StreamRequest& req) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (req.stream_id == 0 || req.device_instance_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  auto dev_it = devices_.find(req.device_instance_id);
  if (dev_it == devices_.end() || !dev_it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (dev_it->second.stream_id != 0) {
    // Core should ensure one repeating stream per device instance.
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }

  auto& st = streams_[req.stream_id];
  if (st.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  st.req = req;
  st.created = true;
  st.started = false;

  dev_it->second.stream_id = req.stream_id;

  callbacks_->on_stream_created(req.stream_id);
  return ProviderResult::success();
}

ProviderResult StubCameraProvider::destroy_stream(uint64_t stream_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (stream_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  auto st_it = streams_.find(stream_id);
  if (st_it == streams_.end() || !st_it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  // Must be stopped before destroy.
  if (st_it->second.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  // Clear device link.
  const uint64_t dev_id = st_it->second.req.device_instance_id;
  auto dev_it = devices_.find(dev_id);
  if (dev_it != devices_.end() && dev_it->second.stream_id == stream_id) {
    dev_it->second.stream_id = 0;
  }

  streams_.erase(st_it);
  callbacks_->on_stream_destroyed(stream_id);
  return ProviderResult::success();
}

ProviderResult StubCameraProvider::start_stream(uint64_t stream_id) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  auto st_it = streams_.find(stream_id);
  if (st_it == streams_.end() || !st_it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (st_it->second.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  st_it->second.started = true;
  callbacks_->on_stream_started(stream_id);
  return ProviderResult::success();
}

ProviderResult StubCameraProvider::stop_stream(uint64_t stream_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  auto st_it = streams_.find(stream_id);
  if (st_it == streams_.end() || !st_it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (!st_it->second.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  st_it->second.started = false;
  callbacks_->on_stream_stopped(stream_id, ProviderError::OK);
  return ProviderResult::success();
}

ProviderResult StubCameraProvider::trigger_capture(const CaptureRequest& req) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (req.capture_id == 0 || req.device_instance_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  auto dev_it = devices_.find(req.device_instance_id);
  if (dev_it == devices_.end() || !dev_it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  // No frames emitted; just lifecycle notifications.
  callbacks_->on_capture_started(req.capture_id);
  callbacks_->on_capture_completed(req.capture_id);
  return ProviderResult::success();
}

ProviderResult StubCameraProvider::abort_capture(uint64_t /*capture_id*/) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult StubCameraProvider::apply_camera_spec_patch(
    const std::string& hardware_id,
    uint64_t /*new_camera_spec_version*/,
    SpecPatchView /*patch*/) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (!is_known_hardware_id(hardware_id)) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  return ProviderResult::success();
}

ProviderResult StubCameraProvider::apply_imaging_spec_patch(
    uint64_t /*new_imaging_spec_version*/,
    SpecPatchView /*patch*/) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return ProviderResult::success();
}

ProviderResult StubCameraProvider::shutdown() {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (shutting_down_) {
    return ProviderResult::success();
  }

  shutting_down_ = true;

  // Stop any started streams (deterministic order due to std::map).
  for (auto& [stream_id, st] : streams_) {
    if (st.started) {
      st.started = false;
      callbacks_->on_stream_stopped(stream_id, ProviderError::OK);
    }
  }

  // Destroy all streams.
  for (auto it = streams_.begin(); it != streams_.end(); ) {
    const uint64_t stream_id = it->first;
    const uint64_t dev_id = it->second.req.device_instance_id;

    auto dev_it = devices_.find(dev_id);
    if (dev_it != devices_.end() && dev_it->second.stream_id == stream_id) {
      dev_it->second.stream_id = 0;
    }

    it = streams_.erase(it);
    callbacks_->on_stream_destroyed(stream_id);
  }

  // Close all devices.
  for (auto& [dev_id, dev] : devices_) {
    if (dev.open) {
      dev.open = false;
      callbacks_->on_device_closed(dev_id);
    }
  }

  return ProviderResult::success();
}

} // namespace cambang
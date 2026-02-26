#include "provider/stub/stub_camera_provider.h"

#include <cstdint>
#include <vector>

namespace cambang {

namespace {

struct TestFramePayload final {
  StubCameraProvider* self = nullptr;
  std::vector<std::uint8_t> bytes;
};

// NOTE: Prefer the canonical helpers/constants in provider_contract_datatypes.h.

} // namespace

const char* StubCameraProvider::provider_name() const {
  return "StubCameraProvider";
}

bool StubCameraProvider::is_known_hardware_id(const std::string& hardware_id) const {
  return hardware_id == kStubHardwareId;
}

void StubCameraProvider::release_test_frame(void* user, const FrameView* /*frame*/) {
  auto* payload = static_cast<TestFramePayload*>(user);
  if (!payload) {
    return;
  }
  if (payload->self) {
    payload->self->frames_released_.fetch_add(1, std::memory_order_relaxed);
  }
  delete payload;
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
  st.frame_index = 0;

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

  // Emit exactly one test frame immediately to prove end-to-end plumbing.
  emit_test_frames(stream_id, 1);
  return ProviderResult::success();
}

void StubCameraProvider::emit_test_frames(uint64_t stream_id, uint32_t count) {
  if (!callbacks_) {
    return;
  }
  auto st_it = streams_.find(stream_id);
  if (st_it == streams_.end() || !st_it->second.created) {
    return;
  }

  auto& st = st_it->second;
  if (!st.started) {
    return;
  }

  const uint32_t w = (st.req.width == 0) ? 320 : st.req.width;
  const uint32_t h = (st.req.height == 0) ? 180 : st.req.height;

  const uint32_t fmt = (st.req.format_fourcc == 0) ? FOURCC_RGBA : st.req.format_fourcc;

  // This stub provider currently only supports RGBA test frames.
  if (fmt != FOURCC_RGBA) {
    return;
  }

  const size_t row_bytes = static_cast<size_t>(w) * 4u;
  const size_t total = row_bytes * static_cast<size_t>(h);

  for (uint32_t i = 0; i < count; ++i) {
    frames_emitted_.fetch_add(1, std::memory_order_relaxed);
    const uint64_t fi = st.frame_index++;

    auto* payload = new TestFramePayload();
    payload->self = this;
    payload->bytes.resize(total);

    // Simple moving pattern: horizontal gradient with a moving vertical bar.
    const uint32_t bar_x = static_cast<uint32_t>((fi * 4) % (w ? w : 1));
    for (uint32_t y = 0; y < h; ++y) {
      uint8_t* row = payload->bytes.data() + static_cast<size_t>(y) * row_bytes;
      for (uint32_t x = 0; x < w; ++x) {
        const uint8_t r = static_cast<uint8_t>((x + fi) & 0xFF);
        const uint8_t g = static_cast<uint8_t>((y + (fi >> 1)) & 0xFF);
        const uint8_t b = static_cast<uint8_t>(((x ^ y) + (fi >> 2)) & 0xFF);
        const bool bar = (x == bar_x);

        const size_t o = static_cast<size_t>(x) * 4u;
        row[o + 0] = bar ? 0xFF : r;
        row[o + 1] = bar ? 0xFF : g;
        row[o + 2] = bar ? 0xFF : b;
        row[o + 3] = 0xFF;
      }
    }

    FrameView fv{};
    fv.device_instance_id = st.req.device_instance_id;
    fv.stream_id = stream_id;
    fv.capture_id = 0;

    fv.width = w;
    fv.height = h;
    fv.format_fourcc = FOURCC_RGBA;

    fv.capture_timestamp.value = 0;
    fv.capture_timestamp.tick_ns = 0;
    fv.capture_timestamp.domain = CaptureTimestampDomain::OPAQUE;

    fv.data = payload->bytes.data();
    fv.size_bytes = payload->bytes.size();
    fv.stride_bytes = static_cast<uint32_t>(row_bytes);

    fv.release = &StubCameraProvider::release_test_frame;
    fv.release_user = payload;

    callbacks_->on_frame(fv);
  }
}

void StubCameraProvider::emit_fact_stream_stopped(uint64_t stream_id, ProviderError error_or_ok) {
  if (!callbacks_) {
    return;
  }
  callbacks_->on_stream_stopped(stream_id, error_or_ok);
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
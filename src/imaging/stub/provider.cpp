#include "imaging/stub/provider.h"

#include <atomic>
#include <cstdint>
#include <vector>
#include "pixels/pattern/pattern_render_target.h"

namespace cambang {

// NOTE: Prefer the canonical helpers/constants in provider_contract_datatypes.h.

const char* StubProvider::provider_name() const {
  return "StubProvider";
}

StreamTemplate StubProvider::stream_template() const {
  StreamTemplate t{};
  t.profile.width = 320;
  t.profile.height = 180;
  t.profile.format_fourcc = FOURCC_RGBA;
  t.profile.target_fps_min = 30;
  t.profile.target_fps_max = 30;

  t.picture.preset = PatternPreset::XyXor;
  t.picture.seed = 0;
  t.picture.overlay_frame_index_offsets = true;
  t.picture.overlay_moving_bar = true;
  return t;
}

bool StubProvider::is_known_hardware_id(const std::string& hardware_id) const {
  return hardware_id == kStubHardwareId;
}

uint64_t StubProvider::alloc_native_id_(NativeObjectType type) const {
  if (!callbacks_) {
    return 0;
  }
  return callbacks_->allocate_native_id(type);
}

void StubProvider::emit_native_created_(
    uint64_t native_id,
    NativeObjectType type,
    uint64_t root_id,
    uint64_t owner_device_id,
    uint64_t owner_stream_id) {
  if (!callbacks_ || native_id == 0) {
    return;
  }
  NativeObjectCreateInfo info{};
  info.native_id = native_id;
  info.type = static_cast<uint32_t>(type);
  info.root_id = root_id;
  info.owner_device_instance_id = owner_device_id;
  info.owner_stream_id = owner_stream_id;
  info.created_ns = 0;
  strand_.post_native_object_created(info);
}

void StubProvider::emit_native_destroyed_(uint64_t native_id) {
  if (!callbacks_ || native_id == 0) {
    return;
  }
  NativeObjectDestroyInfo info{};
  info.native_id = native_id;
  info.destroyed_ns = 0;
  strand_.post_native_object_destroyed(info);
}

void StubProvider::release_test_frame(void* user, const FrameView* /*frame*/) {
  auto* slot = static_cast<StreamState::BufferSlot*>(user);
  if (!slot) return;
  if (slot->owner) {
    slot->owner->frames_released_.fetch_add(1, std::memory_order_relaxed);
  }
  slot->in_use = false;
}

ProviderResult StubProvider::initialize(IProviderCallbacks* callbacks) {
  if (initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (!callbacks) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  callbacks_ = callbacks;
  strand_.start(callbacks_, "stub_provider");
  provider_native_id_ = alloc_native_id_(NativeObjectType::Provider);
  emit_native_created_(provider_native_id_, NativeObjectType::Provider, 0, 0, 0);
  initialized_ = true;
  shutting_down_ = false;

  return ProviderResult::success();
}

ProviderResult StubProvider::enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) {
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

ProviderResult StubProvider::open_device(
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
  dev.native_id = alloc_native_id_(NativeObjectType::Device);

  emit_native_created_(dev.native_id, NativeObjectType::Device, root_id, device_instance_id, 0);
  strand_.post_device_opened(device_instance_id);
  return ProviderResult::success();
}

ProviderResult StubProvider::close_device(uint64_t device_instance_id) {
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
  const uint64_t native_id = it->second.native_id;
  strand_.post_device_closed(device_instance_id);
  emit_native_destroyed_(native_id);
  it->second.native_id = 0;
  return ProviderResult::success();
}

ProviderResult StubProvider::create_stream(const StreamRequest& req) {
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
  st.picture = req.picture;
  st.created = true;
  st.started = false;
  st.frame_index = 0;
  st.native_id = alloc_native_id_(NativeObjectType::Stream);
  st.frame_producer_native_id = 0;
  st.pool.clear();
  st.pool_cursor = 0;

  dev_it->second.stream_id = req.stream_id;

  emit_native_created_(st.native_id, NativeObjectType::Stream, dev_it->second.root_id, req.device_instance_id, req.stream_id);
  strand_.post_stream_created(req.stream_id);
  return ProviderResult::success();
}

ProviderResult StubProvider::destroy_stream(uint64_t stream_id) {
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
  if (st_it->second.frame_producer_native_id != 0) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  // Clear device link.
  const uint64_t dev_id = st_it->second.req.device_instance_id;
  auto dev_it = devices_.find(dev_id);
  if (dev_it != devices_.end() && dev_it->second.stream_id == stream_id) {
    dev_it->second.stream_id = 0;
  }

  const uint64_t native_id = st_it->second.native_id;
  streams_.erase(st_it);
  strand_.post_stream_destroyed(stream_id);
  emit_native_destroyed_(native_id);
  return ProviderResult::success();
}

ProviderResult StubProvider::start_stream(
    uint64_t stream_id,
    const CaptureProfile& profile,
    const PictureConfig& picture) {
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

  auto& st = st_it->second;

  // Core supplies effective config at start.
  st.req.profile = profile;
  st.picture = picture;

  const uint32_t w = (profile.width == 0) ? 320 : profile.width;
  const uint32_t h = (profile.height == 0) ? 180 : profile.height;
  const uint32_t fmt = (profile.format_fourcc == 0) ? FOURCC_RGBA : profile.format_fourcc;
  if (fmt != FOURCC_RGBA) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }

  // Large enough to allow the smoke overload test to saturate the core ingress queue
  // before slots begin to recycle. This remains a one-time allocation (no per-frame).
  constexpr size_t kPoolSize = 1200;
  const size_t row_bytes = static_cast<size_t>(w) * 4u;
  const size_t total = row_bytes * static_cast<size_t>(h);
  if (st.pool.size() != kPoolSize || (st.pool.empty() ? 0 : st.pool[0].bytes.size()) != total) {
    st.pool.clear();
    st.pool.resize(kPoolSize);
    for (auto& slot : st.pool) {
      slot.owner = this;
      slot.stream_id = stream_id;
      slot.bytes.resize(total);
      slot.in_use = false;
    }
    st.pool_cursor = 0;
  }

  st.started = true;
  uint64_t root_id = 0;
  auto dev_it = devices_.find(st.req.device_instance_id);
  if (dev_it != devices_.end()) {
    root_id = dev_it->second.root_id;
  }
  st.frame_producer_native_id = alloc_native_id_(NativeObjectType::FrameProducer);
  emit_native_created_(
      st.frame_producer_native_id,
      NativeObjectType::FrameProducer,
      root_id,
      st.req.device_instance_id,
      stream_id);
  strand_.post_stream_started(stream_id);

  uint32_t fps = profile.target_fps_max;
  if (fps == 0) fps = 30;
  st.period_ns = 1'000'000'000ull / static_cast<uint64_t>(fps);
  if (st.period_ns == 0) st.period_ns = 33'333'333ull;
  st.next_frame_ns = now_ns_; // allow immediate emission on next advance

  // Emit exactly one test frame immediately to prove end-to-end plumbing.
  emit_test_frames(stream_id, 1);
  return ProviderResult::success();
}

void StubProvider::advance(uint64_t dt_ns) {
  if (!initialized_ || shutting_down_) {
    return;
  }

  now_ns_ += dt_ns;

  // Deterministic due-frame emission for all started streams.
  for (auto& [stream_id, st] : streams_) {
    if (!st.created || !st.started) {
      continue;
    }

    // If no period was established (e.g. older stream records), fall back.
    if (st.period_ns == 0) {
      uint32_t fps = st.req.profile.target_fps_max;
      if (fps == 0) fps = 30;
      st.period_ns = 1'000'000'000ull / static_cast<uint64_t>(fps);
      if (st.period_ns == 0) st.period_ns = 33'333'333ull;
    }

    // Emit all frames due up to now. Bound catch-up to avoid pathological bursts
    // if the host stalls; this is a stub heartbeat, not a backlog replayer.
    uint32_t emitted = 0;
    constexpr uint32_t kMaxCatchupFrames = 4;
    while (st.next_frame_ns <= now_ns_ && emitted < kMaxCatchupFrames) {
      emit_test_frames(stream_id, 1);
      st.next_frame_ns += st.period_ns;
      ++emitted;
    }

    if (st.next_frame_ns <= now_ns_) {
      // Skip ahead deterministically if we're far behind.
      const uint64_t behind = now_ns_ - st.next_frame_ns;
      const uint64_t skip = (behind / st.period_ns) + 1;
      st.next_frame_ns += skip * st.period_ns;
    }
  }
}

void StubProvider::emit_test_frames(uint64_t stream_id, uint32_t count) {
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

  const uint32_t w = (st.req.profile.width == 0) ? 320 : st.req.profile.width;
  const uint32_t h = (st.req.profile.height == 0) ? 180 : st.req.profile.height;

  const uint32_t fmt = (st.req.profile.format_fourcc == 0) ? FOURCC_RGBA : st.req.profile.format_fourcc;

  // This stub provider currently only supports RGBA test frames.
  if (fmt != FOURCC_RGBA) {
    return;
  }

  const size_t row_bytes = static_cast<size_t>(w) * 4u;
  const size_t total = row_bytes * static_cast<size_t>(h);

  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t fi = st.frame_index++;

    // Acquire a buffer slot (no per-frame allocation).
    StreamState::BufferSlot* slot = nullptr;
    const size_t n = st.pool.size();
    if (n == 0) {
      return;
    }
    for (size_t probe = 0; probe < n; ++probe) {
      const size_t idx = (st.pool_cursor + probe) % n;
      auto& cand = st.pool[idx];
      if (!cand.in_use) {
        cand.in_use = true;
        slot = &cand;
        st.pool_cursor = (idx + 1) % n;
        break;
      }
    }
    if (!slot) {
      // Pool exhausted: drop silently for stub heartbeat semantics.
      break;
    }

    frames_emitted_.fetch_add(1, std::memory_order_relaxed);

    bool preset_valid = true;
    PatternSpec spec = to_pattern_spec(st.picture, w, h, PatternSpec::PackedFormat::RGBA8, &preset_valid);
    if (!preset_valid) {
      invalid_preset_requests_.fetch_add(1, std::memory_order_relaxed);
    }

    PatternRenderTarget dst;
    dst.data = slot->bytes.data();
    dst.size_bytes = slot->bytes.size();
    dst.width = w;
    dst.height = h;
    dst.stride_bytes = static_cast<uint32_t>(row_bytes);
    dst.format = PatternSpec::PackedFormat::RGBA8;

    const uint64_t capture_ts_ns_raw = (st.next_frame_ns != 0) ? st.next_frame_ns : now_ns_;
    const uint64_t capture_ts_ns = (capture_ts_ns_raw == 0) ? 1 : capture_ts_ns_raw;

    PatternOverlayData ov;
    ov.frame_index = fi;
    ov.timestamp_ns = capture_ts_ns;
    ov.stream_id = stream_id;

    st.renderer.render_into(spec, dst, ov);

    FrameView fv{};
    fv.device_instance_id = st.req.device_instance_id;
    fv.stream_id = stream_id;
    fv.capture_id = 0;

    fv.width = w;
    fv.height = h;
    fv.format_fourcc = FOURCC_RGBA;

    fv.capture_timestamp.value = capture_ts_ns;
    fv.capture_timestamp.tick_ns = 1;
    fv.capture_timestamp.domain = CaptureTimestampDomain::PROVIDER_MONOTONIC;

    fv.data = slot->bytes.data();
    fv.size_bytes = slot->bytes.size();
    fv.stride_bytes = static_cast<uint32_t>(row_bytes);

    fv.release = &StubProvider::release_test_frame;
    fv.release_user = slot;

    strand_.post_frame(fv);
  }
}

void StubProvider::emit_fact_stream_stopped(uint64_t stream_id, ProviderError error_or_ok) {
  if (!callbacks_) {
    return;
  }
  strand_.post_stream_stopped(stream_id, error_or_ok);
}

ProviderResult StubProvider::stop_stream(uint64_t stream_id) {
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
  strand_.post_stream_stopped(stream_id, ProviderError::OK);
  emit_native_destroyed_(st_it->second.frame_producer_native_id);
  st_it->second.frame_producer_native_id = 0;
  return ProviderResult::success();
}

ProviderResult StubProvider::set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  auto st_it = streams_.find(stream_id);
  if (st_it == streams_.end() || !st_it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  // Validate preset deterministically; allow invalid but count and fall back in render.
  if (!find_preset_info(picture.preset)) {
    invalid_preset_requests_.fetch_add(1, std::memory_order_relaxed);
  }

  st_it->second.picture = picture;
  return ProviderResult::success();
}

ProviderResult StubProvider::trigger_capture(const CaptureRequest& req) {
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
  strand_.post_capture_started(req.capture_id);
  strand_.post_capture_completed(req.capture_id);
  return ProviderResult::success();
}

ProviderResult StubProvider::abort_capture(uint64_t /*capture_id*/) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult StubProvider::apply_camera_spec_patch(
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

ProviderResult StubProvider::apply_imaging_spec_patch(
    uint64_t /*new_imaging_spec_version*/,
    SpecPatchView /*patch*/) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return ProviderResult::success();
}

ProviderResult StubProvider::shutdown() {
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
      strand_.post_stream_stopped(stream_id, ProviderError::OK);
      emit_native_destroyed_(st.frame_producer_native_id);
      st.frame_producer_native_id = 0;
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

    const uint64_t native_id = it->second.native_id;
    it = streams_.erase(it);
    strand_.post_stream_destroyed(stream_id);
    emit_native_destroyed_(native_id);
  }

  // Close all devices.
  for (auto& [dev_id, dev] : devices_) {
    if (dev.open) {
      dev.open = false;
      strand_.post_device_closed(dev_id);
      emit_native_destroyed_(dev.native_id);
      dev.native_id = 0;
    }
  }

  emit_native_destroyed_(provider_native_id_);
  provider_native_id_ = 0;

  strand_.flush();
  strand_.stop();
  callbacks_ = nullptr;
  initialized_ = false;

  return ProviderResult::success();
}

} // namespace cambang

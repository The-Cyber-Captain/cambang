#include "imaging/synthetic/provider.h"

#include <atomic>
#include <cstring>
#include <memory>

#include "pixels/pattern/active_pattern_config.h"
#include "pixels/pattern/pattern_spec.h"

namespace cambang {

namespace {

constexpr const char* kHardwareIdPrefix = "synthetic:";

uint64_t fps_period_ns(uint32_t fps_num, uint32_t fps_den) {
  if (fps_num == 0 || fps_den == 0) {
    return 0;
  }
  // period = 1s * den / num
  const uint64_t one_sec = 1'000'000'000ull;
  return (one_sec * static_cast<uint64_t>(fps_den)) / static_cast<uint64_t>(fps_num);
}

} // namespace

SyntheticProvider::SyntheticProvider(const SyntheticProviderConfig& cfg) : cfg_(cfg) {
  // Normalize defaults for v1.
  if (cfg_.nominal.format_fourcc == 0) {
    cfg_.nominal.format_fourcc = FOURCC_RGBA;
  }
  if (cfg_.endpoint_count == 0) {
    cfg_.endpoint_count = 1;
  }
}

ProviderResult SyntheticProvider::initialize(IProviderCallbacks* callbacks) {
  if (initialized_) {
    return ProviderResult::success();
  }
  if (!callbacks) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  callbacks_ = callbacks;
  initialized_ = true;
  shutting_down_ = false;

  // Default active selection (deterministic).
  auto cfg_mut = std::make_shared<ActivePatternConfig>();
  cfg_mut->preset = cfg_.pattern.preset;
  cfg_mut->seed = static_cast<uint32_t>(cfg_.pattern.seed);
  cfg_mut->overlay_frame_index_offsets = cfg_.pattern.overlay_frame_index;
  cfg_mut->overlay_moving_bar = true;
  std::shared_ptr<const ActivePatternConfig> cfg = cfg_mut;
  std::atomic_store(&active_pattern_, cfg);

  // Configure renderer (safe to reconfigure per render; this is a hint).
  bool preset_valid = true;
  PatternSpec hint = to_pattern_spec(*cfg, cfg_.nominal.width, cfg_.nominal.height, PatternSpec::PackedFormat::RGBA8, &preset_valid);
  if (!preset_valid) {
    invalid_preset_requests_.fetch_add(1, std::memory_order_relaxed);
  }
  pattern_renderer_.configure(hint);

  return ProviderResult::success();
}

ProviderResult SyntheticProvider::enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  out_endpoints.clear();
  out_endpoints.reserve(cfg_.endpoint_count);
  for (uint32_t i = 0; i < cfg_.endpoint_count; ++i) {
    CameraEndpoint ep;
    ep.hardware_id = std::string(kHardwareIdPrefix) + std::to_string(i);
    ep.name = std::string("Synthetic Camera ") + std::to_string(i);
    out_endpoints.push_back(std::move(ep));
  }
  return ProviderResult::success();
}

bool SyntheticProvider::is_known_hardware_id_(const std::string& hardware_id) const {
  if (hardware_id.rfind(kHardwareIdPrefix, 0) != 0) {
    return false;
  }
  const char* p = hardware_id.c_str() + std::strlen(kHardwareIdPrefix);
  if (*p == '\0') {
    return false;
  }
  char* end = nullptr;
  const long idx = std::strtol(p, &end, 10);
  if (!end || *end != '\0' || idx < 0) {
    return false;
  }
  return static_cast<uint32_t>(idx) < cfg_.endpoint_count;
}

uint64_t SyntheticProvider::next_native_id_() {
  return native_id_seq_++;
}

void SyntheticProvider::emit_native_create_device_(const DeviceState& d) {
  if (!callbacks_) {
    return;
  }
  NativeObjectCreateInfo info{};
  info.native_id = d.native_id;
  info.type = 1; // v1: provider-owned enum placeholder (document later)
  info.root_id = d.root_id;
  info.owner_device_instance_id = d.device_instance_id;
  info.created_ns = clock_.now_ns();
  callbacks_->on_native_object_created(info);
}

void SyntheticProvider::emit_native_destroy_(uint64_t native_id) {
  if (!callbacks_) {
    return;
  }
  NativeObjectDestroyInfo info{};
  info.native_id = native_id;
  info.destroyed_ns = clock_.now_ns();
  callbacks_->on_native_object_destroyed(info);
}

ProviderResult SyntheticProvider::open_device(
    const std::string& hardware_id,
    uint64_t device_instance_id,
    uint64_t root_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_SHUTTING_DOWN);
  }
  if (!is_known_hardware_id_(hardware_id) || device_instance_id == 0 || root_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  auto& d = devices_[device_instance_id];
  if (d.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  d.hardware_id = hardware_id;
  d.device_instance_id = device_instance_id;
  d.root_id = root_id;
  d.open = true;
  d.native_id = next_native_id_();

  emit_native_create_device_(d);
  callbacks_->on_device_opened(device_instance_id);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::close_device(uint64_t device_instance_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  auto it = devices_.find(device_instance_id);
  if (it == devices_.end() || !it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  // Stop/destroy any streams owned by this device instance.
  for (auto sit = streams_.begin(); sit != streams_.end();) {
    if (sit->second.req.device_instance_id == device_instance_id) {
      if (sit->second.started) {
        (void)stop_stream(sit->first);
      }
      (void)destroy_stream(sit->first);
      sit = streams_.begin();
      continue;
    }
    ++sit;
  }

  emit_native_destroy_(it->second.native_id);
  it->second.open = false;
  callbacks_->on_device_closed(device_instance_id);
  devices_.erase(it);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::create_stream(const StreamRequest& req) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_SHUTTING_DOWN);
  }
  if (req.stream_id == 0 || req.device_instance_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  auto dit = devices_.find(req.device_instance_id);
  if (dit == devices_.end() || !dit->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  auto& s = streams_[req.stream_id];
  if (s.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  s.req = req;
  if (s.req.width == 0) s.req.width = cfg_.nominal.width;
  if (s.req.height == 0) s.req.height = cfg_.nominal.height;
  if (s.req.format_fourcc == 0) s.req.format_fourcc = cfg_.nominal.format_fourcc;
  if (s.req.target_fps_min == 0) s.req.target_fps_min = cfg_.nominal.fps_num;
  if (s.req.target_fps_max == 0) s.req.target_fps_max = cfg_.nominal.fps_num;

  s.created = true;
  s.started = false;
  s.frame_index = 0;
  s.next_due_ns = 0;
  s.native_id = next_native_id_();

  // Report stream native object.
  if (callbacks_) {
    NativeObjectCreateInfo info{};
    info.native_id = s.native_id;
    info.type = 2; // v1 placeholder
    info.root_id = dit->second.root_id;
    info.owner_device_instance_id = req.device_instance_id;
    info.owner_stream_id = req.stream_id;
    info.created_ns = clock_.now_ns();
    callbacks_->on_native_object_created(info);
  }

  callbacks_->on_stream_created(req.stream_id);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::destroy_stream(uint64_t stream_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || !it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (it->second.started) {
    (void)stop_stream(stream_id);
  }
  emit_native_destroy_(it->second.native_id);
  callbacks_->on_stream_destroyed(stream_id);
  streams_.erase(it);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::start_stream(uint64_t stream_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_SHUTTING_DOWN);
  }
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || !it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (it->second.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  it->second.started = true;
  it->second.frame_index = 0;
  it->second.next_due_ns = clock_.now_ns() + cfg_.nominal.start_stream_warmup_ns;
  callbacks_->on_stream_started(stream_id);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::stop_stream(uint64_t stream_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || !it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (!it->second.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  it->second.started = false;
  callbacks_->on_stream_stopped(stream_id, ProviderError::OK);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::trigger_capture(const CaptureRequest& req) {
  (void)req;
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult SyntheticProvider::abort_capture(uint64_t capture_id) {
  (void)capture_id;
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult SyntheticProvider::apply_camera_spec_patch(
    const std::string& hardware_id,
    uint64_t new_camera_spec_version,
    SpecPatchView patch) {
  (void)hardware_id;
  (void)new_camera_spec_version;
  (void)patch;
  // Core validates patches; synthetic currently does not implement dynamic capability changes.
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::apply_imaging_spec_patch(
    uint64_t new_imaging_spec_version,
    SpecPatchView patch) {
  (void)new_imaging_spec_version;
  (void)patch;
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::shutdown() {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  shutting_down_ = true;

  // Deterministic teardown order:
  // stop streams, destroy streams, close devices.
  for (auto& kv : streams_) {
    if (kv.second.started) {
      (void)stop_stream(kv.first);
    }
  }
  while (!streams_.empty()) {
    (void)destroy_stream(streams_.begin()->first);
  }
  while (!devices_.empty()) {
    (void)close_device(devices_.begin()->first);
  }

  initialized_ = false;
  callbacks_ = nullptr;
  shutting_down_ = false;
  return ProviderResult::success();
}

void SyntheticProvider::release_frame_(void* user, const FrameView* frame) {
  (void)frame;
  auto* p = static_cast<uint8_t*>(user);
  delete[] p;
}

void SyntheticProvider::emit_one_frame_(StreamState& s) {
  if (!callbacks_) {
    return;
  }

  const uint32_t w = s.req.width;
  const uint32_t h = s.req.height;
  const uint32_t stride = w * 4;
  const size_t size_bytes = static_cast<size_t>(stride) * static_cast<size_t>(h);
  auto* buf = new uint8_t[size_bytes];

  auto pcfg = std::atomic_load(&active_pattern_);
  bool preset_valid = true;
  PatternSpec spec = to_pattern_spec(*pcfg, w, h, PatternSpec::PackedFormat::RGBA8, &preset_valid);
  if (!preset_valid) {
    invalid_preset_requests_.fetch_add(1, std::memory_order_relaxed);
  }

  PatternRenderTarget dst{};
  dst.data = buf;
  dst.size_bytes = size_bytes;
  dst.width = w;
  dst.height = h;
  dst.stride_bytes = stride;
  dst.format = PatternSpec::PackedFormat::RGBA8;

  PatternOverlayData ov{};
  ov.frame_index = s.frame_index;
  ov.timestamp_ns = clock_.now_ns();
  ov.stream_id = s.req.stream_id;

  pattern_renderer_.render_into(spec, dst, ov);

  FrameView fv{};
  fv.device_instance_id = s.req.device_instance_id;
  fv.stream_id = s.req.stream_id;
  fv.capture_id = 0;
  fv.width = w;
  fv.height = h;
  fv.format_fourcc = FOURCC_RGBA;
  fv.capture_timestamp.value = clock_.now_ns();
  fv.capture_timestamp.tick_ns = 1;
  fv.capture_timestamp.domain = CaptureTimestampDomain::PROVIDER_MONOTONIC;
  fv.data = buf;
  fv.size_bytes = size_bytes;
  fv.stride_bytes = stride;
  fv.release = &SyntheticProvider::release_frame_;
  fv.release_user = buf;

  callbacks_->on_frame(fv);
  s.frame_index++;
}

void SyntheticProvider::emit_due_frames_() {
  const uint64_t now = clock_.now_ns();
  const uint64_t period = fps_period_ns(cfg_.nominal.fps_num, cfg_.nominal.fps_den);
  if (period == 0) {
    return;
  }

  for (auto& kv : streams_) {
    StreamState& s = kv.second;
    if (!s.created || !s.started) {
      continue;
    }
    // Emit as many frames as are due (catch-up) in virtual time.
    while (s.next_due_ns <= now) {
      emit_one_frame_(s);
      s.next_due_ns += period;
    }
  }
}

void SyntheticProvider::advance(uint64_t dt_ns) {
  if (!initialized_ || shutting_down_) {
    return;
  }
  // v1: only VirtualTime is implemented.
  clock_.advance(dt_ns);
  emit_due_frames_();
}

} // namespace cambang

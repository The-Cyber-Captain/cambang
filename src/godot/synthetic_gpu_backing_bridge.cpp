#include "imaging/synthetic/gpu_backing_runtime.h"

#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <chrono>
#include <utility>
#include <vector>

#include <godot_cpp/core/class_db.hpp>

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace cambang {
namespace {

class RenderThreadDrainHelper : public godot::RefCounted {
  GDCLASS(RenderThreadDrainHelper, godot::RefCounted);

public:
  static void _bind_methods() {}

  void drain_pending_releases_on_render_thread();
};

void enqueue_pending_release(const godot::RID& rid);
void request_pending_release_drain();

struct RetainedSyntheticGpuBacking final {
  std::mutex mutex;
  godot::RID rd_texture;
  godot::Ref<godot::Texture2DRD> display_texture;
  godot::PackedByteArray upload_bytes;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
  bool released = false;

  void release_now() {
    godot::RID rid;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (released) {
        return;
      }
      released = true;

      // The bridge owns rd_texture only until it is handed to Texture2DRD.
      // Once display_texture exists, do not manually free its RID here.
      if (rd_texture.is_valid()) {
        rid = rd_texture;
        rd_texture = godot::RID();
      }

      display_texture.unref();
    }

    if (!rid.is_valid()) {
      return;
    }
    enqueue_pending_release(rid);
    request_pending_release_drain();
  }

  ~RetainedSyntheticGpuBacking() {
    release_now();
  }
};

std::mutex g_pending_release_mutex;
// RIDs that must be released from the render thread.
std::vector<godot::RID> g_pending_releases;
bool g_pending_release_drain_scheduled = false;
godot::Ref<RenderThreadDrainHelper> g_render_thread_drain_helper;

RenderThreadDrainHelper* get_render_thread_drain_helper() {
  if (g_render_thread_drain_helper.is_null()) {
    g_render_thread_drain_helper.instantiate();
  }
  return g_render_thread_drain_helper.ptr();
}

void enqueue_pending_release(const godot::RID& rid) {
  if (!rid.is_valid()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  g_pending_releases.push_back(rid);
}

void request_pending_release_drain() {
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    return;
  }

  bool should_schedule = false;
  {
    std::lock_guard<std::mutex> lock(g_pending_release_mutex);
    if (!g_pending_releases.empty() && !g_pending_release_drain_scheduled) {
      g_pending_release_drain_scheduled = true;
      should_schedule = true;
    }
  }
  if (!should_schedule) {
    return;
  }

  RenderThreadDrainHelper* helper = get_render_thread_drain_helper();
  if (!helper) {
    std::lock_guard<std::mutex> lock(g_pending_release_mutex);
    g_pending_release_drain_scheduled = false;
    return;
  }

  rs->call_on_render_thread(callable_mp(helper, &RenderThreadDrainHelper::drain_pending_releases_on_render_thread));
}

void RenderThreadDrainHelper::drain_pending_releases_on_render_thread() {
  std::vector<godot::RID> pending;
  {
    std::lock_guard<std::mutex> lock(g_pending_release_mutex);
    pending.swap(g_pending_releases);
    g_pending_release_drain_scheduled = false;
  }
  if (pending.empty()) {
    return;
  }

  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  godot::RenderingDevice* rd = rs ? rs->get_rendering_device() : nullptr;
  if (!rd) {
    std::lock_guard<std::mutex> lock(g_pending_release_mutex);
    for (godot::RID &rid : pending) {
      g_pending_releases.push_back(std::move(rid));
    }
    if (rs && !g_pending_releases.empty() && !g_pending_release_drain_scheduled) {
      g_pending_release_drain_scheduled = true;
      rs->call_on_render_thread(callable_mp(this, &RenderThreadDrainHelper::drain_pending_releases_on_render_thread));
    }
    return;
  }

  for (const godot::RID &rid : pending) {
    if (rid.is_valid()) {
      rd->free_rid(rid);
    }
  }

  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  if (!g_pending_releases.empty() && !g_pending_release_drain_scheduled) {
    g_pending_release_drain_scheduled = true;
    rs->call_on_render_thread(callable_mp(this, &RenderThreadDrainHelper::drain_pending_releases_on_render_thread));
  }
}

bool gpu_trace_enabled() noexcept {
  const char* value = std::getenv("CAMBANG_DEV_SYNTH_GPU_TRACE");
  return value && value[0] != '\0' && value[0] != '0';
}

struct GpuUpdateTimingStats final {
  uint64_t upload_copy_calls = 0;
  uint64_t upload_copy_total_ns = 0;
  uint64_t upload_copy_max_ns = 0;
  uint64_t texture_update_calls = 0;
  uint64_t texture_update_total_ns = 0;
  uint64_t texture_update_max_ns = 0;
};

std::mutex g_gpu_update_timing_stats_mutex;
GpuUpdateTimingStats g_gpu_update_timing_stats;

void record_update_timing(uint64_t& max_ns, uint64_t& total_ns, uint64_t& calls, uint64_t sample_ns) {
  total_ns += sample_ns;
  ++calls;
  if (sample_ns > max_ns) {
    max_ns = sample_ns;
  }
}

void trace_gpu(const char* message) {
  if (!gpu_trace_enabled()) {
    return;
  }
  godot::UtilityFunctions::print("[CamBANG][SyntheticGPU] ", message);
}

void trace_runtime_query(bool global_rd_ptr, bool runtime_truth_gpu_available) {
  if (!gpu_trace_enabled()) {
    return;
  }
  godot::UtilityFunctions::print(
      "[CamBANG][SyntheticGPU] runtime_query global_rd_ptr=",
      global_rd_ptr,
      " runtime_truth_gpu_available=",
      runtime_truth_gpu_available);
}

struct BackingSnapshot final {
  godot::RID rid;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
};

bool snapshot_backing_for_use(
    const std::shared_ptr<RetainedSyntheticGpuBacking>& retained,
    BackingSnapshot& out) {
  if (!retained) {
    return false;
  }
  std::lock_guard<std::mutex> lock(retained->mutex);
  if (retained->released) {
    return false;
  }
  if (retained->rd_texture.is_valid()) {
    out.rid = retained->rd_texture;
  } else if (retained->display_texture.is_valid()) {
    out.rid = retained->display_texture->get_texture_rd_rid();
  } else {
    return false;
  }
  if (!out.rid.is_valid()) {
    return false;
  }
  out.width = retained->width;
  out.height = retained->height;
  out.stride_bytes = retained->stride_bytes;
  return true;
}

bool global_rd_available() noexcept {
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    trace_runtime_query(false, false);
    return false;
  }
  godot::RenderingDevice* rd = rs->get_rendering_device();
  const bool available = rd != nullptr;
  trace_runtime_query(available, available);
  return available;
}

bool global_rd_roundtrip_rgba8(
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes,
    std::vector<uint8_t>& out) noexcept {
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs || !src || width == 0 || height == 0) {
    trace_gpu("roundtrip_result texture_create=false readback=false success=false reason=invalid_input_or_server");
    return false;
  }
  godot::RenderingDevice* rd = rs->get_rendering_device();
  if (!rd) {
    trace_gpu("roundtrip_result texture_create=false readback=false success=false reason=no_global_rd");
    return false;
  }
  if (stride_bytes != width * 4u) {
    trace_gpu("roundtrip_result texture_create=false readback=false success=false reason=unexpected_stride");
    return false;
  }

  godot::Ref<godot::RDTextureFormat> format;
  format.instantiate();
  format->set_width(static_cast<int64_t>(width));
  format->set_height(static_cast<int64_t>(height));
  format->set_format(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
  format->set_usage_bits(
      godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
      godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
      godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);

  godot::Ref<godot::RDTextureView> view;
  view.instantiate();

  godot::PackedByteArray initial;
  const size_t expected_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  initial.resize(static_cast<int64_t>(expected_size));
  std::memcpy(initial.ptrw(), src, static_cast<size_t>(initial.size()));

  godot::Array data;
  data.push_back(initial);
  const godot::RID tex = rd->texture_create(format, view, data);
  if (!tex.is_valid()) {
    trace_gpu("roundtrip_result texture_create=false readback=false success=false");
    return false;
  }

  const godot::PackedByteArray readback = rd->texture_get_data(tex, 0);
  rd->free_rid(tex);
  if (readback.size() <= 0) {
    trace_gpu("roundtrip_result texture_create=true readback=false success=false");
    return false;
  }
  out.resize(expected_size);
  if (static_cast<size_t>(readback.size()) < out.size()) {
    trace_gpu("roundtrip_result texture_create=true readback=false success=false reason=short_readback");
    return false;
  }
  std::memcpy(out.data(), readback.ptr(), out.size());
  trace_gpu("roundtrip_result texture_create=true readback=true success=true");
  return true;
}

std::shared_ptr<void> retain_primary_gpu_backing_rgba8(
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  if (!src || width == 0 || height == 0 || stride_bytes != width * 4u) {
    return {};
  }
  request_pending_release_drain();
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    return {};
  }
  godot::RenderingDevice* rd = rs->get_rendering_device();
  if (!rd) {
    return {};
  }

  godot::Ref<godot::RDTextureFormat> format;
  format.instantiate();
  format->set_width(static_cast<int64_t>(width));
  format->set_height(static_cast<int64_t>(height));
  format->set_format(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
  format->set_usage_bits(
      godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
      godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
      godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);

  godot::Ref<godot::RDTextureView> view;
  view.instantiate();

  godot::PackedByteArray bytes;
  bytes.resize(static_cast<int64_t>(stride_bytes) * static_cast<int64_t>(height));
  std::memcpy(bytes.ptrw(), src, static_cast<size_t>(bytes.size()));

  godot::Array data;
  data.push_back(bytes);
  const godot::RID texture = rd->texture_create(format, view, data);
  if (!texture.is_valid()) {
    return {};
  }

  auto retained_backing = std::make_shared<RetainedSyntheticGpuBacking>();
  retained_backing->rd_texture = texture;
  retained_backing->width = width;
  retained_backing->height = height;
  return std::static_pointer_cast<void>(retained_backing);
}

std::shared_ptr<void> create_stream_live_gpu_backing_rgba8(
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  if (width == 0 || height == 0 || stride_bytes != width * 4u) {
    return {};
  }
  request_pending_release_drain();
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    return {};
  }
  godot::RenderingDevice* rd = rs->get_rendering_device();
  if (!rd) {
    return {};
  }

  godot::Ref<godot::RDTextureFormat> format;
  format.instantiate();
  format->set_width(static_cast<int64_t>(width));
  format->set_height(static_cast<int64_t>(height));
  format->set_format(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
  format->set_usage_bits(
      godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
      godot::RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
      godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);

  godot::Ref<godot::RDTextureView> view;
  view.instantiate();

  godot::PackedByteArray bytes;
  bytes.resize(static_cast<int64_t>(stride_bytes) * static_cast<int64_t>(height));
  std::memset(bytes.ptrw(), 0, static_cast<size_t>(bytes.size()));

  godot::Array data;
  data.push_back(bytes);
  const godot::RID texture = rd->texture_create(format, view, data);
  if (!texture.is_valid()) {
    return {};
  }

  auto retained_backing = std::make_shared<RetainedSyntheticGpuBacking>();
  retained_backing->rd_texture = texture;
  retained_backing->width = width;
  retained_backing->height = height;
  retained_backing->stride_bytes = stride_bytes;
  return std::static_pointer_cast<void>(retained_backing);
}

bool update_stream_live_gpu_backing_rgba8(
    const std::shared_ptr<void>& backing,
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  if (!backing || !src || width == 0 || height == 0 || stride_bytes != width * 4u) {
    return false;
  }
  request_pending_release_drain();
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    return false;
  }
  godot::RenderingDevice* rd = rs->get_rendering_device();
  if (!rd) {
    return false;
  }

  const std::shared_ptr<RetainedSyntheticGpuBacking> retained =
      std::static_pointer_cast<RetainedSyntheticGpuBacking>(backing);
  if (!retained) {
    return false;
  }
  godot::RID texture_rid;
  {
    std::lock_guard<std::mutex> lock(retained->mutex);
    if (retained->released) {
      return false;
    }
    if (retained->width != width || retained->height != height || retained->stride_bytes != stride_bytes) {
      return false;
    }
    if (retained->rd_texture.is_valid()) {
      texture_rid = retained->rd_texture;
    } else if (retained->display_texture.is_valid()) {
      texture_rid = retained->display_texture->get_texture_rd_rid();
    }
    if (!texture_rid.is_valid()) {
      return false;
    }

    const int64_t frame_bytes = static_cast<int64_t>(stride_bytes) * static_cast<int64_t>(height);
    if (retained->upload_bytes.size() != frame_bytes) {
      retained->upload_bytes.resize(frame_bytes);
    }
    const auto copy_t0 = std::chrono::steady_clock::now();
    std::memcpy(retained->upload_bytes.ptrw(), src, static_cast<size_t>(frame_bytes));
    const auto copy_t1 = std::chrono::steady_clock::now();
    const auto update_t0 = copy_t1;
    rd->texture_update(texture_rid, 0, retained->upload_bytes);
    const auto update_t1 = std::chrono::steady_clock::now();
    const uint64_t copy_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(copy_t1 - copy_t0).count());
    const uint64_t update_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(update_t1 - update_t0).count());
    std::lock_guard<std::mutex> timing_lock(g_gpu_update_timing_stats_mutex);
    record_update_timing(
        g_gpu_update_timing_stats.upload_copy_max_ns,
        g_gpu_update_timing_stats.upload_copy_total_ns,
        g_gpu_update_timing_stats.upload_copy_calls,
        copy_ns);
    record_update_timing(
        g_gpu_update_timing_stats.texture_update_max_ns,
        g_gpu_update_timing_stats.texture_update_total_ns,
        g_gpu_update_timing_stats.texture_update_calls,
        update_ns);
  }
  return true;
}

bool take_update_timing_stats(
    uint64_t& upload_copy_calls,
    uint64_t& upload_copy_total_ns,
    uint64_t& upload_copy_max_ns,
    uint64_t& texture_update_calls,
    uint64_t& texture_update_total_ns,
    uint64_t& texture_update_max_ns) noexcept {
  std::lock_guard<std::mutex> lock(g_gpu_update_timing_stats_mutex);
  upload_copy_calls = g_gpu_update_timing_stats.upload_copy_calls;
  upload_copy_total_ns = g_gpu_update_timing_stats.upload_copy_total_ns;
  upload_copy_max_ns = g_gpu_update_timing_stats.upload_copy_max_ns;
  texture_update_calls = g_gpu_update_timing_stats.texture_update_calls;
  texture_update_total_ns = g_gpu_update_timing_stats.texture_update_total_ns;
  texture_update_max_ns = g_gpu_update_timing_stats.texture_update_max_ns;
  return true;
}

void release_stream_live_gpu_backing(std::shared_ptr<void>& backing) noexcept {
  if (!backing) {
    return;
  }
  const std::shared_ptr<RetainedSyntheticGpuBacking> retained =
      std::static_pointer_cast<RetainedSyntheticGpuBacking>(backing);
  if (retained) {
    retained->release_now();
  }
  backing.reset();
}

const SyntheticGpuBackingRuntimeOps kOps{
    &global_rd_available,
    &global_rd_roundtrip_rgba8,
    &retain_primary_gpu_backing_rgba8,
    &create_stream_live_gpu_backing_rgba8,
    &update_stream_live_gpu_backing_rgba8,
    &release_stream_live_gpu_backing,
    &take_update_timing_stats,
};

} // namespace

void install_synthetic_gpu_backing_godot_bridge() {
  set_synthetic_gpu_backing_runtime_ops(&kOps);
  trace_gpu("bridge_install runtime_ops_registered=true");
}

void uninstall_synthetic_gpu_backing_godot_bridge() {
  clear_synthetic_gpu_backing_runtime_ops();
  trace_gpu("bridge_uninstall runtime_ops_registered=false");
}

godot::Ref<godot::Texture2D> synthetic_gpu_backing_display_texture(const std::shared_ptr<void>& backing) {
  request_pending_release_drain();
  if (!backing) {
    return {};
  }
  const std::shared_ptr<RetainedSyntheticGpuBacking> retained =
      std::static_pointer_cast<RetainedSyntheticGpuBacking>(backing);
  if (!retained) {
    return {};
  }

  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    return {};
  }
  godot::RenderingDevice* rd = rs->get_rendering_device();
  std::lock_guard<std::mutex> lock(retained->mutex);
  if (retained->released) {
    return {};
  }
  if (!rd || !retained->rd_texture.is_valid()) {
    return retained->display_texture;
  }
  if (!retained->display_texture.is_valid()) {
    godot::Ref<godot::Texture2DRD> texture;
    texture.instantiate();
    if (texture.is_null()) {
      return {};
    }
    // Display view is a live wrapper over stream-owned backing. Texture2DRD
    // assumes ownership of this RID after binding.
    texture->set_texture_rd_rid(retained->rd_texture);
    retained->rd_texture = godot::RID();
    retained->display_texture = texture;
  }
  return retained->display_texture;
}

bool synthetic_gpu_backing_can_materialize_to_image(const std::shared_ptr<void>& backing) {
  request_pending_release_drain();
  if (!backing) {
    return false;
  }
  const std::shared_ptr<RetainedSyntheticGpuBacking> retained =
      std::static_pointer_cast<RetainedSyntheticGpuBacking>(backing);
  if (!retained) {
    return false;
  }
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs || !rs->get_rendering_device()) {
    return false;
  }
  BackingSnapshot snapshot{};
  return snapshot_backing_for_use(retained, snapshot);
}

godot::Ref<godot::Image> synthetic_gpu_backing_materialize_to_image(const std::shared_ptr<void>& backing) {
  request_pending_release_drain();
  if (!backing) {
    return {};
  }
  const std::shared_ptr<RetainedSyntheticGpuBacking> retained =
      std::static_pointer_cast<RetainedSyntheticGpuBacking>(backing);
  if (!retained) {
    return {};
  }
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    return {};
  }
  godot::RenderingDevice* rd = rs->get_rendering_device();
  if (!rd) {
    return {};
  }
  BackingSnapshot snapshot{};
  if (!snapshot_backing_for_use(retained, snapshot)) {
    return {};
  }
  const godot::PackedByteArray readback = rd->texture_get_data(snapshot.rid, 0);
  if (readback.size() <= 0) {
    return {};
  }

  if (snapshot.width == 0 || snapshot.height == 0) {
    return {};
  }

  godot::Ref<godot::Image> image;
  image.instantiate();
  if (image.is_null()) {
    return {};
  }
  image->set_data(static_cast<int64_t>(snapshot.width),
                  static_cast<int64_t>(snapshot.height),
                  false,
                  godot::Image::FORMAT_RGBA8,
                  readback);
  if (image->is_empty()) {
    return {};
  }
  return image;
}

} // namespace cambang

#include "imaging/synthetic/gpu_backing_runtime.h"
#include "godot/synthetic_gpu_backing_bridge_internal.h"
#include "godot/godot_gpu_display_service.h"
#include "core/resource_aggregate_telemetry.h"
#include "imaging/api/provider_contract_datatypes.h"

#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <chrono>
#include <utility>
#include <vector>
#include <map>
#include <memory>

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
#include <godot_cpp/variant/string_name.hpp>

namespace cambang {

void register_synthetic_gpu_backing_internal_classes();

static bool enqueue_pending_release(const godot::RID& rid);
static void request_pending_release_drain();
static bool gpu_trace_enabled() noexcept {
  const char* value = std::getenv("CAMBANG_DEV_SYNTH_GPU_TRACE");
  return value && value[0] != '\0' && value[0] != '0';
}

struct GodotBoundaryGpuDisplayKey final {
  uint64_t stream_id = 0;
  uint64_t backing_id = 0;

  bool operator<(const GodotBoundaryGpuDisplayKey& other) const noexcept {
    if (stream_id != other.stream_id) {
      return stream_id < other.stream_id;
    }
    return backing_id < other.backing_id;
  }
};

struct GodotBoundaryGpuDisplayEntry final {
  std::mutex mutex;
  GodotBoundaryGpuDisplayKey key{};
  godot::RID texture_rid;
  godot::Ref<godot::Texture2DRD> display_texture;
  godot::PackedByteArray upload_bytes;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
  bool rid_owned_by_entry = true;
  bool retired = false;
};

struct RetainedSyntheticGpuBacking final {
  std::mutex mutex;
  std::weak_ptr<GodotBoundaryGpuDisplayEntry> display_entry;
  uint64_t stream_id = 0;
  uint64_t backing_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
  bool released = false;
  ScopedResourceTelemetryKey telemetry_key{};

  void release_now() noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    if (released) {
      return;
    }
    released = true;
    display_entry.reset();
    global_resource_aggregate_telemetry().retained_gpu_backing_released(telemetry_key);
  }

  ~RetainedSyntheticGpuBacking() {
    release_now();
  }
};

class DisplayTextureRidOwner : public godot::RefCounted {
  GDCLASS(DisplayTextureRidOwner, godot::RefCounted);

public:
  static void _bind_methods() {}

  void init(const godot::RID& rid) {
    rid_ = rid;
  }

  ~DisplayTextureRidOwner() override {
    release_now();
  }

  void release_now() {
    godot::RID rid;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (released_) {
        return;
      }
      released_ = true;
      rid = rid_;
      rid_ = godot::RID();
    }
    if (!rid.is_valid()) {
      return;
    }
    if (enqueue_pending_release(rid)) {
      request_pending_release_drain();
    }
  }

private:
  std::mutex mutex_{};
  godot::RID rid_{};
  bool released_ = false;
};

void register_synthetic_gpu_backing_internal_classes() {
  // Internal-only helpers: registered only so Ref<...>::instantiate() can
  // resolve through ClassDB. These are not user-facing CamBANG API classes.
  godot::ClassDB::register_class<RenderThreadDrainHelper>();
  godot::ClassDB::register_class<DisplayTextureRidOwner>();
}

static std::mutex g_pending_release_mutex;
// RIDs that must be released from the render thread.
static std::vector<godot::RID> g_pending_releases;
static bool g_pending_release_drain_scheduled = false;
static bool g_bridge_teardown_started = false;
static godot::Ref<RenderThreadDrainHelper> g_render_thread_drain_helper;

static std::mutex g_display_registry_mutex;
static std::map<GodotBoundaryGpuDisplayKey, std::shared_ptr<GodotBoundaryGpuDisplayEntry>> g_display_registry;

static bool descriptor_has_complete_display_identity(const RetainedGpuBackingDescriptor& descriptor) noexcept {
  return descriptor.valid &&
         descriptor.display_available &&
         descriptor.stream_id != 0 &&
         descriptor.backing_id != 0 &&
         descriptor.width != 0 &&
         descriptor.height != 0;
}

static std::shared_ptr<GodotBoundaryGpuDisplayEntry> lookup_display_entry(
    const RetainedGpuBackingDescriptor& descriptor) {
  if (!descriptor_has_complete_display_identity(descriptor)) {
    return {};
  }
  std::lock_guard<std::mutex> lock(g_display_registry_mutex);
  const auto it = g_display_registry.find(GodotBoundaryGpuDisplayKey{descriptor.stream_id, descriptor.backing_id});
  if (it == g_display_registry.end()) {
    return {};
  }
  return it->second;
}

static bool enqueue_pending_release(const godot::RID& rid) {
  if (!rid.is_valid()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  if (g_bridge_teardown_started) {
    return false;
  }
  g_pending_releases.push_back(rid);
  return true;
}

static bool bridge_teardown_started() {
  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  return g_bridge_teardown_started;
}

static void clear_pending_releases_for_teardown() {
  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  g_bridge_teardown_started = true;
  // After teardown starts, do not schedule render-thread callbacks back into this
  // GDExtension. Pending RIDs are abandoned best-effort rather than risking a
  // late callback into torn-down extension or RenderingServer state.
  g_pending_releases.clear();
  g_pending_release_drain_scheduled = false;
  g_render_thread_drain_helper.unref();
}

static void reset_bridge_teardown_state_for_install() {
  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  g_bridge_teardown_started = false;
  g_pending_releases.clear();
  g_pending_release_drain_scheduled = false;
  g_render_thread_drain_helper.unref();
}

static void request_pending_release_drain() {
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  if (g_bridge_teardown_started || g_pending_releases.empty() || g_pending_release_drain_scheduled) {
    return;
  }

  if (g_render_thread_drain_helper.is_null()) {
    g_render_thread_drain_helper.instantiate();
  }
  RenderThreadDrainHelper* helper = g_render_thread_drain_helper.ptr();
  if (!helper) {
    return;
  }

  g_pending_release_drain_scheduled = true;
  rs->call_on_render_thread(callable_mp(helper, &RenderThreadDrainHelper::drain_pending_releases_on_render_thread));
}

void RenderThreadDrainHelper::drain_pending_releases_on_render_thread() {
  std::vector<godot::RID> pending;
  {
    std::lock_guard<std::mutex> lock(g_pending_release_mutex);
    if (g_bridge_teardown_started) {
      g_pending_releases.clear();
      g_pending_release_drain_scheduled = false;
      return;
    }
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
    if (g_bridge_teardown_started) {
      g_pending_release_drain_scheduled = false;
      return;
    }
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
      if (gpu_trace_enabled()) {
        godot::UtilityFunctions::print("[CamBANG][SyntheticGpu] texture_free rid=", rid.get_id());
      }
      rd->free_rid(rid);
    }
  }

  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  if (g_bridge_teardown_started) {
    g_pending_releases.clear();
    g_pending_release_drain_scheduled = false;
    return;
  }
  if (!g_pending_releases.empty() && !g_pending_release_drain_scheduled) {
    g_pending_release_drain_scheduled = true;
    rs->call_on_render_thread(callable_mp(this, &RenderThreadDrainHelper::drain_pending_releases_on_render_thread));
  }
}

static void release_display_entry_from_godot_boundary(
    const std::shared_ptr<GodotBoundaryGpuDisplayEntry>& entry) {
  if (!entry) {
    return;
  }
  godot::RID rid;
  godot::Ref<godot::Texture2DRD> texture;
  {
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->retired) {
      return;
    }
    entry->retired = true;
    if (entry->rid_owned_by_entry && entry->texture_rid.is_valid()) {
      rid = entry->texture_rid;
      entry->texture_rid = godot::RID();
      entry->rid_owned_by_entry = false;
    }
    texture = entry->display_texture;
    entry->display_texture.unref();
  }
  texture.unref();
  if (rid.is_valid() && enqueue_pending_release(rid)) {
    request_pending_release_drain();
  }
}

void synthetic_gpu_backing_invalidate_stream_godot_display(uint64_t stream_id) {
  if (stream_id == 0) {
    return;
  }
  std::vector<std::shared_ptr<GodotBoundaryGpuDisplayEntry>> removed;
  {
    std::lock_guard<std::mutex> lock(g_display_registry_mutex);
    for (auto it = g_display_registry.begin(); it != g_display_registry.end();) {
      if (it->first.stream_id == stream_id) {
        removed.push_back(std::move(it->second));
        it = g_display_registry.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const auto& entry : removed) {
    release_display_entry_from_godot_boundary(entry);
  }
}

void synthetic_gpu_backing_invalidate_all_godot_display() {
  std::vector<std::shared_ptr<GodotBoundaryGpuDisplayEntry>> removed;
  {
    std::lock_guard<std::mutex> lock(g_display_registry_mutex);
    removed.reserve(g_display_registry.size());
    for (auto& kv : g_display_registry) {
      removed.push_back(std::move(kv.second));
    }
    g_display_registry.clear();
  }
  for (const auto& entry : removed) {
    release_display_entry_from_godot_boundary(entry);
  }
}


namespace {

struct GpuUpdateTimingStats final {
  uint64_t upload_copy_calls = 0;
  uint64_t upload_copy_total_ns = 0;
  uint64_t upload_copy_max_ns = 0;
  uint64_t texture_update_calls = 0;
  uint64_t texture_update_total_ns = 0;
  uint64_t texture_update_max_ns = 0;
  uint64_t texture_update_skipped = 0;
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
  godot::UtilityFunctions::print("[CamBANG][SyntheticGpu] ", message);
}

void trace_runtime_query(bool global_rd_ptr, bool runtime_truth_gpu_available) {
  if (!gpu_trace_enabled()) {
    return;
  }
  godot::UtilityFunctions::print(
      "[CamBANG][SyntheticGpu] runtime_query global_rd_ptr=",
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
  std::shared_ptr<GodotBoundaryGpuDisplayEntry> entry;
  {
    std::lock_guard<std::mutex> lock(retained->mutex);
    if (retained->released) {
      return false;
    }
    entry = retained->display_entry.lock();
  }
  if (!entry) {
    return false;
  }
  std::lock_guard<std::mutex> entry_lock(entry->mutex);
  if (entry->retired || !entry->texture_rid.is_valid()) {
    return false;
  }
  out.rid = entry->texture_rid;
  out.width = entry->width;
  out.height = entry->height;
  out.stride_bytes = entry->stride_bytes;
  return true;
}

bool global_rd_available() noexcept {
  if (bridge_teardown_started()) {
    trace_runtime_query(false, false);
    return false;
  }
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
    const uint8_t*,
    uint32_t,
    uint32_t,
    uint32_t,
    std::vector<uint8_t>&) noexcept {
  trace_gpu("roundtrip_result texture_create=false readback=false success=false reason=unsupported_non_render_thread");
  return false;
}

std::shared_ptr<void> retain_primary_gpu_backing_rgba8(
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  if (bridge_teardown_started()) {
    return {};
  }
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
  if (gpu_trace_enabled()) {
    godot::UtilityFunctions::print("[CamBANG][SyntheticGpu] texture_alloc kind=retain_primary rid=", texture.get_id(),
                                   " w=", (long long)width,
                                   " h=", (long long)height);
  }

  if (enqueue_pending_release(texture)) {
    request_pending_release_drain();
  }
  return {};
}

std::shared_ptr<void> create_stream_live_gpu_backing_rgba8(
    uint64_t stream_id,
    uint64_t backing_id,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  if (bridge_teardown_started()) {
    return {};
  }
  if (stream_id == 0 || backing_id == 0 || width == 0 || height == 0 || stride_bytes != width * 4u) {
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
  if (gpu_trace_enabled()) {
    godot::UtilityFunctions::print("[CamBANG][SyntheticGpu] texture_alloc kind=stream_live rid=", texture.get_id(),
                                   " w=", (long long)width,
                                   " h=", (long long)height);
  }

  auto entry = std::make_shared<GodotBoundaryGpuDisplayEntry>();
  entry->key = GodotBoundaryGpuDisplayKey{stream_id, backing_id};
  entry->texture_rid = texture;
  entry->width = width;
  entry->height = height;
  entry->stride_bytes = stride_bytes;
  {
    std::lock_guard<std::mutex> lock(g_display_registry_mutex);
    auto& slot = g_display_registry[entry->key];
    if (slot) {
      return {};
    }
    slot = entry;
  }

  auto retained_backing = std::make_shared<RetainedSyntheticGpuBacking>();
  retained_backing->telemetry_key = make_stream_scoped_resource_telemetry(stream_id);
  global_resource_aggregate_telemetry().retained_gpu_backing_created(retained_backing->telemetry_key);
  retained_backing->display_entry = entry;
  retained_backing->stream_id = stream_id;
  retained_backing->backing_id = backing_id;
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
  if (bridge_teardown_started()) {
    return false;
  }
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
  std::shared_ptr<GodotBoundaryGpuDisplayEntry> entry;
  {
    std::lock_guard<std::mutex> lock(retained->mutex);
    if (retained->released) {
      return false;
    }
    if (retained->width != width || retained->height != height || retained->stride_bytes != stride_bytes) {
      return false;
    }
    entry = retained->display_entry.lock();
  }
  if (!entry) {
    return false;
  }
  {
    std::lock_guard<std::mutex> entry_lock(entry->mutex);
    if (entry->retired || entry->width != width || entry->height != height || entry->stride_bytes != stride_bytes) {
      return false;
    }
    const godot::RID texture_rid = entry->texture_rid;
    if (!texture_rid.is_valid()) {
      return false;
    }

    const int64_t frame_bytes = static_cast<int64_t>(stride_bytes) * static_cast<int64_t>(height);
    if (entry->upload_bytes.size() != frame_bytes) {
      entry->upload_bytes.resize(frame_bytes);
    }
    const auto copy_t0 = std::chrono::steady_clock::now();
    std::memcpy(entry->upload_bytes.ptrw(), src, static_cast<size_t>(frame_bytes));
    const auto copy_t1 = std::chrono::steady_clock::now();
    const auto update_t0 = copy_t1;
    rd->texture_update(texture_rid, 0, entry->upload_bytes);
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
    uint64_t& texture_update_max_ns,
    uint64_t& texture_update_skipped) noexcept {
  std::lock_guard<std::mutex> lock(g_gpu_update_timing_stats_mutex);
  upload_copy_calls = g_gpu_update_timing_stats.upload_copy_calls;
  upload_copy_total_ns = g_gpu_update_timing_stats.upload_copy_total_ns;
  upload_copy_max_ns = g_gpu_update_timing_stats.upload_copy_max_ns;
  texture_update_calls = g_gpu_update_timing_stats.texture_update_calls;
  texture_update_total_ns = g_gpu_update_timing_stats.texture_update_total_ns;
  texture_update_max_ns = g_gpu_update_timing_stats.texture_update_max_ns;
  texture_update_skipped = g_gpu_update_timing_stats.texture_update_skipped;
  return true;
}

bool peek_update_timing_stats(
    uint64_t& upload_copy_calls,
    uint64_t& upload_copy_total_ns,
    uint64_t& upload_copy_max_ns,
    uint64_t& texture_update_calls,
    uint64_t& texture_update_total_ns,
    uint64_t& texture_update_max_ns,
    uint64_t& texture_update_skipped) noexcept {
  return take_update_timing_stats(
      upload_copy_calls,
      upload_copy_total_ns,
      upload_copy_max_ns,
      texture_update_calls,
      texture_update_total_ns,
      texture_update_max_ns,
      texture_update_skipped);
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
    &peek_update_timing_stats,
};

} // namespace

void install_synthetic_gpu_backing_godot_bridge() {
  reset_bridge_teardown_state_for_install();
  set_synthetic_gpu_backing_runtime_ops(&kOps);
  trace_gpu("bridge_install runtime_ops_registered=true");
}

void uninstall_synthetic_gpu_backing_godot_bridge() {
  clear_synthetic_gpu_backing_runtime_ops();
  godot_gpu_display_invalidate_all();
  synthetic_gpu_backing_invalidate_all_godot_display();
  clear_pending_releases_for_teardown();
  trace_gpu("bridge_uninstall runtime_ops_registered=false");
}

bool synthetic_gpu_backing_display_texture_available(const RetainedGpuBackingDescriptor& descriptor) {
  std::shared_ptr<GodotBoundaryGpuDisplayEntry> entry = lookup_display_entry(descriptor);
  if (!entry) {
    return false;
  }
  std::lock_guard<std::mutex> lock(entry->mutex);
  return !entry->retired &&
         entry->texture_rid.is_valid() &&
         entry->width == descriptor.width &&
         entry->height == descriptor.height &&
         entry->stride_bytes == descriptor.stride_bytes;
}

godot::Ref<godot::Texture2D> synthetic_gpu_backing_display_texture(const RetainedGpuBackingDescriptor& descriptor) {
  constexpr const char* kDisplayTextureRidOwnerMetaKey = "__cambang_synth_gpu_rid_owner";
  if (bridge_teardown_started()) {
    return {};
  }
  request_pending_release_drain();
  std::shared_ptr<GodotBoundaryGpuDisplayEntry> entry = lookup_display_entry(descriptor);
  if (!entry) {
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
  std::lock_guard<std::mutex> lock(entry->mutex);
  if (entry->retired ||
      entry->width != descriptor.width ||
      entry->height != descriptor.height ||
      entry->stride_bytes != descriptor.stride_bytes ||
      !entry->texture_rid.is_valid()) {
    return {};
  }
  if (!entry->display_texture.is_valid()) {
    godot::Ref<godot::Texture2DRD> texture;
    texture.instantiate();
    if (texture.is_null()) {
      return {};
    }
    // Display view is a live Godot-boundary wrapper over the registered stream
    // backing RID. Texture2DRD does not own/free this RID; lifetime is carried by
    // attached metadata once the texture is exposed to Godot scene objects.
    texture->set_texture_rd_rid(entry->texture_rid);
    godot::Ref<DisplayTextureRidOwner> rid_owner;
    rid_owner.instantiate();
    if (!rid_owner.is_valid()) {
      return {};
    }
    rid_owner->init(entry->texture_rid);
    texture->set_meta(godot::StringName(kDisplayTextureRidOwnerMetaKey), rid_owner);
    entry->rid_owned_by_entry = false;
    entry->display_texture = texture;
  }
  return entry->display_texture;
}

bool synthetic_gpu_backing_can_materialize_to_image(const std::shared_ptr<void>& backing) {
  (void)backing;
  return false;
}

godot::Ref<godot::Image> synthetic_gpu_backing_materialize_to_image(const std::shared_ptr<void>& backing) {
  (void)backing;
  return {};
}

} // namespace cambang

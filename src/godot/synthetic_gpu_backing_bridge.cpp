#include "imaging/synthetic/gpu_backing_runtime.h"
#include "godot/synthetic_gpu_backing_bridge_internal.h"
#include "core/resource_aggregate_telemetry.h"

#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
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
#include <godot_cpp/variant/string_name.hpp>

namespace cambang {

void register_synthetic_gpu_backing_internal_classes();

static bool enqueue_pending_release(const godot::RID& rid);
static bool bridge_teardown_started();
static void request_pending_release_drain();
static void abandon_display_texture_after_failed_render_release(godot::Ref<godot::Texture2DRD>& texture);
static bool gpu_trace_enabled() noexcept {
  const char* value = std::getenv("CAMBANG_DEV_SYNTH_GPU_TRACE");
  return value && value[0] != '\0' && value[0] != '0';
}

struct RetainedSyntheticGpuBacking final {
  std::mutex mutex;
  godot::RID rd_texture;
  godot::Ref<godot::Texture2DRD> display_texture;
  godot::PackedByteArray upload_bytes;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
  bool released = false;
  ScopedResourceTelemetryKey telemetry_key{};

  void release_now();

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
  godot::ClassDB::register_class<RenderThreadTaskHelper>();
  godot::ClassDB::register_class<DisplayTextureRidOwner>();
}


struct RenderThreadTaskState final {
  enum class Kind {
    NONE,
    TEXTURE_READBACK,
    ROUNDTRIP_RGBA8,
    CREATE_TEXTURE_RGBA8,
    RELEASE_DISPLAY_TEXTURE,
  };

  std::mutex mutex;
  std::condition_variable cv;
  Kind kind = Kind::NONE;
  bool done = false;
  bool success = false;

  godot::RID rid;
  godot::PackedByteArray bytes;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
  uint32_t usage_bits = 0;
  godot::PackedByteArray readback;
  godot::Ref<godot::Texture2DRD> display_texture;
};

std::mutex g_render_thread_identity_mutex;
std::thread::id g_known_render_thread_id{};

void remember_render_thread() {
  std::lock_guard<std::mutex> lock(g_render_thread_identity_mutex);
  g_known_render_thread_id = std::this_thread::get_id();
}

bool current_thread_is_known_render_thread() {
  std::lock_guard<std::mutex> lock(g_render_thread_identity_mutex);
  return g_known_render_thread_id != std::thread::id{} && g_known_render_thread_id == std::this_thread::get_id();
}

void execute_render_thread_task(const std::shared_ptr<RenderThreadTaskState>& state);

bool run_render_thread_task_sync(const std::shared_ptr<RenderThreadTaskState>& state) {
  if (!state || bridge_teardown_started()) {
    return false;
  }
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    return false;
  }
  if (current_thread_is_known_render_thread()) {
    execute_render_thread_task(state);
    return state->success;
  }

  godot::Ref<RenderThreadTaskHelper> helper;
  helper.instantiate();
  if (helper.is_null()) {
    return false;
  }
  helper->init(state);
  // The helper owns a shared reference to the task state, and this stack frame
  // keeps the helper alive while the caller waits for render-thread completion.
  rs->call_on_render_thread(callable_mp(helper.ptr(), &RenderThreadTaskHelper::run_on_render_thread));

  std::unique_lock<std::mutex> lock(state->mutex);
  state->cv.wait(lock, [&]() { return state->done; });
  return state->success;
}

void RenderThreadTaskHelper::init(const std::shared_ptr<RenderThreadTaskState>& state) {
  state_ = state;
}

void RenderThreadTaskHelper::run_on_render_thread() {
  remember_render_thread();
  execute_render_thread_task(state_);
  state_.reset();
}




static void abandon_display_texture_after_failed_render_release(godot::Ref<godot::Texture2DRD>& texture) {
  if (texture.is_null()) {
    return;
  }
  // If Godot is already tearing the render thread down, dropping the final
  // Texture2DRD reference on the caller thread can invoke RenderingServer::free
  // from the wrong thread. Keep the wrapper alive until process teardown instead
  // of risking a shutdown hang or engine thread-guard error.
  static auto* abandoned_textures = new std::vector<godot::Ref<godot::Texture2DRD>>();
  abandoned_textures->push_back(texture);
  texture.unref();
}

void RetainedSyntheticGpuBacking::release_now() {
  godot::RID rid;
  godot::Ref<godot::Texture2DRD> texture_to_release;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (released) {
      return;
    }
    released = true;
    global_resource_aggregate_telemetry().retained_gpu_backing_released(telemetry_key);

    if (rd_texture.is_valid()) {
      rid = rd_texture;
      rd_texture = godot::RID();
    }

    if (display_texture.is_valid()) {
      texture_to_release = display_texture;
      rid = display_texture->get_texture_rd_rid();
      display_texture.unref();
    }
  }

  if (texture_to_release.is_valid()) {
    auto state = std::make_shared<RenderThreadTaskState>();
    state->kind = RenderThreadTaskState::Kind::RELEASE_DISPLAY_TEXTURE;
    state->rid = rid;
    state->display_texture = texture_to_release;
    if (!run_render_thread_task_sync(state)) {
      abandon_display_texture_after_failed_render_release(texture_to_release);
    }
    return;
  }

  if (!rid.is_valid()) {
    return;
  }
  if (enqueue_pending_release(rid)) {
    request_pending_release_drain();
  }
}

static std::mutex g_pending_release_mutex;
// RIDs that must be released from the render thread.
static std::vector<godot::RID> g_pending_releases;
static bool g_pending_release_drain_scheduled = false;
static bool g_bridge_teardown_started = false;
static godot::Ref<RenderThreadDrainHelper> g_render_thread_drain_helper;

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
  remember_render_thread();
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


void finish_render_thread_task(const std::shared_ptr<RenderThreadTaskState>& state, bool success) {
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->success = success;
    state->done = true;
  }
  state->cv.notify_all();
}

void execute_render_thread_task(const std::shared_ptr<RenderThreadTaskState>& state) {
  remember_render_thread();
  if (!state) {
    return;
  }

  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  godot::RenderingDevice* rd = rs ? rs->get_rendering_device() : nullptr;
  bool success = false;

  switch (state->kind) {
    case RenderThreadTaskState::Kind::TEXTURE_READBACK: {
      if (rd && state->rid.is_valid()) {
        state->readback = rd->texture_get_data(state->rid, 0);
        success = state->readback.size() > 0;
      }
    } break;

    case RenderThreadTaskState::Kind::ROUNDTRIP_RGBA8: {
      if (rd && state->width != 0 && state->height != 0 && state->stride_bytes == state->width * 4u &&
          state->bytes.size() > 0) {
        godot::Ref<godot::RDTextureFormat> format;
        format.instantiate();
        format->set_width(static_cast<int64_t>(state->width));
        format->set_height(static_cast<int64_t>(state->height));
        format->set_format(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
        format->set_usage_bits(
            godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
            godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
            godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);

        godot::Ref<godot::RDTextureView> view;
        view.instantiate();
        godot::Array data;
        data.push_back(state->bytes);
        const godot::RID tex = rd->texture_create(format, view, data);
        if (tex.is_valid()) {
          state->readback = rd->texture_get_data(tex, 0);
          rd->free_rid(tex);
          success = state->readback.size() > 0;
        }
      }
    } break;

    case RenderThreadTaskState::Kind::CREATE_TEXTURE_RGBA8: {
      if (rd && state->width != 0 && state->height != 0 && state->stride_bytes == state->width * 4u &&
          state->bytes.size() > 0) {
        godot::Ref<godot::RDTextureFormat> format;
        format.instantiate();
        format->set_width(static_cast<int64_t>(state->width));
        format->set_height(static_cast<int64_t>(state->height));
        format->set_format(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
        format->set_usage_bits(state->usage_bits);

        godot::Ref<godot::RDTextureView> view;
        view.instantiate();
        godot::Array data;
        data.push_back(state->bytes);
        state->rid = rd->texture_create(format, view, data);
        success = state->rid.is_valid();
      }
    } break;

    case RenderThreadTaskState::Kind::RELEASE_DISPLAY_TEXTURE: {
      if (state->display_texture.is_valid()) {
        state->display_texture->set_texture_rd_rid(godot::RID());
        state->display_texture.unref();
      }
      if (rd && state->rid.is_valid()) {
        if (gpu_trace_enabled()) {
          godot::UtilityFunctions::print("[CamBANG][SyntheticGpu] texture_free rid=", state->rid.get_id());
        }
        rd->free_rid(state->rid);
      }
      success = true;
    } break;

    case RenderThreadTaskState::Kind::NONE:
    default:
      break;
  }

  finish_render_thread_task(state, success);
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
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes,
    std::vector<uint8_t>& out) noexcept {
  if (bridge_teardown_started()) {
    return false;
  }
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs || !src || width == 0 || height == 0) {
    trace_gpu("roundtrip_result texture_create=false readback=false success=false reason=invalid_input_or_server");
    return false;
  }
  if (!rs->get_rendering_device()) {
    trace_gpu("roundtrip_result texture_create=false readback=false success=false reason=no_global_rd");
    return false;
  }
  if (stride_bytes != width * 4u) {
    trace_gpu("roundtrip_result texture_create=false readback=false success=false reason=unexpected_stride");
    return false;
  }

  const size_t expected_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  auto state = std::make_shared<RenderThreadTaskState>();
  state->kind = RenderThreadTaskState::Kind::ROUNDTRIP_RGBA8;
  state->width = width;
  state->height = height;
  state->stride_bytes = stride_bytes;
  state->bytes.resize(static_cast<int64_t>(expected_size));
  std::memcpy(state->bytes.ptrw(), src, expected_size);

  if (!run_render_thread_task_sync(state)) {
    trace_gpu("roundtrip_result texture_create=false readback=false success=false reason=render_thread_task_failed");
    return false;
  }
  if (static_cast<size_t>(state->readback.size()) < expected_size) {
    trace_gpu("roundtrip_result texture_create=true readback=false success=false reason=short_readback");
    return false;
  }
  out.resize(expected_size);
  std::memcpy(out.data(), state->readback.ptr(), out.size());
  trace_gpu("roundtrip_result texture_create=true readback=true success=true");
  return true;
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

  auto create_state = std::make_shared<RenderThreadTaskState>();
  create_state->kind = RenderThreadTaskState::Kind::CREATE_TEXTURE_RGBA8;
  create_state->width = width;
  create_state->height = height;
  create_state->stride_bytes = stride_bytes;
  create_state->usage_bits =
      godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
      godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
      godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT;
  create_state->bytes.resize(static_cast<int64_t>(stride_bytes) * static_cast<int64_t>(height));
  std::memcpy(create_state->bytes.ptrw(), src, static_cast<size_t>(create_state->bytes.size()));
  if (!run_render_thread_task_sync(create_state) || !create_state->rid.is_valid()) {
    return {};
  }
  const godot::RID texture = create_state->rid;
  if (gpu_trace_enabled()) {
    godot::UtilityFunctions::print("[CamBANG][SyntheticGpu] texture_alloc kind=retain_primary rid=", texture.get_id(),
                                   " w=", (long long)width,
                                   " h=", (long long)height);
  }

  auto retained_backing = std::make_shared<RetainedSyntheticGpuBacking>();
  retained_backing->telemetry_key = make_unknown_scoped_resource_telemetry();
  global_resource_aggregate_telemetry().retained_gpu_backing_created(retained_backing->telemetry_key);
  retained_backing->rd_texture = texture;
  retained_backing->width = width;
  retained_backing->height = height;
  return std::static_pointer_cast<void>(retained_backing);
}

std::shared_ptr<void> create_stream_live_gpu_backing_rgba8(
    uint64_t stream_id,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  if (bridge_teardown_started()) {
    return {};
  }
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

  auto create_state = std::make_shared<RenderThreadTaskState>();
  create_state->kind = RenderThreadTaskState::Kind::CREATE_TEXTURE_RGBA8;
  create_state->width = width;
  create_state->height = height;
  create_state->stride_bytes = stride_bytes;
  create_state->usage_bits =
      godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
      godot::RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
      godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT;
  create_state->bytes.resize(static_cast<int64_t>(stride_bytes) * static_cast<int64_t>(height));
  std::memset(create_state->bytes.ptrw(), 0, static_cast<size_t>(create_state->bytes.size()));
  if (!run_render_thread_task_sync(create_state) || !create_state->rid.is_valid()) {
    return {};
  }
  const godot::RID texture = create_state->rid;
  if (gpu_trace_enabled()) {
    godot::UtilityFunctions::print("[CamBANG][SyntheticGpu] texture_alloc kind=stream_live rid=", texture.get_id(),
                                   " w=", (long long)width,
                                   " h=", (long long)height);
  }

  auto retained_backing = std::make_shared<RetainedSyntheticGpuBacking>();
  retained_backing->telemetry_key = make_stream_scoped_resource_telemetry(stream_id);
  global_resource_aggregate_telemetry().retained_gpu_backing_created(retained_backing->telemetry_key);
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
  clear_pending_releases_for_teardown();
  trace_gpu("bridge_uninstall runtime_ops_registered=false");
}

godot::Ref<godot::Texture2D> synthetic_gpu_backing_display_texture(const std::shared_ptr<void>& backing) {
  if (bridge_teardown_started()) {
    return {};
  }
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
    // Display view is a live wrapper over stream-owned RD backing. The RD RID
    // remains owned by RetainedSyntheticGpuBacking; Texture2DRD's RenderingServer
    // wrapper is cleared on the render thread before the retained backing frees it.
    texture->set_texture_rd_rid(retained->rd_texture);
    retained->rd_texture = godot::RID();
    retained->display_texture = texture;
  }
  return retained->display_texture;
}

bool synthetic_gpu_backing_can_materialize_to_image(const std::shared_ptr<void>& backing) {
  if (bridge_teardown_started()) {
    return false;
  }
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
  if (bridge_teardown_started()) {
    return {};
  }
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
  auto state = std::make_shared<RenderThreadTaskState>();
  state->kind = RenderThreadTaskState::Kind::TEXTURE_READBACK;
  state->rid = snapshot.rid;
  if (!run_render_thread_task_sync(state) || state->readback.size() <= 0) {
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
                  state->readback);
  if (image->is_empty()) {
    return {};
  }
  return image;
}

} // namespace cambang

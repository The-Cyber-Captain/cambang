#include "imaging/synthetic/gpu_backing_runtime.h"
#include "godot/synthetic_gpu_backing_bridge_internal.h"
#include "godot/godot_gpu_display_service.h"
#include "core/resource_aggregate_telemetry.h"

#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <chrono>
#include <utility>
#include <vector>

#include <godot_cpp/core/callable_method_pointer.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string_name.hpp>

namespace cambang {

void register_synthetic_gpu_backing_internal_classes();

static bool enqueue_pending_release(const godot::RID& rid);
static void request_pending_release_drain();
static void schedule_render_thread_drain(godot::RenderingServer* rs, RenderThreadDrainHelper* helper);

struct SharedDisplayTextureRidState;
static bool enqueue_pending_texture_wrapper_release(
    godot::Ref<godot::Texture2DRD> texture,
    std::shared_ptr<SharedDisplayTextureRidState> state);
static bool gpu_trace_enabled() noexcept {
  const char* value = std::getenv("CAMBANG_DEV_SYNTH_GPU_TRACE");
  return value && value[0] != '\0' && value[0] != '0';
}


struct SharedDisplayTextureRidState final {
  std::mutex mutex;
  godot::RID rd_texture;
  bool abandoned_after_runtime_stop = false;

  explicit SharedDisplayTextureRidState(const godot::RID& rid) : rd_texture(rid) {}

  ~SharedDisplayTextureRidState() {
    release_now();
  }

  godot::RID snapshot_rid() {
    std::lock_guard<std::mutex> lock(mutex);
    return rd_texture;
  }

  void mark_abandoned_after_runtime_stop() {
    std::lock_guard<std::mutex> lock(mutex);
    abandoned_after_runtime_stop = true;
  }

  void release_now() {
    godot::RID rid;
    bool abandon_release = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      rid = rd_texture;
      rd_texture = godot::RID();
      abandon_release = abandoned_after_runtime_stop;
    }
    if (!rid.is_valid() || abandon_release) {
      return;
    }
    if (enqueue_pending_release(rid)) {
      request_pending_release_drain();
    }
  }
};

struct RetainedSyntheticGpuBacking final {
  std::mutex mutex;
  std::shared_ptr<SharedDisplayTextureRidState> rid_state;
  godot::PackedByteArray upload_bytes;
  uint64_t stream_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
  bool released = false;
  ScopedResourceTelemetryKey telemetry_key{};

  void release_now() {
    std::shared_ptr<SharedDisplayTextureRidState> state;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (released) {
        return;
      }
      released = true;
      global_resource_aggregate_telemetry().retained_gpu_backing_released(telemetry_key);
      state = std::move(rid_state);
    }
    // Drop CamBANG's backing reference after releasing the backing lock. If
    // user-facing display wrappers still hold metadata references to the same
    // state, final RID cleanup is deferred until those wrappers are destroyed.
    (void)state;
  }

  ~RetainedSyntheticGpuBacking() {
    release_now();
  }
};

class DeferredDisplayTexture2DRD : public godot::Texture2D {
  GDCLASS(DeferredDisplayTexture2DRD, godot::Texture2D);

public:
  static void _bind_methods() {}

  ~DeferredDisplayTexture2DRD() override;

  void init(
      godot::Ref<godot::Texture2DRD> texture,
      std::shared_ptr<SharedDisplayTextureRidState> state,
      uint32_t width,
      uint32_t height);

  int32_t _get_width() const override;
  int32_t _get_height() const override;
  bool _is_pixel_opaque(int32_t x, int32_t y) const override;
  bool _has_alpha() const override;
  void _draw(
      const godot::RID& to_canvas_item,
      const godot::Vector2& pos,
      const godot::Color& modulate,
      bool transpose) const override;
  void _draw_rect(
      const godot::RID& to_canvas_item,
      const godot::Rect2& rect,
      bool tile,
      const godot::Color& modulate,
      bool transpose) const override;
  void _draw_rect_region(
      const godot::RID& to_canvas_item,
      const godot::Rect2& rect,
      const godot::Rect2& src_rect,
      const godot::Color& modulate,
      bool transpose,
      bool clip_uv) const override;

private:
  godot::Ref<godot::Texture2DRD> texture_;
  std::shared_ptr<SharedDisplayTextureRidState> state_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

class DisplayTextureRidOwner : public godot::RefCounted {
  GDCLASS(DisplayTextureRidOwner, godot::RefCounted);

public:
  static void _bind_methods() {}

  ~DisplayTextureRidOwner() override;

  void init(uint64_t stream_id, std::shared_ptr<SharedDisplayTextureRidState> state);

private:
  uint64_t stream_id_ = 0;
  uint64_t borrow_id_ = 0;
  std::shared_ptr<SharedDisplayTextureRidState> state_;
};

void register_synthetic_gpu_backing_internal_classes() {
  // Internal-only helpers: registered only so Ref<...>::instantiate() can
  // resolve through ClassDB. These are not user-facing CamBANG API classes.
  godot::ClassDB::register_class<RenderThreadDrainHelper>();
  godot::ClassDB::register_class<DeferredDisplayTexture2DRD>();
  godot::ClassDB::register_class<DisplayTextureRidOwner>();
}

static std::mutex g_pending_release_mutex;
// RIDs that must be released from the render thread.
static std::vector<godot::RID> g_pending_releases;

struct PendingTextureWrapperRelease final {
  // Keep backing state alive until after Texture2DRD has freed its internal
  // RenderingServer texture wrapper. Members are destroyed in reverse order,
  // so texture is released before state.
  std::shared_ptr<SharedDisplayTextureRidState> state;
  godot::Ref<godot::Texture2DRD> texture;
};

// Texture2DRD wrappers that must be unreferenced on the render thread because
// their destructor frees Godot's internal RenderingServer texture wrapper.
static std::vector<PendingTextureWrapperRelease> g_pending_texture_wrapper_releases;
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

static bool enqueue_pending_texture_wrapper_release(
    godot::Ref<godot::Texture2DRD> texture,
    std::shared_ptr<SharedDisplayTextureRidState> state) {
  if (texture.is_null()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  if (g_bridge_teardown_started) {
    return false;
  }
  g_pending_texture_wrapper_releases.push_back(PendingTextureWrapperRelease{std::move(state), std::move(texture)});
  return true;
}

static bool bridge_teardown_started() {
  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  return g_bridge_teardown_started;
}

static void schedule_render_thread_drain(godot::RenderingServer* rs, RenderThreadDrainHelper* helper) {
  if (!rs || !helper) {
    return;
  }
  rs->call_on_render_thread(godot::callable_mp(
      helper,
      &RenderThreadDrainHelper::drain_pending_releases_on_render_thread));
}

static void clear_pending_releases_for_teardown() {
  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  g_bridge_teardown_started = true;
  // After teardown starts, do not schedule render-thread callbacks back into this
  // GDExtension. Pending RIDs are abandoned best-effort rather than risking a
  // late callback into torn-down extension or RenderingServer state.
  g_pending_releases.clear();
  g_pending_texture_wrapper_releases.clear();
  g_pending_release_drain_scheduled = false;
  g_render_thread_drain_helper.unref();
}

static void reset_bridge_teardown_state_for_install() {
  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  g_bridge_teardown_started = false;
  g_pending_releases.clear();
  g_pending_texture_wrapper_releases.clear();
  g_pending_release_drain_scheduled = false;
  g_render_thread_drain_helper.unref();
}

namespace {

struct LiveDisplayWrapperBorrow final {
  uint64_t stream_id = 0;
  std::shared_ptr<SharedDisplayTextureRidState> rid_state;
};

std::mutex g_live_display_wrapper_borrow_mutex;
std::map<uint64_t, LiveDisplayWrapperBorrow> g_live_display_wrapper_borrows;
uint64_t g_next_live_display_wrapper_borrow_id = 1;

uint64_t register_live_display_wrapper_borrow(
    uint64_t stream_id,
    const std::shared_ptr<SharedDisplayTextureRidState>& state) {
  if (stream_id == 0 || !state) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_live_display_wrapper_borrow_mutex);
  const uint64_t borrow_id = g_next_live_display_wrapper_borrow_id++;
  g_live_display_wrapper_borrows.emplace(borrow_id, LiveDisplayWrapperBorrow{stream_id, state});
  return borrow_id;
}

void unregister_live_display_wrapper_borrow(uint64_t borrow_id) {
  if (borrow_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_live_display_wrapper_borrow_mutex);
  g_live_display_wrapper_borrows.erase(borrow_id);
}

std::vector<LiveDisplayWrapperBorrow> snapshot_live_display_wrapper_borrows() {
  std::lock_guard<std::mutex> lock(g_live_display_wrapper_borrow_mutex);
  std::vector<LiveDisplayWrapperBorrow> borrows;
  borrows.reserve(g_live_display_wrapper_borrows.size());
  for (const auto& [borrow_id, borrow] : g_live_display_wrapper_borrows) {
    (void)borrow_id;
    borrows.push_back(borrow);
  }
  return borrows;
}

} // namespace

void DeferredDisplayTexture2DRD::init(
    godot::Ref<godot::Texture2DRD> texture,
    std::shared_ptr<SharedDisplayTextureRidState> state,
    uint32_t width,
    uint32_t height) {
  texture_ = std::move(texture);
  state_ = std::move(state);
  width_ = width;
  height_ = height;
}

DeferredDisplayTexture2DRD::~DeferredDisplayTexture2DRD() {
  godot::Ref<godot::Texture2DRD> texture = std::move(texture_);
  std::shared_ptr<SharedDisplayTextureRidState> state = std::move(state_);
  if (texture.is_valid()) {
    const bool enqueued = enqueue_pending_texture_wrapper_release(std::move(texture), std::move(state));
    if (enqueued) {
      request_pending_release_drain();
      return;
    }
  }
}

int32_t DeferredDisplayTexture2DRD::_get_width() const {
  return static_cast<int32_t>(width_);
}

int32_t DeferredDisplayTexture2DRD::_get_height() const {
  return static_cast<int32_t>(height_);
}

bool DeferredDisplayTexture2DRD::_is_pixel_opaque(int32_t x, int32_t y) const {
  (void)x;
  (void)y;
  return false;
}

bool DeferredDisplayTexture2DRD::_has_alpha() const {
  return true;
}

void DeferredDisplayTexture2DRD::_draw(
    const godot::RID& to_canvas_item,
    const godot::Vector2& pos,
    const godot::Color& modulate,
    bool transpose) const {
  if (texture_.is_valid()) {
    texture_->draw(to_canvas_item, pos, modulate, transpose);
  }
}

void DeferredDisplayTexture2DRD::_draw_rect(
    const godot::RID& to_canvas_item,
    const godot::Rect2& rect,
    bool tile,
    const godot::Color& modulate,
    bool transpose) const {
  if (texture_.is_valid()) {
    texture_->draw_rect(to_canvas_item, rect, tile, modulate, transpose);
  }
}

void DeferredDisplayTexture2DRD::_draw_rect_region(
    const godot::RID& to_canvas_item,
    const godot::Rect2& rect,
    const godot::Rect2& src_rect,
    const godot::Color& modulate,
    bool transpose,
    bool clip_uv) const {
  if (texture_.is_valid()) {
    texture_->draw_rect_region(to_canvas_item, rect, src_rect, modulate, transpose, clip_uv);
  }
}

void DisplayTextureRidOwner::init(uint64_t stream_id, std::shared_ptr<SharedDisplayTextureRidState> state) {
  stream_id_ = stream_id;
  state_ = std::move(state);
  borrow_id_ = register_live_display_wrapper_borrow(stream_id_, state_);
}

DisplayTextureRidOwner::~DisplayTextureRidOwner() {
  unregister_live_display_wrapper_borrow(borrow_id_);
  borrow_id_ = 0;
}

static void request_pending_release_drain() {
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    return;
  }

  godot::Ref<RenderThreadDrainHelper> helper_ref;
  RenderThreadDrainHelper* helper = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_pending_release_mutex);
    if (g_bridge_teardown_started ||
        (g_pending_releases.empty() && g_pending_texture_wrapper_releases.empty()) ||
        g_pending_release_drain_scheduled) {
      return;
    }

    if (g_render_thread_drain_helper.is_null()) {
      g_render_thread_drain_helper.instantiate();
    }
    helper_ref = g_render_thread_drain_helper;
    helper = helper_ref.ptr();
    if (!helper) {
      return;
    }

    g_pending_release_drain_scheduled = true;
  }

  schedule_render_thread_drain(rs, helper);
}

void RenderThreadDrainHelper::drain_pending_releases_on_render_thread() {
  std::vector<godot::RID> pending;
  std::vector<PendingTextureWrapperRelease> pending_texture_wrappers;
  {
    std::lock_guard<std::mutex> lock(g_pending_release_mutex);
    if (g_bridge_teardown_started) {
      g_pending_releases.clear();
      g_pending_texture_wrapper_releases.clear();
      g_pending_release_drain_scheduled = false;
      return;
    }
    pending.swap(g_pending_releases);
    pending_texture_wrappers.swap(g_pending_texture_wrapper_releases);
    g_pending_release_drain_scheduled = false;
  }

  pending_texture_wrappers.clear();

  auto schedule_again_if_needed = [](godot::RenderingServer* rs) {
    if (!rs) {
      return;
    }

    godot::Ref<RenderThreadDrainHelper> helper_ref;
    RenderThreadDrainHelper* helper = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_pending_release_mutex);
      if (g_bridge_teardown_started ||
          (g_pending_releases.empty() && g_pending_texture_wrapper_releases.empty()) ||
          g_pending_release_drain_scheduled) {
        return;
      }

      helper_ref = g_render_thread_drain_helper;
      helper = helper_ref.ptr();
      if (!helper) {
        return;
      }

      g_pending_release_drain_scheduled = true;
    }

    schedule_render_thread_drain(rs, helper);
  };

  if (pending.empty()) {
    schedule_again_if_needed(godot::RenderingServer::get_singleton());
    return;
  }

  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  godot::RenderingDevice* rd = rs ? rs->get_rendering_device() : nullptr;
  if (!rd) {
    {
      std::lock_guard<std::mutex> lock(g_pending_release_mutex);
      if (g_bridge_teardown_started) {
        g_pending_release_drain_scheduled = false;
        return;
      }
      for (godot::RID &rid : pending) {
        g_pending_releases.push_back(std::move(rid));
      }
    }

    schedule_again_if_needed(rs);
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

  {
    std::lock_guard<std::mutex> lock(g_pending_release_mutex);
    if (g_bridge_teardown_started) {
      g_pending_releases.clear();
      g_pending_release_drain_scheduled = false;
      return;
    }
  }

  schedule_again_if_needed(rs);
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
                                   " w=", static_cast<uint64_t>(width),
                                   " h=", static_cast<uint64_t>(height));
  }

  auto retained_backing = std::make_shared<RetainedSyntheticGpuBacking>();
  retained_backing->telemetry_key = make_unknown_scoped_resource_telemetry();
  global_resource_aggregate_telemetry().retained_gpu_backing_created(retained_backing->telemetry_key);
  retained_backing->rid_state = std::make_shared<SharedDisplayTextureRidState>(texture);
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
                                   " w=", static_cast<uint64_t>(width),
                                   " h=", static_cast<uint64_t>(height));
  }

  auto retained_backing = std::make_shared<RetainedSyntheticGpuBacking>();
  retained_backing->stream_id = stream_id;
  retained_backing->telemetry_key = make_stream_scoped_resource_telemetry(stream_id);
  global_resource_aggregate_telemetry().retained_gpu_backing_created(retained_backing->telemetry_key);
  retained_backing->rid_state = std::make_shared<SharedDisplayTextureRidState>(texture);
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
    if (retained->released || !retained->rid_state) {
      return false;
    }
    if (retained->width != width || retained->height != height || retained->stride_bytes != stride_bytes) {
      return false;
    }
    texture_rid = retained->rid_state->snapshot_rid();
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

bool can_materialize_to_image(const std::shared_ptr<void>& backing) noexcept {
  if (bridge_teardown_started() || !backing) {
    return false;
  }
  const std::shared_ptr<RetainedSyntheticGpuBacking> retained =
      std::static_pointer_cast<RetainedSyntheticGpuBacking>(backing);
  if (!retained) {
    return false;
  }
  std::lock_guard<std::mutex> lock(retained->mutex);
  if (retained->released ||
      retained->width == 0 ||
      retained->height == 0 ||
      retained->stride_bytes != retained->width * 4u) {
    return false;
  }
  const int64_t required =
      static_cast<int64_t>(retained->stride_bytes) * static_cast<int64_t>(retained->height);
  return required > 0 && retained->upload_bytes.size() >= required;
}

godot::Ref<godot::Image> materialize_to_image(const std::shared_ptr<void>& backing) {
  if (bridge_teardown_started() || !backing) {
    return {};
  }
  const std::shared_ptr<RetainedSyntheticGpuBacking> retained =
      std::static_pointer_cast<RetainedSyntheticGpuBacking>(backing);
  if (!retained) {
    return {};
  }

  godot::PackedByteArray bytes;
  uint32_t width = 0;
  uint32_t height = 0;
  {
    std::lock_guard<std::mutex> lock(retained->mutex);
    if (retained->released ||
        retained->width == 0 ||
        retained->height == 0 ||
        retained->stride_bytes != retained->width * 4u) {
      return {};
    }
    const int64_t required =
        static_cast<int64_t>(retained->stride_bytes) * static_cast<int64_t>(retained->height);
    if (required <= 0 || retained->upload_bytes.size() < required) {
      return {};
    }
    width = retained->width;
    height = retained->height;
    bytes = retained->upload_bytes;
    if (bytes.size() != required) {
      bytes.resize(required);
    }
  }

  return godot::Image::create_from_data(
      static_cast<int>(width),
      static_cast<int>(height),
      false,
      godot::Image::FORMAT_RGBA8,
      bytes);
}

const SyntheticGpuBackingRuntimeOps kOps{
    &global_rd_available,
    &global_rd_roundtrip_rgba8,
    &retain_primary_gpu_backing_rgba8,
    &create_stream_live_gpu_backing_rgba8,
    &update_stream_live_gpu_backing_rgba8,
    &release_stream_live_gpu_backing,
    &can_materialize_to_image,
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
  clear_pending_releases_for_teardown();
  trace_gpu("bridge_uninstall runtime_ops_registered=false");
}

void synthetic_gpu_backing_warn_and_abandon_live_display_wrappers_before_stop() {
  const std::vector<LiveDisplayWrapperBorrow> borrows = snapshot_live_display_wrapper_borrows();
  if (borrows.empty()) {
    return;
  }

  std::map<uint64_t, uint64_t> counts_by_stream_id;
  for (const LiveDisplayWrapperBorrow& borrow : borrows) {
    if (borrow.rid_state) {
      borrow.rid_state->mark_abandoned_after_runtime_stop();
    }
    ++counts_by_stream_id[borrow.stream_id];
  }

  const char* const message =
    "[CamBANG][DisplayLifetime] CamBANGServer.stop() called while GPU StreamResult display_view wrappers returned to Godot are still live. "
    "Release TextureRect.texture / display_view references before stop. Stop will continue; retained views may become stale after runtime teardown.";

  godot::UtilityFunctions::push_warning(message);

  for (const auto& [stream_id, borrow_count] : counts_by_stream_id) {
    godot::UtilityFunctions::push_warning("[CamBANG][DisplayLifetime] live_gpu_display_view stream_id=",
                                          static_cast<uint64_t>(stream_id),
                                          " wrapper_borrow_count=",
                                          static_cast<uint64_t>(borrow_count));
  }
}

godot::Ref<godot::Texture2D> synthetic_gpu_backing_display_texture(const std::shared_ptr<void>& backing) {
  constexpr const char* kDisplayTextureRidOwnerMetaKey = "__cambang_synth_gpu_rid_owner";
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

  std::shared_ptr<SharedDisplayTextureRidState> state;
  uint64_t stream_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  {
    std::lock_guard<std::mutex> lock(retained->mutex);
    if (retained->released || !retained->rid_state) {
      return {};
    }
    state = retained->rid_state;
    stream_id = retained->stream_id;
    width = retained->width;
    height = retained->height;
  }

  const godot::RID display_rid = state->snapshot_rid();
  if (!display_rid.is_valid()) {
    return {};
  }

  godot::Ref<godot::Texture2DRD> texture;
  texture.instantiate();
  if (texture.is_null()) {
    return {};
  }
  texture->set_texture_rd_rid(display_rid);

  godot::Ref<DeferredDisplayTexture2DRD> display_view;
  display_view.instantiate();
  if (display_view.is_null()) {
    return {};
  }
  display_view->init(texture, state, width, height);

  // Display view is a user-facing wrapper over stream-owned backing state.
  // The Texture2DRD delegate creates an internal RenderingServer texture wrapper
  // for display, but DeferredDisplayTexture2DRD moves delegate destruction to a
  // render-thread drain so ordinary TextureRect unbind does not synchronously
  // free that internal wrapper on the main thread.
  godot::Ref<DisplayTextureRidOwner> rid_owner;
  rid_owner.instantiate();
  if (rid_owner.is_null()) {
    return {};
  }
  rid_owner->init(stream_id, std::move(state));
  display_view->set_meta(godot::StringName(kDisplayTextureRidOwnerMetaKey), rid_owner);
  return display_view;
}

bool synthetic_gpu_backing_can_materialize_to_image(const std::shared_ptr<void>& backing) {
  return can_materialize_to_image(backing);
}

godot::Ref<godot::Image> synthetic_gpu_backing_materialize_to_image(const std::shared_ptr<void>& backing) {
  return materialize_to_image(backing);
}

} // namespace cambang

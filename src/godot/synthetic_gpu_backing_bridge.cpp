#include "imaging/synthetic/gpu_backing_runtime.h"
#include "godot/synthetic_gpu_backing_bridge_internal.h"
#include "godot/godot_gpu_display_service.h"
#include "godot/cambang_stream_result_internal.h"
#include "godot/render_resource_release_service.h"
#include "core/resource_aggregate_telemetry.h"

#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <chrono>
#include <atomic>
#include <utility>
#include <vector>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>

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

struct SharedDisplayTextureRidState;
static bool gpu_trace_enabled() noexcept {
  const char* value = std::getenv("CAMBANG_DEV_SYNTH_GPU_TRACE");
  return value && value[0] != '\0' && value[0] != '0';
}


struct SharedDisplayTextureRidState final {
  mutable std::mutex mutex;
  godot::RID rd_texture;
  RenderResourceReleaseReservation release_reservation;
  bool invalidated = false;

  SharedDisplayTextureRidState(
      const godot::RID& rid,
      RenderResourceReleaseReservation reservation) noexcept
      : rd_texture(rid), release_reservation(std::move(reservation)) {}

  ~SharedDisplayTextureRidState() {
    release_now();
  }

  godot::RID snapshot_rid() {
    std::lock_guard<std::mutex> lock(mutex);
    return rd_texture;
  }

  void mark_invalidated() {
    std::lock_guard<std::mutex> lock(mutex);
    invalidated = true;
  }

  bool draw_allowed() const {
    std::lock_guard<std::mutex> lock(mutex);
    return !invalidated && rd_texture.is_valid();
  }

  void release_now() {
    godot::RID rid;
    RenderResourceReleaseReservation reservation;
    {
      std::lock_guard<std::mutex> lock(mutex);
      rid = rd_texture;
      rd_texture = godot::RID();
      reservation = std::move(release_reservation);
      invalidated = true;
    }
    if (!rid.is_valid()) {
      return;
    }
    defer_render_resource_rid_release(
        GodotRenderResourceKind::RenderingDeviceRid,
        rid,
        std::move(reservation));
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

  bool init(
      godot::Ref<godot::Texture2DRD> texture,
      std::shared_ptr<SharedDisplayTextureRidState> state,
      RenderResourceReleaseReservation reservation,
      uint64_t stream_id,
      uint32_t width,
      uint32_t height) noexcept;
  void update_dimensions(uint32_t width, uint32_t height);

  int32_t _get_width() const override;
  int32_t _get_height() const override;
  bool _is_pixel_opaque(int32_t x, int32_t y) const override;
  bool _has_alpha() const override;
  godot::RID _get_rid() const override;
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
  void clear_runtime_references_() noexcept;

  godot::Ref<godot::Texture2DRD> texture_;
  std::shared_ptr<SharedDisplayTextureRidState> state_;
  RenderResourceReleaseReservation release_reservation_;
  std::shared_ptr<DisplayDemandLease> display_demand_lease_;
  uint64_t stream_id_ = 0;
  uint64_t borrow_id_ = 0;
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
  godot::ClassDB::register_class<DeferredDisplayTexture2DRD>();
  godot::ClassDB::register_class<DisplayTextureRidOwner>();
}

std::atomic<bool> g_bridge_teardown_started{false};

#if defined(CAMBANG_INTERNAL_SMOKE)
std::atomic<bool> g_force_primary_post_transfer_failure_once{false};
std::atomic<bool> g_force_wrapper_post_transfer_failure_once{false};
#endif

bool bridge_teardown_started() noexcept {
  return g_bridge_teardown_started.load(std::memory_order_acquire);
}

namespace {

struct LiveDisplayWrapperBorrow final {
  uint64_t stream_id = 0;
  uint64_t wrapper_instance_id = 0;
  std::shared_ptr<SharedDisplayTextureRidState> rid_state;
};

struct PendingLiveDisplayWrapperRefresh final {
  uint64_t stream_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

std::mutex g_live_display_wrapper_borrow_mutex;
std::map<uint64_t, LiveDisplayWrapperBorrow> g_live_display_wrapper_borrows;
uint64_t g_next_live_display_wrapper_borrow_id = 1;
std::mutex g_pending_live_display_wrapper_refresh_mutex;
std::map<uint64_t, PendingLiveDisplayWrapperRefresh> g_pending_live_display_wrapper_refreshes;

uint64_t register_live_display_wrapper_borrow(
    uint64_t stream_id,
    const std::shared_ptr<SharedDisplayTextureRidState>& state) {
  if (stream_id == 0 || !state) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_live_display_wrapper_borrow_mutex);
  RenderResourceReleaseRegistryLockScope release_lock_scope;
  const uint64_t borrow_id = g_next_live_display_wrapper_borrow_id++;
  g_live_display_wrapper_borrows.emplace(borrow_id, LiveDisplayWrapperBorrow{stream_id, 0, state});
  return borrow_id;
}

void set_live_display_wrapper_instance_id(uint64_t borrow_id, uint64_t wrapper_instance_id) {
  if (borrow_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_live_display_wrapper_borrow_mutex);
  RenderResourceReleaseRegistryLockScope release_lock_scope;
  const auto it = g_live_display_wrapper_borrows.find(borrow_id);
  if (it == g_live_display_wrapper_borrows.end()) {
    return;
  }
  it->second.wrapper_instance_id = wrapper_instance_id;
}

void unregister_live_display_wrapper_borrow(uint64_t borrow_id) {
  if (borrow_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_live_display_wrapper_borrow_mutex);
  RenderResourceReleaseRegistryLockScope release_lock_scope;
  g_live_display_wrapper_borrows.erase(borrow_id);
}

void queue_live_display_wrapper_refresh(uint64_t stream_id, uint32_t width, uint32_t height) {
  if (stream_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_pending_live_display_wrapper_refresh_mutex);
  g_pending_live_display_wrapper_refreshes[stream_id] = PendingLiveDisplayWrapperRefresh{stream_id, width, height};
}

std::vector<PendingLiveDisplayWrapperRefresh> take_pending_live_display_wrapper_refreshes() {
  std::lock_guard<std::mutex> lock(g_pending_live_display_wrapper_refresh_mutex);
  std::vector<PendingLiveDisplayWrapperRefresh> refreshes;
  refreshes.reserve(g_pending_live_display_wrapper_refreshes.size());
  for (const auto& [stream_id, refresh] : g_pending_live_display_wrapper_refreshes) {
    (void)stream_id;
    refreshes.push_back(refresh);
  }
  g_pending_live_display_wrapper_refreshes.clear();
  return refreshes;
}

std::vector<LiveDisplayWrapperBorrow> snapshot_live_display_wrapper_borrows() {
  std::lock_guard<std::mutex> lock(g_live_display_wrapper_borrow_mutex);
  RenderResourceReleaseRegistryLockScope release_lock_scope;
  std::vector<LiveDisplayWrapperBorrow> borrows;
  borrows.reserve(g_live_display_wrapper_borrows.size());
  for (const auto& [borrow_id, borrow] : g_live_display_wrapper_borrows) {
    (void)borrow_id;
    borrows.push_back(borrow);
  }
  return borrows;
}

} // namespace

bool DeferredDisplayTexture2DRD::init(
    godot::Ref<godot::Texture2DRD> texture,
    std::shared_ptr<SharedDisplayTextureRidState> state,
    RenderResourceReleaseReservation reservation,
    uint64_t stream_id,
    uint32_t width,
    uint32_t height) noexcept {
  // This method is called exactly once on a newly instantiated wrapper. Move
  // render ownership first so any later registry/allocation failure is handled
  // by this wrapper's deferred destructor rather than by the caller's raw RID.
  texture_ = std::move(texture);
  state_ = std::move(state);
  release_reservation_ = std::move(reservation);
  stream_id_ = stream_id;
  width_ = width;
  height_ = height;
  try {
    borrow_id_ = register_live_display_wrapper_borrow(stream_id_, state_);
    set_live_display_wrapper_instance_id(borrow_id_, static_cast<uint64_t>(get_instance_id()));
    if (stream_id != 0) {
      display_demand_lease_ = retain_display_demand_lease(stream_id);
    }
#if defined(CAMBANG_INTERNAL_SMOKE)
    if (g_force_wrapper_post_transfer_failure_once.exchange(false, std::memory_order_acq_rel)) {
      throw 1;
    }
#endif
    return true;
  } catch (...) {
    return false;
  }
}

DeferredDisplayTexture2DRD::~DeferredDisplayTexture2DRD() {
  clear_runtime_references_();
  godot::Ref<godot::Texture2DRD> texture = std::move(texture_);
  std::shared_ptr<SharedDisplayTextureRidState> state = std::move(state_);
  defer_texture2drd_release(
      std::move(texture),
      std::static_pointer_cast<void>(std::move(state)),
      std::move(release_reservation_));
}

void DeferredDisplayTexture2DRD::update_dimensions(uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;
  emit_changed();
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

godot::RID DeferredDisplayTexture2DRD::_get_rid() const {
  return state_ && state_->draw_allowed() ? state_->snapshot_rid() : godot::RID();
}

void DeferredDisplayTexture2DRD::_draw(
    const godot::RID& to_canvas_item,
    const godot::Vector2& pos,
    const godot::Color& modulate,
    bool transpose) const {
  if (texture_.is_valid() && state_ && state_->draw_allowed()) {
    texture_->draw(to_canvas_item, pos, modulate, transpose);
  }
}

void DeferredDisplayTexture2DRD::_draw_rect(
    const godot::RID& to_canvas_item,
    const godot::Rect2& rect,
    bool tile,
    const godot::Color& modulate,
    bool transpose) const {
  if (texture_.is_valid() && state_ && state_->draw_allowed()) {
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
  if (texture_.is_valid() && state_ && state_->draw_allowed()) {
    texture_->draw_rect_region(to_canvas_item, rect, src_rect, modulate, transpose, clip_uv);
  }
}

void DeferredDisplayTexture2DRD::clear_runtime_references_() noexcept {
  try {
    unregister_live_display_wrapper_borrow(borrow_id_);
  } catch (...) {
  }
  borrow_id_ = 0;
  display_demand_lease_.reset();
  stream_id_ = 0;
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
  RenderResourceReleaseReservation reservation = reserve_render_resource_release();
  if (!reservation) {
    return {};
  }
  godot::RID texture;
  bool local_ownership = false;
  try {
    texture = rd->texture_create(format, view, data);
    if (!texture.is_valid()) {
      return {};
    }
    local_ownership = true;
    if (gpu_trace_enabled()) {
      godot::UtilityFunctions::print("[CamBANG][SyntheticGpu] texture_alloc kind=retain_primary rid=", texture.get_id(),
                                     " w=", static_cast<uint64_t>(width),
                                     " h=", static_cast<uint64_t>(height));
    }
    auto retained_backing = std::make_shared<RetainedSyntheticGpuBacking>();
    retained_backing->telemetry_key = make_unknown_scoped_resource_telemetry();
    global_resource_aggregate_telemetry().retained_gpu_backing_created(retained_backing->telemetry_key);
    retained_backing->width = width;
    retained_backing->height = height;
    retained_backing->stride_bytes = stride_bytes;
    retained_backing->upload_bytes = bytes;
    std::shared_ptr<SharedDisplayTextureRidState> state =
        std::make_shared<SharedDisplayTextureRidState>(
        texture, std::move(reservation));
    local_ownership = false;
    retained_backing->rid_state = std::move(state);
#if defined(CAMBANG_INTERNAL_SMOKE)
    if (g_force_primary_post_transfer_failure_once.exchange(false, std::memory_order_acq_rel)) {
      throw 1;
    }
#endif
    return std::static_pointer_cast<void>(retained_backing);
  } catch (...) {
    if (local_ownership) {
      defer_render_resource_rid_release(
          GodotRenderResourceKind::RenderingDeviceRid,
          texture,
          std::move(reservation));
    }
    return {};
  }
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
  RenderResourceReleaseReservation reservation = reserve_render_resource_release();
  if (!reservation) {
    return {};
  }
  godot::RID texture;
  bool local_ownership = false;
  try {
    texture = rd->texture_create(format, view, data);
    if (!texture.is_valid()) {
      return {};
    }
    local_ownership = true;
    if (gpu_trace_enabled()) {
      godot::UtilityFunctions::print("[CamBANG][SyntheticGpu] texture_alloc kind=stream_live rid=", texture.get_id(),
                                     " w=", static_cast<uint64_t>(width),
                                     " h=", static_cast<uint64_t>(height));
    }
    auto retained_backing = std::make_shared<RetainedSyntheticGpuBacking>();
    retained_backing->stream_id = stream_id;
    retained_backing->telemetry_key = make_stream_scoped_resource_telemetry(stream_id);
    global_resource_aggregate_telemetry().retained_gpu_backing_created(retained_backing->telemetry_key);
    retained_backing->width = width;
    retained_backing->height = height;
    retained_backing->stride_bytes = stride_bytes;
    std::shared_ptr<SharedDisplayTextureRidState> state =
        std::make_shared<SharedDisplayTextureRidState>(
            texture, std::move(reservation));
    local_ownership = false;
    retained_backing->rid_state = std::move(state);
    return std::static_pointer_cast<void>(retained_backing);
  } catch (...) {
    if (local_ownership) {
      defer_render_resource_rid_release(
          GodotRenderResourceKind::RenderingDeviceRid,
          texture,
          std::move(reservation));
    }
    return {};
  }
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
  queue_live_display_wrapper_refresh(retained->stream_id, width, height);
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
  g_bridge_teardown_started.store(false, std::memory_order_release);
  set_synthetic_gpu_backing_runtime_ops(&kOps);
  trace_gpu("bridge_install runtime_ops_registered=true");
}

void uninstall_synthetic_gpu_backing_godot_bridge() {
  clear_synthetic_gpu_backing_runtime_ops();
  godot_gpu_display_invalidate_all();
  g_bridge_teardown_started.store(true, std::memory_order_release);
  trace_gpu("bridge_uninstall runtime_ops_registered=false");
}

void synthetic_gpu_backing_warn_and_invalidate_live_display_wrappers_before_stop() {
  const std::vector<LiveDisplayWrapperBorrow> borrows = snapshot_live_display_wrapper_borrows();
  if (borrows.empty()) {
    return;
  }

  std::map<uint64_t, uint64_t> counts_by_stream_id;
  for (const LiveDisplayWrapperBorrow& borrow : borrows) {
    if (borrow.rid_state) {
      borrow.rid_state->mark_invalidated();
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

void synthetic_gpu_backing_invalidate_live_display_wrappers_for_stream(uint64_t stream_id) {
  if (stream_id == 0) {
    return;
  }
  const std::vector<LiveDisplayWrapperBorrow> borrows = snapshot_live_display_wrapper_borrows();
  for (const LiveDisplayWrapperBorrow& borrow : borrows) {
    if (borrow.stream_id != stream_id || !borrow.rid_state) {
      continue;
    }
    borrow.rid_state->mark_invalidated();
  }
}

void synthetic_gpu_backing_invalidate_all_live_display_wrappers() {
  const std::vector<LiveDisplayWrapperBorrow> borrows = snapshot_live_display_wrapper_borrows();
  for (const LiveDisplayWrapperBorrow& borrow : borrows) {
    if (!borrow.rid_state) {
      continue;
    }
    borrow.rid_state->mark_invalidated();
  }
}

void synthetic_gpu_backing_drain_pending_live_display_wrapper_refreshes() {
  const std::vector<PendingLiveDisplayWrapperRefresh> refreshes =
      take_pending_live_display_wrapper_refreshes();
  if (refreshes.empty()) {
    return;
  }

  std::map<uint64_t, PendingLiveDisplayWrapperRefresh> refreshes_by_stream_id;
  for (const PendingLiveDisplayWrapperRefresh& refresh : refreshes) {
    if (refresh.stream_id == 0) {
      continue;
    }
    refreshes_by_stream_id[refresh.stream_id] = refresh;
  }
  if (refreshes_by_stream_id.empty()) {
    return;
  }

  const std::vector<LiveDisplayWrapperBorrow> borrows = snapshot_live_display_wrapper_borrows();
  for (const LiveDisplayWrapperBorrow& borrow : borrows) {
    if (borrow.stream_id == 0 || borrow.wrapper_instance_id == 0) {
      continue;
    }
    const auto refresh_it = refreshes_by_stream_id.find(borrow.stream_id);
    if (refresh_it == refreshes_by_stream_id.end()) {
      continue;
    }
    godot::Object* object = godot::ObjectDB::get_instance(borrow.wrapper_instance_id);
    DeferredDisplayTexture2DRD* wrapper = godot::Object::cast_to<DeferredDisplayTexture2DRD>(object);
    if (!wrapper) {
      continue;
    }
    wrapper->update_dimensions(refresh_it->second.width, refresh_it->second.height);
  }
}

godot::Ref<godot::Texture2D> synthetic_gpu_backing_display_texture(const std::shared_ptr<void>& backing) {
  constexpr const char* kDisplayTextureRidOwnerMetaKey = "__cambang_synth_gpu_rid_owner";
  if (bridge_teardown_started()) {
    return {};
  }
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
  RenderResourceReleaseReservation wrapper_reservation = reserve_render_resource_release();
  if (!wrapper_reservation) {
    return {};
  }
  godot::Ref<DeferredDisplayTexture2DRD> display_view;
  bool wrapper_ownership_transferred = false;
  try {
    texture.instantiate();
    if (texture.is_null()) {
      return {};
    }
    texture->set_texture_rd_rid(display_rid);

    display_view.instantiate();
    if (display_view.is_null()) {
      defer_texture2drd_release(
          std::move(texture), {}, std::move(wrapper_reservation));
      return {};
    }
    const bool initialized = display_view->init(
        std::move(texture),
        state,
        std::move(wrapper_reservation),
        stream_id,
        width,
        height);
    wrapper_ownership_transferred = true;
    if (!initialized) {
      return {};
    }

    // Display view is a user-facing wrapper over stream-owned backing state.
    // Its delegate owns an internal RenderingServer wrapper whose final Ref is
    // therefore confined to the approved render drain.
    godot::Ref<DisplayTextureRidOwner> rid_owner;
    rid_owner.instantiate();
    if (rid_owner.is_null()) {
      return {};
    }
    rid_owner->init(stream_id, std::move(state));
    display_view->set_meta(godot::StringName(kDisplayTextureRidOwnerMetaKey), rid_owner);
    return display_view;
  } catch (...) {
    if (!wrapper_ownership_transferred && texture.is_valid()) {
      defer_texture2drd_release(
          std::move(texture), {}, std::move(wrapper_reservation));
    }
    return {};
  }
}

godot::Ref<godot::Image> synthetic_gpu_backing_materialize_to_image(const std::shared_ptr<void>& backing) {
  return materialize_to_image(backing);
}

#if defined(CAMBANG_INTERNAL_SMOKE)
bool exercise_primary_gpu_post_transfer_failure_for_smoke() noexcept {
  constexpr uint8_t kPixel[4] = {1, 2, 3, 255};
  g_force_primary_post_transfer_failure_once.store(true, std::memory_order_release);
  const bool injected = !retain_primary_gpu_backing_rgba8(kPixel, 1, 1, 4);
  const bool reached_post_transfer = !g_force_primary_post_transfer_failure_once.exchange(
      false, std::memory_order_acq_rel);
  request_render_resource_release_drain_from_godot_thread();
  return injected && reached_post_transfer;
}

bool exercise_gpu_wrapper_post_transfer_failure_for_smoke() noexcept {
  try {
    constexpr uint8_t kPixel[4] = {1, 2, 3, 255};
    std::shared_ptr<void> backing = retain_primary_gpu_backing_rgba8(kPixel, 1, 1, 4);
    if (!backing) {
      return false;
    }
    g_force_wrapper_post_transfer_failure_once.store(true, std::memory_order_release);
    const godot::Ref<godot::Texture2D> display =
        synthetic_gpu_backing_display_texture(backing);
    const bool reached_post_transfer = !g_force_wrapper_post_transfer_failure_once.exchange(
        false, std::memory_order_acq_rel);
    backing.reset();
    request_render_resource_release_drain_from_godot_thread();
    return display.is_null() && reached_post_transfer;
  } catch (...) {
    g_force_wrapper_post_transfer_failure_once.store(false, std::memory_order_release);
    return false;
  }
}
#endif

} // namespace cambang

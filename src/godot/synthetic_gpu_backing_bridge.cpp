#include "imaging/synthetic/gpu_backing_runtime.h"
#include "godot/synthetic_gpu_backing_bridge_internal.h"
#include "godot/godot_gpu_display_service.h"
#include "godot/cambang_stream_result_internal.h"
#include "core/resource_aggregate_telemetry.h"

#include <cstring>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <chrono>
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

void register_synthetic_gpu_backing_internal_classes();

static bool enqueue_pending_release(const godot::RID& rid);
static void request_pending_release_drain();
static void schedule_render_thread_drain(godot::RenderingServer* rs, RenderThreadDrainHelper* helper);
static void unregister_display_texture_rid_state(uint64_t registration_id);

enum class RenderReleasePhase {
  Closed,
  Active,
  Draining,
};

static std::mutex g_pending_release_mutex;
static std::condition_variable g_pending_release_changed;
// RIDs that must be released from the render thread.
static std::vector<godot::RID> g_pending_releases;
static std::size_t g_release_producers = 0;
static bool g_pending_release_drain_scheduled = false;
static bool g_pending_release_drain_running = false;
static RenderReleasePhase g_render_release_phase = RenderReleasePhase::Closed;
static RenderThreadDrainHelper* g_render_thread_drain_helper = nullptr;

class RenderReleaseProducerLease final {
public:
  RenderReleaseProducerLease() {
    std::lock_guard<std::mutex> lock(g_pending_release_mutex);
    if (g_render_release_phase == RenderReleasePhase::Closed) {
      return;
    }
    ++g_release_producers;
    admitted_ = true;
  }

  RenderReleaseProducerLease(const RenderReleaseProducerLease&) = delete;
  RenderReleaseProducerLease& operator=(const RenderReleaseProducerLease&) = delete;

  ~RenderReleaseProducerLease() {
    if (!admitted_) {
      return;
    }
    std::lock_guard<std::mutex> lock(g_pending_release_mutex);
    --g_release_producers;
    g_pending_release_changed.notify_all();
  }

  explicit operator bool() const noexcept { return admitted_; }

private:
  bool admitted_ = false;
};

struct SharedDisplayTextureRidState;
static bool enqueue_pending_texture_wrapper_release(
    godot::Ref<godot::Texture2DRD> texture,
    std::shared_ptr<SharedDisplayTextureRidState> state);
static bool gpu_trace_enabled() noexcept {
  const char* value = std::getenv("CAMBANG_DEV_SYNTH_GPU_TRACE");
  return value && value[0] != '\0' && value[0] != '0';
}


struct SharedDisplayTextureRidState final {
  mutable std::mutex mutex;
  godot::RID rd_texture;
  bool invalidated = false;
  uint64_t registration_id = 0;

  explicit SharedDisplayTextureRidState(const godot::RID& rid) : rd_texture(rid) {}

  ~SharedDisplayTextureRidState() {
    release_now();
    unregister_display_texture_rid_state(registration_id);
  }

  godot::RID snapshot_rid() {
    std::lock_guard<std::mutex> lock(mutex);
    return rd_texture;
  }

  bool draw_allowed() const {
    std::lock_guard<std::mutex> lock(mutex);
    return !invalidated && rd_texture.is_valid();
  }

  // Godot's RenderingServer/RenderingDevice persist across CamBANG
  // start()/stop() cycles within one process. GDExtension unload first enters
  // Draining, where releases remain admissible, and closes admission only
  // after every registered state and wrapper has joined the final drain. A
  // CamBANG-level stop() alone must not skip release, or the RID leaks once the
  // final retained owner is dropped.
  void release_now() {
    RenderReleaseProducerLease release_admission;
    godot::RID rid;
    {
      std::lock_guard<std::mutex> lock(mutex);
      rid = rd_texture;
      rd_texture = godot::RID();
      invalidated = true;
    }
    if (!rid.is_valid()) {
      return;
    }
    if (release_admission && enqueue_pending_release(rid)) {
      request_pending_release_drain();
      return;
    }
    std::fprintf(stderr,
                 "[CamBANG][SyntheticGpu] release rejected after render bridge closure rid=%llu\n",
                 static_cast<unsigned long long>(rid.get_id()));
    std::fflush(stderr);
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
      uint64_t stream_id,
      uint32_t width,
      uint32_t height);
  void update_dimensions(uint32_t width, uint32_t height);
  void prepare_for_bridge_teardown();

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
  void clear_runtime_references_();
  void defer_texture_wrapper_release_();

  godot::Ref<godot::Texture2DRD> texture_;
  std::shared_ptr<SharedDisplayTextureRidState> state_;
  godot::Ref<DisplayDemandToken> display_demand_token_;
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
  // Internal-only helpers: registered only so bound callback targets can be
  // created through the GDExtension class machinery. These are not
  // user-facing CamBANG API classes.
  godot::ClassDB::register_class<RenderThreadDrainHelper>();
  godot::ClassDB::register_class<DeferredDisplayTexture2DRD>();
  godot::ClassDB::register_class<DisplayTextureRidOwner>();
}

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

static bool enqueue_pending_release(const godot::RID& rid) {
  if (!rid.is_valid()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  if (g_render_release_phase == RenderReleasePhase::Closed) {
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
  if (g_render_release_phase == RenderReleasePhase::Closed) {
    return false;
  }
  g_pending_texture_wrapper_releases.push_back(PendingTextureWrapperRelease{std::move(state), std::move(texture)});
  return true;
}

static bool bridge_teardown_started() {
  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  return g_render_release_phase != RenderReleasePhase::Active;
}

static void schedule_render_thread_drain(godot::RenderingServer* rs, RenderThreadDrainHelper* helper) {
  if (!rs || !helper) {
    return;
  }
  // Callable(Object*, StringName) may retain RefCounted targets in engine
  // queue storage, so this helper is deliberately a plain Object. The bridge
  // owns it until every scheduled/running callback has completed.
  rs->call_on_render_thread(godot::Callable(
      helper,
      godot::StringName("drain_pending_releases_on_render_thread")));
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
std::mutex g_display_texture_rid_state_registry_mutex;
std::map<uint64_t, std::weak_ptr<SharedDisplayTextureRidState>>
    g_display_texture_rid_state_registry;
uint64_t g_next_display_texture_rid_state_registration_id = 1;
std::mutex g_pending_live_display_wrapper_refresh_mutex;
std::map<uint64_t, PendingLiveDisplayWrapperRefresh> g_pending_live_display_wrapper_refreshes;

uint64_t register_live_display_wrapper_borrow(
    uint64_t stream_id,
    const std::shared_ptr<SharedDisplayTextureRidState>& state) {
  if (!state) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_live_display_wrapper_borrow_mutex);
  const uint64_t borrow_id = g_next_live_display_wrapper_borrow_id++;
  g_live_display_wrapper_borrows.emplace(borrow_id, LiveDisplayWrapperBorrow{stream_id, 0, state});
  return borrow_id;
}

std::shared_ptr<SharedDisplayTextureRidState> make_display_texture_rid_state(
    const godot::RID& rid) {
  auto state = std::make_shared<SharedDisplayTextureRidState>(rid);
  std::lock_guard<std::mutex> lock(g_display_texture_rid_state_registry_mutex);
  const uint64_t registration_id = g_next_display_texture_rid_state_registration_id++;
  state->registration_id = registration_id;
  g_display_texture_rid_state_registry.emplace(registration_id, state);
  return state;
}

std::vector<std::shared_ptr<SharedDisplayTextureRidState>>
snapshot_all_display_texture_rid_states() {
  std::lock_guard<std::mutex> lock(g_display_texture_rid_state_registry_mutex);
  std::vector<std::shared_ptr<SharedDisplayTextureRidState>> states;
  states.reserve(g_display_texture_rid_state_registry.size());
  for (auto it = g_display_texture_rid_state_registry.begin();
       it != g_display_texture_rid_state_registry.end();) {
    std::shared_ptr<SharedDisplayTextureRidState> state = it->second.lock();
    if (!state) {
      it = g_display_texture_rid_state_registry.erase(it);
      continue;
    }
    states.push_back(std::move(state));
    ++it;
  }
  return states;
}

void set_live_display_wrapper_instance_id(uint64_t borrow_id, uint64_t wrapper_instance_id) {
  if (borrow_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_live_display_wrapper_borrow_mutex);
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
  std::vector<LiveDisplayWrapperBorrow> borrows;
  borrows.reserve(g_live_display_wrapper_borrows.size());
  for (const auto& [borrow_id, borrow] : g_live_display_wrapper_borrows) {
    (void)borrow_id;
    borrows.push_back(borrow);
  }
  return borrows;
}

} // namespace

static void unregister_display_texture_rid_state(uint64_t registration_id) {
  if (registration_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_display_texture_rid_state_registry_mutex);
  g_display_texture_rid_state_registry.erase(registration_id);
}

void DeferredDisplayTexture2DRD::init(
    godot::Ref<godot::Texture2DRD> texture,
    std::shared_ptr<SharedDisplayTextureRidState> state,
    uint64_t stream_id,
    uint32_t width,
    uint32_t height) {
  clear_runtime_references_();
  texture_ = std::move(texture);
  state_ = std::move(state);
  stream_id_ = stream_id;
  width_ = width;
  height_ = height;
  borrow_id_ = register_live_display_wrapper_borrow(stream_id_, state_);
  set_live_display_wrapper_instance_id(borrow_id_, static_cast<uint64_t>(get_instance_id()));
  if (stream_id != 0) {
    display_demand_token_.instantiate();
    if (display_demand_token_.is_valid()) {
      display_demand_token_->init(stream_id, true);
    }
  }
}

DeferredDisplayTexture2DRD::~DeferredDisplayTexture2DRD() {
  clear_runtime_references_();
  defer_texture_wrapper_release_();
}

void DeferredDisplayTexture2DRD::prepare_for_bridge_teardown() {
  clear_runtime_references_();
  defer_texture_wrapper_release_();
}

void DeferredDisplayTexture2DRD::defer_texture_wrapper_release_() {
  RenderReleaseProducerLease release_admission;
  godot::Ref<godot::Texture2DRD> texture = std::move(texture_);
  std::shared_ptr<SharedDisplayTextureRidState> state = std::move(state_);
  if (texture.is_valid()) {
    const bool enqueued = release_admission &&
        enqueue_pending_texture_wrapper_release(std::move(texture), std::move(state));
    if (enqueued) {
      request_pending_release_drain();
      return;
    }
    std::fprintf(stderr,
                 "[CamBANG][SyntheticGpu] Texture2DRD release rejected after render bridge closure\n");
    std::fflush(stderr);
  }
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
  return state_ ? state_->snapshot_rid() : godot::RID();
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

void DeferredDisplayTexture2DRD::clear_runtime_references_() {
  unregister_live_display_wrapper_borrow(borrow_id_);
  borrow_id_ = 0;
  display_demand_token_.unref();
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

static void request_pending_release_drain() {
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    return;
  }

  RenderThreadDrainHelper* helper = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_pending_release_mutex);
    if (g_render_release_phase == RenderReleasePhase::Closed ||
        (g_pending_releases.empty() && g_pending_texture_wrapper_releases.empty()) ||
        g_pending_release_drain_scheduled ||
        g_pending_release_drain_running) {
      return;
    }

    helper = g_render_thread_drain_helper;
    if (!helper) {
      return;
    }

    g_pending_release_drain_scheduled = true;
  }

  schedule_render_thread_drain(rs, helper);
}

bool RenderThreadDrainHelper::drain_pending_releases_on_render_thread() {
  std::vector<godot::RID> pending;
  std::vector<PendingTextureWrapperRelease> pending_texture_wrappers;
  {
    std::lock_guard<std::mutex> lock(g_pending_release_mutex);
    if (g_render_release_phase == RenderReleasePhase::Closed) {
      g_pending_release_drain_scheduled = false;
      g_pending_release_changed.notify_all();
      return false;
    }
    g_pending_release_drain_running = true;
    pending.swap(g_pending_releases);
    pending_texture_wrappers.swap(g_pending_texture_wrapper_releases);
    g_pending_release_drain_scheduled = false;
  }

  pending_texture_wrappers.clear();

  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  godot::RenderingDevice* rd = rs ? rs->get_rendering_device() : nullptr;
  if (!pending.empty() && !rd) {
    {
      std::lock_guard<std::mutex> lock(g_pending_release_mutex);
      for (godot::RID &rid : pending) {
        g_pending_releases.push_back(std::move(rid));
      }
      g_pending_release_drain_running = false;
      g_pending_release_changed.notify_all();
    }
    request_pending_release_drain();
    return true;
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
    g_pending_release_drain_running = false;
    g_pending_release_changed.notify_all();
  }

  request_pending_release_drain();
  return true;
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
  retained_backing->rid_state = make_display_texture_rid_state(texture);
  retained_backing->width = width;
  retained_backing->height = height;
  retained_backing->stride_bytes = stride_bytes;
  retained_backing->upload_bytes = bytes;
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
  retained_backing->rid_state = make_display_texture_rid_state(texture);
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

bool activate_render_release_bridge() {
  RenderThreadDrainHelper* helper = memnew(RenderThreadDrainHelper);
  if (!helper) {
    return false;
  }

  bool rejected = false;
  {
    std::lock_guard<std::mutex> lock(g_pending_release_mutex);
    rejected = g_render_release_phase != RenderReleasePhase::Closed ||
        !g_pending_releases.empty() ||
        !g_pending_texture_wrapper_releases.empty() ||
        g_release_producers != 0 ||
        g_pending_release_drain_scheduled ||
        g_pending_release_drain_running ||
        g_render_thread_drain_helper != nullptr;
    if (!rejected) {
      g_render_thread_drain_helper = helper;
      g_render_release_phase = RenderReleasePhase::Active;
    }
  }
  if (rejected) {
    memdelete(helper);
    return false;
  }
  return true;
}

void begin_render_release_bridge_drain() {
  std::lock_guard<std::mutex> lock(g_pending_release_mutex);
  if (g_render_release_phase == RenderReleasePhase::Active) {
    g_render_release_phase = RenderReleasePhase::Draining;
    g_pending_release_changed.notify_all();
  }
}

void wait_for_render_release_bridge_quiescence_and_close() {
  request_pending_release_drain();

  RenderThreadDrainHelper* helper_to_delete = nullptr;
  {
    std::unique_lock<std::mutex> lock(g_pending_release_mutex);
    g_pending_release_changed.wait(lock, [] {
      return g_render_release_phase != RenderReleasePhase::Draining ||
          (g_pending_releases.empty() &&
           g_pending_texture_wrapper_releases.empty() &&
           g_release_producers == 0 &&
           !g_pending_release_drain_scheduled &&
           !g_pending_release_drain_running);
    });
    if (g_render_release_phase != RenderReleasePhase::Draining) {
      return;
    }
    g_render_release_phase = RenderReleasePhase::Closed;
    helper_to_delete = g_render_thread_drain_helper;
    g_render_thread_drain_helper = nullptr;
    g_pending_release_changed.notify_all();
  }

  // The bridge is the helper's sole owner. Scheduled/running callback counts
  // are zero, so deleting it cannot leave a callable that may still invoke it.
  if (helper_to_delete) {
    memdelete(helper_to_delete);
  }
}

} // namespace

void install_synthetic_gpu_backing_godot_bridge() {
  if (!activate_render_release_bridge()) {
    godot::UtilityFunctions::push_error(
        "[CamBANG][SyntheticGpu] bridge install rejected: render release protocol was not closed and empty");
    return;
  }
  set_synthetic_gpu_backing_runtime_ops(&kOps);
  trace_gpu("bridge_install runtime_ops_registered=true");
}

void uninstall_synthetic_gpu_backing_godot_bridge() {
  clear_synthetic_gpu_backing_runtime_ops();
  begin_render_release_bridge_drain();
  godot_gpu_display_invalidate_all();
  wait_for_render_release_bridge_quiescence_and_close();
  trace_gpu("bridge_uninstall runtime_ops_registered=false");
}

void synthetic_gpu_backing_warn_and_abandon_live_display_wrappers_before_stop() {
  const std::vector<LiveDisplayWrapperBorrow> borrows = snapshot_live_display_wrapper_borrows();
  if (borrows.empty()) {
    return;
  }

  std::map<uint64_t, uint64_t> counts_by_stream_id;
  for (const LiveDisplayWrapperBorrow& borrow : borrows) {
    if (borrow.wrapper_instance_id != 0) {
      godot::Object* object = godot::ObjectDB::get_instance(borrow.wrapper_instance_id);
      DeferredDisplayTexture2DRD* wrapper =
          godot::Object::cast_to<DeferredDisplayTexture2DRD>(object);
      if (wrapper) {
        // Move the Texture2DRD delegate into the owned render-release queue
        // while the public wrapper is still addressable. A wrapper retained
        // past stop is then inert and its later destruction owns no
        // thread-affine Godot delegate.
        wrapper->prepare_for_bridge_teardown();
      }
    }
    if (borrow.rid_state) {
      // Release now rather than merely marking invalidated: RenderingDevice
      // persists across CamBANGServer stop()/start() cycles within one
      // process, so it is safe and correct to reclaim the RD-backed texture
      // here rather than leaving it allocated until whatever Texture2DRD
      // wrapper reference the caller still holds eventually gets dropped.
      borrow.rid_state->release_now();
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
  // Called only when stream_id has dropped out of the latest snapshot
  // entirely (see CamBANGServer's snapshot-diff caller) -- i.e. permanent
  // stream retirement, not a reversible pause -- so releasing now is correct
  // for the same reason as the stop()-time abandon above.
  const std::vector<LiveDisplayWrapperBorrow> borrows = snapshot_live_display_wrapper_borrows();
  for (const LiveDisplayWrapperBorrow& borrow : borrows) {
    if (borrow.stream_id != stream_id || !borrow.rid_state) {
      continue;
    }
    borrow.rid_state->release_now();
  }
}

void synthetic_gpu_backing_invalidate_all_live_display_wrappers() {
  // Called only from uninstall_synthetic_gpu_backing_godot_bridge() after the
  // runtime-ops seam is drained and render-release state has entered Draining.
  // Detach every Texture2DRD delegate while its wrapper is still addressable;
  // later wrapper destruction then owns no thread-affine Godot delegate.
  const std::vector<LiveDisplayWrapperBorrow> borrows = snapshot_live_display_wrapper_borrows();
  for (const LiveDisplayWrapperBorrow& borrow : borrows) {
    if (borrow.wrapper_instance_id == 0) {
      continue;
    }
    godot::Object* object = godot::ObjectDB::get_instance(borrow.wrapper_instance_id);
    DeferredDisplayTexture2DRD* wrapper =
        godot::Object::cast_to<DeferredDisplayTexture2DRD>(object);
    if (wrapper) {
      wrapper->prepare_for_bridge_teardown();
    }
  }

  // Wrapper borrows are not a complete RID-state registry: retained capture
  // or stream backings may never have produced a display view. Invalidate the
  // complete weak registry so every live RD texture is admitted to this final
  // render-thread drain before release admission closes.
  const std::vector<std::shared_ptr<SharedDisplayTextureRidState>> states =
      snapshot_all_display_texture_rid_states();
  for (const std::shared_ptr<SharedDisplayTextureRidState>& state : states) {
    state->release_now();
  }
}

void synthetic_gpu_backing_drain_render_releases_before_stop() {
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  for (;;) {
    request_pending_release_drain();
    {
      std::lock_guard<std::mutex> lock(g_pending_release_mutex);
      if (g_render_release_phase != RenderReleasePhase::Active ||
          (g_pending_releases.empty() &&
           g_pending_texture_wrapper_releases.empty() &&
           g_release_producers == 0 &&
           !g_pending_release_drain_scheduled &&
           !g_pending_release_drain_running)) {
        return;
      }
    }
    if (!rs) {
      godot::UtilityFunctions::push_error(
          "[CamBANG][SyntheticGpu] cannot drain accepted render releases: RenderingServer unavailable");
      return;
    }
    // Godot's RenderingServerDefault::sync() puts a synchronous command
    // behind prior render commands (or flushes all commands in single-thread
    // mode). No CamBANG lock is held while that engine fence runs.
    rs->force_sync();
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
  display_view->init(texture, state, stream_id, width, height);

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

godot::Ref<godot::Image> synthetic_gpu_backing_materialize_to_image(const std::shared_ptr<void>& backing) {
  return materialize_to_image(backing);
}

} // namespace cambang

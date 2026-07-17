#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2.hpp>

namespace cambang {

struct SharedLiveCpuTextureRidState final {
  mutable std::mutex mutex;
  godot::RID texture_rid;
  bool invalidated = false;

  godot::RID snapshot_rid() const;
  void replace_rid(const godot::RID& texture_rid_in);
  void clear();
  // Marks this display view as torn-down truth (e.g. server stop) without
  // releasing texture_rid itself; draw_allowed() reflects this immediately
  // for any Godot object still holding this shared state. Mirrors
  // SharedDisplayTextureRidState::mark_invalidated() in
  // synthetic_gpu_backing_bridge.cpp.
  void mark_invalidated();
  // Marks this display view invalidated AND releases texture_rid now, rather
  // than deferring release to whenever this object's own destructor happens
  // to run (which depends on when the last shared_ptr/script reference to the
  // owning wrapper is dropped -- not deterministic, and not tied to CamBANG's
  // own lifecycle). RenderingServer persists across CamBANGServer stop()/
  // start() cycles within one process, so it is safe and correct to reclaim
  // texture_rid here rather than leaving it allocated indefinitely. Called
  // from abandon_all_live_cpu_display_wrappers_before_stop(); unlike the GPU
  // path's equivalent stop-time abandon (which only marks invalidated), this
  // one also actively frees the RenderingServer resource.
  void invalidate_and_release();
  bool draw_allowed() const;
  ~SharedLiveCpuTextureRidState();
};

class DisplayDemandToken : public godot::RefCounted {
  GDCLASS(DisplayDemandToken, godot::RefCounted);

public:
  DisplayDemandToken() = default;
  ~DisplayDemandToken() override;

  void init(uint64_t stream_id, bool gpu_display_view);

private:
  static void _bind_methods() {}
  uint64_t stream_id_ = 0;
  bool gpu_display_view_ = false;
};

class LiveCpuDisplayTexture2D : public godot::Texture2D {
  GDCLASS(LiveCpuDisplayTexture2D, godot::Texture2D);

public:
  LiveCpuDisplayTexture2D() = default;
  ~LiveCpuDisplayTexture2D() override;

  void init(
      std::shared_ptr<SharedLiveCpuTextureRidState> state,
      uint64_t stream_id,
      uint32_t width,
      uint32_t height,
      bool retain_display_demand);
  void update_dimensions(uint32_t width, uint32_t height);
  // Marks the underlying shared display state invalidated. Called via
  // abandon_all_live_cpu_display_wrappers_before_stop() so outstanding
  // wrappers stop drawing torn-down-generation content truthfully.
  void invalidate();

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
  godot::RID _get_rid() const override;

private:
  static void _bind_methods() {}
  void clear_runtime_references_();

  std::shared_ptr<SharedLiveCpuTextureRidState> state_;
  godot::Ref<DisplayDemandToken> display_demand_token_;
  uint64_t stream_id_ = 0;
  uint64_t borrow_id_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

// RefCounted target for RenderingServer::call_on_render_thread(), registered
// only so ClassDB can resolve the bound method as a Callable target -- not a
// user-facing CamBANG class. Mirrors RenderThreadDrainHelper in
// synthetic_gpu_backing_bridge_internal.h, but for RenderingServer texture
// creation rather than RenderingDevice release.
class LiveCpuTextureCreateDrainHelper : public godot::RefCounted {
  GDCLASS(LiveCpuTextureCreateDrainHelper, godot::RefCounted);

public:
  bool drain_pending_creates_on_render_thread();

private:
  static void _bind_methods() {
    godot::ClassDB::bind_method(
        godot::D_METHOD("drain_pending_creates_on_render_thread"),
        &LiveCpuTextureCreateDrainHelper::drain_pending_creates_on_render_thread);
  }
};

// Godot's RenderingServer::texture_2d_create() bypasses the normal async
// command queue and executes directly on whatever thread calls it -- unlike
// free_rid()/texture_2d_update(), which are queued and safe to call from any
// thread (see https://docs.godotengine.org/en/stable/tutorials/performance/thread_safe_apis.html).
// get_display_view() is reachable from any GDScript thread (including a
// script's own background Thread), so texture creation for the CPU-backed
// live display path must be deferred to the render thread via
// RenderingServer::call_on_render_thread(), mirroring the drain pattern
// synthetic_gpu_backing_bridge.cpp already uses for RenderingDevice release
// -- here for RenderingServer creation instead.
//
// Enqueuing is keyed by rid_state's identity: a stream refreshing faster
// than the render thread drains only ever creates from the latest queued
// image, never a stale intermediate one, matching this system's general
// latest-state-wins convention rather than piling up wasted RID churn.
void enqueue_live_cpu_texture_create(
    std::shared_ptr<SharedLiveCpuTextureRidState> rid_state,
    godot::Ref<godot::Image> image);

// Lifecycle hooks: call from the same GDExtension init/deinit points as
// install_synthetic_gpu_backing_godot_bridge()/uninstall_...(). Mirrors that
// bridge's teardown-abandon guard (ledger #44) so a render-thread callback
// scheduled here can never fire back into torn-down extension state.
void install_live_cpu_display_bridge();
void uninstall_live_cpu_display_bridge();

uint64_t register_live_cpu_display_wrapper_borrow(uint64_t stream_id);
void unregister_live_cpu_display_wrapper_borrow(uint64_t borrow_id);
bool has_live_cpu_display_wrapper_borrow(uint64_t stream_id);
void notify_live_cpu_display_wrapper_refresh(uint64_t stream_id, uint32_t width, uint32_t height);

// Invalidates every outstanding LiveCpuDisplayTexture2D wrapper so a script
// still holding one across a CamBANGServer stop/restart draws nothing rather
// than a silently stale pre-stop frame. Mirrors
// synthetic_gpu_backing_warn_and_abandon_live_display_wrappers_before_stop()
// in synthetic_gpu_backing_bridge.cpp; call from the same stop sequence.
void abandon_all_live_cpu_display_wrappers_before_stop();

// Internal-only helper class registration required for Ref<DisplayDemandToken>::instantiate().
void register_stream_result_internal_classes();

} // namespace cambang

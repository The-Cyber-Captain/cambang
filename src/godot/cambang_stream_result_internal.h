#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/rid.hpp>

#include "godot/render_resource_release_service.h"
#include <godot_cpp/variant/vector2.hpp>

namespace cambang {

class CamBANGServer;

struct SharedLiveCpuTextureRidState final {
  mutable std::mutex mutex;
  godot::RID texture_rid;
  RenderResourceReleaseReservation release_reservation;

  godot::RID snapshot_rid() const;
  bool replace_rid(
      const godot::RID& texture_rid_in,
      RenderResourceReleaseReservation& reservation) noexcept;
  void clear();
  ~SharedLiveCpuTextureRidState();
};

// Thread-neutral lifetime record for display demand. Unlike a Godot Object,
// it is never a Godot Ref and may therefore reach final ownership off-thread.
class DisplayDemandLease final {
public:
  explicit DisplayDemandLease(uint64_t lease_token) noexcept
      : lease_token_(lease_token) {}
  ~DisplayDemandLease() noexcept;

private:
  uint64_t lease_token_ = 0;
};

std::shared_ptr<DisplayDemandLease> retain_display_demand_lease(uint64_t stream_id);
void install_display_demand_release_dispatcher(CamBANGServer* server) noexcept;
void uninstall_display_demand_release_dispatcher(CamBANGServer* server) noexcept;

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
  std::shared_ptr<DisplayDemandLease> display_demand_lease_;
  uint64_t stream_id_ = 0;
  uint64_t borrow_id_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

uint64_t register_live_cpu_display_wrapper_borrow(uint64_t stream_id);
void unregister_live_cpu_display_wrapper_borrow(uint64_t borrow_id);
bool has_live_cpu_display_wrapper_borrow(uint64_t stream_id);
void notify_live_cpu_display_wrapper_refresh(uint64_t stream_id, uint32_t width, uint32_t height);

// Registers the live CPU display wrapper used by the internal adapter.
void register_stream_result_internal_classes();

#if defined(CAMBANG_INTERNAL_SMOKE)
bool exercise_live_cpu_post_transfer_failure_for_smoke() noexcept;
#endif

} // namespace cambang

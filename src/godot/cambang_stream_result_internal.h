#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2.hpp>

namespace cambang {

struct SharedLiveCpuTextureRidState final {
  mutable std::mutex mutex;
  godot::RID texture_rid;

  godot::RID snapshot_rid() const;
  void replace_rid(const godot::RID& texture_rid_in);
  void clear();
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

uint64_t register_live_cpu_display_wrapper_borrow(uint64_t stream_id);
void unregister_live_cpu_display_wrapper_borrow(uint64_t borrow_id);
bool has_live_cpu_display_wrapper_borrow(uint64_t stream_id);
void notify_live_cpu_display_wrapper_refresh(uint64_t stream_id, uint32_t width, uint32_t height);

// Internal-only helper class registration required for Ref<DisplayDemandToken>::instantiate().
void register_stream_result_internal_classes();

} // namespace cambang

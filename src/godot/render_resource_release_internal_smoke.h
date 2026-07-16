#pragma once

#if defined(CAMBANG_INTERNAL_SMOKE)

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace cambang {

// Present only in explicitly internal GDE smoke binaries. It lets the harness
// make a real display wrapper's final Ref drop on a native worker thread.
class CamBANGRenderReleaseInternalSmoke : public godot::RefCounted {
  GDCLASS(CamBANGRenderReleaseInternalSmoke, godot::RefCounted);

public:
  bool retain_texture(const godot::Ref<godot::Texture2D>& texture) noexcept;
  bool release_retained_from_worker() noexcept;
  bool retained_texture_is_invalidated() const noexcept;
  void reset_release_stats() noexcept;
  godot::Dictionary get_release_stats() const;
  bool force_runtime_full_probe_from_worker() noexcept;
  bool force_allocation_failure_probe_from_worker() noexcept;
  bool force_schedule_failure_probe_from_worker() noexcept;
  bool exercise_primary_gpu_post_transfer_failure() noexcept;
  bool exercise_gpu_wrapper_post_transfer_failure() noexcept;
  bool exercise_cpu_post_transfer_failure() noexcept;
  bool recover_release_service() noexcept;
  void enter_terminal_release_state() noexcept;
  bool restart_release_service_after_clean_terminal() noexcept;
  bool exercise_terminal_cutoff_race() noexcept;
  uint32_t get_persistent_display_demand_refcount(uint64_t stream_id) const noexcept;

  static void _bind_methods();

private:
  godot::Ref<godot::Texture2D> retained_;
};

} // namespace cambang

#endif

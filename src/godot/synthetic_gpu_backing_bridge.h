#pragma once

#include <memory>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>

namespace cambang {

void install_synthetic_gpu_backing_godot_bridge();
void uninstall_synthetic_gpu_backing_godot_bridge();

godot::Ref<godot::Texture2D> synthetic_gpu_backing_display_texture(const std::shared_ptr<void>& backing);
void synthetic_gpu_backing_invalidate_live_display_wrappers_for_stream(uint64_t stream_id);
void synthetic_gpu_backing_invalidate_all_live_display_wrappers();
void synthetic_gpu_backing_warn_and_abandon_live_display_wrappers_before_stop();
void synthetic_gpu_backing_drain_render_releases_before_stop();
void synthetic_gpu_backing_drain_pending_live_display_wrapper_refreshes();
godot::Ref<godot::Image> synthetic_gpu_backing_materialize_to_image(const std::shared_ptr<void>& backing);

} // namespace cambang

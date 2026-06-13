#pragma once

#include <memory>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>

namespace cambang {

void install_synthetic_gpu_backing_godot_bridge();
void uninstall_synthetic_gpu_backing_godot_bridge();

godot::Ref<godot::Texture2D> synthetic_gpu_backing_display_texture(const std::shared_ptr<void>& backing);
void synthetic_gpu_backing_warn_and_abandon_live_display_wrappers_before_stop();
bool synthetic_gpu_backing_can_materialize_to_image(const std::shared_ptr<void>& backing) noexcept;
godot::Ref<godot::Image> synthetic_gpu_backing_materialize_to_image(const std::shared_ptr<void>& backing);

} // namespace cambang

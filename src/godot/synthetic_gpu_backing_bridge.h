#pragma once

#include <memory>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>

namespace cambang {

void install_synthetic_gpu_backing_godot_bridge();
void uninstall_synthetic_gpu_backing_godot_bridge();

godot::Ref<godot::Texture2D> synthetic_gpu_backing_display_texture(const std::shared_ptr<void>& backing);
bool synthetic_gpu_backing_can_materialize_to_image(const std::shared_ptr<void>& backing);
godot::Ref<godot::Image> synthetic_gpu_backing_materialize_to_image(const std::shared_ptr<void>& backing);

} // namespace cambang

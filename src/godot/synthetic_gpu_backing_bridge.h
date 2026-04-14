#pragma once

#include <memory>

#include <godot_cpp/classes/texture2d.hpp>

namespace cambang {

void install_synthetic_gpu_backing_godot_bridge();
void uninstall_synthetic_gpu_backing_godot_bridge();

godot::Ref<godot::Texture2D> synthetic_gpu_backing_display_texture(const std::shared_ptr<void>& backing);

} // namespace cambang

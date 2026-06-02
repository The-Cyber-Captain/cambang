#pragma once

#include <memory>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>

namespace cambang {

void install_synthetic_gpu_backing_godot_bridge();
void uninstall_synthetic_gpu_backing_godot_bridge();
// Drains Godot-facing release payloads previously detached by provider/core
// teardown. This is a Godot-boundary maintenance hook only; it is not a
// display cache/registry and is not used for get_display_view() lookup.
void synthetic_gpu_backing_drain_pending_godot_releases();

godot::Ref<godot::Texture2D> synthetic_gpu_backing_display_texture(const std::shared_ptr<void>& backing);
bool synthetic_gpu_backing_can_materialize_to_image(const std::shared_ptr<void>& backing);
godot::Ref<godot::Image> synthetic_gpu_backing_materialize_to_image(const std::shared_ptr<void>& backing);

} // namespace cambang

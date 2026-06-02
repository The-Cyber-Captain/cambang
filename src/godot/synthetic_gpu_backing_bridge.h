#pragma once

#include <memory>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

void install_synthetic_gpu_backing_godot_bridge();
void uninstall_synthetic_gpu_backing_godot_bridge();

bool synthetic_gpu_backing_display_texture_available(const RetainedGpuBackingDescriptor& descriptor);
godot::Ref<godot::Texture2D> synthetic_gpu_backing_display_texture(const RetainedGpuBackingDescriptor& descriptor);
void synthetic_gpu_backing_invalidate_stream_godot_display(uint64_t stream_id);
void synthetic_gpu_backing_invalidate_all_godot_display();
bool synthetic_gpu_backing_can_materialize_to_image(const std::shared_ptr<void>& backing);
godot::Ref<godot::Image> synthetic_gpu_backing_materialize_to_image(const std::shared_ptr<void>& backing);

} // namespace cambang

#pragma once

#include <cstdint>
#include <memory>

#include <godot_cpp/classes/texture2d.hpp>

#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

// Godot-side display-boundary scaffold for descriptor-keyed GPU display views.
// This service owns Godot display wrappers/metadata. Core/provider/result-store
// code should retain only neutral RetainedGpuBackingDescriptor metadata and any
// temporary legacy compatibility artifacts until a later tranche completes the
// ownership split.
bool godot_gpu_display_descriptor_has_complete_identity(
    const RetainedGpuBackingDescriptor& descriptor) noexcept;

godot::Ref<godot::Texture2D> godot_gpu_display_lookup_texture_by_descriptor(
    const RetainedGpuBackingDescriptor& descriptor);

godot::Ref<godot::Texture2D> godot_gpu_display_get_texture_by_descriptor(
    const RetainedGpuBackingDescriptor& descriptor,
    const std::shared_ptr<void>& legacy_retained_gpu_backing);

void godot_gpu_display_invalidate_descriptor(const RetainedGpuBackingDescriptor& descriptor);
void godot_gpu_display_invalidate_stream(uint64_t stream_id);
void godot_gpu_display_invalidate_all();

} // namespace cambang

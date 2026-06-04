#pragma once

#include <cstdint>
#include <memory>

#include <godot_cpp/classes/texture2d.hpp>

#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

// Internal Godot-side display adapter resolver/factory for retained GPU stream
// views. The service is deliberately non-owning: it does not cache or retain
// Texture2D refs, Godot RIDs, or backend-native handles. Current synthetic GPU
// display resolution still uses the legacy retained backing artifact as the
// compatibility behavior carrier; RetainedGpuBackingDescriptor is the
// provider-neutral scalar metadata seam for future descriptor/platform-backed
// activation. A descriptor with backing_id == 0 must not be treated as valid
// descriptor-cache identity. Core/providers/public results must not own Godot
// display adapters or expose Texture2D/RID/backend-native handles.
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

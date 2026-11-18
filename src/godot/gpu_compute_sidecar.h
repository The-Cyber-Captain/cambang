#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/rid.hpp>

#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

// Godot-layer realisation of a still image member's GPU compute sidecar
// (FrameView::gpu_compute_sidecar, CoreCaptureResultData::ImageMemberData::
// gpu_compute_sidecar).
//
// Every sidecar Core can hold is created by one of the two functions below, so
// the opaque handle Core carries always points at this layer's own record.
// Nothing outside the Godot layer interprets it.
//
// Each plane is one RenderingDevice texture. Its RID is released through the
// render-thread release drain shared with the synthetic GPU backing bridge,
// never from an arbitrary thread, and only once neither Core's retained member
// nor any texture handed to a caller still refers to it.

struct GpuComputeSidecarPlaneRid {
  godot::RID rd_texture;
  GpuBackingPlaneDescriptor shape{};
};

// Takes ownership of already-written RD textures. `after_plane_release`, when
// given, runs once per plane after that plane's RID has been handed to the
// render-thread release drain; a producer whose texture wraps memory it owns
// uses it to schedule that memory's destruction. Returns null, releasing the
// RIDs, when the render-release bridge is not active.
std::shared_ptr<void> gpu_compute_sidecar_adopt_rd_textures(
    const GpuComputeSidecarPlaneRid* planes,
    uint32_t plane_count,
    std::function<void()> after_plane_release);

// Whether a sidecar can still hand out textures: not released, and the render
// bridge still active.
bool gpu_compute_sidecar_is_usable(const std::shared_ptr<void>& sidecar);

// A caller-facing texture for one plane. The texture keeps that plane's RD
// texture alive on its own, so it outlives Core's eviction of the member.
// Main thread only.
godot::Ref<godot::Texture2D> gpu_compute_sidecar_plane_texture(
    const std::shared_ptr<void>& sidecar,
    uint32_t plane_index);

} // namespace cambang

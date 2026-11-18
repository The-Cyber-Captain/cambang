#pragma once

#include <cstdint>

#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include "core/core_result_store.h"
#include "core/result_capability.h"

namespace cambang {

// Capture Compute Texture -- pixel_payload_and_result_contract.md 11.6.1.
//
// GPU-resident, frozen textures of one capture image member, for running
// compute over the captured image. This is NOT a display view: no freshness
// policy, no demand semantics, no live-view contract. The stream display
// machinery is a different concept and none of its reasoning applies.
//
// Planes are exposed in the member's **native** format and are never converted.
// A packed member has one plane; NV12/NV21 have two (luma, interleaved chroma);
// I420/YV12 have three. Converting to RGBA would cost far more than the upload
// it accompanies -- measured at roughly 20 ms against 620 us for a 1280x720
// NV12 member -- and would discard the representation the caller asked for. A
// caller that wants RGB does the conversion in its own shader, where it also
// controls the colour handling.
//
// Everything here runs on the Godot main thread: it touches RenderingServer,
// and the cache is not designed for concurrent access from provider or core
// threads.

// Operation Support for this member, per 11.6.1's table. Never claims CHEAP:
// producing a plane is still a full-frame copy, and no path may be declared
// CHEAP without measured evidence.
ResultCapability capture_compute_texture_support(
    const SharedCaptureResultData& data,
    uint32_t image_member_index);

// Number of planes this member's compute textures are exposed as. Zero when the
// member is unsupported or absent.
uint32_t capture_compute_texture_plane_count(
    const SharedCaptureResultData& data,
    uint32_t image_member_index);

// One plane, produced on first request and cached; a repeat request for the
// same retained plane does not upload again (11.6.1, "Identity, immutability,
// and caching"). Null when unsupported, out of range, or production fails.
//
// The returned Ref keeps the texture alive on its own, so a cache eviction never
// invalidates a texture a caller is still holding -- it only means the next
// request pays again.
godot::Ref<godot::Texture2D> capture_compute_texture_plane(
    const SharedCaptureResultData& data,
    uint32_t image_member_index,
    uint32_t plane_index);

// Which route serves this member's compute textures -- the D6 backing query.
// Values match CamBANGCaptureResult.COMPUTE_TEXTURE_BACKING_*.
//
//   NONE          no compute texture: member absent, or no RenderingDevice
//                 (Compatibility renderer)
//   CPU_UPLOAD    planes uploaded from the retained CPU payload on request
//   GPU_COPY      reserved for a route that copies on the GPU on request; no
//                 path produces it today
//   GPU_RETAINED  planes already GPU-resident: a frozen GPU copy made at
//                 acquisition, or a retained GPU-primary backing
//
// It names the route only. Capability (READY/EXPENSIVE) stays the cost answer.
enum class ComputeTextureBacking : int {
  NONE = 0,
  CPU_UPLOAD = 1,
  GPU_COPY = 2,
  GPU_RETAINED = 3,
};

ComputeTextureBacking capture_compute_texture_backing(
    const SharedCaptureResultData& data,
    uint32_t image_member_index);

// True the first time it is asked for a given device since the last cache
// clear. The caller uses it to raise the device's GPU-copy demand once rather
// than on every plane request.
bool capture_compute_texture_take_first_device_request(uint64_t device_instance_id);
void capture_compute_texture_forget_device_request(uint64_t device_instance_id);

// Dropped on stop/restart, like the stream display caches, so no texture
// outlives the runtime that produced its pixels.
void clear_capture_compute_texture_cache();

// Diagnostic: { uploads, gpu_wraps, gpu_sidecar_planes, hits, entries, uploaded_bytes }.
godot::Dictionary capture_compute_texture_metrics();

} // namespace cambang

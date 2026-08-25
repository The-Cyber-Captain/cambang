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

// Dropped on stop/restart, like the stream display caches, so no texture
// outlives the runtime that produced its pixels.
void clear_capture_compute_texture_cache();

// Diagnostic: { uploads, gpu_wraps, hits, entries, uploaded_bytes }.
godot::Dictionary capture_compute_texture_metrics();

} // namespace cambang

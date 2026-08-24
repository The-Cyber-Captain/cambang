#pragma once

#include <cstdint>
#include <functional>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include "core/core_result_store.h"
#include "core/result_capability.h"

namespace cambang {

// Capture Compute Texture -- pixel_payload_and_result_contract.md 11.6.1.
//
// A GPU-resident, frozen texture of one capture image member, for running
// compute over the captured image. This is NOT a display view: it has no
// freshness policy, no demand semantics, and no live-view contract. The stream
// display machinery is a different concept and none of its reasoning applies
// here.
//
// The texture is read-oriented. A compute shader samples it; anything the
// caller wants to write goes to storage the caller owns.
//
// Everything here runs on the Godot main thread: it touches RenderingServer,
// and the cache is not designed for concurrent access from provider or core
// threads.

// Operation Support for this member, per 11.6.1's table. Never claims CHEAP:
// producing from CPU bytes is a full-frame upload, and no path may be declared
// CHEAP without measured evidence.
ResultCapability capture_compute_texture_support(
    const SharedCaptureResultData& data,
    uint32_t image_member_index);

// Produces on first request and caches; a repeat request for the same retained
// member does not upload again. Returns null when support is UNSUPPORTED or
// production fails.
//
// The returned Ref keeps the texture alive on its own, so a cache eviction
// never invalidates a texture a caller is still holding -- it only means the
// next request pays again.
// `cpu_image_supplier` is invoked only on a cache miss that cannot be served
// from an already-GPU-resident backing -- i.e. only when a full-frame upload is
// actually going to happen. It is a callback so the caller supplies the image
// through the ordinary application materialization path, and so a cache hit
// never materializes anything.
godot::Ref<godot::Texture2D> capture_compute_texture_for_member(
    const SharedCaptureResultData& data,
    uint32_t image_member_index,
    const std::function<godot::Ref<godot::Image>()>& cpu_image_supplier);

// Dropped on stop/restart, like the stream display caches, so no texture
// outlives the runtime that produced its pixels.
void clear_capture_compute_texture_cache();

// Diagnostic: { uploads, gpu_wraps, hits, entries }. `uploads` counts
// full-frame CPU uploads actually performed, which is what a caller checking
// that repeat access is free should assert on.
godot::Dictionary capture_compute_texture_metrics();

} // namespace cambang

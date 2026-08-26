#pragma once

#include <cstdint>

#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include "core/core_result_store.h"
#include "core/result_capability.h"

namespace cambang {

// Stream Compute Texture -- the stream counterpart of Capture Compute Texture
// (pixel_payload_and_result_contract.md 11.6.1).
//
// GPU-resident, FROZEN textures of one retained stream frame, exposed in the
// frame's native planes so a caller can run its own compute over live camera
// output. This is an ADDITIONAL surface, not a replacement for
// get_display_view(): the two answer different needs and coexist.
//
// The distinction that matters most is freshness. The live CPU display view is
// deliberately an alias -- one texture per stream_id, refreshed in place, whose
// contents change under the caller as frames land. A compute texture is the
// opposite: it belongs to one retained frame and never changes. A caller
// pairing pixels with that frame's acquisition mark depends on exactly that,
// and the display view cannot provide it.
//
// Planes are exposed in the frame's native format and are never converted, for
// the reason measured in 4d12d67: for a planar frame the CPU conversion is
// ~97% of the cost of producing a texture and the upload is ~3%. A caller that
// wants RGB does the conversion in its own shader, where it also controls
// colour handling -- which is why the colorimetry the provider declared is
// reported alongside.
//
// Everything here runs on the Godot main thread: it touches RenderingServer,
// and the cache is not designed for concurrent access from provider or core
// threads.

// Operation Support for this retained frame. Never claims CHEAP: producing a
// plane is still a full-frame copy, and no path may be declared CHEAP without
// measured evidence. Never claims READY either, unlike the capture surface --
// see the implementation for why a stream's GPU backing cannot be wrapped
// without breaking the freeze this surface exists to provide.
ResultCapability stream_compute_texture_support(const SharedStreamResultData& data);

// Number of planes this frame's compute textures are exposed as. Zero when
// unsupported.
uint32_t stream_compute_texture_plane_count(const SharedStreamResultData& data);

// One plane, produced on first request and cached against the retained frame it
// came from. Null when unsupported, out of range, or production fails.
//
// The cache keys on retained_frame_id, not merely on stream_id: a stream
// replaces its retained frame constantly, and a key that ignored the frame
// would hand back a texture of a previous one -- silently, and with the
// caller's acquisition mark saying otherwise.
//
// The returned Ref keeps the texture alive on its own, so an eviction never
// invalidates a texture a caller still holds; it only means the next request
// pays again.
godot::Ref<godot::Texture2D> stream_compute_texture_plane(
    const SharedStreamResultData& data,
    uint32_t plane_index);

// Drops just this stream's entries. Called when a stream is destroyed, where
// the other per-stream Godot-side caches are dropped.
void remove_stream_compute_textures(uint64_t stream_id);

// Dropped on stop/restart, like the other Godot-side caches, so no texture
// outlives the runtime that produced its pixels.
void clear_stream_compute_texture_cache();

// Diagnostic: { uploads, hits, entries, uploaded_bytes }. No gpu_wraps row:
// this surface has no GPU-wrap path, for the reason given on support() above.
godot::Dictionary stream_compute_texture_metrics();

} // namespace cambang

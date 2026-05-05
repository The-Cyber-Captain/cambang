#pragma once

namespace cambang {

// Internal-only: registers bridge helper classes required for Godot ClassDB/
// RefCounted instantiation (e.g., RenderThreadDrainHelper). Not public API.
void register_synthetic_gpu_backing_internal_classes();

} // namespace cambang

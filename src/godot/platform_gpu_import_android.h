#pragma once

// Android implementation of the neutral platform GPU import seam
// (imaging/api/platform_gpu_import_ops.h): frozen R8 + RG8 GPU copies of a
// Camera2 still member's AHardwareBuffer, produced on a second VkDevice that
// CamBANG owns on Godot's VkInstance / VkPhysicalDevice and handed to Godot's
// RenderingDevice as RD-owned textures.
//
// Scope gate: the table answers can_copy_native_buffers() true only on a Meta
// Quest headset with OpenXR as the active, initialized XR interface, and only
// once the second device is ready. Everything else stays on CPU_UPLOAD.
//
// The implementation is compiled only for Android; callers guard their calls
// with __ANDROID__.

namespace cambang {

// Scene-level registration of the internal render-thread callback helper.
void register_platform_gpu_import_android_internal_classes();

// Installs the seam table. Call after the synthetic GPU backing bridge is
// installed: sidecars are realised through that bridge's render-release drain.
void install_platform_gpu_import_android();

// Closes admission, drains every job (all sources released, all GPU work
// fenced), then destroys the second device. Call before the synthetic GPU
// backing bridge is uninstalled.
void uninstall_platform_gpu_import_android();

// Main thread, from CamBANGServer's start and its per-frame tick: refreshes the
// XR part of the scope gate and, while the gate holds, creates the second
// device once (on the render thread).
void platform_gpu_import_android_main_thread_tick();

} // namespace cambang

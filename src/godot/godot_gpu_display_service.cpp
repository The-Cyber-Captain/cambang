#include "godot/godot_gpu_display_service.h"

#include "godot/synthetic_gpu_backing_bridge.h"
#include "imaging/synthetic/gpu_backing_runtime.h"

namespace cambang {

bool godot_gpu_display_descriptor_has_complete_identity(
    const RetainedGpuBackingDescriptor& descriptor) noexcept {
  // backing_id == 0 is compatibility metadata only. It means a GPU backing may
  // exist, but provider/core did not supply a scalar identity/generation. Never
  // use such a descriptor as display-cache identity, materialization identity,
  // or stale-generation identity.
  return descriptor.valid &&
         descriptor.display_available &&
         descriptor.stream_id != 0 &&
         descriptor.backing_id != 0 &&
         descriptor.width != 0 &&
         descriptor.height != 0;
}

godot::Ref<godot::Texture2D> godot_gpu_display_lookup_texture_by_descriptor(
    const RetainedGpuBackingDescriptor& descriptor) {
  (void)godot_gpu_display_descriptor_has_complete_identity(descriptor);
  // Descriptor-only display resolution is a future activation point. This
  // service intentionally has no descriptor-keyed cache in the current slice.
  return {};
}

godot::Ref<godot::Texture2D> godot_gpu_display_get_texture_by_descriptor(
    const RetainedGpuBackingDescriptor& descriptor,
    const std::shared_ptr<void>& legacy_retained_gpu_backing) {
  (void)godot_gpu_display_descriptor_has_complete_identity(descriptor);
  // The completeness helper is for future descriptor-native/provider-backed
  // lookup and must not block the current synthetic compatibility path. When a
  // legacy retained backing exists, delegate without storing the returned
  // Texture2D so display-view ownership and lifetime diagnostics remain visible
  // in the synthetic bridge. Descriptor-only lookup remains no-op/null for now.
  if (legacy_retained_gpu_backing) {
    return synthetic_gpu_backing_display_texture(legacy_retained_gpu_backing);
  }
  return godot_gpu_display_lookup_texture_by_descriptor(descriptor);
}

bool godot_gpu_display_can_materialize_to_image(
    const RetainedGpuBackingDescriptor& descriptor,
    const std::shared_ptr<void>& legacy_retained_gpu_backing) {
  if (!descriptor.valid || !descriptor.materialization_available) {
    return false;
  }
  if (legacy_retained_gpu_backing) {
    return synthetic_gpu_backing_can_materialize_to_image(legacy_retained_gpu_backing);
  }
  return false;
}

godot::Ref<godot::Image> godot_gpu_display_materialize_to_image(
    const RetainedGpuBackingDescriptor& descriptor,
    const std::shared_ptr<void>& legacy_retained_gpu_backing) {
  if (!godot_gpu_display_can_materialize_to_image(descriptor, legacy_retained_gpu_backing)) {
    return {};
  }
  if (legacy_retained_gpu_backing) {
    return synthetic_gpu_backing_materialize_to_image(legacy_retained_gpu_backing);
  }
  return {};
}

void godot_gpu_display_invalidate_descriptor(const RetainedGpuBackingDescriptor& descriptor) {
  (void)godot_gpu_display_descriptor_has_complete_identity(descriptor);
  // No descriptor-keyed display cache exists in this slice. Future providers
  // may use this boundary for Godot-layer adapter invalidation only.
}

void godot_gpu_display_invalidate_stream(uint64_t stream_id) {
  if (stream_id == 0) {
    return;
  }
  // No stream-keyed display cache exists in this slice. Future providers may
  // use this boundary for Godot-layer adapter invalidation only.
}

void godot_gpu_display_invalidate_all() {
  // Non-owning today: no service cache owns Godot Texture2D refs.
}

} // namespace cambang

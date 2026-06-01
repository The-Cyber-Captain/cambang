#include "godot/godot_gpu_display_service.h"

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
  // Dormant Tranche 2 scaffold: no Godot display cache owns Texture2D refs yet.
  return {};
}

godot::Ref<godot::Texture2D> godot_gpu_display_get_texture_by_descriptor(
    const RetainedGpuBackingDescriptor& descriptor,
    const std::shared_ptr<void>& legacy_retained_gpu_backing) {
  (void)godot_gpu_display_descriptor_has_complete_identity(descriptor);
  (void)legacy_retained_gpu_backing;
  // Dormant Tranche 2 scaffold only. StreamResult.get_display_view() must keep
  // using the legacy retained_gpu_backing path directly in this tranche; do not
  // adapt or return legacy textures through this service yet.
  return {};
}

void godot_gpu_display_invalidate_descriptor(const RetainedGpuBackingDescriptor& descriptor) {
  (void)godot_gpu_display_descriptor_has_complete_identity(descriptor);
  // Dormant scaffold: no descriptor-keyed display cache exists yet.
}

void godot_gpu_display_invalidate_stream(uint64_t stream_id) {
  if (stream_id == 0) {
    return;
  }
  // Dormant scaffold: no stream-keyed display cache exists yet.
}

void godot_gpu_display_invalidate_all() {
  // Dormant scaffold: no display cache owns Godot Texture2D refs yet.
}

} // namespace cambang

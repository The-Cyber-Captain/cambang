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
  //
  // stream_id is deliberately NOT required. It once was, which quietly meant
  // "only a stream backing can have identity" and made this predicate answer
  // false for every still capture -- a capture belongs to no stream, so its
  // stream_id is legitimately 0. That requirement was really compensating for
  // a provider minting per-stream backing_ids, where the pair was needed to
  // disambiguate; backing_id is now provider-unique by contract, so it
  // identifies a backing on its own and stream_id is correlation only.
  return descriptor.valid &&
         descriptor.display_available &&
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

namespace {

// Whether the opaque backing handle may be handed to the synthetic bridge.
//
// The bridge static_pointer_casts the handle straight to its own
// RetainedSyntheticGpuBacking, an RGBA8 texture it allocated itself. That cast
// is unchecked, so routing any other producer's artifact into it is undefined
// behaviour rather than a failed lookup. A LINEAR descriptor is the only shape
// the synthetic path ever produces; an OPAQUE one means a real platform
// resource, which has no synthetic-bridge realization and must fall through to
// descriptor-native lookup instead of being reinterpreted.
bool descriptor_routes_to_synthetic_bridge(
    const RetainedGpuBackingDescriptor& descriptor,
    const std::shared_ptr<void>& legacy_retained_gpu_backing) noexcept {
  if (!legacy_retained_gpu_backing) {
    return false;
  }
  return !descriptor.valid || descriptor.layout_kind == GpuBackingLayoutKind::LINEAR;
}

} // namespace

godot::Ref<godot::Texture2D> godot_gpu_display_get_texture_by_descriptor(
    const RetainedGpuBackingDescriptor& descriptor,
    const std::shared_ptr<void>& legacy_retained_gpu_backing) {
  (void)godot_gpu_display_descriptor_has_complete_identity(descriptor);
  // The completeness helper is for future descriptor-native/provider-backed
  // lookup and must not block the current synthetic compatibility path. When a
  // legacy retained backing exists, delegate without storing the returned
  // Texture2D so display-view ownership and lifetime diagnostics remain visible
  // in the synthetic bridge. Descriptor-only lookup remains no-op/null for now.
  if (descriptor_routes_to_synthetic_bridge(descriptor, legacy_retained_gpu_backing)) {
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
  if (descriptor_routes_to_synthetic_bridge(descriptor, legacy_retained_gpu_backing)) {
    return synthetic_gpu_backing_can_materialize_to_image(legacy_retained_gpu_backing);
  }
  // An OPAQUE backing may well be materializable, but not by this bridge. Its
  // producer-side readback path is not built yet, so report unsupported rather
  // than promising a conversion nothing here can perform.
  return false;
}

godot::Ref<godot::Image> godot_gpu_display_materialize_to_image(
    const RetainedGpuBackingDescriptor& descriptor,
    const std::shared_ptr<void>& legacy_retained_gpu_backing) {
  if (!godot_gpu_display_can_materialize_to_image(descriptor, legacy_retained_gpu_backing)) {
    return {};
  }
  if (descriptor_routes_to_synthetic_bridge(descriptor, legacy_retained_gpu_backing)) {
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
  synthetic_gpu_backing_invalidate_live_display_wrappers_for_stream(stream_id);
}

void godot_gpu_display_invalidate_all() {
  synthetic_gpu_backing_invalidate_all_live_display_wrappers();
}

} // namespace cambang

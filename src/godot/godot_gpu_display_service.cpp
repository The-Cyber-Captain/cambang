#include "godot/godot_gpu_display_service.h"

#include <map>
#include <mutex>
#include <tuple>
#include <utility>

#include "godot/synthetic_gpu_backing_bridge.h"

namespace cambang {
namespace {

struct DisplayDescriptorKey final {
  uint64_t stream_id = 0;
  uint64_t backing_id = 0;
  uint64_t capture_timestamp_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
  uint32_t format_fourcc = 0;

  bool operator<(const DisplayDescriptorKey& other) const noexcept {
    return std::tie(stream_id,
                    backing_id,
                    capture_timestamp_ns,
                    width,
                    height,
                    stride_bytes,
                    format_fourcc) <
           std::tie(other.stream_id,
                    other.backing_id,
                    other.capture_timestamp_ns,
                    other.width,
                    other.height,
                    other.stride_bytes,
                    other.format_fourcc);
  }
};

struct DisplayCacheEntry final {
  godot::Ref<godot::Texture2D> texture;
};

std::mutex g_display_cache_mutex;
std::map<DisplayDescriptorKey, DisplayCacheEntry> g_display_cache;

DisplayDescriptorKey make_display_descriptor_key(const RetainedGpuBackingDescriptor& descriptor) noexcept {
  DisplayDescriptorKey key{};
  key.stream_id = descriptor.stream_id;
  key.backing_id = descriptor.backing_id;
  key.capture_timestamp_ns = descriptor.capture_timestamp_ns;
  key.width = descriptor.width;
  key.height = descriptor.height;
  key.stride_bytes = descriptor.stride_bytes;
  key.format_fourcc = descriptor.format_fourcc;
  return key;
}

} // namespace

bool godot_gpu_display_descriptor_has_complete_identity(
    const RetainedGpuBackingDescriptor& descriptor) noexcept {
  // backing_id == 0 is compatibility metadata only. It means a GPU backing may
  // exist, but provider/core did not supply a scalar identity/generation. Never
  // use such a descriptor as a display-cache key or for stale-generation checks.
  return descriptor.valid &&
         descriptor.display_available &&
         descriptor.stream_id != 0 &&
         descriptor.backing_id != 0 &&
         descriptor.width != 0 &&
         descriptor.height != 0;
}

godot::Ref<godot::Texture2D> godot_gpu_display_lookup_texture_by_descriptor(
    const RetainedGpuBackingDescriptor& descriptor) {
  if (!godot_gpu_display_descriptor_has_complete_identity(descriptor)) {
    return {};
  }

  std::lock_guard<std::mutex> lock(g_display_cache_mutex);
  const auto it = g_display_cache.find(make_display_descriptor_key(descriptor));
  if (it == g_display_cache.end()) {
    return {};
  }
  return it->second.texture;
}

godot::Ref<godot::Texture2D> godot_gpu_display_get_texture_by_descriptor(
    const RetainedGpuBackingDescriptor& descriptor,
    const std::shared_ptr<void>& legacy_retained_gpu_backing) {
  if (!godot_gpu_display_descriptor_has_complete_identity(descriptor)) {
    // Incomplete descriptor identity, including backing_id == 0 compatibility
    // metadata, remains on the legacy retained_gpu_backing display path in this
    // tranche so existing behaviour is preserved.
    return synthetic_gpu_backing_display_texture(legacy_retained_gpu_backing);
  }

  const DisplayDescriptorKey key = make_display_descriptor_key(descriptor);
  {
    std::lock_guard<std::mutex> lock(g_display_cache_mutex);
    const auto it = g_display_cache.find(key);
    if (it != g_display_cache.end()) {
      return it->second.texture;
    }
  }

  // Temporary adapter: until a later tranche adds descriptor-native display
  // resolution, descriptor-keyed display entries are populated from the existing
  // synthetic legacy retained_gpu_backing path.
  godot::Ref<godot::Texture2D> texture = synthetic_gpu_backing_display_texture(legacy_retained_gpu_backing);
  if (texture.is_null()) {
    return {};
  }

  std::lock_guard<std::mutex> lock(g_display_cache_mutex);
  DisplayCacheEntry& entry = g_display_cache[key];
  entry.texture = texture;
  return entry.texture;
}

void godot_gpu_display_invalidate_descriptor(const RetainedGpuBackingDescriptor& descriptor) {
  if (!godot_gpu_display_descriptor_has_complete_identity(descriptor)) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_display_cache_mutex);
  g_display_cache.erase(make_display_descriptor_key(descriptor));
}

void godot_gpu_display_invalidate_stream(uint64_t stream_id) {
  if (stream_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_display_cache_mutex);
  for (auto it = g_display_cache.begin(); it != g_display_cache.end();) {
    if (it->first.stream_id == stream_id) {
      it = g_display_cache.erase(it);
    } else {
      ++it;
    }
  }
}

void godot_gpu_display_invalidate_all() {
  std::lock_guard<std::mutex> lock(g_display_cache_mutex);
  g_display_cache.clear();
}

} // namespace cambang

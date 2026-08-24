#include "godot/capture_compute_texture.h"

#include <deque>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>

#include "godot/cambang_result_convert.h"
#include "godot/godot_gpu_display_service.h"

namespace cambang {
namespace {

// A GPU device is what makes any of this possible. Under the Compatibility
// renderer there is no RenderingDevice, so there is nowhere to put a texture
// and the honest answer is UNSUPPORTED rather than a silently degraded object.
bool rendering_device_available() {
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  return rs != nullptr && rs->get_rendering_device() != nullptr;
}

// Whether the member's retained GPU backing can be handed over as-is.
//
// display_requires_import is the discriminator the descriptor carries for a
// backing that exists but costs real work to reach -- an AHardwareBuffer that
// still needs importing, a shared D3D11 handle. Such a backing is supported but
// not READY, and it must not be imported here just to make the classification
// read better.
bool member_has_ready_gpu_texture(const CoreCaptureResultData::ImageMemberData& member) {
  return member.retained_gpu_backing &&
         member.retained_gpu_backing_descriptor.valid &&
         member.retained_gpu_backing_descriptor.display_available &&
         !member.retained_gpu_backing_descriptor.display_requires_import;
}

bool member_has_cpu_source(const CoreCaptureResultData::ImageMemberData& member) {
  return retained_cpu_payload_is_packed_readable(member.payload) ||
         retained_cpu_payload_is_convertible(member.payload);
}

struct CacheKey {
  uint64_t capture_id = 0;
  uint64_t device_instance_id = 0;
  uint32_t image_member_index = 0;

  bool operator<(const CacheKey& other) const noexcept {
    if (capture_id != other.capture_id) return capture_id < other.capture_id;
    if (device_instance_id != other.device_instance_id) {
      return device_instance_id < other.device_instance_id;
    }
    return image_member_index < other.image_member_index;
  }
};

// Bounded on purpose. The cache exists so a caller polling get_result() does
// not re-upload the same frozen member every tick; it is not a retention
// mechanism, and it deliberately has no coupling to Core's capture eviction.
// Dropping an entry is always safe because a caller holding the Ref keeps the
// texture alive independently -- eviction costs a later re-upload, never a
// dangling texture.
constexpr size_t kMaxCachedComputeTextures = 8;

std::mutex g_mutex;
std::map<CacheKey, godot::Ref<godot::Texture2D>> g_cache;
std::deque<CacheKey> g_insertion_order;
uint64_t g_uploads = 0;
uint64_t g_gpu_wraps = 0;
uint64_t g_hits = 0;

} // namespace

ResultCapability capture_compute_texture_support(
    const SharedCaptureResultData& data,
    uint32_t image_member_index) {
  if (!data) {
    return ResultCapability::UNSUPPORTED;
  }
  const auto* member = data->image_member_at(image_member_index);
  if (!member) {
    return ResultCapability::UNSUPPORTED;
  }
  if (!rendering_device_available()) {
    return ResultCapability::UNSUPPORTED;
  }
  if (member_has_ready_gpu_texture(*member)) {
    return ResultCapability::READY;
  }
  // Everything else that has pixels at all is a full-frame upload or import.
  // 11.2's worked example for EXPENSIVE is exactly a full-frame copy, and
  // reporting UNSUPPORTED here would be false: the caller can have the texture,
  // it simply costs.
  if (member_has_cpu_source(*member) || member->retained_gpu_backing) {
    return ResultCapability::EXPENSIVE;
  }
  return ResultCapability::UNSUPPORTED;
}

godot::Ref<godot::Texture2D> capture_compute_texture_for_member(
    const SharedCaptureResultData& data,
    uint32_t image_member_index,
    const std::function<godot::Ref<godot::Image>()>& cpu_image_supplier) {
  if (capture_compute_texture_support(data, image_member_index) ==
      ResultCapability::UNSUPPORTED) {
    return {};
  }
  const auto* member = data->image_member_at(image_member_index);
  if (!member) {
    return {};
  }

  const CacheKey key{data->capture_id, data->device_instance_id, image_member_index};
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_cache.find(key);
    if (it != g_cache.end() && it->second.is_valid()) {
      ++g_hits;
      return it->second;
    }
  }

  godot::Ref<godot::Texture2D> produced;
  bool was_upload = false;
  if (member_has_ready_gpu_texture(*member)) {
    // Already GPU-resident: wrap the retained backing rather than moving any
    // pixels. This is the whole point of the READY row.
    produced = godot_gpu_display_get_texture_by_descriptor(
        member->retained_gpu_backing_descriptor,
        member->retained_gpu_backing);
  }
  if (produced.is_null()) {
    // Full-frame upload from the retained CPU payload. FORMAT_RGBA8 out of
    // payload_to_image() means the resulting RenderingDevice texture has an
    // ordinary format a compute shader can sample -- unlike a vendor external
    // format, which 11.6.1 excludes.
    const godot::Ref<godot::Image> image =
        cpu_image_supplier ? cpu_image_supplier() : godot::Ref<godot::Image>();
    if (image.is_null()) {
      return {};
    }
    produced = godot::ImageTexture::create_from_image(image);
    was_upload = true;
  }
  if (produced.is_null()) {
    return {};
  }

  std::vector<godot::Ref<godot::Texture2D>> dropped;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (was_upload) {
      ++g_uploads;
    } else {
      ++g_gpu_wraps;
    }
    g_cache[key] = produced;
    g_insertion_order.push_back(key);
    while (g_insertion_order.size() > kMaxCachedComputeTextures) {
      const CacheKey oldest = g_insertion_order.front();
      g_insertion_order.pop_front();
      const auto it = g_cache.find(oldest);
      if (it != g_cache.end()) {
        dropped.push_back(std::move(it->second));
        g_cache.erase(it);
      }
    }
  }
  // Evicted refs are released after the lock. A DeferredDisplayTexture2DRD
  // destructor hands its wrapper to the render-thread release drain, and that
  // is not work to do while holding this mutex.
  dropped.clear();
  return produced;
}

void clear_capture_compute_texture_cache() {
  std::map<CacheKey, godot::Ref<godot::Texture2D>> dropped;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    dropped.swap(g_cache);
    g_insertion_order.clear();
  }
  dropped.clear();
}

godot::Dictionary capture_compute_texture_metrics() {
  std::lock_guard<std::mutex> lock(g_mutex);
  godot::Dictionary d;
  d["uploads"] = static_cast<uint64_t>(g_uploads);
  d["gpu_wraps"] = static_cast<uint64_t>(g_gpu_wraps);
  d["hits"] = static_cast<uint64_t>(g_hits);
  d["entries"] = static_cast<uint64_t>(g_cache.size());
  return d;
}

} // namespace cambang

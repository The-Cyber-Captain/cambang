#include "godot/stream_compute_texture.h"

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
#include "godot/compute_texture_planes.h"

namespace cambang {
namespace {

// A GPU device is what makes any of this possible. Under the Compatibility
// renderer there is no RenderingDevice, so there is nowhere to put a texture
// and the honest answer is UNSUPPORTED rather than a silently degraded object.
bool rendering_device_available() {
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  return rs != nullptr && rs->get_rendering_device() != nullptr;
}

bool stream_has_planar_source(const CoreStreamResultData& data) {
  return retained_cpu_bytes_are_current(data) && data.payload.is_planar() &&
         data.payload.plane_count > 0;
}

bool stream_has_packed_source(const CoreStreamResultData& data) {
  return retained_cpu_bytes_are_current(data) &&
         retained_cpu_payload_is_packed_readable(data.payload);
}

// Frame identity, not stream identity. See the header: keying on stream_id
// alone would serve a previous frame's pixels for the current frame's mark.
struct CacheKey {
  uint64_t stream_id = 0;
  uint64_t device_instance_id = 0;
  uint64_t retained_frame_id = 0;
  uint32_t plane_index = 0;

  bool operator<(const CacheKey& other) const noexcept {
    if (stream_id != other.stream_id) return stream_id < other.stream_id;
    if (device_instance_id != other.device_instance_id) {
      return device_instance_id < other.device_instance_id;
    }
    if (retained_frame_id != other.retained_frame_id) {
      return retained_frame_id < other.retained_frame_id;
    }
    return plane_index < other.plane_index;
  }
};

// Bounded on purpose, and smaller than the capture cache relative to its churn:
// a stream replaces its retained frame every frame, so entries go stale fast and
// the cache exists only so that several plane requests against ONE frame do not
// each re-upload. It is not a retention mechanism. Dropping an entry is always
// safe because a caller holding the Ref keeps its texture alive independently --
// eviction costs a later re-upload, never a dangling texture.
constexpr size_t kMaxCachedStreamComputeTextures = 12;

std::mutex g_mutex;
std::map<CacheKey, godot::Ref<godot::Texture2D>> g_cache;
std::deque<CacheKey> g_insertion_order;
uint64_t g_uploads = 0;
uint64_t g_hits = 0;
uint64_t g_uploaded_bytes = 0;

} // namespace

ResultCapability stream_compute_texture_support(const SharedStreamResultData& data) {
  if (!data || !rendering_device_available()) {
    return ResultCapability::UNSUPPORTED;
  }
  // Deliberately NO READY row, unlike the capture surface.
  //
  // A stream's retained_gpu_backing is documented in core_result_store.h as
  // "stream-owned live backing updated in place while flowing... not frozen
  // per-frame GPU artifact identity". Wrapping it would hand back an ALIASED
  // texture from a surface whose whole contract is that it is frozen -- the
  // one guarantee that distinguishes this from get_display_view(). There is no
  // field on the descriptor that could gate it, because the liveness is a
  // property of what the stream path retains, not of the backing's shape.
  //
  // So a GPU-only stream frame is UNSUPPORTED here, and a GPU-primary frame
  // with a current CPU sidecar is served from the sidecar below like any other.
  // Truthful, and it does not promise a freeze it cannot keep.

  // A plane upload is still a full-frame copy, which is 11.2's worked example
  // of EXPENSIVE. Reporting UNSUPPORTED would be false -- the caller can have
  // the textures, they simply cost.
  if (stream_has_planar_source(*data) || stream_has_packed_source(*data)) {
    return ResultCapability::EXPENSIVE;
  }
  return ResultCapability::UNSUPPORTED;
}

uint32_t stream_compute_texture_plane_count(const SharedStreamResultData& data) {
  if (stream_compute_texture_support(data) == ResultCapability::UNSUPPORTED) {
    return 0;
  }
  if (stream_has_planar_source(*data)) {
    return static_cast<uint32_t>(data->payload.plane_count);
  }
  return 1;
}

godot::Ref<godot::Texture2D> stream_compute_texture_plane(
    const SharedStreamResultData& data,
    uint32_t plane_index) {
  const uint32_t plane_count = stream_compute_texture_plane_count(data);
  if (plane_count == 0 || plane_index >= plane_count) {
    return {};
  }

  const CacheKey key{data->stream_id, data->device_instance_id,
                     data->retained_frame_id, plane_index};
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_cache.find(key);
    if (it != g_cache.end() && it->second.is_valid()) {
      ++g_hits;
      return it->second;
    }
  }

  godot::Ref<godot::Texture2D> produced;
  uint64_t uploaded_bytes = 0;

  if (stream_has_planar_source(*data)) {
    const PlaneShape shape = describe_plane(data->payload, plane_index);
    const godot::Ref<godot::Image> image = plane_to_image(data->payload, plane_index, shape);
    if (image.is_null()) {
      return {};
    }
    produced = godot::ImageTexture::create_from_image(image);
    uploaded_bytes = static_cast<uint64_t>(shape.width) * shape.height * shape.bytes_per_sample;
  } else {
    // Packed frame: one plane, handed over in the packed form it is retained
    // in. payload_to_image() is a copy for a packed RGB payload, not a
    // conversion.
    const godot::Ref<godot::Image> image = payload_to_image(data->payload);
    if (image.is_null()) {
      return {};
    }
    produced = godot::ImageTexture::create_from_image(image);
    uploaded_bytes = static_cast<uint64_t>(data->payload.size_bytes());
  }
  if (produced.is_null()) {
    return {};
  }

  std::vector<godot::Ref<godot::Texture2D>> dropped;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_uploads;
    g_uploaded_bytes += uploaded_bytes;
    g_cache[key] = produced;
    g_insertion_order.push_back(key);
    while (g_insertion_order.size() > kMaxCachedStreamComputeTextures) {
      const CacheKey oldest = g_insertion_order.front();
      g_insertion_order.pop_front();
      const auto it = g_cache.find(oldest);
      if (it != g_cache.end()) {
        dropped.push_back(std::move(it->second));
        g_cache.erase(it);
      }
    }
  }
  // Evicted refs are released after the lock, for the same reason the capture
  // cache does it: a deferred GPU wrapper's destructor hands work to the
  // render-thread release drain, which is not work to do while holding this.
  dropped.clear();
  return produced;
}

void remove_stream_compute_textures(uint64_t stream_id) {
  // Called where the other per-stream Godot-side caches are dropped. Without
  // it a destroyed stream's textures stay resident until later entries push
  // them out -- bounded, but holding GPU memory for a stream that no longer
  // exists, and inconsistent with how the live display view is handled.
  std::vector<godot::Ref<godot::Texture2D>> dropped;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_cache.begin(); it != g_cache.end();) {
      if (it->first.stream_id == stream_id) {
        dropped.push_back(std::move(it->second));
        it = g_cache.erase(it);
      } else {
        ++it;
      }
    }
    std::deque<CacheKey> kept;
    for (const CacheKey& k : g_insertion_order) {
      if (k.stream_id != stream_id) {
        kept.push_back(k);
      }
    }
    g_insertion_order.swap(kept);
  }
  // Released outside the lock, for the same reason eviction is.
  dropped.clear();
}

void clear_stream_compute_texture_cache() {
  std::map<CacheKey, godot::Ref<godot::Texture2D>> dropped;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    dropped.swap(g_cache);
    g_insertion_order.clear();
    g_uploads = 0;
    g_hits = 0;
    g_uploaded_bytes = 0;
  }
  dropped.clear();
}

godot::Dictionary stream_compute_texture_metrics() {
  godot::Dictionary out;
  std::lock_guard<std::mutex> lock(g_mutex);
  out["uploads"] = static_cast<int64_t>(g_uploads);
  out["hits"] = static_cast<int64_t>(g_hits);
  out["entries"] = static_cast<int64_t>(g_cache.size());
  out["uploaded_bytes"] = static_cast<int64_t>(g_uploaded_bytes);
  return out;
}

} // namespace cambang

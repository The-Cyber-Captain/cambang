#include "godot/capture_compute_texture.h"

#include <cstring>
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
#include "pixels/format/pixel_format_descriptor.h"

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
// display_requires_import is the discriminator for a backing that exists but
// costs real work to reach -- an AHardwareBuffer that still needs importing, a
// shared D3D11 handle. Such a backing is supported but not READY, and must not
// be imported here just to make the classification read better.
bool member_has_ready_gpu_texture(const CoreCaptureResultData::ImageMemberData& member) {
  return member.retained_gpu_backing &&
         member.retained_gpu_backing_descriptor.valid &&
         member.retained_gpu_backing_descriptor.display_available &&
         !member.retained_gpu_backing_descriptor.display_requires_import;
}

// Geometry of one plane as it will be exposed: a single-plane texture in the
// member's own format, never a converted one.
struct PlaneShape {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bytes_per_sample = 0;
  godot::Image::Format image_format = godot::Image::FORMAT_R8;
  bool valid = false;
};

PlaneShape describe_plane(const CoreResultPayloadCpu& payload, uint32_t plane_index) {
  PlaneShape shape{};
  const PixelFormatDescriptor desc = describe_pixel_format(payload.format_fourcc);
  if (!desc.valid || plane_index >= payload.plane_count) {
    return shape;
  }
  if (plane_index == 0) {
    // Luma, or the sole plane of anything single-plane.
    shape.width = payload.width;
    shape.height = payload.height;
    shape.bytes_per_sample = desc.plane0_bytes_per_sample;
  } else {
    shape.width = payload.width >> desc.chroma_shift_x;
    shape.height = payload.height >> desc.chroma_shift_y;
    // Semi-planar keeps both chroma components interleaved in one plane, so a
    // sample there is two bytes wide; fully planar splits them, one byte each.
    shape.bytes_per_sample = (desc.plane_count == 2) ? 2u : 1u;
  }
  if (shape.width == 0 || shape.height == 0) {
    return shape;
  }
  switch (shape.bytes_per_sample) {
    case 1: shape.image_format = godot::Image::FORMAT_R8; break;
    case 2: shape.image_format = godot::Image::FORMAT_RG8; break;
    default: return shape;  // stays invalid: no single-plane format fits
  }
  shape.valid = true;
  return shape;
}

// Copies one plane out of the retained buffer into a tightly packed image.
//
// The retained stride may exceed the tight row width -- camera buffers are
// commonly padded -- and Image::create_from_data requires tight packing, so a
// padded plane is copied row by row. Even then this moves one plane's bytes
// once, against the whole-frame colour conversion it replaces.
godot::Ref<godot::Image> plane_to_image(const CoreResultPayloadCpu& payload,
                                        uint32_t plane_index,
                                        const PlaneShape& shape) {
  const uint8_t* src = payload.plane_data(plane_index);
  if (!src || !shape.valid) {
    return {};
  }
  const CoreResultPayloadCpuPlane& plane = payload.planes[plane_index];
  const uint32_t tight_row = shape.width * shape.bytes_per_sample;
  if (tight_row == 0 || plane.stride_bytes < tight_row || plane.rows < shape.height) {
    return {};
  }
  godot::PackedByteArray bytes;
  bytes.resize(static_cast<int64_t>(tight_row) * static_cast<int64_t>(shape.height));
  uint8_t* dst = bytes.ptrw();
  if (!dst) {
    return {};
  }
  if (plane.stride_bytes == tight_row) {
    std::memcpy(dst, src, static_cast<size_t>(tight_row) * shape.height);
  } else {
    for (uint32_t row = 0; row < shape.height; ++row) {
      std::memcpy(dst + static_cast<size_t>(row) * tight_row,
                  src + static_cast<size_t>(row) * plane.stride_bytes,
                  tight_row);
    }
  }
  return godot::Image::create_from_data(static_cast<int32_t>(shape.width),
                                        static_cast<int32_t>(shape.height),
                                        false,
                                        shape.image_format,
                                        bytes);
}

bool member_has_packed_source(const CoreCaptureResultData::ImageMemberData& member) {
  return retained_cpu_payload_is_packed_readable(member.payload);
}

bool member_has_planar_source(const CoreCaptureResultData::ImageMemberData& member) {
  return member.payload.is_planar() && member.payload.plane_count > 0 &&
         !member.payload.empty();
}

struct CacheKey {
  uint64_t capture_id = 0;
  uint64_t device_instance_id = 0;
  uint32_t image_member_index = 0;
  uint32_t plane_index = 0;

  bool operator<(const CacheKey& other) const noexcept {
    if (capture_id != other.capture_id) return capture_id < other.capture_id;
    if (device_instance_id != other.device_instance_id) {
      return device_instance_id < other.device_instance_id;
    }
    if (image_member_index != other.image_member_index) {
      return image_member_index < other.image_member_index;
    }
    return plane_index < other.plane_index;
  }
};

// Bounded on purpose. The cache exists so a caller polling get_result() does not
// re-upload the same frozen plane every tick; it is not a retention mechanism,
// and it deliberately has no coupling to Core's capture eviction. Dropping an
// entry is always safe because a caller holding the Ref keeps the texture alive
// independently -- eviction costs a later re-upload, never a dangling texture.
constexpr size_t kMaxCachedComputeTextures = 24;

std::mutex g_mutex;
std::map<CacheKey, godot::Ref<godot::Texture2D>> g_cache;
std::deque<CacheKey> g_insertion_order;
uint64_t g_uploads = 0;
uint64_t g_gpu_wraps = 0;
uint64_t g_hits = 0;
uint64_t g_uploaded_bytes = 0;

} // namespace

ResultCapability capture_compute_texture_support(
    const SharedCaptureResultData& data,
    uint32_t image_member_index) {
  if (!data) {
    return ResultCapability::UNSUPPORTED;
  }
  const auto* member = data->image_member_at(image_member_index);
  if (!member || !rendering_device_available()) {
    return ResultCapability::UNSUPPORTED;
  }
  if (member_has_ready_gpu_texture(*member)) {
    return ResultCapability::READY;
  }
  // A plane upload is still a full-frame copy, which is 11.2's worked example
  // of EXPENSIVE. Reporting UNSUPPORTED would be false -- the caller can have
  // the textures, they simply cost.
  if (member_has_packed_source(*member) || member_has_planar_source(*member)) {
    return ResultCapability::EXPENSIVE;
  }
  return ResultCapability::UNSUPPORTED;
}

uint32_t capture_compute_texture_plane_count(
    const SharedCaptureResultData& data,
    uint32_t image_member_index) {
  if (capture_compute_texture_support(data, image_member_index) ==
      ResultCapability::UNSUPPORTED) {
    return 0;
  }
  const auto* member = data->image_member_at(image_member_index);
  if (!member) {
    return 0;
  }
  if (member_has_ready_gpu_texture(*member)) {
    return 1;
  }
  if (member_has_planar_source(*member)) {
    return static_cast<uint32_t>(member->payload.plane_count);
  }
  return 1;
}

godot::Ref<godot::Texture2D> capture_compute_texture_plane(
    const SharedCaptureResultData& data,
    uint32_t image_member_index,
    uint32_t plane_index) {
  const uint32_t plane_count = capture_compute_texture_plane_count(data, image_member_index);
  if (plane_count == 0 || plane_index >= plane_count) {
    return {};
  }
  const auto* member = data->image_member_at(image_member_index);
  if (!member) {
    return {};
  }

  const CacheKey key{data->capture_id, data->device_instance_id, image_member_index,
                     plane_index};
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
  uint64_t uploaded_bytes = 0;

  if (member_has_ready_gpu_texture(*member)) {
    // Already GPU-resident: wrap the retained backing rather than moving any
    // pixels. This is the whole point of the READY row.
    produced = godot_gpu_display_get_texture_by_descriptor(
        member->retained_gpu_backing_descriptor,
        member->retained_gpu_backing);
  } else if (member_has_planar_source(*member)) {
    const PlaneShape shape = describe_plane(member->payload, plane_index);
    const godot::Ref<godot::Image> image = plane_to_image(member->payload, plane_index, shape);
    if (image.is_null()) {
      return {};
    }
    produced = godot::ImageTexture::create_from_image(image);
    was_upload = true;
    uploaded_bytes = static_cast<uint64_t>(shape.width) * shape.height * shape.bytes_per_sample;
  } else {
    // Packed member: one plane, handed over in the packed form it is retained
    // in. payload_to_image() is a copy for a packed RGB payload, not a
    // conversion.
    const godot::Ref<godot::Image> image = payload_to_image(member->payload);
    if (image.is_null()) {
      return {};
    }
    produced = godot::ImageTexture::create_from_image(image);
    was_upload = true;
    uploaded_bytes = static_cast<uint64_t>(member->payload.size_bytes());
  }
  if (produced.is_null()) {
    return {};
  }

  std::vector<godot::Ref<godot::Texture2D>> dropped;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (was_upload) {
      ++g_uploads;
      g_uploaded_bytes += uploaded_bytes;
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
  // destructor hands its wrapper to the render-thread release drain, and that is
  // not work to do while holding this mutex.
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
  d["uploaded_bytes"] = static_cast<uint64_t>(g_uploaded_bytes);
  return d;
}

} // namespace cambang

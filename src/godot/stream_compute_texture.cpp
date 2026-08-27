#include "godot/stream_compute_texture.h"

#include <atomic>
#include <chrono>
#include <utility>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>

#include "godot/cambang_result_convert.h"
#include "godot/compute_texture_planes.h"
#include "godot/result_access_cost_evidence.h"

namespace cambang {
namespace {

// Same clock the other result-access paths measure with, so the three routes
// are directly comparable.
uint64_t access_now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

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

// No cache lives here any more. Plane textures are held by the
// CamBANGStreamResult that produced them, because that object IS the identity
// a cache would have to key on: stream, device and retained frame. See the
// comment on CamBANGStreamResult::compute_texture_planes_ for what the global
// FIFO that used to be here cost and why it never paid for itself.
//
// These counters remain because they are cheap and answer a real question:
// how often a caller re-requests a plane it is already holding.
std::atomic<uint64_t> g_uploads{0};
std::atomic<uint64_t> g_hits{0};
std::atomic<uint64_t> g_uploaded_bytes{0};

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
  // Measured through the same cost-evidence machinery as get_display_view()
  // and to_image(), so the three are comparable rather than each having its own
  // notion of what a measurement is.
  const uint64_t begin_ns = access_now_ns();
  const ResultCapability reported = stream_compute_texture_support(data);
  const uint32_t plane_count = stream_compute_texture_plane_count(data);
  if (plane_count == 0 || plane_index >= plane_count) {
    result_access_cost_evidence::record_stream_access(
        result_access_cost_evidence::kRouteStreamAccessUnsupported, data,
        access_now_ns() - begin_ns, false,
        ResultCapability::UNSUPPORTED);
    return {};
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

  g_uploads.fetch_add(1, std::memory_order_relaxed);
  g_uploaded_bytes.fetch_add(uploaded_bytes, std::memory_order_relaxed);
  result_access_cost_evidence::record_stream_access(
      result_access_cost_evidence::kRouteStreamComputeTexturePlaneUpload, data,
      access_now_ns() - begin_ns, true, reported);
  return produced;
}

void stream_compute_texture_note_cached_plane(const SharedStreamResultData& data) {
  // A plane the caller already holds, returned from the result's own storage.
  // Recorded so `hits` still means what it always meant -- a re-request that
  // cost no upload -- now that the holding is done by the result rather than
  // by a cache here.
  g_hits.fetch_add(1, std::memory_order_relaxed);
  result_access_cost_evidence::record_stream_access(
      result_access_cost_evidence::kRouteStreamComputeTexturePlaneCached, data,
      0, true, ResultCapability::EXPENSIVE);
}

void clear_stream_compute_texture_cache() {
  // Nothing to free: the textures belong to the results that produced them and
  // go when those go. The name is kept because this is called beside the other
  // per-runtime cache resets on stop/restart, and the counters must reset with
  // them or a new session inherits the last one's totals.
  g_uploads.store(0, std::memory_order_relaxed);
  g_hits.store(0, std::memory_order_relaxed);
  g_uploaded_bytes.store(0, std::memory_order_relaxed);
}

godot::Dictionary stream_compute_texture_metrics() {
  godot::Dictionary out;
  out["uploads"] = static_cast<int64_t>(g_uploads.load(std::memory_order_relaxed));
  out["hits"] = static_cast<int64_t>(g_hits.load(std::memory_order_relaxed));
  out["uploaded_bytes"] =
      static_cast<int64_t>(g_uploaded_bytes.load(std::memory_order_relaxed));
  return out;
}

} // namespace cambang

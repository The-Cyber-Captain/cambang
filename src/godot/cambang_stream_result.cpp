#include "godot/cambang_stream_result.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>

#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include "core/core_runtime.h"
#include "godot/cambang_result_convert.h"
#include "godot/cambang_server.h"
#include "godot/godot_gpu_display_service.h"
#include "godot/result_access_cost_evidence.h"
#include "godot/cambang_stream_result_internal.h"

namespace cambang {

namespace {

bool stream_display_trace_enabled() {
  const char* value = std::getenv("CAMBANG_DEV_SYNTH_GPU_TRACE");
  return value && value[0] != '\0' && value[0] != '0';
}

void trace_stream_display_path(const char* path) {
  if (!stream_display_trace_enabled()) {
    return;
  }
  godot::UtilityFunctions::print("[CamBANG][StreamResult] display_view_path=", path);
}
bool display_demand_trace_enabled() {
  const char* value = std::getenv("CAMBANG_DEV_DISPLAY_DEMAND_TRACE");
  return value && value[0] != '\0' && value[0] != '0';
}

bool has_current_retained_cpu_payload(const SharedStreamResultData& data) {
  if (!data || data->payload_capture_timestamp_ns != data->capture_timestamp_ns) {
    return false;
  }
  if (data->payload.width == 0 || data->payload.height == 0 || data->payload.empty()) {
    return false;
  }
  if (data->payload.width != data->image_width ||
      data->payload.height != data->image_height ||
      data->payload.format_fourcc != data->image_format_fourcc) {
    return false;
  }
  if (data->payload.format_fourcc != FOURCC_RGBA && data->payload.format_fourcc != FOURCC_BGRA) {
    return false;
  }
  const size_t expected_size =
      static_cast<size_t>(data->payload.width) * static_cast<size_t>(data->payload.height) * 4u;
  return data->payload.stride_bytes == data->payload.width * 4u &&
         data->payload.size_bytes() >= expected_size;
}

uint64_t result_access_now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t elapsed_ns_since(const std::chrono::steady_clock::time_point& begin) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - begin)
          .count());
}

const char* to_image_evidence_route(const SharedStreamResultData& data) {
  if (!data) {
    return result_access_cost_evidence::kRouteStreamAccessUnsupported;
  }
  if (has_current_retained_cpu_payload(data)) {
    if (data->payload_kind == ResultPayloadKind::GPU_SURFACE) {
      return result_access_cost_evidence::kRouteStreamToImageGpuPrimaryCpuSidecar;
    }
    return result_access_cost_evidence::kRouteStreamToImageCpuPacked;
  }
  if (data->payload_kind == ResultPayloadKind::GPU_SURFACE &&
      data->retained_gpu_backing &&
      data->retained_gpu_backing_descriptor.valid &&
      data->retained_gpu_backing_descriptor.materialization_available) {
    return result_access_cost_evidence::kRouteStreamToImageGpuPrimaryNoCpuSidecarMaterializer;
  }
  return result_access_cost_evidence::kRouteStreamAccessUnsupported;
}

const char* gpu_materializer_evidence_route_for_posture(const SharedStreamResultData& data) {
  if (!data) {
    return result_access_cost_evidence::kRouteStreamAccessUnsupported;
  }
  return data->access_posture.has_retained_cpu_payload
      ? result_access_cost_evidence::kRouteStreamToImageGpuPrimaryCpuSidecarMaterializer
      : result_access_cost_evidence::kRouteStreamToImageGpuPrimaryNoCpuSidecarMaterializer;
}

const char* display_view_evidence_route(const SharedStreamResultData& data) {
  if (!data) {
    return result_access_cost_evidence::kRouteStreamAccessUnsupported;
  }
  if (data->payload_kind == ResultPayloadKind::GPU_SURFACE && data->retained_gpu_backing) {
    return result_access_cost_evidence::kRouteStreamDisplayViewRetainedGpuBacking;
  }
  return result_access_cost_evidence::kRouteStreamDisplayViewCpuLiveDisplayView;
}

struct LiveCpuDisplayViewEntry final {
  godot::Ref<godot::ImageTexture> texture;
  godot::Ref<godot::Image> image;
  uint64_t last_capture_timestamp_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

bool ensure_live_cpu_image_storage(
    LiveCpuDisplayViewEntry& entry,
    uint32_t width,
    uint32_t height) {
  if (width == 0 || height == 0) {
    return false;
  }
  const bool need_recreate =
      entry.image.is_null() ||
      entry.width != width ||
      entry.height != height;
  if (!need_recreate) {
    return true;
  }
  entry.image = godot::Image::create_empty(
      static_cast<int32_t>(width),
      static_cast<int32_t>(height),
      false,
      godot::Image::FORMAT_RGBA8);
  return entry.image.is_valid();
}

bool write_live_cpu_rgba_pixels(
    LiveCpuDisplayViewEntry& entry,
    const SharedStreamResultData& data,
    uint32_t width,
    uint32_t height) {
  if (!data || entry.image.is_null()) {
    return false;
  }
  const size_t required = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  if (data->payload.size_bytes() < required) {
    return false;
  }
  uint8_t* dst = entry.image->ptrw();
  if (!dst) {
    return false;
  }
  const uint8_t* src = data->payload.data();
  if (!src) {
    return false;
  }
  if (data->payload.format_fourcc == FOURCC_RGBA) {
    std::memcpy(dst, src, required);
    return true;
  }
  if (data->payload.format_fourcc == FOURCC_BGRA) {
    for (size_t i = 0; i + 3 < required; i += 4) {
      dst[i] = src[i + 2];
      dst[i + 1] = src[i + 1];
      dst[i + 2] = src[i];
      dst[i + 3] = 255;
    }
    return true;
  }
  return false;
}

std::mutex g_live_cpu_display_views_mutex;
std::map<uint64_t, LiveCpuDisplayViewEntry> g_live_cpu_display_views;
constexpr const char* kDisplayDemandTokenMetaKey = "__cambang_display_demand_token";

void attach_display_demand_token(const godot::Ref<godot::Texture2D>& texture, uint64_t stream_id, const char* path_kind) {
  if (texture.is_null() || stream_id == 0) {
    return;
  }
  godot::Ref<DisplayDemandToken> token;
  token.instantiate();
  if (token.is_null()) {
    return;
  }
  const bool gpu_display_view = path_kind && std::strcmp(path_kind, "retained_gpu_backing") == 0;
  token->init(stream_id, gpu_display_view);
  texture->set_meta(godot::StringName(kDisplayDemandTokenMetaKey), token);
  if (display_demand_trace_enabled()) {
    const uint64_t tex_id = texture->get_instance_id();
    godot::UtilityFunctions::print("[CamBANG][DemandTrace] token_attach stream_id=", static_cast<uint64_t>(stream_id),
                                   " token_ptr=", static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(token.ptr())),
                                   " texture_id=", static_cast<uint64_t>(tex_id),
                                   " path=", path_kind);
  }
}

bool refresh_live_cpu_display_view_entry(
    LiveCpuDisplayViewEntry& entry,
    const SharedStreamResultData& data) {
  if (!data || data->stream_id == 0 || !has_current_retained_cpu_payload(data)) {
    return false;
  }
  if (entry.last_capture_timestamp_ns == data->capture_timestamp_ns &&
      entry.width == data->payload.width &&
      entry.height == data->payload.height &&
      entry.texture.is_valid()) {
    return true;
  }
  const uint32_t width = data->payload.width;
  const uint32_t height = data->payload.height;
  if (!ensure_live_cpu_image_storage(entry, width, height) ||
      !write_live_cpu_rgba_pixels(entry, data, width, height)) {
    return false;
  }

  const bool need_recreate =
      entry.texture.is_null() ||
      entry.width != width ||
      entry.height != height;
  if (need_recreate) {
    entry.texture = godot::ImageTexture::create_from_image(entry.image);
    if (entry.texture.is_null()) {
      return false;
    }
  } else {
    entry.texture->update(entry.image);
  }

  entry.last_capture_timestamp_ns = data->capture_timestamp_ns;
  entry.width = width;
  entry.height = height;
  return true;
}

godot::Ref<godot::Texture2D> ensure_live_cpu_display_view(const SharedStreamResultData& data) {
  if (!data || data->stream_id == 0 || !has_current_retained_cpu_payload(data)) {
    return {};
  }
  std::lock_guard<std::mutex> lock(g_live_cpu_display_views_mutex);
  LiveCpuDisplayViewEntry& entry = g_live_cpu_display_views[data->stream_id];
  if (!refresh_live_cpu_display_view_entry(entry, data)) {
    return {};
  }
  return entry.texture;
}

godot::Ref<godot::Texture2D> make_ephemeral_cpu_display_view(const SharedStreamResultData& data) {
  if (!data || data->stream_id == 0 || !has_current_retained_cpu_payload(data)) {
    return {};
  }
  LiveCpuDisplayViewEntry entry;
  if (!refresh_live_cpu_display_view_entry(entry, data)) {
    return {};
  }
  return entry.texture;
}

} // namespace

uint32_t CamBANGStreamResult::get_width() const { return data_ ? data_->image_width : 0; }
uint32_t CamBANGStreamResult::get_height() const { return data_ ? data_->image_height : 0; }
uint32_t CamBANGStreamResult::get_format() const { return data_ ? data_->image_format_fourcc : 0; }
int CamBANGStreamResult::get_payload_kind() const {
  return data_ ? static_cast<int>(data_->payload_kind) : static_cast<int>(ResultPayloadKind::CPU_PACKED);
}
uint64_t CamBANGStreamResult::get_capture_timestamp() const { return data_ ? data_->capture_timestamp_ns : 0; }
uint64_t CamBANGStreamResult::get_stream_id() const { return data_ ? data_->stream_id : 0; }
uint64_t CamBANGStreamResult::get_device_instance_id() const { return data_ ? data_->device_instance_id : 0; }
int CamBANGStreamResult::get_intent() const { return data_ ? static_cast<int>(data_->intent) : 0; }

bool CamBANGStreamResult::has_image_properties() const { return data_ && data_->facts.has_image_properties; }
bool CamBANGStreamResult::has_capture_attributes() const { return data_ && data_->facts.has_capture_attributes; }
bool CamBANGStreamResult::has_location_attributes() const { return data_ && data_->facts.has_location_attributes; }
bool CamBANGStreamResult::has_optical_calibration() const { return data_ && data_->facts.has_optical_calibration; }

godot::Dictionary CamBANGStreamResult::get_image_properties() const {
  return has_image_properties() ? to_dict(data_->facts.image_properties) : godot::Dictionary();
}
godot::Dictionary CamBANGStreamResult::get_capture_attributes() const {
  return has_capture_attributes() ? to_dict(data_->facts.capture_attributes) : godot::Dictionary();
}
godot::Dictionary CamBANGStreamResult::get_location_attributes() const {
  return has_location_attributes() ? to_dict(data_->facts.location_attributes) : godot::Dictionary();
}
godot::Dictionary CamBANGStreamResult::get_optical_calibration() const {
  return has_optical_calibration() ? to_dict(data_->facts.optical_calibration) : godot::Dictionary();
}

godot::Dictionary CamBANGStreamResult::get_image_properties_provenance() const {
  return has_image_properties() ? to_dict(data_->facts.image_properties_provenance) : godot::Dictionary();
}
godot::Dictionary CamBANGStreamResult::get_capture_attributes_provenance() const {
  return has_capture_attributes() ? to_dict(data_->facts.capture_attributes_provenance) : godot::Dictionary();
}
godot::Dictionary CamBANGStreamResult::get_location_attributes_provenance() const {
  return has_location_attributes() ? to_dict(data_->facts.location_attributes_provenance) : godot::Dictionary();
}
godot::Dictionary CamBANGStreamResult::get_optical_calibration_provenance() const {
  return has_optical_calibration() ? to_dict(data_->facts.optical_calibration_provenance) : godot::Dictionary();
}

int CamBANGStreamResult::can_get_display_view() const {
  if (!data_) {
    return CAPABILITY_UNSUPPORTED;
  }
  return static_cast<int>(resolve_result_access_classification(
      data_->retained_access_truth.display_view,
      data_->access_classification,
      CoreResultAccessOperation::DISPLAY_VIEW));
}

int CamBANGStreamResult::can_to_image() const {
  if (!data_) {
    return CAPABILITY_UNSUPPORTED;
  }
  return static_cast<int>(resolve_result_access_classification(
      data_->retained_access_truth.to_image,
      data_->access_classification,
      CoreResultAccessOperation::TO_IMAGE));
}

int CamBANGStreamResult::get_display_view_path_kind() const {
  if (!data_) {
    return DISPLAY_PATH_NONE;
  }
  if (data_->payload_kind == ResultPayloadKind::GPU_SURFACE && data_->retained_gpu_backing) {
    return DISPLAY_PATH_RETAINED_GPU_BACKING;
  }
  if (has_current_retained_cpu_payload(data_)) {
    return DISPLAY_PATH_STREAM_LIVE_CPU_DISPLAY_VIEW;
  }
  return DISPLAY_PATH_NONE;
}

godot::Variant perform_stream_display_view_access(
    const SharedStreamResultData& data,
    bool mark_display_demand,
    bool attach_display_demand_tokens,
    bool persistent_cpu_display_view) {
  if (!data) {
    const uint64_t begin_ns = result_access_now_ns();
    result_access_cost_evidence::record_stream_access(
        result_access_cost_evidence::kRouteStreamAccessUnsupported,
        data,
        result_access_now_ns() - begin_ns,
        false,
        ResultCapability::UNSUPPORTED);
    return godot::Variant();
  }
  const char* evidence_route = display_view_evidence_route(data);
  const ResultCapability reported_capability = resolve_result_access_classification(
      data->retained_access_truth.display_view,
      data->access_classification,
      CoreResultAccessOperation::DISPLAY_VIEW);
  const uint64_t begin_ns = result_access_now_ns();
  godot::Variant result;
  if (mark_display_demand && data->stream_id != 0) {
    if (CamBANGServer* server = CamBANGServer::get_singleton()) {
      server->mark_stream_display_demand(data->stream_id);
    }
  }
  if (data->payload_kind == ResultPayloadKind::GPU_SURFACE) {
    if (data->retained_gpu_backing) {
      godot::Ref<godot::Texture2D> retained = godot_gpu_display_get_texture_by_descriptor(
          data->retained_gpu_backing_descriptor,
          data->retained_gpu_backing);
      if (retained.is_valid()) {
        if (attach_display_demand_tokens) {
          attach_display_demand_token(retained, data->stream_id, "retained_gpu_backing");
        }
        trace_stream_display_path("retained_gpu_backing");
        result = retained;
        result_access_cost_evidence::record_stream_access(
            evidence_route,
            data,
            result_access_now_ns() - begin_ns,
            true,
            reported_capability);
        return result;
      }
    }
    result_access_cost_evidence::record_stream_access(
        evidence_route,
        data,
        result_access_now_ns() - begin_ns,
        false,
        reported_capability);
    return result;
  }
  godot::Ref<godot::Texture2D> live_cpu = persistent_cpu_display_view
      ? ensure_live_cpu_display_view(data)
      : make_ephemeral_cpu_display_view(data);
  if (live_cpu.is_valid()) {
    if (attach_display_demand_tokens) {
      attach_display_demand_token(live_cpu, data->stream_id, "stream_live_cpu_display_view");
    }
    trace_stream_display_path("stream_live_cpu_display_view");
    result = live_cpu;
    result_access_cost_evidence::record_stream_access(
        evidence_route,
        data,
        result_access_now_ns() - begin_ns,
        true,
        reported_capability);
    return result;
  }
  result_access_cost_evidence::record_stream_access(
      evidence_route,
      data,
      result_access_now_ns() - begin_ns,
      false,
      reported_capability);
  return result;
}


godot::Ref<godot::Image> perform_stream_to_image_access(const SharedStreamResultData& data) {
  if (!data) {
    const uint64_t begin_ns = result_access_now_ns();
    godot::Ref<godot::Image> image;
    result_access_cost_evidence::record_stream_access(
        result_access_cost_evidence::kRouteStreamAccessUnsupported,
        data,
        result_access_now_ns() - begin_ns,
        false,
        ResultCapability::UNSUPPORTED);
    return image;
  }
  const char* evidence_route = to_image_evidence_route(data);
  const ResultCapability reported_capability = resolve_result_access_classification(
      data->retained_access_truth.to_image,
      data->access_classification,
      CoreResultAccessOperation::TO_IMAGE);
  const uint64_t begin_ns = result_access_now_ns();
  godot::Ref<godot::Image> image;
  if (has_current_retained_cpu_payload(data)) {
    image = payload_to_image(data->payload);
    result_access_cost_evidence::record_stream_access(
        evidence_route,
        data,
        result_access_now_ns() - begin_ns,
        image.is_valid(),
        reported_capability);
    return image;
  }
  if (data->payload_kind == ResultPayloadKind::GPU_SURFACE && data->retained_gpu_backing) {
    image = godot_gpu_display_materialize_to_image(
        data->retained_gpu_backing_descriptor,
        data->retained_gpu_backing);
    result_access_cost_evidence::record_stream_access(
        evidence_route,
        data,
        result_access_now_ns() - begin_ns,
        image.is_valid(),
        reported_capability);
    return image;
  }
  result_access_cost_evidence::record_stream_access(
      evidence_route,
      data,
      result_access_now_ns() - begin_ns,
      false,
      reported_capability);
  return image;
}

godot::Ref<godot::Image> perform_stream_to_image_cpu_payload_access(const SharedStreamResultData& data) {
  const uint64_t begin_ns = result_access_now_ns();
  godot::Ref<godot::Image> image;
  if (!data || !has_current_retained_cpu_payload(data)) {
    result_access_cost_evidence::record_stream_access(
        result_access_cost_evidence::kRouteStreamAccessUnsupported,
        data,
        result_access_now_ns() - begin_ns,
        false,
        data ? data->retained_access_truth.to_image : ResultCapability::UNSUPPORTED);
    return image;
  }
  const char* route = data->payload_kind == ResultPayloadKind::GPU_SURFACE
      ? result_access_cost_evidence::kRouteStreamToImageGpuPrimaryCpuSidecar
      : result_access_cost_evidence::kRouteStreamToImageCpuPacked;
  image = payload_to_image(data->payload);
  result_access_cost_evidence::record_stream_access(
      route,
      data,
      result_access_now_ns() - begin_ns,
      image.is_valid(),
      data->retained_access_truth.to_image);
  return image;
}

godot::Ref<godot::Image> perform_stream_to_image_gpu_materializer_access(const SharedStreamResultData& data) {
  const uint64_t begin_ns = result_access_now_ns();
  godot::Ref<godot::Image> image;
  const char* route = gpu_materializer_evidence_route_for_posture(data);
  if (!data ||
      data->payload_kind != ResultPayloadKind::GPU_SURFACE ||
      !data->retained_gpu_backing ||
      !data->retained_gpu_backing_descriptor.valid ||
      !data->retained_gpu_backing_descriptor.materialization_available) {
    result_access_cost_evidence::record_stream_access(
        result_access_cost_evidence::kRouteStreamAccessUnsupported,
        data,
        result_access_now_ns() - begin_ns,
        false,
        data ? data->retained_access_truth.to_image : ResultCapability::UNSUPPORTED);
    return image;
  }
  image = godot_gpu_display_materialize_to_image(
      data->retained_gpu_backing_descriptor,
      data->retained_gpu_backing);
  result_access_cost_evidence::record_stream_access(
      route,
      data,
      result_access_now_ns() - begin_ns,
      image.is_valid(),
      data->retained_access_truth.to_image);
  return image;
}

godot::Variant CamBANGStreamResult::get_display_view() const {
  return perform_stream_display_view_access(
      data_,
      /*mark_display_demand=*/true,
      /*attach_display_demand_tokens=*/true,
      /*persistent_cpu_display_view=*/true);
}

godot::Ref<godot::Image> CamBANGStreamResult::to_image() const {
  return perform_stream_to_image_access(data_);
}

godot::Variant CamBANGStreamResult::calibrate_display_view_for_retained_access(const SharedStreamResultData& data) {
  return perform_stream_display_view_access(
      data,
      /*mark_display_demand=*/false,
      /*attach_display_demand_tokens=*/false,
      /*persistent_cpu_display_view=*/false);
}

godot::Ref<godot::Image> CamBANGStreamResult::calibrate_to_image_for_retained_access(const SharedStreamResultData& data) {
  return perform_stream_to_image_access(data);
}

godot::Ref<godot::Image> CamBANGStreamResult::calibrate_to_image_cpu_payload_for_retained_access(
    const SharedStreamResultData& data) {
  return perform_stream_to_image_cpu_payload_access(data);
}

godot::Ref<godot::Image> CamBANGStreamResult::calibrate_to_image_gpu_materializer_for_retained_access(
    const SharedStreamResultData& data) {
  return perform_stream_to_image_gpu_materializer_access(data);
}

void CamBANGStreamResult::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_width"), &CamBANGStreamResult::get_width);
  godot::ClassDB::bind_method(godot::D_METHOD("get_height"), &CamBANGStreamResult::get_height);
  godot::ClassDB::bind_method(godot::D_METHOD("get_format"), &CamBANGStreamResult::get_format);
  godot::ClassDB::bind_method(godot::D_METHOD("get_payload_kind"), &CamBANGStreamResult::get_payload_kind);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_timestamp"), &CamBANGStreamResult::get_capture_timestamp);
  godot::ClassDB::bind_method(godot::D_METHOD("get_stream_id"), &CamBANGStreamResult::get_stream_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_device_instance_id"), &CamBANGStreamResult::get_device_instance_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_intent"), &CamBANGStreamResult::get_intent);

  godot::ClassDB::bind_method(godot::D_METHOD("has_image_properties"), &CamBANGStreamResult::has_image_properties);
  godot::ClassDB::bind_method(godot::D_METHOD("has_capture_attributes"), &CamBANGStreamResult::has_capture_attributes);
  godot::ClassDB::bind_method(godot::D_METHOD("has_location_attributes"), &CamBANGStreamResult::has_location_attributes);
  godot::ClassDB::bind_method(godot::D_METHOD("has_optical_calibration"), &CamBANGStreamResult::has_optical_calibration);

  godot::ClassDB::bind_method(godot::D_METHOD("get_image_properties"), &CamBANGStreamResult::get_image_properties);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_attributes"), &CamBANGStreamResult::get_capture_attributes);
  godot::ClassDB::bind_method(godot::D_METHOD("get_location_attributes"), &CamBANGStreamResult::get_location_attributes);
  godot::ClassDB::bind_method(godot::D_METHOD("get_optical_calibration"), &CamBANGStreamResult::get_optical_calibration);

  godot::ClassDB::bind_method(godot::D_METHOD("get_image_properties_provenance"), &CamBANGStreamResult::get_image_properties_provenance);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_attributes_provenance"), &CamBANGStreamResult::get_capture_attributes_provenance);
  godot::ClassDB::bind_method(godot::D_METHOD("get_location_attributes_provenance"), &CamBANGStreamResult::get_location_attributes_provenance);
  godot::ClassDB::bind_method(godot::D_METHOD("get_optical_calibration_provenance"), &CamBANGStreamResult::get_optical_calibration_provenance);

  godot::ClassDB::bind_method(godot::D_METHOD("can_get_display_view"), &CamBANGStreamResult::can_get_display_view);
  godot::ClassDB::bind_method(godot::D_METHOD("can_to_image"), &CamBANGStreamResult::can_to_image);

  godot::ClassDB::bind_method(godot::D_METHOD("get_display_view_path_kind"), &CamBANGStreamResult::get_display_view_path_kind);
  godot::ClassDB::bind_method(godot::D_METHOD("get_display_view"), &CamBANGStreamResult::get_display_view);
  godot::ClassDB::bind_method(godot::D_METHOD("to_image"), &CamBANGStreamResult::to_image);

  BIND_CONSTANT(CAPABILITY_READY);
  BIND_CONSTANT(CAPABILITY_CHEAP);
  BIND_CONSTANT(CAPABILITY_EXPENSIVE);
  BIND_CONSTANT(CAPABILITY_UNSUPPORTED);

  BIND_CONSTANT(DISPLAY_PATH_NONE);
  BIND_CONSTANT(DISPLAY_PATH_RETAINED_GPU_BACKING);
  BIND_CONSTANT(DISPLAY_PATH_STREAM_LIVE_CPU_DISPLAY_VIEW);
}

void CamBANGStreamResult::refresh_live_stream_cpu_display_views(const CoreRuntime& runtime) {
  std::vector<uint64_t> stream_ids;
  {
    std::lock_guard<std::mutex> lock(g_live_cpu_display_views_mutex);
    stream_ids.reserve(g_live_cpu_display_views.size());
    for (const auto& kv : g_live_cpu_display_views) {
      stream_ids.push_back(kv.first);
    }
  }

  const auto refresh_begin = std::chrono::steady_clock::now();
  uint32_t removed_count = 0;
  uint32_t refreshed_count = 0;
  uint32_t updated_count = 0;
  for (uint64_t stream_id : stream_ids) {
    SharedStreamResultData data = runtime.get_latest_stream_result(stream_id);
    if (!data) {
      std::lock_guard<std::mutex> lock(g_live_cpu_display_views_mutex);
      removed_count += static_cast<uint32_t>(g_live_cpu_display_views.erase(stream_id));
      continue;
    }
    std::lock_guard<std::mutex> lock(g_live_cpu_display_views_mutex);
    auto it = g_live_cpu_display_views.find(stream_id);
    if (it == g_live_cpu_display_views.end()) {
      continue;
    }
    if (data->payload_kind == ResultPayloadKind::CPU_PACKED && has_current_retained_cpu_payload(data)) {
      const uint64_t prior_capture_timestamp_ns = it->second.last_capture_timestamp_ns;
      if (refresh_live_cpu_display_view_entry(it->second, data)) {
        ++refreshed_count;
        if (it->second.last_capture_timestamp_ns != prior_capture_timestamp_ns) {
          ++updated_count;
        }
      }
    }
  }
  if (display_demand_trace_enabled()) {
    const uint64_t refresh_elapsed_ns = elapsed_ns_since(refresh_begin);
    if (refreshed_count != 0 || removed_count != 0 || refresh_elapsed_ns >= 1'000'000ull) {
      godot::UtilityFunctions::print(
          "[CamBANG][DemandTrace] cpu_display_refresh tracked=",
          static_cast<uint64_t>(stream_ids.size()),
          " refreshed=",
          static_cast<uint64_t>(refreshed_count),
          " updated=",
          static_cast<uint64_t>(updated_count),
          " removed=",
          static_cast<uint64_t>(removed_count),
          " elapsed_us=",
          static_cast<uint64_t>(refresh_elapsed_ns / 1000ull));
    }
  }
}

void CamBANGStreamResult::remove_live_stream_cpu_display_view(uint64_t stream_id) {
  if (stream_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_live_cpu_display_views_mutex);
  g_live_cpu_display_views.erase(stream_id);
}

void CamBANGStreamResult::clear_live_stream_cpu_display_views() {
  std::lock_guard<std::mutex> lock(g_live_cpu_display_views_mutex);
  g_live_cpu_display_views.clear();
}

} // namespace cambang

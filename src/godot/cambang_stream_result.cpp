#include "godot/cambang_stream_result.h"

#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "core/core_runtime.h"
#include "godot/cambang_result_convert.h"
#include "godot/synthetic_gpu_backing_bridge.h"

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

bool has_retained_cpu_payload(const SharedStreamResultData& data) {
  if (!data) {
    return false;
  }
  return data->payload.width != 0 &&
         data->payload.height != 0 &&
         !data->payload.bytes.empty();
}

struct LiveCpuDisplayViewEntry final {
  godot::Ref<godot::ImageTexture> texture;
  uint64_t last_capture_timestamp_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

std::mutex g_live_cpu_display_views_mutex;
std::map<uint64_t, LiveCpuDisplayViewEntry> g_live_cpu_display_views;

bool refresh_live_cpu_display_view_entry(
    LiveCpuDisplayViewEntry& entry,
    const SharedStreamResultData& data) {
  if (!data || data->stream_id == 0 || !has_retained_cpu_payload(data)) {
    return false;
  }
  if (entry.last_capture_timestamp_ns == data->capture_timestamp_ns &&
      entry.width == data->payload.width &&
      entry.height == data->payload.height &&
      entry.texture.is_valid()) {
    return true;
  }

  godot::Ref<godot::Image> image = payload_to_image(data->payload);
  if (image.is_null()) {
    return false;
  }

  const bool need_recreate =
      entry.texture.is_null() ||
      entry.width != data->payload.width ||
      entry.height != data->payload.height;
  if (need_recreate) {
    entry.texture = godot::ImageTexture::create_from_image(image);
    if (entry.texture.is_null()) {
      return false;
    }
  } else {
    entry.texture->update(image);
  }

  entry.last_capture_timestamp_ns = data->capture_timestamp_ns;
  entry.width = data->payload.width;
  entry.height = data->payload.height;
  return true;
}

godot::Ref<godot::Texture2D> ensure_live_cpu_display_view(const SharedStreamResultData& data) {
  if (!data || data->stream_id == 0 || !has_retained_cpu_payload(data)) {
    return {};
  }
  std::lock_guard<std::mutex> lock(g_live_cpu_display_views_mutex);
  LiveCpuDisplayViewEntry& entry = g_live_cpu_display_views[data->stream_id];
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

  // Display-view capability is classified by the currently implemented display
  // path, not by payload-kind family alone.
  //
  // READY:
  //   The retained artifact is already directly suitable for display-view
  //   access. In the current slice, this is GPU_SURFACE with retained GPU
  //   backing.
  //
  // CHEAP:
  //   Only explicitly whitelisted payload/path combinations known to require
  //   modest additional work in the current implementation. In the current
  //   slice, CPU_PACKED is cheap because get_display_view() uses a stream-owned
  //   live ImageTexture that is refreshed from current retained stream state
  //   without a broad conversion/decode/repack pipeline.
  //
  // EXPENSIVE:
  //   A supported display-view path exists, but requires substantial extra work
  //   such as materialization, conversion, decode, repack, or comparable
  //   processing. New payload kinds should default here only once such a path
  //   is actually implemented and proven.
  //
  // UNSUPPORTED:
  //   No current supported display-view path exists.
  //
  // New payload kinds must not inherit CHEAP by default; they must earn their
  // classification from the actual implemented display path.
  switch (data_->payload_kind) {
    case ResultPayloadKind::GPU_SURFACE:
      return data_->retained_gpu_backing ? CAPABILITY_READY : CAPABILITY_UNSUPPORTED;
    case ResultPayloadKind::CPU_PACKED:
      return CAPABILITY_CHEAP;
    case ResultPayloadKind::CPU_PLANAR:
    case ResultPayloadKind::ENCODED_IMAGE:
    case ResultPayloadKind::RAW_IMAGE:
    default:
      return CAPABILITY_UNSUPPORTED;
  }
}

int CamBANGStreamResult::can_to_image() const {
  if (!data_) {
    return CAPABILITY_UNSUPPORTED;
  }
  if (has_retained_cpu_payload(data_)) {
    return CAPABILITY_CHEAP;
  }
  if (data_->payload_kind == ResultPayloadKind::GPU_SURFACE && data_->retained_gpu_backing) {
    return synthetic_gpu_backing_can_materialize_to_image(data_->retained_gpu_backing)
        ? CAPABILITY_EXPENSIVE
        : CAPABILITY_UNSUPPORTED;
  }
  return CAPABILITY_UNSUPPORTED;
}

godot::Variant CamBANGStreamResult::get_display_view() const {
  if (!data_) {
    return godot::Variant();
  }
  if (data_->payload_kind == ResultPayloadKind::GPU_SURFACE) {
    if (data_->retained_gpu_backing) {
      godot::Ref<godot::Texture2D> retained = synthetic_gpu_backing_display_texture(data_->retained_gpu_backing);
      if (retained.is_valid()) {
        trace_stream_display_path("retained_gpu_backing");
        return retained;
      }
    }
    return godot::Variant();
  }
  godot::Ref<godot::Texture2D> live_cpu = ensure_live_cpu_display_view(data_);
  if (live_cpu.is_valid()) {
    trace_stream_display_path("stream_live_cpu_display_view");
    return live_cpu;
  }
  return godot::Variant();
}

godot::Ref<godot::Image> CamBANGStreamResult::to_image() const {
  if (!data_) {
    return godot::Ref<godot::Image>();
  }
  if (has_retained_cpu_payload(data_)) {
    return payload_to_image(data_->payload);
  }
  if (data_->payload_kind == ResultPayloadKind::GPU_SURFACE && data_->retained_gpu_backing) {
    return synthetic_gpu_backing_materialize_to_image(data_->retained_gpu_backing);
  }
  return godot::Ref<godot::Image>();
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

  godot::ClassDB::bind_method(godot::D_METHOD("get_display_view"), &CamBANGStreamResult::get_display_view);
  godot::ClassDB::bind_method(godot::D_METHOD("to_image"), &CamBANGStreamResult::to_image);

  BIND_CONSTANT(CAPABILITY_READY);
  BIND_CONSTANT(CAPABILITY_CHEAP);
  BIND_CONSTANT(CAPABILITY_EXPENSIVE);
  BIND_CONSTANT(CAPABILITY_UNSUPPORTED);
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

  for (uint64_t stream_id : stream_ids) {
    SharedStreamResultData data = runtime.get_latest_stream_result(stream_id);
    if (!data || data->payload_kind != ResultPayloadKind::CPU_PACKED || !has_retained_cpu_payload(data)) {
      continue;
    }
    std::lock_guard<std::mutex> lock(g_live_cpu_display_views_mutex);
    auto it = g_live_cpu_display_views.find(stream_id);
    if (it == g_live_cpu_display_views.end()) {
      continue;
    }
    (void)refresh_live_cpu_display_view_entry(it->second, data);
  }
}

void CamBANGStreamResult::clear_live_stream_cpu_display_views() {
  std::lock_guard<std::mutex> lock(g_live_cpu_display_views_mutex);
  g_live_cpu_display_views.clear();
}

} // namespace cambang

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
#include <godot_cpp/classes/rendering_server.hpp>
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
  if (!data || data->payload_retained_frame_id != data->retained_frame_id) {
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
  std::mutex mutex;
  std::shared_ptr<SharedLiveCpuTextureRidState> rid_state;
  godot::Ref<godot::Image> image;
  uint64_t last_retained_frame_id = 0;
  uint64_t next_refresh_after_ns = 0;
  uint64_t last_refresh_elapsed_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

constexpr uint64_t kLiveCpuDisplayRefreshIntervalNs = 66'666'667ull;
constexpr uint64_t kLiveCpuDisplayRefreshBudgetNs = 4'000'000ull;
constexpr uint64_t kLiveCpuDisplayRefreshBackoffMaxNs = 500'000'000ull;

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
std::map<uint64_t, std::shared_ptr<LiveCpuDisplayViewEntry>> g_live_cpu_display_views;

struct LiveCpuDisplayMetrics {
  uint64_t refresh_attempts = 0;
  uint64_t refresh_updated = 0;
  uint64_t live_refresh_attempts = 0;
  uint64_t live_refresh_updated = 0;
  uint64_t live_total_ns = 0;
  uint64_t live_update_ns = 0;
  uint64_t ephemeral_refresh_attempts = 0;
  uint64_t ephemeral_refresh_updated = 0;
  uint64_t ephemeral_total_ns = 0;
  uint64_t ephemeral_update_ns = 0;
  uint64_t skipped_unchanged = 0;
  uint64_t skipped_due_budget = 0;
  uint64_t skipped_due_no_demand = 0;
  uint64_t removed = 0;
  uint64_t total_ns = 0;
  uint64_t update_ns = 0;
};

std::mutex g_live_cpu_display_metrics_mutex;
LiveCpuDisplayMetrics g_live_cpu_display_metrics;

void note_live_cpu_display_refresh_attempt(
    uint64_t total_ns,
    uint64_t update_ns,
    bool updated,
    bool persistent_live_display_view) {
  std::lock_guard<std::mutex> lock(g_live_cpu_display_metrics_mutex);
  ++g_live_cpu_display_metrics.refresh_attempts;
  if (updated) {
    ++g_live_cpu_display_metrics.refresh_updated;
  }
  if (persistent_live_display_view) {
    ++g_live_cpu_display_metrics.live_refresh_attempts;
    if (updated) {
      ++g_live_cpu_display_metrics.live_refresh_updated;
    }
    g_live_cpu_display_metrics.live_total_ns += total_ns;
    g_live_cpu_display_metrics.live_update_ns += update_ns;
  } else {
    ++g_live_cpu_display_metrics.ephemeral_refresh_attempts;
    if (updated) {
      ++g_live_cpu_display_metrics.ephemeral_refresh_updated;
    }
    g_live_cpu_display_metrics.ephemeral_total_ns += total_ns;
    g_live_cpu_display_metrics.ephemeral_update_ns += update_ns;
  }
  g_live_cpu_display_metrics.total_ns += total_ns;
  g_live_cpu_display_metrics.update_ns += update_ns;
}

void note_live_cpu_display_refresh_skip_unchanged() {
  std::lock_guard<std::mutex> lock(g_live_cpu_display_metrics_mutex);
  ++g_live_cpu_display_metrics.skipped_unchanged;
}

void note_live_cpu_display_refresh_skip_due_budget() {
  std::lock_guard<std::mutex> lock(g_live_cpu_display_metrics_mutex);
  ++g_live_cpu_display_metrics.skipped_due_budget;
}

void note_live_cpu_display_refresh_skip_due_no_demand() {
  std::lock_guard<std::mutex> lock(g_live_cpu_display_metrics_mutex);
  ++g_live_cpu_display_metrics.skipped_due_no_demand;
}

void note_live_cpu_display_refresh_removed(uint32_t removed_count) {
  if (removed_count == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_live_cpu_display_metrics_mutex);
  g_live_cpu_display_metrics.removed += removed_count;
}

godot::Dictionary snapshot_live_cpu_display_metrics() {
  std::lock_guard<std::mutex> lock(g_live_cpu_display_metrics_mutex);
  godot::Dictionary d;
  d["cpu_display_refresh_attempts"] =
      static_cast<uint64_t>(g_live_cpu_display_metrics.refresh_attempts);
  d["cpu_display_refresh_updated"] =
      static_cast<uint64_t>(g_live_cpu_display_metrics.refresh_updated);
  d["cpu_display_refresh_live_attempts"] =
      static_cast<uint64_t>(g_live_cpu_display_metrics.live_refresh_attempts);
  d["cpu_display_refresh_live_updated"] =
      static_cast<uint64_t>(g_live_cpu_display_metrics.live_refresh_updated);
  d["cpu_display_refresh_live_total_ms"] =
      static_cast<double>(g_live_cpu_display_metrics.live_total_ns) / 1'000'000.0;
  d["cpu_display_refresh_live_update_ms"] =
      static_cast<double>(g_live_cpu_display_metrics.live_update_ns) / 1'000'000.0;
  d["cpu_display_refresh_ephemeral_attempts"] =
      static_cast<uint64_t>(g_live_cpu_display_metrics.ephemeral_refresh_attempts);
  d["cpu_display_refresh_ephemeral_updated"] =
      static_cast<uint64_t>(g_live_cpu_display_metrics.ephemeral_refresh_updated);
  d["cpu_display_refresh_ephemeral_total_ms"] =
      static_cast<double>(g_live_cpu_display_metrics.ephemeral_total_ns) / 1'000'000.0;
  d["cpu_display_refresh_ephemeral_update_ms"] =
      static_cast<double>(g_live_cpu_display_metrics.ephemeral_update_ns) / 1'000'000.0;
  d["cpu_display_refresh_skipped_unchanged"] =
      static_cast<uint64_t>(g_live_cpu_display_metrics.skipped_unchanged);
  d["cpu_display_refresh_skipped_due_budget"] =
      static_cast<uint64_t>(g_live_cpu_display_metrics.skipped_due_budget);
  d["cpu_display_refresh_skipped_due_no_demand"] =
      static_cast<uint64_t>(g_live_cpu_display_metrics.skipped_due_no_demand);
  d["cpu_display_refresh_removed"] =
      static_cast<uint64_t>(g_live_cpu_display_metrics.removed);
  d["cpu_display_refresh_total_ms"] =
      static_cast<double>(g_live_cpu_display_metrics.total_ns) / 1'000'000.0;
  d["cpu_display_refresh_update_ms"] =
      static_cast<double>(g_live_cpu_display_metrics.update_ns) / 1'000'000.0;
  return d;
}

void clear_live_cpu_display_metrics() {
  std::lock_guard<std::mutex> lock(g_live_cpu_display_metrics_mutex);
  g_live_cpu_display_metrics = LiveCpuDisplayMetrics{};
}

bool refresh_live_cpu_display_view_entry(
    LiveCpuDisplayViewEntry& entry,
    const SharedStreamResultData& data,
    bool force_refresh,
    bool demand_active,
    bool persistent_live_display_view) {
  const auto total_begin = std::chrono::steady_clock::now();
  if (!data || data->stream_id == 0 || !has_current_retained_cpu_payload(data)) {
    return false;
  }
  const uint64_t now_ns = result_access_now_ns();
  const uint32_t width = data->payload.width;
  const uint32_t height = data->payload.height;
  uint32_t prior_width = 0;
  uint32_t prior_height = 0;
  {
    std::lock_guard<std::mutex> lock(entry.mutex);
    prior_width = entry.width;
    prior_height = entry.height;
    const bool unchanged =
        entry.last_retained_frame_id == data->retained_frame_id &&
        entry.width == width &&
        entry.height == height &&
        entry.rid_state &&
        entry.rid_state->snapshot_rid().is_valid();
    if (unchanged) {
      note_live_cpu_display_refresh_skip_unchanged();
      if (display_demand_trace_enabled()) {
        godot::UtilityFunctions::print(
            "[CamBANG][DemandTrace] cpu_display_refresh stream_id=",
            static_cast<uint64_t>(data->stream_id),
            " action=skipped_unchanged demand_active=",
            demand_active);
      }
      return true;
    }
    if (!force_refresh && now_ns < entry.next_refresh_after_ns) {
      note_live_cpu_display_refresh_skip_due_budget();
      if (display_demand_trace_enabled()) {
        godot::UtilityFunctions::print(
            "[CamBANG][DemandTrace] cpu_display_refresh stream_id=",
            static_cast<uint64_t>(data->stream_id),
            " action=skipped_due_budget demand_active=",
            demand_active,
            " retry_after_us=",
            static_cast<uint64_t>((entry.next_refresh_after_ns - now_ns) / 1000ull));
      }
      return true;
    }
  }

  godot::Ref<godot::Image> image;
  std::shared_ptr<SharedLiveCpuTextureRidState> rid_state;
  {
    std::lock_guard<std::mutex> lock(entry.mutex);
    image = entry.image;
    rid_state = entry.rid_state;
  }
  if (!rid_state) {
    rid_state = std::make_shared<SharedLiveCpuTextureRidState>();
  }

  LiveCpuDisplayViewEntry working_entry;
  working_entry.image = image;
  working_entry.width = width;
  working_entry.height = height;
  if (!ensure_live_cpu_image_storage(working_entry, width, height) ||
      !write_live_cpu_rgba_pixels(working_entry, data, width, height)) {
    return false;
  }

  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    return false;
  }
  const bool need_recreate =
      !rid_state->snapshot_rid().is_valid() ||
      prior_width != width ||
      prior_height != height;
  const auto refresh_begin = std::chrono::steady_clock::now();
  if (need_recreate) {
    const godot::RID texture_rid = rs->texture_2d_create(working_entry.image);
    if (!texture_rid.is_valid()) {
      return false;
    }
    rid_state->replace_rid(texture_rid);
  } else {
    const godot::RID texture_rid = rid_state->snapshot_rid();
    if (!texture_rid.is_valid()) {
      return false;
    }
    rs->texture_2d_update(texture_rid, working_entry.image, 0);
  }
  const uint64_t refresh_elapsed_ns = elapsed_ns_since(refresh_begin);
  uint64_t next_refresh_after_ns = now_ns + kLiveCpuDisplayRefreshIntervalNs;
  if (refresh_elapsed_ns > kLiveCpuDisplayRefreshBudgetNs) {
    const uint64_t doubled_refresh_ns = refresh_elapsed_ns * static_cast<uint64_t>(2);
    const uint64_t backoff_ns = std::min(doubled_refresh_ns, kLiveCpuDisplayRefreshBackoffMaxNs);
    next_refresh_after_ns = now_ns + backoff_ns;
  }

  {
    std::lock_guard<std::mutex> lock(entry.mutex);
    entry.image = working_entry.image;
    entry.rid_state = rid_state;
    entry.last_retained_frame_id = data->retained_frame_id;
    entry.last_refresh_elapsed_ns = refresh_elapsed_ns;
    entry.next_refresh_after_ns = next_refresh_after_ns;
    entry.width = width;
    entry.height = height;
  }
  notify_live_cpu_display_wrapper_refresh(data->stream_id, width, height);
  note_live_cpu_display_refresh_attempt(
      elapsed_ns_since(total_begin),
      refresh_elapsed_ns,
      true,
      persistent_live_display_view);
  if (display_demand_trace_enabled()) {
    godot::UtilityFunctions::print(
        "[CamBANG][DemandTrace] cpu_display_refresh stream_id=",
        static_cast<uint64_t>(data->stream_id),
        " action=updated demand_active=",
        demand_active,
        " elapsed_us=",
        static_cast<uint64_t>(refresh_elapsed_ns / 1000ull));
  }
  return true;
}

godot::Ref<godot::Texture2D> ensure_live_cpu_display_view(const SharedStreamResultData& data) {
  if (!data || data->stream_id == 0 || !has_current_retained_cpu_payload(data)) {
    return {};
  }
  std::shared_ptr<LiveCpuDisplayViewEntry> entry;
  {
    std::lock_guard<std::mutex> lock(g_live_cpu_display_views_mutex);
    auto& slot = g_live_cpu_display_views[data->stream_id];
    if (!slot) {
      slot = std::make_shared<LiveCpuDisplayViewEntry>();
    }
    entry = slot;
  }
  if (!entry || !refresh_live_cpu_display_view_entry(*entry, data, true, true, true)) {
    return {};
  }
  std::shared_ptr<SharedLiveCpuTextureRidState> rid_state;
  uint32_t width = 0;
  uint32_t height = 0;
  {
    std::lock_guard<std::mutex> lock(entry->mutex);
    rid_state = entry->rid_state;
    width = entry->width;
    height = entry->height;
  }
  if (!rid_state) {
    return {};
  }
  godot::Ref<LiveCpuDisplayTexture2D> texture;
  texture.instantiate();
  if (texture.is_null()) {
    return {};
  }
  texture->init(rid_state, data->stream_id, width, height, true);
  return texture;
}

godot::Ref<godot::Texture2D> make_ephemeral_cpu_display_view(const SharedStreamResultData& data) {
  if (!data || data->stream_id == 0 || !has_current_retained_cpu_payload(data)) {
    return {};
  }
  auto entry = std::make_shared<LiveCpuDisplayViewEntry>();
  if (!refresh_live_cpu_display_view_entry(*entry, data, true, false, false)) {
    return {};
  }
  std::shared_ptr<SharedLiveCpuTextureRidState> rid_state;
  {
    std::lock_guard<std::mutex> lock(entry->mutex);
    rid_state = entry->rid_state;
  }
  if (!rid_state) {
    return {};
  }
  godot::Ref<LiveCpuDisplayTexture2D> texture;
  texture.instantiate();
  if (texture.is_null()) {
    return {};
  }
  texture->init(rid_state, 0, data->payload.width, data->payload.height, false);
  return texture;
}

} // namespace

uint32_t CamBANGStreamResult::get_width() const { return data_ ? data_->image_width : 0; }
uint32_t CamBANGStreamResult::get_height() const { return data_ ? data_->image_height : 0; }
uint32_t CamBANGStreamResult::get_format() const { return data_ ? data_->image_format_fourcc : 0; }
int CamBANGStreamResult::get_payload_kind() const {
  return data_ ? static_cast<int>(data_->payload_kind) : static_cast<int>(ResultPayloadKind::CPU_PACKED);
}
uint64_t CamBANGStreamResult::get_stream_id() const { return data_ ? data_->stream_id : 0; }
uint64_t CamBANGStreamResult::get_device_instance_id() const { return data_ ? data_->device_instance_id : 0; }
int CamBANGStreamResult::get_intent() const { return data_ ? static_cast<int>(data_->intent) : 0; }
godot::Dictionary CamBANGStreamResult::get_camera_facts() const {
  if (!data_) {
    return godot::Dictionary();
  }
  godot::Dictionary out;
  add_acquisition_timing_camera_fact(out, data_->image_facts.acquisition_timing);
  return out;
}

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
      /*persistent_cpu_display_view=*/true);
}

godot::Ref<godot::Image> CamBANGStreamResult::to_image() const {
  return perform_stream_to_image_access(data_);
}

godot::Variant CamBANGStreamResult::calibrate_display_view_for_retained_access(const SharedStreamResultData& data) {
  return perform_stream_display_view_access(
      data,
      /*mark_display_demand=*/false,
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
  godot::ClassDB::bind_method(godot::D_METHOD("get_stream_id"), &CamBANGStreamResult::get_stream_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_device_instance_id"), &CamBANGStreamResult::get_device_instance_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_intent"), &CamBANGStreamResult::get_intent);
  godot::ClassDB::bind_method(godot::D_METHOD("get_camera_facts"), &CamBANGStreamResult::get_camera_facts);

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
  struct RefreshCandidate final {
    uint64_t stream_id = 0;
    std::shared_ptr<LiveCpuDisplayViewEntry> entry;
  };

  std::vector<RefreshCandidate> candidates;
  {
    std::lock_guard<std::mutex> lock(g_live_cpu_display_views_mutex);
    candidates.reserve(g_live_cpu_display_views.size());
    for (const auto& kv : g_live_cpu_display_views) {
      candidates.push_back(RefreshCandidate{kv.first, kv.second});
    }
  }

  const auto refresh_begin = std::chrono::steady_clock::now();
  uint32_t removed_count = 0;
  uint32_t refreshed_count = 0;
  uint32_t updated_count = 0;
  uint32_t skipped_no_demand_count = 0;
  for (const RefreshCandidate& candidate : candidates) {
    const uint64_t stream_id = candidate.stream_id;
    SharedStreamResultData data = runtime.get_latest_stream_result(stream_id);
    if (!data) {
      std::lock_guard<std::mutex> lock(g_live_cpu_display_views_mutex);
      const uint32_t removed_now =
          static_cast<uint32_t>(g_live_cpu_display_views.erase(stream_id));
      removed_count += removed_now;
      note_live_cpu_display_refresh_removed(removed_now);
      continue;
    }
    if (!candidate.entry) {
      continue;
    }
    const bool wrapper_live = has_live_cpu_display_wrapper_borrow(stream_id);
    const bool demand_active = wrapper_live || runtime.is_stream_display_demand_active(stream_id);
    if (!demand_active) {
      note_live_cpu_display_refresh_skip_due_no_demand();
      ++skipped_no_demand_count;
      if (display_demand_trace_enabled()) {
        godot::UtilityFunctions::print(
            "[CamBANG][DemandTrace] cpu_display_refresh stream_id=",
            static_cast<uint64_t>(stream_id),
            " action=skipped_due_no_demand");
      }
      continue;
    }
    if (data->payload_kind == ResultPayloadKind::CPU_PACKED && has_current_retained_cpu_payload(data)) {
      uint64_t prior_retained_frame_id = 0;
      {
        std::lock_guard<std::mutex> entry_lock(candidate.entry->mutex);
        prior_retained_frame_id = candidate.entry->last_retained_frame_id;
      }
      if (refresh_live_cpu_display_view_entry(*candidate.entry, data, false, demand_active, true)) {
        ++refreshed_count;
        uint64_t latest_retained_frame_id = 0;
        {
          std::lock_guard<std::mutex> entry_lock(candidate.entry->mutex);
          latest_retained_frame_id = candidate.entry->last_retained_frame_id;
        }
        if (latest_retained_frame_id != prior_retained_frame_id) {
          ++updated_count;
        }
      }
    } else if (display_demand_trace_enabled()) {
      godot::UtilityFunctions::print(
          "[CamBANG][DemandTrace] cpu_display_refresh stream_id=",
          static_cast<uint64_t>(stream_id),
          " action=skipped_no_payload demand_active=",
          demand_active);
    }
  }
  if (display_demand_trace_enabled()) {
    const uint64_t refresh_elapsed_ns = elapsed_ns_since(refresh_begin);
    if (refreshed_count != 0 || removed_count != 0 || skipped_no_demand_count != 0 || refresh_elapsed_ns >= 1'000'000ull) {
      godot::UtilityFunctions::print(
          "[CamBANG][DemandTrace] cpu_display_refresh tracked=",
          static_cast<uint64_t>(candidates.size()),
          " refreshed=",
          static_cast<uint64_t>(refreshed_count),
          " updated=",
          static_cast<uint64_t>(updated_count),
          " skipped_due_no_demand=",
          static_cast<uint64_t>(skipped_no_demand_count),
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
  clear_live_cpu_display_metrics();
}

godot::Dictionary CamBANGStreamResult::get_live_stream_cpu_display_metrics_snapshot() {
  return snapshot_live_cpu_display_metrics();
}

} // namespace cambang

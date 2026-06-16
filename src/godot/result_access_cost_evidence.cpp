#include "godot/result_access_cost_evidence.h"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace cambang::result_access_cost_evidence {
namespace {

constexpr size_t kMaxSeenFreshIdentities = 4096;

struct OperationIdentity final {
  std::string route;
  uint64_t posture_id = 0;
  uint64_t stream_id = 0;
  uint64_t capture_id = 0;
  uint32_t image_member_index = 0;
  uint64_t capture_timestamp_ns = 0;

  bool operator<(const OperationIdentity& other) const noexcept {
    if (route != other.route) return route < other.route;
    if (posture_id != other.posture_id) return posture_id < other.posture_id;
    if (stream_id != other.stream_id) return stream_id < other.stream_id;
    if (capture_id != other.capture_id) return capture_id < other.capture_id;
    if (image_member_index != other.image_member_index) return image_member_index < other.image_member_index;
    return capture_timestamp_ns < other.capture_timestamp_ns;
  }
};

struct RouteEvidence final {
  uint64_t calls = 0;
  uint64_t successes = 0;
  uint64_t failures = 0;
  uint64_t total_ns = 0;
  uint64_t max_ns = 0;
  uint64_t first_success_ns = 0;
  uint64_t fresh_result_successes = 0;
  uint64_t fresh_result_total_ns = 0;
  uint64_t fresh_result_max_ns = 0;
  uint64_t repeat_successes = 0;
  uint64_t repeat_total_ns = 0;
  uint64_t repeat_max_ns = 0;
  uint64_t last_width = 0;
  uint64_t last_height = 0;
  uint64_t last_bytes = 0;
  uint64_t last_posture_id = 0;
  uint64_t posture_count = 0;
  int last_payload_kind = static_cast<int>(ResultPayloadKind::CPU_PACKED);
  bool last_has_retained_cpu_payload = false;
  bool last_has_retained_gpu_backing = false;
  bool last_gpu_materialization_available = false;
  int last_reported_capability = static_cast<int>(ResultCapability::UNSUPPORTED);
};

std::mutex g_mutex;
std::map<std::string, RouteEvidence> g_routes;
std::set<OperationIdentity> g_seen_fresh_identities;
std::deque<OperationIdentity> g_seen_fresh_order;
std::map<std::string, std::set<uint64_t>> g_postures_by_route;
std::map<OperationIdentity, RecordedAccessMeasurement> g_latest_measurements;

uint64_t infer_last_bytes(const SharedStreamResultData& data) noexcept {
  if (!data) {
    return 0;
  }
  if (!data->payload.empty()) {
    return static_cast<uint64_t>(data->payload.size_bytes());
  }
  if (data->retained_gpu_backing_descriptor.valid && data->retained_gpu_backing_descriptor.stride_bytes != 0) {
    return static_cast<uint64_t>(data->retained_gpu_backing_descriptor.stride_bytes) *
           static_cast<uint64_t>(data->retained_gpu_backing_descriptor.height);
  }
  if (data->image_width != 0 && data->image_height != 0) {
    return static_cast<uint64_t>(data->image_width) * static_cast<uint64_t>(data->image_height) * 4ull;
  }
  return 0;
}

bool mark_stream_fresh_locked(const char* route, const SharedStreamResultData& data) {
  if (!route || !data || data->stream_id == 0 || data->capture_timestamp_ns == 0) {
    return false;
  }
  OperationIdentity identity{route, data->access_posture.posture_id, data->stream_id, 0, 0, data->capture_timestamp_ns};
  const auto inserted = g_seen_fresh_identities.insert(identity);
  if (!inserted.second) {
    return false;
  }
  g_seen_fresh_order.push_back(identity);
  while (g_seen_fresh_order.size() > kMaxSeenFreshIdentities) {
    g_seen_fresh_identities.erase(g_seen_fresh_order.front());
    g_seen_fresh_order.pop_front();
  }
  return true;
}


uint64_t infer_capture_last_bytes(const CoreCaptureResultData::ImageMemberData* member) noexcept {
  if (!member) return 0;
  if (!member->payload.empty()) return static_cast<uint64_t>(member->payload.size_bytes());
  if (member->retained_gpu_backing_descriptor.valid && member->retained_gpu_backing_descriptor.stride_bytes != 0) {
    return static_cast<uint64_t>(member->retained_gpu_backing_descriptor.stride_bytes) *
           static_cast<uint64_t>(member->retained_gpu_backing_descriptor.height);
  }
  if (member->retained_gpu_backing_descriptor.width != 0 && member->retained_gpu_backing_descriptor.height != 0) {
    return static_cast<uint64_t>(member->retained_gpu_backing_descriptor.width) *
           static_cast<uint64_t>(member->retained_gpu_backing_descriptor.height) * 4ull;
  }
  return 0;
}

bool mark_capture_fresh_locked(
    const char* route,
    const SharedCaptureResultData& data,
    const CoreCaptureResultData::ImageMemberData* member) {
  if (!route || !data || !member || data->capture_id == 0 || member->capture_timestamp_ns == 0) {
    return false;
  }
  OperationIdentity identity{
      route,
      member->access_posture.posture_id,
      member->access_posture.stream_id,
      data->capture_id,
      member->image_member_index,
      member->capture_timestamp_ns};
  const auto inserted = g_seen_fresh_identities.insert(identity);
  if (!inserted.second) return false;
  g_seen_fresh_order.push_back(identity);
  while (g_seen_fresh_order.size() > kMaxSeenFreshIdentities) {
    g_seen_fresh_identities.erase(g_seen_fresh_order.front());
    g_seen_fresh_order.pop_front();
  }
  return true;
}

void note_posture_locked(RouteEvidence& evidence, const std::string& route, const CoreResultAccessPostureKey& posture) {
  evidence.last_posture_id = posture.posture_id;
  evidence.last_payload_kind = static_cast<int>(posture.payload_kind);
  evidence.last_has_retained_cpu_payload = posture.has_retained_cpu_payload;
  evidence.last_has_retained_gpu_backing = posture.has_retained_gpu_backing;
  evidence.last_gpu_materialization_available = posture.gpu_materialization_available;
  if (posture.posture_id != 0) {
    auto& postures = g_postures_by_route[route];
    postures.insert(posture.posture_id);
    evidence.posture_count = static_cast<uint64_t>(postures.size());
  }
}

void set_dictionary_value(godot::Dictionary& dictionary, const char* key, const godot::Variant& value) {
  dictionary.set(godot::Variant(godot::String(key)), value);
}

void set_dictionary_value(godot::Dictionary& dictionary, const std::string& key, const godot::Variant& value) {
  dictionary.set(godot::Variant(godot::String(key.c_str())), value);
}

godot::Dictionary route_to_dictionary(const RouteEvidence& evidence) {
  godot::Dictionary d;
  set_dictionary_value(d, "calls", godot::Variant(static_cast<uint64_t>(evidence.calls)));
  set_dictionary_value(d, "successes", godot::Variant(static_cast<uint64_t>(evidence.successes)));
  set_dictionary_value(d, "failures", godot::Variant(static_cast<uint64_t>(evidence.failures)));
  set_dictionary_value(d, "total_ns", godot::Variant(static_cast<uint64_t>(evidence.total_ns)));
  set_dictionary_value(d, "max_ns", godot::Variant(static_cast<uint64_t>(evidence.max_ns)));
  set_dictionary_value(d, "first_success_ns", godot::Variant(static_cast<uint64_t>(evidence.first_success_ns)));
  set_dictionary_value(d, "fresh_result_successes", godot::Variant(static_cast<uint64_t>(evidence.fresh_result_successes)));
  set_dictionary_value(d, "fresh_result_total_ns", godot::Variant(static_cast<uint64_t>(evidence.fresh_result_total_ns)));
  set_dictionary_value(d, "fresh_result_max_ns", godot::Variant(static_cast<uint64_t>(evidence.fresh_result_max_ns)));
  set_dictionary_value(d, "repeat_successes", godot::Variant(static_cast<uint64_t>(evidence.repeat_successes)));
  set_dictionary_value(d, "repeat_total_ns", godot::Variant(static_cast<uint64_t>(evidence.repeat_total_ns)));
  set_dictionary_value(d, "repeat_max_ns", godot::Variant(static_cast<uint64_t>(evidence.repeat_max_ns)));
  set_dictionary_value(d, "last_width", godot::Variant(static_cast<uint64_t>(evidence.last_width)));
  set_dictionary_value(d, "last_height", godot::Variant(static_cast<uint64_t>(evidence.last_height)));
  set_dictionary_value(d, "last_bytes", godot::Variant(static_cast<uint64_t>(evidence.last_bytes)));
  set_dictionary_value(d, "last_posture_id", godot::Variant(static_cast<uint64_t>(evidence.last_posture_id)));
  set_dictionary_value(d, "posture_count", godot::Variant(static_cast<uint64_t>(evidence.posture_count)));
  set_dictionary_value(d, "last_payload_kind", godot::Variant(evidence.last_payload_kind));
  set_dictionary_value(d, "last_has_retained_cpu_payload", godot::Variant(evidence.last_has_retained_cpu_payload));
  set_dictionary_value(d, "last_has_retained_gpu_backing", godot::Variant(evidence.last_has_retained_gpu_backing));
  set_dictionary_value(d, "last_gpu_materialization_available", godot::Variant(evidence.last_gpu_materialization_available));
  set_dictionary_value(d, "last_reported_capability", godot::Variant(evidence.last_reported_capability));
  return d;
}

} // namespace

void record_stream_access(
    const char* route,
    const SharedStreamResultData& data,
    uint64_t elapsed_ns,
    bool success,
    ResultCapability reported_capability) noexcept {
  if (!route || route[0] == '\0') {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::string route_key(route);
  RouteEvidence& evidence = g_routes[route_key];
  ++evidence.calls;
  evidence.total_ns += elapsed_ns;
  evidence.max_ns = std::max(evidence.max_ns, elapsed_ns);
  if (success) {
    ++evidence.successes;
    if (evidence.first_success_ns == 0) {
      evidence.first_success_ns = elapsed_ns;
    }
    const bool fresh = mark_stream_fresh_locked(route, data);
    if (fresh) {
      ++evidence.fresh_result_successes;
      evidence.fresh_result_total_ns += elapsed_ns;
      evidence.fresh_result_max_ns = std::max(evidence.fresh_result_max_ns, elapsed_ns);
    } else {
      ++evidence.repeat_successes;
      evidence.repeat_total_ns += elapsed_ns;
      evidence.repeat_max_ns = std::max(evidence.repeat_max_ns, elapsed_ns);
    }
  } else {
    ++evidence.failures;
  }
  if (data) {
    evidence.last_width = data->image_width;
    evidence.last_height = data->image_height;
    evidence.last_bytes = infer_last_bytes(data);
    note_posture_locked(evidence, route_key, data->access_posture);
    OperationIdentity identity{route, data->access_posture.posture_id, data->stream_id, 0, 0, 0};
    g_latest_measurements[identity] = RecordedAccessMeasurement{
        route_key,
        data->access_posture.posture_id,
        elapsed_ns,
        evidence.last_bytes,
        success,
        reported_capability};
  } else {
    evidence.last_width = 0;
    evidence.last_height = 0;
    evidence.last_bytes = 0;
  }
  evidence.last_reported_capability = static_cast<int>(reported_capability);
}


void record_capture_member_access(
    const char* route,
    const SharedCaptureResultData& data,
    const CoreCaptureResultData::ImageMemberData* member,
    uint64_t elapsed_ns,
    bool success,
    ResultCapability reported_capability) noexcept {
  if (!route || route[0] == '\0') {
    return;
  }
  const std::string route_key(route);
  std::lock_guard<std::mutex> lock(g_mutex);
  RouteEvidence& evidence = g_routes[route_key];
  ++evidence.calls;
  evidence.total_ns += elapsed_ns;
  evidence.max_ns = std::max(evidence.max_ns, elapsed_ns);
  if (success) {
    ++evidence.successes;
    if (evidence.first_success_ns == 0) evidence.first_success_ns = elapsed_ns;
    const bool fresh = mark_capture_fresh_locked(route, data, member);
    if (fresh) {
      ++evidence.fresh_result_successes;
      evidence.fresh_result_total_ns += elapsed_ns;
      evidence.fresh_result_max_ns = std::max(evidence.fresh_result_max_ns, elapsed_ns);
    } else {
      ++evidence.repeat_successes;
      evidence.repeat_total_ns += elapsed_ns;
      evidence.repeat_max_ns = std::max(evidence.repeat_max_ns, elapsed_ns);
    }
  } else {
    ++evidence.failures;
  }
  if (member) {
    evidence.last_width = member->access_posture.width;
    evidence.last_height = member->access_posture.height;
    evidence.last_bytes = infer_capture_last_bytes(member);
    note_posture_locked(evidence, route_key, member->access_posture);
    OperationIdentity identity{
        route,
        member->access_posture.posture_id,
        member->access_posture.stream_id,
        data ? data->capture_id : 0,
        member->image_member_index,
        0};
    g_latest_measurements[identity] = RecordedAccessMeasurement{
        route_key,
        member->access_posture.posture_id,
        elapsed_ns,
        evidence.last_bytes,
        success,
        reported_capability};
  } else {
    evidence.last_width = 0;
    evidence.last_height = 0;
    evidence.last_bytes = 0;
  }
  evidence.last_reported_capability = static_cast<int>(reported_capability);
}

godot::Dictionary snapshot() {
  godot::Dictionary d;
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const auto& kv : g_routes) {
    set_dictionary_value(d, kv.first, godot::Variant(route_to_dictionary(kv.second)));
  }
  return d;
}

std::optional<RecordedAccessMeasurement> latest_stream_measurement(
    const char* route,
    uint64_t posture_id) {
  if (!route || route[0] == '\0' || posture_id == 0) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  OperationIdentity identity{route, posture_id, 0, 0, 0, 0};
  auto it = g_latest_measurements.lower_bound(identity);
  while (it != g_latest_measurements.end() &&
         it->first.route == route &&
         it->first.posture_id == posture_id) {
    if (it->first.capture_id == 0 && it->first.image_member_index == 0) {
      return it->second;
    }
    ++it;
  }
  return std::nullopt;
}

std::optional<RecordedAccessMeasurement> latest_capture_measurement(
    const char* route,
    uint64_t posture_id,
    uint32_t image_member_index) {
  if (!route || route[0] == '\0' || posture_id == 0) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  OperationIdentity identity{route, posture_id, 0, 0, image_member_index, 0};
  auto it = g_latest_measurements.lower_bound(identity);
  while (it != g_latest_measurements.end() &&
         it->first.route == route &&
         it->first.posture_id == posture_id) {
    if (it->first.image_member_index == image_member_index) {
      return it->second;
    }
    ++it;
  }
  return std::nullopt;
}

void clear() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_routes.clear();
  g_seen_fresh_identities.clear();
  g_seen_fresh_order.clear();
  g_postures_by_route.clear();
  g_latest_measurements.clear();
}

} // namespace cambang::result_access_cost_evidence

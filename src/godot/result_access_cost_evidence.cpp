#include "godot/result_access_cost_evidence.h"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <godot_cpp/variant/string.hpp>

namespace cambang::result_access_cost_evidence {
namespace {

constexpr size_t kMaxSeenFreshIdentities = 4096;

struct RouteIdentity final {
  std::string route;
  uint64_t stream_id = 0;
  uint64_t capture_timestamp_ns = 0;

  bool operator<(const RouteIdentity& other) const noexcept {
    if (route != other.route) {
      return route < other.route;
    }
    if (stream_id != other.stream_id) {
      return stream_id < other.stream_id;
    }
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
  int last_reported_capability = static_cast<int>(ResultCapability::UNSUPPORTED);
};

std::mutex g_mutex;
std::map<std::string, RouteEvidence> g_routes;
std::set<RouteIdentity> g_seen_fresh_identities;
std::deque<RouteIdentity> g_seen_fresh_order;

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

bool mark_fresh_locked(const char* route, const SharedStreamResultData& data) {
  if (!route || !data || data->stream_id == 0 || data->capture_timestamp_ns == 0) {
    return false;
  }
  RouteIdentity identity{route, data->stream_id, data->capture_timestamp_ns};
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

godot::Dictionary route_to_dictionary(const RouteEvidence& evidence) {
  godot::Dictionary d;
  d["calls"] = static_cast<uint64_t>(evidence.calls);
  d["successes"] = static_cast<uint64_t>(evidence.successes);
  d["failures"] = static_cast<uint64_t>(evidence.failures);
  d["total_ns"] = static_cast<uint64_t>(evidence.total_ns);
  d["max_ns"] = static_cast<uint64_t>(evidence.max_ns);
  d["first_success_ns"] = static_cast<uint64_t>(evidence.first_success_ns);
  d["fresh_result_successes"] = static_cast<uint64_t>(evidence.fresh_result_successes);
  d["fresh_result_total_ns"] = static_cast<uint64_t>(evidence.fresh_result_total_ns);
  d["fresh_result_max_ns"] = static_cast<uint64_t>(evidence.fresh_result_max_ns);
  d["repeat_successes"] = static_cast<uint64_t>(evidence.repeat_successes);
  d["repeat_total_ns"] = static_cast<uint64_t>(evidence.repeat_total_ns);
  d["repeat_max_ns"] = static_cast<uint64_t>(evidence.repeat_max_ns);
  d["last_width"] = static_cast<uint64_t>(evidence.last_width);
  d["last_height"] = static_cast<uint64_t>(evidence.last_height);
  d["last_bytes"] = static_cast<uint64_t>(evidence.last_bytes);
  d["last_reported_capability"] = evidence.last_reported_capability;
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
  RouteEvidence& evidence = g_routes[route];
  ++evidence.calls;
  evidence.total_ns += elapsed_ns;
  evidence.max_ns = std::max(evidence.max_ns, elapsed_ns);
  if (success) {
    ++evidence.successes;
    if (evidence.first_success_ns == 0) {
      evidence.first_success_ns = elapsed_ns;
    }
    const bool fresh = mark_fresh_locked(route, data);
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
    d[godot::String(kv.first.c_str())] = route_to_dictionary(kv.second);
  }
  return d;
}

void clear() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_routes.clear();
  g_seen_fresh_identities.clear();
  g_seen_fresh_order.clear();
}

} // namespace cambang::result_access_cost_evidence

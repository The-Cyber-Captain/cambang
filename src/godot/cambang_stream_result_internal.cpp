#include "godot/cambang_stream_result_internal.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>
#include <vector>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "godot/cambang_server.h"

namespace cambang {

namespace {

std::mutex g_gpu_display_view_borrow_mutex;
std::map<uint64_t, uint64_t> g_gpu_display_view_borrow_counts;

void increment_gpu_display_view_borrow_count(uint64_t stream_id) {
  std::lock_guard<std::mutex> lock(g_gpu_display_view_borrow_mutex);
  ++g_gpu_display_view_borrow_counts[stream_id];
}

void decrement_gpu_display_view_borrow_count(uint64_t stream_id) {
  std::lock_guard<std::mutex> lock(g_gpu_display_view_borrow_mutex);
  auto it = g_gpu_display_view_borrow_counts.find(stream_id);
  if (it == g_gpu_display_view_borrow_counts.end()) {
    return;
  }
  if (it->second <= 1) {
    g_gpu_display_view_borrow_counts.erase(it);
    return;
  }
  --it->second;
}

std::vector<std::pair<uint64_t, uint64_t>> snapshot_gpu_display_view_borrow_counts() {
  std::lock_guard<std::mutex> lock(g_gpu_display_view_borrow_mutex);
  return std::vector<std::pair<uint64_t, uint64_t>>(g_gpu_display_view_borrow_counts.begin(),
                                                    g_gpu_display_view_borrow_counts.end());
}

bool display_demand_trace_enabled() {
  const char* v = std::getenv("CAMBANG_DEV_DISPLAY_DEMAND_TRACE");
  return v && v[0] != '\0' && v[0] != '0';
}
}

DisplayDemandToken::~DisplayDemandToken() {
  if (tracker_registered_) {
    decrement_gpu_display_view_borrow_count(stream_id_);
    tracker_registered_ = false;
  }
  if (display_demand_trace_enabled()) {
    godot::UtilityFunctions::print("[CamBANG][DemandTrace] token_release stream_id=", (long long)stream_id_,
                                   " token_ptr=", (uint64_t)(uintptr_t)this,
                                   " gpu_display_view=", gpu_display_view_);
  }
  if (stream_id_ != 0) {
    if (CamBANGServer* server = CamBANGServer::get_singleton()) {
      server->release_stream_display_demand_async(stream_id_);
    }
  }
}

void DisplayDemandToken::init(uint64_t stream_id, bool gpu_display_view) {
  stream_id_ = stream_id;
  gpu_display_view_ = gpu_display_view;
  if (gpu_display_view_ && stream_id_ != 0) {
    increment_gpu_display_view_borrow_count(stream_id_);
    tracker_registered_ = true;
  }
  if (display_demand_trace_enabled()) {
    godot::UtilityFunctions::print("[CamBANG][DemandTrace] token_retain stream_id=", (long long)stream_id_,
                                   " token_ptr=", (uint64_t)(uintptr_t)this,
                                   " gpu_display_view=", gpu_display_view_);
  }
  if (stream_id_ != 0) {
    if (CamBANGServer* server = CamBANGServer::get_singleton()) {
      server->retain_stream_display_demand(stream_id_);
    }
  }
}

void warn_if_outstanding_gpu_display_views_before_stop() {
  const auto outstanding = snapshot_gpu_display_view_borrow_counts();
  if (outstanding.empty()) {
    return;
  }

  godot::UtilityFunctions::push_warning(
      "[CamBANG][DisplayLifetime] CamBANGServer.stop() called while GPU StreamResult display_view wrappers returned to Godot are still live. Release TextureRect.texture / display_view references before stop. Stop will continue; retained views may become stale after runtime teardown.");
  for (const auto& [stream_id, borrow_count] : outstanding) {
    godot::UtilityFunctions::print("[CamBANG][DisplayLifetime] live_gpu_display_view stream_id=",
                                   (long long)stream_id,
                                   " wrapper_borrow_count=",
                                   (long long)borrow_count);
  }
}

void register_stream_result_internal_classes() {
  godot::ClassDB::register_class<DisplayDemandToken>();
}

} // namespace cambang

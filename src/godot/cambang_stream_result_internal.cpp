#include "godot/cambang_stream_result_internal.h"

#include <cstdint>
#include <cstdlib>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "godot/cambang_server.h"

namespace cambang {

namespace {

bool display_demand_trace_enabled() {
  const char* v = std::getenv("CAMBANG_DEV_DISPLAY_DEMAND_TRACE");
  return v && v[0] != '\0' && v[0] != '0';
}
}

DisplayDemandToken::~DisplayDemandToken() {
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

void register_stream_result_internal_classes() {
  godot::ClassDB::register_class<DisplayDemandToken>();
}

} // namespace cambang

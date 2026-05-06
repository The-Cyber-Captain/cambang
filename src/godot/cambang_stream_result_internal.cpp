#include "godot/cambang_stream_result_internal.h"

#include <godot_cpp/core/class_db.hpp>

#include "godot/cambang_server.h"

namespace cambang {

DisplayDemandToken::~DisplayDemandToken() {
  if (stream_id_ != 0) {
    if (CamBANGServer* server = CamBANGServer::get_singleton()) {
      server->release_stream_display_demand(stream_id_);
    }
  }
}

void DisplayDemandToken::init(uint64_t stream_id) {
  stream_id_ = stream_id;
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

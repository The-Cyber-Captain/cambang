// src/core/core_stream_registry.cpp

#include "core/core_stream_registry.h"

namespace cambang {

bool CoreStreamRegistry::declare_stream_effective(const StreamRequest& effective) {
  if (effective.stream_id == 0) return false;
  auto& rec = streams_[effective.stream_id];
  rec.stream_id = effective.stream_id;
  rec.device_instance_id = effective.device_instance_id;
  rec.intent = effective.intent;
  rec.profile_version = effective.profile_version;
  rec.profile = effective.profile;
  rec.picture = effective.picture;
  rec.ingress_queue_depth = 0;
  // created/started are driven by provider callbacks.
  return true;
}

bool CoreStreamRegistry::on_stream_created(uint64_t stream_id) {
  auto& rec = streams_[stream_id];
  rec.stream_id = stream_id;
  rec.created = true;
  rec.last_stop_origin = StopOrigin::None;
  rec.stop_requested_by_core = false;
  // started remains false until started event arrives
  return true;
}

bool CoreStreamRegistry::on_stream_destroyed(uint64_t stream_id) {
  return streams_.erase(stream_id) > 0;
}

bool CoreStreamRegistry::on_stream_started(uint64_t stream_id) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.started = true;
  it->second.last_stop_origin = StopOrigin::None;
  it->second.stop_requested_by_core = false;
  return true;
}

bool CoreStreamRegistry::on_stream_stopped(uint64_t stream_id, uint32_t error_code) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.started = false;
  it->second.last_error_code = error_code;
  it->second.last_stop_origin = it->second.stop_requested_by_core ? StopOrigin::User : StopOrigin::Provider;
  it->second.stop_requested_by_core = false;
  it->second.ingress_queue_depth = 0;
  return true;
}

bool CoreStreamRegistry::mark_stop_requested_by_core(uint64_t stream_id) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.stop_requested_by_core = true;
  return true;
}

bool CoreStreamRegistry::on_frame_received(uint64_t stream_id, uint64_t integrated_ts_ns, uint32_t ingress_queue_depth) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.frames_received++;
  it->second.last_frame_ts_ns = integrated_ts_ns;
  it->second.ingress_queue_depth = ingress_queue_depth;
  return true;
}

bool CoreStreamRegistry::on_frame_released(uint64_t stream_id) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.frames_released++;
  if (it->second.ingress_queue_depth > 0) {
    it->second.ingress_queue_depth--;
  }
  return true;
}

bool CoreStreamRegistry::on_frame_dropped(uint64_t stream_id) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.frames_dropped++;
  if (it->second.ingress_queue_depth > 0) {
    it->second.ingress_queue_depth--;
  }
  return true;
}

bool CoreStreamRegistry::set_picture(uint64_t stream_id, const PictureConfig& picture) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.picture = picture;
  return true;
}

bool CoreStreamRegistry::forget_stream(uint64_t stream_id) {
  return streams_.erase(stream_id) != 0;
}

bool CoreStreamRegistry::on_stream_error(uint64_t stream_id, uint32_t error_code) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.last_error_code = error_code;
  return true;
}

const CoreStreamRegistry::StreamRecord* CoreStreamRegistry::find(uint64_t stream_id) const noexcept {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return nullptr;
  return &it->second;
}

bool CoreStreamRegistry::has_flowing_stream_for_device(uint64_t device_instance_id) const noexcept {
  if (device_instance_id == 0) {
    return false;
  }
  for (const auto& [stream_id, rec] : streams_) {
    (void)stream_id;
    if (rec.device_instance_id == device_instance_id && rec.created && rec.started) {
      return true;
    }
  }
  return false;
}

} // namespace cambang

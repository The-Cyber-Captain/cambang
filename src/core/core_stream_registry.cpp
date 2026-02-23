// src/core/core_stream_registry.cpp

#include "core/core_stream_registry.h"

namespace cambang {

bool CoreStreamRegistry::on_stream_created(uint64_t stream_id) {
  auto& rec = streams_[stream_id];
  rec.stream_id = stream_id;
  rec.created = true;
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
  return true;
}

bool CoreStreamRegistry::on_stream_stopped(uint64_t stream_id, uint32_t error_code) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.started = false;
  it->second.last_error_code = error_code;
  return true;
}

bool CoreStreamRegistry::on_frame_received(uint64_t stream_id) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.frames_received++;
  return true;
}

bool CoreStreamRegistry::on_frame_released(uint64_t stream_id) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.frames_released++;
  return true;
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

} // namespace cambang

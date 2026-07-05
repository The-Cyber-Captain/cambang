// src/core/core_stream_registry.cpp

#include "core/core_stream_registry.h"

#include <limits>

namespace cambang {

namespace {
bool same_retained_plan(CoreRetainedProductionPlan a,
                        CoreRetainedProductionPlan b) noexcept {
  return a.valid == b.valid && (!a.valid || a.posture == b.posture);
}

void apply_stream_started(CoreStreamRegistry::StreamRecord& rec, uint64_t access_posture_epoch) noexcept {
  rec.started = true;
  rec.last_stop_origin = CoreStreamRegistry::StopOrigin::None;
  rec.stop_requested_by_core = false;
  rec.access_posture_epoch = access_posture_epoch;
}

void apply_stream_stopped(CoreStreamRegistry::StreamRecord& rec,
                          uint32_t error_code,
                          CoreStreamRegistry::StopOrigin origin) noexcept {
  const bool latch_user_stop = origin == CoreStreamRegistry::StopOrigin::User ||
      rec.stop_requested_by_core ||
      (!rec.started && rec.last_stop_origin == CoreStreamRegistry::StopOrigin::User);

  rec.started = false;
  rec.last_error_code = error_code;
  rec.last_stop_origin = latch_user_stop
      ? CoreStreamRegistry::StopOrigin::User
      : CoreStreamRegistry::StopOrigin::Provider;
  rec.stop_requested_by_core = false;
  rec.access_posture_epoch = 0;
}

void increment_saturating(uint32_t& value) noexcept {
  if (value != std::numeric_limits<uint32_t>::max()) {
    ++value;
  }
}
} // namespace

uint64_t CoreStreamRegistry::allocate_access_posture_epoch() noexcept {
  const uint64_t epoch = next_access_posture_epoch_;
  if (next_access_posture_epoch_ != std::numeric_limits<uint64_t>::max()) {
    ++next_access_posture_epoch_;
  }
  return epoch == 0 ? 1 : epoch;
}

bool CoreStreamRegistry::declare_stream_effective(
    const StreamRequest& effective,
    CoreRetainedProductionPlan steady_retained_plan) {
  if (effective.stream_id == 0) return false;
  destroyed_stream_tombstones_.erase(effective.stream_id);
  auto& rec = streams_[effective.stream_id];
  rec.stream_id = effective.stream_id;
  rec.device_instance_id = effective.device_instance_id;
  rec.intent = effective.intent;
  rec.profile_version = effective.profile_version;
  rec.access_posture_epoch = allocate_access_posture_epoch();
  rec.profile = effective.profile;
  rec.picture = effective.picture;
  rec.requested_retained_plan = effective.requested_retained_plan;
  rec.steady_retained_plan = steady_retained_plan;
  // created/started are driven by provider callbacks and core-directed
  // synchronous lifecycle reconciliation.
  return true;
}

bool CoreStreamRegistry::on_stream_created(uint64_t stream_id) {
  if (stream_id == 0) {
    return false;
  }
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    if (destroyed_stream_tombstones_.count(stream_id) > 0) {
      // Ignore delayed provider-created facts for a stream the core has already
      // destroyed. A later real reuse of the same stream_id clears the tombstone
      // via declare_stream_effective().
      return false;
    }
    it = streams_.emplace(stream_id, StreamRecord{}).first;
  }
  auto& rec = it->second;
  if (rec.created) {
    // Provider-created facts can arrive after later start/stop transitions for
    // an already-known stream. Treat them as idempotent presence confirmation;
    // do not reset lifecycle counters or stop-origin truth.
    return true;
  }
  rec.stream_id = stream_id;
  rec.created = true;
  rec.last_stop_origin = StopOrigin::None;
  rec.stop_requested_by_core = false;
  rec.pending_core_start_facts = 0;
  rec.pending_core_stop_facts = 0;
  if (rec.access_posture_epoch == 0) {
    rec.access_posture_epoch = allocate_access_posture_epoch();
  }
  // started remains false until started event arrives or a synchronous core
  // start command succeeds.
  return true;
}

bool CoreStreamRegistry::on_stream_destroyed(uint64_t stream_id) {
  destroyed_stream_tombstones_.insert(stream_id);
  return streams_.erase(stream_id) > 0;
}

bool CoreStreamRegistry::on_core_stream_started(uint64_t stream_id) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  apply_stream_started(it->second, allocate_access_posture_epoch());
  increment_saturating(it->second.pending_core_start_facts);
  return true;
}

bool CoreStreamRegistry::on_provider_stream_started(uint64_t stream_id) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  auto& rec = it->second;
  if (rec.pending_core_start_facts > 0) {
    --rec.pending_core_start_facts;
    return true;
  }
  if (!rec.started && rec.last_stop_origin == StopOrigin::User) {
    // A provider start fact can be delayed behind a newer core-directed stop.
    // Without an operation token in the provider callback, the retained user-stop
    // truth is the newer state and must not be overwritten by this stale fact.
    return true;
  }
  apply_stream_started(rec, allocate_access_posture_epoch());
  return true;
}

bool CoreStreamRegistry::on_core_stream_stopped(uint64_t stream_id, uint32_t error_code) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  apply_stream_stopped(it->second, error_code, StopOrigin::User);
  increment_saturating(it->second.pending_core_stop_facts);
  return true;
}

bool CoreStreamRegistry::on_provider_stream_stopped(uint64_t stream_id, uint32_t error_code) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  auto& rec = it->second;
  if (rec.pending_core_stop_facts > 0) {
    --rec.pending_core_stop_facts;
    rec.last_error_code = error_code;
    return true;
  }
  apply_stream_stopped(rec, error_code, StopOrigin::Provider);
  return true;
}

bool CoreStreamRegistry::mark_stop_requested_by_core(uint64_t stream_id) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.stop_requested_by_core = true;
  return true;
}

bool CoreStreamRegistry::on_frame_received(uint64_t stream_id, uint64_t integrated_ts_ns) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.frames_received++;
  it->second.last_frame_ts_ns = integrated_ts_ns;
  return true;
}

bool CoreStreamRegistry::on_frame_released(uint64_t stream_id) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.frames_released++;
  return true;
}

bool CoreStreamRegistry::on_frame_dropped(uint64_t stream_id) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.frames_dropped++;
  return true;
}

bool CoreStreamRegistry::on_visibility_path(uint64_t stream_id, CoreVisibilityPath path) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;

  switch (path) {
    case CoreVisibilityPath::RGBA_DIRECT:
    case CoreVisibilityPath::BGRA_SWIZZLED:
      it->second.visibility_frames_presented++;
      break;
    case CoreVisibilityPath::REJECTED_UNSUPPORTED:
      it->second.visibility_frames_rejected_unsupported++;
      break;
    case CoreVisibilityPath::REJECTED_INVALID:
      it->second.visibility_frames_rejected_invalid++;
      break;
    case CoreVisibilityPath::NONE:
      return false;
  }

  it->second.visibility_last_path = path;
  return true;
}

bool CoreStreamRegistry::set_picture(uint64_t stream_id, const PictureConfig& picture) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.picture = picture;
  it->second.access_posture_epoch = allocate_access_posture_epoch();
  return true;
}

bool CoreStreamRegistry::set_backing_capabilities(
    uint64_t stream_id,
    ProducerBackingCapabilities runtime_backing_capabilities,
    ProducerBackingCapabilities parent_context_backing_capabilities) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return false;
  }
  it->second.runtime_backing_capabilities = runtime_backing_capabilities;
  it->second.parent_context_backing_capabilities =
      parent_context_backing_capabilities;
  return true;
}

bool CoreStreamRegistry::set_requested_retained_plan(
    uint64_t stream_id,
    CoreRetainedProductionPlan requested_retained_plan,
    bool bump_access_posture_epoch) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  auto& rec = it->second;
  const bool changed = !same_retained_plan(rec.requested_retained_plan,
                                           requested_retained_plan);
  rec.requested_retained_plan = requested_retained_plan;
  if ((changed && bump_access_posture_epoch) || rec.access_posture_epoch == 0) {
    rec.access_posture_epoch = allocate_access_posture_epoch();
  }
  return true;
}

bool CoreStreamRegistry::set_steady_retained_plan(
    uint64_t stream_id,
    CoreRetainedProductionPlan steady_retained_plan) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.steady_retained_plan = steady_retained_plan;
  return true;
}

bool CoreStreamRegistry::clear_steady_retained_plan(uint64_t stream_id) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return false;
  it->second.steady_retained_plan = CoreRetainedProductionPlan{};
  return true;
}

bool CoreStreamRegistry::forget_stream(uint64_t stream_id) {
  destroyed_stream_tombstones_.insert(stream_id);
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

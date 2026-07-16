#include "godot/render_resource_release_internal_smoke.h"

#if defined(CAMBANG_INTERNAL_SMOKE)

#include <memory>
#include <thread>
#include <utility>

#include "godot/cambang_server.h"
#include "godot/render_resource_release_service.h"
#include "godot/cambang_stream_result_internal.h"
#include "godot/synthetic_gpu_backing_bridge_internal.h"

namespace cambang {

bool CamBANGRenderReleaseInternalSmoke::retain_texture(
    const godot::Ref<godot::Texture2D>& texture) noexcept {
  if (texture.is_null()) {
    return false;
  }
  try {
    retained_ = texture;
    return retained_.is_valid();
  } catch (...) {
    return false;
  }
}

bool CamBANGRenderReleaseInternalSmoke::release_retained_from_worker() noexcept {
  if (retained_.is_null()) {
    return false;
  }
  std::unique_ptr<godot::Ref<godot::Texture2D>> worker_owned;
  try {
    worker_owned = std::make_unique<godot::Ref<godot::Texture2D>>(retained_);
    retained_.unref();
    std::thread worker([owned = std::move(worker_owned)]() mutable {
      owned->unref();
    });
    worker.join();
    return true;
  } catch (...) {
    if (worker_owned) {
      retained_ = *worker_owned;
    }
    return false;
  }
}

bool CamBANGRenderReleaseInternalSmoke::retained_texture_is_invalidated() const noexcept {
  try {
    return retained_.is_valid() && !retained_->get_rid().is_valid();
  } catch (...) {
    return false;
  }
}

void CamBANGRenderReleaseInternalSmoke::reset_release_stats() noexcept {
  render_resource_release_reset_stats_for_smoke();
}

godot::Dictionary CamBANGRenderReleaseInternalSmoke::get_release_stats() const {
  const RenderResourceReleaseStats stats = render_resource_release_stats_copy();
  godot::Dictionary out;
  out["accepted_handoffs"] = stats.accepted_handoffs;
  out["drained_texture2drd_wrappers"] = stats.drained_texture2drd_wrappers;
  out["drained_smoke_probes"] = stats.drained_smoke_probes;
  out["freed_rendering_server_rids"] = stats.freed_rendering_server_rids;
  out["freed_rendering_device_rids"] = stats.freed_rendering_device_rids;
  out["runtime_full"] = stats.runtime_full;
  out["runtime_allocation_failure"] = stats.runtime_allocation_failure;
  out["scheduling_failure"] = stats.scheduling_failure;
  out["terminal_closed_wrapper_quarantine"] = stats.terminal_closed_wrapper_quarantine;
  out["terminal_closed_rid_quarantine"] = stats.terminal_closed_rid_quarantine;
  out["wrong_thread_release_attempt"] = stats.wrong_thread_release_attempt;
  out["release_while_lock_held"] = stats.release_while_lock_held;
  out["runtime_failure_transitions"] = stats.runtime_failure_transitions;
  out["godot_thread_scheduling_attempts"] = stats.godot_thread_scheduling_attempts;
  out["scheduling_wrong_thread"] = stats.scheduling_wrong_thread;
  out["duplicate_release_attempts"] = stats.duplicate_release_attempts;
  out["active_reservations"] = stats.active_reservations;
  out["pending"] = stats.pending;
  out["emergency_pending"] = stats.emergency_pending;
  out["handoffs_in_flight"] = stats.handoffs_in_flight;
  out["phase"] = static_cast<uint32_t>(stats.phase);
  return out;
}

namespace {

bool defer_probe_from_worker(bool full, bool allocation_failure, bool schedule_failure) noexcept {
  try {
    RenderResourceReleaseReservation reservation = reserve_render_resource_release();
    if (!reservation) {
      return false;
    }
    std::shared_ptr<void> probe = std::make_shared<uint64_t>(1);
    if (full) {
      render_resource_release_force_next_full_for_smoke();
    } else if (allocation_failure) {
      render_resource_release_force_next_allocation_failure_for_smoke();
    }
    if (schedule_failure) {
      render_resource_release_force_next_schedule_failure_for_smoke();
    }
    bool admitted = false;
    std::thread worker([
        &admitted,
        probe = std::move(probe),
        reservation = std::move(reservation)]() mutable {
      admitted = render_resource_release_defer_probe_for_smoke(
          std::move(probe), std::move(reservation));
    });
    worker.join();
    return admitted;
  } catch (...) {
    return false;
  }
}

} // namespace

bool CamBANGRenderReleaseInternalSmoke::force_runtime_full_probe_from_worker() noexcept {
  return defer_probe_from_worker(true, false, false);
}

bool CamBANGRenderReleaseInternalSmoke::force_allocation_failure_probe_from_worker() noexcept {
  return defer_probe_from_worker(false, true, false);
}

bool CamBANGRenderReleaseInternalSmoke::force_schedule_failure_probe_from_worker() noexcept {
  return defer_probe_from_worker(false, false, true);
}

bool CamBANGRenderReleaseInternalSmoke::exercise_primary_gpu_post_transfer_failure() noexcept {
  return exercise_primary_gpu_post_transfer_failure_for_smoke();
}

bool CamBANGRenderReleaseInternalSmoke::exercise_gpu_wrapper_post_transfer_failure() noexcept {
  return exercise_gpu_wrapper_post_transfer_failure_for_smoke();
}

bool CamBANGRenderReleaseInternalSmoke::exercise_cpu_post_transfer_failure() noexcept {
  return exercise_live_cpu_post_transfer_failure_for_smoke();
}

bool CamBANGRenderReleaseInternalSmoke::recover_release_service() noexcept {
  return render_resource_release_recover_for_smoke();
}

void CamBANGRenderReleaseInternalSmoke::enter_terminal_release_state() noexcept {
  render_resource_release_enter_terminal_for_smoke();
}

bool CamBANGRenderReleaseInternalSmoke::restart_release_service_after_clean_terminal() noexcept {
  return render_resource_release_restart_after_terminal_for_smoke();
}

bool CamBANGRenderReleaseInternalSmoke::exercise_terminal_cutoff_race() noexcept {
  return render_resource_release_exercise_terminal_cutoff_race_for_smoke();
}

uint32_t CamBANGRenderReleaseInternalSmoke::get_persistent_display_demand_refcount(
    uint64_t stream_id) const noexcept {
  CamBANGServer* server = CamBANGServer::get_singleton();
  return server ? server->stream_display_demand_refcount_for_smoke(stream_id) : 0u;
}

void CamBANGRenderReleaseInternalSmoke::_bind_methods() {
  godot::ClassDB::bind_method(
      godot::D_METHOD("retain_texture", "texture"),
      &CamBANGRenderReleaseInternalSmoke::retain_texture);
  godot::ClassDB::bind_method(
      godot::D_METHOD("release_retained_from_worker"),
      &CamBANGRenderReleaseInternalSmoke::release_retained_from_worker);
  godot::ClassDB::bind_method(
      godot::D_METHOD("retained_texture_is_invalidated"),
      &CamBANGRenderReleaseInternalSmoke::retained_texture_is_invalidated);
  godot::ClassDB::bind_method(
      godot::D_METHOD("reset_release_stats"),
      &CamBANGRenderReleaseInternalSmoke::reset_release_stats);
  godot::ClassDB::bind_method(
      godot::D_METHOD("get_release_stats"),
      &CamBANGRenderReleaseInternalSmoke::get_release_stats);
  godot::ClassDB::bind_method(
      godot::D_METHOD("force_runtime_full_probe_from_worker"),
      &CamBANGRenderReleaseInternalSmoke::force_runtime_full_probe_from_worker);
  godot::ClassDB::bind_method(
      godot::D_METHOD("force_allocation_failure_probe_from_worker"),
      &CamBANGRenderReleaseInternalSmoke::force_allocation_failure_probe_from_worker);
  godot::ClassDB::bind_method(
      godot::D_METHOD("force_schedule_failure_probe_from_worker"),
      &CamBANGRenderReleaseInternalSmoke::force_schedule_failure_probe_from_worker);
  godot::ClassDB::bind_method(
      godot::D_METHOD("exercise_primary_gpu_post_transfer_failure"),
      &CamBANGRenderReleaseInternalSmoke::exercise_primary_gpu_post_transfer_failure);
  godot::ClassDB::bind_method(
      godot::D_METHOD("exercise_gpu_wrapper_post_transfer_failure"),
      &CamBANGRenderReleaseInternalSmoke::exercise_gpu_wrapper_post_transfer_failure);
  godot::ClassDB::bind_method(
      godot::D_METHOD("exercise_cpu_post_transfer_failure"),
      &CamBANGRenderReleaseInternalSmoke::exercise_cpu_post_transfer_failure);
  godot::ClassDB::bind_method(
      godot::D_METHOD("recover_release_service"),
      &CamBANGRenderReleaseInternalSmoke::recover_release_service);
  godot::ClassDB::bind_method(
      godot::D_METHOD("enter_terminal_release_state"),
      &CamBANGRenderReleaseInternalSmoke::enter_terminal_release_state);
  godot::ClassDB::bind_method(
      godot::D_METHOD("restart_release_service_after_clean_terminal"),
      &CamBANGRenderReleaseInternalSmoke::restart_release_service_after_clean_terminal);
  godot::ClassDB::bind_method(
      godot::D_METHOD("exercise_terminal_cutoff_race"),
      &CamBANGRenderReleaseInternalSmoke::exercise_terminal_cutoff_race);
  godot::ClassDB::bind_method(
      godot::D_METHOD("get_persistent_display_demand_refcount", "stream_id"),
      &CamBANGRenderReleaseInternalSmoke::get_persistent_display_demand_refcount);
}

} // namespace cambang

#endif

#include "godot/render_resource_release_service.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include "godot/bounded_render_resource_release_queue.h"

namespace cambang {
namespace {

constexpr size_t kReleaseCreditCapacity = 256;

struct PendingRenderResourceRelease final {
  GodotRenderResourceKind kind = GodotRenderResourceKind::RenderingDeviceRid;
  godot::RID rid;
  std::shared_ptr<void> keepalive;
  godot::Ref<godot::Texture2DRD> texture;
  RenderResourceReleaseReservation reservation;
  bool smoke_probe = false;

  PendingRenderResourceRelease() = default;
  PendingRenderResourceRelease(const PendingRenderResourceRelease&) = delete;
  PendingRenderResourceRelease& operator=(const PendingRenderResourceRelease&) = delete;
  PendingRenderResourceRelease(PendingRenderResourceRelease&& other) noexcept
      : kind(other.kind),
        rid(std::move(other.rid)),
        keepalive(std::move(other.keepalive)),
        texture(std::move(other.texture)),
        reservation(std::move(other.reservation)),
        smoke_probe(other.smoke_probe) {}
  PendingRenderResourceRelease& operator=(PendingRenderResourceRelease&& other) noexcept {
    if (this != &other) {
      kind = other.kind;
      rid = std::move(other.rid);
      keepalive = std::move(other.keepalive);
      texture = std::move(other.texture);
      reservation = std::move(other.reservation);
      smoke_probe = other.smoke_probe;
    }
    return *this;
  }
};

using PendingReleaseQueue =
    BoundedRenderResourceReleaseQueue<PendingRenderResourceRelease, kReleaseCreditCapacity>;

struct ReleaseServiceState final {
  PendingReleaseQueue pending;
  // Linearizes handoff admission against the final teardown cutoff. This lock
  // is never held while destroying wrappers, releasing keepalives, or freeing RIDs.
  std::mutex handoff_gate;
  bool terminal_cutoff = true;
  std::atomic<uint64_t> handoffs_in_flight{0};
  std::mutex emergency_mutex;
  std::array<std::optional<PendingRenderResourceRelease>, kReleaseCreditCapacity> emergency{};
  godot::Ref<RenderThreadDrainHelper> helper;
  std::atomic<RenderResourceReleasePhase> phase{RenderResourceReleasePhase::Unavailable};
  std::atomic<size_t> available_credits{kReleaseCreditCapacity};
  std::atomic<bool> drain_scheduled{false};
  std::atomic<uint64_t> accepted_handoffs{0};
  std::atomic<uint64_t> drained_texture2drd_wrappers{0};
  std::atomic<uint64_t> drained_smoke_probes{0};
  std::atomic<uint64_t> freed_rendering_server_rids{0};
  std::atomic<uint64_t> freed_rendering_device_rids{0};
  std::atomic<uint64_t> runtime_full{0};
  std::atomic<uint64_t> runtime_allocation_failure{0};
  std::atomic<uint64_t> scheduling_failure{0};
  std::atomic<uint64_t> terminal_closed_wrapper_quarantine{0};
  std::atomic<uint64_t> terminal_closed_rid_quarantine{0};
  std::atomic<uint64_t> wrong_thread_release_attempt{0};
  std::atomic<uint64_t> release_while_lock_held{0};
  std::atomic<uint64_t> runtime_failure_transitions{0};
  std::atomic<uint64_t> godot_thread_scheduling_attempts{0};
  std::atomic<uint64_t> scheduling_wrong_thread{0};
  std::atomic<uint64_t> duplicate_release_attempts{0};
  std::atomic<uint8_t> pending_failure{
      static_cast<uint8_t>(RenderResourceReleaseFailure::None)};
  std::thread::id scheduling_thread_id;
#if defined(CAMBANG_INTERNAL_SMOKE)
  std::atomic<bool> force_schedule_failure_once{false};
  std::mutex cutoff_smoke_mutex;
  std::condition_variable cutoff_smoke_cv;
  bool cutoff_smoke_active = false;
  bool pause_next_handoff = false;
  bool handoff_paused = false;
  bool release_paused_handoff = false;
  bool cutoff_attempting = false;
#endif
};

// Process-lifetime state prevents terminally quarantined Godot ownership from
// being destructed later by static teardown on an arbitrary thread.
ReleaseServiceState* g_release_service = nullptr;
thread_local uint32_t g_registry_lock_depth = 0;

ReleaseServiceState* service_state() noexcept { return g_release_service; }

bool enter_runtime_failure(ReleaseServiceState& state) noexcept {
  RenderResourceReleasePhase expected = RenderResourceReleasePhase::Active;
  if (state.phase.compare_exchange_strong(
          expected,
          RenderResourceReleasePhase::RuntimeFailed,
          std::memory_order_acq_rel)) {
    state.runtime_failure_transitions.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}

void latch_runtime_failure(
    ReleaseServiceState& state,
    RenderResourceReleaseFailure failure) noexcept {
  if (enter_runtime_failure(state)) {
    state.pending_failure.store(static_cast<uint8_t>(failure), std::memory_order_release);
  }
}

size_t emergency_size(ReleaseServiceState& state) noexcept {
  std::lock_guard<std::mutex> lock(state.emergency_mutex);
  size_t count = 0;
  for (const auto& slot : state.emergency) {
    count += slot.has_value() ? 1u : 0u;
  }
  return count;
}

bool store_emergency(
    ReleaseServiceState& state,
    PendingRenderResourceRelease&& release) noexcept {
  std::lock_guard<std::mutex> lock(state.emergency_mutex);
  for (auto& slot : state.emergency) {
    if (!slot.has_value()) {
      slot.emplace(std::move(release));
      return true;
    }
  }
  // Every created render resource owns one of kReleaseCreditCapacity credits,
  // so queue plus emergency occupancy cannot exceed this array's capacity.
  std::fputs("[CamBANG][RenderRelease] fatal release-credit invariant violation\n", stderr);
  std::abort();
}

std::optional<PendingRenderResourceRelease> take_emergency_one(
    ReleaseServiceState& state) noexcept {
  std::lock_guard<std::mutex> lock(state.emergency_mutex);
  for (auto& slot : state.emergency) {
    if (slot.has_value()) {
      std::optional<PendingRenderResourceRelease> out(std::move(slot));
      slot.reset();
      return out;
    }
  }
  return std::nullopt;
}

void terminalize(PendingRenderResourceRelease& release) noexcept {
  ReleaseServiceState* state = service_state();
  if (release.texture.is_valid()) {
    try {
      // Keep one terminal reference alive after this local Ref drops.
      (void)release.texture->reference();
    } catch (...) {
    }
    if (state) {
      state->terminal_closed_wrapper_quarantine.fetch_add(1, std::memory_order_relaxed);
    }
    std::fputs("[CamBANG][RenderRelease] terminal closed wrapper quarantine\n", stderr);
  }
  if (release.rid.is_valid()) {
    if (state) {
      state->terminal_closed_rid_quarantine.fetch_add(1, std::memory_order_relaxed);
    }
    std::fputs("[CamBANG][RenderRelease] terminal closed RID quarantine\n", stderr);
  }
  release.keepalive.reset();
}

bool append_pending(PendingRenderResourceRelease&& release) noexcept {
  ReleaseServiceState* state = service_state();
  if (!state) {
    terminalize(release);
    return false;
  }
  if (g_registry_lock_depth != 0 || PendingReleaseQueue::lock_held_by_current_thread()) {
    state->release_while_lock_held.fetch_add(1, std::memory_order_relaxed);
  }
  if (!release.reservation) {
    state->duplicate_release_attempts.fetch_add(1, std::memory_order_relaxed);
    std::fputs("[CamBANG][RenderRelease] fatal uncredited or duplicate release handoff\n", stderr);
    std::abort();
  }

  bool post_cutoff = false;
  bool in_flight = false;
  try {
    std::unique_lock<std::mutex> gate_lock(state->handoff_gate);
    if (state->terminal_cutoff) {
      post_cutoff = true;
    } else {
      state->handoffs_in_flight.fetch_add(1, std::memory_order_release);
      in_flight = true;
#if defined(CAMBANG_INTERNAL_SMOKE)
      {
        std::unique_lock<std::mutex> smoke_lock(state->cutoff_smoke_mutex);
        if (state->cutoff_smoke_active && state->pause_next_handoff) {
          state->pause_next_handoff = false;
          state->handoff_paused = true;
          state->cutoff_smoke_cv.notify_all();
          state->cutoff_smoke_cv.wait(smoke_lock, [&state] {
            return state->release_paused_handoff;
          });
        }
      }
#endif
      const PendingReleaseQueue::AdmissionResult result =
          state->pending.admit(std::move(release));
      bool admitted = false;
      switch (result) {
        case PendingReleaseQueue::AdmissionResult::Accepted:
          state->accepted_handoffs.fetch_add(1, std::memory_order_relaxed);
          admitted = true;
          break;
        case PendingReleaseQueue::AdmissionResult::Full:
          state->runtime_full.fetch_add(1, std::memory_order_relaxed);
          latch_runtime_failure(*state, RenderResourceReleaseFailure::QueueFull);
          admitted = store_emergency(*state, std::move(release));
          break;
        case PendingReleaseQueue::AdmissionResult::AllocationFailure:
          state->runtime_allocation_failure.fetch_add(1, std::memory_order_relaxed);
          latch_runtime_failure(*state, RenderResourceReleaseFailure::AllocationFailure);
          admitted = store_emergency(*state, std::move(release));
          break;
        case PendingReleaseQueue::AdmissionResult::Closed:
          state->runtime_full.fetch_add(1, std::memory_order_relaxed);
          latch_runtime_failure(*state, RenderResourceReleaseFailure::AdmissionClosed);
          admitted = store_emergency(*state, std::move(release));
          break;
      }
      state->handoffs_in_flight.fetch_sub(1, std::memory_order_release);
      in_flight = false;
      return admitted;
    }
  } catch (...) {
    post_cutoff = true;
    if (in_flight) {
      state->handoffs_in_flight.fetch_sub(1, std::memory_order_release);
    }
  }
  if (post_cutoff) {
    terminalize(release);
  }
  return false;
}

bool try_linearize_terminal_cutoff_if_quiescent(
    ReleaseServiceState& state) noexcept {
#if defined(CAMBANG_INTERNAL_SMOKE)
  {
    std::lock_guard<std::mutex> smoke_lock(state.cutoff_smoke_mutex);
    if (state.cutoff_smoke_active) {
      state.cutoff_attempting = true;
      state.cutoff_smoke_cv.notify_all();
    }
  }
#endif
  try {
    std::lock_guard<std::mutex> gate_lock(state.handoff_gate);
    if (state.pending.has_pending() || emergency_size(state) != 0) {
      return false;
    }
    state.terminal_cutoff = true;
    state.pending.close_terminal();
    state.phase.store(RenderResourceReleasePhase::Terminal, std::memory_order_release);
    return true;
  } catch (...) {
    return false;
  }
}

void force_linearized_terminal_cutoff(ReleaseServiceState& state) noexcept {
  try {
    std::lock_guard<std::mutex> gate_lock(state.handoff_gate);
    state.terminal_cutoff = true;
    state.pending.close_terminal();
    state.phase.store(RenderResourceReleasePhase::Terminal, std::memory_order_release);
  } catch (...) {
    state.phase.store(RenderResourceReleasePhase::Terminal, std::memory_order_release);
  }
}

void note_schedule_failure(ReleaseServiceState& state) noexcept {
  state.scheduling_failure.fetch_add(1, std::memory_order_relaxed);
}

void schedule_drain_if_needed() noexcept {
  ReleaseServiceState* state = service_state();
  if (!state || state->phase.load(std::memory_order_acquire) == RenderResourceReleasePhase::Terminal ||
      (!state->pending.has_pending() && emergency_size(*state) == 0)) {
    return;
  }
  if (std::this_thread::get_id() != state->scheduling_thread_id) {
    state->scheduling_wrong_thread.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  state->godot_thread_scheduling_attempts.fetch_add(1, std::memory_order_relaxed);
  bool expected = false;
  if (!state->drain_scheduled.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return;
  }
  try {
#if defined(CAMBANG_INTERNAL_SMOKE)
    if (state->force_schedule_failure_once.exchange(false, std::memory_order_acq_rel)) {
      state->drain_scheduled.store(false, std::memory_order_release);
      note_schedule_failure(*state);
      return;
    }
#endif
    godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
    if (state->helper.is_null() || !rs) {
      state->drain_scheduled.store(false, std::memory_order_release);
      note_schedule_failure(*state);
      return;
    }
    rs->call_on_render_thread(godot::Callable(
        state->helper.ptr(), godot::StringName("drain_pending_releases_on_render_thread")));
  } catch (...) {
    state->drain_scheduled.store(false, std::memory_order_release);
    note_schedule_failure(*state);
  }
}

bool process_release(
    ReleaseServiceState& state,
    PendingRenderResourceRelease& release,
    godot::RenderingServer& rs,
    godot::RenderingDevice* rd) noexcept {
  if (PendingReleaseQueue::lock_held_by_current_thread()) {
    state.release_while_lock_held.fetch_add(1, std::memory_order_relaxed);
  }
  if (release.texture.is_valid()) {
    release.texture.unref();
    state.drained_texture2drd_wrappers.fetch_add(1, std::memory_order_relaxed);
  }
  if (release.smoke_probe) {
    state.drained_smoke_probes.fetch_add(1, std::memory_order_relaxed);
  }
  release.keepalive.reset();
  if (!release.rid.is_valid()) {
    return true;
  }
  if (release.kind == GodotRenderResourceKind::RenderingServerRid) {
    rs.free_rid(release.rid);
    state.freed_rendering_server_rids.fetch_add(1, std::memory_order_relaxed);
  } else if (rd) {
    rd->free_rid(release.rid);
    state.freed_rendering_device_rids.fetch_add(1, std::memory_order_relaxed);
  } else {
    (void)append_pending(std::move(release));
    return false;
  }
  return true;
}

void terminalize_all(ReleaseServiceState& state) noexcept {
  while (std::optional<PendingRenderResourceRelease> release = state.pending.take_one()) {
    terminalize(*release);
  }
  while (std::optional<PendingRenderResourceRelease> release = take_emergency_one(state)) {
    terminalize(*release);
  }
}

} // namespace

namespace render_resource_release_detail {
void return_credit() noexcept {
  if (ReleaseServiceState* state = service_state()) {
    state->available_credits.fetch_add(1, std::memory_order_release);
  }
}
} // namespace render_resource_release_detail

void register_render_resource_release_internal_classes() {
  godot::ClassDB::register_class<RenderThreadDrainHelper>();
}

void install_render_resource_release_service() noexcept {
  try {
    if (!g_release_service) {
      g_release_service = new ReleaseServiceState();
    }
    ReleaseServiceState& state = *g_release_service;
    state.scheduling_thread_id = std::this_thread::get_id();
    if (state.helper.is_null()) {
      state.helper.instantiate();
    }
    if (state.helper.is_null()) {
      state.phase.store(RenderResourceReleasePhase::Unavailable, std::memory_order_release);
      state.pending_failure.store(
          static_cast<uint8_t>(RenderResourceReleaseFailure::AllocationFailure),
          std::memory_order_release);
      return;
    }
    state.available_credits.store(kReleaseCreditCapacity, std::memory_order_release);
    {
      std::lock_guard<std::mutex> gate_lock(state.handoff_gate);
      state.pending.open();
      state.terminal_cutoff = false;
    }
    state.drain_scheduled.store(false, std::memory_order_release);
    state.phase.store(RenderResourceReleasePhase::Active, std::memory_order_release);
  } catch (...) {
  }
}

void uninstall_render_resource_release_service() noexcept {
  ReleaseServiceState* state = service_state();
  if (!state || state->phase.load(std::memory_order_acquire) == RenderResourceReleasePhase::Terminal) {
    return;
  }
  state->phase.store(RenderResourceReleasePhase::Closing, std::memory_order_release);
  size_t stalled_passes = 0;
  bool cutoff_complete = false;
  for (size_t pass = 0; pass <= kReleaseCreditCapacity; ++pass) {
    if (try_linearize_terminal_cutoff_if_quiescent(*state)) {
      cutoff_complete = true;
      break;
    }
    const size_t before = state->pending.size() + emergency_size(*state);
    try {
      request_render_resource_release_drain_from_godot_thread();
      if (godot::RenderingServer* rs = godot::RenderingServer::get_singleton()) {
        rs->force_sync();
      }
    } catch (...) {
      note_schedule_failure(*state);
    }
    const size_t after = state->pending.size() + emergency_size(*state);
    stalled_passes = after >= before ? stalled_passes + 1u : 0u;
    if (stalled_passes >= 2u) {
      break;
    }
  }
  if (!cutoff_complete) {
    // A missing render context cannot be retried without bound. Close the same
    // admission gate before quarantining any already accepted ownership.
    force_linearized_terminal_cutoff(*state);
  }
  state->drain_scheduled.store(false, std::memory_order_release);
  state->helper.unref();
  terminalize_all(*state);
}

RenderResourceReleaseReservation reserve_render_resource_release() noexcept {
  ReleaseServiceState* state = service_state();
  if (!state || state->phase.load(std::memory_order_acquire) != RenderResourceReleasePhase::Active) {
    return {};
  }
  size_t available = state->available_credits.load(std::memory_order_acquire);
  while (available != 0) {
    if (state->available_credits.compare_exchange_weak(
            available, available - 1u, std::memory_order_acq_rel)) {
      if (state->phase.load(std::memory_order_acquire) == RenderResourceReleasePhase::Active) {
        return RenderResourceReleaseReservation(true);
      }
      state->available_credits.fetch_add(1, std::memory_order_release);
      return {};
    }
  }
  state->runtime_full.fetch_add(1, std::memory_order_relaxed);
  latch_runtime_failure(*state, RenderResourceReleaseFailure::CreditExhausted);
  return {};
}

bool render_resource_release_runtime_available() noexcept {
  ReleaseServiceState* state = service_state();
  return state && state->phase.load(std::memory_order_acquire) == RenderResourceReleasePhase::Active;
}

RenderResourceReleaseFailure take_render_resource_release_failure() noexcept {
  ReleaseServiceState* state = service_state();
  if (!state) {
    return RenderResourceReleaseFailure::None;
  }
  return static_cast<RenderResourceReleaseFailure>(state->pending_failure.exchange(
      static_cast<uint8_t>(RenderResourceReleaseFailure::None),
      std::memory_order_acq_rel));
}

const char* render_resource_release_failure_name(
    RenderResourceReleaseFailure failure) noexcept {
  switch (failure) {
    case RenderResourceReleaseFailure::None: return "none";
    case RenderResourceReleaseFailure::QueueFull: return "queue_full";
    case RenderResourceReleaseFailure::AllocationFailure: return "allocation_failure";
    case RenderResourceReleaseFailure::CreditExhausted: return "credit_exhausted";
    case RenderResourceReleaseFailure::AdmissionClosed: return "admission_closed";
  }
  return "unknown";
}

RenderResourceReleaseStats render_resource_release_stats_copy() noexcept {
  RenderResourceReleaseStats out;
  ReleaseServiceState* state = service_state();
  if (!state) {
    return out;
  }
  out.accepted_handoffs = state->accepted_handoffs.load(std::memory_order_relaxed);
  out.drained_texture2drd_wrappers = state->drained_texture2drd_wrappers.load(std::memory_order_relaxed);
  out.drained_smoke_probes = state->drained_smoke_probes.load(std::memory_order_relaxed);
  out.freed_rendering_server_rids = state->freed_rendering_server_rids.load(std::memory_order_relaxed);
  out.freed_rendering_device_rids = state->freed_rendering_device_rids.load(std::memory_order_relaxed);
  out.runtime_full = state->runtime_full.load(std::memory_order_relaxed);
  out.runtime_allocation_failure = state->runtime_allocation_failure.load(std::memory_order_relaxed);
  out.scheduling_failure = state->scheduling_failure.load(std::memory_order_relaxed);
  out.terminal_closed_wrapper_quarantine = state->terminal_closed_wrapper_quarantine.load(std::memory_order_relaxed);
  out.terminal_closed_rid_quarantine = state->terminal_closed_rid_quarantine.load(std::memory_order_relaxed);
  out.wrong_thread_release_attempt = state->wrong_thread_release_attempt.load(std::memory_order_relaxed);
  out.release_while_lock_held = state->release_while_lock_held.load(std::memory_order_relaxed);
  out.runtime_failure_transitions = state->runtime_failure_transitions.load(std::memory_order_relaxed);
  out.godot_thread_scheduling_attempts = state->godot_thread_scheduling_attempts.load(std::memory_order_relaxed);
  out.scheduling_wrong_thread = state->scheduling_wrong_thread.load(std::memory_order_relaxed);
  out.duplicate_release_attempts = state->duplicate_release_attempts.load(std::memory_order_relaxed);
  out.active_reservations = kReleaseCreditCapacity - state->available_credits.load(std::memory_order_relaxed);
  out.pending = state->pending.size();
  out.emergency_pending = emergency_size(*state);
  out.handoffs_in_flight = state->handoffs_in_flight.load(std::memory_order_acquire);
  out.phase = state->phase.load(std::memory_order_acquire);
  return out;
}

void render_resource_release_registry_lock_enter() noexcept {
  ++g_registry_lock_depth;
}

void render_resource_release_registry_lock_exit() noexcept {
  if (g_registry_lock_depth != 0) {
    --g_registry_lock_depth;
  }
}

void defer_render_resource_rid_release(
    GodotRenderResourceKind kind,
    const godot::RID& rid,
    RenderResourceReleaseReservation reservation) noexcept {
  if (!rid.is_valid()) {
    return;
  }
  PendingRenderResourceRelease release;
  release.kind = kind;
  release.rid = rid;
  release.reservation = std::move(reservation);
  (void)append_pending(std::move(release));
}

void defer_texture2drd_release(
    godot::Ref<godot::Texture2DRD> texture,
    std::shared_ptr<void> keepalive,
    RenderResourceReleaseReservation reservation) noexcept {
  if (texture.is_null()) {
    return;
  }
  PendingRenderResourceRelease release;
  release.keepalive = std::move(keepalive);
  release.texture = std::move(texture);
  release.reservation = std::move(reservation);
  (void)append_pending(std::move(release));
}

void request_render_resource_release_drain_from_godot_thread() noexcept {
  schedule_drain_if_needed();
}

bool RenderThreadDrainHelper::drain_pending_releases_on_render_thread() noexcept {
  ReleaseServiceState* state = service_state();
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!state || !rs || !rs->is_on_render_thread()) {
    if (state) {
      state->wrong_thread_release_attempt.fetch_add(1, std::memory_order_relaxed);
      state->drain_scheduled.store(false, std::memory_order_release);
    }
    return false;
  }
  state->drain_scheduled.store(false, std::memory_order_release);
  godot::RenderingDevice* rd = rs->get_rendering_device();
  for (size_t processed = 0; processed < kReleaseCreditCapacity; ++processed) {
    std::optional<PendingRenderResourceRelease> release = state->pending.take_one();
    if (!release) {
      release = take_emergency_one(*state);
    }
    if (!release) {
      break;
    }
    if (!process_release(*state, *release, *rs, rd)) {
      break;
    }
  }
  return true;
}

#if defined(CAMBANG_INTERNAL_SMOKE)
void render_resource_release_reset_stats_for_smoke() noexcept {
  ReleaseServiceState* state = service_state();
  if (!state || state->pending.has_pending() || emergency_size(*state) != 0 ||
      state->handoffs_in_flight.load(std::memory_order_acquire) != 0) {
    return;
  }
  state->accepted_handoffs.store(0, std::memory_order_relaxed);
  state->drained_texture2drd_wrappers.store(0, std::memory_order_relaxed);
  state->drained_smoke_probes.store(0, std::memory_order_relaxed);
  state->freed_rendering_server_rids.store(0, std::memory_order_relaxed);
  state->freed_rendering_device_rids.store(0, std::memory_order_relaxed);
  state->runtime_full.store(0, std::memory_order_relaxed);
  state->runtime_allocation_failure.store(0, std::memory_order_relaxed);
  state->scheduling_failure.store(0, std::memory_order_relaxed);
  state->terminal_closed_wrapper_quarantine.store(0, std::memory_order_relaxed);
  state->terminal_closed_rid_quarantine.store(0, std::memory_order_relaxed);
  state->wrong_thread_release_attempt.store(0, std::memory_order_relaxed);
  state->release_while_lock_held.store(0, std::memory_order_relaxed);
  state->runtime_failure_transitions.store(0, std::memory_order_relaxed);
  state->godot_thread_scheduling_attempts.store(0, std::memory_order_relaxed);
  state->scheduling_wrong_thread.store(0, std::memory_order_relaxed);
  state->duplicate_release_attempts.store(0, std::memory_order_relaxed);
}

void render_resource_release_force_next_full_for_smoke() noexcept {
  if (ReleaseServiceState* state = service_state()) {
    state->pending.force_next_full_for_smoke();
  }
}

void render_resource_release_force_next_allocation_failure_for_smoke() noexcept {
  if (ReleaseServiceState* state = service_state()) {
    state->pending.force_next_allocation_failure_for_smoke();
  }
}

void render_resource_release_force_next_schedule_failure_for_smoke() noexcept {
  if (ReleaseServiceState* state = service_state()) {
    state->force_schedule_failure_once.store(true, std::memory_order_release);
  }
}

bool render_resource_release_defer_probe_for_smoke(
    std::shared_ptr<void> probe,
    RenderResourceReleaseReservation reservation) noexcept {
  PendingRenderResourceRelease release;
  release.keepalive = std::move(probe);
  release.reservation = std::move(reservation);
  release.smoke_probe = true;
  return append_pending(std::move(release));
}

bool render_resource_release_recover_for_smoke() noexcept {
  ReleaseServiceState* state = service_state();
  if (!state || state->phase.load(std::memory_order_acquire) != RenderResourceReleasePhase::RuntimeFailed ||
      state->pending.has_pending() || emergency_size(*state) != 0 ||
      state->handoffs_in_flight.load(std::memory_order_acquire) != 0 ||
      state->available_credits.load(std::memory_order_acquire) != kReleaseCreditCapacity ||
      state->pending_failure.load(std::memory_order_acquire) !=
          static_cast<uint8_t>(RenderResourceReleaseFailure::None)) {
    return false;
  }
  state->phase.store(RenderResourceReleasePhase::Active, std::memory_order_release);
  return true;
}

void render_resource_release_enter_terminal_for_smoke() noexcept {
  uninstall_render_resource_release_service();
}

bool render_resource_release_restart_after_terminal_for_smoke() noexcept {
  ReleaseServiceState* state = service_state();
  if (!state || state->phase.load(std::memory_order_acquire) != RenderResourceReleasePhase::Terminal ||
      state->pending.has_pending() || emergency_size(*state) != 0 ||
      state->handoffs_in_flight.load(std::memory_order_acquire) != 0 ||
      state->available_credits.load(std::memory_order_acquire) != kReleaseCreditCapacity ||
      state->terminal_closed_wrapper_quarantine.load(std::memory_order_relaxed) != 0 ||
      state->terminal_closed_rid_quarantine.load(std::memory_order_relaxed) != 0) {
    return false;
  }
  install_render_resource_release_service();
  return state->phase.load(std::memory_order_acquire) == RenderResourceReleasePhase::Active;
}

bool render_resource_release_exercise_terminal_cutoff_race_for_smoke() noexcept {
  ReleaseServiceState* state = service_state();
  if (!state || state->phase.load(std::memory_order_acquire) !=
                    RenderResourceReleasePhase::Active ||
      state->pending.has_pending() || emergency_size(*state) != 0) {
    return false;
  }
  std::thread worker;
  std::thread controller;
  try {
    RenderResourceReleaseReservation reservation = reserve_render_resource_release();
    if (!reservation) {
      return false;
    }
    std::shared_ptr<void> probe = std::make_shared<uint64_t>(1);
    {
      std::lock_guard<std::mutex> smoke_lock(state->cutoff_smoke_mutex);
      state->cutoff_smoke_active = true;
      state->pause_next_handoff = true;
      state->handoff_paused = false;
      state->release_paused_handoff = false;
      state->cutoff_attempting = false;
    }

    bool admitted = false;
    worker = std::thread([
        &admitted,
        probe = std::move(probe),
        reservation = std::move(reservation)]() mutable {
      admitted = render_resource_release_defer_probe_for_smoke(
          std::move(probe), std::move(reservation));
    });
    {
      std::unique_lock<std::mutex> smoke_lock(state->cutoff_smoke_mutex);
      state->cutoff_smoke_cv.wait(smoke_lock, [&state] {
        return state->handoff_paused;
      });
    }
    controller = std::thread([state] {
      std::unique_lock<std::mutex> smoke_lock(state->cutoff_smoke_mutex);
      state->cutoff_smoke_cv.wait(smoke_lock, [&state] {
        return state->cutoff_attempting;
      });
      state->release_paused_handoff = true;
      state->cutoff_smoke_cv.notify_all();
    });

    uninstall_render_resource_release_service();
    worker.join();
    controller.join();
    {
      std::lock_guard<std::mutex> smoke_lock(state->cutoff_smoke_mutex);
      state->cutoff_smoke_active = false;
    }
    return admitted;
  } catch (...) {
    {
      std::lock_guard<std::mutex> smoke_lock(state->cutoff_smoke_mutex);
      state->release_paused_handoff = true;
      state->cutoff_attempting = true;
      state->cutoff_smoke_active = false;
      state->cutoff_smoke_cv.notify_all();
    }
    if (worker.joinable()) {
      worker.join();
    }
    if (controller.joinable()) {
      controller.join();
    }
    return false;
  }
}
#endif

} // namespace cambang

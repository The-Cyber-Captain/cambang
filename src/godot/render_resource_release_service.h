#pragma once

#include <cstdint>
#include <memory>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/rid.hpp>

namespace cambang {

namespace render_resource_release_detail {
void return_credit() noexcept;
}

class RenderResourceReleaseReservation final {
public:
  RenderResourceReleaseReservation() = default;
  ~RenderResourceReleaseReservation() { reset(); }
  RenderResourceReleaseReservation(const RenderResourceReleaseReservation&) = delete;
  RenderResourceReleaseReservation& operator=(const RenderResourceReleaseReservation&) = delete;
  RenderResourceReleaseReservation(RenderResourceReleaseReservation&& other) noexcept
      : active_(other.active_) {
    other.active_ = false;
  }
  RenderResourceReleaseReservation& operator=(RenderResourceReleaseReservation&& other) noexcept {
    if (this != &other) {
      reset();
      active_ = other.active_;
      other.active_ = false;
    }
    return *this;
  }

  explicit operator bool() const noexcept { return active_; }
  void reset() noexcept {
    if (active_) {
      active_ = false;
      render_resource_release_detail::return_credit();
    }
  }

private:
  explicit RenderResourceReleaseReservation(bool active) noexcept : active_(active) {}
  bool active_ = false;
  friend RenderResourceReleaseReservation reserve_render_resource_release() noexcept;
};

enum class GodotRenderResourceKind : unsigned char {
  RenderingServerRid,
  RenderingDeviceRid,
};

enum class RenderResourceReleasePhase : unsigned char {
  Unavailable,
  Active,
  Closing,
  RuntimeFailed,
  Terminal,
};

enum class RenderResourceReleaseFailure : unsigned char {
  None,
  QueueFull,
  AllocationFailure,
  CreditExhausted,
  AdmissionClosed,
};

struct RenderResourceReleaseStats final {
  uint64_t accepted_handoffs = 0;
  uint64_t drained_texture2drd_wrappers = 0;
  uint64_t drained_smoke_probes = 0;
  uint64_t freed_rendering_server_rids = 0;
  uint64_t freed_rendering_device_rids = 0;
  uint64_t runtime_full = 0;
  uint64_t runtime_allocation_failure = 0;
  uint64_t scheduling_failure = 0;
  uint64_t terminal_closed_wrapper_quarantine = 0;
  uint64_t terminal_closed_rid_quarantine = 0;
  uint64_t wrong_thread_release_attempt = 0;
  uint64_t release_while_lock_held = 0;
  uint64_t runtime_failure_transitions = 0;
  uint64_t godot_thread_scheduling_attempts = 0;
  uint64_t scheduling_wrong_thread = 0;
  uint64_t duplicate_release_attempts = 0;
  uint64_t active_reservations = 0;
  uint64_t pending = 0;
  uint64_t emergency_pending = 0;
  uint64_t handoffs_in_flight = 0;
  RenderResourceReleasePhase phase = RenderResourceReleasePhase::Unavailable;
};

class RenderThreadDrainHelper : public godot::RefCounted {
  GDCLASS(RenderThreadDrainHelper, godot::RefCounted);

public:
  bool drain_pending_releases_on_render_thread() noexcept;

  static void _bind_methods() {
    godot::ClassDB::bind_method(
        godot::D_METHOD("drain_pending_releases_on_render_thread"),
        &RenderThreadDrainHelper::drain_pending_releases_on_render_thread);
  }
};

void register_render_resource_release_internal_classes();
void install_render_resource_release_service() noexcept;
void uninstall_render_resource_release_service() noexcept;

RenderResourceReleaseReservation reserve_render_resource_release() noexcept;
bool render_resource_release_runtime_available() noexcept;
RenderResourceReleaseFailure take_render_resource_release_failure() noexcept;
const char* render_resource_release_failure_name(RenderResourceReleaseFailure failure) noexcept;
RenderResourceReleaseStats render_resource_release_stats_copy() noexcept;

void render_resource_release_registry_lock_enter() noexcept;
void render_resource_release_registry_lock_exit() noexcept;
class RenderResourceReleaseRegistryLockScope final {
public:
  RenderResourceReleaseRegistryLockScope() noexcept {
    render_resource_release_registry_lock_enter();
  }
  ~RenderResourceReleaseRegistryLockScope() {
    render_resource_release_registry_lock_exit();
  }
  RenderResourceReleaseRegistryLockScope(const RenderResourceReleaseRegistryLockScope&) = delete;
  RenderResourceReleaseRegistryLockScope& operator=(const RenderResourceReleaseRegistryLockScope&) = delete;
};

void defer_render_resource_rid_release(
    GodotRenderResourceKind kind,
    const godot::RID& rid,
    RenderResourceReleaseReservation reservation) noexcept;
void defer_texture2drd_release(
    godot::Ref<godot::Texture2DRD> texture,
    std::shared_ptr<void> keepalive,
    RenderResourceReleaseReservation reservation) noexcept;
void request_render_resource_release_drain_from_godot_thread() noexcept;

#if defined(CAMBANG_INTERNAL_SMOKE)
void render_resource_release_reset_stats_for_smoke() noexcept;
void render_resource_release_force_next_full_for_smoke() noexcept;
void render_resource_release_force_next_allocation_failure_for_smoke() noexcept;
void render_resource_release_force_next_schedule_failure_for_smoke() noexcept;
bool render_resource_release_defer_probe_for_smoke(
    std::shared_ptr<void> probe,
    RenderResourceReleaseReservation reservation) noexcept;
bool render_resource_release_recover_for_smoke() noexcept;
void render_resource_release_enter_terminal_for_smoke() noexcept;
bool render_resource_release_restart_after_terminal_for_smoke() noexcept;
bool render_resource_release_exercise_terminal_cutoff_race_for_smoke() noexcept;
#endif

} // namespace cambang

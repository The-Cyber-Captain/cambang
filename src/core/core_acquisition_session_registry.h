#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>

#include "core/snapshot/state_snapshot.h"
#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

class CoreAcquisitionSessionRegistry final {
public:
  struct AcquisitionSessionEntry {
    uint64_t acquisition_session_id = 0;
    uint64_t device_instance_id = 0;

    CBLifecyclePhase phase = CBLifecyclePhase::CREATED;

    uint64_t capture_profile_version = 0;
    uint64_t capture_access_posture_epoch = 0;
    CoreRetainedProductionPlan requested_retained_plan{};
    CoreRetainedProductionPlan steady_retained_plan{};
    ProducerBackingCapabilities runtime_backing_capabilities{};
    ProducerBackingCapabilities parent_context_backing_capabilities{};
    uint32_t capture_width = 0;
    uint32_t capture_height = 0;
    uint32_t capture_format = 0;
    CaptureStillImageBundle capture_still_image_bundle{};

    uint64_t captures_triggered = 0;
    uint64_t captures_completed = 0;
    uint64_t captures_failed = 0;

    uint64_t last_capture_id = 0;
    uint64_t last_capture_latency_ns = 0;

    int32_t error_code = 0;
    uint64_t started_ns = 0;
    uint64_t ended_ns = 0;
  };

  bool on_native_object_created(uint64_t native_id,
                                uint32_t type,
                                uint64_t owner_device_instance_id,
                                uint64_t created_ns,
                                uint32_t capture_width,
                                uint32_t capture_height,
                                uint32_t capture_format,
                                uint64_t capture_profile_version,
                                const CaptureStillImageBundle& capture_still_image_bundle);
  bool on_native_object_destroyed(uint64_t native_id, uint64_t destroyed_ns);
  bool on_capture_started(uint64_t device_instance_id,
                          uint64_t capture_id,
                          uint64_t started_ns,
                          uint32_t capture_width,
                          uint32_t capture_height,
                          uint32_t capture_format,
                          uint64_t capture_profile_version,
                          const CaptureStillImageBundle& capture_still_image_bundle);
  bool on_capture_completed(uint64_t device_instance_id, uint64_t capture_id, uint64_t completed_ns);
  bool on_capture_failed(uint64_t device_instance_id,
                         uint64_t capture_id,
                         uint32_t error_code,
                         uint64_t failed_ns);
  bool set_backing_capabilities(uint64_t acquisition_session_id,
                                ProducerBackingCapabilities runtime_backing_capabilities,
                                ProducerBackingCapabilities parent_context_backing_capabilities);
  bool set_requested_retained_plan(uint64_t acquisition_session_id,
                                   CoreRetainedProductionPlan requested_retained_plan,
                                   bool bump_capture_access_posture_epoch = true);
  bool set_steady_retained_plan(uint64_t acquisition_session_id,
                                CoreRetainedProductionPlan steady_retained_plan);
  bool clear_steady_retained_plan(uint64_t acquisition_session_id);
  void clear();

  const AcquisitionSessionEntry* find(uint64_t acquisition_session_id) const noexcept;
  const std::map<uint64_t, AcquisitionSessionEntry>& all() const noexcept { return sessions_; }
  uint64_t resolve_live_session_id_for_device(uint64_t device_instance_id) const noexcept;
  bool has_capture_in_flight_for_device(uint64_t device_instance_id) const noexcept;
  uint64_t resolve_session_id_for_capture(uint64_t device_instance_id,
                                          uint64_t capture_id,
                                          uint64_t acquisition_session_id_hint = 0) const noexcept;

private:
  uint64_t allocate_capture_access_posture_epoch() noexcept;
  uint64_t resolve_live_session_id_for_device_(uint64_t device_instance_id) const;
  void rebuild_device_live_index_(uint64_t device_instance_id);

  struct CaptureInFlight {
    uint64_t acquisition_session_id = 0;
    uint64_t started_ns = 0;
  };

  std::map<uint64_t, AcquisitionSessionEntry> sessions_;
  std::unordered_map<uint64_t, uint64_t> device_live_session_id_;
  std::unordered_map<uint64_t, uint32_t> device_live_session_count_;
  std::unordered_map<uint64_t, CaptureInFlight> captures_in_flight_;
  uint64_t next_capture_access_posture_epoch_ = 1;
};

} // namespace cambang

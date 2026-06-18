// src/core/core_device_registry.h
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

// Minimal per-device core state registry.
//
// Purpose (this build slice):
// - Track which device_instance_ids are known/open according to provider facts.
// - Provide deterministic ordering for teardown (ascending instance_id).
//
// Threading:
// - Core-thread-only. Not atomic. Determinism-first.
class CoreDeviceRegistry final {
public:
  struct DeviceRecord {
    uint64_t device_instance_id = 0;
    std::string hardware_id;
    uint64_t camera_spec_version = 0;
    uint64_t capture_profile_version = 0;
    uint64_t capture_access_posture_epoch = 0;
    CoreRetainedProductionPlan requested_retained_plan{};
    CoreRetainedProductionPlan steady_retained_plan{};
    ProducerBackingCapabilities runtime_backing_capabilities{};
    ProducerBackingCapabilities parent_context_backing_capabilities{};
    uint32_t capture_width = 0;
    uint32_t capture_height = 0;
    uint32_t capture_format = 0;
    CaptureStillImageBundle capture_still_image_bundle = make_default_metered_still_image_bundle();
    PictureConfig capture_picture{};
    uint32_t warm_hold_ms = 0;
    bool warm_deadline_active = false;
    uint64_t warm_deadline_ns = 0;
    bool warm_expired_close_requested = false;
    bool warm_was_in_use = false;
    bool open = false;
    uint32_t last_error_code = 0;
    uint64_t errors_count = 0;
  };

  bool note_device_identity(uint64_t device_instance_id, const std::string& hardware_id);
  bool set_camera_spec_version(uint64_t device_instance_id, uint64_t camera_spec_version);
  bool retain_capture_profile(uint64_t device_instance_id,
                              uint32_t width,
                              uint32_t height,
                              uint32_t format,
                              uint64_t capture_profile_version);
  bool set_capture_still_image_bundle(uint64_t device_instance_id,
                                  const CaptureStillImageBundle& sequence,
                                  uint64_t capture_profile_version);
  bool set_capture_picture(uint64_t device_instance_id, const PictureConfig& picture);
  bool set_backing_capabilities(uint64_t device_instance_id,
                                ProducerBackingCapabilities runtime_backing_capabilities,
                                ProducerBackingCapabilities parent_context_backing_capabilities);
  bool set_requested_retained_plan(uint64_t device_instance_id,
                                   CoreRetainedProductionPlan requested_retained_plan,
                                   bool bump_capture_access_posture_epoch = true);
  bool set_steady_retained_plan(uint64_t device_instance_id,
                                CoreRetainedProductionPlan steady_retained_plan);
  bool clear_steady_retained_plan(uint64_t device_instance_id);
  bool set_warm_hold_ms(uint64_t device_instance_id, uint32_t warm_hold_ms);
  bool arm_warm_deadline(uint64_t device_instance_id, uint64_t deadline_ns);
  bool clear_warm_deadline(uint64_t device_instance_id);
  bool mark_warm_expired_close_requested(uint64_t device_instance_id, bool requested);
  bool set_warm_was_in_use(uint64_t device_instance_id, bool warm_was_in_use);

  bool on_device_opened(uint64_t device_instance_id);
  bool on_device_closed(uint64_t device_instance_id);
  bool on_device_error(uint64_t device_instance_id, uint32_t error_code);

  const DeviceRecord* find(uint64_t device_instance_id) const noexcept;
  const std::map<uint64_t, DeviceRecord>& all() const noexcept { return devices_; }

private:
  uint64_t allocate_capture_access_posture_epoch() noexcept;

  std::map<uint64_t, DeviceRecord> devices_; // key: device_instance_id
  uint64_t next_capture_access_posture_epoch_ = 1;
};

} // namespace cambang

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "imaging/api/provider_contract_datatypes.h" // cambang::StreamIntent

// Public release-facing truth surface payload (schema v1).
// See: docs/state_snapshot.md (canonical).

enum class CBLifecyclePhase : uint8_t {
    CREATED = 0,
    LIVE = 1,
    TEARING_DOWN = 2,
    DESTROYED = 3,
};

enum class CBRigMode : uint8_t {
    OFF = 0,
    ARMED = 1,
    TRIGGERING = 2,
    COLLECTING = 3,
    ERROR = 4,
};

enum class CBDeviceMode : uint8_t {
    IDLE = 0,
    STREAMING = 1,
    CAPTURING = 2,
    ERROR = 3,
};

enum class CBStreamMode : uint8_t {
    STOPPED = 0,
    FLOWING = 1,
    STARVED = 2,
    ERROR = 3,
};

enum class CBStreamStopReason : uint8_t {
    NONE = 0,
    USER = 1,
    PREEMPTED = 2,
    PROVIDER = 3,
};

struct CamBANGRigState {
    uint64_t rig_id = 0;
    std::string name;

    CBLifecyclePhase phase = CBLifecyclePhase::CREATED;
    CBRigMode mode = CBRigMode::OFF;

    std::vector<std::string> member_hardware_ids;

    uint64_t active_capture_id = 0;
    uint64_t capture_profile_version = 0;

    uint64_t captures_triggered = 0;
    uint64_t captures_completed = 0;
    uint64_t captures_failed = 0;

    uint64_t last_capture_id = 0;
    uint64_t last_capture_latency_ns = 0;
    uint64_t last_sync_skew_ns = 0;

    int32_t error_code = 0;
};

struct CamBANGDeviceState {
    std::string hardware_id;
    uint64_t instance_id = 0;

    CBLifecyclePhase phase = CBLifecyclePhase::CREATED;
    CBDeviceMode mode = CBDeviceMode::IDLE;

    bool engaged = false;
    uint64_t rig_id = 0;

    uint64_t camera_spec_version = 0;
    uint64_t capture_profile_version = 0;

    uint32_t warm_hold_ms = 0;
    uint32_t warm_remaining_ms = 0;

    uint64_t rebuild_count = 0;
    uint64_t errors_count = 0;
    int32_t last_error_code = 0;
};

struct CamBANGStreamState {
    uint64_t stream_id = 0;
    uint64_t device_instance_id = 0;

    CBLifecyclePhase phase = CBLifecyclePhase::CREATED;
    cambang::StreamIntent intent = cambang::StreamIntent::PREVIEW;
    CBStreamMode mode = CBStreamMode::STOPPED;

    CBStreamStopReason stop_reason = CBStreamStopReason::NONE;

    uint64_t profile_version = 0;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;

    uint32_t target_fps_min = 0;
    uint32_t target_fps_max = 0;

    uint64_t frames_received = 0;
    uint64_t frames_delivered = 0;
    uint64_t frames_dropped = 0;
    uint32_t queue_depth = 0;

    uint64_t last_frame_ts_ns = 0;
};

struct NativeObjectRecord {
    uint64_t native_id = 0;
    uint32_t type = 0;

    CBLifecyclePhase phase = CBLifecyclePhase::CREATED;

    uint64_t owner_device_instance_id = 0; // 0 if none
    uint64_t owner_stream_id = 0;          // 0 if none

    uint64_t root_id = 0;

    // Snapshot `gen` when this record was created.
    uint64_t creation_gen = 0;

    uint64_t created_ns = 0;
    uint64_t destroyed_ns = 0;

    uint64_t bytes_allocated = 0;
    uint32_t buffers_in_use = 0;
};

struct CamBANGStateSnapshot {
    static constexpr uint32_t kSchemaVersion = 1;

    uint32_t schema_version = kSchemaVersion;
    uint64_t gen = 0;
    uint64_t version = 0;
    uint64_t topology_version = 0;
    uint64_t timestamp_ns = 0; // monotonic publish timestamp (generation-relative)

    uint64_t imaging_spec_version = 0;

    std::vector<CamBANGRigState> rigs;
    std::vector<CamBANGDeviceState> devices;
    std::vector<CamBANGStreamState> streams;

    std::vector<NativeObjectRecord> native_objects;
    std::vector<uint64_t> detached_root_ids;
};

#include "godot/state_snapshot_export.h"

#include <godot_cpp/variant/utility_functions.hpp>

namespace cambang {

static inline godot::String gs(const std::string& s) {
  return godot::String(s.c_str());
}

static inline godot::String lifecycle_phase_token(CBLifecyclePhase phase) {
  switch (phase) {
    case CBLifecyclePhase::CREATED:
      return "CREATED";
    case CBLifecyclePhase::LIVE:
      return "LIVE";
    case CBLifecyclePhase::TEARING_DOWN:
      return "TEARING_DOWN";
    case CBLifecyclePhase::DESTROYED:
      return "DESTROYED";
    default:
      godot::UtilityFunctions::push_warning(
          "CamBANG export_snapshot_to_godot: unknown lifecycle phase; emitting UNKNOWN");
      return "UNKNOWN";
  }
}

static inline godot::String rig_mode_token(CBRigMode mode) {
  switch (mode) {
    case CBRigMode::OFF:
      return "OFF";
    case CBRigMode::ARMED:
      return "ARMED";
    case CBRigMode::TRIGGERING:
      return "TRIGGERING";
    case CBRigMode::COLLECTING:
      return "COLLECTING";
    case CBRigMode::ERROR:
      return "ERROR";
    default:
      godot::UtilityFunctions::push_warning(
          "CamBANG export_snapshot_to_godot: unknown rig mode; emitting UNKNOWN");
      return "UNKNOWN";
  }
}

static inline godot::String device_mode_token(CBDeviceMode mode) {
  switch (mode) {
    case CBDeviceMode::IDLE:
      return "IDLE";
    case CBDeviceMode::STREAMING:
      return "STREAMING";
    case CBDeviceMode::CAPTURING:
      return "CAPTURING";
    case CBDeviceMode::ERROR:
      return "ERROR";
    default:
      godot::UtilityFunctions::push_warning(
          "CamBANG export_snapshot_to_godot: unknown device mode; emitting UNKNOWN");
      return "UNKNOWN";
  }
}

static inline godot::String stream_mode_token(CBStreamMode mode) {
  switch (mode) {
    case CBStreamMode::STOPPED:
      return "STOPPED";
    case CBStreamMode::FLOWING:
      return "FLOWING";
    case CBStreamMode::STARVED:
      return "STARVED";
    case CBStreamMode::ERROR:
      return "ERROR";
    default:
      godot::UtilityFunctions::push_warning(
          "CamBANG export_snapshot_to_godot: unknown stream mode; emitting UNKNOWN");
      return "UNKNOWN";
  }
}

static inline godot::String stream_intent_token(cambang::StreamIntent intent) {
  switch (intent) {
    case cambang::StreamIntent::PREVIEW:
      return "PREVIEW";
    case cambang::StreamIntent::VIEWFINDER:
      return "VIEWFINDER";
    default:
      godot::UtilityFunctions::push_warning(
          "CamBANG export_snapshot_to_godot: unknown stream intent; emitting UNKNOWN");
      return "UNKNOWN";
  }
}

static inline godot::String stream_stop_reason_token(CBStreamStopReason reason) {
  switch (reason) {
    case CBStreamStopReason::NONE:
      return "NONE";
    case CBStreamStopReason::USER:
      return "USER";
    case CBStreamStopReason::PREEMPTED:
      return "PREEMPTED";
    case CBStreamStopReason::PROVIDER:
      return "PROVIDER";
    default:
      godot::UtilityFunctions::push_warning(
          "CamBANG export_snapshot_to_godot: unknown stream stop reason; emitting UNKNOWN");
      return "UNKNOWN";
  }
}

static inline godot::String native_object_type_token(uint32_t raw_type) {
  switch (static_cast<NativeObjectType>(raw_type)) {
    case NativeObjectType::Provider:
      return "provider";
    case NativeObjectType::Device:
      return "device";
    case NativeObjectType::AcquisitionSession:
      return "acquisition_session";
    case NativeObjectType::Stream:
      return "stream";
    case NativeObjectType::FrameProducer:
      return "legacy_frameproducer";
    default:
      godot::UtilityFunctions::push_warning(
          "CamBANG export_snapshot_to_godot: unknown native object type; emitting unknown");
      return "unknown";
  }
}

static inline godot::String visibility_last_path_token(CBVisibilityLastPath path) {
  switch (path) {
    case CBVisibilityLastPath::NONE:
      return "NONE";
    case CBVisibilityLastPath::RGBA_DIRECT:
      return "RGBA_DIRECT";
    case CBVisibilityLastPath::BGRA_SWIZZLED:
      return "BGRA_SWIZZLED";
    case CBVisibilityLastPath::REJECTED_UNSUPPORTED:
      return "REJECTED_UNSUPPORTED";
    case CBVisibilityLastPath::REJECTED_INVALID:
      return "REJECTED_INVALID";
    default:
      godot::UtilityFunctions::push_warning(
          "CamBANG export_snapshot_to_godot: unknown visibility path; emitting UNKNOWN");
      return "UNKNOWN";
  }
}

static godot::Dictionary export_rig(const CamBANGRigState& r) {
  godot::Dictionary d;
  d["rig_id"] = static_cast<uint64_t>(r.rig_id);
  d["name"] = gs(r.name);
  d["phase"] = lifecycle_phase_token(r.phase);
  d["mode"] = rig_mode_token(r.mode);

  godot::Array members;
  members.resize(static_cast<int>(r.member_hardware_ids.size()));
  for (size_t i = 0; i < r.member_hardware_ids.size(); ++i) {
    members[static_cast<int>(i)] = gs(r.member_hardware_ids[i]);
  }
  d["member_hardware_ids"] = members;

  d["active_capture_id"] = static_cast<uint64_t>(r.active_capture_id);
  d["capture_profile_version"] = static_cast<uint64_t>(r.capture_profile_version);
  d["capture_width"] = static_cast<uint32_t>(r.capture_width);
  d["capture_height"] = static_cast<uint32_t>(r.capture_height);
  d["capture_format"] = static_cast<uint32_t>(r.capture_format);

  d["captures_triggered"] = static_cast<uint64_t>(r.captures_triggered);
  d["captures_completed"] = static_cast<uint64_t>(r.captures_completed);
  d["captures_failed"] = static_cast<uint64_t>(r.captures_failed);

  d["last_capture_id"] = static_cast<uint64_t>(r.last_capture_id);
  d["last_capture_latency_ns"] = static_cast<uint64_t>(r.last_capture_latency_ns);
  d["last_sync_skew_ns"] = static_cast<uint64_t>(r.last_sync_skew_ns);

  d["error_code"] = static_cast<int>(r.error_code);
  return d;
}

static godot::Dictionary export_device(const CamBANGDeviceState& s) {
  godot::Dictionary d;
  d["hardware_id"] = gs(s.hardware_id);
  d["instance_id"] = static_cast<uint64_t>(s.instance_id);
  d["phase"] = lifecycle_phase_token(s.phase);
  d["mode"] = device_mode_token(s.mode);
  d["engaged"] = static_cast<bool>(s.engaged);
  d["rig_id"] = static_cast<uint64_t>(s.rig_id);
  d["camera_spec_version"] = static_cast<uint64_t>(s.camera_spec_version);
  d["capture_profile_version"] = static_cast<uint64_t>(s.capture_profile_version);
  d["capture_width"] = static_cast<uint32_t>(s.capture_width);
  d["capture_height"] = static_cast<uint32_t>(s.capture_height);
  d["capture_format"] = static_cast<uint32_t>(s.capture_format);
  d["warm_hold_ms"] = static_cast<uint32_t>(s.warm_hold_ms);
  d["warm_remaining_ms"] = static_cast<uint32_t>(s.warm_remaining_ms);
  d["rebuild_count"] = static_cast<uint64_t>(s.rebuild_count);
  d["errors_count"] = static_cast<uint64_t>(s.errors_count);
  d["last_error_code"] = static_cast<int>(s.last_error_code);
  return d;
}

static godot::Dictionary export_stream(const CamBANGStreamState& s) {
  godot::Dictionary d;
  d["stream_id"] = static_cast<uint64_t>(s.stream_id);
  d["device_instance_id"] = static_cast<uint64_t>(s.device_instance_id);
  d["phase"] = lifecycle_phase_token(s.phase);
  d["intent"] = stream_intent_token(s.intent);
  d["mode"] = stream_mode_token(s.mode);
  d["stop_reason"] = stream_stop_reason_token(s.stop_reason);
  d["profile_version"] = static_cast<uint64_t>(s.profile_version);
  d["width"] = static_cast<uint32_t>(s.width);
  d["height"] = static_cast<uint32_t>(s.height);
  d["format"] = static_cast<uint32_t>(s.format);
  d["target_fps_min"] = static_cast<uint32_t>(s.target_fps_min);
  d["target_fps_max"] = static_cast<uint32_t>(s.target_fps_max);
  d["frames_received"] = static_cast<uint64_t>(s.frames_received);
  d["frames_delivered"] = static_cast<uint64_t>(s.frames_delivered);
  d["frames_dropped"] = static_cast<uint64_t>(s.frames_dropped);
  d["queue_depth"] = static_cast<uint32_t>(s.queue_depth);
  d["last_frame_ts_ns"] = static_cast<uint64_t>(s.last_frame_ts_ns);
  d["visibility_frames_presented"] = static_cast<uint64_t>(s.visibility_frames_presented);
  d["visibility_frames_rejected_unsupported"] =
      static_cast<uint64_t>(s.visibility_frames_rejected_unsupported);
  d["visibility_frames_rejected_invalid"] =
      static_cast<uint64_t>(s.visibility_frames_rejected_invalid);
  d["visibility_last_path"] = visibility_last_path_token(s.visibility_last_path);
  return d;
}

static godot::Dictionary export_acquisition_session(const AcquisitionSessionState& s) {
  godot::Dictionary d;
  d["acquisition_session_id"] = static_cast<uint64_t>(s.acquisition_session_id);
  d["device_instance_id"] = static_cast<uint64_t>(s.device_instance_id);
  d["phase"] = lifecycle_phase_token(s.phase);
  d["capture_profile_version"] = static_cast<uint64_t>(s.capture_profile_version);
  d["capture_width"] = static_cast<uint32_t>(s.capture_width);
  d["capture_height"] = static_cast<uint32_t>(s.capture_height);
  d["capture_format"] = static_cast<uint32_t>(s.capture_format);
  d["captures_triggered"] = static_cast<uint64_t>(s.captures_triggered);
  d["captures_completed"] = static_cast<uint64_t>(s.captures_completed);
  d["captures_failed"] = static_cast<uint64_t>(s.captures_failed);
  d["last_capture_id"] = static_cast<uint64_t>(s.last_capture_id);
  d["last_capture_latency_ns"] = static_cast<uint64_t>(s.last_capture_latency_ns);
  d["error_code"] = static_cast<int>(s.error_code);
  return d;
}

static godot::Dictionary export_native_object(const NativeObjectRecord& r) {
  godot::Dictionary d;
  d["native_id"] = static_cast<uint64_t>(r.native_id);
  d["type"] = native_object_type_token(r.type);
  d["phase"] = lifecycle_phase_token(r.phase);
  d["owner_device_instance_id"] = static_cast<uint64_t>(r.owner_device_instance_id);
  d["owner_acquisition_session_id"] = static_cast<uint64_t>(r.owner_acquisition_session_id);
  d["owner_stream_id"] = static_cast<uint64_t>(r.owner_stream_id);
  d["owner_provider_native_id"] = static_cast<uint64_t>(r.owner_provider_native_id);
  d["owner_rig_id"] = static_cast<uint64_t>(r.owner_rig_id);
  d["root_id"] = static_cast<uint64_t>(r.root_id);
  d["creation_gen"] = static_cast<uint64_t>(r.creation_gen);
  d["created_ns"] = static_cast<uint64_t>(r.created_ns);
  d["destroyed_ns"] = static_cast<uint64_t>(r.destroyed_ns);
  d["bytes_allocated"] = static_cast<uint64_t>(r.bytes_allocated);
  d["buffers_in_use"] = static_cast<uint32_t>(r.buffers_in_use);
  return d;
}

godot::Dictionary export_snapshot_to_godot(const CamBANGStateSnapshot& snap,
                                          uint64_t gen,
                                          uint64_t version,
                                          uint64_t topology_version) {
  godot::Dictionary out;

  // Header.
  out["schema_version"] = static_cast<uint32_t>(snap.schema_version);
  out["gen"] = static_cast<uint64_t>(gen);
  out["version"] = static_cast<uint64_t>(version);
  out["topology_version"] = static_cast<uint64_t>(topology_version);
  out["timestamp_ns"] = static_cast<uint64_t>(snap.timestamp_ns);
  out["imaging_spec_version"] = static_cast<uint64_t>(snap.imaging_spec_version);

  // Records.
  {
    godot::Array rigs;
    rigs.resize(static_cast<int>(snap.rigs.size()));
    for (size_t i = 0; i < snap.rigs.size(); ++i) {
      rigs[static_cast<int>(i)] = export_rig(snap.rigs[i]);
    }
    out["rigs"] = rigs;
  }
  {
    godot::Array devices;
    devices.resize(static_cast<int>(snap.devices.size()));
    for (size_t i = 0; i < snap.devices.size(); ++i) {
      devices[static_cast<int>(i)] = export_device(snap.devices[i]);
    }
    out["devices"] = devices;
  }
  {
    godot::Array acquisition_sessions;
    acquisition_sessions.resize(static_cast<int>(snap.acquisition_sessions.size()));
    for (size_t i = 0; i < snap.acquisition_sessions.size(); ++i) {
      acquisition_sessions[static_cast<int>(i)] = export_acquisition_session(snap.acquisition_sessions[i]);
    }
    out["acquisition_sessions"] = acquisition_sessions;
  }
  {
    godot::Array streams;
    streams.resize(static_cast<int>(snap.streams.size()));
    for (size_t i = 0; i < snap.streams.size(); ++i) {
      streams[static_cast<int>(i)] = export_stream(snap.streams[i]);
    }
    out["streams"] = streams;
  }
  {
    godot::Array native_objects;
    native_objects.resize(static_cast<int>(snap.native_objects.size()));
    for (size_t i = 0; i < snap.native_objects.size(); ++i) {
      native_objects[static_cast<int>(i)] = export_native_object(snap.native_objects[i]);
    }
    out["native_objects"] = native_objects;
  }
  {
    godot::Array detached;
    detached.resize(static_cast<int>(snap.detached_root_ids.size()));
    for (size_t i = 0; i < snap.detached_root_ids.size(); ++i) {
      detached[static_cast<int>(i)] = static_cast<uint64_t>(snap.detached_root_ids[i]);
    }
    out["detached_root_ids"] = detached;
  }

  return out;
}

} // namespace cambang

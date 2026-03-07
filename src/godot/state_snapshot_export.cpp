#include "godot/state_snapshot_export.h"

#include <godot_cpp/variant/utility_functions.hpp>

namespace cambang {

static inline godot::String gs(const std::string& s) {
  return godot::String(s.c_str());
}

static godot::Dictionary export_rig(const CamBANGRigState& r) {
  godot::Dictionary d;
  d["rig_id"] = static_cast<uint64_t>(r.rig_id);
  d["name"] = gs(r.name);
  d["phase"] = static_cast<int>(r.phase);
  d["mode"] = static_cast<int>(r.mode);

  godot::Array members;
  members.resize(static_cast<int>(r.member_hardware_ids.size()));
  for (size_t i = 0; i < r.member_hardware_ids.size(); ++i) {
    members[static_cast<int>(i)] = gs(r.member_hardware_ids[i]);
  }
  d["member_hardware_ids"] = members;

  d["active_capture_id"] = static_cast<uint64_t>(r.active_capture_id);
  d["capture_profile_version"] = static_cast<uint64_t>(r.capture_profile_version);

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
  d["phase"] = static_cast<int>(s.phase);
  d["mode"] = static_cast<int>(s.mode);
  d["engaged"] = static_cast<bool>(s.engaged);
  d["rig_id"] = static_cast<uint64_t>(s.rig_id);
  d["camera_spec_version"] = static_cast<uint64_t>(s.camera_spec_version);
  d["capture_profile_version"] = static_cast<uint64_t>(s.capture_profile_version);
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
  d["phase"] = static_cast<int>(s.phase);
  d["intent"] = static_cast<int>(s.intent);
  d["mode"] = static_cast<int>(s.mode);
  d["stop_reason"] = static_cast<int>(s.stop_reason);
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
  return d;
}

static godot::Dictionary export_native_object(const NativeObjectRecord& r) {
  godot::Dictionary d;
  d["native_id"] = static_cast<uint64_t>(r.native_id);
  d["type"] = static_cast<uint32_t>(r.type);
  d["phase"] = static_cast<int>(r.phase);
  d["owner_device_instance_id"] = static_cast<uint64_t>(r.owner_device_instance_id);
  d["owner_stream_id"] = static_cast<uint64_t>(r.owner_stream_id);
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

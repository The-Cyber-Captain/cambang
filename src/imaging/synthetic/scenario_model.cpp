#include "imaging/synthetic/scenario_model.h"

#include <algorithm>
#include <unordered_map>

namespace cambang {

namespace {

template <typename T>
bool duplicate_key(const std::vector<T>& entries, const std::string T::*member_key) {
  std::unordered_map<std::string, bool> seen;
  seen.reserve(entries.size());
  for (const auto& e : entries) {
    if ((e.*member_key).empty()) {
      return true;
    }
    if (!seen.emplace(e.*member_key, true).second) {
      return true;
    }
  }
  return false;
}

void set_error(std::string* error, const std::string& value) {
  if (error) {
    *error = value;
  }
}

} // namespace

bool materialize_synthetic_canonical_scenario(
    const SyntheticCanonicalScenario& canonical,
    const SyntheticScenarioMaterializationOptions& options,
    SyntheticScenarioMaterializationResult& out,
    std::string* error) {
  out = {};

  if (duplicate_key(canonical.devices, &SyntheticScenarioDeviceDeclaration::key)) {
    set_error(error, "invalid canonical scenario device declarations (empty/duplicate key)");
    return false;
  }
  if (duplicate_key(canonical.streams, &SyntheticScenarioStreamDeclaration::key)) {
    set_error(error, "invalid canonical scenario stream declarations (empty/duplicate key)");
    return false;
  }

  std::unordered_map<std::string, SyntheticMaterializedDeviceBinding> device_by_key;
  device_by_key.reserve(canonical.devices.size());
  out.devices.reserve(canonical.devices.size());
  for (size_t i = 0; i < canonical.devices.size(); ++i) {
    const auto& declared = canonical.devices[i];
    SyntheticMaterializedDeviceBinding bound{};
    bound.key = declared.key;
    bound.endpoint_index = declared.endpoint_index;
    bound.device_instance_id = options.device_instance_id_base + static_cast<uint64_t>(i + 1);
    bound.root_id = options.root_id_base + static_cast<uint64_t>(i + 1);
    out.devices.push_back(bound);
    device_by_key.emplace(bound.key, bound);
  }

  std::unordered_map<std::string, SyntheticMaterializedStreamBinding> stream_by_key;
  stream_by_key.reserve(canonical.streams.size());
  out.streams.reserve(canonical.streams.size());
  for (size_t i = 0; i < canonical.streams.size(); ++i) {
    const auto& declared = canonical.streams[i];
    const auto dit = device_by_key.find(declared.device_key);
    if (dit == device_by_key.end()) {
      set_error(error, "stream declaration references unknown device key: " + declared.device_key);
      return false;
    }

    SyntheticMaterializedStreamBinding bound{};
    bound.key = declared.key;
    bound.device_key = declared.device_key;
    bound.intent = declared.intent;
    bound.baseline_capture_profile = declared.baseline_capture_profile;
    bound.device_instance_id = dit->second.device_instance_id;
    bound.stream_id = options.stream_id_base + static_cast<uint64_t>(i + 1);
    out.streams.push_back(bound);
    stream_by_key.emplace(bound.key, bound);
  }

  std::vector<SyntheticScheduledEvent> indexed;
  indexed.reserve(canonical.timeline.size());

  // Explicit-only semantics: executable behavior comes strictly from authored
  // timeline actions after key->id binding. No lifecycle/picture synthesis.
  for (size_t i = 0; i < canonical.timeline.size(); ++i) {
    const auto& action = canonical.timeline[i];
    SyntheticScheduledEvent ev{};
    ev.at_ns = action.at_ns;
    ev.type = action.type;
    ev.has_picture = action.has_picture;
    ev.picture = action.picture;

    if (!action.device_key.empty()) {
      const auto dit = device_by_key.find(action.device_key);
      if (dit == device_by_key.end()) {
        set_error(error, "timeline action references unknown device key: " + action.device_key);
        return false;
      }
      ev.endpoint_index = dit->second.endpoint_index;
      ev.device_instance_id = dit->second.device_instance_id;
      ev.root_id = dit->second.root_id;
    }

    if (!action.stream_key.empty()) {
      const auto sit = stream_by_key.find(action.stream_key);
      if (sit == stream_by_key.end()) {
        set_error(error, "timeline action references unknown stream key: " + action.stream_key);
        return false;
      }
      ev.stream_id = sit->second.stream_id;
      if (ev.device_instance_id == 0) {
        ev.device_instance_id = sit->second.device_instance_id;
      }
    }

    indexed.push_back(ev);
  }

  std::stable_sort(indexed.begin(), indexed.end(), [](const SyntheticScheduledEvent& a, const SyntheticScheduledEvent& b) {
    return a.at_ns < b.at_ns;
  });

  std::unordered_map<uint64_t, bool> device_open;
  std::unordered_map<uint64_t, std::string> device_key_by_id;
  device_open.reserve(out.devices.size());
  device_key_by_id.reserve(out.devices.size());
  for (const auto& d : out.devices) {
    device_open.emplace(d.device_instance_id, false);
    device_key_by_id.emplace(d.device_instance_id, d.key);
  }

  std::unordered_map<uint64_t, bool> stream_created;
  std::unordered_map<uint64_t, bool> stream_started;
  std::unordered_map<uint64_t, bool> stream_created_once;
  std::unordered_map<uint64_t, uint64_t> stream_device;
  std::unordered_map<uint64_t, std::string> stream_key_by_id;
  stream_created.reserve(out.streams.size());
  stream_started.reserve(out.streams.size());
  stream_created_once.reserve(out.streams.size());
  stream_device.reserve(out.streams.size());
  stream_key_by_id.reserve(out.streams.size());
  for (const auto& s : out.streams) {
    stream_created.emplace(s.stream_id, false);
    stream_started.emplace(s.stream_id, false);
    stream_created_once.emplace(s.stream_id, false);
    stream_device.emplace(s.stream_id, s.device_instance_id);
    stream_key_by_id.emplace(s.stream_id, s.key);
  }

  auto require_stream_device_open = [&](uint64_t stream_id, const char* action_name) -> bool {
    const auto sd = stream_device.find(stream_id);
    if (sd == stream_device.end()) {
      set_error(error, std::string(action_name) + " requires declared stream key");
      return false;
    }
    const auto dit = device_open.find(sd->second);
    if (dit == device_open.end() || !dit->second) {
      set_error(error, std::string(action_name) + " requires stream device to be open for stream key: " + stream_key_by_id[stream_id]);
      return false;
    }
    return true;
  };

  for (const auto& ev : indexed) {
    switch (ev.type) {
      case SyntheticEventType::OpenDevice:
        if (ev.device_instance_id != 0) {
          if (device_open[ev.device_instance_id]) {
            set_error(error, "OpenDevice duplicated while already open for device key: " + device_key_by_id[ev.device_instance_id]);
            return false;
          }
          device_open[ev.device_instance_id] = true;
        }
        break;

      case SyntheticEventType::CreateStream: {
        if (stream_created[ev.stream_id]) {
          set_error(error, "CreateStream duplicated for stream key: " + stream_key_by_id[ev.stream_id]);
          return false;
        }
        if (stream_created_once[ev.stream_id]) {
          set_error(error, "CreateStream after DestroyStream is not supported for stream key: " + stream_key_by_id[ev.stream_id]);
          return false;
        }
        const auto sd = stream_device.find(ev.stream_id);
        if (sd != stream_device.end()) {
          const auto dit = device_open.find(sd->second);
          if (dit == device_open.end() || !dit->second) {
            set_error(error, "CreateStream requires prior OpenDevice for stream key: " + stream_key_by_id[ev.stream_id]);
            return false;
          }
        }
        stream_created[ev.stream_id] = true;
        stream_created_once[ev.stream_id] = true;
        stream_started[ev.stream_id] = false;
        break;
      }

      case SyntheticEventType::StartStream:
        if (!require_stream_device_open(ev.stream_id, "StartStream")) {
          return false;
        }
        if (stream_created_once[ev.stream_id] && !stream_created[ev.stream_id]) {
          set_error(error, "StartStream after DestroyStream is invalid for stream key: " + stream_key_by_id[ev.stream_id]);
          return false;
        }
        if (!stream_created[ev.stream_id]) {
          set_error(error, "StartStream requires prior CreateStream for stream key: " + stream_key_by_id[ev.stream_id]);
          return false;
        }
        if (stream_started[ev.stream_id]) {
          set_error(error, "StartStream duplicated while already started for stream key: " + stream_key_by_id[ev.stream_id]);
          return false;
        }
        stream_started[ev.stream_id] = true;
        break;

      case SyntheticEventType::StopStream:
        if (!require_stream_device_open(ev.stream_id, "StopStream")) {
          return false;
        }
        if (!stream_started[ev.stream_id]) {
          set_error(error, "StopStream requires stream to be started for stream key: " + stream_key_by_id[ev.stream_id]);
          return false;
        }
        stream_started[ev.stream_id] = false;
        break;

      case SyntheticEventType::DestroyStream:
        if (!require_stream_device_open(ev.stream_id, "DestroyStream")) {
          return false;
        }
        if (!stream_created[ev.stream_id]) {
          if (stream_created_once[ev.stream_id]) {
            set_error(error, "DestroyStream duplicated for already destroyed stream key: " + stream_key_by_id[ev.stream_id]);
          } else {
            set_error(error, "DestroyStream requires prior CreateStream for stream key: " + stream_key_by_id[ev.stream_id]);
          }
          return false;
        }
        stream_created[ev.stream_id] = false;
        stream_started[ev.stream_id] = false;
        break;

      case SyntheticEventType::CloseDevice:
        if (ev.device_instance_id != 0) {
          const auto dit = device_open.find(ev.device_instance_id);
          if (dit == device_open.end() || !dit->second) {
            set_error(error, "CloseDevice requires prior OpenDevice for device key: " + device_key_by_id[ev.device_instance_id]);
            return false;
          }
          for (const auto& sd : stream_device) {
            if (sd.second == ev.device_instance_id && stream_created[sd.first]) {
              set_error(error, "CloseDevice requires associated streams to be destroyed first for device key: " + device_key_by_id[ev.device_instance_id]);
              return false;
            }
          }
          device_open[ev.device_instance_id] = false;
        }
        break;

      case SyntheticEventType::UpdateStreamPicture:
        if (!require_stream_device_open(ev.stream_id, "UpdateStreamPicture")) {
          return false;
        }
        if (!stream_created[ev.stream_id]) {
          if (stream_created_once[ev.stream_id]) {
            set_error(error, "UpdateStreamPicture after DestroyStream is invalid for stream key: " + stream_key_by_id[ev.stream_id]);
          } else {
            set_error(error, "UpdateStreamPicture requires prior CreateStream for stream key: " + stream_key_by_id[ev.stream_id]);
          }
          return false;
        }
        break;
      case SyntheticEventType::EmitFrame:
        if (!require_stream_device_open(ev.stream_id, "EmitFrame")) {
          return false;
        }
        if (!stream_created[ev.stream_id]) {
          if (stream_created_once[ev.stream_id]) {
            set_error(error, "EmitFrame after DestroyStream is invalid for stream key: " + stream_key_by_id[ev.stream_id]);
          } else {
            set_error(error, "EmitFrame requires prior CreateStream for stream key: " + stream_key_by_id[ev.stream_id]);
          }
          return false;
        }
        break;
    }
  }

  out.executable_schedule.events.reserve(indexed.size());
  for (size_t i = 0; i < indexed.size(); ++i) {
    indexed[i].seq = static_cast<uint64_t>(i + 1);
    out.executable_schedule.events.push_back(indexed[i]);
  }

  return true;
}

} // namespace cambang

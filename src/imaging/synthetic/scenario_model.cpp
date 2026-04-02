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

  out.executable_schedule.events.reserve(indexed.size());
  for (size_t i = 0; i < indexed.size(); ++i) {
    indexed[i].seq = static_cast<uint64_t>(i + 1);
    out.executable_schedule.events.push_back(indexed[i]);
  }

  return true;
}

} // namespace cambang

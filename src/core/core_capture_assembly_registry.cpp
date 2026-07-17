#include "core/core_capture_assembly_registry.h"

#include <algorithm>

namespace cambang {

namespace {
CoreCaptureAssemblyRegistry::DeviceCaptureAssembly& get_or_create_assembly(
    std::map<uint64_t, std::map<uint64_t, CoreCaptureAssemblyRegistry::DeviceCaptureAssembly>>& by_capture,
    uint64_t capture_id,
    uint64_t device_instance_id) {
  auto& assembly = by_capture[capture_id][device_instance_id];
  assembly.capture_id = capture_id;
  assembly.device_instance_id = device_instance_id;
  return assembly;
}
} // namespace

void CoreCaptureAssemblyRegistry::mark_default_image_retained(uint64_t capture_id, uint64_t device_instance_id) {
  if (capture_id == 0 || device_instance_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  DeviceCaptureAssembly& assembly =
      get_or_create_assembly(assemblies_by_capture_id_, capture_id, device_instance_id);
  assembly.has_default_image_retained = true;
}

void CoreCaptureAssemblyRegistry::record_admission_context(
    uint64_t capture_id,
    uint64_t device_instance_id,
    CaptureAdmissionContext context,
    const CaptureStillImageBundle& still_image_bundle,
    uint64_t admitted_ns) {
  if (capture_id == 0 || device_instance_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  DeviceCaptureAssembly& assembly =
      get_or_create_assembly(assemblies_by_capture_id_, capture_id, device_instance_id);
  assembly.admission_context = std::move(context);
  assembly.has_admission_context = true;
  assembly.admitted_ns = admitted_ns;
  assembly.expected_image_member_indices.clear();
  assembly.expected_image_member_indices.reserve(still_image_bundle.members.size());
  for (const CaptureStillImageMember& member : still_image_bundle.members) {
    assembly.expected_image_member_indices.push_back(member.image_member_index);
  }
}

bool CoreCaptureAssemblyRegistry::has_admitted_capture_member(
    uint64_t capture_id,
    uint64_t device_instance_id,
    uint32_t image_member_index) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto capture_it = assemblies_by_capture_id_.find(capture_id);
  if (capture_it == assemblies_by_capture_id_.end()) return false;
  const auto device_it = capture_it->second.find(device_instance_id);
  if (device_it == capture_it->second.end()) return false;
  const auto& members = device_it->second.expected_image_member_indices;
  return std::find(members.begin(), members.end(), image_member_index) != members.end();
}

std::optional<CaptureAdmissionContext> CoreCaptureAssemblyRegistry::admission_context_for(
    uint64_t capture_id, uint64_t device_instance_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto capture_it = assemblies_by_capture_id_.find(capture_id);
  if (capture_it == assemblies_by_capture_id_.end()) return std::nullopt;
  const auto device_it = capture_it->second.find(device_instance_id);
  if (device_it == capture_it->second.end() || !device_it->second.has_admission_context) {
    return std::nullopt;
  }
  return device_it->second.admission_context;
}

void CoreCaptureAssemblyRegistry::mark_capture_completed(uint64_t capture_id, uint64_t device_instance_id) {
  if (capture_id == 0 || device_instance_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  DeviceCaptureAssembly& assembly =
      get_or_create_assembly(assemblies_by_capture_id_, capture_id, device_instance_id);
  assembly.terminal_state = TerminalState::COMPLETED;
  assembly.has_failure_error_code = false;
  assembly.failure_error_code = 0;
}

void CoreCaptureAssemblyRegistry::mark_capture_failed(uint64_t capture_id,
                                                      uint64_t device_instance_id,
                                                      uint32_t error_code) {
  if (capture_id == 0 || device_instance_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  DeviceCaptureAssembly& assembly =
      get_or_create_assembly(assemblies_by_capture_id_, capture_id, device_instance_id);
  assembly.terminal_state = TerminalState::FAILED;
  assembly.has_failure_error_code = true;
  assembly.failure_error_code = error_code;
}

bool CoreCaptureAssemblyRegistry::is_assembly_successful(uint64_t capture_id,
                                                         uint64_t device_instance_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto cap_it = assemblies_by_capture_id_.find(capture_id);
  if (cap_it == assemblies_by_capture_id_.end()) {
    return false;
  }
  const auto dev_it = cap_it->second.find(device_instance_id);
  if (dev_it == cap_it->second.end()) {
    return false;
  }
  const DeviceCaptureAssembly& assembly = dev_it->second;
  return assembly.has_default_image_retained &&
         assembly.terminal_state == TerminalState::COMPLETED;
}

bool CoreCaptureAssemblyRegistry::is_result_safe(uint64_t capture_id,
                                                 uint64_t device_instance_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto cap_it = assemblies_by_capture_id_.find(capture_id);
  if (cap_it == assemblies_by_capture_id_.end()) {
    return false;
  }
  const auto dev_it = cap_it->second.find(device_instance_id);
  if (dev_it == cap_it->second.end()) {
    return false;
  }
  const DeviceCaptureAssembly& assembly = dev_it->second;
  if (assembly.terminal_state == TerminalState::FAILED) {
    return true;
  }
  return assembly.has_default_image_retained &&
         assembly.terminal_state == TerminalState::COMPLETED;
}

std::vector<CoreCaptureAssemblyRegistry::TimedOutAssembly>
CoreCaptureAssemblyRegistry::sweep_admission_timeouts(uint64_t now_ns, uint64_t timeout_ns) {
  std::vector<TimedOutAssembly> timed_out;
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [capture_id, by_device] : assemblies_by_capture_id_) {
    for (auto& [device_instance_id, assembly] : by_device) {
      if (!assembly.has_admission_context ||
          assembly.terminal_state != TerminalState::NONE) {
        continue;
      }
      if (now_ns < assembly.admitted_ns + timeout_ns) {
        continue;
      }
      assembly.terminal_state = TerminalState::FAILED;
      assembly.has_failure_error_code = true;
      assembly.failure_error_code = static_cast<uint32_t>(ProviderError::ERR_TIMEOUT);
      timed_out.push_back(TimedOutAssembly{capture_id, device_instance_id});
    }
  }
  return timed_out;
}

std::optional<uint64_t> CoreCaptureAssemblyRegistry::next_admission_timeout_delay_ns(
    uint64_t now_ns, uint64_t timeout_ns) const {
  std::optional<uint64_t> min_delay;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [capture_id, by_device] : assemblies_by_capture_id_) {
    (void)capture_id;
    for (const auto& [device_instance_id, assembly] : by_device) {
      (void)device_instance_id;
      if (!assembly.has_admission_context ||
          assembly.terminal_state != TerminalState::NONE) {
        continue;
      }
      const uint64_t deadline_ns = assembly.admitted_ns + timeout_ns;
      uint64_t delay = 0;
      if (deadline_ns > now_ns) {
        delay = deadline_ns - now_ns;
      }
      if (!min_delay.has_value() || delay < *min_delay) {
        min_delay = delay;
      }
    }
  }
  return min_delay;
}

std::vector<CoreCaptureAssemblyRegistry::RetiredAssembly>
CoreCaptureAssemblyRegistry::retire_terminal_older_than(
    uint64_t now_ns, uint64_t retention_window_ns) {
  std::vector<RetiredAssembly> retired;
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto capture_it = assemblies_by_capture_id_.begin();
       capture_it != assemblies_by_capture_id_.end();) {
    for (auto device_it = capture_it->second.begin();
         device_it != capture_it->second.end();) {
      const DeviceCaptureAssembly& assembly = device_it->second;
      if (assembly.terminal_state == TerminalState::NONE ||
          now_ns < assembly.admitted_ns + retention_window_ns) {
        ++device_it;
        continue;
      }
      retired.push_back(RetiredAssembly{capture_it->first, device_it->first});
      device_it = capture_it->second.erase(device_it);
    }
    if (capture_it->second.empty()) {
      capture_it = assemblies_by_capture_id_.erase(capture_it);
    } else {
      ++capture_it;
    }
  }
  return retired;
}

std::optional<uint64_t> CoreCaptureAssemblyRegistry::next_terminal_retirement_delay_ns(
    uint64_t now_ns, uint64_t retention_window_ns) const {
  std::optional<uint64_t> min_delay;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [capture_id, by_device] : assemblies_by_capture_id_) {
    (void)capture_id;
    for (const auto& [device_instance_id, assembly] : by_device) {
      (void)device_instance_id;
      if (assembly.terminal_state == TerminalState::NONE) {
        continue;
      }
      const uint64_t deadline_ns = assembly.admitted_ns + retention_window_ns;
      uint64_t delay = 0;
      if (deadline_ns > now_ns) {
        delay = deadline_ns - now_ns;
      }
      if (!min_delay.has_value() || delay < *min_delay) {
        min_delay = delay;
      }
    }
  }
  return min_delay;
}


std::vector<std::pair<uint64_t, uint64_t>>
CoreCaptureAssemblyRegistry::terminal_capture_device_pairs() const {
  std::vector<std::pair<uint64_t, uint64_t>> pairs;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [capture_id, by_device] : assemblies_by_capture_id_) {
    for (const auto& [device_instance_id, assembly] : by_device) {
      if (assembly.terminal_state != TerminalState::NONE) {
        pairs.emplace_back(capture_id, device_instance_id);
      }
    }
  }
  return pairs;
}

void CoreCaptureAssemblyRegistry::remove_assembly(uint64_t capture_id, uint64_t device_instance_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto capture_it = assemblies_by_capture_id_.find(capture_id);
  if (capture_it == assemblies_by_capture_id_.end()) {
    return;
  }
  capture_it->second.erase(device_instance_id);
  if (capture_it->second.empty()) {
    assemblies_by_capture_id_.erase(capture_it);
  }
}

void CoreCaptureAssemblyRegistry::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  assemblies_by_capture_id_.clear();
}

#if defined(CAMBANG_INTERNAL_SMOKE)
std::optional<CoreCaptureAssemblyRegistry::DeviceCaptureAssembly>
CoreCaptureAssemblyRegistry::find_for_smoke(uint64_t capture_id, uint64_t device_instance_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto cap_it = assemblies_by_capture_id_.find(capture_id);
  if (cap_it == assemblies_by_capture_id_.end()) {
    return std::nullopt;
  }
  const auto dev_it = cap_it->second.find(device_instance_id);
  if (dev_it == cap_it->second.end()) {
    return std::nullopt;
  }
  return dev_it->second;
}
#endif

} // namespace cambang

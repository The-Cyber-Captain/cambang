#include "core/core_capture_assembly_registry.h"

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
  DeviceCaptureAssembly& assembly =
      get_or_create_assembly(assemblies_by_capture_id_, capture_id, device_instance_id);
  assembly.has_default_image_retained = true;
}

void CoreCaptureAssemblyRegistry::mark_capture_completed(uint64_t capture_id, uint64_t device_instance_id) {
  if (capture_id == 0 || device_instance_id == 0) {
    return;
  }
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
  DeviceCaptureAssembly& assembly =
      get_or_create_assembly(assemblies_by_capture_id_, capture_id, device_instance_id);
  assembly.terminal_state = TerminalState::FAILED;
  assembly.has_failure_error_code = true;
  assembly.failure_error_code = error_code;
}

bool CoreCaptureAssemblyRegistry::is_assembly_successful(uint64_t capture_id,
                                                         uint64_t device_instance_id) const {
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

void CoreCaptureAssemblyRegistry::clear() {
  assemblies_by_capture_id_.clear();
}

#if defined(CAMBANG_INTERNAL_SMOKE)
std::optional<CoreCaptureAssemblyRegistry::DeviceCaptureAssembly>
CoreCaptureAssemblyRegistry::find_for_smoke(uint64_t capture_id, uint64_t device_instance_id) const {
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

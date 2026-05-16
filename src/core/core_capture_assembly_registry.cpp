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

void CoreCaptureAssemblyRegistry::clear() {
  assemblies_by_capture_id_.clear();
}

} // namespace cambang

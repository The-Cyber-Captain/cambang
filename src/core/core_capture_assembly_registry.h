#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "core/capture_admission_context.h"
#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

class CoreCaptureAssemblyRegistry final {
public:
  enum class TerminalState : uint8_t {
    NONE = 0,
    COMPLETED = 1,
    FAILED = 2,
  };

  struct DeviceCaptureAssembly {
    uint64_t capture_id = 0;
    uint64_t device_instance_id = 0;
    bool has_admission_context = false;
    CaptureAdmissionContext admission_context{};
    std::vector<uint32_t> expected_image_member_indices{};
    bool has_default_image_retained = false;
    TerminalState terminal_state = TerminalState::NONE;
    bool has_failure_error_code = false;
    uint32_t failure_error_code = 0;
  };

  void mark_default_image_retained(uint64_t capture_id, uint64_t device_instance_id);
  void record_admission_context(uint64_t capture_id, uint64_t device_instance_id,
                                CaptureAdmissionContext context,
                                const CaptureStillImageBundle& still_image_bundle);
  bool has_admitted_capture_member(uint64_t capture_id,
                                   uint64_t device_instance_id,
                                   uint32_t image_member_index) const;
  void mark_capture_completed(uint64_t capture_id, uint64_t device_instance_id);
  void mark_capture_failed(uint64_t capture_id, uint64_t device_instance_id, uint32_t error_code);
  bool is_assembly_successful(uint64_t capture_id, uint64_t device_instance_id) const;
  bool is_result_safe(uint64_t capture_id, uint64_t device_instance_id) const;

  void clear();

#if defined(CAMBANG_INTERNAL_SMOKE)
  std::optional<DeviceCaptureAssembly> find_for_smoke(uint64_t capture_id,
                                                      uint64_t device_instance_id) const;
#endif

private:
  mutable std::mutex mutex_;
  std::map<uint64_t, std::map<uint64_t, DeviceCaptureAssembly>> assemblies_by_capture_id_;
};

} // namespace cambang

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/camera_concurrency_adc.h"
#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

class CoreSpecState final {
public:
  struct ImagingSpecInterpretation {
    camera_concurrency::Truth camera_concurrency{};
  };

  enum class ImagingSpecRetentionKind : uint8_t {
    None = 0,
    Replace = 1,
    Patch = 2,
  };

  void reset_for_generation(uint64_t imaging_spec_version);
  void set_camera_spec_version(const std::string& hardware_id, uint64_t camera_spec_version);
  uint64_t camera_spec_version(const std::string& hardware_id) const noexcept;

  void set_imaging_spec_version(uint64_t imaging_spec_version) noexcept;
  uint64_t imaging_spec_version() const noexcept { return imaging_spec_version_; }
  bool retain_imaging_spec_replace(uint64_t imaging_spec_version, SpecPatchView effective_spec);
  bool retain_imaging_spec_patch(uint64_t imaging_spec_version, SpecPatchView effective_spec);
  bool has_imaging_spec_payload() const noexcept { return !imaging_spec_payload_.empty(); }
  SpecPatchView imaging_spec_payload() const noexcept;
  std::vector<uint8_t> imaging_spec_payload_copy() const;
  ImagingSpecInterpretation interpret_imaging_spec() const noexcept;
  ImagingSpecRetentionKind imaging_spec_retention_kind() const noexcept {
    return imaging_spec_retention_kind_;
  }

private:
  bool retain_imaging_spec_payload_(
      uint64_t imaging_spec_version,
      SpecPatchView effective_spec,
      ImagingSpecRetentionKind retention_kind);

  std::unordered_map<std::string, uint64_t> camera_spec_versions_;
  uint64_t imaging_spec_version_ = 0;
  std::vector<uint8_t> imaging_spec_payload_{};
  ImagingSpecRetentionKind imaging_spec_retention_kind_ = ImagingSpecRetentionKind::None;
  ImagingSpecInterpretation imaging_spec_interpretation_{};
};

} // namespace cambang

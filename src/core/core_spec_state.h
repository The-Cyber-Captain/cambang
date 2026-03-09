#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace cambang {

class CoreSpecState final {
public:
  void reset_for_generation(uint64_t imaging_spec_version);
  void set_camera_spec_version(const std::string& hardware_id, uint64_t camera_spec_version);
  uint64_t ensure_camera_spec_version(const std::string& hardware_id, uint64_t fallback_version);
  uint64_t camera_spec_version(const std::string& hardware_id) const noexcept;

  void set_imaging_spec_version(uint64_t imaging_spec_version) noexcept;
  uint64_t imaging_spec_version() const noexcept { return imaging_spec_version_; }

private:
  std::unordered_map<std::string, uint64_t> camera_spec_versions_;
  uint64_t imaging_spec_version_ = 0;
};

} // namespace cambang

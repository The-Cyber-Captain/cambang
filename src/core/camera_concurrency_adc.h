#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "imaging/api/provider_contract_datatypes.h"

namespace cambang::camera_concurrency {

namespace ADC {
inline constexpr std::uint32_t kMinSupportedSchemaVersion = 1;
inline constexpr std::uint32_t kMaxSupportedSchemaVersion = 1;
}

enum class TruthKind : std::uint8_t {
  Unavailable = 0,
  Unsupported = 1,
  Supported = 2,
};

struct Truth {
  TruthKind kind = TruthKind::Unavailable;
  std::vector<std::vector<std::string>> allowed_camera_id_combinations{};
};

enum class LoadErrorKind : std::uint8_t {
  None = 0,
  Parse = 1,
  Validation = 2,
};

struct LoadResult {
  bool ok = false;
  LoadErrorKind error_kind = LoadErrorKind::None;
  std::string error_message;
  Truth truth{};
};

std::size_t max_supported_input_bytes() noexcept;
std::size_t max_supported_nesting_depth() noexcept;
std::size_t max_supported_string_bytes() noexcept;
std::size_t max_supported_camera_records() noexcept;
std::size_t max_supported_combination_count() noexcept;
std::size_t max_supported_combination_members() noexcept;

LoadResult load_truth_from_adc_json_text(const std::string& text);
LoadResult load_truth_from_adc_json_payload(SpecPatchView payload);

bool requested_camera_id_set_is_allowed(
    const Truth& truth,
    const std::vector<std::string>& requested_camera_ids) noexcept;

} // namespace cambang::camera_concurrency

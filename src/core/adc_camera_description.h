#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "core/external_camera_description_state.h"

namespace cambang::adc_camera_description {

namespace ADC {
inline constexpr std::uint32_t kMinSupportedSchemaVersion = 2;
inline constexpr std::uint32_t kMaxSupportedSchemaVersion = 2;
}

enum class LoadErrorKind : std::uint8_t {
  None = 0,
  Parse = 1,
  Validation = 2,
};

struct LoadResult {
  bool ok = false;
  LoadErrorKind error_kind = LoadErrorKind::None;
  std::string error_message;
  ExternalCameraDescriptionState state;
};

std::size_t max_supported_input_bytes() noexcept;
std::size_t max_supported_nesting_depth() noexcept;
std::size_t max_supported_string_bytes() noexcept;
std::size_t max_supported_camera_records() noexcept;
std::size_t max_supported_combination_count() noexcept;
std::size_t max_supported_combination_members() noexcept;

LoadResult load_replacement_from_json_text(const std::string& text);

} // namespace cambang::adc_camera_description

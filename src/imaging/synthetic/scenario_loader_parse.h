#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cambang {

struct SyntheticScenarioLoaderParsedCaptureProfile {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t format_fourcc = 0;
  std::uint32_t target_fps_min = 0;
  std::uint32_t target_fps_max = 0;
};

struct SyntheticScenarioLoaderParsedPicture {
  std::string preset;
  std::uint32_t seed = 0;
  std::uint32_t generator_fps_num = 30;
  std::uint32_t generator_fps_den = 1;
  bool overlay_frame_index_offsets = true;
  bool overlay_moving_bar = true;
  std::uint8_t solid_r = 0;
  std::uint8_t solid_g = 0;
  std::uint8_t solid_b = 0;
  std::uint8_t solid_a = 0xFF;
  std::uint32_t checker_size_px = 16;
};

struct SyntheticScenarioLoaderParsedDevice {
  std::string key;
  std::uint32_t endpoint_index = 0;
};

struct SyntheticScenarioLoaderParsedStream {
  std::string key;
  std::string device_key;
  std::string intent;
  SyntheticScenarioLoaderParsedCaptureProfile capture_profile{};
};

struct SyntheticScenarioLoaderParsedAction {
  std::uint64_t at_ns = 0;
  std::string type;
  bool has_device_key = false;
  std::string device_key;
  bool has_stream_key = false;
  std::string stream_key;
  bool has_picture = false;
  SyntheticScenarioLoaderParsedPicture picture{};
};

struct SyntheticScenarioLoaderParsedDocument {
  std::uint32_t schema_version = 0;
  std::vector<SyntheticScenarioLoaderParsedDevice> devices;
  std::vector<SyntheticScenarioLoaderParsedStream> streams;
  std::vector<SyntheticScenarioLoaderParsedAction> timeline;
};

bool parse_synthetic_scenario_loader_json_text(
    const std::string& text,
    SyntheticScenarioLoaderParsedDocument& out,
    std::string* error = nullptr);

} // namespace cambang

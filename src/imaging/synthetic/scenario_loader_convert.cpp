#include "imaging/synthetic/scenario_loader_convert.h"

#include "pixels/pattern/pattern_registry.h"

namespace cambang {
namespace {

void set_error(std::string* error, const std::string& message) {
  if (error) {
    *error = message;
  }
}

bool parse_stream_intent(const std::string& token, StreamIntent& out) {
  if (token == "PREVIEW") {
    out = StreamIntent::PREVIEW;
    return true;
  }
  if (token == "VIEWFINDER") {
    out = StreamIntent::VIEWFINDER;
    return true;
  }
  return false;
}

bool parse_event_type(const std::string& token, SyntheticEventType& out) {
  if (token == "OpenDevice") {
    out = SyntheticEventType::OpenDevice;
    return true;
  }
  if (token == "CreateStream") {
    out = SyntheticEventType::CreateStream;
    return true;
  }
  if (token == "StartStream") {
    out = SyntheticEventType::StartStream;
    return true;
  }
  if (token == "StopStream") {
    out = SyntheticEventType::StopStream;
    return true;
  }
  if (token == "DestroyStream") {
    out = SyntheticEventType::DestroyStream;
    return true;
  }
  if (token == "CloseDevice") {
    out = SyntheticEventType::CloseDevice;
    return true;
  }
  if (token == "UpdateStreamPicture") {
    out = SyntheticEventType::UpdateStreamPicture;
    return true;
  }
  if (token == "UpdateCapturePicture") {
    out = SyntheticEventType::UpdateCapturePicture;
    return true;
  }
  return false;
}

bool parse_pattern_preset(const std::string& token, PatternPreset& out) {
  if (const auto* info = find_preset_info_by_name(token)) {
    out = info->preset;
    return true;
  }
  return false;
}

} // namespace

bool convert_parsed_synthetic_scenario_loader_document_to_canonical(
    const SyntheticScenarioLoaderParsedDocument& parsed,
    SyntheticCanonicalScenario& out,
    std::string* error) {
  out = {};

  out.devices.reserve(parsed.devices.size());
  for (const auto& d : parsed.devices) {
    SyntheticScenarioDeviceDeclaration mapped{};
    mapped.key = d.key;
    mapped.endpoint_index = d.endpoint_index;
    out.devices.push_back(mapped);
  }

  out.streams.reserve(parsed.streams.size());
  for (const auto& s : parsed.streams) {
    SyntheticScenarioStreamDeclaration mapped{};
    mapped.key = s.key;
    mapped.device_key = s.device_key;
    if (!parse_stream_intent(s.intent, mapped.intent)) {
      set_error(error, "invalid stream intent during conversion: " + s.intent);
      return false;
    }
    mapped.baseline_capture_profile.width = s.capture_profile.width;
    mapped.baseline_capture_profile.height = s.capture_profile.height;
    mapped.baseline_capture_profile.format_fourcc = s.capture_profile.format_fourcc;
    mapped.baseline_capture_profile.target_fps_min = s.capture_profile.target_fps_min;
    mapped.baseline_capture_profile.target_fps_max = s.capture_profile.target_fps_max;
    out.streams.push_back(mapped);
  }

  out.timeline.reserve(parsed.timeline.size());
  for (const auto& a : parsed.timeline) {
    SyntheticScenarioTimelineAction mapped{};
    mapped.at_ns = a.at_ns;
    if (!parse_event_type(a.type, mapped.type)) {
      set_error(error, "invalid action type during conversion: " + a.type);
      return false;
    }
    mapped.device_key = a.has_device_key ? a.device_key : "";
    mapped.stream_key = a.has_stream_key ? a.stream_key : "";

    if (a.has_picture) {
      mapped.has_picture = true;
      mapped.picture.seed = a.picture.seed;
      mapped.picture.overlay_frame_index_offsets = a.picture.overlay_frame_index_offsets;
      mapped.picture.overlay_moving_bar = a.picture.overlay_moving_bar;
      mapped.picture.solid_r = a.picture.solid_r;
      mapped.picture.solid_g = a.picture.solid_g;
      mapped.picture.solid_b = a.picture.solid_b;
      mapped.picture.solid_a = a.picture.solid_a;
      mapped.picture.checker_size_px = a.picture.checker_size_px;
      if (!parse_pattern_preset(a.picture.preset, mapped.picture.preset)) {
        set_error(error, "invalid picture preset during conversion: " + a.picture.preset);
        return false;
      }
    }

    out.timeline.push_back(mapped);
  }

  return true;
}

} // namespace cambang

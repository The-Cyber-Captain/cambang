#include "imaging/synthetic/builtin_scenario_library.h"

namespace cambang {

namespace {

constexpr const char* kDeviceKey = "builtin_device";
constexpr const char* kMainStreamKey = "builtin_main_stream";
constexpr const char* kProbeStreamKey = "builtin_probe_stream";

void add_timeline_action(
    SyntheticCanonicalScenario& scenario,
    std::uint64_t at_ns,
    SyntheticEventType type,
    const char* device_key,
    const char* stream_key,
    bool has_picture,
    const PictureConfig& picture) {
  SyntheticScenarioTimelineAction action{};
  action.at_ns = at_ns;
  action.type = type;
  action.device_key = device_key ? device_key : "";
  action.stream_key = stream_key ? stream_key : "";
  action.has_picture = has_picture;
  action.picture = picture;
  scenario.timeline.push_back(action);
}

} // namespace

const char* synthetic_builtin_scenario_library_name(SyntheticBuiltinScenarioLibraryId id) noexcept {
  switch (id) {
    case SyntheticBuiltinScenarioLibraryId::StreamLifecycleVersions:
      return "stream_lifecycle_versions";
    case SyntheticBuiltinScenarioLibraryId::TopologyChangeVersions:
      return "topology_change_versions";
    case SyntheticBuiltinScenarioLibraryId::PublicationCoalescing:
      return "publication_coalescing";
  }
  return "unknown";
}

bool build_synthetic_builtin_scenario_library_canonical_scenario(
    SyntheticBuiltinScenarioLibraryId id,
    const CaptureProfile& baseline_profile,
    SyntheticCanonicalScenario& out,
    std::string* error) {
  // Current scope: built-in scenario library only (C++-authored scenarios).
  // Future file-backed collections belong to external scenario library naming,
  // with serialized ingestion living under scenario loader terminology.
  out = {};

  SyntheticScenarioDeviceDeclaration device_decl{};
  device_decl.key = kDeviceKey;
  device_decl.endpoint_index = 0;
  out.devices.push_back(device_decl);

  SyntheticScenarioStreamDeclaration stream_decl{};
  stream_decl.key = kMainStreamKey;
  stream_decl.device_key = kDeviceKey;
  stream_decl.intent = StreamIntent::PREVIEW;
  stream_decl.baseline_capture_profile = baseline_profile;
  out.streams.push_back(stream_decl);

  add_timeline_action(out, 0, SyntheticEventType::OpenDevice, kDeviceKey, nullptr, false, PictureConfig{});
  add_timeline_action(out, 0, SyntheticEventType::CreateStream, nullptr, kMainStreamKey, false, PictureConfig{});
  add_timeline_action(out, 0, SyntheticEventType::StartStream, nullptr, kMainStreamKey, false, PictureConfig{});

  if (id == SyntheticBuiltinScenarioLibraryId::StreamLifecycleVersions) {
    PictureConfig checker{};
    checker.preset = PatternPreset::Checker;
    checker.seed = 3;
    checker.overlay_frame_index_offsets = false;
    checker.overlay_moving_bar = true;
    checker.checker_size_px = 12;
    add_timeline_action(out, 15'000'000, SyntheticEventType::UpdateStreamPicture, nullptr, kMainStreamKey, true, checker);
    add_timeline_action(out, 60'000'000, SyntheticEventType::StopStream, nullptr, kMainStreamKey, false, PictureConfig{});
    add_timeline_action(out, 60'000'001, SyntheticEventType::DestroyStream, nullptr, kMainStreamKey, false, PictureConfig{});
    add_timeline_action(out, 60'000'002, SyntheticEventType::CloseDevice, kDeviceKey, nullptr, false, PictureConfig{});
    return true;
  }

  if (id == SyntheticBuiltinScenarioLibraryId::PublicationCoalescing) {
    PictureConfig p0{};
    p0.preset = PatternPreset::Solid;
    p0.overlay_frame_index_offsets = false;
    p0.overlay_moving_bar = false;
    p0.solid_r = 220;
    p0.solid_g = 40;
    p0.solid_b = 40;
    add_timeline_action(out, 10'000'000, SyntheticEventType::UpdateStreamPicture, nullptr, kMainStreamKey, true, p0);

    PictureConfig p1{};
    p1.preset = PatternPreset::Solid;
    p1.overlay_frame_index_offsets = false;
    p1.overlay_moving_bar = false;
    p1.solid_r = 40;
    p1.solid_g = 210;
    p1.solid_b = 60;
    add_timeline_action(out, 20'000'000, SyntheticEventType::UpdateStreamPicture, nullptr, kMainStreamKey, true, p1);

    PictureConfig p2{};
    p2.preset = PatternPreset::Solid;
    p2.overlay_frame_index_offsets = false;
    p2.overlay_moving_bar = false;
    p2.solid_r = 60;
    p2.solid_g = 80;
    p2.solid_b = 220;
    add_timeline_action(out, 30'000'000, SyntheticEventType::UpdateStreamPicture, nullptr, kMainStreamKey, true, p2);

    add_timeline_action(out, 200'000'000, SyntheticEventType::StopStream, nullptr, kMainStreamKey, false, PictureConfig{});
    add_timeline_action(out, 200'000'001, SyntheticEventType::DestroyStream, nullptr, kMainStreamKey, false, PictureConfig{});
    add_timeline_action(out, 200'000'002, SyntheticEventType::CloseDevice, kDeviceKey, nullptr, false, PictureConfig{});
    return true;
  }

  if (id == SyntheticBuiltinScenarioLibraryId::TopologyChangeVersions) {
    PictureConfig noise{};
    noise.preset = PatternPreset::NoiseAnimated;
    noise.seed = 99;
    noise.overlay_frame_index_offsets = true;
    noise.overlay_moving_bar = true;
    add_timeline_action(out, 15'000'000, SyntheticEventType::UpdateStreamPicture, nullptr, kMainStreamKey, true, noise);

    SyntheticScenarioStreamDeclaration probe_decl{};
    probe_decl.key = kProbeStreamKey;
    probe_decl.device_key = kDeviceKey;
    probe_decl.intent = StreamIntent::PREVIEW;
    probe_decl.baseline_capture_profile = baseline_profile;
    // Preserve the explicit create/destroy timing for the probe stream.
    out.streams.push_back(probe_decl);

    add_timeline_action(out, 50'000'000, SyntheticEventType::CreateStream, nullptr, kProbeStreamKey, false, PictureConfig{});
    add_timeline_action(out, 50'000'001, SyntheticEventType::DestroyStream, nullptr, kProbeStreamKey, false, PictureConfig{});
    add_timeline_action(out, 100'000'000, SyntheticEventType::StopStream, nullptr, kMainStreamKey, false, PictureConfig{});
    add_timeline_action(out, 100'000'001, SyntheticEventType::DestroyStream, nullptr, kMainStreamKey, false, PictureConfig{});
    add_timeline_action(out, 100'000'002, SyntheticEventType::CloseDevice, kDeviceKey, nullptr, false, PictureConfig{});
    return true;
  }

  if (error) {
    *error = "unknown synthetic builtin scenario library id";
  }
  return false;
}

} // namespace cambang

#include "smoke/scenario/scenario_catalog.h"

#include "dev/cli_log.h"

#include <iostream>

namespace cambang {
namespace {

bool check_step(int index, const SnapshotExpectation& exp, const ObservedSnapshot& observed) {
  std::string error;
  if (!exp.matches(observed, error)) {
    cli::error("step ", index, " FAIL (", error, ")");
    return false;
  }
  cli::line("step ", index, " OK");
  return true;
}

bool fail_step(int index, const std::string& message) {
  cli::error("step ", index, " FAIL (", message, ")");
  return false;
}

int baseline_start(ScenarioProviderKind provider_kind) {
  ScenarioHarness h(provider_kind);
  std::string error;
  if (!h.start_runtime(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  if (!h.wait_for_core_snapshot([](const CamBANGStateSnapshot&) { return true; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  if (!check_step(0, SnapshotExpectation{}.is_nil(true), h.observed())) {
    return 1;
  }

  h.tick();
  if (!check_step(1,
                  SnapshotExpectation{}.is_nil(false).gen(0).version(0).topology_version(0).device_count(0).stream_count(0),
                  h.observed())) {
    return 1;
  }

  cli::line("Scenario PASSED");
  return 0;
}

int restart_nil_before_baseline(ScenarioProviderKind provider_kind) {
  ScenarioHarness h(provider_kind);
  std::string error;
  if (!h.start_runtime(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!h.wait_for_core_snapshot([](const CamBANGStateSnapshot&) { return true; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();

  h.stop_runtime();

  if (!h.start_runtime(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!h.wait_for_core_snapshot([](const CamBANGStateSnapshot& s) { return s.gen == 1; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  if (!check_step(0, SnapshotExpectation{}.is_nil(true), h.observed())) {
    return 1;
  }

  cli::line("Scenario PASSED");
  return 0;
}

int stream_lifecycle_versions(ScenarioProviderKind provider_kind) {
  ScenarioHarness h(provider_kind);
  std::string error;
  if (!h.start_runtime(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!h.wait_for_core_snapshot([](const CamBANGStateSnapshot&) { return true; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  h.tick();
  if (!check_step(0,
                  SnapshotExpectation{}.is_nil(false).gen(0).version(0).topology_version(0).device_count(0).stream_count(0),
                  h.observed())) {
    return 1;
  }

  if (!h.open_device(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(1, SnapshotExpectation{}.version(1).topology_version(1).device_count(1).stream_count(0), h.observed())) {
    return 1;
  }

  if (!h.create_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(2, SnapshotExpectation{}.version(2).topology_version(2).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  if (!h.start_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(3, SnapshotExpectation{}.version(3).topology_version(2).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  if (!h.emit_frame(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(4, SnapshotExpectation{}.version(4).topology_version(2).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  if (!h.stop_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(5, SnapshotExpectation{}.version(5).topology_version(2).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  cli::line("Scenario PASSED");
  return 0;
}

int topology_change_versions(ScenarioProviderKind provider_kind) {
  ScenarioHarness h(provider_kind);
  std::string error;
  if (!h.start_runtime(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!h.wait_for_core_snapshot([](const CamBANGStateSnapshot&) { return true; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  h.tick();
  if (!check_step(0, SnapshotExpectation{}.version(0).topology_version(0).device_count(0).stream_count(0), h.observed())) {
    return 1;
  }

  if (!h.open_device(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(1, SnapshotExpectation{}.version(1).topology_version(1).device_count(1).stream_count(0), h.observed())) {
    return 1;
  }

  if (!h.create_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(2, SnapshotExpectation{}.version(2).topology_version(2).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  if (!h.destroy_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(3, SnapshotExpectation{}.version(3).topology_version(3).device_count(1).stream_count(0), h.observed())) {
    return 1;
  }

  if (!h.close_device(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(4, SnapshotExpectation{}.version(4).topology_version(4).device_count(0).stream_count(0), h.observed())) {
    return 1;
  }

  cli::line("Scenario PASSED");
  return 0;
}

int publication_coalescing(ScenarioProviderKind provider_kind) {
  ScenarioHarness h(provider_kind);
  std::string error;
  if (!h.start_runtime(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!h.wait_for_core_snapshot([](const CamBANGStateSnapshot&) { return true; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  h.tick();
  if (!check_step(0, SnapshotExpectation{}.version(0).topology_version(0).device_count(0).stream_count(0), h.observed())) {
    return 1;
  }

  if (!h.perform_coalesced_burst(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  const ObservedSnapshot before_tick = h.observed();
  if (!SnapshotExpectation{}.version(0).topology_version(0).device_count(0).stream_count(0).matches(before_tick, error)) {
    cli::error("step 1 FAIL (state changed before observation boundary)");
    return 1;
  }

  h.tick();
  if (!check_step(1, SnapshotExpectation{}.version(1).topology_version(1).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  cli::line("Scenario PASSED");
  return 0;
}

int device_disconnect(ScenarioProviderKind provider_kind) {
  if (provider_kind != ScenarioProviderKind::Synthetic) {
    cli::line("SKIP: scenario 'device_disconnect' requires SyntheticProvider timeline support");
    return 0;
  }

  ScenarioHarness h(provider_kind);
  std::string error;
  if (!h.start_runtime(error) ||
      !h.wait_for_core_snapshot([](const CamBANGStateSnapshot&) { return true; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  h.tick();
  if (!check_step(0, SnapshotExpectation{}.version(0).topology_version(0).device_count(0).stream_count(0), h.observed())) {
    return 1;
  }

  if (!h.open_device(error) || !h.create_stream(error) || !h.start_stream(error) || !h.emit_frame(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(1, SnapshotExpectation{}.version(1).topology_version(1).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  if (!h.inject_provider_stream_stop(ScenarioHarness::kStreamId, ProviderError::ERR_PROVIDER_FAILED, error) ||
      !h.inject_provider_stream_destroyed(ScenarioHarness::kStreamId, error) ||
      !h.inject_provider_device_closed(ScenarioHarness::kDeviceId, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(2, SnapshotExpectation{}.version(2).topology_version(2).device_count(0).stream_count(0), h.observed())) {
    return 1;
  }

  cli::line("Scenario PASSED");
  return 0;
}

int close_while_streaming(ScenarioProviderKind provider_kind) {
  ScenarioHarness h(provider_kind);
  std::string error;
  if (!h.start_runtime(error) ||
      !h.wait_for_core_snapshot([](const CamBANGStateSnapshot&) { return true; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  h.tick();
  if (!check_step(0, SnapshotExpectation{}.version(0).topology_version(0).device_count(0).stream_count(0), h.observed())) {
    return 1;
  }

  if (!h.open_device(error) || !h.create_stream(error) || !h.start_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(1, SnapshotExpectation{}.version(1).topology_version(1).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  const ProviderResult close_result = h.close_device_result(ScenarioHarness::kDeviceId);
  if (close_result.ok() || close_result.code != ProviderError::ERR_BAD_STATE) {
    fail_step(2, "close_device did not fail with live child stream");
    return 1;
  }
  if (!check_step(2, SnapshotExpectation{}.version(1).topology_version(1).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  if (!h.stop_stream(error) || !h.destroy_stream(error) || !h.close_device(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(3, SnapshotExpectation{}.version(2).topology_version(2).device_count(0).stream_count(0), h.observed())) {
    return 1;
  }

  cli::line("Scenario PASSED");
  return 0;
}

int frame_starvation(ScenarioProviderKind provider_kind) {
  if (provider_kind != ScenarioProviderKind::Synthetic) {
    cli::line("SKIP: scenario 'frame_starvation' requires SyntheticProvider timeline support");
    return 0;
  }

  ScenarioHarness h(provider_kind);
  std::string error;
  if (!h.start_runtime(error) ||
      !h.wait_for_core_snapshot([](const CamBANGStateSnapshot&) { return true; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  h.tick();
  if (!check_step(0, SnapshotExpectation{}.version(0).topology_version(0).device_count(0).stream_count(0), h.observed())) {
    return 1;
  }

  if (!h.open_device(error) || !h.create_stream(error) || !h.start_stream(error) || !h.emit_frame(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(1, SnapshotExpectation{}.version(1).topology_version(1).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  const auto* flowing = ScenarioHarness::find_stream(*h.observed().raw, ScenarioHarness::kStreamId);
  if (!flowing || flowing->mode != CBStreamMode::FLOWING) {
    fail_step(2, "stream not flowing before starvation window");
    return 1;
  }
  const uint64_t before_frames = flowing->frames_received;
  if (!h.request_publish_only(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(2, SnapshotExpectation{}.version(2).topology_version(1).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  const auto* after = ScenarioHarness::find_stream(*h.observed().raw, ScenarioHarness::kStreamId);
  if (!after) {
    fail_step(3, "stream missing after starvation window");
    return 1;
  }
  if (after->frames_received != before_frames) {
    fail_step(3, "frames advanced during starvation window");
    return 1;
  }
  if (after->mode != CBStreamMode::FLOWING) {
    fail_step(3, "stream changed mode during starvation window");
    return 1;
  }
  cli::line("step 3 OK");

  cli::line("Scenario PASSED");
  return 0;
}

int provider_error_mid_stream(ScenarioProviderKind provider_kind) {
  if (provider_kind != ScenarioProviderKind::Synthetic) {
    cli::line("SKIP: scenario 'provider_error_mid_stream' requires SyntheticProvider timeline support");
    return 0;
  }

  ScenarioHarness h(provider_kind);
  std::string error;
  if (!h.start_runtime(error) ||
      !h.wait_for_core_snapshot([](const CamBANGStateSnapshot&) { return true; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  h.tick();
  if (!check_step(0, SnapshotExpectation{}.version(0).topology_version(0).device_count(0).stream_count(0), h.observed())) {
    return 1;
  }

  if (!h.open_device(error) || !h.create_stream(error) || !h.start_stream(error) || !h.emit_frame(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(1, SnapshotExpectation{}.version(1).topology_version(1).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  h.inject_provider_stream_error(ScenarioHarness::kStreamId, ProviderError::ERR_BAD_STATE);
  if (!h.inject_provider_stream_stop(ScenarioHarness::kStreamId, ProviderError::ERR_BAD_STATE, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(2, SnapshotExpectation{}.version(2).topology_version(1).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  const auto* stream = h.runtime().stream_record(ScenarioHarness::kStreamId);
  if (!stream || stream->started || stream->last_error_code == 0) {
    fail_step(3, "provider error did not propagate to stream record");
    return 1;
  }
  cli::line("step 3 OK");

  cli::line("Scenario PASSED");
  return 0;
}

int redundant_stop(ScenarioProviderKind provider_kind) {
  ScenarioHarness h(provider_kind);
  std::string error;
  if (!h.start_runtime(error) ||
      !h.wait_for_core_snapshot([](const CamBANGStateSnapshot&) { return true; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  h.tick();
  if (!check_step(0, SnapshotExpectation{}.version(0).topology_version(0).device_count(0).stream_count(0), h.observed())) {
    return 1;
  }

  if (!h.open_device(error) || !h.create_stream(error) || !h.start_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(1, SnapshotExpectation{}.version(1).topology_version(1).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  if (!h.stop_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(2, SnapshotExpectation{}.version(2).topology_version(1).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  if (!h.stop_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(3, SnapshotExpectation{}.version(3).topology_version(1).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  const auto* stream = ScenarioHarness::find_stream(*h.observed().raw, ScenarioHarness::kStreamId);
  if (!stream || stream->mode != CBStreamMode::STOPPED) {
    fail_step(4, "stream not stable after redundant stop");
    return 1;
  }
  cli::line("step 4 OK");

  cli::line("Scenario PASSED");
  return 0;
}

int multi_device_topology_change(ScenarioProviderKind provider_kind) {
  if (provider_kind != ScenarioProviderKind::Synthetic) {
    cli::line("SKIP: scenario 'multi_device_topology_change' requires SyntheticProvider timeline support");
    return 0;
  }

  ScenarioHarness h(provider_kind);
  std::string error;
  if (!h.start_runtime(error) ||
      !h.wait_for_core_snapshot([](const CamBANGStateSnapshot&) { return true; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  h.tick();
  if (!check_step(0, SnapshotExpectation{}.version(0).topology_version(0).device_count(0).stream_count(0), h.observed())) {
    return 1;
  }

  if (!h.open_device_id(ScenarioHarness::kDeviceId, 0, ScenarioHarness::kRootId, error) ||
      !h.open_device_id(ScenarioHarness::kDeviceBId, 1, ScenarioHarness::kRootBId, error) ||
      !h.create_stream_id(ScenarioHarness::kStreamId, ScenarioHarness::kDeviceId, 1, error) ||
      !h.create_stream_id(ScenarioHarness::kStreamBId, ScenarioHarness::kDeviceBId, 2, error) ||
      !h.start_stream_id(ScenarioHarness::kStreamId, error) ||
      !h.start_stream_id(ScenarioHarness::kStreamBId, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(1, SnapshotExpectation{}.version(1).topology_version(1).device_count(2).stream_count(2), h.observed())) {
    return 1;
  }

  if (!h.stop_stream_id(ScenarioHarness::kStreamId, error) ||
      !h.destroy_stream_id(ScenarioHarness::kStreamId, error) ||
      !h.close_device_id(ScenarioHarness::kDeviceId, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(2, SnapshotExpectation{}.version(2).topology_version(2).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  const auto* remaining_stream = ScenarioHarness::find_stream(*h.observed().raw, ScenarioHarness::kStreamBId);
  if (!remaining_stream || remaining_stream->device_instance_id != ScenarioHarness::kDeviceBId ||
      remaining_stream->mode != CBStreamMode::FLOWING) {
    fail_step(3, "remaining device/stream topology corrupted");
    return 1;
  }
  cli::line("step 3 OK");

  cli::line("Scenario PASSED");
  return 0;
}

} // namespace

std::vector<ScenarioDefinition> scenario_catalog(ScenarioProviderKind provider_kind) {
  return {
      {"baseline_start", [provider_kind]() { return baseline_start(provider_kind); }},
      {"restart_nil_before_baseline", [provider_kind]() { return restart_nil_before_baseline(provider_kind); }},
      {"stream_lifecycle_versions", [provider_kind]() { return stream_lifecycle_versions(provider_kind); }},
      {"topology_change_versions", [provider_kind]() { return topology_change_versions(provider_kind); }},
      {"publication_coalescing", [provider_kind]() { return publication_coalescing(provider_kind); }},
      {"device_disconnect", [provider_kind]() { return device_disconnect(provider_kind); }},
      {"close_while_streaming", [provider_kind]() { return close_while_streaming(provider_kind); }},
      {"frame_starvation", [provider_kind]() { return frame_starvation(provider_kind); }},
      {"provider_error_mid_stream", [provider_kind]() { return provider_error_mid_stream(provider_kind); }},
      {"redundant_stop", [provider_kind]() { return redundant_stop(provider_kind); }},
      {"multi_device_topology_change", [provider_kind]() { return multi_device_topology_change(provider_kind); }},
  };
}

} // namespace cambang

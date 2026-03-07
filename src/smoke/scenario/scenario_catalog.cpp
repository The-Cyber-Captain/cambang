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
  if (!check_step(1, SnapshotExpectation{}
                        .is_nil(false)
                        .gen(0)
                        .version(0)
                        .topology_version(0)
                        .device_count(0)
                        .stream_count(0), h.observed())) {
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
  if (!check_step(0, SnapshotExpectation{}.is_nil(false).gen(0).version(0).topology_version(0).device_count(0).stream_count(0), h.observed())) {
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

} // namespace

std::vector<ScenarioDefinition> scenario_catalog(ScenarioProviderKind provider_kind) {
  return {
      {"baseline_start", [provider_kind]() { return baseline_start(provider_kind); }},
      {"restart_nil_before_baseline", [provider_kind]() { return restart_nil_before_baseline(provider_kind); }},
      {"stream_lifecycle_versions", [provider_kind]() { return stream_lifecycle_versions(provider_kind); }},
      {"topology_change_versions", [provider_kind]() { return topology_change_versions(provider_kind); }},
      {"publication_coalescing", [provider_kind]() { return publication_coalescing(provider_kind); }},
  };
}

} // namespace cambang

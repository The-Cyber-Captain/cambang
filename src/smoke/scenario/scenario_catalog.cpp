#include "smoke/scenario/scenario_catalog.h"

#include "dev/cli_log.h"

#include <iostream>

namespace cambang {
namespace {

size_t count_native_type(const CamBANGStateSnapshot& snap, NativeObjectType type) {
  const uint32_t want = static_cast<uint32_t>(type);
  size_t count = 0;
  for (const auto& rec : snap.native_objects) {
    if (rec.type == want) {
      ++count;
    }
  }
  return count;
}

bool check_step(int index, const SnapshotExpectation& exp, const ObservedSnapshot& observed);
bool expect_step(int index, const SnapshotExpectation& exp, const ObservedSnapshot& observed);
bool fail_step(int index, const std::string& message);

const char* native_type_name(uint32_t type) {
  switch (static_cast<NativeObjectType>(type)) {
    case NativeObjectType::Provider: return "provider";
    case NativeObjectType::Device: return "device";
    case NativeObjectType::Stream: return "stream";
    case NativeObjectType::FrameProducer: return "frameproducer";
  }
  return "unknown";
}

const char* phase_name(CBLifecyclePhase phase) {
  switch (phase) {
    case CBLifecyclePhase::CREATED: return "CREATED";
    case CBLifecyclePhase::LIVE: return "LIVE";
    case CBLifecyclePhase::TEARING_DOWN: return "TEARING_DOWN";
    case CBLifecyclePhase::DESTROYED: return "DESTROYED";
  }
  return "UNKNOWN";
}

struct NativeShapeSummary {
  size_t current_generation_provider_count = 0;
  size_t current_generation_device_count = 0;
  size_t current_generation_stream_count = 0;
  size_t current_generation_frameproducer_count = 0;
  size_t stale_prior_generation_count = 0;
};

NativeShapeSummary summarize_native_shape(const CamBANGStateSnapshot& snap, uint64_t current_gen) {
  NativeShapeSummary summary{};
  for (const auto& rec : snap.native_objects) {
    if (rec.creation_gen != current_gen) {
      ++summary.stale_prior_generation_count;
      continue;
    }
    switch (static_cast<NativeObjectType>(rec.type)) {
      case NativeObjectType::Provider: ++summary.current_generation_provider_count; break;
      case NativeObjectType::Device: ++summary.current_generation_device_count; break;
      case NativeObjectType::Stream: ++summary.current_generation_stream_count; break;
      case NativeObjectType::FrameProducer: ++summary.current_generation_frameproducer_count; break;
    }
  }
  return summary;
}

void log_restarted_baseline_diagnostic(int step_index, const ObservedSnapshot& observed) {
  cli::error("step ", step_index, " detail: gen=", observed.gen,
             " version=", observed.version,
             " topology_version=", observed.topology_version,
             " devices=", observed.device_count,
             " streams=", observed.stream_count,
             " native_objects=", (observed.raw ? observed.raw->native_objects.size() : 0));

  if (!observed.raw) {
    cli::error("step ", step_index, " detail: raw snapshot missing");
    return;
  }

  const NativeShapeSummary summary = summarize_native_shape(*observed.raw, observed.gen);
  cli::error("step ", step_index, " detail: current_gen_provider_present=",
             (summary.current_generation_provider_count > 0 ? "yes" : "no"),
             " current_gen_device_descendants_present=",
             (summary.current_generation_device_count > 0 ? "yes" : "no"),
             " current_gen_stream_descendants_present=",
             (summary.current_generation_stream_count > 0 ? "yes" : "no"),
             " any_frameproducer_descendants_present=",
             (summary.current_generation_frameproducer_count > 0 ? "yes" : "no"),
             " stale_prior_generation_native_objects_present=",
             (summary.stale_prior_generation_count > 0 ? "yes" : "no"));

  for (const auto& rec : observed.raw->native_objects) {
    cli::error("step ", step_index, " native_object native_id=", rec.native_id,
               " type=", native_type_name(rec.type),
               " phase=", phase_name(rec.phase),
               " creation_gen=", rec.creation_gen,
               " root_id=", rec.root_id,
               " owner_provider_native_id=", rec.owner_provider_native_id,
               " owner_rig_id=", rec.owner_rig_id,
               " owner_device_instance_id=", rec.owner_device_instance_id,
               " owner_stream_id=", rec.owner_stream_id);
  }
}

bool is_provider_only_authoritative_snapshot(const CamBANGStateSnapshot& snap, uint64_t current_gen) {
  const NativeShapeSummary summary = summarize_native_shape(snap, current_gen);
  if (summary.stale_prior_generation_count != 0) {
    return false;
  }
  if (summary.current_generation_provider_count != 1 ||
      summary.current_generation_device_count != 0 ||
      summary.current_generation_stream_count != 0 ||
      summary.current_generation_frameproducer_count != 0) {
    return false;
  }
  if (snap.native_objects.size() != 1) {
    return false;
  }

  const auto& provider = snap.native_objects.front();
  return provider.creation_gen == current_gen &&
         provider.type == static_cast<uint32_t>(NativeObjectType::Provider) &&
         provider.phase == CBLifecyclePhase::LIVE;
}

bool is_provider_pending_snapshot(const CamBANGStateSnapshot& snap, uint64_t current_gen) {
  const NativeShapeSummary summary = summarize_native_shape(snap, current_gen);
  return summary.current_generation_provider_count == 0 &&
         summary.current_generation_device_count == 0 &&
         summary.current_generation_stream_count == 0 &&
         summary.current_generation_frameproducer_count == 0 &&
         summary.stale_prior_generation_count == 0 &&
         snap.native_objects.empty();
}

bool observe_provider_only_authoritative_start(ScenarioHarness& h,
                                               std::string& error,
                                               int step_index,
                                               uint64_t want_gen,
                                               uint64_t& settled_version,
                                               const char* no_publish_message,
                                               const char* not_provider_only_message) {
  if (!h.tick()) {
    fail_step(step_index, no_publish_message);
    return false;
  }
  if (!check_step(step_index,
                  SnapshotExpectation{}.gen(want_gen).version(0).topology_version(0).device_count(0).stream_count(0),
                  h.observed())) {
    return false;
  }
  if (!h.observed().raw) {
    log_restarted_baseline_diagnostic(step_index, h.observed());
    fail_step(step_index, "expected raw snapshot for startup baseline");
    return false;
  }

  settled_version = 0;
  if (is_provider_pending_snapshot(*h.observed().raw, want_gen)) {
    if (!h.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
          return s.gen == want_gen && is_provider_only_authoritative_snapshot(s, want_gen);
        }, error)) {
      cli::error("FAIL: ", error);
      return false;
    }
    if (!h.tick()) {
      fail_step(step_index, "expected provider-only authoritative publish after provider_pending baseline");
      return false;
    }
    if (!expect_step(step_index,
                     SnapshotExpectation{}.gen(want_gen).version(1).topology_version(0).device_count(0).stream_count(0),
                     h.observed())) {
      return false;
    }
    settled_version = 1;
  }

  if (!h.observed().raw || !is_provider_only_authoritative_snapshot(*h.observed().raw, want_gen)) {
    log_restarted_baseline_diagnostic(step_index, h.observed());
    fail_step(step_index, not_provider_only_message);
    return false;
  }
  return true;
}

bool check_step(int index, const SnapshotExpectation& exp, const ObservedSnapshot& observed) {
  std::string error;
  if (!exp.matches(observed, error)) {
    cli::error("step ", index, " FAIL (", error, ")");
    return false;
  }
  cli::line("step ", index, " OK");
  return true;
}

bool expect_step(int index, const SnapshotExpectation& exp, const ObservedSnapshot& observed) {
  std::string error;
  if (!exp.matches(observed, error)) {
    cli::error("step ", index, " FAIL (", error, ")");
    return false;
  }
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

int provider_only_authoritative_baseline(ScenarioProviderKind provider_kind) {
  ScenarioHarness h(provider_kind);
  std::string error;

  auto check_provider_only_snapshot = [&](int step_index, uint64_t want_gen) -> bool {
    uint64_t settled_version = 0;
    if (!observe_provider_only_authoritative_start(h,
                                                   error,
                                                   step_index,
                                                   want_gen,
                                                   settled_version,
                                                   "expected baseline publish for provider-only startup state",
                                                   "startup state did not settle to provider-only authoritative truth")) {
      return false;
    }

    const auto& observed = h.observed();
    if (!observed.raw) {
      return fail_step(step_index, "expected raw snapshot");
    }
    if (!observed.raw->devices.empty()) {
      return fail_step(step_index, "expected no devices in provider-only baseline");
    }
    if (!observed.raw->streams.empty()) {
      return fail_step(step_index, "expected no streams in provider-only baseline");
    }
    if (observed.raw->native_objects.size() != 1) {
      return fail_step(step_index, "expected exactly one native object in provider-only baseline");
    }

    const auto& provider_native = observed.raw->native_objects[0];
    if (provider_native.type != static_cast<uint32_t>(NativeObjectType::Provider)) {
      return fail_step(step_index, "expected sole native object to be Provider");
    }
    if (provider_native.phase != CBLifecyclePhase::LIVE) {
      return fail_step(step_index, "expected provider native object to be LIVE");
    }
    if (provider_native.creation_gen != want_gen) {
      return fail_step(step_index, "expected provider creation_gen to match snapshot gen");
    }
    if (observed.version != settled_version) {
      return fail_step(step_index, "expected observed version to match settled provider-only publication");
    }
    if (!observed.raw->detached_root_ids.empty()) {
      return fail_step(step_index, "expected no detached roots in provider-only baseline");
    }
    return true;
  };

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

  if (!check_provider_only_snapshot(1, 0)) {
    return 1;
  }
  cli::line("step 1 detail OK");

  h.stop_runtime();

  if (!h.start_runtime(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!h.wait_for_core_snapshot([](const CamBANGStateSnapshot& s) { return s.gen == 1; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!check_step(2, SnapshotExpectation{}.is_nil(true), h.observed())) {
    return 1;
  }

  if (!check_provider_only_snapshot(3, 1)) {
    return 1;
  }
  cli::line("step 3 detail OK");

  cli::line("Scenario PASSED");
  return 0;
}

int provider_only_to_realized(ScenarioProviderKind provider_kind) {
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

  uint64_t baseline_version = 0;
  if (!observe_provider_only_authoritative_start(h,
                                                 error,
                                                 1,
                                                 0,
                                                 baseline_version,
                                                 "expected baseline publish on first boundary tick",
                                                 "startup state did not settle to provider-only authoritative truth")) {
    return 1;
  }
  if (!h.observed().raw || count_native_type(*h.observed().raw, NativeObjectType::Provider) != 1 ||
      count_native_type(*h.observed().raw, NativeObjectType::Device) != 0 ||
      count_native_type(*h.observed().raw, NativeObjectType::Stream) != 0 ||
      count_native_type(*h.observed().raw, NativeObjectType::FrameProducer) != 0) {
    fail_step(1, "provider-only baseline native-object shape mismatch");
    return 1;
  }
  cli::line("step 1 detail OK");

  if (h.tick()) {
    fail_step(2, "unexpected repeated provider-only publication without new runtime facts");
    return 1;
  }
  if (!check_step(2,
                  SnapshotExpectation{}
                      .is_nil(false)
                      .gen(0)
                      .version(baseline_version)
                      .topology_version(0)
                      .device_count(0)
                      .stream_count(0),
                  h.observed())) {
    return 1;
  }

  if (!h.open_device(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!h.tick()) {
    fail_step(3, "expected device realization publish after open_device");
    return 1;
  }
  if (!check_step(3,
                  SnapshotExpectation{}
                      .gen(0)
                      .version(baseline_version + 1)
                      .topology_version(1)
                      .device_count(1)
                      .stream_count(0),
                  h.observed())) {
    return 1;
  }
  if (!h.observed().raw || count_native_type(*h.observed().raw, NativeObjectType::Provider) != 1 ||
      count_native_type(*h.observed().raw, NativeObjectType::Device) != 1 ||
      count_native_type(*h.observed().raw, NativeObjectType::Stream) != 0 ||
      count_native_type(*h.observed().raw, NativeObjectType::FrameProducer) != 0) {
    log_restarted_baseline_diagnostic(3, h.observed());
    fail_step(3, "device realization native-object shape mismatch");
    return 1;
  }
  cli::line("step 3 detail OK");

  if (!h.create_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!h.tick()) {
    fail_step(4, "expected stream realization publish after create_stream");
    return 1;
  }
  if (!check_step(4,
                  SnapshotExpectation{}
                      .gen(0)
                      .version(baseline_version + 2)
                      .topology_version(2)
                      .device_count(1)
                      .stream_count(1),
                  h.observed())) {
    return 1;
  }
  if (!h.observed().raw || count_native_type(*h.observed().raw, NativeObjectType::Provider) != 1 ||
      count_native_type(*h.observed().raw, NativeObjectType::Device) != 1 ||
      count_native_type(*h.observed().raw, NativeObjectType::Stream) != 1 ||
      count_native_type(*h.observed().raw, NativeObjectType::FrameProducer) != 0) {
    log_restarted_baseline_diagnostic(4, h.observed());
    fail_step(4, "stream realization native-object shape mismatch");
    return 1;
  }
  cli::line("step 4 detail OK");

  if (!h.start_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!h.tick()) {
    fail_step(5, "expected frameproducer realization publish after start_stream");
    return 1;
  }
  if (!check_step(5,
                  SnapshotExpectation{}
                      .gen(0)
                      .version(baseline_version + 3)
                      .topology_version(2)
                      .device_count(1)
                      .stream_count(1),
                  h.observed())) {
    return 1;
  }
  if (!h.observed().raw || count_native_type(*h.observed().raw, NativeObjectType::Provider) != 1 ||
      count_native_type(*h.observed().raw, NativeObjectType::Device) != 1 ||
      count_native_type(*h.observed().raw, NativeObjectType::Stream) != 1 ||
      count_native_type(*h.observed().raw, NativeObjectType::FrameProducer) != 1) {
    log_restarted_baseline_diagnostic(5, h.observed());
    fail_step(5, "full realization native-object shape mismatch");
    return 1;
  }
  cli::line("step 5 detail OK");

  cli::line("Scenario PASSED");
  return 0;
}

int provider_only_then_stop(ScenarioProviderKind provider_kind) {
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

  uint64_t baseline_version = 0;
  if (!observe_provider_only_authoritative_start(h,
                                                 error,
                                                 1,
                                                 0,
                                                 baseline_version,
                                                 "expected provider-only baseline publish before stop",
                                                 "startup state did not settle to provider-only authoritative truth before stop")) {
    return 1;
  }
  if (!h.observed().raw || count_native_type(*h.observed().raw, NativeObjectType::Provider) != 1 ||
      count_native_type(*h.observed().raw, NativeObjectType::Device) != 0 ||
      count_native_type(*h.observed().raw, NativeObjectType::Stream) != 0 ||
      count_native_type(*h.observed().raw, NativeObjectType::FrameProducer) != 0) {
    fail_step(1, "provider-only baseline native-object shape mismatch before stop");
    return 1;
  }

  h.stop_runtime();

  if (!check_step(2, SnapshotExpectation{}.is_nil(true), h.observed())) {
    return 1;
  }
  if (!h.last_snapshot_before_stop_clear().is_nil) {
    const auto* final_raw = h.last_snapshot_before_stop_clear().raw.get();
    if (final_raw && (count_native_type(*final_raw, NativeObjectType::Device) != 0 ||
                      count_native_type(*final_raw, NativeObjectType::Stream) != 0 ||
                      count_native_type(*final_raw, NativeObjectType::FrameProducer) != 0)) {
      fail_step(2, "stop boundary fabricated descendants before NIL clear");
      return 1;
    }
  }
  cli::line("step 2 detail OK");

  cli::line("Scenario PASSED");
  return 0;
}

int repeated_provider_only_across_generations(ScenarioProviderKind provider_kind) {
  ScenarioHarness h(provider_kind);
  std::string error;

  for (uint64_t gen = 0; gen < 3; ++gen) {
    const int pre_baseline_step = static_cast<int>(gen * 2);
    const int baseline_step = static_cast<int>(gen * 2 + 1);
    if (!h.start_runtime(error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!h.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) { return s.gen == gen; }, error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!check_step(pre_baseline_step, SnapshotExpectation{}.is_nil(true), h.observed())) {
      return 1;
    }
    uint64_t settled_version = 0;
    if (!observe_provider_only_authoritative_start(h,
                                                   error,
                                                   baseline_step,
                                                   gen,
                                                   settled_version,
                                                   "expected baseline publish after restart churn",
                                                   "restarted generation did not settle to provider-only authoritative truth")) {
      return 1;
    }

    h.stop_runtime();
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
      {"provider_only_authoritative_baseline", [provider_kind]() { return provider_only_authoritative_baseline(provider_kind); }},
      {"provider_only_to_realized", [provider_kind]() { return provider_only_to_realized(provider_kind); }},
      {"provider_only_then_stop", [provider_kind]() { return provider_only_then_stop(provider_kind); }},
      {"repeated_provider_only_across_generations", [provider_kind]() { return repeated_provider_only_across_generations(provider_kind); }},
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

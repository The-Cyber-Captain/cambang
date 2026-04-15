#include "smoke/verify_case/verify_case_catalog.h"

#include "dev/cli_log.h"
#include "core/core_runtime.h"
#include "core/state_snapshot_buffer.h"
#include "core/synthetic_timeline_request_binding.h"
#include "imaging/broker/mode.h"
#include "imaging/broker/provider_broker.h"
#include "imaging/synthetic/scenario_model.h"

#include <iostream>
#include <chrono>
#include <string_view>
#include <thread>

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

bool wait_until_poll(const std::function<bool()>& pred,
                     std::string& error,
                     const char* timeout_message,
                     int max_iters = 500,
                     int sleep_ms = 5) {
  for (int i = 0; i < max_iters; ++i) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  error = timeout_message;
  return false;
}

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


enum class RestartChurnCutPoint : uint8_t {
  ProviderVisible = 0,
  DeviceIdentity = 1,
  DeviceNative = 2,
  StreamVisible = 3,
  Complete = 4,
};

const char* restart_churn_cut_point_name(RestartChurnCutPoint cut_point) {
  switch (cut_point) {
    case RestartChurnCutPoint::ProviderVisible: return "provider_visible";
    case RestartChurnCutPoint::DeviceIdentity: return "device_identity";
    case RestartChurnCutPoint::DeviceNative: return "device_native";
    case RestartChurnCutPoint::StreamVisible: return "stream_visible";
    case RestartChurnCutPoint::Complete: return "complete";
  }
  return "unknown";
}

RestartChurnCutPoint observed_restart_churn_stage(const CamBANGStateSnapshot& snap, uint64_t current_gen) {
  const bool provider_visible = count_native_type(snap, NativeObjectType::Provider) >= 1;
  const bool device_identity = !snap.devices.empty();

  bool device_native = false;
  bool frameproducer_visible = false;
  for (const auto& rec : snap.native_objects) {
    if (rec.creation_gen != current_gen) {
      continue;
    }
    if (rec.type == static_cast<uint32_t>(NativeObjectType::Device) && rec.owner_device_instance_id == VerifyCaseHarness::kDeviceId) {
      device_native = true;
    }
    if (rec.type == static_cast<uint32_t>(NativeObjectType::FrameProducer) &&
        rec.owner_device_instance_id == VerifyCaseHarness::kDeviceId &&
        rec.owner_stream_id == VerifyCaseHarness::kStreamId) {
      frameproducer_visible = true;
    }
  }

  if (frameproducer_visible) {
    return RestartChurnCutPoint::Complete;
  }
  if (!snap.streams.empty()) {
    return RestartChurnCutPoint::StreamVisible;
  }
  if (device_native) {
    return RestartChurnCutPoint::DeviceNative;
  }
  if (device_identity) {
    return RestartChurnCutPoint::DeviceIdentity;
  }
  if (provider_visible) {
    return RestartChurnCutPoint::ProviderVisible;
  }
  return RestartChurnCutPoint::ProviderVisible;
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

bool observe_provider_only_authoritative_start(VerifyCaseHarness& h,
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

int baseline_start(VerifyCaseProviderKind provider_kind) {
  VerifyCaseHarness h(provider_kind);
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

  cli::line("Verification case PASSED");
  return 0;
}

int provider_only_authoritative_baseline(VerifyCaseProviderKind provider_kind) {
  VerifyCaseHarness h(provider_kind);
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

  cli::line("Verification case PASSED");
  return 0;
}

int provider_only_to_realized(VerifyCaseProviderKind provider_kind, const RealizationProfilerOptions& profiler_options) {
  RealizationProfiler profiler(profiler_options);
  VerifyCaseHarness h(provider_kind);
  std::string error;
  if (profiler.enabled()) {
    h.set_realization_profiler(&profiler);
  }

  auto settle_native_shape = [&](int step_index,
                                 uint64_t want_gen,
                                 size_t want_device_count,
                                 size_t want_stream_count,
                                 const char* wait_timeout_message,
                                 const char* publish_message,
                                 const std::function<bool(const CamBANGStateSnapshot&)>& pred) -> bool {
    if (h.observed().raw && pred(*h.observed().raw)) {
      return true;
    }

    const ObservedSnapshot before = h.observed();
    if (!h.wait_for_core_snapshot(pred, error, 500, 5, wait_timeout_message)) {
      cli::error("FAIL: ", error);
      return false;
    }
    if (!h.tick()) {
      fail_step(step_index, publish_message);
      return false;
    }
    if (h.observed().is_nil || !h.observed().raw) {
      fail_step(step_index, "expected non-NIL snapshot while settling native shape");
      return false;
    }
    if (h.observed().gen != want_gen) {
      fail_step(step_index, "native-shape settle changed generation unexpectedly");
      return false;
    }
    if (h.observed().device_count != want_device_count || h.observed().stream_count != want_stream_count) {
      fail_step(step_index, "native-shape settle changed public device/stream counts unexpectedly");
      return false;
    }
    if (h.observed().version <= before.version) {
      fail_step(step_index, "expected native-shape settle to advance observable version");
      return false;
    }
    if (h.observed().topology_version < before.topology_version) {
      fail_step(step_index, "native-shape settle regressed topology_version");
      return false;
    }
    return pred(*h.observed().raw);
  };

  const auto is_device_realization_native_shape = [](const CamBANGStateSnapshot& s) {
    return count_native_type(s, NativeObjectType::Provider) == 1 &&
           count_native_type(s, NativeObjectType::Device) == 1 &&
           count_native_type(s, NativeObjectType::Stream) == 0 &&
           count_native_type(s, NativeObjectType::FrameProducer) == 0;
  };

  const auto is_stream_realization_native_shape = [](const CamBANGStateSnapshot& s) {
    return count_native_type(s, NativeObjectType::Provider) == 1 &&
           count_native_type(s, NativeObjectType::Device) == 1 &&
           count_native_type(s, NativeObjectType::Stream) == 1 &&
           count_native_type(s, NativeObjectType::FrameProducer) == 0;
  };

  const auto is_full_realization_native_shape = [](const CamBANGStateSnapshot& s) {
    return count_native_type(s, NativeObjectType::Provider) == 1 &&
           count_native_type(s, NativeObjectType::Device) == 1 &&
           count_native_type(s, NativeObjectType::Stream) == 1 &&
           count_native_type(s, NativeObjectType::FrameProducer) == 1;
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
  uint64_t current_version = h.observed().version;
  uint64_t current_topology_version = h.observed().topology_version;

  if (h.tick()) {
    if (!check_step(2,
                    SnapshotExpectation{}
                        .is_nil(false)
                        .gen(0)
                        .version(current_version + 1)
                        .topology_version(current_topology_version)
                        .device_count(0)
                        .stream_count(0),
                    h.observed())) {
      return 1;
    }
    if (!h.observed().raw || count_native_type(*h.observed().raw, NativeObjectType::Provider) != 1 ||
        count_native_type(*h.observed().raw, NativeObjectType::Device) != 0 ||
        count_native_type(*h.observed().raw, NativeObjectType::Stream) != 0 ||
        count_native_type(*h.observed().raw, NativeObjectType::FrameProducer) != 0) {
      log_restarted_baseline_diagnostic(2, h.observed());
      fail_step(2, "repeated provider-only publication changed native shape unexpectedly");
      return 1;
    }
    current_version = h.observed().version;
  } else if (!check_step(2,
                         SnapshotExpectation{}
                             .is_nil(false)
                             .gen(0)
                             .version(current_version)
                             .topology_version(current_topology_version)
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
                      .version(current_version + 1)
                      .topology_version(current_topology_version + 1)
                      .device_count(1)
                      .stream_count(0),
                  h.observed())) {
    return 1;
  }
  if (!settle_native_shape(3,
                           0,
                           1,
                           0,
                           "timed out waiting for device-descendant native shape after device realization",
                           "expected device-descendant native-object publish after device realization",
                           is_device_realization_native_shape)) {
    log_restarted_baseline_diagnostic(3, h.observed());
    fail_step(3, "device realization native-object shape mismatch");
    return 1;
  }
  cli::line("step 3 detail OK");
  current_version = h.observed().version;
  current_topology_version = h.observed().topology_version;

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
                      .version(current_version + 1)
                      .topology_version(current_topology_version + 1)
                      .device_count(1)
                      .stream_count(1),
                  h.observed())) {
    return 1;
  }
  if (!settle_native_shape(4,
                           0,
                           1,
                           1,
                           "timed out waiting for stream-descendant native shape after stream realization",
                           "expected stream-descendant native-object publish after stream realization",
                           is_stream_realization_native_shape)) {
    log_restarted_baseline_diagnostic(4, h.observed());
    fail_step(4, "stream realization native-object shape mismatch");
    return 1;
  }
  cli::line("step 4 detail OK");
  current_version = h.observed().version;
  current_topology_version = h.observed().topology_version;

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
                      .version(current_version + 1)
                      .topology_version(current_topology_version)
                      .device_count(1)
                      .stream_count(1),
                  h.observed())) {
    return 1;
  }
  if (!settle_native_shape(5,
                           0,
                           1,
                           1,
                           "timed out waiting for frameproducer native shape after start_stream",
                           "expected frameproducer native-object publish after start_stream",
                           is_full_realization_native_shape)) {
    log_restarted_baseline_diagnostic(5, h.observed());
    fail_step(5, "full realization native-object shape mismatch");
    return 1;
  }
  cli::line("step 5 detail OK");

  if (profiler.enabled()) {
    profiler.emit_report();
  }

  cli::line("Verification case PASSED");
  return 0;
}

int provider_only_then_stop(VerifyCaseProviderKind provider_kind) {
  VerifyCaseHarness h(provider_kind);
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

  cli::line("Verification case PASSED");
  return 0;
}

int repeated_provider_only_across_generations(VerifyCaseProviderKind provider_kind) {
  VerifyCaseHarness h(provider_kind);
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

  cli::line("Verification case PASSED");
  return 0;
}

int restart_churn_realization(VerifyCaseProviderKind provider_kind,
                              const RealizationProfilerOptions& profiler_options) {
  RealizationProfiler profiler(profiler_options);
  VerifyCaseHarness h(provider_kind);
  std::string error;
  if (profiler.enabled()) {
    h.set_realization_profiler(&profiler);
  }

  const std::vector<RestartChurnCutPoint> cut_points = {
      RestartChurnCutPoint::ProviderVisible,
      RestartChurnCutPoint::DeviceIdentity,
      RestartChurnCutPoint::ProviderVisible,
      RestartChurnCutPoint::DeviceNative,
      RestartChurnCutPoint::StreamVisible,
      RestartChurnCutPoint::Complete,
  };

  auto require_boundary_tick = [&](const char* message) -> bool {
    if (h.tick()) {
      return true;
    }
    cli::error("FAIL: ", message);
    return false;
  };

  auto emit_cycle_report = [&](size_t cycle_index, uint64_t gen, RestartChurnCutPoint planned_cut_point) {
    cli::line("[restart-churn] cycle=", cycle_index + 1,
              " gen=", gen,
              " planned_stop_after=", restart_churn_cut_point_name(planned_cut_point),
              " observed_stage=", restart_churn_cut_point_name(observed_restart_churn_stage(*h.observed().raw, gen)));
    if (profiler.enabled()) {
      profiler.emit_report();
    }
  };

  for (size_t cycle_index = 0; cycle_index < cut_points.size(); ++cycle_index) {
    const uint64_t want_gen = static_cast<uint64_t>(cycle_index);
    const RestartChurnCutPoint planned_cut_point = cut_points[cycle_index];

    cli::line("[restart-churn] cycle=", cycle_index + 1,
              " planned_stop_after=", restart_churn_cut_point_name(planned_cut_point),
              " start_requested");

    if (!h.start_runtime(error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!h.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) { return s.gen == want_gen; }, error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!check_step(static_cast<int>(cycle_index * 10), SnapshotExpectation{}.is_nil(true), h.observed())) {
      return 1;
    }

    uint64_t settled_version = 0;
    if (!observe_provider_only_authoritative_start(h,
                                                   error,
                                                   static_cast<int>(cycle_index * 10 + 1),
                                                   want_gen,
                                                   settled_version,
                                                   "expected provider-visible start during restart churn",
                                                   "restart churn generation did not settle to provider-visible authoritative truth")) {
      return 1;
    }

    if (planned_cut_point == RestartChurnCutPoint::ProviderVisible) {
      emit_cycle_report(cycle_index, want_gen, planned_cut_point);
      cli::line("[restart-churn] cycle=", cycle_index + 1, " stop_requested");
      h.stop_runtime();
      continue;
    }

    if (!h.open_device(error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!require_boundary_tick("expected observable publish after open_device during restart churn")) {
      return 1;
    }

    if (planned_cut_point == RestartChurnCutPoint::DeviceIdentity) {
      emit_cycle_report(cycle_index, want_gen, planned_cut_point);
      cli::line("[restart-churn] cycle=", cycle_index + 1, " stop_requested");
      h.stop_runtime();
      continue;
    }

    if (observed_restart_churn_stage(*h.observed().raw, want_gen) == RestartChurnCutPoint::DeviceIdentity) {
      if (!h.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
            return static_cast<int>(observed_restart_churn_stage(s, want_gen)) >=
                   static_cast<int>(RestartChurnCutPoint::DeviceNative);
          }, error, 500, 5, "timed out waiting for device native during restart churn")) {
        cli::error("FAIL: ", error);
        return 1;
      }
      if (!require_boundary_tick("expected observable publish for device native during restart churn")) {
        return 1;
      }
    }

    if (planned_cut_point == RestartChurnCutPoint::DeviceNative) {
      emit_cycle_report(cycle_index, want_gen, planned_cut_point);
      cli::line("[restart-churn] cycle=", cycle_index + 1, " stop_requested");
      h.stop_runtime();
      continue;
    }

    if (!h.create_stream(error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!require_boundary_tick("expected observable publish after create_stream during restart churn")) {
      return 1;
    }

    if (planned_cut_point == RestartChurnCutPoint::StreamVisible) {
      emit_cycle_report(cycle_index, want_gen, planned_cut_point);
      cli::line("[restart-churn] cycle=", cycle_index + 1, " stop_requested");
      h.stop_runtime();
      continue;
    }

    if (!h.start_stream(error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!require_boundary_tick("expected observable publish after start_stream during restart churn")) {
      return 1;
    }

    const auto is_complete = [&](const CamBANGStateSnapshot& s) {
      return observed_restart_churn_stage(s, want_gen) == RestartChurnCutPoint::Complete;
    };
    if (!is_complete(*h.observed().raw)) {
      if (!h.wait_for_core_snapshot(is_complete, error, 500, 5, "timed out waiting for completion during restart churn")) {
        cli::error("FAIL: ", error);
        return 1;
      }
      if (!require_boundary_tick("expected observable completion publish during restart churn")) {
        return 1;
      }
    }

    emit_cycle_report(cycle_index, want_gen, planned_cut_point);
    cli::line("[restart-churn] cycle=", cycle_index + 1, " stop_requested");
    h.stop_runtime();
  }

  cli::line("Verification case PASSED");
  return 0;
}

int restart_churn_then_settle(VerifyCaseProviderKind provider_kind,
                             const RealizationProfilerOptions& profiler_options) {
  constexpr uint64_t kSettlePublishWindow = 12;
  constexpr uint64_t kSettleTimeNs = 75'000'000ull;
  constexpr uint64_t kProfilerPublishGap = 8;
  constexpr uint64_t kProfilerTimeNs = 50'000'000ull;

  struct FinalSettleState {
    uint64_t provider_timestamp_ns = 0;
    uint64_t first_descendant_timestamp_ns = 0;
    uint64_t observed_publishes_since_provider = 0;
    bool provider_visible = false;
    bool device_identity = false;
    bool device_native = false;
    bool stream_visible = false;
    bool frameproducer_visible = false;
    bool progressed_after_churn = false;
    RestartChurnCutPoint last_stage = RestartChurnCutPoint::ProviderVisible;
  };

  auto settle_stage_name = [](RestartChurnCutPoint stage) {
    if (stage == RestartChurnCutPoint::Complete) {
      return "frameproducer_visible";
    }
    return restart_churn_cut_point_name(stage);
  };

  auto classify_final_status = [&](const FinalSettleState& state, uint64_t latest_timestamp_ns) {
    if (state.frameproducer_visible) {
      return "COMPLETE";
    }
    const uint64_t publish_delta = state.observed_publishes_since_provider;
    const uint64_t time_delta = latest_timestamp_ns - state.provider_timestamp_ns;
    if (publish_delta > kProfilerPublishGap || time_delta > kProfilerTimeNs) {
      return "INCOMPLETE";
    }
    return "PARTIAL";
  };

  RealizationProfiler profiler(profiler_options);
  VerifyCaseHarness h(provider_kind);
  std::string error;
  if (profiler.enabled()) {
    h.set_realization_profiler(&profiler);
  }

  const std::vector<RestartChurnCutPoint> cut_points = {
      RestartChurnCutPoint::ProviderVisible,
      RestartChurnCutPoint::DeviceIdentity,
      RestartChurnCutPoint::DeviceNative,
      RestartChurnCutPoint::StreamVisible,
  };

  auto require_boundary_tick = [&](const char* message) -> bool {
    if (h.tick()) {
      return true;
    }
    cli::error("FAIL: ", message);
    return false;
  };

  auto emit_cycle_report = [&](size_t cycle_index, uint64_t gen, RestartChurnCutPoint planned_cut_point) {
    cli::line("[restart-churn] cycle=", cycle_index + 1,
              " gen=", gen,
              " planned_stop_after=", restart_churn_cut_point_name(planned_cut_point),
              " observed_stage=", restart_churn_cut_point_name(observed_restart_churn_stage(*h.observed().raw, gen)));
    if (profiler.enabled()) {
      profiler.emit_report();
    }
  };

  for (size_t cycle_index = 0; cycle_index < cut_points.size(); ++cycle_index) {
    const uint64_t want_gen = static_cast<uint64_t>(cycle_index);
    const RestartChurnCutPoint planned_cut_point = cut_points[cycle_index];

    cli::line("[restart-churn] cycle=", cycle_index + 1,
              " planned_stop_after=", restart_churn_cut_point_name(planned_cut_point),
              " start_requested");

    if (!h.start_runtime(error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!h.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) { return s.gen == want_gen; }, error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!check_step(static_cast<int>(cycle_index * 10), SnapshotExpectation{}.is_nil(true), h.observed())) {
      return 1;
    }

    uint64_t settled_version = 0;
    if (!observe_provider_only_authoritative_start(h,
                                                   error,
                                                   static_cast<int>(cycle_index * 10 + 1),
                                                   want_gen,
                                                   settled_version,
                                                   "expected provider-visible start during restart churn settle prelude",
                                                   "restart churn prelude generation did not settle to provider-visible authoritative truth")) {
      return 1;
    }

    if (planned_cut_point == RestartChurnCutPoint::ProviderVisible) {
      emit_cycle_report(cycle_index, want_gen, planned_cut_point);
      cli::line("[restart-churn] cycle=", cycle_index + 1, " stop_requested");
      h.stop_runtime();
      continue;
    }

    if (!h.open_device(error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!require_boundary_tick("expected observable publish after open_device during restart churn settle prelude")) {
      return 1;
    }

    if (planned_cut_point == RestartChurnCutPoint::DeviceIdentity) {
      emit_cycle_report(cycle_index, want_gen, planned_cut_point);
      cli::line("[restart-churn] cycle=", cycle_index + 1, " stop_requested");
      h.stop_runtime();
      continue;
    }

    if (observed_restart_churn_stage(*h.observed().raw, want_gen) == RestartChurnCutPoint::DeviceIdentity) {
      if (!h.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
            return static_cast<int>(observed_restart_churn_stage(s, want_gen)) >=
                   static_cast<int>(RestartChurnCutPoint::DeviceNative);
          }, error, 500, 5, "timed out waiting for device native during restart churn settle prelude")) {
        cli::error("FAIL: ", error);
        return 1;
      }
      if (!require_boundary_tick("expected observable publish for device native during restart churn settle prelude")) {
        return 1;
      }
    }

    if (planned_cut_point == RestartChurnCutPoint::DeviceNative) {
      emit_cycle_report(cycle_index, want_gen, planned_cut_point);
      cli::line("[restart-churn] cycle=", cycle_index + 1, " stop_requested");
      h.stop_runtime();
      continue;
    }

    if (!h.create_stream(error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!require_boundary_tick("expected observable publish after create_stream during restart churn settle prelude")) {
      return 1;
    }

    emit_cycle_report(cycle_index, want_gen, planned_cut_point);
    cli::line("[restart-churn] cycle=", cycle_index + 1, " stop_requested");
    h.stop_runtime();
  }

  const uint64_t final_gen = static_cast<uint64_t>(cut_points.size());
  cli::line("[restart-churn] cycle=final planned_stop_after=none start_requested");

  if (!h.start_runtime(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!h.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) { return s.gen == final_gen; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!check_step(static_cast<int>(cut_points.size() * 10), SnapshotExpectation{}.is_nil(true), h.observed())) {
    return 1;
  }

  uint64_t settled_version = 0;
  if (!observe_provider_only_authoritative_start(h,
                                                 error,
                                                 static_cast<int>(cut_points.size() * 10 + 1),
                                                 final_gen,
                                                 settled_version,
                                                 "expected provider-visible start for restart_churn_then_settle final generation",
                                                 "final generation did not settle to provider-visible authoritative truth")) {
    return 1;
  }

  FinalSettleState settle_state{};
  settle_state.provider_visible = true;
  settle_state.provider_timestamp_ns = h.observed().raw->timestamp_ns;
  settle_state.last_stage = RestartChurnCutPoint::ProviderVisible;
  const uint64_t settle_begin_timestamp_ns = h.observed().raw->timestamp_ns;

  auto record_settle_snapshot = [&]() {
    const auto stage = observed_restart_churn_stage(*h.observed().raw, final_gen);
    settle_state.last_stage = stage;
    settle_state.observed_publishes_since_provider += 1;

    if (static_cast<int>(stage) >= static_cast<int>(RestartChurnCutPoint::DeviceIdentity) && !settle_state.device_identity) {
      settle_state.device_identity = true;
      settle_state.progressed_after_churn = true;
      if (settle_state.first_descendant_timestamp_ns == 0) {
        settle_state.first_descendant_timestamp_ns = h.observed().raw->timestamp_ns;
      }
    }
    if (static_cast<int>(stage) >= static_cast<int>(RestartChurnCutPoint::DeviceNative)) {
      settle_state.device_native = true;
    }
    if (static_cast<int>(stage) >= static_cast<int>(RestartChurnCutPoint::StreamVisible)) {
      settle_state.stream_visible = true;
    }
    if (stage == RestartChurnCutPoint::Complete) {
      settle_state.frameproducer_visible = true;
    }
  };

  if (!h.open_device(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!require_boundary_tick("expected observable publish after open_device during final settle generation")) {
    return 1;
  }
  record_settle_snapshot();

  if (!h.create_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!require_boundary_tick("expected observable publish after create_stream during final settle generation")) {
    return 1;
  }
  record_settle_snapshot();

  if (!h.start_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!require_boundary_tick("expected observable publish after start_stream during final settle generation")) {
    return 1;
  }
  record_settle_snapshot();

  while (!settle_state.frameproducer_visible) {
    const uint64_t time_delta_ns = h.observed().raw->timestamp_ns - settle_begin_timestamp_ns;
    if (settle_state.observed_publishes_since_provider >= kSettlePublishWindow || time_delta_ns >= kSettleTimeNs) {
      break;
    }
    if (!h.request_publish_only(error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!require_boundary_tick("expected observable publish during final settle window")) {
      return 1;
    }
    record_settle_snapshot();
  }

  cli::line("[restart-churn] cycle=final gen=", final_gen, " settle_complete");
  if (profiler.enabled()) {
    profiler.emit_report();
  }

  const char* final_status = classify_final_status(settle_state, h.observed().raw->timestamp_ns);
  cli::line("[settle] gen=", final_gen);
  cli::line("  progressed_after_churn = ", settle_state.progressed_after_churn ? "true" : "false");
  cli::line("  final_status = ", final_status);
  cli::line("  last_stage = ", settle_stage_name(settle_state.last_stage));
  cli::line("  device_identity_appeared = ", settle_state.device_identity ? "true" : "false");
  cli::line("  device_native_appeared = ", settle_state.device_native ? "true" : "false");
  cli::line("  stream_visible_appeared = ", settle_state.stream_visible ? "true" : "false");
  cli::line("  frameproducer_visible_appeared = ", settle_state.frameproducer_visible ? "true" : "false");
  if (settle_state.first_descendant_timestamp_ns != 0) {
    cli::line("  first_descendant_delta_ns = ", settle_state.first_descendant_timestamp_ns - settle_state.provider_timestamp_ns);
  } else {
    cli::line("  first_descendant_delta_ns = n/a");
  }

  cli::line("Verification case PASSED");
  return 0;
}

int restart_churn_then_settle_variant(const char* verify_case_label,
                                      VerifyCaseProviderKind provider_kind,
                                      const RealizationProfilerOptions& profiler_options,
                                      const std::vector<RestartChurnCutPoint>& cut_points,
                                      uint64_t settle_publish_window,
                                      uint64_t settle_time_ns) {
  constexpr uint64_t kProfilerPublishGap = 8;
  constexpr uint64_t kProfilerTimeNs = 50'000'000ull;

  struct FinalSettleState {
    uint64_t provider_timestamp_ns = 0;
    uint64_t first_descendant_timestamp_ns = 0;
    uint64_t publish_count_during_settle = 0;
    bool provider_visible = false;
    bool device_identity = false;
    bool device_native = false;
    bool stream_visible = false;
    bool frameproducer_visible = false;
    bool progressed_after_churn = false;
    RestartChurnCutPoint last_stage = RestartChurnCutPoint::ProviderVisible;
  };

  auto settle_stage_name = [](RestartChurnCutPoint stage) {
    if (stage == RestartChurnCutPoint::Complete) {
      return "frameproducer_visible";
    }
    return restart_churn_cut_point_name(stage);
  };

  auto classify_final_status = [&](const FinalSettleState& state, uint64_t latest_timestamp_ns) {
    if (state.frameproducer_visible) {
      return "COMPLETE";
    }
    const uint64_t publish_delta = state.publish_count_during_settle;
    const uint64_t time_delta = latest_timestamp_ns - state.provider_timestamp_ns;
    if (publish_delta > kProfilerPublishGap || time_delta > kProfilerTimeNs) {
      return "INCOMPLETE";
    }
    return "PARTIAL";
  };

  auto classify_health = [](const char* final_status, bool progressed_after_churn) {
    if (std::string_view(final_status) == "COMPLETE") {
      return "HEALTHY";
    }
    if (progressed_after_churn) {
      return "DELAYED";
    }
    return "STRANDED_CANDIDATE";
  };

  RealizationProfiler profiler(profiler_options);
  VerifyCaseHarness h(provider_kind);
  std::string error;
  if (profiler.enabled()) {
    h.set_realization_profiler(&profiler);
  }

  auto require_boundary_tick = [&](const char* message) -> bool {
    if (h.tick()) {
      return true;
    }
    cli::error("FAIL: ", message);
    return false;
  };

  auto emit_cycle_report = [&](size_t cycle_index, uint64_t gen, RestartChurnCutPoint planned_cut_point) {
    cli::line("[restart-churn] verification_case=", verify_case_label,
              " cycle=", cycle_index + 1,
              " gen=", gen,
              " planned_stop_after=", restart_churn_cut_point_name(planned_cut_point),
              " observed_stage=", restart_churn_cut_point_name(observed_restart_churn_stage(*h.observed().raw, gen)));
    if (profiler.enabled()) {
      profiler.emit_report();
    }
  };

  for (size_t cycle_index = 0; cycle_index < cut_points.size(); ++cycle_index) {
    const uint64_t want_gen = static_cast<uint64_t>(cycle_index);
    const RestartChurnCutPoint planned_cut_point = cut_points[cycle_index];

    cli::line("[restart-churn] verification_case=", verify_case_label,
              " cycle=", cycle_index + 1,
              " planned_stop_after=", restart_churn_cut_point_name(planned_cut_point),
              " start_requested");

    if (!h.start_runtime(error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!h.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) { return s.gen == want_gen; }, error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!check_step(static_cast<int>(cycle_index * 10), SnapshotExpectation{}.is_nil(true), h.observed())) {
      return 1;
    }

    uint64_t settled_version = 0;
    if (!observe_provider_only_authoritative_start(h,
                                                   error,
                                                   static_cast<int>(cycle_index * 10 + 1),
                                                   want_gen,
                                                   settled_version,
                                                   "expected provider-visible start during failure-seeking restart churn",
                                                   "failure-seeking restart churn generation did not settle to provider-visible authoritative truth")) {
      return 1;
    }

    if (planned_cut_point == RestartChurnCutPoint::ProviderVisible) {
      emit_cycle_report(cycle_index, want_gen, planned_cut_point);
      cli::line("[restart-churn] verification_case=", verify_case_label, " cycle=", cycle_index + 1, " stop_requested");
      h.stop_runtime();
      continue;
    }

    if (!h.open_device(error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!require_boundary_tick("expected observable publish after open_device during failure-seeking restart churn")) {
      return 1;
    }

    if (planned_cut_point == RestartChurnCutPoint::DeviceIdentity) {
      emit_cycle_report(cycle_index, want_gen, planned_cut_point);
      cli::line("[restart-churn] verification_case=", verify_case_label, " cycle=", cycle_index + 1, " stop_requested");
      h.stop_runtime();
      continue;
    }

    if (observed_restart_churn_stage(*h.observed().raw, want_gen) == RestartChurnCutPoint::DeviceIdentity) {
      if (!h.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
            return static_cast<int>(observed_restart_churn_stage(s, want_gen)) >=
                   static_cast<int>(RestartChurnCutPoint::DeviceNative);
          }, error, 500, 5, "timed out waiting for device native during failure-seeking restart churn")) {
        cli::error("FAIL: ", error);
        return 1;
      }
      if (!require_boundary_tick("expected observable publish for device native during failure-seeking restart churn")) {
        return 1;
      }
    }

    if (planned_cut_point == RestartChurnCutPoint::DeviceNative) {
      emit_cycle_report(cycle_index, want_gen, planned_cut_point);
      cli::line("[restart-churn] verification_case=", verify_case_label, " cycle=", cycle_index + 1, " stop_requested");
      h.stop_runtime();
      continue;
    }

    if (!h.create_stream(error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!require_boundary_tick("expected observable publish after create_stream during failure-seeking restart churn")) {
      return 1;
    }

    emit_cycle_report(cycle_index, want_gen, planned_cut_point);
    cli::line("[restart-churn] verification_case=", verify_case_label, " cycle=", cycle_index + 1, " stop_requested");
    h.stop_runtime();
  }

  const uint64_t final_gen = static_cast<uint64_t>(cut_points.size());
  cli::line("[restart-churn] verification_case=", verify_case_label, " cycle=final planned_stop_after=none start_requested");

  if (!h.start_runtime(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!h.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) { return s.gen == final_gen; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!check_step(static_cast<int>(cut_points.size() * 10), SnapshotExpectation{}.is_nil(true), h.observed())) {
    return 1;
  }

  uint64_t settled_version = 0;
  if (!observe_provider_only_authoritative_start(h,
                                                 error,
                                                 static_cast<int>(cut_points.size() * 10 + 1),
                                                 final_gen,
                                                 settled_version,
                                                 "expected provider-visible start for failure-seeking settle final generation",
                                                 "final failure-seeking generation did not settle to provider-visible authoritative truth")) {
    return 1;
  }

  FinalSettleState settle_state{};
  settle_state.provider_visible = true;
  settle_state.provider_timestamp_ns = h.observed().raw->timestamp_ns;
  settle_state.last_stage = RestartChurnCutPoint::ProviderVisible;
  const uint64_t settle_begin_timestamp_ns = h.observed().raw->timestamp_ns;

  auto record_settle_snapshot = [&]() {
    const auto stage = observed_restart_churn_stage(*h.observed().raw, final_gen);
    settle_state.last_stage = stage;
    settle_state.publish_count_during_settle += 1;

    if (static_cast<int>(stage) >= static_cast<int>(RestartChurnCutPoint::DeviceIdentity) && !settle_state.device_identity) {
      settle_state.device_identity = true;
      settle_state.progressed_after_churn = true;
      if (settle_state.first_descendant_timestamp_ns == 0) {
        settle_state.first_descendant_timestamp_ns = h.observed().raw->timestamp_ns;
      }
    }
    if (static_cast<int>(stage) >= static_cast<int>(RestartChurnCutPoint::DeviceNative)) {
      settle_state.device_native = true;
    }
    if (static_cast<int>(stage) >= static_cast<int>(RestartChurnCutPoint::StreamVisible)) {
      settle_state.stream_visible = true;
    }
    if (stage == RestartChurnCutPoint::Complete) {
      settle_state.frameproducer_visible = true;
    }
  };

  if (!h.open_device(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!require_boundary_tick("expected observable publish after open_device during failure-seeking settle")) {
    return 1;
  }
  record_settle_snapshot();

  if (!h.create_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!require_boundary_tick("expected observable publish after create_stream during failure-seeking settle")) {
    return 1;
  }
  record_settle_snapshot();

  if (!h.start_stream(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!require_boundary_tick("expected observable publish after start_stream during failure-seeking settle")) {
    return 1;
  }
  record_settle_snapshot();

  while (!settle_state.frameproducer_visible) {
    const uint64_t time_delta_ns = h.observed().raw->timestamp_ns - settle_begin_timestamp_ns;
    if (settle_state.publish_count_during_settle >= settle_publish_window || time_delta_ns >= settle_time_ns) {
      break;
    }
    if (!h.request_publish_only(error)) {
      cli::error("FAIL: ", error);
      return 1;
    }
    if (!require_boundary_tick("expected observable publish during failure-seeking settle window")) {
      return 1;
    }
    record_settle_snapshot();
  }

  cli::line("[restart-churn] verification_case=", verify_case_label, " cycle=final gen=", final_gen, " settle_complete");
  if (profiler.enabled()) {
    profiler.emit_report();
  }

  const char* final_status = classify_final_status(settle_state, h.observed().raw->timestamp_ns);
  const char* classification = classify_health(final_status, settle_state.progressed_after_churn);
  cli::line("[settle] gen=", final_gen);
  cli::line("  progressed_after_churn = ", settle_state.progressed_after_churn ? "true" : "false");
  cli::line("  final_status = ", final_status);
  cli::line("  last_stage = ", settle_stage_name(settle_state.last_stage));
  cli::line("  device_identity_appeared = ", settle_state.device_identity ? "true" : "false");
  cli::line("  device_native_appeared = ", settle_state.device_native ? "true" : "false");
  cli::line("  stream_visible_appeared = ", settle_state.stream_visible ? "true" : "false");
  cli::line("  frameproducer_visible_appeared = ", settle_state.frameproducer_visible ? "true" : "false");
  if (settle_state.first_descendant_timestamp_ns != 0) {
    cli::line("  first_descendant_delta_ns = ", settle_state.first_descendant_timestamp_ns - settle_state.provider_timestamp_ns);
  } else {
    cli::line("  first_descendant_delta_ns = none");
  }
  cli::line("  classification = ", classification);
  cli::line("  publish_count_during_settle = ", settle_state.publish_count_during_settle);

  cli::line("Verification case PASSED");
  return 0;
}

int restart_churn_then_settle_deep(VerifyCaseProviderKind provider_kind,
                                   const RealizationProfilerOptions& profiler_options) {
  const std::vector<RestartChurnCutPoint> cut_points = {
      RestartChurnCutPoint::DeviceIdentity,
      RestartChurnCutPoint::DeviceNative,
      RestartChurnCutPoint::StreamVisible,
      RestartChurnCutPoint::DeviceNative,
      RestartChurnCutPoint::StreamVisible,
      RestartChurnCutPoint::DeviceNative,
      RestartChurnCutPoint::StreamVisible,
  };
  return restart_churn_then_settle_variant("restart_churn_then_settle_deep",
                                           provider_kind,
                                           profiler_options,
                                           cut_points,
                                           12,
                                           75'000'000ull);
}

int restart_churn_then_settle_burst(VerifyCaseProviderKind provider_kind,
                                    const RealizationProfilerOptions& profiler_options) {
  const std::vector<RestartChurnCutPoint> cut_points = {
      RestartChurnCutPoint::ProviderVisible,
      RestartChurnCutPoint::DeviceIdentity,
      RestartChurnCutPoint::ProviderVisible,
      RestartChurnCutPoint::DeviceIdentity,
      RestartChurnCutPoint::ProviderVisible,
      RestartChurnCutPoint::DeviceNative,
      RestartChurnCutPoint::ProviderVisible,
      RestartChurnCutPoint::StreamVisible,
  };
  return restart_churn_then_settle_variant("restart_churn_then_settle_burst",
                                           provider_kind,
                                           profiler_options,
                                           cut_points,
                                           12,
                                           75'000'000ull);
}

int restart_churn_then_settle_stream_churn(VerifyCaseProviderKind provider_kind,
                                           const RealizationProfilerOptions& profiler_options) {
  const std::vector<RestartChurnCutPoint> cut_points = {
      RestartChurnCutPoint::DeviceNative,
      RestartChurnCutPoint::StreamVisible,
      RestartChurnCutPoint::StreamVisible,
      RestartChurnCutPoint::StreamVisible,
      RestartChurnCutPoint::DeviceNative,
      RestartChurnCutPoint::StreamVisible,
  };
  return restart_churn_then_settle_variant("restart_churn_then_settle_stream_churn",
                                           provider_kind,
                                           profiler_options,
                                           cut_points,
                                           12,
                                           75'000'000ull);
}

int restart_churn_then_settle_long(VerifyCaseProviderKind provider_kind,
                                   const RealizationProfilerOptions& profiler_options) {
  const std::vector<RestartChurnCutPoint> cut_points = {
      RestartChurnCutPoint::ProviderVisible,
      RestartChurnCutPoint::DeviceIdentity,
      RestartChurnCutPoint::DeviceNative,
      RestartChurnCutPoint::StreamVisible,
  };
  return restart_churn_then_settle_variant("restart_churn_then_settle_long",
                                           provider_kind,
                                           profiler_options,
                                           cut_points,
                                           24,
                                           150'000'000ull);
}

int restart_nil_before_baseline(VerifyCaseProviderKind provider_kind) {
  VerifyCaseHarness h(provider_kind);
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

  cli::line("Verification case PASSED");
  return 0;
}

int stream_lifecycle_versions(VerifyCaseProviderKind provider_kind) {
  VerifyCaseHarness h(provider_kind);
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

  cli::line("Verification case PASSED");
  return 0;
}

int topology_change_versions(VerifyCaseProviderKind provider_kind) {
  VerifyCaseHarness h(provider_kind);
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

  cli::line("Verification case PASSED");
  return 0;
}

int publication_coalescing(VerifyCaseProviderKind provider_kind) {
  VerifyCaseHarness h(provider_kind);
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

  cli::line("Verification case PASSED");
  return 0;
}

int canonical_timeline_realization(VerifyCaseProviderKind provider_kind) {
  if (provider_kind != VerifyCaseProviderKind::Synthetic) {
    cli::line("SKIP: verification case 'canonical_timeline_realization' requires synthetic provider");
    return 0;
  }
  if (!ProviderBroker::check_mode_supported_in_build(RuntimeMode::synthetic).ok()) {
    cli::line("SKIP: verification case 'canonical_timeline_realization' synthetic mode not built");
    return 0;
  }

  CoreRuntime runtime;
  StateSnapshotBuffer snapshot_buffer;
  runtime.set_snapshot_publisher(&snapshot_buffer);

  std::string error;
  if (!runtime.start()) {
    cli::error("FAIL: runtime start failed");
    return 1;
  }
  if (!wait_until_poll([&]() { return runtime.state_copy() == CoreRuntimeState::LIVE; },
                       error,
                       "timed out waiting for runtime LIVE")) {
    runtime.stop();
    cli::error("FAIL: ", error);
    return 1;
  }

  ProviderBroker broker;
  if (!broker.set_runtime_mode_requested(RuntimeMode::synthetic).ok()) {
    runtime.stop();
    cli::error("FAIL: provider broker synthetic mode request failed");
    return 1;
  }
  if (!broker.set_synthetic_role_requested(SyntheticRole::Timeline).ok()) {
    runtime.stop();
    cli::error("FAIL: provider broker synthetic role request failed");
    return 1;
  }
  if (!broker.set_synthetic_timing_driver_requested(TimingDriver::VirtualTime).ok()) {
    runtime.stop();
    cli::error("FAIL: provider broker synthetic timing request failed");
    return 1;
  }

  std::vector<SyntheticScheduledEvent> dispatched;
  const auto core_dispatch = make_synthetic_timeline_request_dispatch_hook(runtime);
  broker.set_synthetic_timeline_request_dispatch_hook(
      [&dispatched, core_dispatch](const SyntheticScheduledEvent& ev) {
        dispatched.push_back(ev);
        core_dispatch(ev);
      });

  runtime.attach_provider(&broker);
  if (!broker.initialize(runtime.provider_callbacks()).ok()) {
    runtime.attach_provider(nullptr);
    runtime.stop();
    cli::error("FAIL: provider broker initialize failed");
    return 1;
  }

  std::vector<CameraEndpoint> eps;
  if (!broker.enumerate_endpoints(eps).ok() || eps.empty()) {
    (void)broker.shutdown();
    runtime.attach_provider(nullptr);
    runtime.stop();
    cli::error("FAIL: enumerate_endpoints failed");
    return 1;
  }

  const StreamTemplate st = broker.stream_template();
  const uint32_t fps_num = st.profile.target_fps_max != 0 ? st.profile.target_fps_max : st.profile.target_fps_min;
  if (fps_num == 0) {
    (void)broker.shutdown();
    runtime.attach_provider(nullptr);
    runtime.stop();
    cli::error("FAIL: synthetic stream template fps invalid");
    return 1;
  }
  const uint64_t period_ns = 1'000'000'000ull / static_cast<uint64_t>(fps_num);

  bool broker_initialized = true;
  bool provider_attached = true;
  auto cleanup = [&]() {
    if (broker_initialized) {
      (void)broker.shutdown();
      broker_initialized = false;
    }
    if (provider_attached) {
      runtime.attach_provider(nullptr);
      provider_attached = false;
    }
    runtime.stop();
  };

  auto snapshot_now = [&]() -> std::shared_ptr<const CamBANGStateSnapshot> {
    runtime.request_publish();
    if (!wait_until_poll([&]() { return snapshot_buffer.snapshot_copy() != nullptr; },
                         error,
                         "timed out waiting for snapshot publish")) {
      return nullptr;
    }
    return snapshot_buffer.snapshot_copy();
  };

  auto advance_and_snapshot = [&](uint64_t dt_ns,
                                  const std::function<bool(const CamBANGStateSnapshot&)>& pred,
                                  const char* timeout_message) -> bool {
    if (!broker.try_tick_virtual_time(dt_ns)) {
      error = "synthetic virtual time tick not consumed";
      return false;
    }
    // IMPORTANT: callers validating progression relative to a captured baseline
    // must provide a delta predicate (e.g., `after > before` checks). Absolute
    // predicates can be satisfied by already-coalesced snapshots before the
    // intended post-event progression is actually observed.
    return wait_until_poll([&]() {
      runtime.request_publish();
      auto snap = snapshot_buffer.snapshot_copy();
      return snap && pred(*snap);
    }, error, timeout_message);
  };

  // ---- Explicit lifecycle canonical timeline realization ----
  dispatched.clear();
  SyntheticCanonicalScenario scenario{};
  SyntheticScenarioDeviceDeclaration d0{};
  d0.key = "cam0";
  d0.endpoint_index = 0;
  scenario.devices.push_back(d0);
  SyntheticScenarioStreamDeclaration s0{};
  s0.key = "preview0";
  s0.device_key = "cam0";
  s0.intent = StreamIntent::PREVIEW;
  s0.baseline_capture_profile = st.profile;
  scenario.streams.push_back(s0);

  PictureConfig updated = st.picture;
  updated.preset = PatternPreset::Solid;
  updated.overlay_frame_index_offsets = false;
  updated.overlay_moving_bar = false;
  updated.solid_r = 25;
  updated.solid_g = 200;
  updated.solid_b = 75;
  updated.solid_a = 255;

  scenario.timeline.push_back({0, SyntheticEventType::OpenDevice, "cam0", "", false, {}});
  scenario.timeline.push_back({0, SyntheticEventType::CreateStream, "", "preview0", false, {}});
  scenario.timeline.push_back({0, SyntheticEventType::StartStream, "", "preview0", false, {}});
  scenario.timeline.push_back({period_ns, SyntheticEventType::EmitFrame, "", "preview0", false, {}});
  scenario.timeline.push_back({period_ns * 2, SyntheticEventType::UpdateStreamPicture, "", "preview0", true, updated});
  scenario.timeline.push_back({period_ns * 4, SyntheticEventType::StopStream, "", "preview0", false, {}});
  scenario.timeline.push_back({period_ns * 4 + 1, SyntheticEventType::DestroyStream, "", "preview0", false, {}});
  scenario.timeline.push_back({period_ns * 4 + 2, SyntheticEventType::CloseDevice, "cam0", "", false, {}});

  if (!broker.set_timeline_canonical_scenario_for_host(scenario).ok()) {
    cleanup();
    cli::error("FAIL: canonical explicit-lifecycle submission rejected");
    return 1;
  }
  if (!broker.start_timeline_scenario_for_host().ok()) {
    cleanup();
    cli::error("FAIL: canonical explicit-lifecycle start rejected");
    return 1;
  }

  auto snap_before = snapshot_now();
  if (!snap_before) {
    cleanup();
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!snap_before->devices.empty() || !snap_before->streams.empty()) {
    cleanup();
    fail_step(0, "snapshot truth violated before timeline execution");
    return 1;
  }

  if (!advance_and_snapshot(0, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, 30001);
        return VerifyCaseHarness::has_device(s, 10001) && stream && stream->mode == CBStreamMode::FLOWING;
      }, "timed out waiting for explicit lifecycle open/create/start")) {
    cleanup();
    cli::error("FAIL: ", error);
    return 1;
  }

  if (dispatched.size() < 3 ||
      dispatched[0].type != SyntheticEventType::OpenDevice ||
      dispatched[1].type != SyntheticEventType::CreateStream ||
      dispatched[2].type != SyntheticEventType::StartStream) {
    cleanup();
    fail_step(1, "explicit lifecycle ordering/path mismatch (expected Open/Create before Start)");
    return 1;
  }

  if (!advance_and_snapshot(period_ns, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, 30001);
        return stream && stream->frames_received >= 1;
      }, "timed out waiting for first frame")) {
    cleanup();
    cli::error("FAIL: ", error);
    return 1;
  }

  auto snap_after_first_frame = snapshot_buffer.snapshot_copy();
  if (!snap_after_first_frame) {
    cleanup();
    fail_step(2, "snapshot missing after first emit");
    return 1;
  }
  const auto* stream_before_update = VerifyCaseHarness::find_stream(*snap_after_first_frame, 30001);
  if (!stream_before_update) {
    cleanup();
    fail_step(2, "stream missing after first emit");
    return 1;
  }
  const uint64_t frames_before_update = stream_before_update->frames_received;
  const uint64_t ts_before_update = stream_before_update->last_frame_ts_ns;

  // Step 3 is a DELTA check relative to the captured baseline above:
  // require both frame-count and timestamp progression after the authored
  // update-phase advance. Absolute thresholds (e.g. frames_received >= 2) are
  // incorrect here because a pre-existing/coalesced snapshot may already satisfy
  // them before post-update progression is observable.
  if (!advance_and_snapshot(period_ns * 2, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, 30001);
        return stream &&
               stream->mode == CBStreamMode::FLOWING &&
               stream->frames_received > frames_before_update &&
               stream->last_frame_ts_ns > ts_before_update;
      }, "timed out waiting for post-update frame")) {
    cleanup();
    cli::error("FAIL: ", error);
    return 1;
  }

  auto snap_after_update = snapshot_buffer.snapshot_copy();
  if (!snap_after_update) {
    cleanup();
    fail_step(3, "snapshot missing after update emit");
    return 1;
  }
  const auto* stream_after_update = VerifyCaseHarness::find_stream(*snap_after_update, 30001);
  if (!stream_after_update) {
    cleanup();
    fail_step(3, "stream missing after update emit");
    return 1;
  }
  if (stream_after_update->frames_received <= frames_before_update ||
      stream_after_update->last_frame_ts_ns <= ts_before_update) {
    cleanup();
    cli::error("step 3 detail: frames_before=", frames_before_update,
               " frames_after=", stream_after_update->frames_received,
               " ts_before=", ts_before_update,
               " ts_after=", stream_after_update->last_frame_ts_ns);
    fail_step(3, "post-update frame progression missing");
    return 1;
  }
  // NOTE: verify-case smoke builds do not have a dedicated frame-copy observer
  // exposed at this layer (CoreRuntime latest_frame_mailbox accessor is dev-node scoped).
  // We therefore assert update request dispatch + post-update frame progression
  // rather than pixel signatures in this runtime/harness verification.

  if (!advance_and_snapshot(period_ns, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, 30001);
        return stream && stream->mode == CBStreamMode::STOPPED;
      }, "timed out waiting for stop")) {
    cleanup();
    cli::error("FAIL: ", error);
    return 1;
  }

  auto advance_until_realized = [&](uint64_t step_ns,
                                    int max_steps,
                                    const std::function<bool(const CamBANGStateSnapshot&)>& pred,
                                    const char* timeout_message) -> bool {
    runtime.request_publish();
    if (auto snap = snapshot_buffer.snapshot_copy(); snap && pred(*snap)) {
      return true;
    }
    for (int i = 0; i < max_steps; ++i) {
      if (!broker.try_tick_virtual_time(step_ns)) {
        error = "synthetic virtual time tick not consumed";
        return false;
      }
      runtime.request_publish();
      if (wait_until_poll([&]() {
            auto snap = snapshot_buffer.snapshot_copy();
            return snap && pred(*snap);
          }, error, timeout_message, /*max_iters=*/20, /*sleep_ms=*/1)) {
        return true;
      }
    }
    error = timeout_message;
    return false;
  };

  if (!advance_until_realized(1,
                              /*max_steps=*/256,
                              [&](const CamBANGStateSnapshot& s) {
                                return !VerifyCaseHarness::has_stream(s, 30001);
                              },
                              "timed out waiting for destroy")) {
    cleanup();
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!advance_until_realized(1,
                              /*max_steps=*/256,
                              [&](const CamBANGStateSnapshot& s) {
                                return !VerifyCaseHarness::has_device(s, 10001);
                              },
                              "timed out waiting for close")) {
    cleanup();
    cli::error("FAIL: ", error);
    return 1;
  }

  size_t emitframe_dispatched = 0;
  for (const auto& ev : dispatched) {
    if (ev.type == SyntheticEventType::EmitFrame) {
      ++emitframe_dispatched;
    }
  }
  if (emitframe_dispatched != 0) {
    cleanup();
    fail_step(4, "EmitFrame crossed Core request dispatch boundary");
    return 1;
  }

  if (!broker.shutdown().ok()) {
    broker_initialized = false;
    runtime.attach_provider(nullptr);
    provider_attached = false;
    runtime.stop();
    cli::error("FAIL: broker shutdown failed");
    return 1;
  }
  broker_initialized = false;
  runtime.attach_provider(nullptr);
  provider_attached = false;
  runtime.stop();

  cli::line("Verification case PASSED");
  return 0;
}

int device_disconnect(VerifyCaseProviderKind provider_kind) {
  if (provider_kind != VerifyCaseProviderKind::Synthetic) {
    cli::line("SKIP: verification case 'device_disconnect' requires SyntheticProvider timeline support");
    return 0;
  }

  VerifyCaseHarness h(provider_kind);
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

  if (!h.inject_provider_stream_stop(VerifyCaseHarness::kStreamId, ProviderError::ERR_PROVIDER_FAILED, error) ||
      !h.inject_provider_stream_destroyed(VerifyCaseHarness::kStreamId, error) ||
      !h.inject_provider_device_closed(VerifyCaseHarness::kDeviceId, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(2, SnapshotExpectation{}.version(2).topology_version(2).device_count(0).stream_count(0), h.observed())) {
    return 1;
  }

  cli::line("Verification case PASSED");
  return 0;
}

int close_while_streaming(VerifyCaseProviderKind provider_kind) {
  VerifyCaseHarness h(provider_kind);
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

  const ProviderResult close_result = h.close_device_result(VerifyCaseHarness::kDeviceId);
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

  cli::line("Verification case PASSED");
  return 0;
}

int frame_starvation(VerifyCaseProviderKind provider_kind) {
  if (provider_kind != VerifyCaseProviderKind::Synthetic) {
    cli::line("SKIP: verification case 'frame_starvation' requires SyntheticProvider timeline support");
    return 0;
  }

  VerifyCaseHarness h(provider_kind);
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

  const auto* flowing = VerifyCaseHarness::find_stream(*h.observed().raw, VerifyCaseHarness::kStreamId);
  if (!flowing || flowing->mode != CBStreamMode::FLOWING) {
    fail_step(2, "stream not flowing before starvation window");
    return 1;
  }
  uint64_t before_frames = 0;
  if (!h.wait_for_stream_quiescence(VerifyCaseHarness::kStreamId, error, &before_frames)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!h.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, VerifyCaseHarness::kStreamId);
        return stream && stream->mode == CBStreamMode::FLOWING;
      }, error, 500, 5, "timed out waiting for flowing stream at starvation boundary")) {
    cli::error("FAIL: ", error);
    return 1;
  }
  auto boundary_snap = h.snapshot_buffer().snapshot_copy();
  const auto* quiesced = boundary_snap ? VerifyCaseHarness::find_stream(*boundary_snap, VerifyCaseHarness::kStreamId) : nullptr;
  if (!quiesced || quiesced->mode != CBStreamMode::FLOWING) {
    fail_step(2, "stream not flowing at starvation boundary");
    return 1;
  }
  before_frames = quiesced->frames_received;

  uint64_t after_frames = 0;
  if (!h.wait_for_stream_quiescence(VerifyCaseHarness::kStreamId, error, &after_frames)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  auto after_snap = h.snapshot_buffer().snapshot_copy();
  const auto* after = after_snap ? VerifyCaseHarness::find_stream(*after_snap, VerifyCaseHarness::kStreamId) : nullptr;
  if (!after) {
    fail_step(4, "stream missing after starvation window");
    return 1;
  }
  if (after_frames != before_frames || after->frames_received != before_frames) {
    fail_step(4, "frames advanced during starvation window");
    return 1;
  }
  if (after->mode != CBStreamMode::FLOWING) {
    fail_step(4, "stream changed mode during starvation window");
    return 1;
  }
  cli::line("step 4 OK");

  cli::line("Verification case PASSED");
  return 0;
}

int provider_error_mid_stream(VerifyCaseProviderKind provider_kind) {
  if (provider_kind != VerifyCaseProviderKind::Synthetic) {
    cli::line("SKIP: verification case 'provider_error_mid_stream' requires SyntheticProvider timeline support");
    return 0;
  }

  VerifyCaseHarness h(provider_kind);
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

  h.inject_provider_stream_error(VerifyCaseHarness::kStreamId, ProviderError::ERR_BAD_STATE);
  if (!h.inject_provider_stream_stop(VerifyCaseHarness::kStreamId, ProviderError::ERR_BAD_STATE, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(2, SnapshotExpectation{}.version(2).topology_version(1).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  const auto* stream = h.runtime().stream_record(VerifyCaseHarness::kStreamId);
  if (!stream || stream->started || stream->last_error_code == 0) {
    fail_step(3, "provider error did not propagate to stream record");
    return 1;
  }
  cli::line("step 3 OK");

  cli::line("Verification case PASSED");
  return 0;
}

int redundant_stop(VerifyCaseProviderKind provider_kind) {
  VerifyCaseHarness h(provider_kind);
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

  const auto* stream = VerifyCaseHarness::find_stream(*h.observed().raw, VerifyCaseHarness::kStreamId);
  if (!stream || stream->mode != CBStreamMode::STOPPED) {
    fail_step(4, "stream not stable after redundant stop");
    return 1;
  }
  cli::line("step 4 OK");

  cli::line("Verification case PASSED");
  return 0;
}

int multi_device_topology_change(VerifyCaseProviderKind provider_kind) {
  if (provider_kind != VerifyCaseProviderKind::Synthetic) {
    cli::line("SKIP: verification case 'multi_device_topology_change' requires SyntheticProvider timeline support");
    return 0;
  }

  VerifyCaseHarness h(provider_kind);
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

  if (!h.open_device_id(VerifyCaseHarness::kDeviceId, 0, VerifyCaseHarness::kRootId, error) ||
      !h.open_device_id(VerifyCaseHarness::kDeviceBId, 1, VerifyCaseHarness::kRootBId, error) ||
      !h.create_stream_id(VerifyCaseHarness::kStreamId, VerifyCaseHarness::kDeviceId, 1, error) ||
      !h.create_stream_id(VerifyCaseHarness::kStreamBId, VerifyCaseHarness::kDeviceBId, 2, error) ||
      !h.start_stream_id(VerifyCaseHarness::kStreamId, error) ||
      !h.start_stream_id(VerifyCaseHarness::kStreamBId, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(1, SnapshotExpectation{}.version(1).topology_version(1).device_count(2).stream_count(2), h.observed())) {
    return 1;
  }

  if (!h.stop_stream_id(VerifyCaseHarness::kStreamId, error) ||
      !h.destroy_stream_id(VerifyCaseHarness::kStreamId, error) ||
      !h.close_device_id(VerifyCaseHarness::kDeviceId, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check_step(2, SnapshotExpectation{}.version(2).topology_version(2).device_count(1).stream_count(1), h.observed())) {
    return 1;
  }

  const auto* remaining_stream = VerifyCaseHarness::find_stream(*h.observed().raw, VerifyCaseHarness::kStreamBId);
  if (!remaining_stream || remaining_stream->device_instance_id != VerifyCaseHarness::kDeviceBId ||
      remaining_stream->mode != CBStreamMode::FLOWING) {
    fail_step(3, "remaining device/stream topology corrupted");
    return 1;
  }
  cli::line("step 3 OK");

  cli::line("Verification case PASSED");
  return 0;
}

} // namespace

std::vector<VerifyCaseDefinition> verify_case_catalog(VerifyCaseProviderKind provider_kind,
                                               const RealizationProfilerOptions& profiler_options) {
  return {
      {"baseline_start", [provider_kind]() { return baseline_start(provider_kind); }},
      {"provider_only_authoritative_baseline", [provider_kind]() { return provider_only_authoritative_baseline(provider_kind); }},
      {"provider_only_to_realized", [provider_kind, profiler_options]() { return provider_only_to_realized(provider_kind, profiler_options); }},
      {"provider_only_then_stop", [provider_kind]() { return provider_only_then_stop(provider_kind); }},
      {"repeated_provider_only_across_generations", [provider_kind]() { return repeated_provider_only_across_generations(provider_kind); }},
      {"restart_churn_realization", [provider_kind, profiler_options]() { return restart_churn_realization(provider_kind, profiler_options); }},
      {"restart_churn_then_settle", [provider_kind, profiler_options]() { return restart_churn_then_settle(provider_kind, profiler_options); }},
      {"restart_churn_then_settle_deep", [provider_kind, profiler_options]() { return restart_churn_then_settle_deep(provider_kind, profiler_options); }},
      {"restart_churn_then_settle_burst", [provider_kind, profiler_options]() { return restart_churn_then_settle_burst(provider_kind, profiler_options); }},
      {"restart_churn_then_settle_stream_churn", [provider_kind, profiler_options]() { return restart_churn_then_settle_stream_churn(provider_kind, profiler_options); }},
      {"restart_churn_then_settle_long", [provider_kind, profiler_options]() { return restart_churn_then_settle_long(provider_kind, profiler_options); }},
      {"restart_nil_before_baseline", [provider_kind]() { return restart_nil_before_baseline(provider_kind); }},
      {"stream_lifecycle_versions", [provider_kind]() { return stream_lifecycle_versions(provider_kind); }},
      {"topology_change_versions", [provider_kind]() { return topology_change_versions(provider_kind); }},
      {"publication_coalescing", [provider_kind]() { return publication_coalescing(provider_kind); }},
      {"canonical_timeline_realization", [provider_kind]() { return canonical_timeline_realization(provider_kind); }},
      {"device_disconnect", [provider_kind]() { return device_disconnect(provider_kind); }},
      {"close_while_streaming", [provider_kind]() { return close_while_streaming(provider_kind); }},
      {"frame_starvation", [provider_kind]() { return frame_starvation(provider_kind); }},
      {"provider_error_mid_stream", [provider_kind]() { return provider_error_mid_stream(provider_kind); }},
      {"redundant_stop", [provider_kind]() { return redundant_stop(provider_kind); }},
      {"multi_device_topology_change", [provider_kind]() { return multi_device_topology_change(provider_kind); }},
  };
}

} // namespace cambang

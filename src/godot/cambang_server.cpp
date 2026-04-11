#include "godot/cambang_server.h"
#include "godot/cambang_capture_result.h"
#include "godot/cambang_capture_result_set.h"
#include "godot/cambang_stream_result.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>

#include "core/synthetic_timeline_request_binding.h"
#include "imaging/api/timeline_teardown_trace.h"
#include "imaging/broker/provider_broker.h"
#include "imaging/broker/banner_info.h"

#include <cstdlib>

namespace cambang {

static bool banners_enabled() noexcept {
  const char* v = std::getenv("CAMBANG_BANNERS");
  return !(v && v[0] == '0' && v[1] == '\0');
}

CamBANGServer* CamBANGServer::singleton_ = nullptr;


namespace {

static const char* mode_to_cstr(RuntimeMode m) noexcept {
  switch (m) {
    case RuntimeMode::platform_backed: return "platform_backed";
    case RuntimeMode::synthetic: return "synthetic";
    default: return "unknown";
  }
}

static bool parse_synthetic_role_int(int value, SyntheticRole& out_role) noexcept {
  switch (value) {
    case static_cast<int>(SyntheticRole::Nominal):
      out_role = SyntheticRole::Nominal;
      return true;
    case static_cast<int>(SyntheticRole::Timeline):
      out_role = SyntheticRole::Timeline;
      return true;
    default:
      return false;
  }
}

static bool parse_timing_driver_int(int value, TimingDriver& out_timing_driver) noexcept {
  switch (value) {
    case static_cast<int>(TimingDriver::RealTime):
      out_timing_driver = TimingDriver::RealTime;
      return true;
    case static_cast<int>(TimingDriver::VirtualTime):
      out_timing_driver = TimingDriver::VirtualTime;
      return true;
    default:
      return false;
  }
}

static bool parse_timeline_reconciliation_int(int value, TimelineReconciliation& out_reconciliation) noexcept {
  switch (value) {
    case CamBANGServer::TIMELINE_RECONCILIATION_COMPLETION_GATED:
      out_reconciliation = TimelineReconciliation::CompletionGated;
      return true;
    case CamBANGServer::TIMELINE_RECONCILIATION_STRICT:
      out_reconciliation = TimelineReconciliation::Strict;
      return true;
    default:
      return false;
  }
}

static godot::Error map_provider_result_to_godot_error(ProviderResult pr) noexcept {
  switch (pr.code) {
    case ProviderError::OK: return godot::OK;
    case ProviderError::ERR_BUSY: return godot::ERR_BUSY;
    case ProviderError::ERR_INVALID_ARGUMENT: return godot::ERR_INVALID_PARAMETER;
    case ProviderError::ERR_BAD_STATE: return godot::ERR_INVALID_PARAMETER;
    case ProviderError::ERR_NOT_SUPPORTED: return godot::ERR_UNAVAILABLE;
    default: return godot::FAILED;
  }
}

static bool line_contains_token(const std::string& line, const char* token) {
  return token && line.find(token) != std::string::npos;
}

static int runtime_mode_to_provider_kind_int(RuntimeMode mode) noexcept {
  switch (mode) {
    case RuntimeMode::platform_backed: return CamBANGServer::PROVIDER_KIND_PLATFORM_BACKED;
    case RuntimeMode::synthetic: return CamBANGServer::PROVIDER_KIND_SYNTHETIC;
    default: return CamBANGServer::PROVIDER_KIND_PLATFORM_BACKED;
  }
}

} // namespace

CamBANGServer::CamBANGServer() {
  if (singleton_ && singleton_ != this) {
    // This should never happen: the engine singleton is registered once.
    ERR_PRINT("CamBANGServer: multiple instances detected; only one is supported.");
    return;
  }
  singleton_ = this;
  runtime_.set_snapshot_publisher(&snapshot_buffer_);
  timeline_trace_echo_enabled_ = timeline_teardown_trace_enabled();
  _refresh_timeline_teardown_trace_mode();
}

CamBANGServer::~CamBANGServer() {
  // Ensure graceful stop if the extension is torn down.
  runtime_.stop();
  if (singleton_ == this) {
    singleton_ = nullptr;
  }
}

godot::Error CamBANGServer::start(const godot::Variant& provider_kind_arg,
                                  const godot::Variant& role_arg,
                                  const godot::Variant& timing_driver_arg,
                                  const godot::Variant& timeline_reconciliation_arg) {
  const bool has_provider_kind = provider_kind_arg.get_type() != godot::Variant::NIL;
  const bool has_role = role_arg.get_type() != godot::Variant::NIL;
  const bool has_timing_driver = timing_driver_arg.get_type() != godot::Variant::NIL;
  const bool has_timeline_reconciliation = timeline_reconciliation_arg.get_type() != godot::Variant::NIL;

  int provider_kind = PROVIDER_KIND_PLATFORM_BACKED;
  if (has_provider_kind) {
    if (provider_kind_arg.get_type() != godot::Variant::INT) {
      ERR_PRINT("CamBANGServer: start rejected; provider_kind must be an integer when supplied.");
      return godot::ERR_INVALID_PARAMETER;
    }
    provider_kind = static_cast<int>(int64_t(provider_kind_arg));
  }

  if (provider_kind == PROVIDER_KIND_PLATFORM_BACKED) {
    if (has_role || has_timing_driver || has_timeline_reconciliation) {
      ERR_PRINT("CamBANGServer: start rejected; platform-backed start does not accept synthetic role/timing/reconciliation arguments.");
      return godot::ERR_INVALID_PARAMETER;
    }
    return _start_with_provider_config(
        RuntimeMode::platform_backed,
        SyntheticRole::Nominal,
        TimingDriver::VirtualTime,
        true);
  }
  if (provider_kind != PROVIDER_KIND_SYNTHETIC) {
    ERR_PRINT(godot::vformat(
        "CamBANGServer: start rejected; unknown provider_kind value '%d'.",
        provider_kind));
    return godot::ERR_INVALID_PARAMETER;
  }

  int role = SYNTHETIC_ROLE_NOMINAL;
  if (has_role) {
    if (role_arg.get_type() != godot::Variant::INT) {
      ERR_PRINT("CamBANGServer: start rejected; synthetic role must be an integer when supplied.");
      return godot::ERR_INVALID_PARAMETER;
    }
    role = static_cast<int>(int64_t(role_arg));
  }

  SyntheticRole parsed_role{};
  if (!parse_synthetic_role_int(role, parsed_role)) {
    ERR_PRINT(godot::vformat(
        "CamBANGServer: start rejected; unknown synthetic role value '%d'.",
        role));
    return godot::ERR_INVALID_PARAMETER;
  }

  int timing_driver = TIMING_DRIVER_VIRTUAL_TIME;
  if (has_timing_driver) {
    if (timing_driver_arg.get_type() != godot::Variant::INT) {
      ERR_PRINT("CamBANGServer: start rejected; timing_driver must be an integer when supplied.");
      return godot::ERR_INVALID_PARAMETER;
    }
    timing_driver = static_cast<int>(int64_t(timing_driver_arg));
  }

  TimingDriver parsed_timing_driver{};
  if (!parse_timing_driver_int(timing_driver, parsed_timing_driver)) {
    ERR_PRINT(godot::vformat(
        "CamBANGServer: start rejected; unknown timing driver value '%d'.",
        timing_driver));
    return godot::ERR_INVALID_PARAMETER;
  }

  const bool reconciliation_applicable =
      parsed_role == SyntheticRole::Timeline && parsed_timing_driver == TimingDriver::VirtualTime;
  bool completion_gated_destructive_sequencing_enabled = true;
  TimelineReconciliation requested_timeline_reconciliation = TimelineReconciliation::CompletionGated;
  if (has_timeline_reconciliation) {
    if (!reconciliation_applicable) {
      ERR_PRINT("CamBANGServer: start rejected; timeline_reconciliation applies only to synthetic timeline virtual_time mode.");
      return godot::ERR_INVALID_PARAMETER;
    }
    if (timeline_reconciliation_arg.get_type() != godot::Variant::INT) {
      ERR_PRINT("CamBANGServer: start rejected; timeline_reconciliation must be an integer when supplied.");
      return godot::ERR_INVALID_PARAMETER;
    }
    const int timeline_reconciliation = static_cast<int>(int64_t(timeline_reconciliation_arg));
    if (!parse_timeline_reconciliation_int(timeline_reconciliation, requested_timeline_reconciliation)) {
      ERR_PRINT(godot::vformat(
          "CamBANGServer: start rejected; unknown timeline_reconciliation value '%d'.",
          timeline_reconciliation));
      return godot::ERR_INVALID_PARAMETER;
    }
  }

  completion_gated_destructive_sequencing_enabled =
      (requested_timeline_reconciliation == TimelineReconciliation::CompletionGated);

  return _start_with_provider_config(
      RuntimeMode::synthetic,
      parsed_role,
      parsed_timing_driver,
      completion_gated_destructive_sequencing_enabled);
}

godot::Error CamBANGServer::_start_with_provider_config(
    RuntimeMode mode,
    SyntheticRole synthetic_role,
    TimingDriver timing_driver,
    bool completion_gated_destructive_sequencing_enabled) {
  const CoreRuntimeState state = runtime_.state_copy();
  if (state == CoreRuntimeState::STARTING || state == CoreRuntimeState::LIVE) {
    return godot::ERR_ALREADY_IN_USE;
  }

  // New session starts with no Godot-latched snapshot until first publish.
  latest_.reset();
  latest_export_.clear();
  has_latest_export_ = false;
  has_godot_counters_ = false;

  // Begin a new boundary session and reject any prior-generation late publishes
  // until the first snapshot from the next expected generation is observed.
  active_session_id_ = ++session_counter_;
  if (has_last_completed_gen_) {
    accepted_min_gen_ = last_completed_gen_ + 1;
    enforce_min_gen_gate_ = true;
  } else {
    accepted_min_gen_ = 0;
    enforce_min_gen_gate_ = false;
  }
  last_seen_published_seq_ = runtime_.published_seq();

  active_runtime_mode_ = mode;
  active_synthetic_role_ = synthetic_role;
  completion_gated_destructive_sequencing_enabled_ = completion_gated_destructive_sequencing_enabled;
  strict_scenario_unmet_logged_ = false;
  _refresh_timeline_teardown_trace_mode();

  // Defensive re-check: requested mode must be supported in this build.
  {
    ProviderResult cap = ProviderBroker::check_mode_supported_in_build(mode);
    if (!cap.ok()) {
      ERR_PRINT(godot::vformat(
          "CamBANGServer: cannot start; requested provider_mode='%s' is not supported in this build.",
          mode_to_cstr(mode)));
      return map_provider_result_to_godot_error(cap);
    }
  }

  // Ensure the SceneTree tick hook exists so snapshots can be drained + signals emitted.
  _ensure_tick_connected();

  // Explicit user action: do not auto-start on launch.
  runtime_.start();
  _refresh_timeline_teardown_trace_mode();

  // Ensure a provider is attached + initialized (latched selection).
  // This is the canonical linkage point between Godot and the core runtime.
  if (!_ensure_provider_attached_and_initialized(mode, synthetic_role, timing_driver)) {
    return godot::FAILED;
  }
  return godot::OK;
}

void CamBANGServer::stop() {
  // Deterministic provider shutdown before stopping the core runtime.
  if (provider_) {
    (void)provider_->shutdown();
    runtime_.attach_provider(nullptr);
    provider_.reset();
  }
  runtime_.stop();

  strict_scenario_unmet_logged_ = false;
  _refresh_timeline_teardown_trace_mode();

  // Stop is a boundary operation; core may have published final teardown/retirement
  // truth during deterministic shutdown. Drain one final boundary observation before
  // returning to NIL so the prior generation can be observed once more if changed.
  if (has_godot_counters_) {
    (void)_consume_latest_core_snapshot(/*emit_signal=*/true);
  }

  if (has_godot_counters_) {
    has_last_completed_gen_ = true;
    last_completed_gen_ = godot_gen_;
  }

  // Enforce documented NIL pre-baseline behaviour across restart boundaries.
  latest_.reset();
  latest_export_.clear();
  has_latest_export_ = false;
  has_godot_counters_ = false;
  snapshot_buffer_.clear();
  last_seen_published_seq_ = runtime_.published_seq();
  active_session_id_ = 0;
  enforce_min_gen_gate_ = false;
}

bool CamBANGServer::is_running() const {
  const CoreRuntimeState state = runtime_.state_copy();
  return state == CoreRuntimeState::STARTING || state == CoreRuntimeState::LIVE;
}

CamBANGStreamResult* CamBANGServer::get_latest_stream_result(uint64_t stream_id) const {
  SharedStreamResultData data = runtime_.get_latest_stream_result(stream_id);
  if (!data) {
    return nullptr;
  }
  CamBANGStreamResult* out = memnew(CamBANGStreamResult);
  out->set_data(std::move(data));
  return out;
}

CamBANGCaptureResult* CamBANGServer::get_capture_result(uint64_t capture_id, uint64_t device_instance_id) const {
  SharedCaptureResultData data = runtime_.get_capture_result(capture_id, device_instance_id);
  if (!data) {
    return nullptr;
  }
  CamBANGCaptureResult* out = memnew(CamBANGCaptureResult);
  out->set_data(std::move(data));
  return out;
}

CamBANGCaptureResultSet* CamBANGServer::get_capture_result_set(uint64_t capture_id) const {
  std::vector<SharedCaptureResultData> results = runtime_.get_capture_result_set(capture_id);
  CamBANGCaptureResultSet* out = memnew(CamBANGCaptureResultSet);
  out->set_capture_id(capture_id);
  out->set_results(std::move(results));
  return out;
}

#if defined(CAMBANG_ENABLE_DEV_NODES)
ProviderBroker* CamBANGServer::provider_broker_for_dev() const noexcept {
  return dynamic_cast<ProviderBroker*>(provider_.get());
}
#endif

void CamBANGServer::_ensure_tick_connected() {
  if (tick_connected_) {
    return;
  }

  // Connect to SceneTree's per-frame tick. This is the cleanest way for an
  // Engine singleton (not in the scene tree) to receive a main-thread tick.
  godot::MainLoop* ml = godot::Engine::get_singleton()->get_main_loop();
  godot::SceneTree* tree = godot::Object::cast_to<godot::SceneTree>(ml);
  if (!tree) {
    return;
  }

  // process_frame is emitted once per rendered frame (Godot main thread).
  // It has no args; we compute delta locally.
  godot::Callable cb(this, "_on_godot_process_frame");
  tree->connect("process_frame", cb);

  tick_connected_ = true;
  last_tick_time_ns_ = 0;
}

void CamBANGServer::_on_godot_process_frame() {
  using clock = std::chrono::steady_clock;
  const uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count());

  double delta_s = 0.0;
  if (last_tick_time_ns_ != 0 && now_ns >= last_tick_time_ns_) {
    delta_s = static_cast<double>(now_ns - last_tick_time_ns_) / 1'000'000'000.0;
  }
  last_tick_time_ns_ = now_ns;

  _on_godot_tick(delta_s);
}

bool CamBANGServer::_consume_latest_core_snapshot(bool should_emit_signal) {
  const uint64_t published_seq = runtime_.published_seq();
  if (published_seq == last_seen_published_seq_) {
    return false;
  }

  // Something changed since the last boundary observation. Latch the latest core snapshot.
  auto snap = snapshot_buffer_.snapshot_copy();
  if (!snap) {
    // Do not consume published_seq yet; retry on a later observation so we do not
    // lose this publish if the marker becomes observable before buffer visibility.
    return false;
  }
  last_seen_published_seq_ = published_seq;

  if (active_session_id_ == 0) {
    return false;
  }

  if (enforce_min_gen_gate_) {
    if (snap->gen < accepted_min_gen_) {
      // Late prior-generation publication after stop/start boundary; ignore.
      return false;
    }
    enforce_min_gen_gate_ = false;
  }

  // Establish / reset tick-bounded counters on new gen.
  // gen is defined from the Godot-facing perspective but still aligns with
  // core generations: it advances on each successful start() STOPPED->LIVE.
  if (!has_godot_counters_ || snap->gen != godot_gen_) {
    has_godot_counters_ = true;
    godot_gen_ = snap->gen;
    godot_version_ = 0;
    godot_topology_version_ = 0;
    last_emitted_topology_sig_ = runtime_.published_topology_sig();
  } else {
    // Tick-bounded version increments on every emission within a gen.
    ++godot_version_;

    // topology_version increments only when the observed topology differs
    // from the topology at the previous emission.
    const uint64_t topo_sig = runtime_.published_topology_sig();
    if (topo_sig != last_emitted_topology_sig_) {
      last_emitted_topology_sig_ = topo_sig;
      ++godot_topology_version_;
    }
  }

  latest_ = snap;

  // Export as a struct-like Variant graph for Godot inspection.
  latest_export_ = export_snapshot_to_godot(*snap, godot_gen_, godot_version_, godot_topology_version_);
  has_latest_export_ = true;

  if (should_emit_signal) {
    emit_signal("state_published",
                static_cast<uint64_t>(godot_gen_),
                static_cast<uint64_t>(godot_version_),
                static_cast<uint64_t>(godot_topology_version_));
  }

  return true;
}

void CamBANGServer::_on_godot_tick(double delta) {
  // If a virtual_time provider is active (stub heartbeat, synthetic virtual_time),
  // advance it using the Godot frame delta.
  if (runtime_.is_running()) {
    if (ICameraProvider* prov = runtime_.attached_provider()) {
      if (auto* broker = dynamic_cast<ProviderBroker*>(prov)) {
        const uint64_t dt_ns = static_cast<uint64_t>(delta * 1'000'000'000.0);
        (void)broker->try_tick_virtual_time(dt_ns);
      }
    }

    // Echo any core-thread banner line through Godot logging so it's visible in
    // the editor output panel (stdout isn't reliably surfaced on Windows).
    if (banners_enabled()) {
      char line[192];
      if (runtime_.take_core_banner_line(line, sizeof(line))) {
        godot::UtilityFunctions::print(line);
      }
    }
  }

  std::string timeline_line;
  while (timeline_teardown_trace_try_pop(timeline_line)) {
    _handle_timeline_teardown_trace_line(timeline_line);
    if (timeline_trace_echo_enabled_) {
      godot::UtilityFunctions::print(timeline_line.c_str());
    }
  }

  // Godot-facing snapshot truth is tick-bounded.
  // Core may publish multiple intermediate snapshots between Godot ticks.
  // We emit at most once per tick, and only if *anything* has changed since
  // the previous tick.
  (void)_consume_latest_core_snapshot(/*emit_signal=*/true);
}

godot::Variant CamBANGServer::get_state_snapshot() const {
  if (!has_latest_export_) {
    return godot::Variant();
  }
  return latest_export_;
}

godot::Variant CamBANGServer::get_active_provider_config() const {
  if (!runtime_.is_running()) {
    return godot::Variant();
  }
  const ProviderBroker* broker = dynamic_cast<const ProviderBroker*>(provider_.get());
  if (!broker) {
    return godot::Variant();
  }
  godot::Dictionary d;
  const RuntimeMode mode = broker->runtime_mode_latched();
  d["provider_kind"] = runtime_mode_to_provider_kind_int(mode);
  d["timeline_reconciliation"] = godot::Variant();
  if (mode == RuntimeMode::synthetic) {
    const SyntheticRole role = broker->synthetic_role_latched();
    const TimingDriver timing_driver = broker->synthetic_timing_driver_latched();
    d["synthetic_role"] = static_cast<int>(role);
    d["timing_driver"] = static_cast<int>(timing_driver);
    if (role == SyntheticRole::Timeline && timing_driver == TimingDriver::VirtualTime) {
      const TimelineReconciliation reconciliation = broker->synthetic_timeline_reconciliation_latched();
      d["timeline_reconciliation"] =
          (reconciliation == TimelineReconciliation::CompletionGated)
              ? godot::String("completion_gated")
              : godot::String("strict");
    }
  } else {
    d["synthetic_role"] = godot::Variant();
    d["timing_driver"] = godot::Variant();
  }
  return d;
}

godot::Error CamBANGServer::select_builtin_scenario(const godot::String& scenario_name) {
  ProviderBroker* broker = dynamic_cast<ProviderBroker*>(provider_.get());
  if (!broker) {
    return map_provider_result_to_godot_error(
        ProviderResult::failure(ProviderError::ERR_BAD_STATE));
  }
  const std::string scenario_utf8 = scenario_name.utf8().get_data();
  const ProviderResult pr = broker->select_timeline_builtin_scenario_for_host(scenario_utf8);
  if (pr.ok()) {
    strict_scenario_unmet_logged_ = false;
  }
  return map_provider_result_to_godot_error(pr);
}

godot::Error CamBANGServer::load_external_scenario(const godot::String& json_text) {
  ProviderBroker* broker = dynamic_cast<ProviderBroker*>(provider_.get());
  if (!broker) {
    return map_provider_result_to_godot_error(
        ProviderResult::failure(ProviderError::ERR_BAD_STATE));
  }
  const std::string text_utf8 = json_text.utf8().get_data();
  const ProviderResult pr = broker->load_timeline_canonical_scenario_from_json_text_for_host(text_utf8);
  if (pr.ok()) {
    strict_scenario_unmet_logged_ = false;
  }
  return map_provider_result_to_godot_error(pr);
}

godot::Error CamBANGServer::start_scenario() {
  ProviderBroker* broker = dynamic_cast<ProviderBroker*>(provider_.get());
  if (!broker) {
    return map_provider_result_to_godot_error(
        ProviderResult::failure(ProviderError::ERR_BAD_STATE));
  }
  const ProviderResult pr = broker->start_timeline_scenario_for_host();
  if (pr.ok()) {
    strict_scenario_unmet_logged_ = false;
  }
  return map_provider_result_to_godot_error(pr);
}

godot::Error CamBANGServer::stop_scenario() {
  ProviderBroker* broker = dynamic_cast<ProviderBroker*>(provider_.get());
  if (!broker) {
    return map_provider_result_to_godot_error(
        ProviderResult::failure(ProviderError::ERR_BAD_STATE));
  }
  return map_provider_result_to_godot_error(broker->stop_timeline_scenario_for_host());
}

godot::Error CamBANGServer::set_timeline_paused(bool paused) {
  ProviderBroker* broker = dynamic_cast<ProviderBroker*>(provider_.get());
  if (!broker) {
    return map_provider_result_to_godot_error(
        ProviderResult::failure(ProviderError::ERR_BAD_STATE));
  }
  return map_provider_result_to_godot_error(broker->set_timeline_scenario_paused_for_host(paused));
}

godot::Error CamBANGServer::advance_timeline(uint64_t dt_ns) {
  ProviderBroker* broker = dynamic_cast<ProviderBroker*>(provider_.get());
  if (!broker) {
    return map_provider_result_to_godot_error(
        ProviderResult::failure(ProviderError::ERR_BAD_STATE));
  }
  return map_provider_result_to_godot_error(broker->advance_timeline_for_host(dt_ns));
}


void CamBANGServer::_refresh_timeline_teardown_trace_mode() {
  const bool strict_timeline_monitor =
      runtime_.is_running() && active_runtime_mode_ == RuntimeMode::synthetic &&
      active_synthetic_role_ == SyntheticRole::Timeline &&
      !completion_gated_destructive_sequencing_enabled_;
  timeline_teardown_trace_set_enabled(timeline_trace_echo_enabled_ || strict_timeline_monitor);
}

void CamBANGServer::_handle_timeline_teardown_trace_line(const std::string& line) {
  const bool strict_timeline_monitor =
      runtime_.is_running() && active_runtime_mode_ == RuntimeMode::synthetic &&
      active_synthetic_role_ == SyntheticRole::Timeline &&
      !completion_gated_destructive_sequencing_enabled_;
  if (!strict_timeline_monitor || strict_scenario_unmet_logged_) {
    return;
  }

  if (!(line_contains_token(line, "[timeline_teardown][FAIL] DestroyStream") ||
        line_contains_token(line, "[timeline_teardown][FAIL] CloseDevice"))) {
    return;
  }

  godot::UtilityFunctions::push_warning(
      "[CamBANG][Synthetic][Strict] Scenario conditions unmet. Strict destructive sequencing could not be satisfied in-band for the active timeline scenario.");
  strict_scenario_unmet_logged_ = true;
}

bool CamBANGServer::_ensure_provider_attached_and_initialized(
    RuntimeMode mode,
    SyntheticRole synthetic_role,
    TimingDriver timing_driver) {
  if (!runtime_.is_running()) {
    return false;
  }

  // If we already have an attached provider, nothing to do.
  if (provider_ && runtime_.attached_provider() == provider_.get()) {
    return true;
  }

  // Fresh broker per start cycle (latched provider configuration).
  {
    auto broker = std::make_unique<ProviderBroker>();
    broker->set_synthetic_timeline_request_dispatch_hook(
        make_synthetic_timeline_request_dispatch_hook(runtime_));
    ProviderResult sr = broker->set_runtime_mode_requested(mode);
    if (!sr.ok()) {
      ERR_PRINT(godot::vformat(
          "CamBANGServer: provider_mode='%s' is not supported in this build.",
          mode_to_cstr(mode)));
      return false;
    }
    ProviderResult role_req = broker->set_synthetic_role_requested(synthetic_role);
    if (!role_req.ok()) {
      ERR_PRINT("CamBANGServer: requested synthetic role configuration rejected by provider broker.");
      return false;
    }
    ProviderResult timing_req = broker->set_synthetic_timing_driver_requested(timing_driver);
    if (!timing_req.ok()) {
      ERR_PRINT("CamBANGServer: requested synthetic timing_driver configuration rejected by provider broker.");
      return false;
    }
    const bool reconciliation_applicable =
        mode == RuntimeMode::synthetic &&
        synthetic_role == SyntheticRole::Timeline &&
        timing_driver == TimingDriver::VirtualTime;
    if (reconciliation_applicable) {
      ProviderResult recon_req = broker->set_synthetic_timeline_reconciliation_requested(
          completion_gated_destructive_sequencing_enabled_
              ? TimelineReconciliation::CompletionGated
              : TimelineReconciliation::Strict);
      if (!recon_req.ok()) {
        ERR_PRINT("CamBANGServer: requested synthetic timeline_reconciliation configuration rejected by provider broker.");
        return false;
      }
    }
    provider_ = std::move(broker);
  }
  runtime_.attach_provider(provider_.get());

  ProviderResult pr = provider_->initialize(runtime_.provider_callbacks());
  if (!pr.ok()) {
    runtime_.attach_provider(nullptr);
    provider_.reset();
    ERR_PRINT("CamBANGServer: provider initialize failed.");
    return false;
  }

  // Banner 1: Godot-facing provider selection (effective runtime attachment).
  if (banners_enabled()) {
    const ProviderBannerInfo bi = describe_provider_for_banner(provider_.get());
    godot::UtilityFunctions::print("[CamBANG] provider selected: ", bi.provider_mode, " / ", bi.provider_name);
  }

  return true;
}
void CamBANGServer::_bind_methods() {
  godot::ClassDB::bind_method(
      godot::D_METHOD("start", "provider_kind", "role", "timing_driver", "timeline_reconciliation"),
      &CamBANGServer::start,
      DEFVAL(godot::Variant()),
      DEFVAL(godot::Variant()),
      DEFVAL(godot::Variant()),
      DEFVAL(godot::Variant()));
  godot::ClassDB::bind_method(godot::D_METHOD("stop"), &CamBANGServer::stop);
  godot::ClassDB::bind_method(godot::D_METHOD("is_running"), &CamBANGServer::is_running);
  godot::ClassDB::bind_method(godot::D_METHOD("get_active_provider_config"), &CamBANGServer::get_active_provider_config);
  godot::ClassDB::bind_method(godot::D_METHOD("select_builtin_scenario", "scenario_name"), &CamBANGServer::select_builtin_scenario);
  godot::ClassDB::bind_method(godot::D_METHOD("load_external_scenario", "json_text"), &CamBANGServer::load_external_scenario);
  godot::ClassDB::bind_method(godot::D_METHOD("start_scenario"), &CamBANGServer::start_scenario);
  godot::ClassDB::bind_method(godot::D_METHOD("stop_scenario"), &CamBANGServer::stop_scenario);
  godot::ClassDB::bind_method(godot::D_METHOD("set_timeline_paused", "paused"), &CamBANGServer::set_timeline_paused);
  godot::ClassDB::bind_method(godot::D_METHOD("advance_timeline", "dt_ns"), &CamBANGServer::advance_timeline);
  godot::ClassDB::bind_method(godot::D_METHOD("get_state_snapshot"), &CamBANGServer::get_state_snapshot);
  godot::ClassDB::bind_method(godot::D_METHOD("get_latest_stream_result", "stream_id"), &CamBANGServer::get_latest_stream_result);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_result", "capture_id", "device_instance_id"), &CamBANGServer::get_capture_result);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_result_set", "capture_id"), &CamBANGServer::get_capture_result_set);

  // Internal tick hook (connected to SceneTree.process_frame).
  godot::ClassDB::bind_method(godot::D_METHOD("_on_godot_process_frame"), &CamBANGServer::_on_godot_process_frame);

  BIND_CONSTANT(PROVIDER_KIND_PLATFORM_BACKED);
  BIND_CONSTANT(PROVIDER_KIND_SYNTHETIC);
  BIND_CONSTANT(SYNTHETIC_ROLE_NOMINAL);
  BIND_CONSTANT(SYNTHETIC_ROLE_TIMELINE);
  BIND_CONSTANT(TIMING_DRIVER_REAL_TIME);
  BIND_CONSTANT(TIMING_DRIVER_VIRTUAL_TIME);
  BIND_CONSTANT(TIMELINE_RECONCILIATION_COMPLETION_GATED);
  BIND_CONSTANT(TIMELINE_RECONCILIATION_STRICT);

  ADD_SIGNAL(godot::MethodInfo(
      "state_published",
      godot::PropertyInfo(godot::Variant::INT, "gen"),
      godot::PropertyInfo(godot::Variant::INT, "version"),
      godot::PropertyInfo(godot::Variant::INT, "topology_version")));
}

} // namespace cambang

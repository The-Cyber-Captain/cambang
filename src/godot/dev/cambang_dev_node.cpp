#include "godot/dev/cambang_dev_node.h"

#include "godot/cambang_server.h"
#include "core/core_runtime.h"
#include "core/latest_frame_mailbox.h"

#include "imaging/broker/provider_broker.h"
#include "imaging/api/provider_contract_datatypes.h"

#include "pixels/pattern/pattern_registry.h"

#include <vector>
#include <string>
#include <cctype>
#include <cstdlib>

#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/engine.hpp>

using godot::UtilityFunctions;

namespace cambang {

std::atomic<bool> CamBANGDevNode::s_live{false};

CamBANGDevNode::CamBANGDevNode() = default;

CamBANGDevNode::~CamBANGDevNode() {
    stop_runtime_();
}

void CamBANGDevNode::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("start_scenario", "name"), &CamBANGDevNode::start_scenario);
    godot::ClassDB::bind_method(godot::D_METHOD("get_exit_reason"), &CamBANGDevNode::get_exit_reason);
    ADD_SIGNAL(godot::MethodInfo("scenario_completed", godot::PropertyInfo(godot::Variant::STRING, "name")));
}

bool CamBANGDevNode::start_scenario(const godot::String& name) {
    if (!runtime_ || !runtime_->is_running()) {
        UtilityFunctions::printerr("[CamBANGDevNode] start_scenario requires a running runtime.");
        return false;
    }
    const godot::String normalized = name.strip_edges().to_lower();
    ActiveScenario requested = ActiveScenario::None;

    if (normalized == "stream_lifecycle_versions") {
        requested = ActiveScenario::StreamLifecycleVersions;
    } else if (normalized == "topology_change_versions") {
        requested = ActiveScenario::TopologyChangeVersions;
    } else if (normalized == "publication_coalescing") {
        requested = ActiveScenario::PublicationCoalescing;
    } else {
        UtilityFunctions::printerr("[CamBANGDevNode] start_scenario resolution failed: unknown scenario name '", name, "'.");
        return false;
    }

    if (bringup_state_ != BringUpState::Running) {
        pending_scenario_ = requested;
        UtilityFunctions::print("[CamBANGDevNode] scenario queued: ", normalized, " (waiting for provider stream Running)");
        return true;
    }

    if (!dispatch_scenario_now_(requested)) {
        UtilityFunctions::printerr("[CamBANGDevNode] start_scenario ", normalized, " dispatch failed: provider stream is not running.");
        return false;
    }

    UtilityFunctions::print("[CamBANGDevNode] scenario started: ", normalized);
    return true;
}

godot::String CamBANGDevNode::get_exit_reason() const {
    return exit_reason_;
}

void CamBANGDevNode::_enter_tree() {
    if (godot::Engine::get_singleton()->is_editor_hint()) {
        // Don’t start runtime in the editor; avoids crashes when the editor auto-opens scenes.
        set_process(false);
        return;
    }
    if (s_live.exchange(true)) {
        mark_exit_reason_("duplicate_instance_guard");
        UtilityFunctions::printerr("[CamBANGDevNode] duplicate instance; queue_free.");
        queue_free();
        return;
    }

    start_runtime_();
    set_process(true);
}

void CamBANGDevNode::_exit_tree() {
    if (exit_reason_ == "none") {
        mark_exit_reason_("external_tree_teardown");
    }
    if (godot::Engine::get_singleton()->is_editor_hint()) {
        set_process(false);
        return;
    }
    stop_runtime_();
    set_process(false);
    s_live.store(false);
}

void CamBANGDevNode::_process(double delta) {
    if (!started_ || !runtime_) {
        return;
    }

    // Detect external runtime start/stop cycles (e.g. stress scripts calling CamBANGServer).
    const bool running = runtime_->is_running();
    if (running != last_running_) {
        if (!running) {
            stop_provider_();
        } else {
            if (!start_provider_()) {
                UtilityFunctions::printerr("[CamBANGDevNode] Provider bring-up failed after restart.");
            }
        }
        last_running_ = running;
    }

    // Core commands are explicitly non-blocking; provider bring-up progresses
    // asynchronously across ticks.
    if (bringup_state_ != BringUpState::Idle && bringup_state_ != BringUpState::Running) {
        tick_bringup_();
    }

    if (bringup_state_ == BringUpState::Running && pending_scenario_ != ActiveScenario::None) {
        if (dispatch_scenario_now_(pending_scenario_)) {
            pending_scenario_ = ActiveScenario::None;
        }
    }

    if (bringup_state_ == BringUpState::Running) {
        if (active_scenario_ != ActiveScenario::None) {
            complete_active_scenario_();
        }
    }


    // Dev-only manual pattern cycling helper.
    // This is intentionally non-canonical and not part of scenario semantics.
    // Canonical scenario-visible appearance changes should be authored via
    // provider-owned timeline events (e.g. UpdateStreamPicture).
    if (pattern_cycle_enabled_ && runtime_ && bringup_state_ == BringUpState::Running) {
        pattern_cycle_accum_s_ += delta;
        if (pattern_cycle_accum_s_ >= pattern_cycle_period_s_) {
            pattern_cycle_accum_s_ = 0.0;

            PictureConfig cfg{};
            cfg.preset = preset_from_index_or_default(pattern_cycle_index_, PatternPreset::XyXor);
            cfg.seed = 0;
            //cfg.overlay_frame_index_offsets = true;
            //cfg.overlay_moving_bar = true;
            cfg.overlay_frame_index_offsets = false;
            cfg.overlay_moving_bar = true;

            const auto st = runtime_->try_set_stream_picture_config(stream_id_, cfg);
            if (st == TrySetStreamPictureStatus::NotSupported && !pattern_cycle_logged_unsupported_) {
                UtilityFunctions::printerr("[CamBANGDevNode] Pattern cycling enabled but provider does not support stream picture updates.");
                pattern_cycle_logged_unsupported_ = true;
            }

            const uint32_t n = static_cast<uint32_t>(pattern_preset_count());
            pattern_cycle_index_ = (n == 0) ? 0u : ((pattern_cycle_index_ + 1u) % n);
        }
    }

    // Provider virtual_time ticking is now owned by CamBANGServer::_on_godot_tick().
    (void)delta;
}

const LatestFrameMailbox* CamBANGDevNode::get_latest_frame_mailbox() const {
    if (!runtime_) {
        return nullptr;
    }
    return &runtime_->latest_frame_mailbox();
}

void CamBANGDevNode::start_runtime_() {
    if (started_) return;

    auto* server = CamBANGServer::get_singleton();
    if (!server) {
        mark_exit_reason_("startup_abort_no_singleton");
        UtilityFunctions::printerr("[CamBANGDevNode] startup aborted: no CamBANGServer singleton.");
        s_live.store(false);
        queue_free();
        return;
    }

    runtime_ = server->runtime_for_dev();
    if (!runtime_) {
        mark_exit_reason_("startup_abort_runtime_unavailable");
        UtilityFunctions::printerr("[CamBANGDevNode] startup aborted: runtime unavailable.");
        s_live.store(false);
        queue_free();
        return;
    }

    // Dev convenience: if the server/runtime isn't running yet, start it.
    // This preserves the release contract (explicit start/stop) while keeping
    // the dev visibility scenes self-contained.
    if (!runtime_->is_running()) {
        server->start();
        started_server_ = true;
    }

    if (!runtime_->is_running()) {
        mark_exit_reason_("startup_abort_runtime_start_failed");
        UtilityFunctions::printerr("[CamBANGDevNode] startup aborted: runtime start failed.");
        s_live.store(false);
        queue_free();
        return;
    }

    started_ = true;
    last_running_ = runtime_->is_running();

    // Bring up provider for the initial running state.
    if (last_running_) {
        if (!start_provider_()) {
            mark_exit_reason_("startup_abort_provider_bringup_failed");
            UtilityFunctions::printerr("[CamBANGDevNode] startup aborted: provider bring-up failed.");
            stop_runtime_();
            s_live.store(false);
            queue_free();
            return;
        }
    }
}

bool CamBANGDevNode::start_provider_() {
    if (!runtime_ || !runtime_->is_running()) {
        return false;
    }

    // Dev-only manual pattern cycling configuration (visual verification aid).
    // Not part of canonical scenario semantics.
    pattern_cycle_enabled_ = false;
    pattern_cycle_logged_unsupported_ = false;
    pattern_cycle_accum_s_ = 0.0;
    pattern_cycle_index_ = 0;

    if (const char* v = std::getenv("CAMBANG_DEV_PATTERN_CYCLE")) {
        if (v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T') {
            pattern_cycle_enabled_ = true;
        }
    }
    if (pattern_cycle_enabled_) {
        if (const char* p = std::getenv("CAMBANG_DEV_PATTERN_CYCLE_PERIOD")) {
            char* end = nullptr;
            const double s = std::strtod(p, &end);
            if (end && end != p && s > 0.05) {
                pattern_cycle_period_s_ = s;
            }
        }
        if (const char* si = std::getenv("CAMBANG_DEV_PATTERN_CYCLE_START_INDEX")) {
            char* end = nullptr;
            const long i = std::strtol(si, &end, 10);
            if (end && end != si && i >= 0) {
                pattern_cycle_index_ = static_cast<uint32_t>(i);
            }
        }
        const uint32_t n = static_cast<uint32_t>(pattern_preset_count());
        if (n != 0) {
            pattern_cycle_index_ %= n;
        } else {
            pattern_cycle_index_ = 0;
        }
        UtilityFunctions::print("[CamBANGDevNode] Pattern cycling enabled. period_s=", pattern_cycle_period_s_, " start_index=", (int)pattern_cycle_index_);
    }

    // Provider lifecycle (create/attach/initialize + Banner 1) is now owned by CamBANGServer.
    auto* server = CamBANGServer::get_singleton();
    if (!server) {
        UtilityFunctions::printerr("[CamBANGDevNode] No CamBANGServer singleton.");
        return false;
    }
    provider_ = server->provider_broker_for_dev();
    if (!provider_) {
        UtilityFunctions::printerr("[CamBANGDevNode] No provider broker attached. Did CamBANGServer.start() succeed?");
        return false;
    }

    std::vector<CameraEndpoint> eps;
    ProviderResult pr = provider_->enumerate_endpoints(eps);
    if (!pr.ok() || eps.empty()) {
        UtilityFunctions::printerr("[CamBANGDevNode] Provider enumerate_endpoints failed.");
        stop_provider_();
        return false;
    }

    // Dev visibility: list endpoints once at provider bring-up.
    for (size_t i = 0; i < eps.size(); ++i) {
        UtilityFunctions::print("[CamBANGDevNode] Endpoint ", (int)i, ": ", eps[i].name.c_str());
    }

    // Dev-only selection policy:
    // Prefer a device whose name contains "emeet" (case-insensitive).
    // Fallback to index 0 if not found.
    size_t selected = 0;
    for (size_t i = 0; i < eps.size(); ++i) {
        std::string name = eps[i].name;
        for (char& c : name) c = (char)std::tolower((unsigned char)c);
        if (name.find("emeet") != std::string::npos) {
            selected = i;
            break;
        }
    }

    UtilityFunctions::print("[CamBANGDevNode] Opening endpoint index ", (int)selected, ": ", eps[selected].name.c_str());

    runtime_->retain_device_identity(device_instance_id_, eps[selected].hardware_id);

    pr = provider_->open_device(eps[selected].hardware_id, device_instance_id_, root_id_);
    if (!pr.ok()) {
        UtilityFunctions::printerr("[CamBANGDevNode] Provider open_device failed.");
        stop_provider_();
        return false;
    }

    bringup_state_ = BringUpState::DeviceOpened;
    bringup_ticks_ = 0;

    StreamTemplate tmpl = provider_->stream_template();

    // Cache the effective config we intend to run (dev scaffolding).
    // Dev node must not override provider StreamTemplate fields.
    effective_profile_ = tmpl.profile;
    effective_picture_ = tmpl.picture;
    effective_profile_version_ = 1;

    // Kick off create; start will be attempted on subsequent ticks after
    // the core thread has declared the stream record.
    bringup_state_ = BringUpState::CreatePending;
    tick_bringup_();
    return (bringup_state_ != BringUpState::Idle);
}

void CamBANGDevNode::stop_provider_() {
    if (!runtime_) {
        provider_ = nullptr;
        return;
    }

    auto* server = CamBANGServer::get_singleton();
    const bool provider_still_owned_by_server =
        (server != nullptr) &&
        (provider_ != nullptr) &&
        (server->provider_broker_for_dev() == provider_);

    // Best-effort dev teardown; core shutdown remains deterministic.
    // Streams are core-owned; tear down via CoreRuntime to avoid split-brain state.
    (void)runtime_->try_stop_stream(stream_id_);
    (void)runtime_->try_destroy_stream(stream_id_);

    // Device open/close is still provider-direct in this dev node (for now),
    // but only while the broker is still owned by CamBANGServer. External
    // CamBANGServer.stop() destroys the broker before this dev node observes the
    // runtime transition, so skip provider-direct teardown once ownership has moved.
    if (provider_still_owned_by_server) {
        (void)provider_->dev_stop_timeline_scenario();
        (void)provider_->close_device(device_instance_id_);
    }

    // Provider lifetime is owned by CamBANGServer; do not detach/shutdown here.
    provider_ = nullptr;
    emit_accum_ = 0.0;
    pattern_cycle_accum_s_ = 0.0;

    bringup_state_ = BringUpState::Idle;
    bringup_ticks_ = 0;
    active_scenario_ = ActiveScenario::None;
    pending_scenario_ = ActiveScenario::None;
}

void CamBANGDevNode::tick_bringup_() {
    if (!runtime_ || !runtime_->is_running() || !provider_) {
        bringup_state_ = BringUpState::Idle;
        return;
    }

    // Prevent infinite spam if something is fundamentally wrong.
    // (This is dev-only; keep it deterministic and non-blocking.)
    ++bringup_ticks_;
    if (bringup_ticks_ > 600) { // ~10s at 60fps
        UtilityFunctions::printerr("[CamBANGDevNode] Provider bring-up timed out.");
        stop_provider_();
        return;
    }

    if (bringup_state_ == BringUpState::CreatePending) {
        // Picture omitted: core defaults from StreamTemplate (canonical behavior).
        const auto cs = runtime_->try_create_stream(
            stream_id_, device_instance_id_, StreamIntent::PREVIEW,
            &effective_profile_,
            nullptr,
            effective_profile_version_);
        if (cs == TryCreateStreamStatus::OK) {
            bringup_state_ = BringUpState::StartPending;
        }
        return; // Busy => retry next tick
    }

    if (bringup_state_ == BringUpState::StartPending) {
        const auto ss = runtime_->try_start_stream(stream_id_);
        if (ss == TryStartStreamStatus::OK) {
            bringup_state_ = BringUpState::Running;
            if (pending_scenario_ != ActiveScenario::None && dispatch_scenario_now_(pending_scenario_)) {
                pending_scenario_ = ActiveScenario::None;
            }
            return;
        }
        // Busy => retry next tick.
        // InvalidArgument is expected transiently if create hasn't executed
        // on the core thread yet; retry.
        return;
    }
}

bool CamBANGDevNode::dispatch_scenario_now_(ActiveScenario scenario) {
    if (bringup_state_ != BringUpState::Running) {
        return false;
    }
    if (scenario == ActiveScenario::None) {
        return true;
    }
    if (!provider_) {
        return false;
    }
    auto scenario_def = build_provider_scenario_(scenario);
    if (!scenario_def.has_value()) {
        return false;
    }
    ProviderResult sr = provider_->dev_set_timeline_scenario(*scenario_def);
    if (!sr.ok()) {
        return false;
    }
    ProviderResult st = provider_->dev_start_timeline_scenario();
    if (!st.ok()) {
        return false;
    }
    active_scenario_ = scenario;
    return true;
}

godot::String CamBANGDevNode::scenario_name_(ActiveScenario scenario) {
    switch (scenario) {
        case ActiveScenario::StreamLifecycleVersions:
            return "stream_lifecycle_versions";
        case ActiveScenario::TopologyChangeVersions:
            return "topology_change_versions";
        case ActiveScenario::PublicationCoalescing:
            return "publication_coalescing";
        case ActiveScenario::None:
        default:
            return "none";
    }
}

std::optional<SyntheticTimelineScenario> CamBANGDevNode::build_provider_scenario_(ActiveScenario scenario) const {
    SyntheticTimelineScenario out{};
    SyntheticScheduledEvent ev{};

    const uint64_t alt_device_id = device_instance_id_ + 100;
    const uint64_t alt_root_id = root_id_ + 100;
    const uint64_t alt_stream_id = stream_id_ + 100;

    if (scenario == ActiveScenario::StreamLifecycleVersions) {
        ev.at_ns = 0;
        ev.type = SyntheticEventType::OpenDevice;
        ev.endpoint_index = 0;
        ev.device_instance_id = alt_device_id;
        ev.root_id = alt_root_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 0;
        ev.type = SyntheticEventType::CreateStream;
        ev.device_instance_id = alt_device_id;
        ev.stream_id = alt_stream_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 0;
        ev.type = SyntheticEventType::StartStream;
        ev.stream_id = alt_stream_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 15'000'000;
        ev.type = SyntheticEventType::UpdateStreamPicture;
        ev.stream_id = alt_stream_id;
        ev.has_picture = true;
        ev.picture.preset = PatternPreset::Checker;
        ev.picture.seed = 3;
        ev.picture.overlay_frame_index_offsets = false;
        ev.picture.overlay_moving_bar = true;
        ev.picture.checker_size_px = 12;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 60'000'000;
        ev.type = SyntheticEventType::StopStream;
        ev.stream_id = alt_stream_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 60'000'001;
        ev.type = SyntheticEventType::DestroyStream;
        ev.stream_id = alt_stream_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 60'000'002;
        ev.type = SyntheticEventType::CloseDevice;
        ev.device_instance_id = alt_device_id;
        out.events.push_back(ev);
        return out;
    }

    if (scenario == ActiveScenario::PublicationCoalescing) {
        ev.at_ns = 0;
        ev.type = SyntheticEventType::OpenDevice;
        ev.endpoint_index = 0;
        ev.device_instance_id = alt_device_id;
        ev.root_id = alt_root_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 0;
        ev.type = SyntheticEventType::CreateStream;
        ev.device_instance_id = alt_device_id;
        ev.stream_id = alt_stream_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 0;
        ev.type = SyntheticEventType::StartStream;
        ev.stream_id = alt_stream_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 10'000'000;
        ev.type = SyntheticEventType::UpdateStreamPicture;
        ev.stream_id = alt_stream_id;
        ev.has_picture = true;
        ev.picture.preset = PatternPreset::Solid;
        ev.picture.overlay_frame_index_offsets = false;
        ev.picture.overlay_moving_bar = false;
        ev.picture.solid_r = 220;
        ev.picture.solid_g = 40;
        ev.picture.solid_b = 40;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 20'000'000;
        ev.type = SyntheticEventType::UpdateStreamPicture;
        ev.stream_id = alt_stream_id;
        ev.has_picture = true;
        ev.picture.preset = PatternPreset::Solid;
        ev.picture.overlay_frame_index_offsets = false;
        ev.picture.overlay_moving_bar = false;
        ev.picture.solid_r = 40;
        ev.picture.solid_g = 210;
        ev.picture.solid_b = 60;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 30'000'000;
        ev.type = SyntheticEventType::UpdateStreamPicture;
        ev.stream_id = alt_stream_id;
        ev.has_picture = true;
        ev.picture.preset = PatternPreset::Solid;
        ev.picture.overlay_frame_index_offsets = false;
        ev.picture.overlay_moving_bar = false;
        ev.picture.solid_r = 60;
        ev.picture.solid_g = 80;
        ev.picture.solid_b = 220;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 200'000'000;
        ev.type = SyntheticEventType::StopStream;
        ev.stream_id = alt_stream_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 200'000'001;
        ev.type = SyntheticEventType::DestroyStream;
        ev.stream_id = alt_stream_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 200'000'002;
        ev.type = SyntheticEventType::CloseDevice;
        ev.device_instance_id = alt_device_id;
        out.events.push_back(ev);
        return out;
    }

    if (scenario == ActiveScenario::TopologyChangeVersions) {
        ev.at_ns = 0;
        ev.type = SyntheticEventType::OpenDevice;
        ev.endpoint_index = 0;
        ev.device_instance_id = alt_device_id;
        ev.root_id = alt_root_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 0;
        ev.type = SyntheticEventType::CreateStream;
        ev.device_instance_id = alt_device_id;
        ev.stream_id = alt_stream_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 0;
        ev.type = SyntheticEventType::StartStream;
        ev.stream_id = alt_stream_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 15'000'000;
        ev.type = SyntheticEventType::UpdateStreamPicture;
        ev.stream_id = alt_stream_id;
        ev.has_picture = true;
        ev.picture.preset = PatternPreset::NoiseAnimated;
        ev.picture.seed = 99;
        ev.picture.overlay_frame_index_offsets = true;
        ev.picture.overlay_moving_bar = true;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 50'000'000;
        ev.type = SyntheticEventType::CreateStream;
        ev.device_instance_id = alt_device_id;
        ev.stream_id = alt_stream_id + 1;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 50'000'001;
        ev.type = SyntheticEventType::DestroyStream;
        ev.stream_id = alt_stream_id + 1;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 100'000'000;
        ev.type = SyntheticEventType::StopStream;
        ev.stream_id = alt_stream_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 100'000'001;
        ev.type = SyntheticEventType::DestroyStream;
        ev.stream_id = alt_stream_id;
        out.events.push_back(ev);

        ev = {};
        ev.at_ns = 100'000'002;
        ev.type = SyntheticEventType::CloseDevice;
        ev.device_instance_id = alt_device_id;
        out.events.push_back(ev);
        return out;
    }

    return std::nullopt;
}

void CamBANGDevNode::mark_exit_reason_(const godot::String& reason) {
    exit_reason_ = reason;
}

void CamBANGDevNode::complete_active_scenario_() {
    const ActiveScenario completed = active_scenario_;
    if (completed == ActiveScenario::None) {
        return;
    }
    active_scenario_ = ActiveScenario::None;
    emit_signal("scenario_completed", scenario_name_(completed));
}

void CamBANGDevNode::stop_runtime_() {
    if (!runtime_) {
        provider_ = nullptr;
        started_ = false;
        last_running_ = false;
        active_scenario_ = ActiveScenario::None;
        pending_scenario_ = ActiveScenario::None;
        return;
    }

    stop_provider_();

    // If this dev node started the server, stop it on teardown.
    if (started_server_) {
        if (auto* server = CamBANGServer::get_singleton()) {
            server->stop();
        }
        started_server_ = false;
    }

    // Do not stop the runtime; it is owned by CamBANGServer.
    runtime_ = nullptr;
    started_ = false;
    last_running_ = false;
}

} // namespace cambang

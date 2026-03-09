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
}

bool CamBANGDevNode::start_scenario(const godot::String& name) {
    if (!runtime_ || !runtime_->is_running()) {
        UtilityFunctions::printerr("[CamBANGDevNode] start_scenario requires a running runtime.");
        return false;
    }
    const godot::String normalized = name.strip_edges().to_lower();

    if (normalized == "stream_lifecycle_versions") {
        if (bringup_state_ != BringUpState::Running) {
            UtilityFunctions::printerr("[CamBANGDevNode] start_scenario stream_lifecycle_versions dispatch failed: provider stream is not running.");
            return false;
        }
        active_scenario_ = ActiveScenario::StreamLifecycleVersions;
        scenario_tick_ = 0;
        UtilityFunctions::print("[CamBANGDevNode] scenario started: stream_lifecycle_versions");
        return true;
    }

    if (normalized == "topology_change_versions") {
        if (bringup_state_ != BringUpState::Running) {
            UtilityFunctions::printerr("[CamBANGDevNode] start_scenario topology_change_versions dispatch failed: provider stream is not running.");
            return false;
        }
        // Existing scenario name accepted for dev glue completeness.
        // Do not route this path through endpoint/device open in start_scenario.
        active_scenario_ = ActiveScenario::None;
        UtilityFunctions::print("[CamBANGDevNode] scenario started: topology_change_versions");
        return true;
    }

    if (normalized == "publication_coalescing") {
        if (bringup_state_ != BringUpState::Running) {
            UtilityFunctions::printerr("[CamBANGDevNode] start_scenario publication_coalescing dispatch failed: provider stream is not running.");
            return false;
        }
        active_scenario_ = ActiveScenario::PublicationCoalescing;
        scenario_tick_ = 0;
        UtilityFunctions::print("[CamBANGDevNode] scenario started: publication_coalescing");
        return true;
    }

    UtilityFunctions::printerr("[CamBANGDevNode] start_scenario resolution failed: unknown scenario name '", name, "'.");
    return false;
}

void CamBANGDevNode::_enter_tree() {
    if (godot::Engine::get_singleton()->is_editor_hint()) {
        // Don’t start runtime in the editor; avoids crashes when the editor auto-opens scenes.
        set_process(false);
        return;
    }
    if (s_live.exchange(true)) {
        UtilityFunctions::printerr("[CamBANGDevNode] Another instance already live; freeing this node.");
        queue_free();
        return;
    }

    start_runtime_();
    set_process(true);
}

void CamBANGDevNode::_exit_tree() {
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

    if (bringup_state_ == BringUpState::Running) {
        tick_active_scenario_();
    }


    // Dev-only pattern cycling: stream-scoped PictureConfig updates.
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
        UtilityFunctions::printerr("[CamBANGDevNode] No CamBANGServer singleton found. Ensure the CamBANG GDExtension is loaded.");
        s_live.store(false);
        queue_free();
        return;
    }

    runtime_ = server->runtime_for_dev();
    if (!runtime_) {
        UtilityFunctions::printerr("[CamBANGDevNode] CamBANGServer runtime is unavailable.");
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
        UtilityFunctions::printerr("[CamBANGDevNode] CamBANGServer failed to start runtime.");
        s_live.store(false);
        queue_free();
        return;
    }

    started_ = true;
    last_running_ = runtime_->is_running();

    // Bring up provider for the initial running state.
    if (last_running_) {
        if (!start_provider_()) {
            UtilityFunctions::printerr("[CamBANGDevNode] Provider bring-up failed.");
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

    // Dev-only pattern cycling configuration (visual verification aid).
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

    // Best-effort dev teardown; core shutdown remains deterministic.
    // Streams are core-owned; tear down via CoreRuntime to avoid split-brain state.
    (void)runtime_->try_stop_stream(stream_id_);
    (void)runtime_->try_destroy_stream(stream_id_);

    // Device open/close is still provider-direct in this dev node (for now).
    if (provider_) {
        (void)provider_->close_device(device_instance_id_);
    }

    // Provider lifetime is owned by CamBANGServer; do not detach/shutdown here.
    provider_ = nullptr;
    emit_accum_ = 0.0;
    pattern_cycle_accum_s_ = 0.0;

    bringup_state_ = BringUpState::Idle;
    bringup_ticks_ = 0;
    active_scenario_ = ActiveScenario::None;
    scenario_tick_ = 0;
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
            return;
        }
        // Busy => retry next tick.
        // InvalidArgument is expected transiently if create hasn't executed
        // on the core thread yet; retry.
        return;
    }
}

void CamBANGDevNode::tick_active_scenario_() {
    if (!runtime_) {
        active_scenario_ = ActiveScenario::None;
        return;
    }

    if (active_scenario_ == ActiveScenario::None) {
        return;
    }

    ++scenario_tick_;

    if (active_scenario_ == ActiveScenario::PublicationCoalescing) {
        // Per-tick burst: multiple snapshot-affecting stream updates in one tick
        // to exercise Godot-side coalescing without endpoint/device churn.
        PictureConfig cfg_a{};
        cfg_a.preset = PatternPreset::XyXor;
        cfg_a.seed = scenario_seed_;
        cfg_a.overlay_frame_index_offsets = false;
        cfg_a.overlay_moving_bar = true;

        PictureConfig cfg_b = cfg_a;
        cfg_b.seed = scenario_seed_ + 1;

        PictureConfig cfg_c = cfg_a;
        cfg_c.seed = scenario_seed_ + 2;

        (void)runtime_->try_set_stream_picture_config(stream_id_, cfg_a);
        (void)runtime_->try_set_stream_picture_config(stream_id_, cfg_b);
        (void)runtime_->try_set_stream_picture_config(stream_id_, cfg_c);

        scenario_seed_ += 3;
        if (scenario_tick_ >= 4u) {
            active_scenario_ = ActiveScenario::None;
        }
        return;
    }

    if (active_scenario_ == ActiveScenario::StreamLifecycleVersions) {
        // Emit deterministic publish activity over several ticks without touching
        // endpoint enumeration/device opening.
        if ((scenario_tick_ % 2u) == 0u) {
            PictureConfig cfg{};
            cfg.preset = PatternPreset::XyXor;
            cfg.seed = scenario_seed_++;
            cfg.overlay_frame_index_offsets = false;
            cfg.overlay_moving_bar = true;
            (void)runtime_->try_set_stream_picture_config(stream_id_, cfg);
        }
        if (scenario_tick_ >= 12u) {
            active_scenario_ = ActiveScenario::None;
        }
        return;
    }
}

void CamBANGDevNode::stop_runtime_() {
    if (!runtime_) {
        provider_ = nullptr;
        started_ = false;
        last_running_ = false;
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

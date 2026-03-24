#pragma once

#include <atomic>
#include <memory>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/string.hpp>

#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

class CoreRuntime;
class LatestFrameMailbox;
class ProviderBroker;

// Dev-only scaffolding node.
// - Does NOT own CoreRuntime (CamBANGServer owns it).
// - May auto-start/auto-stop CamBANGServer for scene convenience.
// - Drives visibility providers (stub or Windows MF) in dev builds.
class CamBANGDevNode : public godot::Node {
    GDCLASS(CamBANGDevNode, godot::Node)

public:
    CamBANGDevNode();
    ~CamBANGDevNode() override;

    void _enter_tree() override;
    void _exit_tree() override;
    void _process(double delta) override;

    // Dev-only read access for display nodes.
    const LatestFrameMailbox* get_latest_frame_mailbox() const;

    // Dev-only scenario trigger.
    // Returns true when the named scenario trigger was accepted.
    bool start_scenario(const godot::String& name);

protected:
    static void _bind_methods();

private:
    static std::atomic<bool> s_live;

    CoreRuntime* runtime_ = nullptr;          // owned by CamBANGServer
    bool started_server_ = false;             // dev-only: whether this node started the server
    ProviderBroker* provider_ = nullptr;      // owned by CamBANGServer
    bool started_ = false;

    // Track runtime running state so we can handle external start/stop cycles
    // (e.g. automated stress scripts that call CamBANGServer.start()/stop()).
    bool last_running_ = false;

    // Dev-only scaffolding ids.
    uint64_t device_instance_id_ = 1;
    uint64_t root_id_ = 1;
    uint64_t stream_id_ = 1;

    double emit_accum_ = 0.0;

    // Dev-only pattern cycling (visual verification aid).
    bool pattern_cycle_enabled_ = false;
    double pattern_cycle_period_s_ = 2.0;
    double pattern_cycle_accum_s_ = 0.0;
    uint32_t pattern_cycle_index_ = 0;
    bool pattern_cycle_logged_unsupported_ = false;

    // Dev-only provider bring-up state machine.
    enum class BringUpState {
        Idle,
        DeviceOpened,
        CreatePending,
        StartPending,
        Running,
    };
    BringUpState bringup_state_ = BringUpState::Idle;
    uint32_t bringup_ticks_ = 0; // counts _process ticks while bring-up is active

    // Cached effective config used for stream create/start.
    CaptureProfile effective_profile_{};
    PictureConfig effective_picture_{};
    uint64_t effective_profile_version_ = 1;

    // Dev-only lightweight scenario runner (Godot abuse scenes).
    enum class ActiveScenario {
        None,
        StreamLifecycleVersions,
        PublicationCoalescing,
    };
    ActiveScenario active_scenario_ = ActiveScenario::None;
    ActiveScenario pending_scenario_ = ActiveScenario::None;
    uint32_t scenario_tick_ = 0;
    uint32_t scenario_seed_ = 1;


    void start_runtime_();
    void stop_runtime_();

    // Dev-only provider lifecycle (re)establishment.
    bool start_provider_();
    void stop_provider_();

    // Bring-up continues asynchronously (core commands are non-blocking).
    void tick_bringup_();
    void tick_active_scenario_();
    bool dispatch_scenario_now_(ActiveScenario scenario);
    static godot::String scenario_name_(ActiveScenario scenario);
    void mark_exit_reason_(const godot::String& reason);

    godot::String exit_reason_ = "none";
};

} // namespace cambang

#pragma once

#include <atomic>
#include <memory>

#include <godot_cpp/classes/node.hpp>

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

    void start_runtime_();
    void stop_runtime_();

    // Dev-only provider lifecycle (re)establishment.
    bool start_provider_();
    void stop_provider_();
};

} // namespace cambang

#pragma once

#include <atomic>
#include <memory>

#include <godot_cpp/classes/node.hpp>

namespace cambang {

    class CoreRuntime;
    class LatestFrameMailbox;
    class ICameraProvider;

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

        CoreRuntime* runtime_ = nullptr; // owned by CamBANGServer
        bool started_server_ = false;    // dev-only: whether this node started the server
        std::unique_ptr<ICameraProvider> provider_;
        bool started_ = false;

        // Dev-only scaffolding ids.
        uint64_t device_instance_id_ = 1;
        uint64_t root_id_ = 1;
        uint64_t stream_id_ = 1;

        double emit_accum_ = 0.0;

        void start_runtime_();
        void stop_runtime_();
    };

} // namespace cambang
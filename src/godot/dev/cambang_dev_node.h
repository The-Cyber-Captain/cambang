#pragma once

#include <atomic>
#include <memory>

#include <godot_cpp/classes/node.hpp>

namespace cambang {

    class CoreRuntime;

    class CamBANGDevNode : public godot::Node {
        GDCLASS(CamBANGDevNode, godot::Node)

    public:
        CamBANGDevNode();
        ~CamBANGDevNode() override;

        void _enter_tree() override;
        void _exit_tree() override;

    protected:
        static void _bind_methods();

    private:
        static std::atomic<bool> s_live;

        std::unique_ptr<CoreRuntime> runtime_;
        bool started_ = false;

        void start_runtime_();
        void stop_runtime_();
    };

} // namespace cambang
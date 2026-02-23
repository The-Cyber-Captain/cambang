#include "godot/dev/cambang_dev_node.h"

#include "core/core_runtime.h"

#include <godot_cpp/variant/utility_functions.hpp>

using godot::UtilityFunctions;

namespace cambang {

    std::atomic<bool> CamBANGDevNode::s_live{false};

    CamBANGDevNode::CamBANGDevNode() = default;

    CamBANGDevNode::~CamBANGDevNode() {
        stop_runtime_();
    }

    void CamBANGDevNode::_bind_methods() {
        // Temporary scaffolding: no exposed methods/properties.
    }

    void CamBANGDevNode::_enter_tree() {
        if (s_live.exchange(true)) {
            UtilityFunctions::printerr("[CamBANGDevNode] Another instance already live; freeing this node.");
            queue_free();
            return;
        }

        start_runtime_();
    }

    void CamBANGDevNode::_exit_tree() {
        stop_runtime_();
        s_live.store(false);
    }

    void CamBANGDevNode::start_runtime_() {
        if (started_) return;

        UtilityFunctions::print("[CamBANGDevNode] Starting CoreRuntime...");
        runtime_ = std::make_unique<CoreRuntime>();

        const bool ok = runtime_->start();
        if (!ok) {
            UtilityFunctions::printerr("[CamBANGDevNode] CoreRuntime failed to start; freeing node.");
            runtime_.reset();
            started_ = false;
            s_live.store(false);
            queue_free();
            return;
        }

        started_ = true;
    }

    void CamBANGDevNode::stop_runtime_() {
        if (!started_) return;

        UtilityFunctions::print("[CamBANGDevNode] Stopping CoreRuntime...");
        if (runtime_) {
            runtime_->stop();
            runtime_.reset();
        }

        started_ = false;
    }

} // namespace cambang
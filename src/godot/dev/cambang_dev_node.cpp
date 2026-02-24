#include "godot/dev/cambang_dev_node.h"

#include "core/core_runtime.h"
#include "core/latest_frame_mailbox.h"

#include "provider/stub/stub_camera_provider.h"

#include <vector>

#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/engine.hpp>

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
		// Dev-only: drive stub provider frames on the main thread (no extra threads).
		if (!started_ || !provider_) {
			return;
		}

		emit_accum_ += delta;
		constexpr double kFramePeriod = 1.0 / 30.0;
		if (emit_accum_ < kFramePeriod) {
			return;
		}
		emit_accum_ -= kFramePeriod;
		provider_->emit_test_frames(stream_id_, 1);
	}

	const LatestFrameMailbox* CamBANGDevNode::get_latest_frame_mailbox() const {
		if (!runtime_) {
			return nullptr;
		}
		return &runtime_->latest_frame_mailbox();
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

	        // Dev-only: attach and start the stub provider to drive the end-to-end path.
	        provider_ = std::make_unique<StubCameraProvider>();
	        runtime_->attach_provider(provider_.get());

	        ProviderResult pr = provider_->initialize(runtime_->provider_callbacks());
	        if (!pr.ok()) {
	            UtilityFunctions::printerr("[CamBANGDevNode] StubCameraProvider initialize failed.");
	            stop_runtime_();
	            s_live.store(false);
	            queue_free();
	            return;
	        }

	        std::vector<CameraEndpoint> eps;
	        pr = provider_->enumerate_endpoints(eps);
	        if (!pr.ok() || eps.empty()) {
	            UtilityFunctions::printerr("[CamBANGDevNode] StubCameraProvider enumerate_endpoints failed.");
	            stop_runtime_();
	            s_live.store(false);
	            queue_free();
	            return;
	        }

	        pr = provider_->open_device(eps[0].hardware_id, device_instance_id_, root_id_);
	        if (!pr.ok()) {
	            UtilityFunctions::printerr("[CamBANGDevNode] StubCameraProvider open_device failed.");
	            stop_runtime_();
	            s_live.store(false);
	            queue_free();
	            return;
	        }

	        const uint32_t RGBA_FOURCC = (static_cast<uint32_t>('R')) |
	                                    (static_cast<uint32_t>('G') << 8) |
	                                    (static_cast<uint32_t>('B') << 16) |
	                                    (static_cast<uint32_t>('A') << 24);

	        StreamRequest req{};
	        req.stream_id = stream_id_;
	        req.device_instance_id = device_instance_id_;
	        req.intent = StreamIntent::PREVIEW;
	        req.width = 320;
	        req.height = 180;
	        req.format_fourcc = RGBA_FOURCC;
	        req.target_fps_min = 30;
	        req.target_fps_max = 30;
	        req.profile_version = 1;

	        pr = provider_->create_stream(req);
	        if (!pr.ok()) {
	            UtilityFunctions::printerr("[CamBANGDevNode] StubCameraProvider create_stream failed.");
	            stop_runtime_();
	            s_live.store(false);
	            queue_free();
	            return;
	        }

	        pr = provider_->start_stream(stream_id_);
	        if (!pr.ok()) {
	            UtilityFunctions::printerr("[CamBANGDevNode] StubCameraProvider start_stream failed.");
	            stop_runtime_();
	            s_live.store(false);
	            queue_free();
	            return;
	        }
    }

    void CamBANGDevNode::stop_runtime_() {
        if (!started_) return;

	        UtilityFunctions::print("[CamBANGDevNode] Stopping CoreRuntime...");
	        if (provider_) {
	            provider_->shutdown();
	            provider_.reset();
	        }
	        if (runtime_) {
	            runtime_->stop();
	            runtime_.reset();
	        }

        started_ = false;
    }

} // namespace cambang
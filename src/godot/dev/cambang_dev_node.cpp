#include "godot/dev/cambang_dev_node.h"

#include "godot/cambang_server.h"
#include "core/core_runtime.h"
#include "core/latest_frame_mailbox.h"

#include "provider/icamera_provider.h"
#include "provider/provider_contract_datatypes.h"

#if defined(CAMBANG_PROVIDER_WINDOWS_MF) && CAMBANG_PROVIDER_WINDOWS_MF
  #include "provider/windows_mediafoundation/windows_mf_provider.h"
#else
  #include "provider/stub/stub_camera_provider.h"
#endif

#include <vector>
#include <string>
#include <cctype>

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
#if !defined(CAMBANG_PROVIDER_WINDOWS_MF) || !CAMBANG_PROVIDER_WINDOWS_MF
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

    // Safe: this code is only compiled when the stub provider is selected.
    auto* stub = static_cast<StubCameraProvider*>(provider_.get());
    stub->emit_test_frames(stream_id_, 1);
#else
    (void)delta;
#endif
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

    // Dev-only: attach and start a provider to drive the end-to-end path.
#if defined(CAMBANG_PROVIDER_WINDOWS_MF) && CAMBANG_PROVIDER_WINDOWS_MF
    provider_ = std::make_unique<WindowsMfCameraProvider>();
#else
    provider_ = std::make_unique<StubCameraProvider>();
#endif
    runtime_->attach_provider(provider_.get());

    ProviderResult pr = provider_->initialize(runtime_->provider_callbacks());
    if (!pr.ok()) {
        UtilityFunctions::printerr("[CamBANGDevNode] Provider initialize failed.");
        stop_runtime_();
        s_live.store(false);
        queue_free();
        return;
    }

    
std::vector<CameraEndpoint> eps;
pr = provider_->enumerate_endpoints(eps);
if (!pr.ok() || eps.empty()) {
    UtilityFunctions::printerr("[CamBANGDevNode] Provider enumerate_endpoints failed.");
    stop_runtime_();
    s_live.store(false);
    queue_free();
    return;
}

// Dev visibility: list endpoints once at startup.
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
        stop_runtime_();
        s_live.store(false);
        queue_free();
        return;
    }

    StreamRequest req{};
    req.stream_id = stream_id_;
    req.device_instance_id = device_instance_id_;
    req.intent = StreamIntent::PREVIEW;
    req.width = 320;
    req.height = 180;

#if defined(CAMBANG_PROVIDER_WINDOWS_MF) && CAMBANG_PROVIDER_WINDOWS_MF
    // MF common output is BGRA-ish; dev mailbox will swizzle BGRA -> RGBA.
    req.format_fourcc = FOURCC_BGRA;
    req.target_fps_min = 30;
    req.target_fps_max = 60;
#else
    req.format_fourcc = FOURCC_RGBA;
    req.target_fps_min = 30;
    req.target_fps_max = 30;
#endif

    req.profile_version = 1;

    pr = provider_->create_stream(req);
    if (!pr.ok()) {
        UtilityFunctions::printerr("[CamBANGDevNode] Provider create_stream failed.");
        stop_runtime_();
        s_live.store(false);
        queue_free();
        return;
    }

    pr = provider_->start_stream(stream_id_);
    if (!pr.ok()) {
        UtilityFunctions::printerr("[CamBANGDevNode] Provider start_stream failed.");
        stop_runtime_();
        s_live.store(false);
        queue_free();
        return;
    }
}

void CamBANGDevNode::stop_runtime_() {
    if (!runtime_) {
        provider_.reset();
        started_ = false;
        return;
    }

    // Best-effort dev teardown; core shutdown remains deterministic.
    if (provider_) {
        provider_->stop_stream(stream_id_);
        provider_->destroy_stream(stream_id_);
        provider_->close_device(device_instance_id_);
        provider_->shutdown();
    }

    runtime_->attach_provider(nullptr);

    // If this dev node started the server, stop it on teardown.
    if (started_server_) {
        if (auto* server = CamBANGServer::get_singleton()) {
            server->stop();
        }
        started_server_ = false;
    }

    // Do not stop the runtime; it is owned by CamBANGServer.
    runtime_ = nullptr;
    provider_.reset();
    started_ = false;
}

} // namespace cambang

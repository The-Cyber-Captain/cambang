// Death test for CoreRuntime::check_core_thread_liveness() (see
// docs/dev/current_tranche.md, Step 2: enforce the provider prompt/bounded
// contract).
//
// This binary deliberately induces a provider call that blocks well past
// the documented prompt/bounded contract (provider_architecture.md Section
// 8.1), on purpose, so the watchdog's abort path fires. Because this is a
// CAMBANG_INTERNAL_SMOKE build, check_core_thread_liveness() calls
// std::abort() once staleness is detected -- this process is EXPECTED to
// crash, not exit cleanly. It cannot self-report PASS/FAIL by normal return
// (abort() ends the process at the point of detection). See
// scripts/run_core_thread_liveness_watchdog_verify.ps1, which launches this
// binary as a child process and asserts it terminates via abort/crash (not
// a clean exit, not a hang) within a bounded window -- the actual PASS/FAIL
// verdict is produced there, not here.

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "core_thread_liveness_watchdog_verify: build through the repo SCons maintainer_tools alias so CAMBANG_INTERNAL_SMOKE=1 is defined."
#endif

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/core_runtime.h"
#include "core/state_snapshot_buffer.h"
#include "imaging/api/icamera_provider.h"
#include "imaging/stub/provider.h"

namespace cambang {
namespace {

// Forwards every ICameraProvider call to a real StubProvider (StubProvider
// is `final`, so composition rather than inheritance), except
// trigger_capture(), which sleeps well past both CoreRuntime's 2s
// caller-facing future::wait_for() bound and the 5s core-thread staleness
// threshold before forwarding. This deliberately violates
// provider_architecture.md Section 8.1's "prompt, bounded" contract.
class HangingCaptureProvider final : public ICameraProvider {
public:
  explicit HangingCaptureProvider(std::chrono::milliseconds hang_for)
      : inner_(std::make_unique<StubProvider>()), hang_for_(hang_for) {}

  const char* provider_name() const override { return inner_->provider_name(); }
  ProviderKind provider_kind() const noexcept override { return inner_->provider_kind(); }
  StreamTemplate stream_template() const override { return inner_->stream_template(); }
  CaptureTemplate capture_template() const override { return inner_->capture_template(); }
  bool supports_stream_picture_updates() const noexcept override {
    return inner_->supports_stream_picture_updates();
  }
  bool supports_capture_picture_updates() const noexcept override {
    return inner_->supports_capture_picture_updates();
  }
  bool supports_multi_image_still_sequence() const noexcept override {
    return inner_->supports_multi_image_still_sequence();
  }
  ProducerBackingCapabilities stream_backing_capabilities(
      const CaptureProfile& profile, const PictureConfig& picture) const noexcept override {
    return inner_->stream_backing_capabilities(profile, picture);
  }
  ProducerBackingCapabilities capture_backing_capabilities(
      const CaptureRequest& req) const noexcept override {
    return inner_->capture_backing_capabilities(req);
  }
  uint64_t stream_backing_plan_evaluation_settle_delay_ns() const noexcept override {
    return inner_->stream_backing_plan_evaluation_settle_delay_ns();
  }
  uint64_t capture_backing_plan_evaluation_settle_delay_ns() const noexcept override {
    return inner_->capture_backing_plan_evaluation_settle_delay_ns();
  }

  ProviderResult initialize(IProviderCallbacks* callbacks) override {
    return inner_->initialize(callbacks);
  }
  ProviderResult enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) override {
    return inner_->enumerate_endpoints(out_endpoints);
  }
  ProviderResult open_device(
      const std::string& hardware_id, uint64_t device_instance_id, uint64_t root_id) override {
    return inner_->open_device(hardware_id, device_instance_id, root_id);
  }
  ProviderResult close_device(uint64_t device_instance_id) override {
    return inner_->close_device(device_instance_id);
  }
  ProviderResult create_stream(const StreamRequest& req) override {
    return inner_->create_stream(req);
  }
  ProviderResult destroy_stream(uint64_t stream_id) override {
    return inner_->destroy_stream(stream_id);
  }
  ProviderResult start_stream(
      uint64_t stream_id, const CaptureProfile& profile, const PictureConfig& picture) override {
    return inner_->start_stream(stream_id, profile, picture);
  }
  ProviderResult stop_stream(uint64_t stream_id) override { return inner_->stop_stream(stream_id); }
  ProviderResult update_stream_retained_production_plan(
      uint64_t stream_id, CoreRetainedProductionPlan requested_retained_plan) override {
    return inner_->update_stream_retained_production_plan(stream_id, requested_retained_plan);
  }
  ProviderResult set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) override {
    return inner_->set_stream_picture_config(stream_id, picture);
  }
  ProviderResult set_capture_picture_config(
      uint64_t device_instance_id, const PictureConfig& picture) override {
    return inner_->set_capture_picture_config(device_instance_id, picture);
  }
  ProviderResult sync_capture_parent_priming(const CaptureRequest& req) override {
    return inner_->sync_capture_parent_priming(req);
  }
  ProviderResult release_capture_parent_priming(uint64_t device_instance_id) override {
    return inner_->release_capture_parent_priming(device_instance_id);
  }

  ProviderResult trigger_capture(const CaptureRequest& req) override {
    std::fprintf(stderr,
                 "[core_thread_liveness_watchdog_verify] HangingCaptureProvider::trigger_capture "
                 "sleeping for %llds to deliberately violate the prompt/bounded contract...\n",
                 static_cast<long long>(
                     std::chrono::duration_cast<std::chrono::seconds>(hang_for_).count()));
    std::fflush(stderr);
    std::this_thread::sleep_for(hang_for_);
    return inner_->trigger_capture(req);
  }

  ProviderResult abort_capture(uint64_t capture_id) override {
    return inner_->abort_capture(capture_id);
  }

  ProviderResult apply_camera_spec_patch(
      const std::string& hardware_id,
      uint64_t new_camera_spec_version,
      SpecPatchView patch) override {
    return inner_->apply_camera_spec_patch(hardware_id, new_camera_spec_version, patch);
  }
  ProviderResult apply_imaging_spec_patch(
      uint64_t new_imaging_spec_version, SpecPatchView patch) override {
    return inner_->apply_imaging_spec_patch(new_imaging_spec_version, patch);
  }

  ProviderResult shutdown() override { return inner_->shutdown(); }

private:
  std::unique_ptr<ICameraProvider> inner_;
  std::chrono::milliseconds hang_for_;
};

bool wait_until(const std::function<bool()>& pred, int max_iters, int sleep_ms) {
  for (int i = 0; i < max_iters; ++i) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  return false;
}

} // namespace
} // namespace cambang

int main() {
  using namespace cambang;

  CoreRuntime runtime;
  StateSnapshotBuffer buffer;
  runtime.set_snapshot_publisher(&buffer);

  if (!runtime.start()) {
    std::fprintf(stderr, "FAIL: runtime start failed\n");
    return 1;
  }
  if (!wait_until([&]() { return runtime.state_copy() == CoreRuntimeState::LIVE; }, 500, 5)) {
    std::fprintf(stderr, "FAIL: timed out waiting for runtime LIVE\n");
    runtime.stop();
    return 1;
  }

  // Sleep comfortably past both the 2s caller-facing bound and the 5s
  // core-thread staleness threshold.
  auto provider = std::make_unique<HangingCaptureProvider>(std::chrono::seconds(8));

  if (!provider->initialize(runtime.provider_callbacks()).ok()) {
    std::fprintf(stderr, "FAIL: provider initialize failed\n");
    runtime.stop();
    return 1;
  }

  std::vector<CameraEndpoint> endpoints;
  if (!provider->enumerate_endpoints(endpoints).ok() || endpoints.empty()) {
    std::fprintf(stderr, "FAIL: enumerate_endpoints failed\n");
    (void)provider->shutdown();
    runtime.stop();
    return 1;
  }

  runtime.attach_provider(provider.get());

  constexpr uint64_t kDeviceId = 100;
  constexpr uint64_t kRootId = 900;
  if (runtime.retain_device_identity(kDeviceId, endpoints.front().hardware_id) !=
      CoreThread::PostResult::Enqueued) {
    std::fprintf(stderr, "FAIL: retain_device_identity admission failed\n");
    runtime.attach_provider(nullptr);
    runtime.stop();
    return 1;
  }
  if (!provider->open_device(endpoints.front().hardware_id, kDeviceId, kRootId).ok()) {
    std::fprintf(stderr, "FAIL: open_device failed\n");
    runtime.attach_provider(nullptr);
    runtime.stop();
    return 1;
  }
  if (!wait_until(
          [&]() {
            auto snap = buffer.snapshot_copy();
            if (!snap) {
              return false;
            }
            for (const auto& d : snap->devices) {
              if (d.instance_id == kDeviceId) {
                return true;
              }
            }
            return false;
          },
          500,
          5)) {
    std::fprintf(stderr, "FAIL: timed out waiting for device open publish\n");
    runtime.attach_provider(nullptr);
    runtime.stop();
    return 1;
  }

  std::fprintf(stderr,
               "[core_thread_liveness_watchdog_verify] triggering device capture; expect the "
               "core thread to wedge inside HangingCaptureProvider::trigger_capture and the "
               "watchdog to abort this process within a few seconds of that call reaching the "
               "provider.\n");
  std::fflush(stderr);

  // This call blocks the calling (main) thread for up to CoreRuntime's own
  // internal 2s future::wait_for() bound and then returns -- it does not
  // wait for the full 8s hang. The core thread, however, remains wedged
  // inside HangingCaptureProvider::trigger_capture() regardless of what this
  // call returns; check_core_thread_liveness() (polled below, exactly as a
  // real Godot tick would call it) is what is actually expected to observe
  // that and abort the process.
  uint64_t capture_id = 1;
  (void)runtime.try_trigger_device_capture_with_capture_id_for_server(kDeviceId, capture_id);

  const auto poll_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < poll_deadline) {
    runtime.check_core_thread_liveness();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::fprintf(stderr,
               "FAIL: watchdog did not abort this process within the expected window; the "
               "prompt/bounded contract enforcement did not fire.\n");
  runtime.attach_provider(nullptr);
  runtime.stop();
  return 1;
}

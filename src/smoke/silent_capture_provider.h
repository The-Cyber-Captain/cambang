#pragma once

// A provider that accepts a capture and then never speaks about it again.
//
// Two Core paths terminalise a capture without the provider having produced a
// terminal fact, and both must settle what the abandoned capture still owes the
// platform (capture_identity_and_lifecycle.md 5.4 and 7):
//
//   * the capture-admission watchdog, when a device stays silent past
//     ICameraProvider::capture_admission_watchdog_timeout_ns(); and
//   * device loss, when a device carrying an in-flight capture is closed.
//
// Both call ICameraProvider::abort_capture() at the moment of abandonment. That
// call is the whole point: Core has just declared a capture over while the
// provider may still be holding its buffers, and a payload released later can
// otherwise be delivered into a subsequent capture on the same device and
// attributed to it. Attribution must be by accounting, never by timing.
//
// Neither path could be proved before this existed. StubProvider completes a
// capture synchronously, so its captures are never abandoned, and it is `final`
// so it cannot be subclassed into one that stalls. This wraps it instead: every
// call forwards to the real StubProvider, except the two that matter.
//
//   * trigger_capture()/trigger_capture_submission() report success WITHOUT
//     telling the inner provider, so Core admits a capture no terminal fact
//     will ever arrive for. This is the silence being modelled.
//   * abort_capture() records the id rather than forwarding it -- the inner
//     provider never saw the capture, so it has nothing to abort.
//
// The watchdog timeout is settable because the contract default is 30s and a
// test cannot wait that long. That is the one value a test may legitimately
// shorten: icamera_provider.h warns against overriding it with a guess, but the
// warning is about real providers mistaking a slow device for a broken one.
// Here the silence is deliberate and total.
//
// Maintainer test scaffolding. Not a product provider, and it does not belong
// in src/imaging/ -- it deliberately violates the provider contract by
// accepting work it never reports on.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "imaging/api/icamera_provider.h"
#include "imaging/stub/provider.h"

namespace cambang {

class SilentCaptureProvider final : public ICameraProvider {
public:
  explicit SilentCaptureProvider(StubProvider& inner) : inner_(inner) {}
  ~SilentCaptureProvider() override = default;

  // --- the silence -------------------------------------------------------
  //
  // Accept and say nothing. Core admits the capture, starts its watchdog, and
  // will wait for a terminal fact that is never coming.
  ProviderResult trigger_capture(const CaptureRequest& req) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      accepted_.push_back(req.capture_id);
    }
    return ProviderResult::success();
  }

  // Record, do not forward. The inner provider was never told about this
  // capture, so it has nothing to abort; forwarding would only test that
  // StubProvider rejects an unknown id.
  ProviderResult abort_capture(uint64_t capture_id) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      aborted_.push_back(capture_id);
    }
    return ProviderResult::success();
  }

  // --- test instrumentation (thread-safe; not part of the contract) ------
  //
  // abort_capture() arrives on the core thread while the test reads from its
  // own, so both sides take the lock.
  bool was_aborted(uint64_t capture_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const uint64_t id : aborted_) {
      if (id == capture_id) return true;
    }
    return false;
  }
  size_t abort_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return aborted_.size();
  }
  size_t accepted_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return accepted_.size();
  }

  void set_capture_admission_watchdog_timeout_ns(uint64_t ns) noexcept {
    watchdog_timeout_ns_.store(ns, std::memory_order_relaxed);
  }
  uint64_t capture_admission_watchdog_timeout_ns() const noexcept override {
    return watchdog_timeout_ns_.load(std::memory_order_relaxed);
  }

  // --- everything else is the inner provider, unchanged ------------------
  const char* provider_name() const override { return "silent_capture_test"; }
  ProviderKind provider_kind() const noexcept override { return inner_.provider_kind(); }

  StreamTemplate stream_template() const override { return inner_.stream_template(); }
  CaptureTemplate capture_template() const override { return inner_.capture_template(); }
  bool supports_stream_picture_updates() const noexcept override {
    return inner_.supports_stream_picture_updates();
  }
  bool supports_capture_picture_updates() const noexcept override {
    return inner_.supports_capture_picture_updates();
  }
  bool supports_multi_image_still_sequence() const noexcept override {
    return inner_.supports_multi_image_still_sequence();
  }

  uint64_t stream_backing_plan_evaluation_settle_delay_ns() const noexcept override {
    return inner_.stream_backing_plan_evaluation_settle_delay_ns();
  }
  uint64_t capture_backing_plan_evaluation_settle_delay_ns() const noexcept override {
    return inner_.capture_backing_plan_evaluation_settle_delay_ns();
  }
  uint64_t stream_reprovision_resume_timeout_ns() const noexcept override {
    return inner_.stream_reprovision_resume_timeout_ns();
  }

  ProducerBackingCapabilities stream_backing_capabilities(
      const CaptureProfile& profile,
      const PictureConfig& picture) const noexcept override {
    return inner_.stream_backing_capabilities(profile, picture);
  }
  ProducerBackingCapabilities capture_backing_capabilities(
      const CaptureRequest& req) const noexcept override {
    return inner_.capture_backing_capabilities(req);
  }
  ProducerFormatCapabilities stream_format_capabilities(
      const CaptureProfile& profile,
      const PictureConfig& picture) const noexcept override {
    return inner_.stream_format_capabilities(profile, picture);
  }
  ProducerFormatCapabilities capture_format_capabilities(
      const CaptureRequest& req) const noexcept override {
    return inner_.capture_format_capabilities(req);
  }
  ProducerFormatCapabilities stream_parent_context_format_capabilities(
      uint64_t device_instance_id,
      uint64_t stream_id,
      StreamIntent intent,
      const CaptureProfile& profile,
      const PictureConfig& picture) noexcept override {
    return inner_.stream_parent_context_format_capabilities(
        device_instance_id, stream_id, intent, profile, picture);
  }
  ProducerFormatCapabilities capture_parent_context_format_capabilities(
      uint64_t device_instance_id,
      const CaptureRequest& req) noexcept override {
    return inner_.capture_parent_context_format_capabilities(device_instance_id, req);
  }
  ProducerBackingCapabilities stream_parent_context_backing_capabilities(
      uint64_t device_instance_id,
      uint64_t stream_id,
      StreamIntent intent,
      const CaptureProfile& profile,
      const PictureConfig& picture) noexcept override {
    return inner_.stream_parent_context_backing_capabilities(
        device_instance_id, stream_id, intent, profile, picture);
  }
  ProducerBackingCapabilities capture_parent_context_backing_capabilities(
      uint64_t device_instance_id,
      const CaptureRequest& req) noexcept override {
    return inner_.capture_parent_context_backing_capabilities(device_instance_id, req);
  }
  AcquisitionCoexistence acquisition_coexistence(
      uint64_t device_instance_id,
      const AcquisitionUseSet& proposed) noexcept override {
    return inner_.acquisition_coexistence(device_instance_id, proposed);
  }

  ProviderResult initialize(IProviderCallbacks* callbacks) override {
    return inner_.initialize(callbacks);
  }
  ProviderResult enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) override {
    return inner_.enumerate_endpoints(out_endpoints);
  }
  ProviderResult open_device(
      const std::string& hardware_id,
      uint64_t device_instance_id,
      uint64_t root_id) override {
    return inner_.open_device(hardware_id, device_instance_id, root_id);
  }
  ProviderResult close_device(uint64_t device_instance_id) override {
    return inner_.close_device(device_instance_id);
  }
  ProviderResult create_stream(const StreamRequest& req) override {
    return inner_.create_stream(req);
  }
  ProviderResult destroy_stream(uint64_t stream_id) override {
    return inner_.destroy_stream(stream_id);
  }
  ProviderResult start_stream(
      uint64_t stream_id,
      const CaptureProfile& profile,
      const PictureConfig& picture) override {
    return inner_.start_stream(stream_id, profile, picture);
  }
  ProviderResult stop_stream(uint64_t stream_id) override {
    return inner_.stop_stream(stream_id);
  }
  ProviderResult update_stream_retained_production_plan(
      uint64_t stream_id,
      CoreRetainedProductionPlan requested_retained_plan) override {
    return inner_.update_stream_retained_production_plan(stream_id, requested_retained_plan);
  }
  ProviderResult set_stream_picture_config(
      uint64_t stream_id,
      const PictureConfig& picture) override {
    return inner_.set_stream_picture_config(stream_id, picture);
  }
  ProviderResult set_capture_picture_config(
      uint64_t device_instance_id,
      const PictureConfig& picture) override {
    return inner_.set_capture_picture_config(device_instance_id, picture);
  }
  ProviderResult sync_capture_parent_priming(const CaptureRequest& req) override {
    return inner_.sync_capture_parent_priming(req);
  }
  ProviderResult release_capture_parent_priming(uint64_t device_instance_id) override {
    return inner_.release_capture_parent_priming(device_instance_id);
  }
  ProviderResult apply_camera_spec_patch(
      const std::string& hardware_id,
      uint64_t new_camera_spec_version,
      SpecPatchView patch) override {
    return inner_.apply_camera_spec_patch(hardware_id, new_camera_spec_version, patch);
  }
  ProviderResult apply_imaging_spec_patch(
      uint64_t new_imaging_spec_version,
      SpecPatchView patch) override {
    return inner_.apply_imaging_spec_patch(new_imaging_spec_version, patch);
  }
  ProviderResult shutdown() override { return inner_.shutdown(); }

private:
  StubProvider& inner_;

  mutable std::mutex mu_;
  std::vector<uint64_t> accepted_;
  std::vector<uint64_t> aborted_;

  // Short by design; see the note at the top of this file.
  std::atomic<uint64_t> watchdog_timeout_ns_{150ull * 1000ull * 1000ull};
};

} // namespace cambang

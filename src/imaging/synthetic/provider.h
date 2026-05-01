#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include "imaging/api/icamera_provider.h"
#include "imaging/api/provider_strand.h"

#include "imaging/synthetic/config.h"
#include "imaging/synthetic/scenario_model.h"
#include "imaging/synthetic/scenario.h"
#include "imaging/synthetic/virtual_clock.h"
#include "pixels/pattern/cpu_packed_pattern_renderer.h"

namespace cambang {

class SyntheticProvider final : public ICameraProvider {
public:
  using TimelineRequestDispatchHook = std::function<void(const SyntheticScheduledEvent&)>;

  explicit SyntheticProvider(const SyntheticProviderConfig& cfg);
  ~SyntheticProvider() override = default;

  const char* provider_name() const override { return "SyntheticProvider"; }
  ProviderKind provider_kind() const noexcept override { return ProviderKind::synthetic; }


  StreamTemplate stream_template() const override;
  CaptureTemplate capture_template() const override;
  bool supports_stream_picture_updates() const noexcept override { return true; }
  bool supports_capture_picture_updates() const noexcept override { return true; }

  ProducerBackingCapabilities stream_backing_capabilities(
      const CaptureProfile& profile,
      const PictureConfig& picture) const noexcept override;
  ProducerBackingCapabilities capture_backing_capabilities(
      const CaptureRequest& req) const noexcept override;

  ProviderResult initialize(IProviderCallbacks* callbacks) override;
  ProviderResult enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) override;

  ProviderResult open_device(
      const std::string& hardware_id,
      uint64_t device_instance_id,
      uint64_t root_id) override;

  ProviderResult close_device(uint64_t device_instance_id) override;

  ProviderResult create_stream(const StreamRequest& req) override;
  ProviderResult destroy_stream(uint64_t stream_id) override;

  ProviderResult start_stream(
      uint64_t stream_id,
      const CaptureProfile& profile,
      const PictureConfig& picture) override;
  ProviderResult stop_stream(uint64_t stream_id) override;

  ProviderResult set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) override;
  ProviderResult set_capture_picture_config(uint64_t device_instance_id, const PictureConfig& picture) override;

  ProviderResult trigger_capture(const CaptureRequest& req) override;
  ProviderResult abort_capture(uint64_t capture_id) override;

  ProviderResult apply_camera_spec_patch(
      const std::string& hardware_id,
      uint64_t new_camera_spec_version,
      SpecPatchView patch) override;

  ProviderResult apply_imaging_spec_patch(
      uint64_t new_imaging_spec_version,
      SpecPatchView patch) override;

  ProviderResult shutdown() override;

  void advance(uint64_t dt_ns);

  // Smoke/scenario-only helpers. These preserve the main runtime architecture
  // while allowing deterministic lifecycle edge cases to be exercised.
  ProviderResult disconnect_device_for_test(uint64_t device_instance_id);
  ProviderResult force_close_device_for_test(uint64_t device_instance_id);
  ProviderResult fail_stream_for_test(uint64_t stream_id, ProviderError error);
  ProviderResult load_timeline_canonical_scenario_from_json_text_for_host(
      const std::string& text,
      std::string* error = nullptr);
  ProviderResult load_timeline_canonical_scenario_from_json_file_for_host(
      const std::string& path,
      std::string* error = nullptr);
  ProviderResult set_timeline_scenario_for_host(const SyntheticTimelineScenario& scenario);
  ProviderResult set_timeline_scenario_for_host(const SyntheticCanonicalScenario& scenario);
  ProviderResult start_timeline_scenario_for_host();
  ProviderResult stop_timeline_scenario_for_host();
  ProviderResult set_timeline_scenario_paused_for_host(bool paused);
  ProviderResult advance_timeline_for_host(uint64_t dt_ns);
  ProviderResult set_timeline_reconciliation_for_host(TimelineReconciliation reconciliation);
  void set_timeline_request_dispatch_hook_for_host(TimelineRequestDispatchHook hook);

private:
  CBProviderStrand strand_;
  struct TimelineEventCompare {
    bool operator()(const SyntheticScheduledEvent& a, const SyntheticScheduledEvent& b) const noexcept {
      if (a.at_ns != b.at_ns) {
        return a.at_ns > b.at_ns;
      }
      return a.seq > b.seq;
    }
  };

  void timeline_schedule_(uint64_t at_ns, SyntheticEventType type, uint64_t stream_id);
  void timeline_schedule_(const SyntheticScheduledEvent& ev);
  void timeline_dispatch_request_(const SyntheticScheduledEvent& ev);
  void timeline_activate_or_dispatch_(const SyntheticScheduledEvent& ev, bool allow_pending);
  bool timeline_destructive_prereq_ready_(const SyntheticScheduledEvent& ev, const char*& reason) const;
  bool timeline_is_destructive_primitive_(SyntheticEventType type) const;
  void timeline_pump_();
  bool materialize_staged_canonical_scenario_(SyntheticTimelineScenario& out, std::string& error) const;
  static bool has_runtime_gpu_backing_path_() noexcept;
  ProducerBackingCapabilities query_stream_producer_capabilities_(
      const CaptureProfile& profile,
      const PictureConfig& picture) const noexcept;
  ProducerBackingCapabilities query_capture_producer_capabilities_(
      const CaptureRequest& req) const noexcept;
  ProducerBackingCapabilities apply_verification_backing_override_(
      ProducerBackingCapabilities runtime_truth) const noexcept;
  bool choose_stream_gpu_preference_(ProducerBackingCapabilities capabilities) const noexcept;

  struct DeviceState {
    std::string hardware_id;
    uint64_t device_instance_id = 0;
    uint64_t root_id = 0;
    bool open = false;
    uint64_t native_id = 0;
    uint64_t acquisition_session_native_id = 0;
    uint32_t acquisition_session_stream_refs = 0;
    uint32_t acquisition_session_capture_refs = 0;
    PictureConfig capture_picture{};
  };

  struct StreamState {
    StreamRequest req{};
    bool created = false;
    bool started = false;
    bool producing = false;
    uint64_t next_due_ns = 0;
    uint64_t native_id = 0;
    uint64_t acquisition_session_native_id = 0;

    PictureConfig picture{};
    CpuPackedPatternRenderer renderer{};
    bool prefer_gpu_backing = false;
    std::vector<std::uint8_t> gpu_staging;
    std::shared_ptr<void> live_gpu_backing{};
    uint64_t live_gpu_backing_native_id = 0;
    uint32_t live_gpu_width = 0;
    uint32_t live_gpu_height = 0;
    uint32_t live_gpu_stride_bytes = 0;

    struct BufferSlot {
      uint64_t stream_id = 0;
      std::vector<std::uint8_t> bytes;
      std::atomic<bool> in_use{false};
    };
    std::vector<std::shared_ptr<BufferSlot>> pool;
    size_t pool_cursor = 0;
    uint32_t consecutive_behind_ticks = 0;
  };

  struct FrameReleaseLease {
    std::shared_ptr<StreamState::BufferSlot> slot;
    std::shared_ptr<std::vector<std::uint8_t>> bytes;
  };

  static void release_frame_(void* user, const FrameView* frame);
  bool is_known_hardware_id_(const std::string& hardware_id) const;

  uint64_t alloc_native_id_(NativeObjectType type);
  void emit_native_create_device_(const DeviceState& d);
  void emit_native_destroy_(uint64_t native_id);
  uint64_t ensure_native_acquisition_session_(DeviceState& d);
  void retain_native_acquisition_session_for_stream_(DeviceState& d);
  void retain_native_acquisition_session_for_capture_(DeviceState& d);
  void release_native_acquisition_session_for_stream_(uint64_t device_instance_id);
  void release_native_acquisition_session_for_capture_(uint64_t device_instance_id);
  void release_native_acquisition_session_if_unheld_(DeviceState& d);

  void emit_due_frames_();
  void emit_one_frame_(StreamState& s, uint64_t scheduled_capture_ns);
  bool ensure_stream_live_gpu_backing_(StreamState& s, uint32_t width, uint32_t height, uint32_t stride);
  void release_stream_live_gpu_backing_(StreamState& s);
  void emit_triage_trace_if_due_();
  static uint64_t generator_frame_ordinal_from_ns_(uint64_t timestamp_ns, const PictureConfig& picture) noexcept;

  void destroy_stream_storage_(std::map<uint64_t, StreamState>::iterator it,
                               ProviderError stop_error,
                               bool emit_stop_event);
  void close_device_storage_(std::map<uint64_t, DeviceState>::iterator it);

private:
  SyntheticProviderConfig cfg_{};
  IProviderCallbacks* callbacks_ = nullptr;
  bool initialized_ = false;
  bool shutting_down_ = false;

  SyntheticVirtualClock clock_;

  std::map<uint64_t, DeviceState> devices_;
  std::map<uint64_t, StreamState> streams_;

  uint64_t provider_native_id_ = 0;

  uint64_t timeline_seq_ = 0;
  SyntheticTimelineScenario timeline_scenario_{};
  SyntheticCanonicalScenario timeline_canonical_scenario_{};
  bool timeline_canonical_staged_ = false;
  bool timeline_running_ = false;
  bool timeline_paused_ = false;
  bool completion_gated_destructive_sequencing_enabled_ = false;
  std::priority_queue<SyntheticScheduledEvent,
                      std::vector<SyntheticScheduledEvent>,
                      TimelineEventCompare>
      timeline_q_;
  std::vector<SyntheticScheduledEvent> timeline_pending_destructive_;
  TimelineRequestDispatchHook timeline_request_dispatch_hook_{};

  // Triage-only stream scheduling/GPU-path instrumentation.
  bool triage_trace_enabled_ = false;
  uint32_t triage_catchup_cap_per_tick_ = 0;
  uint64_t triage_next_log_ns_ = 0;
  uint64_t triage_frames_emitted_total_ = 0;
  uint64_t triage_catchup_bursts_total_ = 0;
  uint64_t triage_catchup_ticks_capped_total_ = 0;
  uint64_t triage_catchup_frames_dropped_total_ = 0;
  uint32_t triage_catchup_max_frames_in_tick_ = 0;
  uint64_t triage_falling_behind_repeat_total_ = 0;
  uint64_t triage_gpu_update_attempts_total_ = 0;
  uint64_t triage_gpu_update_demand_skipped_total_ = 0;
  uint64_t triage_gpu_update_failures_total_ = 0;
  uint64_t triage_gpu_update_retries_total_ = 0;
  uint64_t triage_gpu_backing_recreate_total_ = 0;
  uint64_t triage_gpu_backing_release_total_ = 0;
  uint64_t triage_gpu_ensure_backing_calls_total_ = 0;
  uint64_t triage_gpu_ensure_backing_total_ns_ = 0;
  uint64_t triage_gpu_ensure_backing_max_ns_ = 0;
  uint64_t triage_gpu_update_total_calls_ = 0;
  uint64_t triage_gpu_update_total_ns_ = 0;
  uint64_t triage_gpu_update_total_max_ns_ = 0;
  uint64_t triage_timeline_pump_calls_ = 0;
  uint64_t triage_timeline_pump_total_ns_ = 0;
  uint64_t triage_timeline_pump_max_ns_ = 0;
  uint64_t triage_timeline_event_exec_calls_ = 0;
  uint64_t triage_timeline_event_exec_total_ns_ = 0;
  uint64_t triage_timeline_event_exec_max_ns_ = 0;
  uint64_t triage_timeline_emit_event_calls_ = 0;
  uint64_t triage_timeline_emit_event_total_ns_ = 0;
  uint64_t triage_timeline_emit_event_max_ns_ = 0;
  uint64_t triage_emit_frame_calls_ = 0;
  uint64_t triage_emit_frame_total_ns_ = 0;
  uint64_t triage_emit_frame_max_ns_ = 0;
  uint64_t triage_frame_render_calls_ = 0;
  uint64_t triage_frame_render_total_ns_ = 0;
  uint64_t triage_frame_render_max_ns_ = 0;
  uint64_t triage_render_spec_build_total_ns_ = 0;
  uint64_t triage_render_spec_build_max_ns_ = 0;
  uint64_t triage_render_target_prepare_total_ns_ = 0;
  uint64_t triage_render_target_prepare_max_ns_ = 0;
  uint64_t triage_render_allocation_or_resize_count_ = 0;
  uint64_t triage_frame_copy_calls_ = 0;
  uint64_t triage_frame_copy_total_ns_ = 0;
  uint64_t triage_frame_copy_max_ns_ = 0;
  uint64_t triage_post_frame_calls_ = 0;
  uint64_t triage_post_frame_total_ns_ = 0;
  uint64_t triage_post_frame_max_ns_ = 0;
  uint64_t triage_strand_flush_calls_ = 0;
  uint64_t triage_strand_flush_total_ns_ = 0;
  uint64_t triage_strand_flush_max_ns_ = 0;
  bool triage_timeline_path_banner_emitted_ = false;
  bool triage_nominal_path_banner_emitted_ = false;

  std::atomic<uint64_t> invalid_preset_requests_{0};
};

} // namespace cambang

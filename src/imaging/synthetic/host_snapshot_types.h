#pragma once

// Lightweight, dependency-free snapshot/topology types read by
// ProviderBroker's *_for_host() maintainer/test surface
// (get_synthetic_metrics_snapshot_for_host(), get_synthetic_staged_rig_topology_for_host()).
// Deliberately kept separate from imaging/synthetic/provider.h so
// provider_broker.h does not need the concrete SyntheticProvider class just
// to declare those two accessors.

#include <cstdint>
#include <string>
#include <vector>

namespace cambang {

struct SyntheticCaptureGpuBackingRetainPostureMetricsSnapshot {
  uint64_t calls = 0;
  double total_ms = 0.0;
  double max_ms = 0.0;
  bool has_first_call = false;
  double first_call_ms = 0.0;
  uint64_t later_calls = 0;
  double later_total_ms = 0.0;
  double later_max_ms = 0.0;
};

struct SyntheticCaptureReadyStagePostureMetricsSnapshot {
  uint64_t calls = 0;
  double pre_capture_started_total_ms = 0.0;
  double post_capture_started_before_first_member_total_ms = 0.0;
  double post_capture_started_member_iteration_gap_total_ms = 0.0;
  double post_capture_started_cpu_prep_total_ms = 0.0;
  double post_capture_started_gpu_retain_total_ms = 0.0;
  double post_capture_started_frame_assembly_total_ms = 0.0;
  double post_capture_started_post_frame_total_ms = 0.0;
  double post_capture_started_after_last_frame_before_terminal_total_ms = 0.0;
  double post_capture_started_after_last_frame_generation_tail_total_ms = 0.0;
  double post_capture_started_after_last_frame_return_to_capture_lock_total_ms = 0.0;
  double post_capture_started_after_last_frame_return_to_finish_begin_total_ms = 0.0;
  double post_capture_started_after_last_frame_post_return_timing_record_lock_wait_total_ms = 0.0;
  double post_capture_started_after_last_frame_post_return_timing_record_update_total_ms = 0.0;
  double post_capture_started_after_last_frame_post_return_after_timing_record_to_finish_begin_total_ms = 0.0;
  double post_capture_started_after_last_frame_post_return_after_timing_record_to_pre_return_total_ms = 0.0;
  double post_capture_started_after_last_frame_pre_return_to_finish_begin_total_ms = 0.0;
  double post_capture_started_after_last_frame_finish_begin_to_capture_lock_wait_begin_total_ms = 0.0;
  double post_capture_started_after_last_frame_capture_lock_wait_total_ms = 0.0;
  double post_capture_started_after_last_frame_finish_provider_state_total_ms = 0.0;
  double post_capture_started_after_last_frame_finish_non_state_total_ms = 0.0;
  double capture_terminal_post_total_ms = 0.0;
  double capture_ready_provider_window_total_ms = 0.0;
};

struct SyntheticCaptureReadyTimingRecordSnapshot {
  uint64_t capture_id = 0;
  uint64_t device_instance_id = 0;
  uint64_t acquisition_session_id = 0;
  bool primary_cpu = true;
  bool retain_cpu_sidecar = false;
  bool has_provider_post_capture_started_steady_ns = false;
  uint64_t provider_post_capture_started_steady_ns = 0;
  bool has_provider_post_capture_completed_steady_ns = false;
  uint64_t provider_post_capture_completed_steady_ns = 0;
  uint64_t provider_pre_capture_started_total_ns = 0;
  uint64_t provider_post_capture_started_before_first_member_total_ns = 0;
  uint64_t provider_post_capture_started_member_iteration_gap_total_ns = 0;
  uint64_t provider_post_capture_started_cpu_prep_total_ns = 0;
  uint64_t provider_post_capture_started_gpu_retain_total_ns = 0;
  uint64_t provider_post_capture_started_frame_assembly_total_ns = 0;
  uint64_t provider_post_capture_started_post_frame_total_ns = 0;
  uint64_t provider_post_capture_started_after_last_frame_before_terminal_total_ns = 0;
  uint64_t provider_post_capture_started_after_last_frame_generation_tail_total_ns = 0;
  uint64_t provider_post_capture_started_after_last_frame_return_to_capture_lock_total_ns = 0;
  uint64_t provider_post_capture_started_after_last_frame_return_to_finish_begin_total_ns = 0;
  uint64_t provider_post_capture_started_after_last_frame_post_return_timing_record_lock_wait_total_ns = 0;
  uint64_t provider_post_capture_started_after_last_frame_post_return_timing_record_update_total_ns = 0;
  uint64_t provider_post_capture_started_after_last_frame_post_return_after_timing_record_to_finish_begin_total_ns = 0;
  uint64_t provider_post_capture_started_after_last_frame_post_return_after_timing_record_to_pre_return_total_ns = 0;
  uint64_t provider_post_capture_started_after_last_frame_pre_return_to_finish_begin_total_ns = 0;
  uint64_t provider_base_bytes_use_count_after_timing_record = 0;
  uint64_t provider_base_bytes_use_count_pre_return = 0;
  uint64_t provider_post_capture_started_after_last_frame_finish_begin_to_capture_lock_wait_begin_total_ns = 0;
  uint64_t provider_post_capture_started_after_last_frame_capture_lock_wait_total_ns = 0;
  uint64_t provider_post_capture_started_after_last_frame_finish_provider_state_total_ns = 0;
  uint64_t provider_post_capture_started_after_last_frame_finish_non_state_total_ns = 0;
  uint64_t provider_capture_terminal_post_total_ns = 0;
  uint64_t provider_capture_ready_provider_window_total_ns = 0;
};

struct SyntheticMetricsSnapshot {
  uint64_t current_virtual_timeline_ns = 0;
  uint64_t total_emitted_frames = 0;
  uint64_t gpu_update_attempts = 0;
  uint64_t gpu_update_demand_skipped = 0;
  uint64_t capture_gpu_backing_retain_calls = 0;
  uint64_t gpu_texture_update_calls = 0;
  uint64_t frame_copy_calls = 0;
  double capture_gpu_backing_retain_total_ms = 0.0;
  double capture_gpu_backing_retain_max_ms = 0.0;
  double frame_render_total_ms = 0.0;
  double pattern_overlay_total_ms = 0.0;
  double pattern_base_copy_total_ms = 0.0;
  double gpu_update_total_total_ms = 0.0;
  double gpu_upload_copy_total_ms = 0.0;
  double gpu_texture_update_total_ms = 0.0;
  uint64_t catchup_ticks_capped = 0;
  uint64_t catchup_frames_dropped = 0;
  SyntheticCaptureGpuBackingRetainPostureMetricsSnapshot
      capture_gpu_backing_retain_cpu_primary{};
  SyntheticCaptureGpuBackingRetainPostureMetricsSnapshot
      capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar{};
  SyntheticCaptureGpuBackingRetainPostureMetricsSnapshot
      capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar{};
  SyntheticCaptureReadyStagePostureMetricsSnapshot
      capture_ready_stage_cpu_primary{};
  SyntheticCaptureReadyStagePostureMetricsSnapshot
      capture_ready_stage_gpu_primary_no_cpu_sidecar{};
  SyntheticCaptureReadyStagePostureMetricsSnapshot
      capture_ready_stage_gpu_primary_with_cpu_sidecar{};
  std::vector<SyntheticCaptureReadyTimingRecordSnapshot>
      capture_ready_timing_records;
};

struct SyntheticStagedRigTopology {
  std::uint64_t rig_id = 0;
  std::vector<std::string> member_hardware_ids;
};

} // namespace cambang

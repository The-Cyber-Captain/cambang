// src/core/core_stream_registry.h
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <map>
#include <vector>

#include "core/core_frame_sink.h"
#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

// Minimal per-stream core state registry.
//
// Purpose (this build slice):
// - Provide a deterministic place for the core to remember streams exist.
// - Accumulate per-stream counters as commands are dispatched.
// - No frame delivery semantics (always-release remains).
//
// Threading:
// - Core-thread-only. Not atomic. Determinism-first.
class CoreStreamRegistry final {
public:
  enum class StopOrigin : uint8_t {
    None = 0,
    User = 1,
    Provider = 2,
    // Core stopped the stream to let a higher-priority capture proceed
    // (arbitration_policy.md 2, 6.2). Distinct from User because the caller did
    // not ask for it and from Provider because nothing failed -- reporting it
    // as either would misdescribe a working stream that Core took away.
    Preemption = 3,
  };

  struct StreamRecord {
    uint64_t stream_id = 0;

    uint64_t device_instance_id = 0;
    StreamIntent intent = StreamIntent::PREVIEW;
    uint64_t profile_version = 0;
    uint64_t access_posture_epoch = 0;
    CoreRetainedProductionPlan requested_retained_plan{};
    CoreRetainedProductionPlan steady_retained_plan{};
    ProducerBackingCapabilities runtime_backing_capabilities{};
    ProducerBackingCapabilities parent_context_backing_capabilities{};

    CaptureProfile profile{};
    PictureConfig picture{};

    bool created = false;
    bool started = false;
    bool stop_requested_by_core = false;
    // Set alongside stop_requested_by_core when that stop is a preemption, so
    // the stopped fact can be attributed when it arrives.
    bool preemption_requested_by_core = false;
    // Deadline by which frames must resume after Core authorised a provider
    // reprovision of this stream's session. 0 means nothing is expected.
    //
    // Armed only for a reprovision Core itself permitted, never as a general
    // cadence watch: a stream that is merely slow is not this registry's
    // business, and treating it as one would fire on healthy streams.
    uint64_t frame_resume_deadline_ns = 0;
    StopOrigin last_stop_origin = StopOrigin::None;
    uint32_t pending_core_start_facts = 0;
    uint32_t pending_core_stop_facts = 0;

    uint64_t frames_received = 0;
    uint64_t frames_released = 0;

    // Frames released without presentation or retained result acceptance,
    // including Stage C repeating stream-frame coalescing before expensive dispatch.
    uint64_t frames_dropped = 0;
    uint64_t last_frame_ts_ns = 0;

    // REALIZED frame rate, measured from arriving frames -- never the requested
    // rate echoed back. profile.target_fps_min/max is what was ASKED FOR, and
    // until this measurement existed nothing anywhere observed what the sensor
    // actually did, so a request that no provider applied looked identical to
    // one that worked.
    //
    // Measured over a bounded WINDOW OF TIME rather than continuously, and
    // rather than over a fixed frame count. A frame count was tried first and
    // is wrong here: its duration is a function of the very quantity being
    // measured, so thirty frames is a quarter-second at 120fps and six seconds
    // at 5. Since closing a window is what makes a new measurement observable
    // (see kRealizedFpsWindowMs), that would have made the publish schedule
    // scale with frame rate, which is the one thing it must not do.
    //
    // Held in milli-fps so the record carries no floating point.
    //
    // Timestamps are Core's own monotonic ingest marks, NOT sensor acquisition
    // marks -- the frame paths stamp these with steady_clock at integration
    // time. So this measures the rate at which frames REACH CORE, which is the
    // rate that matters to a consumer, but it does include transport and
    // queueing jitter. It is not a sensor-level cadence measurement and must
    // not be described as one.
    //
    // Zero means not yet measured -- a stream that has not completed a window
    // has no honest answer, and must not be given a made-up one.
    uint64_t realized_window_first_ts_ns = 0;
    uint32_t realized_window_frames = 0;
    uint32_t realized_fps_milli = 0;

    uint64_t visibility_frames_presented = 0;
    uint64_t visibility_frames_rejected_unsupported = 0;
    uint64_t visibility_frames_rejected_invalid = 0;
    CoreVisibilityPath visibility_last_path = CoreVisibilityPath::NONE;

    uint32_t last_error_code = 0;
  };

  // Nominal duration of one realized-rate window.
  //
  // This constant governs TWO things, and the second is the reason it is a
  // duration at all. It sets how promptly a wrong rate becomes visible, and --
  // because a closed window is the only event that makes a new realized rate
  // observable -- it also sets the CEILING ON SNAPSHOT PUBLISH FREQUENCY
  // attributable to rate measurement: at most one publish per window per
  // stream. Do not change it without accounting for both.
  //
  // One second: prompt enough to notice a rate fault, and a publish rate that
  // sits far below the tick-bounded ceiling even with many streams running.
  static constexpr uint32_t kRealizedFpsWindowMs = 1000;

  // Frames a window must contain before it may close, regardless of elapsed
  // time. Two is the arithmetic floor -- one interval -- but a rate derived
  // from a single interval is that interval's jitter, not a rate. Four keeps
  // one late frame from dominating the answer.
  //
  // For streams slower than kRealizedFpsWindowMinFrames per
  // kRealizedFpsWindowMs (under 4fps), this stretches the window PAST the
  // nominal duration. That direction is safe: it only ever makes measurement
  // and publication rarer, never more frequent.
  static constexpr uint32_t kRealizedFpsWindowMinFrames = 4;

  CoreStreamRegistry() = default;
  ~CoreStreamRegistry() = default;

  CoreStreamRegistry(const CoreStreamRegistry&) = delete;
  CoreStreamRegistry& operator=(const CoreStreamRegistry&) = delete;

  // Lifecycle
  // declare_stream_effective: called by core surfaces that create streams.
  // Providers must not apply implicit defaults; core stores effective config.
  bool declare_stream_effective(const StreamRequest& effective,
                                CoreRetainedProductionPlan steady_retained_plan = {});
  bool on_stream_created(uint64_t stream_id);
  bool on_stream_destroyed(uint64_t stream_id);
  bool on_core_stream_started(uint64_t stream_id);
  bool on_provider_stream_started(uint64_t stream_id);
  bool on_core_stream_stopped(uint64_t stream_id, uint32_t error_code);
  bool on_provider_stream_stopped(uint64_t stream_id, uint32_t error_code);
  bool mark_stop_requested_by_core(uint64_t stream_id);
  // As above, but records that the stop is a capture preemption rather than the
  // caller's own stop. Must be called before the provider stop_stream, so the
  // attribution is already in place whichever order the facts arrive in.
  bool mark_stop_requested_by_core_for_preemption(uint64_t stream_id);

  // Expect frames to resume on this stream by `deadline_ns`, because Core has
  // just permitted the provider to reprovision the session underneath it.
  bool arm_frame_resumption(uint64_t stream_id, uint64_t deadline_ns);
  // Stream ids whose expectation has passed, cleared as they are returned so a
  // single expiry is reported once. A stream that stopped, started, or
  // delivered a frame in the meantime is not returned: those all disarm it.
  std::vector<uint64_t> take_expired_frame_resumptions(uint64_t now_ns);
  // Delay until the soonest armed expectation falls due, or nothing if none is
  // armed. Core folds this into its next-wake deadline: the timer tick is
  // demand-driven, so an expectation nobody schedules a wake for would only be
  // noticed if something else happened to wake the core thread first.
  std::optional<uint64_t> next_frame_resumption_delay_ns(uint64_t now_ns) const noexcept;

  // What a frame arrival changed. Deliberately NOT convertible to bool: the
  // caller must name the field it means. Normal frame delivery publishes
  // nothing, so a call site that silently ignored realized_window_closed would
  // leave the measured rate correct in this registry and permanently invisible
  // to every consumer -- which is exactly the defect this struct replaced.
  struct FrameReceipt final {
    // The stream id named a live record. False means the frame arrived for a
    // stream this registry does not know.
    bool known = false;
    // This frame closed a realized-rate window, so realized_fps_milli now
    // holds a NEW measurement. Callers must request a snapshot publish.
    bool realized_window_closed = false;
  };

  // Frame accounting (stream must exist).
  FrameReceipt on_frame_received(uint64_t stream_id, uint64_t integrated_ts_ns);
  bool on_frame_released(uint64_t stream_id);
  bool on_frame_dropped(uint64_t stream_id);
  bool on_visibility_path(uint64_t stream_id, CoreVisibilityPath path);

  // Mutable config updates (stream should exist).
  bool set_picture(uint64_t stream_id, const PictureConfig& picture);
  bool set_backing_capabilities(uint64_t stream_id,
                                ProducerBackingCapabilities runtime_backing_capabilities,
                                ProducerBackingCapabilities parent_context_backing_capabilities);
  bool set_requested_retained_plan(uint64_t stream_id,
                                   CoreRetainedProductionPlan requested_retained_plan,
                                   bool bump_access_posture_epoch = true);
  bool set_steady_retained_plan(uint64_t stream_id,
                                CoreRetainedProductionPlan steady_retained_plan);
  bool clear_steady_retained_plan(uint64_t stream_id);

  // Best-effort cleanup for failed creations (core-thread-only).
  bool forget_stream(uint64_t stream_id);

  // Error reporting (stream must exist).
  bool on_stream_error(uint64_t stream_id, uint32_t error_code);

  // Introspection (core-thread-only).
  const StreamRecord* find(uint64_t stream_id) const noexcept;

  // For future snapshot/publisher. Core-thread-only.
  const std::map<uint64_t, StreamRecord>& all() const noexcept { return streams_; }
  bool has_flowing_stream_for_device(uint64_t device_instance_id) const noexcept;
  bool has_error_stream_for_device(uint64_t device_instance_id) const noexcept;

private:
  uint64_t allocate_access_posture_epoch() noexcept;

  std::map<uint64_t, StreamRecord> streams_; // key: stream_id
  std::set<uint64_t> destroyed_stream_tombstones_;
  uint64_t next_access_posture_epoch_ = 1;
};

} // namespace cambang

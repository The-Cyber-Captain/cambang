#pragma once

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/core_runtime.h"
#include "core/synthetic_timeline_request_binding.h"
#include "core/state_snapshot_buffer.h"
#include "core/snapshot/state_snapshot.h"
#include "imaging/api/provider_contract_datatypes.h"
#include "imaging/api/icamera_provider.h"
#include "imaging/stub/provider.h"
#include "imaging/synthetic/provider.h"

namespace cambang {

enum class VerifyCaseProviderKind {
  Synthetic,
  Stub,
};

inline const char* verify_case_provider_name(VerifyCaseProviderKind kind) noexcept {
  switch (kind) {
    case VerifyCaseProviderKind::Synthetic: return "synthetic";
    case VerifyCaseProviderKind::Stub: return "stub";
  }
  return "unknown";
}

struct ObservedSnapshot {
  bool is_nil = true;
  uint64_t gen = 0;
  uint64_t version = 0;
  uint64_t topology_version = 0;
  size_t device_count = 0;
  size_t stream_count = 0;
  std::shared_ptr<const CamBANGStateSnapshot> raw;
};


enum class RealizationTraceFormat : uint8_t {
  Block = 0,
  Csv = 1,
  Both = 2,
};

struct RealizationProfilerOptions {
  bool enabled = false;
  RealizationTraceFormat format = RealizationTraceFormat::Block;
  uint64_t target_device_id = 0;
  uint64_t target_stream_id = 0;
};

class RealizationProfiler final {
public:
  // Profiles first-seen realization milestones at the observation boundary.
  // This intentionally measures what becomes visible per boundary tick.
  // Observable gaps can therefore represent coalescing, not necessarily the absence
  // of intermediate internal core states. snapshot_version/snapshot_topology_version
  // are recorded alongside boundary-visible counters to make that distinction explicit.
  explicit RealizationProfiler(RealizationProfilerOptions options = {}) : options_(options) {}

  bool enabled() const noexcept { return options_.enabled; }

  void reset() {
    observed_publish_ordinal_ = 0;
    generations_.clear();
  }

  void observe(const ObservedSnapshot& observed, uint64_t published_seq_marker) {
    if (!options_.enabled || observed.is_nil || !observed.raw) {
      return;
    }

    GenerationProfile& gen = generations_[observed.gen];
    gen.gen = observed.gen;

    const SnapshotView view = snapshot_view_(observed, published_seq_marker);
    maybe_record_(gen.provider_visible, view, milestone_matches_provider_(view), Stage::Provider, gen);
    maybe_record_(gen.device_identity, view, milestone_matches_device_identity_(view), Stage::DeviceIdentity, gen);
    maybe_record_(gen.device_native, view, milestone_matches_device_native_(view), Stage::DeviceNative, gen);
    maybe_record_(gen.stream_visible, view, milestone_matches_stream_visible_(view), Stage::Stream, gen);
    maybe_record_(gen.frameproducer_visible, view, milestone_matches_frameproducer_visible_(view), Stage::FrameProducer, gen);

    if (gen.provider_visible.seen && gen.provider_publish == kInvalidMarker) {
      gen.provider_publish = gen.provider_visible.observed_publish_ordinal;
      gen.provider_timestamp_ns = gen.provider_visible.timestamp_ns;
    }

    gen.last_observed_publish = view.observed_publish_ordinal;
    gen.last_observed_timestamp_ns = view.timestamp_ns;

    if (gen.frameproducer_visible.seen) {
      gen.completed = true;
    }

    if (!gen.completed && gen.provider_publish != kInvalidMarker) {
      const uint64_t publish_delta = gen.last_observed_publish - gen.provider_publish;
      const uint64_t time_delta = gen.last_observed_timestamp_ns - gen.provider_timestamp_ns;
      if (publish_delta > kDefaultMaxPublishGap || time_delta > kDefaultMaxTimeNs) {
        gen.incomplete = true;
        if (gen.stalled_at == Stage::None) {
          gen.stalled_at = gen.last_stage;
        }
      }
    }

    ++observed_publish_ordinal_;
  }

  void emit_report(FILE* out = stdout) const {
    if (!options_.enabled || !out) {
      return;
    }

    if (options_.format == RealizationTraceFormat::Csv || options_.format == RealizationTraceFormat::Both) {
      std::fputs(
          "gen,stage,observed_publish,observed_version,observed_topology_version,snapshot_version,"
          "snapshot_topology_version,published_seq,t_ns,delta_publish,delta_ns\n",
          out);
      for (const auto& [gen_key, gen] : generations_) {
        (void)gen_key;
        emit_csv_line_(out, gen.gen, "provider_visible", gen.provider_visible, nullptr);
        emit_csv_line_(out, gen.gen, "device_identity", gen.device_identity, &gen.provider_visible);
        emit_csv_line_(out, gen.gen, "device_native", gen.device_native, &gen.device_identity);
        emit_csv_line_(out, gen.gen, "stream_visible", gen.stream_visible, &gen.device_native);
        emit_csv_line_(out, gen.gen, "frameproducer_visible", gen.frameproducer_visible, &gen.stream_visible);
      }
      std::fputs("gen,status,stalled_at,publish_delta,time_delta_ns\n", out);
      for (const auto& [gen_key, gen] : generations_) {
        (void)gen_key;
        emit_csv_status_line_(out, gen);
      }
    }

    if (options_.format == RealizationTraceFormat::Block || options_.format == RealizationTraceFormat::Both) {
      for (const auto& [gen_key, gen] : generations_) {
        (void)gen_key;
        std::fprintf(out,
                     "[realization] gen=%llu note=observable_versions_are_boundary_visible;"
                     " snapshot_versions_are_core_publish_headers; coalesced observable stages may share a publish\n",
                     static_cast<unsigned long long>(gen.gen));
        emit_block_line_(out, "provider_visible", gen.provider_visible, nullptr);
        emit_block_line_(out, "device_identity", gen.device_identity, &gen.provider_visible);
        emit_block_line_(out, "device_native", gen.device_native, &gen.device_identity);
        emit_block_line_(out, "stream_visible", gen.stream_visible, &gen.device_native);
        emit_block_line_(out, "frameproducer_visible", gen.frameproducer_visible, &gen.stream_visible);
        emit_block_status_(out, gen);
      }
    }

    std::fflush(out);
  }

private:
  static constexpr uint64_t kInvalidMarker = std::numeric_limits<uint64_t>::max();
  static constexpr uint64_t kDefaultMaxPublishGap = 8;
  static constexpr uint64_t kDefaultMaxTimeNs = 50'000'000ull;

  enum class Stage : uint8_t {
    None = 0,
    Provider = 1,
    DeviceIdentity = 2,
    DeviceNative = 3,
    Stream = 4,
    FrameProducer = 5,
  };
  struct SnapshotView {
    const CamBANGStateSnapshot* raw = nullptr;
    uint64_t gen = 0;
    uint64_t observed_publish_ordinal = 0;
    uint64_t observed_version = 0;
    uint64_t observed_topology_version = 0;
    uint64_t snapshot_version = 0;
    uint64_t snapshot_topology_version = 0;
    uint64_t published_seq_marker = 0;
    uint64_t timestamp_ns = 0;
  };

  struct MilestoneRecord {
    bool seen = false;
    uint64_t observed_publish_ordinal = 0;
    uint64_t observed_version = 0;
    uint64_t observed_topology_version = 0;
    uint64_t snapshot_version = 0;
    uint64_t snapshot_topology_version = 0;
    uint64_t published_seq_marker = 0;
    uint64_t timestamp_ns = 0;
  };

  struct GenerationProfile {
    uint64_t gen = 0;
    bool completed = false;
    bool incomplete = false;
    uint64_t provider_publish = kInvalidMarker;
    uint64_t last_observed_publish = kInvalidMarker;
    uint64_t provider_timestamp_ns = 0;
    uint64_t last_observed_timestamp_ns = 0;
    Stage last_stage = Stage::None;
    Stage stalled_at = Stage::None;
    MilestoneRecord provider_visible;
    MilestoneRecord device_identity;
    MilestoneRecord device_native;
    MilestoneRecord stream_visible;
    MilestoneRecord frameproducer_visible;
  };

  SnapshotView snapshot_view_(const ObservedSnapshot& observed, uint64_t published_seq_marker) const {
    SnapshotView view;
    view.raw = observed.raw.get();
    view.gen = observed.gen;
    view.observed_publish_ordinal = observed_publish_ordinal_;
    view.observed_version = observed.version;
    view.observed_topology_version = observed.topology_version;
    view.snapshot_version = observed.raw->version;
    view.snapshot_topology_version = observed.raw->topology_version;
    view.published_seq_marker = published_seq_marker;
    view.timestamp_ns = observed.raw->timestamp_ns;
    return view;
  }

  static void maybe_record_(MilestoneRecord& record,
                            const SnapshotView& view,
                            bool matched,
                            Stage stage,
                            GenerationProfile& gen) {
    if (record.seen || !matched) {
      return;
    }
    record.seen = true;
    record.observed_publish_ordinal = view.observed_publish_ordinal;
    record.observed_version = view.observed_version;
    record.observed_topology_version = view.observed_topology_version;
    record.snapshot_version = view.snapshot_version;
    record.snapshot_topology_version = view.snapshot_topology_version;
    record.published_seq_marker = view.published_seq_marker;
    record.timestamp_ns = view.timestamp_ns;
    gen.last_stage = stage;
  }

  static bool native_exists_for_generation_(const CamBANGStateSnapshot& snap,
                                            uint64_t current_gen,
                                            NativeObjectType type,
                                            uint64_t owner_device_instance_id,
                                            uint64_t owner_stream_id) {
    for (const auto& rec : snap.native_objects) {
      if (rec.creation_gen != current_gen || rec.type != static_cast<uint32_t>(type)) {
        continue;
      }
      if (owner_device_instance_id != 0 && rec.owner_device_instance_id != owner_device_instance_id) {
        continue;
      }
      if (owner_stream_id != 0 && rec.owner_stream_id != owner_stream_id) {
        continue;
      }
      return true;
    }
    return false;
  }

  static bool has_device_id_(const CamBANGStateSnapshot& snap, uint64_t device_id) {
    for (const auto& device : snap.devices) {
      if (device.instance_id == device_id) {
        return true;
      }
    }
    return false;
  }

  static bool has_stream_id_(const CamBANGStateSnapshot& snap, uint64_t stream_id) {
    for (const auto& stream : snap.streams) {
      if (stream.stream_id == stream_id) {
        return true;
      }
    }
    return false;
  }

  bool milestone_matches_provider_(const SnapshotView& view) const {
    return native_exists_for_generation_(*view.raw, view.gen, NativeObjectType::Provider, 0, 0);
  }

  bool milestone_matches_device_identity_(const SnapshotView& view) const {
    return options_.target_device_id != 0 && has_device_id_(*view.raw, options_.target_device_id);
  }

  bool milestone_matches_device_native_(const SnapshotView& view) const {
    return options_.target_device_id != 0 &&
           native_exists_for_generation_(*view.raw, view.gen, NativeObjectType::Device, options_.target_device_id, 0);
  }

  bool milestone_matches_stream_visible_(const SnapshotView& view) const {
    return options_.target_stream_id != 0 && has_stream_id_(*view.raw, options_.target_stream_id);
  }

  bool milestone_matches_frameproducer_visible_(const SnapshotView& view) const {
    return options_.target_device_id != 0 && options_.target_stream_id != 0 &&
           native_exists_for_generation_(
               *view.raw, view.gen, NativeObjectType::FrameProducer, options_.target_device_id, options_.target_stream_id);
  }

  static void emit_delta_(FILE* out, const MilestoneRecord& current, const MilestoneRecord* previous) {
    if (!previous || !previous->seen || !current.seen) {
      return;
    }
    std::fprintf(out,
                 " delta_publish=+%llu delta_ns=+%llu",
                 static_cast<unsigned long long>(current.observed_publish_ordinal - previous->observed_publish_ordinal),
                 static_cast<unsigned long long>(current.timestamp_ns - previous->timestamp_ns));
  }

  static void emit_block_line_(FILE* out,
                               const char* stage,
                               const MilestoneRecord& current,
                               const MilestoneRecord* previous) {
    std::fprintf(out, "  %-22s ", stage);
    if (!current.seen) {
      std::fputs("observed=no\n", out);
      return;
    }
    std::fprintf(out,
                 "observed=yes publish=%llu version=%llu topology=%llu snapshot_version=%llu"
                 " snapshot_topology=%llu published_seq=%llu t_ns=%llu",
                 static_cast<unsigned long long>(current.observed_publish_ordinal),
                 static_cast<unsigned long long>(current.observed_version),
                 static_cast<unsigned long long>(current.observed_topology_version),
                 static_cast<unsigned long long>(current.snapshot_version),
                 static_cast<unsigned long long>(current.snapshot_topology_version),
                 static_cast<unsigned long long>(current.published_seq_marker),
                 static_cast<unsigned long long>(current.timestamp_ns));
    emit_delta_(out, current, previous);
    std::fputc('\n', out);
  }

  static void emit_csv_line_(FILE* out,
                             uint64_t gen,
                             const char* stage,
                             const MilestoneRecord& current,
                             const MilestoneRecord* previous) {
    if (!current.seen) {
      std::fprintf(out, "%llu,%s,,,,,,,,,\n", static_cast<unsigned long long>(gen), stage);
      return;
    }

    std::fprintf(out,
                 "%llu,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
                 static_cast<unsigned long long>(gen),
                 stage,
                 static_cast<unsigned long long>(current.observed_publish_ordinal),
                 static_cast<unsigned long long>(current.observed_version),
                 static_cast<unsigned long long>(current.observed_topology_version),
                 static_cast<unsigned long long>(current.snapshot_version),
                 static_cast<unsigned long long>(current.snapshot_topology_version),
                 static_cast<unsigned long long>(current.published_seq_marker),
                 static_cast<unsigned long long>(current.timestamp_ns));

    if (previous && previous->seen) {
      std::fprintf(out,
                   ",%llu,%llu\n",
                   static_cast<unsigned long long>(current.observed_publish_ordinal - previous->observed_publish_ordinal),
                   static_cast<unsigned long long>(current.timestamp_ns - previous->timestamp_ns));
      return;
    }

    std::fputs(",,\n", out);
  }

  static const char* stage_name_(Stage stage) {
    switch (stage) {
      case Stage::None: return "none";
      case Stage::Provider: return "provider_visible";
      case Stage::DeviceIdentity: return "device_identity";
      case Stage::DeviceNative: return "device_native";
      case Stage::Stream: return "stream_visible";
      case Stage::FrameProducer: return "frameproducer_visible";
    }
    return "unknown";
  }

  static const char* final_status_name_(const GenerationProfile& gen) {
    if (gen.completed) {
      return "COMPLETE";
    }
    if (gen.incomplete) {
      return "INCOMPLETE";
    }
    return "PARTIAL";
  }

  static uint64_t final_publish_delta_(const GenerationProfile& gen) {
    if (gen.provider_publish == kInvalidMarker || gen.last_observed_publish == kInvalidMarker) {
      return 0;
    }
    return gen.last_observed_publish - gen.provider_publish;
  }

  static uint64_t final_time_delta_ns_(const GenerationProfile& gen) {
    if (gen.provider_publish == kInvalidMarker) {
      return 0;
    }
    return gen.last_observed_timestamp_ns - gen.provider_timestamp_ns;
  }

  static void emit_block_status_(FILE* out, const GenerationProfile& gen) {
    std::fprintf(out, "  status=%s\n", final_status_name_(gen));
    if (gen.completed) {
      return;
    }
    if (gen.incomplete) {
      std::fprintf(out,
                   "  stalled_at=%s\n"
                   "  publish_delta=+%llu\n"
                   "  time_delta_ns=%llu\n",
                   stage_name_(gen.stalled_at),
                   static_cast<unsigned long long>(final_publish_delta_(gen)),
                   static_cast<unsigned long long>(final_time_delta_ns_(gen)));
      return;
    }
    std::fprintf(out, "  last_stage=%s\n", stage_name_(gen.last_stage));
  }

  static void emit_csv_status_line_(FILE* out, const GenerationProfile& gen) {
    if (gen.completed) {
      std::fprintf(out, "%llu,%s,,,\n", static_cast<unsigned long long>(gen.gen), final_status_name_(gen));
      return;
    }

    const char* stalled_or_last = gen.incomplete ? stage_name_(gen.stalled_at) : stage_name_(gen.last_stage);
    std::fprintf(out,
                 "%llu,%s,%s,%llu,%llu\n",
                 static_cast<unsigned long long>(gen.gen),
                 final_status_name_(gen),
                 stalled_or_last,
                 static_cast<unsigned long long>(final_publish_delta_(gen)),
                 static_cast<unsigned long long>(final_time_delta_ns_(gen)));
  }

  RealizationProfilerOptions options_{};
  uint64_t observed_publish_ordinal_ = 0;
  std::map<uint64_t, GenerationProfile> generations_;
};

class SnapshotExpectation final {
public:
  SnapshotExpectation& is_nil(bool value) {
    is_nil_ = value;
    return *this;
  }
  SnapshotExpectation& gen(uint64_t value) {
    gen_ = value;
    return *this;
  }
  SnapshotExpectation& version(uint64_t value) {
    version_ = value;
    return *this;
  }
  SnapshotExpectation& topology_version(uint64_t value) {
    topology_version_ = value;
    return *this;
  }
  SnapshotExpectation& device_count(size_t value) {
    device_count_ = value;
    return *this;
  }
  SnapshotExpectation& stream_count(size_t value) {
    stream_count_ = value;
    return *this;
  }

  bool matches(const ObservedSnapshot& observed, std::string& error) const {
    std::ostringstream oss;
    bool ok = true;

    auto require = [&](bool cond, const std::string& msg) {
      if (!cond) {
        ok = false;
        if (oss.tellp() > 0) {
          oss << "; ";
        }
        oss << msg;
      }
    };

    if (is_nil_.has_value()) {
      require(observed.is_nil == *is_nil_, std::string("snapshot nil expected=") + (*is_nil_ ? "true" : "false"));
    }
    if (gen_.has_value()) {
      require(!observed.is_nil && observed.gen == *gen_, "gen mismatch");
    }
    if (version_.has_value()) {
      require(!observed.is_nil && observed.version == *version_, "version mismatch");
    }
    if (topology_version_.has_value()) {
      require(!observed.is_nil && observed.topology_version == *topology_version_, "topology_version mismatch");
    }
    if (device_count_.has_value()) {
      require(!observed.is_nil && observed.device_count == *device_count_, "device_count mismatch");
    }
    if (stream_count_.has_value()) {
      require(!observed.is_nil && observed.stream_count == *stream_count_, "stream_count mismatch");
    }

    error = oss.str();
    return ok;
  }

private:
  std::optional<bool> is_nil_;
  std::optional<uint64_t> gen_;
  std::optional<uint64_t> version_;
  std::optional<uint64_t> topology_version_;
  std::optional<size_t> device_count_;
  std::optional<size_t> stream_count_;
};

class ObservationBoundary final {
public:
  void reset(uint64_t current_published_seq) {
    current_ = ObservedSnapshot{};
    has_counters_ = false;
    godot_gen_ = 0;
    godot_version_ = 0;
    godot_topology_version_ = 0;
    last_emitted_topology_sig_ = 0;
    last_seen_published_seq_ = (current_published_seq > 0) ? (current_published_seq - 1) : 0;
  }

  const ObservedSnapshot& current() const noexcept { return current_; }

  bool tick(CoreRuntime& runtime, StateSnapshotBuffer& buffer) {
    const uint64_t published_seq = runtime.published_seq();
    if (published_seq == last_seen_published_seq_) {
      return false;
    }

    auto snap = buffer.snapshot_copy();
    if (!snap) {
      // Do not consume published_seq yet; retry on a later boundary tick so a
      // transient visibility gap cannot drop this publish.
      current_ = ObservedSnapshot{};
      return false;
    }
    last_seen_published_seq_ = published_seq;

    if (!has_counters_ || snap->gen != godot_gen_) {
      has_counters_ = true;
      godot_gen_ = snap->gen;
      godot_version_ = 0;
      godot_topology_version_ = 0;
      last_emitted_topology_sig_ = runtime.published_topology_sig();
    } else {
      ++godot_version_;
      const uint64_t topo_sig = runtime.published_topology_sig();
      if (topo_sig != last_emitted_topology_sig_) {
        last_emitted_topology_sig_ = topo_sig;
        ++godot_topology_version_;
      }
    }

    current_.is_nil = false;
    current_.gen = godot_gen_;
    current_.version = godot_version_;
    current_.topology_version = godot_topology_version_;
    current_.device_count = snap->devices.size();
    current_.stream_count = snap->streams.size();
    current_.raw = std::move(snap);
    return true;
  }

private:
  ObservedSnapshot current_{};
  bool has_counters_ = false;
  uint64_t godot_gen_ = 0;
  uint64_t godot_version_ = 0;
  uint64_t godot_topology_version_ = 0;
  uint64_t last_emitted_topology_sig_ = 0;
  uint64_t last_seen_published_seq_ = 0;
};

class VerifyCaseHarness final {
public:
  static constexpr uint64_t kDeviceId = 100;
  static constexpr uint64_t kStreamId = 200;
  static constexpr uint64_t kRootId = 900;
  static constexpr uint64_t kDeviceBId = 101;
  static constexpr uint64_t kStreamBId = 201;
  static constexpr uint64_t kRootBId = 901;

  explicit VerifyCaseHarness(VerifyCaseProviderKind provider_kind = VerifyCaseProviderKind::Synthetic)
      : provider_kind_(provider_kind) {
    runtime_.set_snapshot_publisher(&snapshot_buffer_);
  }

  ~VerifyCaseHarness() { stop_runtime(); }

  VerifyCaseProviderKind provider_kind() const noexcept { return provider_kind_; }

  void set_realization_profiler(RealizationProfiler* profiler) noexcept { realization_profiler_ = profiler; }

  bool start_runtime(std::string& error) {
    stop_runtime();

    if (!runtime_.start()) {
      error = "runtime start failed";
      return false;
    }
    if (!wait_until([&]() { return runtime_.state_copy() == CoreRuntimeState::LIVE; },
                    error,
                    500,
                    5,
                    "timed out waiting for runtime LIVE")) {
      runtime_.stop();
      return false;
    }

    provider_ = make_provider_();
    if (!provider_) {
      error = "provider construction failed";
      runtime_.stop();
      return false;
    }
    if (auto* synthetic = dynamic_cast<SyntheticProvider*>(provider_.get())) {
      synthetic->set_timeline_request_dispatch_hook_for_host(
          make_synthetic_timeline_request_dispatch_hook(runtime_));
    }
    runtime_.attach_provider(provider_.get());

    if (!provider_->initialize(runtime_.provider_callbacks()).ok()) {
      error = std::string("provider initialize failed (") + verify_case_provider_name(provider_kind_) + ")";
      runtime_.attach_provider(nullptr);
      (void)provider_->shutdown();
      provider_.reset();
      runtime_.stop();
      return false;
    }

    if (!select_endpoints_(error)) {
      runtime_.attach_provider(nullptr);
      (void)provider_->shutdown();
      provider_.reset();
      runtime_.stop();
      return false;
    }

    boundary_.reset(runtime_.published_seq());
    if (realization_profiler_) {
      realization_profiler_->reset();
    }
    return true;
  }

  void stop_runtime() {
    if (provider_) {
      (void)provider_->shutdown();
      runtime_.attach_provider(nullptr);
      provider_.reset();
    }

    const bool was_running = runtime_.is_running();
    if (was_running) {
      runtime_.stop();
      // Mirror Godot stop-boundary behaviour: consume one final publication (if any)
      // before returning to NIL.
      (void)boundary_.tick(runtime_, snapshot_buffer_);
      last_snapshot_before_stop_clear_ = boundary_.current();
    } else {
      last_snapshot_before_stop_clear_ = ObservedSnapshot{};
    }

    snapshot_buffer_.clear();
    endpoint_hardware_ids_.clear();
    synthetic_frame_period_ns_ = 0;
    boundary_.reset(runtime_.published_seq());
    if (realization_profiler_) {
      realization_profiler_->reset();
    }
  }

  bool wait_for_core_publish_count(uint64_t min_count,
                                   std::string& error,
                                   int max_iters = 500,
                                   int sleep_ms = 5) {
    return wait_until([&]() { return runtime_.published_seq() >= min_count; },
                      error,
                      max_iters,
                      sleep_ms,
                      "timed out waiting for core publish count");
  }

  bool wait_for_core_snapshot(std::function<bool(const CamBANGStateSnapshot&)> pred,
                              std::string& error,
                              int max_iters = 500,
                              int sleep_ms = 5,
                              const char* timeout_msg = "timed out waiting for snapshot") {
    return wait_until([&]() {
      auto snap = snapshot_buffer_.snapshot_copy();
      return snap && pred(*snap);
    }, error, max_iters, sleep_ms, timeout_msg);
  }

  bool open_device(std::string& error) { return open_device_id(kDeviceId, 0, kRootId, error); }

  bool open_device_id(uint64_t device_id, size_t endpoint_index, uint64_t root_id, std::string& error) {
    if (!provider_) {
      error = "provider not attached";
      return false;
    }
    if (endpoint_index >= endpoint_hardware_ids_.size()) {
      error = "endpoint index out of range";
      return false;
    }
    runtime_.retain_device_identity(device_id, endpoint_hardware_ids_[endpoint_index]);
    const ProviderResult r = provider_->open_device(endpoint_hardware_ids_[endpoint_index], device_id, root_id);
    if (!r.ok()) {
      error = "open_device failed";
      return false;
    }
    return wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
      return has_device(s, device_id);
    }, error, 500, 5, "timed out waiting for device open publish");
  }

  bool close_device(std::string& error) { return close_device_id(kDeviceId, error); }

  bool close_device_id(uint64_t device_id, std::string& error) {
    if (!provider_) {
      error = "provider not attached";
      return false;
    }
    const ProviderResult r = provider_->close_device(device_id);
    if (!r.ok()) {
      error = "close_device failed";
      return false;
    }
    return wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
      return !has_device(s, device_id);
    }, error, 500, 5, "timed out waiting for device close publish");
  }

  ProviderResult close_device_result(uint64_t device_id) {
    if (!provider_) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    return provider_->close_device(device_id);
  }

  bool create_stream(std::string& error, uint64_t profile_version = 1) {
    return create_stream_id(kStreamId, kDeviceId, profile_version, error);
  }

  bool create_stream_id(uint64_t stream_id, uint64_t device_id, uint64_t profile_version, std::string& error) {
    const uint64_t before = runtime_.published_seq();
    const auto r = runtime_.try_create_stream(stream_id, device_id, StreamIntent::PREVIEW, nullptr, nullptr, profile_version);
    if (r != TryCreateStreamStatus::OK) {
      error = "try_create_stream failed";
      return false;
    }
    runtime_.request_publish();
    if (!wait_for_core_publish_count(before + 1, error, 500, 5)) {
      return false;
    }
    return wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
      return has_stream(s, stream_id);
    }, error, 500, 5, "timed out waiting for stream create publish");
  }

  bool start_stream(std::string& error) { return start_stream_id(kStreamId, error); }

  bool start_stream_id(uint64_t stream_id, std::string& error) {
    const uint64_t before = runtime_.published_seq();
    const auto r = runtime_.try_start_stream(stream_id);
    if (r != TryStartStreamStatus::OK) {
      error = "try_start_stream failed";
      return false;
    }
    runtime_.request_publish();
    if (!wait_for_core_publish_count(before + 1, error, 500, 5)) {
      return false;
    }
    return wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
      const auto* stream = find_stream(s, stream_id);
      return stream && stream->mode == CBStreamMode::FLOWING;
    }, error, 500, 5, "timed out waiting for stream start publish");
  }

  bool stop_stream(std::string& error) { return stop_stream_id(kStreamId, error); }

  bool stop_stream_id(uint64_t stream_id, std::string& error) {
    const uint64_t before = runtime_.published_seq();
    const auto r = runtime_.try_stop_stream(stream_id);
    if (r != TryStopStreamStatus::OK) {
      error = "try_stop_stream failed";
      return false;
    }
    runtime_.request_publish();
    if (!wait_for_core_publish_count(before + 1, error, 500, 5)) {
      return false;
    }
    return wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
      const auto* stream = find_stream(s, stream_id);
      return stream && stream->mode == CBStreamMode::STOPPED;
    }, error, 500, 5, "timed out waiting for stream stop publish");
  }

  bool destroy_stream(std::string& error) { return destroy_stream_id(kStreamId, error); }

  bool destroy_stream_id(uint64_t stream_id, std::string& error) {
    const uint64_t before = runtime_.published_seq();
    const auto r = runtime_.try_destroy_stream(stream_id);
    if (r != TryDestroyStreamStatus::OK) {
      error = "try_destroy_stream failed";
      return false;
    }
    runtime_.request_publish();
    if (!wait_for_core_publish_count(before + 1, error, 500, 5)) {
      return false;
    }
    return wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
      return !has_stream(s, stream_id);
    }, error, 500, 5, "timed out waiting for stream destroy publish");
  }

  bool emit_frame(std::string& error) { return emit_frame_for_stream(kStreamId, error); }

  bool emit_frame_for_stream(uint64_t stream_id, std::string& error) {
    const uint64_t before = runtime_.published_seq();

    switch (provider_kind_) {
      case VerifyCaseProviderKind::Synthetic: {
        auto* synthetic = dynamic_cast<SyntheticProvider*>(provider_.get());
        if (!synthetic) {
          error = "synthetic provider cast failed";
          return false;
        }
        synthetic->advance(synthetic_frame_period_ns_);
        break;
      }
      case VerifyCaseProviderKind::Stub: {
        auto* stub = dynamic_cast<StubProvider*>(provider_.get());
        if (!stub) {
          error = "stub provider cast failed";
          return false;
        }
        stub->emit_test_frames(stream_id, 1);
        break;
      }
    }

    runtime_.request_publish();
    if (!wait_for_core_publish_count(before + 1, error, 500, 5)) {
      return false;
    }
    return wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
      const auto* stream = find_stream(s, stream_id);
      return stream && stream->frames_received >= 1;
    }, error, 500, 5, "timed out waiting for frame publish");
  }

  bool request_publish_only(std::string& error) {
    const uint64_t before = runtime_.published_seq();
    runtime_.request_publish();
    return wait_for_core_publish_count(before + 1, error, 500, 5);
  }

  bool inject_provider_stream_stop(uint64_t stream_id, ProviderError error_code, std::string& error) {
    runtime_.provider_callbacks()->on_stream_stopped(stream_id, error_code);
    return wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
      const auto* stream = find_stream(s, stream_id);
      return stream && stream->mode == CBStreamMode::STOPPED;
    }, error, 500, 5, "timed out waiting for provider stream stop publish");
  }

  bool inject_provider_stream_destroyed(uint64_t stream_id, std::string& error) {
    runtime_.provider_callbacks()->on_stream_destroyed(stream_id);
    return wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
      return !has_stream(s, stream_id);
    }, error, 500, 5, "timed out waiting for provider stream destroy publish");
  }

  bool inject_provider_device_closed(uint64_t device_id, std::string& error) {
    runtime_.provider_callbacks()->on_device_closed(device_id);
    return wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
      return !has_device(s, device_id);
    }, error, 500, 5, "timed out waiting for provider device close publish");
  }

  void inject_provider_stream_error(uint64_t stream_id, ProviderError error_code) {
    runtime_.provider_callbacks()->on_stream_error(stream_id, error_code);
  }

  bool perform_coalesced_burst(std::string& error) {
    if (!open_device(error)) {
      return false;
    }
    if (!create_stream(error)) {
      return false;
    }
    if (!start_stream(error)) {
      return false;
    }
    if (!emit_frame(error)) {
      return false;
    }
    return stop_stream(error);
  }

  bool tick() {
    // Observe at the publication boundary rather than inside core mutation paths so
    // profiling reflects Godot-visible publication/coalescing behavior without changing runtime semantics.
    const bool advanced = boundary_.tick(runtime_, snapshot_buffer_);
    if (advanced && realization_profiler_) {
      realization_profiler_->observe(boundary_.current(), runtime_.published_seq());
    }
    return advanced;
  }
  const ObservedSnapshot& observed() const noexcept { return boundary_.current(); }
  const ObservedSnapshot& last_snapshot_before_stop_clear() const noexcept {
    return last_snapshot_before_stop_clear_;
  }
  CoreRuntime& runtime() noexcept { return runtime_; }
  StateSnapshotBuffer& snapshot_buffer() noexcept { return snapshot_buffer_; }

  static const CamBANGDeviceState* find_device(const CamBANGStateSnapshot& snap, uint64_t device_id) {
    for (const auto& device : snap.devices) {
      if (device.instance_id == device_id) {
        return &device;
      }
    }
    return nullptr;
  }

  static const CamBANGStreamState* find_stream(const CamBANGStateSnapshot& snap, uint64_t stream_id) {
    for (const auto& stream : snap.streams) {
      if (stream.stream_id == stream_id) {
        return &stream;
      }
    }
    return nullptr;
  }

  static bool has_device(const CamBANGStateSnapshot& snap, uint64_t device_id) {
    return find_device(snap, device_id) != nullptr;
  }

  static bool has_stream(const CamBANGStateSnapshot& snap, uint64_t stream_id) {
    return find_stream(snap, stream_id) != nullptr;
  }

private:
  std::unique_ptr<ICameraProvider> make_provider_() {
    if (provider_kind_ == VerifyCaseProviderKind::Stub) {
      return std::make_unique<StubProvider>();
    }

    SyntheticProviderConfig cfg{};
    cfg.synthetic_role = SyntheticRole::Timeline;
    cfg.timing_driver = TimingDriver::VirtualTime;
    cfg.endpoint_count = 2;
    cfg.nominal.width = 320;
    cfg.nominal.height = 180;
    cfg.nominal.fps_num = 30;
    cfg.nominal.fps_den = 1;
    cfg.nominal.start_stream_warmup_ns = 0;
    cfg.pattern.preset = PatternPreset::XyXor;
    cfg.pattern.seed = 1;
    return std::make_unique<SyntheticProvider>(cfg);
  }

  bool select_endpoints_(std::string& error) {
    std::vector<CameraEndpoint> endpoints;
    if (!provider_->enumerate_endpoints(endpoints).ok() || endpoints.empty()) {
      error = "enumerate_endpoints failed";
      return false;
    }

    endpoint_hardware_ids_.clear();
    endpoint_hardware_ids_.reserve(endpoints.size());
    for (const auto& endpoint : endpoints) {
      endpoint_hardware_ids_.push_back(endpoint.hardware_id);
    }

    if (provider_kind_ == VerifyCaseProviderKind::Synthetic) {
      auto* synthetic = dynamic_cast<SyntheticProvider*>(provider_.get());
      if (!synthetic) {
        error = "synthetic provider cast failed";
        return false;
      }
      const StreamTemplate st = synthetic->stream_template();
      const uint32_t fps_num = st.profile.target_fps_max != 0 ? st.profile.target_fps_max : st.profile.target_fps_min;
      if (fps_num == 0) {
        error = "synthetic fps invalid";
        return false;
      }
      synthetic_frame_period_ns_ = 1'000'000'000ull / static_cast<uint64_t>(fps_num);
    }

    return true;
  }

  static bool wait_until(const std::function<bool()>& pred,
                         std::string& error,
                         int max_iters,
                         int sleep_ms,
                         const char* timeout_msg) {
    for (int i = 0; i < max_iters; ++i) {
      if (pred()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
    error = timeout_msg;
    return false;
  }

  VerifyCaseProviderKind provider_kind_ = VerifyCaseProviderKind::Synthetic;
  CoreRuntime runtime_;
  StateSnapshotBuffer snapshot_buffer_;
  std::unique_ptr<ICameraProvider> provider_;
  std::vector<std::string> endpoint_hardware_ids_;
  uint64_t synthetic_frame_period_ns_ = 0;
  ObservationBoundary boundary_;
  ObservedSnapshot last_snapshot_before_stop_clear_{};
  RealizationProfiler* realization_profiler_ = nullptr;
};

} // namespace cambang

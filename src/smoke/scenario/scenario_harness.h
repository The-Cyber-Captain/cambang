#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/core_runtime.h"
#include "core/state_snapshot_buffer.h"
#include "core/snapshot/state_snapshot.h"
#include "imaging/api/provider_contract_datatypes.h"
#include "imaging/api/icamera_provider.h"
#include "imaging/stub/provider.h"
#include "imaging/synthetic/provider.h"

namespace cambang {

enum class ScenarioProviderKind {
  Synthetic,
  Stub,
};

inline const char* scenario_provider_name(ScenarioProviderKind kind) noexcept {
  switch (kind) {
    case ScenarioProviderKind::Synthetic: return "synthetic";
    case ScenarioProviderKind::Stub: return "stub";
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

class ScenarioHarness final {
public:
  static constexpr uint64_t kDeviceId = 100;
  static constexpr uint64_t kStreamId = 200;
  static constexpr uint64_t kRootId = 900;
  static constexpr uint64_t kDeviceBId = 101;
  static constexpr uint64_t kStreamBId = 201;
  static constexpr uint64_t kRootBId = 901;

  explicit ScenarioHarness(ScenarioProviderKind provider_kind = ScenarioProviderKind::Synthetic)
      : provider_kind_(provider_kind) {
    runtime_.set_snapshot_publisher(&snapshot_buffer_);
  }

  ~ScenarioHarness() { stop_runtime(); }

  ScenarioProviderKind provider_kind() const noexcept { return provider_kind_; }

  bool start_runtime(std::string& error) {
    stop_runtime();

    if (!runtime_.start()) {
      error = "runtime start failed";
      return false;
    }

    provider_ = make_provider_();
    if (!provider_) {
      error = "provider construction failed";
      runtime_.stop();
      return false;
    }

    if (!provider_->initialize(runtime_.provider_callbacks()).ok()) {
      error = std::string("provider initialize failed (") + scenario_provider_name(provider_kind_) + ")";
      provider_.reset();
      runtime_.stop();
      return false;
    }

    runtime_.attach_provider(provider_.get());
    if (!select_endpoints_(error)) {
      runtime_.attach_provider(nullptr);
      (void)provider_->shutdown();
      provider_.reset();
      runtime_.stop();
      return false;
    }

    boundary_.reset(runtime_.published_seq());
    return true;
  }

  void stop_runtime() {
    if (provider_) {
      (void)provider_->shutdown();
      runtime_.attach_provider(nullptr);
      provider_.reset();
    }
    if (runtime_.is_running()) {
      runtime_.stop();
    }
    snapshot_buffer_.clear();
    endpoint_hardware_ids_.clear();
    synthetic_frame_period_ns_ = 0;
    boundary_.reset(runtime_.published_seq());
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
      case ScenarioProviderKind::Synthetic: {
        auto* synthetic = dynamic_cast<SyntheticProvider*>(provider_.get());
        if (!synthetic) {
          error = "synthetic provider cast failed";
          return false;
        }
        synthetic->advance(synthetic_frame_period_ns_);
        break;
      }
      case ScenarioProviderKind::Stub: {
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

  bool tick() { return boundary_.tick(runtime_, snapshot_buffer_); }
  const ObservedSnapshot& observed() const noexcept { return boundary_.current(); }
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
    if (provider_kind_ == ScenarioProviderKind::Stub) {
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

    if (provider_kind_ == ScenarioProviderKind::Synthetic) {
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

  ScenarioProviderKind provider_kind_ = ScenarioProviderKind::Synthetic;
  CoreRuntime runtime_;
  StateSnapshotBuffer snapshot_buffer_;
  std::unique_ptr<ICameraProvider> provider_;
  std::vector<std::string> endpoint_hardware_ids_;
  uint64_t synthetic_frame_period_ns_ = 0;
  ObservationBoundary boundary_;
};

} // namespace cambang

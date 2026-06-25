// Deterministic provider compliance verifier: provider contract only.
// This tool intentionally uses provider callbacks, retained snapshots,
// and deterministic timeline dispatch observations as PASS/FAIL evidence.
#include <cstdint>
#include <algorithm>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/core_runtime.h"
#include "imaging/broker/provider_broker.h"
#include "imaging/stub/provider.h"
#include "imaging/synthetic/builtin_scenario_library.h"
#include "imaging/synthetic/gpu_backing_runtime.h"
#include "imaging/synthetic/provider.h"
#include "imaging/synthetic/scenario_loader.h"
#include "imaging/synthetic/scenario_model.h"
#include "smoke/verify_case/verify_case_harness.h"

using namespace cambang;

namespace {

struct Options {
  std::string external_scenario_file;
  std::string only_check;
};

constexpr int kMaxIters = 500;
constexpr int kSleepMs = 5;

constexpr uint64_t kLifecycleDeviceId = 9101;
constexpr uint64_t kLifecycleRootId = 9102;
constexpr uint64_t kLifecycleStreamId = 9103;

constexpr uint64_t kClusteredDeviceId = 121;
constexpr uint64_t kClusteredRootId = 12201;
constexpr uint64_t kClusteredStreamId = 122;

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

void usage(const char* argv0) {
  std::cerr << "Usage: " << argv0
            << " [--external_scenario_file=<path>] [--only_check=<name>]\n";
}

bool parse_opts(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(argv[0]);
      return false;
    }
    if (starts_with(a, "--external_scenario_file=")) {
      opt.external_scenario_file = a.substr(std::string("--external_scenario_file=").size());
      continue;
    }
    if (starts_with(a, "--only_check=")) {
      opt.only_check = a.substr(std::string("--only_check=").size());
      continue;
    }
    std::cerr << "Unknown arg: " << a << "\n";
    usage(argv[0]);
    return false;
  }
  return true;
}

struct EventRec {
  std::string tag;
  uint64_t id = 0;
  uint64_t capture_id = 0;
  uint32_t type = 0;
  uint64_t owner_acquisition_session_id = 0;
  uint64_t owner_stream_id = 0;
  uint32_t pixel_sig = 0;
  uint64_t payload_hash = 0;
  size_t payload_size_bytes = 0;
  uint32_t format_fourcc = 0;
  double sampled_luma = 0.0;
  uint8_t sample_r = 0;
  uint8_t sample_g = 0;
  uint8_t sample_b = 0;
  CaptureImageRouting capture_image_routing = CaptureImageRouting::DEFAULT_METERED;
  uint32_t capture_image_member_index = 0;
  int32_t capture_image_applied_exposure_compensation_milli_ev = 0;
  bool capture_image_has_realized_exposure_compensation_milli_ev = false;
  int32_t capture_image_realized_exposure_compensation_milli_ev = 0;
  CaptureTimestamp ts{};
  ProducerBackingKind primary_backing_kind = ProducerBackingKind::CPU;
  bool has_primary_backing_artifact = false;
  bool retain_cpu_sidecar = true;
};

uint64_t fnv1a64_hash_bytes(const uint8_t* data, size_t size_bytes) {
  constexpr uint64_t kOffset = 1469598103934665603ull;
  constexpr uint64_t kPrime = 1099511628211ull;
  uint64_t h = kOffset;
  for (size_t i = 0; i < size_bytes; ++i) {
    h ^= static_cast<uint64_t>(data[i]);
    h *= kPrime;
  }
  return h;
}

double luma_from_rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (0.2126 * static_cast<double>(r)) +
         (0.7152 * static_cast<double>(g)) +
         (0.0722 * static_cast<double>(b));
}


struct RecorderCallbacks final : IProviderCallbacks {
  uint64_t next_native_id = 1;
  std::vector<EventRec> events;
  std::unordered_map<uint64_t, uint32_t> native_type_by_id;
  std::unordered_map<uint64_t, uint64_t> native_owner_stream_by_id;
  mutable std::mutex mu;

  std::vector<EventRec> snapshot_events() const {
    std::lock_guard<std::mutex> lk(mu);
    return events;
  }

  uint64_t allocate_native_id(NativeObjectType) override { return next_native_id++; }
  uint64_t core_monotonic_now_ns() override { return 0; }
  bool is_stream_display_demand_active(uint64_t) override { return false; }

  void on_device_opened(uint64_t id) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"device_opened", id}); }
  void on_device_closed(uint64_t id) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"device_closed", id}); }
  void on_stream_created(uint64_t id) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"stream_created", id}); }
  void on_stream_destroyed(uint64_t id) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"stream_destroyed", id}); }
  void on_stream_started(uint64_t id) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"stream_started", id}); }
  void on_stream_stopped(uint64_t id, ProviderError) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"stream_stopped", id}); }
  void on_capture_started(uint64_t id, uint64_t) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"capture_started", id}); }
  void on_capture_completed(uint64_t id, uint64_t) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"capture_completed", id}); }
  void on_capture_failed(uint64_t id, uint64_t, ProviderError) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"capture_failed", id}); }

  void on_frame(const FrameView& frame) override {
    EventRec ev{"frame", frame.stream_id};
    ev.capture_id = frame.capture_id;
    ev.format_fourcc = frame.format_fourcc;
    ev.ts = frame.capture_timestamp;
    if (frame.data && frame.size_bytes >= 4) {
      const uint8_t* p = static_cast<const uint8_t*>(frame.data);
      ev.pixel_sig = static_cast<uint32_t>(p[0]) |
                     (static_cast<uint32_t>(p[1]) << 8) |
                     (static_cast<uint32_t>(p[2]) << 16) |
                     (static_cast<uint32_t>(p[3]) << 24);
    }
    if (frame.data && frame.size_bytes > 0) {
      const uint8_t* p = static_cast<const uint8_t*>(frame.data);
      ev.payload_size_bytes = frame.size_bytes;
      ev.payload_hash = fnv1a64_hash_bytes(p, frame.size_bytes);
      const bool valid_shape = (frame.width > 0 && frame.height > 0 && frame.stride_bytes >= frame.width * 4u);
      const bool rgba_like = (frame.format_fourcc == FOURCC_RGBA || frame.format_fourcc == FOURCC_BGRA);
      if (valid_shape && rgba_like) {
        const uint32_t sx = frame.width / 2u;
        const uint32_t sy = frame.height / 2u;
        const size_t off = static_cast<size_t>(sy) * static_cast<size_t>(frame.stride_bytes) + static_cast<size_t>(sx) * 4u;
        if (off + 3 < frame.size_bytes) {
          const uint8_t c0 = p[off + 0];
          const uint8_t c1 = p[off + 1];
          const uint8_t c2 = p[off + 2];
          uint8_t r = c0;
          uint8_t g = c1;
          uint8_t b = c2;
          if (frame.format_fourcc == FOURCC_BGRA) {
            b = c0;
            g = c1;
            r = c2;
          }
          ev.sample_r = r;
          ev.sample_g = g;
          ev.sample_b = b;
          ev.sampled_luma = luma_from_rgb(r, g, b);
        }
      }
    }
    ev.primary_backing_kind = frame.primary_backing_kind;
    ev.has_primary_backing_artifact = static_cast<bool>(frame.primary_backing_artifact);
    ev.retain_cpu_sidecar = frame.retain_cpu_sidecar;
    ev.capture_image_routing = frame.capture_image.routing;
    ev.capture_image_member_index = frame.capture_image.image_member_index;
    ev.capture_image_applied_exposure_compensation_milli_ev =
        frame.capture_image.applied_exposure_compensation_milli_ev;
    ev.capture_image_has_realized_exposure_compensation_milli_ev =
        frame.capture_image.has_realized_exposure_compensation_milli_ev;
    ev.capture_image_realized_exposure_compensation_milli_ev =
        frame.capture_image.realized_exposure_compensation_milli_ev;
    std::lock_guard<std::mutex> lk(mu);
    events.push_back(ev);
    if (frame.release) {
      frame.release(frame.release_user, &frame);
    }
  }

  void on_device_error(uint64_t id, ProviderError) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"device_error", id}); }
  void on_stream_error(uint64_t id, ProviderError) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"stream_error", id}); }
  void on_native_object_created(const NativeObjectCreateInfo& info) override {
    EventRec ev{"native_created", info.native_id};
    ev.type = info.type;
    ev.owner_acquisition_session_id = info.owner_acquisition_session_id;
    ev.owner_stream_id = info.owner_stream_id;
    std::lock_guard<std::mutex> lk(mu);
    native_type_by_id[info.native_id] = info.type;
    native_owner_stream_by_id[info.native_id] = info.owner_stream_id;
    events.push_back(ev);
  }
  void on_native_object_destroyed(const NativeObjectDestroyInfo& info) override {
    std::lock_guard<std::mutex> lk(mu);
    EventRec ev{"native_destroyed", info.native_id};
    auto type_it = native_type_by_id.find(info.native_id);
    if (type_it != native_type_by_id.end()) {
      ev.type = type_it->second;
    }
    auto owner_stream_it = native_owner_stream_by_id.find(info.native_id);
    if (owner_stream_it != native_owner_stream_by_id.end()) {
      ev.owner_stream_id = owner_stream_it->second;
    }
    events.push_back(ev);
  }
};

class BackingPlanEvaluationTestProvider final : public ICameraProvider {
 public:
  const char* provider_name() const override { return "BackingPlanEvaluationTestProvider"; }
  ProviderKind provider_kind() const noexcept override {
    return ProviderKind::synthetic;
  }

  StreamTemplate stream_template() const override {
    StreamTemplate t{};
    t.profile.width = 16;
    t.profile.height = 16;
    t.profile.format_fourcc = FOURCC_RGBA;
    t.profile.target_fps_min = 30;
    t.profile.target_fps_max = 30;
    t.picture.preset = PatternPreset::Checker;
    t.picture.seed = 1;
    return t;
  }

  CaptureTemplate capture_template() const override {
    CaptureTemplate t{};
    t.profile = stream_template().profile;
    t.picture = stream_template().picture;
    return t;
  }

  bool supports_stream_picture_updates() const noexcept override { return true; }
  bool supports_capture_picture_updates() const noexcept override { return true; }
  bool supports_multi_image_still_sequence() const noexcept override {
    return false;
  }
  uint64_t stream_backing_plan_evaluation_settle_delay_ns() const noexcept override {
    std::lock_guard<std::mutex> lk(mu_);
    return backing_plan_evaluation_settle_delay_ns_;
  }
  uint64_t capture_backing_plan_evaluation_settle_delay_ns() const noexcept override {
    std::lock_guard<std::mutex> lk(mu_);
    return backing_plan_evaluation_settle_delay_ns_;
  }

  ProducerBackingCapabilities stream_backing_capabilities(
      const CaptureProfile&,
      const PictureConfig&) const noexcept override {
    std::lock_guard<std::mutex> lk(mu_);
    return stream_caps_;
  }

  ProducerBackingCapabilities capture_backing_capabilities(
      const CaptureRequest&) const noexcept override {
    std::lock_guard<std::mutex> lk(mu_);
    return capture_caps_;
  }

  ProviderResult initialize(IProviderCallbacks* callbacks) override {
    std::lock_guard<std::mutex> lk(mu_);
    callbacks_ = callbacks;
    initialized_ = callbacks != nullptr;
    return initialized_ ? ProviderResult::success()
                        : ProviderResult::failure(
                              ProviderError::ERR_INVALID_ARGUMENT);
  }

  ProviderResult enumerate_endpoints(
      std::vector<CameraEndpoint>& out_endpoints) override {
    out_endpoints.clear();
    out_endpoints.push_back(CameraEndpoint{"backing_plan_eval:0", "Backing Plan Evaluation"});
    return ProviderResult::success();
  }

  ProviderResult open_device(
      const std::string& hardware_id,
      uint64_t device_instance_id,
      uint64_t root_id) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (!initialized_ || hardware_id != "backing_plan_eval:0" || device_instance_id == 0 ||
        root_id == 0) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    devices_[device_instance_id].open = true;
    devices_[device_instance_id].root_id = root_id;
    if (callbacks_) {
      callbacks_->on_device_opened(device_instance_id);
    }
    return ProviderResult::success();
  }

  ProviderResult close_device(uint64_t device_instance_id) override {
    uint64_t primed_acquisition_session_id = 0;
    std::lock_guard<std::mutex> lk(mu_);
    auto device_it = devices_.find(device_instance_id);
    if (device_it != devices_.end()) {
      primed_acquisition_session_id =
          device_it->second.primed_acquisition_session_id;
      devices_.erase(device_it);
    }
    for (auto it = pending_captures_.begin(); it != pending_captures_.end();) {
      if (it->second.device_instance_id != device_instance_id) {
        ++it;
        continue;
      }
      it = pending_captures_.erase(it);
    }
    if (callbacks_ && primed_acquisition_session_id != 0) {
      NativeObjectDestroyInfo info{};
      info.native_id = primed_acquisition_session_id;
      callbacks_->on_native_object_destroyed(info);
    }
    if (callbacks_) {
      callbacks_->on_device_closed(device_instance_id);
    }
    return ProviderResult::success();
  }

  ProviderResult create_stream(const StreamRequest& req) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (!initialized_ || req.stream_id == 0 || req.device_instance_id == 0) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    StreamState& state = streams_[req.stream_id];
    state.req = req;
    state.picture = req.picture;
    state.created = true;
    if (callbacks_) {
      callbacks_->on_stream_created(req.stream_id);
    }
    return ProviderResult::success();
  }

  ProviderResult destroy_stream(uint64_t stream_id) override {
    std::lock_guard<std::mutex> lk(mu_);
    streams_.erase(stream_id);
    if (callbacks_) {
      callbacks_->on_stream_destroyed(stream_id);
    }
    return ProviderResult::success();
  }

  ProviderResult start_stream(
      uint64_t stream_id,
      const CaptureProfile& profile,
      const PictureConfig& picture) override {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second.created) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    it->second.req.profile = profile;
    it->second.picture = picture;
    it->second.started = true;
    if (callbacks_) {
      callbacks_->on_stream_started(stream_id);
    }
    return ProviderResult::success();
  }

  ProviderResult stop_stream(uint64_t stream_id) override {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second.created) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    it->second.started = false;
    if (callbacks_) {
      callbacks_->on_stream_stopped(stream_id, ProviderError::OK);
    }
    return ProviderResult::success();
  }

  ProviderResult update_stream_retained_production_plan(
      uint64_t stream_id,
      CoreRetainedProductionPlan requested_retained_plan) override {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second.created ||
        !requested_retained_plan.valid ||
        !stream_caps_.viable(requested_retained_plan.posture)) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    it->second.req.requested_retained_plan = requested_retained_plan;
    ++it->second.plan_updates;
    return ProviderResult::success();
  }

  ProviderResult set_stream_picture_config(
      uint64_t stream_id,
      const PictureConfig& picture) override {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || !it->second.created) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    it->second.picture = picture;
    return ProviderResult::success();
  }

  ProviderResult set_capture_picture_config(
      uint64_t device_instance_id,
      const PictureConfig& picture) override {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = devices_.find(device_instance_id);
    if (it == devices_.end() || !it->second.open) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    it->second.picture = picture;
    return ProviderResult::success();
  }

  ProviderResult sync_capture_parent_priming(const CaptureRequest& req) override {
    uint64_t root_id = 0;
    uint64_t acquisition_session_id = 0;
    bool newly_created = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = devices_.find(req.device_instance_id);
      if (!initialized_ || req.device_instance_id == 0 ||
          it == devices_.end() || !it->second.open) {
        return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
      }
      ++it->second.priming_sync_count;
      root_id = it->second.root_id;
      acquisition_session_id = it->second.primed_acquisition_session_id;
      if (acquisition_session_id == 0 && callbacks_) {
        acquisition_session_id = callbacks_->allocate_native_id(
            NativeObjectType::AcquisitionSession);
        it->second.primed_acquisition_session_id = acquisition_session_id;
        newly_created = true;
      }
    }
    if (callbacks_ && newly_created && acquisition_session_id != 0) {
      NativeObjectCreateInfo info{};
      info.native_id = acquisition_session_id;
      info.type = static_cast<uint32_t>(NativeObjectType::AcquisitionSession);
      info.root_id = root_id;
      info.owner_device_instance_id = req.device_instance_id;
      callbacks_->on_native_object_created(info);
    }
    return acquisition_session_id != 0
        ? ProviderResult::success()
        : ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  ProviderResult release_capture_parent_priming(uint64_t device_instance_id) override {
    uint64_t acquisition_session_id = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = devices_.find(device_instance_id);
      if (!initialized_ || device_instance_id == 0 ||
          it == devices_.end() || !it->second.open) {
        return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
      }
      ++it->second.priming_release_count;
      acquisition_session_id = it->second.primed_acquisition_session_id;
      it->second.primed_acquisition_session_id = 0;
    }
    if (callbacks_ && acquisition_session_id != 0) {
      NativeObjectDestroyInfo info{};
      info.native_id = acquisition_session_id;
      callbacks_->on_native_object_destroyed(info);
    }
    return ProviderResult::success();
  }

  ProviderResult trigger_capture(const CaptureRequest& req) override {
    uint64_t root_id = 0;
    uint64_t acquisition_session_id = 0;
    bool destroy_acquisition_session_on_emit = false;
    FrameView frame{};
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = devices_.find(req.device_instance_id);
      if (it == devices_.end() || !it->second.open) {
        return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
      }
      if (pending_captures_.find(req.capture_id) != pending_captures_.end()) {
        return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
      }
      root_id = it->second.root_id;
      acquisition_session_id = it->second.primed_acquisition_session_id;
      if (acquisition_session_id == 0 && callbacks_) {
        acquisition_session_id = callbacks_->allocate_native_id(
            NativeObjectType::AcquisitionSession);
        destroy_acquisition_session_on_emit = true;
      }
      frame = build_capture_frame_(req, true, acquisition_session_id);
      pending_captures_[req.capture_id] = PendingCapture{
          frame,
          req.device_instance_id,
          acquisition_session_id,
          destroy_acquisition_session_on_emit};
    }
    if (callbacks_) {
      if (destroy_acquisition_session_on_emit && acquisition_session_id != 0) {
        NativeObjectCreateInfo info{};
        info.native_id = acquisition_session_id;
        info.type = static_cast<uint32_t>(NativeObjectType::AcquisitionSession);
        info.root_id = root_id;
        info.owner_device_instance_id = req.device_instance_id;
        callbacks_->on_native_object_created(info);
      }
      callbacks_->on_capture_started(req.capture_id, req.device_instance_id);
    }
    return ProviderResult::success();
  }

  ProviderResult abort_capture(uint64_t) override {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }

  ProviderResult apply_camera_spec_patch(
      const std::string&,
      uint64_t,
      SpecPatchView) override {
    return ProviderResult::success();
  }

  ProviderResult apply_imaging_spec_patch(
      uint64_t,
      SpecPatchView) override {
    return ProviderResult::success();
  }

  ProviderResult shutdown() override {
    std::lock_guard<std::mutex> lk(mu_);
    if (callbacks_) {
      for (const auto& [device_instance_id, device] : devices_) {
        (void)device_instance_id;
        if (device.primed_acquisition_session_id == 0) {
          continue;
        }
        NativeObjectDestroyInfo info{};
        info.native_id = device.primed_acquisition_session_id;
        callbacks_->on_native_object_destroyed(info);
      }
    }
    streams_.clear();
    devices_.clear();
    pending_captures_.clear();
    return ProviderResult::success();
  }

  void set_capture_cpu_available(bool available) {
    std::lock_guard<std::mutex> lk(mu_);
    capture_caps_.cpu_backed_available = available;
  }

  void set_stream_capabilities(ProducerBackingCapabilities caps) {
    std::lock_guard<std::mutex> lk(mu_);
    stream_caps_ = caps;
  }

  void set_capture_capabilities(ProducerBackingCapabilities caps) {
    std::lock_guard<std::mutex> lk(mu_);
    capture_caps_ = caps;
  }

  void set_backing_plan_evaluation_settle_delay_ns(uint64_t settle_delay_ns) {
    std::lock_guard<std::mutex> lk(mu_);
    backing_plan_evaluation_settle_delay_ns_ = settle_delay_ns;
  }

  bool emit_stream_frame(uint64_t stream_id, bool gpu_materialization_available) {
    StreamRequest request_copy{};
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = streams_.find(stream_id);
      if (it == streams_.end() || !it->second.started) {
        return false;
      }
      request_copy = it->second.req;
    }
    if (!callbacks_) {
      return false;
    }
    callbacks_->on_frame(
        build_stream_frame_(request_copy, gpu_materialization_available));
    return true;
  }

  CoreRetainedProductionPlan stream_requested_plan(uint64_t stream_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = streams_.find(stream_id);
    return it == streams_.end() ? CoreRetainedProductionPlan{}
                                : it->second.req.requested_retained_plan;
  }

  uint64_t stream_plan_update_count(uint64_t stream_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = streams_.find(stream_id);
    return it == streams_.end() ? 0ull : it->second.plan_updates;
  }

  bool has_active_capture_parent_priming(uint64_t device_instance_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = devices_.find(device_instance_id);
    return it != devices_.end() && it->second.primed_acquisition_session_id != 0;
  }

  uint64_t active_capture_parent_session_id(uint64_t device_instance_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = devices_.find(device_instance_id);
    return it == devices_.end() ? 0ull : it->second.primed_acquisition_session_id;
  }

  uint64_t capture_parent_priming_sync_count(uint64_t device_instance_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = devices_.find(device_instance_id);
    return it == devices_.end() ? 0ull : it->second.priming_sync_count;
  }

  uint64_t capture_parent_priming_release_count(uint64_t device_instance_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = devices_.find(device_instance_id);
    return it == devices_.end() ? 0ull : it->second.priming_release_count;
  }

  size_t active_capture_parent_priming_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t count = 0;
    for (const auto& [device_instance_id, device] : devices_) {
      (void)device_instance_id;
      if (device.primed_acquisition_session_id != 0) {
        ++count;
      }
    }
    return count;
  }

  bool replace_primed_acquisition_session(uint64_t device_instance_id) {
    uint64_t root_id = 0;
    uint64_t old_acquisition_session_id = 0;
    uint64_t new_acquisition_session_id = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = devices_.find(device_instance_id);
      if (it == devices_.end() || !it->second.open || !callbacks_) {
        return false;
      }
      root_id = it->second.root_id;
      old_acquisition_session_id = it->second.primed_acquisition_session_id;
      new_acquisition_session_id = callbacks_->allocate_native_id(
          NativeObjectType::AcquisitionSession);
      if (new_acquisition_session_id == 0) {
        return false;
      }
      it->second.primed_acquisition_session_id = new_acquisition_session_id;
    }
    if (!callbacks_) {
      return false;
    }
    if (old_acquisition_session_id != 0) {
      NativeObjectDestroyInfo destroy_info{};
      destroy_info.native_id = old_acquisition_session_id;
      callbacks_->on_native_object_destroyed(destroy_info);
    }
    NativeObjectCreateInfo create_info{};
    create_info.native_id = new_acquisition_session_id;
    create_info.type = static_cast<uint32_t>(NativeObjectType::AcquisitionSession);
    create_info.root_id = root_id;
    create_info.owner_device_instance_id = device_instance_id;
    callbacks_->on_native_object_created(create_info);
    return true;
  }

  bool emit_pending_capture(uint64_t capture_id) {
    PendingCapture pending{};
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = pending_captures_.find(capture_id);
      if (it == pending_captures_.end()) {
        return false;
      }
      pending = it->second;
      pending_captures_.erase(it);
    }
    if (!callbacks_) {
      return false;
    }
    callbacks_->on_frame(pending.frame);
    callbacks_->on_capture_completed(capture_id, pending.device_instance_id);
    if (pending.destroy_acquisition_session_on_emit &&
        pending.acquisition_session_id != 0) {
      NativeObjectDestroyInfo info{};
      info.native_id = pending.acquisition_session_id;
      callbacks_->on_native_object_destroyed(info);
    }
    return true;
  }

private:
  struct DeviceState {
    bool open = false;
    uint64_t root_id = 0;
    PictureConfig picture{};
    uint64_t primed_acquisition_session_id = 0;
    uint64_t priming_sync_count = 0;
    uint64_t priming_release_count = 0;
  };

  struct PendingCapture {
    FrameView frame{};
    uint64_t device_instance_id = 0;
    uint64_t acquisition_session_id = 0;
    bool destroy_acquisition_session_on_emit = false;
  };

  struct StreamState {
    StreamRequest req{};
    bool created = false;
    bool started = false;
    PictureConfig picture{};
    uint64_t plan_updates = 0;
  };

  static std::shared_ptr<std::vector<uint8_t>> make_bytes_() {
    return std::make_shared<std::vector<uint8_t>>(16u * 16u * 4u, 7u);
  }

  static FrameView build_frame_from_plan_(
      uint64_t device_instance_id,
      uint64_t stream_id,
      uint64_t capture_id,
      uint64_t acquisition_session_id,
      CoreRetainedProductionPlan requested_retained_plan,
      bool gpu_materialization_available) {
    FrameView frame{};
    frame.device_instance_id = device_instance_id;
    frame.stream_id = stream_id;
    frame.capture_id = capture_id;
    frame.acquisition_session_id = acquisition_session_id;
    frame.width = 16;
    frame.height = 16;
    frame.format_fourcc = FOURCC_RGBA;
    frame.capture_timestamp.value =
        capture_id != 0 ? capture_id : (stream_id != 0 ? stream_id : 1);
    frame.capture_timestamp.tick_ns = 1;
    frame.capture_timestamp.domain = CaptureTimestampDomain::PROVIDER_MONOTONIC;
    frame.requested_retained_plan = requested_retained_plan;
    frame.retain_cpu_sidecar = requested_retained_plan.retain_cpu_sidecar();
    if (requested_retained_plan.primary_cpu()) {
      const auto bytes = make_bytes_();
      frame.primary_backing_kind = ProducerBackingKind::CPU;
      frame.data = bytes->data();
      frame.size_bytes = bytes->size();
      frame.stride_bytes = 16u * 4u;
      frame.cpu_payload_owner = bytes;
      return frame;
    }

    frame.primary_backing_kind = ProducerBackingKind::GPU;
    frame.primary_backing_artifact = std::make_shared<int>(1);
    frame.retained_gpu_backing_descriptor.valid = true;
    frame.retained_gpu_backing_descriptor.stream_id = stream_id;
    frame.retained_gpu_backing_descriptor.backing_id =
        capture_id != 0 ? capture_id : stream_id;
    frame.retained_gpu_backing_descriptor.capture_timestamp_ns =
        frame.capture_timestamp.value;
    frame.retained_gpu_backing_descriptor.width = 16;
    frame.retained_gpu_backing_descriptor.height = 16;
    frame.retained_gpu_backing_descriptor.stride_bytes = 16u * 4u;
    frame.retained_gpu_backing_descriptor.format_fourcc = FOURCC_RGBA;
    frame.retained_gpu_backing_descriptor.display_available = true;
    frame.retained_gpu_backing_descriptor.materialization_available =
        gpu_materialization_available;
    frame.retained_gpu_backing_descriptor.materialization_requires_gpu_readback =
        false;
    if (requested_retained_plan.retain_cpu_sidecar()) {
      const auto bytes = make_bytes_();
      frame.data = bytes->data();
      frame.size_bytes = bytes->size();
      frame.stride_bytes = 16u * 4u;
      frame.cpu_payload_owner = bytes;
    }
    return frame;
  }

  static FrameView build_stream_frame_(
      const StreamRequest& req,
      bool gpu_materialization_available) {
    return build_frame_from_plan_(
        req.device_instance_id,
        req.stream_id,
        0,
        0,
        req.requested_retained_plan,
        gpu_materialization_available);
  }

  static FrameView build_capture_frame_(
      const CaptureRequest& req,
      bool gpu_materialization_available,
      uint64_t acquisition_session_id) {
    return build_frame_from_plan_(
        req.device_instance_id,
        0,
        req.capture_id,
        acquisition_session_id,
        req.requested_retained_plan,
        gpu_materialization_available);
  }

  mutable std::mutex mu_;
  IProviderCallbacks* callbacks_ = nullptr;
  bool initialized_ = false;
  ProducerBackingCapabilities stream_caps_{true, true, true};
  ProducerBackingCapabilities capture_caps_{true, true, true};
  uint64_t backing_plan_evaluation_settle_delay_ns_ = 0;
  std::map<uint64_t, DeviceState> devices_;
  std::map<uint64_t, PendingCapture> pending_captures_;
  std::map<uint64_t, StreamState> streams_;
};


bool wait_for_core_runtime_live(CoreRuntime& rt) {
  for (int i = 0; i < kMaxIters; ++i) {
    if (rt.state_copy() == CoreRuntimeState::LIVE) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
  }
  return false;
}

CaptureRequest make_direct_provider_default_still_capture_request(uint64_t capture_id,
                                                                 uint64_t device_instance_id,
                                                                 uint32_t width,
                                                                 uint32_t height,
                                                                 uint32_t format_fourcc) {
  CaptureRequest req{};
  req.capture_id = capture_id;
  req.device_instance_id = device_instance_id;
  req.width = width;
  req.height = height;
  req.format_fourcc = format_fourcc;
  req.still_image_bundle = make_default_metered_still_image_bundle();
  return req;
}

void append_additional_bracket_members(CaptureStillImageBundle& bundle,
                                       const std::vector<int32_t>& exposure_compensation_milli_evs) {
  for (const int32_t ev : exposure_compensation_milli_evs) {
    bundle.members.push_back(CaptureStillImageMember{
        static_cast<uint32_t>(bundle.members.size()),
        CaptureStillImageMemberRole::ADDITIONAL_BRACKET,
        ev});
  }
}

CaptureRequest make_direct_provider_multi_member_still_capture_request(
    uint64_t capture_id,
    uint64_t device_instance_id,
    uint32_t width,
    uint32_t height,
    uint32_t format_fourcc,
    const std::vector<int32_t>& additional_exposure_compensation_milli_evs) {
  CaptureRequest req = make_direct_provider_default_still_capture_request(
      capture_id, device_instance_id, width, height, format_fourcc);
  append_additional_bracket_members(req.still_image_bundle, additional_exposure_compensation_milli_evs);
  return req;
}

bool wait_for_stream_frame(const RecorderCallbacks& cb, uint64_t stream_id, EventRec& out) {
  for (int i = 0; i < kMaxIters; ++i) {
    const auto events = cb.snapshot_events();
    for (const auto& ev : events) {
      if (ev.tag == "frame" && ev.id == stream_id && ev.capture_id == 0) {
        out = ev;
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
  }
  return false;
}

bool wait_for_capture_completed_with_frames(const RecorderCallbacks& cb,
                                            uint64_t capture_id,
                                            size_t expected_frame_count) {
  for (int i = 0; i < kMaxIters; ++i) {
    bool completed = false;
    size_t frame_count = 0;
    const auto events = cb.snapshot_events();
    for (const auto& ev : events) {
      if (ev.tag == "capture_failed" && ev.id == capture_id) {
        return false;
      }
      if (ev.tag == "capture_completed" && ev.id == capture_id) {
        completed = true;
      }
      if (ev.tag == "frame" && ev.capture_id == capture_id) {
        ++frame_count;
      }
    }
    if (completed && frame_count >= expected_frame_count) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
  }
  return false;
}

int find_event_index(const std::vector<EventRec>& events, const char* tag, uint64_t id) {
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].tag == tag && events[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

int find_native_create_id(const std::vector<EventRec>& events, uint32_t type, uint64_t owner_stream_id) {
  for (const auto& e : events) {
    if (e.tag == "native_created" && e.type == type && e.owner_stream_id == owner_stream_id) {
      return static_cast<int>(e.id);
    }
  }
  return -1;
}

int find_native_create_id_with_owners(const std::vector<EventRec>& events,
                                      uint32_t type,
                                      uint64_t owner_acquisition_session_id,
                                      uint64_t owner_stream_id) {
  for (const auto& e : events) {
    if (e.tag == "native_created" &&
        e.type == type &&
        e.owner_acquisition_session_id == owner_acquisition_session_id &&
        e.owner_stream_id == owner_stream_id) {
      return static_cast<int>(e.id);
    }
  }
  return -1;
}

int find_native_create_id_by_type(const std::vector<EventRec>& events, uint32_t type) {
  for (const auto& e : events) {
    if (e.tag == "native_created" && e.type == type) {
      return static_cast<int>(e.id);
    }
  }
  return -1;
}

int find_frame_index_by_ts(const std::vector<EventRec>& events, uint64_t ts_ns) {
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].tag == "frame" && events[i].ts.value == ts_ns) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int count_events_by_tag_and_type(const std::vector<EventRec>& events, const char* tag, uint32_t type) {
  int count = 0;
  for (const auto& e : events) {
    if (e.tag == tag && e.type == type) {
      ++count;
    }
  }
  return count;
}

int count_events_by_tag_type_and_owner_stream(const std::vector<EventRec>& events,
                                              const char* tag,
                                              uint32_t type,
                                              uint64_t owner_stream_id) {
  int count = 0;
  for (const auto& e : events) {
    if (e.tag == tag && e.type == type && e.owner_stream_id == owner_stream_id) {
      ++count;
    }
  }
  return count;
}

bool assert_native_balance(const std::vector<EventRec>& events, const char* name) {
  int created = 0;
  int destroyed = 0;
  for (const auto& e : events) {
    if (e.tag == "native_created") ++created;
    if (e.tag == "native_destroyed") ++destroyed;
  }
  if (created != destroyed) {
    std::cerr << "FAIL " << name << " native create/destroy mismatch\n";
    return false;
  }
  return true;
}

bool wait_for_snapshot_with_progress(VerifyCaseHarness& harness,
                                     uint64_t advance_step_ns,
                                     uint64_t min_published_seq,
                                     const std::function<bool(const CamBANGStateSnapshot&)>& predicate,
                                     std::string& error,
                                     const char* timeout_msg,
                                     int max_iters = kMaxIters,
                                     int sleep_ms = kSleepMs) {
  for (int i = 0; i < max_iters; ++i) {
    (void)harness.advance_synthetic_timeline_for_host(advance_step_ns);
    harness.runtime().request_publish();
    if (harness.runtime().published_seq() < min_published_seq) {
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
      continue;
    }
    auto snap = harness.snapshot_buffer().snapshot_copy();
    if (snap && predicate(*snap)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  error = timeout_msg;
  return false;
}

SyntheticTimelineScenario build_clustered_destructive_scenario(uint64_t period_ns) {
  SyntheticTimelineScenario scenario{};
  SyntheticScheduledEvent ev{};

  ev.at_ns = 0;
  ev.type = SyntheticEventType::OpenDevice;
  ev.endpoint_index = 0;
  ev.device_instance_id = kClusteredDeviceId;
  ev.root_id = kClusteredRootId;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::CreateStream;
  ev.device_instance_id = kClusteredDeviceId;
  ev.stream_id = kClusteredStreamId;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::StartStream;
  ev.stream_id = kClusteredStreamId;
  scenario.events.push_back(ev);

  // Shared clustered destructive boundary.
  ev = {};
  ev.at_ns = period_ns * 2;
  ev.type = SyntheticEventType::StopStream;
  ev.stream_id = kClusteredStreamId;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = period_ns * 2;
  ev.type = SyntheticEventType::DestroyStream;
  ev.stream_id = kClusteredStreamId;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = period_ns * 2;
  ev.type = SyntheticEventType::CloseDevice;
  ev.device_instance_id = kClusteredDeviceId;
  scenario.events.push_back(ev);

  return scenario;
}

// ===== Family A: Scenario materialization / loader compliance =====


bool run_provider_access_preflight_check() {
  const ProviderAccessStatus synthetic_access =
      ProviderBroker::check_mode_access_readiness(RuntimeMode::synthetic);
  if (!synthetic_access.ok() || synthetic_access.code != ProviderAccessCode::Ready) {
    std::cerr << "synthetic provider access/readiness preflight was not Ready\n";
    return false;
  }

  const ProviderAccessStatus platform_access =
      ProviderBroker::check_mode_access_readiness(RuntimeMode::platform_backed);
  if (!platform_access.ok() || platform_access.code != ProviderAccessCode::Ready) {
    std::cerr << "stub platform-backed access/readiness preflight was not Ready\n";
    return false;
  }

  const ProviderResult platform_support =
      ProviderBroker::check_mode_supported_in_build(RuntimeMode::platform_backed);
  if (!platform_support.ok()) {
    std::cerr << "platform_backed build support unexpectedly changed while checking access readiness\n";
    return false;
  }

  return true;
}

bool run_synthetic_scenario_materialization_check() {
  SyntheticCanonicalScenario canonical{};

  SyntheticScenarioDeviceDeclaration d{};
  d.key = "cam_a";
  d.endpoint_index = 0;
  canonical.devices.push_back(d);

  SyntheticScenarioStreamDeclaration s{};
  s.key = "preview_a";
  s.device_key = "cam_a";
  s.intent = StreamIntent::PREVIEW;
  s.baseline_capture_profile.width = 64;
  s.baseline_capture_profile.height = 64;
  s.baseline_capture_profile.format_fourcc = FOURCC_RGBA;
  s.baseline_capture_profile.target_fps_min = 30;
  s.baseline_capture_profile.target_fps_max = 30;
  canonical.streams.push_back(s);

  SyntheticScenarioTimelineAction a{};
  a.at_ns = 10;
  a.type = SyntheticEventType::StartStream;
  a.stream_key = "preview_a";
  canonical.timeline.push_back(a);

  a = {};
  a.at_ns = 0;
  a.type = SyntheticEventType::OpenDevice;
  a.device_key = "cam_a";
  canonical.timeline.push_back(a);

  a = {};
  a.at_ns = 0;
  a.type = SyntheticEventType::CreateStream;
  a.stream_key = "preview_a";
  canonical.timeline.push_back(a);

  a = {};
  a.at_ns = 20;
  a.type = SyntheticEventType::StopStream;
  a.stream_key = "preview_a";
  canonical.timeline.push_back(a);

  SyntheticScenarioMaterializationOptions opts{};
  opts.device_instance_id_base = 200;
  opts.root_id_base = 400;
  opts.stream_id_base = 600;

  SyntheticScenarioMaterializationResult result{};
  std::string error;
  if (!materialize_synthetic_canonical_scenario(canonical, opts, result, &error)) {
    std::cerr << "FAIL scenario materialization rejected valid canonical scenario: " << error << "\n";
    return false;
  }

  if (result.devices.size() != 1 || result.streams.size() != 1 || result.executable_schedule.events.size() != 4) {
    std::cerr << "FAIL scenario materialization unexpected output sizes\n";
    return false;
  }
  if (result.devices[0].device_instance_id != 201 || result.devices[0].root_id != 401 ||
      result.streams[0].stream_id != 601 || result.streams[0].device_instance_id != 201) {
    std::cerr << "FAIL scenario materialization unstable id mapping\n";
    return false;
  }

  const auto& events = result.executable_schedule.events;
  const std::vector<SyntheticEventType> expected{
      SyntheticEventType::OpenDevice,
      SyntheticEventType::CreateStream,
      SyntheticEventType::StartStream,
      SyntheticEventType::StopStream,
  };
  for (size_t i = 0; i < expected.size(); ++i) {
    if (events[i].type != expected[i]) {
      std::cerr << "FAIL scenario materialization did not stable-order by at_ns\n";
      return false;
    }
  }

  return true;
}

bool run_synthetic_builtin_scenario_library_build_check() {
  CaptureProfile baseline{};
  baseline.width = 64;
  baseline.height = 64;
  baseline.format_fourcc = FOURCC_RGBA;
  baseline.target_fps_min = 30;
  baseline.target_fps_max = 30;

  const SyntheticBuiltinScenarioLibraryId ids[] = {
      SyntheticBuiltinScenarioLibraryId::StreamLifecycleVersions,
      SyntheticBuiltinScenarioLibraryId::TopologyChangeVersions,
      SyntheticBuiltinScenarioLibraryId::PublicationCoalescing,
  };

  for (SyntheticBuiltinScenarioLibraryId id : ids) {
    SyntheticCanonicalScenario canonical{};
    std::string error;
    if (!build_synthetic_builtin_scenario_library_canonical_scenario(id, baseline, canonical, &error)) {
      std::cerr << "FAIL builtin scenario library build failed for "
                << synthetic_builtin_scenario_library_name(id) << ": " << error << "\n";
      return false;
    }
    if (canonical.devices.empty() || canonical.streams.empty() || canonical.timeline.empty()) {
      std::cerr << "FAIL builtin scenario library produced empty scenario for "
                << synthetic_builtin_scenario_library_name(id) << "\n";
      return false;
    }
  }

  return true;
}

bool run_synthetic_external_scenario_loader_check() {
  const std::string json = R"JSON(
{
  "schema_version": 1,
  "devices": [
    { "key": "builtin_device", "endpoint_index": 0 }
  ],
  "streams": [
    {
      "key": "builtin_main_stream",
      "device_key": "builtin_device",
      "intent": "PREVIEW",
      "capture_profile": {
        "width": 64,
        "height": 64,
        "format_fourcc": 1094862674,
        "target_fps_min": 30,
        "target_fps_max": 30
      }
    }
  ],
  "timeline": [
    { "at_ns": 0, "type": "OpenDevice", "device_key": "builtin_device" },
    { "at_ns": 0, "type": "CreateStream", "stream_key": "builtin_main_stream" },
    { "at_ns": 0, "type": "StartStream", "stream_key": "builtin_main_stream" },
    {
      "at_ns": 15000000,
      "type": "UpdateStreamPicture",
      "stream_key": "builtin_main_stream",
      "picture": {
        "preset": "checker",
        "seed": 3,
        "overlay_frame_index_offsets": false,
        "overlay_moving_bar": true,
        "solid_r": 0,
        "solid_g": 0,
        "solid_b": 0,
        "solid_a": 255,
        "checker_size_px": 12
      }
    },
    { "at_ns": 60000000, "type": "StopStream", "stream_key": "builtin_main_stream" },
    { "at_ns": 60000001, "type": "DestroyStream", "stream_key": "builtin_main_stream" },
    { "at_ns": 60000002, "type": "CloseDevice", "device_key": "builtin_device" }
  ]
}
)JSON";

  SyntheticCanonicalScenario loaded{};
  std::string error;
  if (!load_synthetic_canonical_scenario_from_json_text(json, loaded, &error)) {
    std::cerr << "FAIL external loader parse/validate/convert failed: " << error << "\n";
    return false;
  }

  SyntheticScenarioMaterializationResult materialized{};
  if (!materialize_synthetic_canonical_scenario(loaded, {}, materialized, &error)) {
    std::cerr << "FAIL external loader materialization failed: " << error << "\n";
    return false;
  }

  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.synthetic_role = SyntheticRole::Timeline;
  cfg.timing_driver = TimingDriver::VirtualTime;
  cfg.endpoint_count = 1;
  cfg.nominal.fps_num = 30;
  cfg.nominal.fps_den = 1;

  SyntheticProvider synthetic(cfg);
  if (!synthetic.initialize(&cb).ok()) return false;

  std::vector<SyntheticEventType> dispatched;
  std::mutex dispatched_mu;
  synthetic.set_timeline_request_dispatch_hook_for_host([&dispatched, &dispatched_mu](const SyntheticScheduledEvent& ev) {
    std::lock_guard<std::mutex> lk(dispatched_mu);
    dispatched.push_back(ev.type);
  });

  if (!synthetic.load_timeline_canonical_scenario_from_json_text_for_host(json, &error).ok()) {
    std::cerr << "FAIL external loader provider-facing load failed: " << error << "\n";
    (void)synthetic.shutdown();
    return false;
  }
  if (!synthetic.start_timeline_scenario_for_host().ok()) {
    (void)synthetic.shutdown();
    return false;
  }

  synthetic.advance(60'000'002);
  const std::vector<SyntheticEventType> expected{
      SyntheticEventType::OpenDevice,
      SyntheticEventType::CreateStream,
      SyntheticEventType::StartStream,
      SyntheticEventType::UpdateStreamPicture,
      SyntheticEventType::StopStream,
      SyntheticEventType::DestroyStream,
      SyntheticEventType::CloseDevice,
  };
  if (dispatched != expected) {
    std::cerr << "FAIL external loader authored/materialized/dispatched mismatch\n";
    (void)synthetic.shutdown();
    return false;
  }

  return synthetic.shutdown().ok();
}

bool run_synthetic_external_scenario_loader_negative_check() {
  struct NegativeCase {
    const char* name;
    const char* json;
    const char* error_hint;
  };

  const std::vector<NegativeCase> cases{
      {"unknown_top_level_field", R"JSON({"schema_version":1,"devices":[],"streams":[],"timeline":[],"extra":1})JSON", "unknown field"},
      {"missing_required_top_level_field", R"JSON({"schema_version":1,"devices":[],"streams":[]})JSON", "missing required field"},
      {"wrong_type_required_field", R"JSON({"schema_version":"1","devices":[],"streams":[],"timeline":[]})JSON", "wrong type"},
      {"unknown_action_type", R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[{"at_ns":0,"type":"DoThing","stream_key":"stream0"}]})JSON", "type is unknown"},
      {"emit_frame_rejected", R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[{"at_ns":0,"type":"EmitFrame","stream_key":"stream0"}]})JSON", "EmitFrame"},
      {"stream_device_key_unknown", R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"missing_cam","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[]})JSON", "unknown device key"},
      {"timeline_unknown_stream_key", R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[{"at_ns":0,"type":"StartStream","stream_key":"missing_stream"}]})JSON", "unknown stream key"},
      {"schema_version_not_one", R"JSON({"schema_version":2,"devices":[],"streams":[],"timeline":[]})JSON", "schema_version"},
  };

  for (const auto& c : cases) {
    SyntheticCanonicalScenario out{};
    std::string error;
    if (load_synthetic_canonical_scenario_from_json_text(c.json, out, &error)) {
      std::cerr << "FAIL external loader negative unexpectedly succeeded: " << c.name << "\n";
      return false;
    }
    if (error.find(c.error_hint) == std::string::npos) {
      std::cerr << "FAIL external loader negative mismatch: " << c.name
                << " error=" << error << " expected_hint=" << c.error_hint << "\n";
      return false;
    }
  }

  return true;
}

bool run_external_scenario_file_execution_check(const std::string& path) {
  SyntheticCanonicalScenario canonical{};
  std::string error;
  if (!load_synthetic_canonical_scenario_from_json_file(path, canonical, &error)) {
    std::cerr << "FAIL external scenario file pre-load failed: " << error << "\n";
    return false;
  }

  SyntheticScenarioMaterializationResult materialized{};
  if (!materialize_synthetic_canonical_scenario(canonical, {}, materialized, &error)) {
    std::cerr << "FAIL external scenario file materialization failed: " << error << "\n";
    return false;
  }

  std::vector<SyntheticEventType> expected_dispatch;
  uint64_t max_at_ns = 0;
  expected_dispatch.reserve(materialized.executable_schedule.events.size());
  for (const auto& ev : materialized.executable_schedule.events) {
    if (ev.at_ns > max_at_ns) max_at_ns = ev.at_ns;
    if (ev.type != SyntheticEventType::EmitFrame) {
      expected_dispatch.push_back(ev.type);
    }
  }

  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.synthetic_role = SyntheticRole::Timeline;
  cfg.timing_driver = TimingDriver::VirtualTime;
  cfg.endpoint_count = 1;
  cfg.nominal.fps_num = 30;
  cfg.nominal.fps_den = 1;

  SyntheticProvider synthetic(cfg);
  if (!synthetic.initialize(&cb).ok()) return false;

  std::vector<SyntheticEventType> dispatched;
  std::mutex dispatched_mu;
  synthetic.set_timeline_request_dispatch_hook_for_host([&dispatched, &dispatched_mu](const SyntheticScheduledEvent& ev) {
    std::lock_guard<std::mutex> lk(dispatched_mu);
    dispatched.push_back(ev.type);
  });

  if (!synthetic.load_timeline_canonical_scenario_from_json_file_for_host(path, &error).ok()) {
    std::cerr << "FAIL external scenario file provider load+stage failed: " << error << "\n";
    (void)synthetic.shutdown();
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(dispatched_mu);
    if (!dispatched.empty()) {
      std::cerr << "FAIL external scenario file dispatched before explicit start\n";
      (void)synthetic.shutdown();
      return false;
    }
  }

  if (!synthetic.start_timeline_scenario_for_host().ok()) {
    (void)synthetic.shutdown();
    return false;
  }
  synthetic.advance(max_at_ns + 1);

  std::vector<SyntheticEventType> dispatched_snapshot;
  {
    std::lock_guard<std::mutex> lk(dispatched_mu);
    dispatched_snapshot = dispatched;
  }
  if (dispatched_snapshot != expected_dispatch) {
    std::cerr << "FAIL external scenario file dispatch/order mismatch\n";
    synthetic.set_timeline_request_dispatch_hook_for_host({});
    (void)synthetic.shutdown();
    return false;
  }
  synthetic.set_timeline_request_dispatch_hook_for_host({});
  return synthetic.shutdown().ok();
}

// ===== Family B: Primitive lifecycle compliance (foundational) =====

bool run_synthetic_primitive_lifecycle_foundation_check() {
  VerifyCaseHarness harness(VerifyCaseProviderKind::Synthetic);
  harness.set_callback_diagnostics_enabled(true);

  std::string error;
  if (!harness.start_runtime(error)) {
    std::cerr << "FAIL primitive lifecycle harness start: " << error << "\n";
    return false;
  }

  if (!harness.open_device_id(kLifecycleDeviceId, 0, kLifecycleRootId, error) ||
      !harness.create_stream_id(kLifecycleStreamId, kLifecycleDeviceId, 1, error) ||
      !harness.start_stream_id(kLifecycleStreamId, error) ||
      !harness.stop_stream_id(kLifecycleStreamId, error) ||
      !harness.destroy_stream_id(kLifecycleStreamId, error) ||
      !harness.close_device_id(kLifecycleDeviceId, error)) {
    std::cerr << "FAIL primitive lifecycle action failed: " << error << "\n";
    harness.stop_runtime();
    return false;
  }

  if (!harness.wait_for_core_snapshot(
          [&](const CamBANGStateSnapshot& s) {
            return !VerifyCaseHarness::has_stream(s, kLifecycleStreamId) &&
                   !VerifyCaseHarness::has_device(s, kLifecycleDeviceId);
          },
          error,
          kMaxIters,
          kSleepMs,
          "timed out waiting for primitive lifecycle final absence")) {
    std::cerr << "FAIL primitive lifecycle final truth failed: " << error << "\n";
    harness.stop_runtime();
    return false;
  }

  const int opened = harness.find_recorded_callback_index("device_opened", kLifecycleDeviceId);
  const int created = harness.find_recorded_callback_index("stream_created", kLifecycleStreamId);
  const int started = harness.find_recorded_callback_index("stream_started", kLifecycleStreamId);
  const int stopped = harness.find_recorded_callback_index("stream_stopped", kLifecycleStreamId);
  const int destroyed = harness.find_recorded_callback_index("stream_destroyed", kLifecycleStreamId);
  const int closed = harness.find_recorded_callback_index("device_closed", kLifecycleDeviceId);

  if (opened < 0 || created < 0 || started < 0 || stopped < 0 || destroyed < 0 || closed < 0) {
    std::cerr << "FAIL primitive lifecycle missing callback evidence\n";
    harness.stop_runtime();
    return false;
  }
  if (!(opened < created && created < started && started < stopped && stopped < destroyed && destroyed < closed)) {
    std::cerr << "FAIL primitive lifecycle callback order mismatch\n";
    harness.stop_runtime();
    return false;
  }

  harness.stop_runtime();
  return true;
}

// ===== Family C: Clustered destructive sequencing interpretation =====

bool run_clustered_strict_branch_check() {
  VerifyCaseHarness harness(VerifyCaseProviderKind::Synthetic);
  std::string error;
  if (!harness.start_runtime(error)) {
    std::cerr << "FAIL clustered strict harness start: " << error << "\n";
    return false;
  }

  auto* synthetic = dynamic_cast<SyntheticProvider*>(harness.runtime().attached_provider());
  if (!synthetic) {
    std::cerr << "FAIL clustered strict provider cast failed\n";
    harness.stop_runtime();
    return false;
  }

  const uint64_t period_ns = 1'000'000'000ull / 30ull;
  if (!synthetic->set_timeline_reconciliation_for_host(TimelineReconciliation::Strict).ok() ||
      !synthetic->set_timeline_scenario_for_host(build_clustered_destructive_scenario(period_ns)).ok() ||
      !synthetic->start_timeline_scenario_for_host().ok()) {
    std::cerr << "FAIL clustered strict setup failed\n";
    harness.stop_runtime();
    return false;
  }

  if (!wait_for_snapshot_with_progress(
          harness,
          0,
          harness.runtime().published_seq(),
          [&](const CamBANGStateSnapshot& s) {
            const auto* stream = VerifyCaseHarness::find_stream(s, kClusteredStreamId);
            return VerifyCaseHarness::has_device(s, kClusteredDeviceId) &&
                   stream && stream->mode == CBStreamMode::FLOWING;
          },
          error,
          "timed out waiting for clustered strict precondition")) {
    std::cerr << "FAIL clustered strict precondition failed: " << error << "\n";
    harness.stop_runtime();
    return false;
  }

  const uint64_t post_boundary_min_publish = harness.runtime().published_seq() + 1;
  (void)harness.advance_synthetic_timeline_for_host(period_ns * 2);
  harness.runtime().request_publish();

  if (!wait_for_snapshot_with_progress(
          harness,
          1,
          post_boundary_min_publish,
          [&](const CamBANGStateSnapshot& s) {
            const auto* stream = VerifyCaseHarness::find_stream(s, kClusteredStreamId);
            return !stream || stream->mode != CBStreamMode::FLOWING;
          },
          error,
          "timed out waiting for clustered strict post-boundary convergence")) {
    std::cerr << "FAIL clustered strict post-boundary convergence failed: " << error << "\n";
    harness.stop_runtime();
    return false;
  }

  auto snap = harness.snapshot_buffer().snapshot_copy();
  if (!snap) {
    std::cerr << "FAIL clustered strict missing converged post-boundary snapshot\n";
    harness.stop_runtime();
    return false;
  }

  // Strict interpretation: after clustered boundary, truth must be self-consistent.
  const bool has_stream = VerifyCaseHarness::has_stream(*snap, kClusteredStreamId);
  const bool has_device = VerifyCaseHarness::has_device(*snap, kClusteredDeviceId);
  if (has_stream) {
    const auto* stream = VerifyCaseHarness::find_stream(*snap, kClusteredStreamId);
    if (!stream || stream->mode == CBStreamMode::FLOWING) {
      std::cerr << "FAIL clustered strict impossible retained stream state\n";
      harness.stop_runtime();
      return false;
    }
    if (!has_device) {
      std::cerr << "FAIL clustered strict impossible retained topology (stream without device)\n";
      harness.stop_runtime();
      return false;
    }
  }

  harness.stop_runtime();
  return true;
}

bool run_clustered_completion_gated_branch_check() {
  VerifyCaseHarness harness(VerifyCaseProviderKind::Synthetic);
  harness.set_callback_diagnostics_enabled(true);
  std::string error;
  if (!harness.start_runtime(error)) {
    std::cerr << "FAIL clustered gated harness start: " << error << "\n";
    return false;
  }
  auto* synthetic = dynamic_cast<SyntheticProvider*>(harness.runtime().attached_provider());
  if (!synthetic) {
    std::cerr << "FAIL clustered gated provider cast failed\n";
    harness.stop_runtime();
    return false;
  }

  std::vector<SyntheticEventType> dispatched;
  harness.set_synthetic_timeline_event_observer(
      [&dispatched](const SyntheticScheduledEvent& ev) { dispatched.push_back(ev.type); });

  const uint64_t period_ns = 1'000'000'000ull / 30ull;
  if (!synthetic->set_timeline_reconciliation_for_host(TimelineReconciliation::CompletionGated).ok() ||
      !synthetic->set_timeline_scenario_for_host(build_clustered_destructive_scenario(period_ns)).ok() ||
      !synthetic->start_timeline_scenario_for_host().ok()) {
    std::cerr << "FAIL clustered gated setup failed\n";
    harness.stop_runtime();
    return false;
  }

  (void)harness.advance_synthetic_timeline_for_host(0);
  const int start_dispatch = static_cast<int>(
      std::find(dispatched.begin(), dispatched.end(), SyntheticEventType::StartStream) - dispatched.begin());
  if (start_dispatch >= static_cast<int>(dispatched.size())) {
    std::cerr << "FAIL clustered gated precondition dispatch evidence missing\n";
    harness.stop_runtime();
    return false;
  }

  int opened = -1;
  int created = -1;
  int started = -1;
  for (int i = 0; i < kMaxIters; ++i) {
    (void)harness.advance_synthetic_timeline_for_host(1);
    harness.runtime().request_publish();
    opened = harness.find_recorded_callback_index("device_opened", kClusteredDeviceId);
    created = harness.find_recorded_callback_index("stream_created", kClusteredStreamId);
    started = harness.find_recorded_callback_index("stream_started", kClusteredStreamId);
    if (opened >= 0 && created >= 0 && started >= 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
  }

  if (opened < 0 || created < 0 || started < 0) {
    std::cerr << "DIAG clustered gated startup precondition timeout expected stream_id=" << kClusteredStreamId
              << " device_id=" << kClusteredDeviceId
              << " callback_indices=(opened=" << opened
              << ", created=" << created
              << ", started=" << started << ")\n";
    std::cerr << "FAIL clustered gated startup precondition not realized\n";
    harness.stop_runtime();
    return false;
  }
  if (!(opened < created && created < started)) {
    std::cerr << "FAIL clustered gated startup callback order mismatch\n";
    harness.stop_runtime();
    return false;
  }

  harness.clear_recorded_callbacks();
  (void)harness.advance_synthetic_timeline_for_host(period_ns * 2);
  harness.runtime().request_publish();

  int stopped = -1;
  int destroyed = -1;
  int closed = -1;
  constexpr int kStageMaxIters = kMaxIters * 2;
  constexpr int kStageDrainIters = 50;
  auto wait_for_stage = [&](const char* tag,
                            uint64_t id,
                            const char* stage_name,
                            int& out_index) -> bool {
    for (int i = 0; i < kStageMaxIters; ++i) {
      (void)harness.advance_synthetic_timeline_for_host(1);
      harness.runtime().request_publish();
      stopped = harness.find_recorded_callback_index("stream_stopped", kClusteredStreamId);
      destroyed = harness.find_recorded_callback_index("stream_destroyed", kClusteredStreamId);
      closed = harness.find_recorded_callback_index("device_closed", kClusteredDeviceId);
      out_index = harness.find_recorded_callback_index(tag, id);
      if (out_index >= 0) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
    }

    for (int i = 0; i < kStageDrainIters; ++i) {
      (void)harness.advance_synthetic_timeline_for_host(1);
      harness.runtime().request_publish();
      out_index = harness.find_recorded_callback_index(tag, id);
      if (out_index >= 0) {
        stopped = harness.find_recorded_callback_index("stream_stopped", kClusteredStreamId);
        destroyed = harness.find_recorded_callback_index("stream_destroyed", kClusteredStreamId);
        closed = harness.find_recorded_callback_index("device_closed", kClusteredDeviceId);
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
    }

    stopped = harness.find_recorded_callback_index("stream_stopped", kClusteredStreamId);
    destroyed = harness.find_recorded_callback_index("stream_destroyed", kClusteredStreamId);
    closed = harness.find_recorded_callback_index("device_closed", kClusteredDeviceId);
    std::cerr << "DIAG clustered gated callback stage timeout stage=" << stage_name
              << " expected stream_id=" << kClusteredStreamId
              << " device_id=" << kClusteredDeviceId
              << " callback_indices=(stopped=" << stopped
              << ", destroyed=" << destroyed
              << ", closed=" << closed << ")\n";
    std::cerr << "FAIL clustered gated missing callback evidence at stage " << stage_name << "\n";
    return false;
  };

  if (!wait_for_stage("stream_stopped", kClusteredStreamId, "stream_stopped", stopped)) {
    harness.stop_runtime();
    return false;
  }
  if (!wait_for_stage("stream_destroyed", kClusteredStreamId, "stream_destroyed", destroyed)) {
    harness.stop_runtime();
    return false;
  }
  if (!wait_for_stage("device_closed", kClusteredDeviceId, "device_closed", closed)) {
    harness.stop_runtime();
    return false;
  }

  if (!(stopped < destroyed && destroyed < closed)) {
    std::cerr << "FAIL clustered gated callback order mismatch\n";
    harness.stop_runtime();
    return false;
  }

  const int stop_dispatch = static_cast<int>(
      std::find(dispatched.begin(), dispatched.end(), SyntheticEventType::StopStream) - dispatched.begin());
  const int destroy_dispatch = static_cast<int>(
      std::find(dispatched.begin(), dispatched.end(), SyntheticEventType::DestroyStream) - dispatched.begin());
  const int close_dispatch = static_cast<int>(
      std::find(dispatched.begin(), dispatched.end(), SyntheticEventType::CloseDevice) - dispatched.begin());
  if (stop_dispatch >= static_cast<int>(dispatched.size()) ||
      destroy_dispatch >= static_cast<int>(dispatched.size()) ||
      close_dispatch >= static_cast<int>(dispatched.size())) {
    std::cerr << "FAIL clustered gated clustered-boundary dispatch evidence missing\n";
    harness.stop_runtime();
    return false;
  }

  harness.stop_runtime();
  return true;
}

// ===== Family D: Broker / host surface compliance =====

bool run_broker_timeline_host_surface_check() {
  if (!ProviderBroker::check_mode_supported_in_build(RuntimeMode::synthetic).ok()) {
    std::cout << "SKIP broker timeline host surface: synthetic mode not built\n";
    return true;
  }

  RecorderCallbacks cb;
  ProviderBroker broker;
  if (!broker.set_runtime_mode_requested(RuntimeMode::synthetic).ok() ||
      !broker.set_synthetic_role_requested(SyntheticRole::Timeline).ok() ||
      !broker.set_synthetic_timing_driver_requested(TimingDriver::VirtualTime).ok() ||
      !broker.initialize(&cb).ok()) {
    std::cerr << "FAIL broker timeline host surface setup failed\n";
    return false;
  }

  std::vector<SyntheticScheduledEvent> dispatched;
  broker.set_synthetic_timeline_request_dispatch_hook([&](const SyntheticScheduledEvent& ev) {
    dispatched.push_back(ev);
  });

  if (!broker.select_timeline_builtin_scenario_for_host("stream_lifecycle_versions").ok()) {
    std::cerr << "FAIL broker timeline host surface builtin selection failed\n";
    (void)broker.shutdown();
    return false;
  }

  if (!broker.advance_timeline_for_host(0).ok() || !dispatched.empty()) {
    std::cerr << "FAIL broker timeline host surface staged scenario dispatched before start\n";
    (void)broker.shutdown();
    return false;
  }

  if (!broker.start_timeline_scenario_for_host().ok() ||
      !broker.set_timeline_reconciliation_for_host(TimelineReconciliation::CompletionGated).ok() ||
      !broker.set_timeline_reconciliation_for_host(TimelineReconciliation::Strict).ok() ||
      !broker.advance_timeline_for_host(60'000'002).ok()) {
    std::cerr << "FAIL broker timeline host surface run failed\n";
    (void)broker.shutdown();
    return false;
  }

  bool saw_open = false;
  bool saw_create = false;
  bool saw_start = false;
  bool saw_stop = false;
  bool saw_destroy = false;
  bool saw_close = false;
  for (const auto& ev : dispatched) {
    if (!saw_open && ev.type == SyntheticEventType::OpenDevice) saw_open = true;
    if (saw_open && !saw_create && ev.type == SyntheticEventType::CreateStream) saw_create = true;
    if (saw_create && !saw_start && ev.type == SyntheticEventType::StartStream) saw_start = true;
    if (saw_start && !saw_stop && ev.type == SyntheticEventType::StopStream) saw_stop = true;
    if (saw_stop && !saw_destroy && ev.type == SyntheticEventType::DestroyStream) saw_destroy = true;
    if (saw_destroy && !saw_close && ev.type == SyntheticEventType::CloseDevice) saw_close = true;
  }
  if (!(saw_open && saw_create && saw_start && saw_stop && saw_destroy && saw_close)) {
    std::cerr << "FAIL broker timeline host surface missing expected dispatch progression\n";
    (void)broker.shutdown();
    return false;
  }

  const std::string valid_json = R"JSON(
{
  "schema_version": 1,
  "devices": [{ "key": "cam0", "endpoint_index": 0 }],
  "streams": [{
    "key": "stream0",
    "device_key": "cam0",
    "intent": "PREVIEW",
    "capture_profile": {
      "width": 64,
      "height": 64,
      "format_fourcc": 1094862674,
      "target_fps_min": 30,
      "target_fps_max": 30
    }
  }],
  "timeline": [
    { "at_ns": 0, "type": "OpenDevice", "device_key": "cam0" },
    { "at_ns": 0, "type": "CreateStream", "stream_key": "stream0" },
    { "at_ns": 0, "type": "StartStream", "stream_key": "stream0" }
  ]
}
)JSON";

  dispatched.clear();
  if (!broker.load_timeline_canonical_scenario_from_json_text_for_host(valid_json, nullptr).ok() ||
      !broker.advance_timeline_for_host(0).ok() || !dispatched.empty()) {
    std::cerr << "FAIL broker timeline host surface external stage-only behavior changed\n";
    (void)broker.shutdown();
    return false;
  }

  std::string load_error;
  if (broker.load_timeline_canonical_scenario_from_json_text_for_host("{\"schema_version\":\"bad\"}", &load_error).ok()) {
    std::cerr << "FAIL broker timeline host surface strict loader accepted invalid json\n";
    (void)broker.shutdown();
    return false;
  }

  if (!broker.shutdown().ok()) {
    std::cerr << "FAIL broker timeline host surface shutdown failed\n";
    return false;
  }

  return true;
}


bool run_synthetic_backing_capability_truth_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;

  SyntheticProvider provider(cfg);
  if (!provider.initialize(&cb).ok()) {
    std::cerr << "FAIL synthetic backing capability truth check initialize failed\n";
    return false;
  }

  CaptureProfile profile{};
  PictureConfig picture{};
  CaptureRequest req{};

  const ProducerBackingCapabilities stream_caps = provider.stream_backing_capabilities(profile, picture);
  const ProducerBackingCapabilities capture_caps = provider.capture_backing_capabilities(req);

  if (!provider.shutdown().ok()) {
    std::cerr << "FAIL synthetic backing capability truth check shutdown failed\n";
    return false;
  }

  const bool runtime_gpu_gate = synthetic_gpu_backing_runtime_available();
  const bool stream_ok = stream_caps.cpu_backed_available &&
                         (stream_caps.gpu_backed_available == runtime_gpu_gate) &&
                         (stream_caps.gpu_with_cpu_sidecar_available == runtime_gpu_gate) &&
                         stream_caps.viable(CoreProductionPostureShape::CpuPrimary) &&
                         (stream_caps.viable(CoreProductionPostureShape::GpuPrimaryNoCpuSidecar) == runtime_gpu_gate) &&
                         (stream_caps.viable(CoreProductionPostureShape::GpuPrimaryWithCpuSidecar) == runtime_gpu_gate);
  const bool capture_ok = capture_caps.cpu_backed_available &&
                          (capture_caps.gpu_backed_available == runtime_gpu_gate) &&
                          (capture_caps.gpu_with_cpu_sidecar_available == runtime_gpu_gate) &&
                          capture_caps.viable(CoreProductionPostureShape::CpuPrimary) &&
                          (capture_caps.viable(CoreProductionPostureShape::GpuPrimaryNoCpuSidecar) == runtime_gpu_gate) &&
                          (capture_caps.viable(CoreProductionPostureShape::GpuPrimaryWithCpuSidecar) == runtime_gpu_gate);
  if (!stream_ok || !capture_ok) {
    std::cerr << "FAIL synthetic backing capability truth mismatch\n";
    return false;
  }

  auto verify_mode_truth = [&](SyntheticProducerOutputFormMode mode,
                               bool expect_cpu,
                               bool expect_gpu,
                               const char* label) -> bool {
    SyntheticProviderConfig mode_cfg{};
    mode_cfg.endpoint_count = 1;
    mode_cfg.producer_output_form_mode = mode;
    RecorderCallbacks mode_cb;
    SyntheticProvider mode_provider(mode_cfg);
    if (!mode_provider.initialize(&mode_cb).ok()) {
      std::cerr << "FAIL synthetic backing capability truth " << label << " initialize failed\n";
      return false;
    }
    const ProducerBackingCapabilities mode_stream_caps =
        mode_provider.stream_backing_capabilities(profile, picture);
    const ProducerBackingCapabilities mode_capture_caps =
        mode_provider.capture_backing_capabilities(req);
    if (!mode_provider.shutdown().ok()) {
      std::cerr << "FAIL synthetic backing capability truth " << label << " shutdown failed\n";
      return false;
    }
    const bool expect_sidecar = expect_cpu && expect_gpu;
    if (mode_stream_caps.cpu_backed_available != expect_cpu ||
        mode_stream_caps.gpu_backed_available != expect_gpu ||
        mode_stream_caps.gpu_with_cpu_sidecar_available != expect_sidecar ||
        mode_stream_caps.viable(CoreProductionPostureShape::CpuPrimary) != expect_cpu ||
        mode_stream_caps.viable(CoreProductionPostureShape::GpuPrimaryNoCpuSidecar) != expect_gpu ||
        mode_stream_caps.viable(CoreProductionPostureShape::GpuPrimaryWithCpuSidecar) != expect_sidecar ||
        mode_capture_caps.cpu_backed_available != expect_cpu ||
        mode_capture_caps.gpu_backed_available != expect_gpu ||
        mode_capture_caps.gpu_with_cpu_sidecar_available != expect_sidecar ||
        mode_capture_caps.viable(CoreProductionPostureShape::CpuPrimary) != expect_cpu ||
        mode_capture_caps.viable(CoreProductionPostureShape::GpuPrimaryNoCpuSidecar) != expect_gpu ||
        mode_capture_caps.viable(CoreProductionPostureShape::GpuPrimaryWithCpuSidecar) != expect_sidecar) {
      std::cerr << "FAIL synthetic backing capability truth " << label << " mode mismatch\n";
      return false;
    }
    return true;
  };

  if (!verify_mode_truth(SyntheticProducerOutputFormMode::CpuOnly, true, false, "cpu_only")) return false;
  if (!verify_mode_truth(SyntheticProducerOutputFormMode::CpuAndGpu, true, runtime_gpu_gate, "cpu_gpu")) return false;
  if (!verify_mode_truth(SyntheticProducerOutputFormMode::GpuOnly, false, runtime_gpu_gate, "gpu_only")) return false;

  return true;
}

bool run_synthetic_parent_context_capability_downgrade_matrix_check() {
  const bool runtime_gpu_gate = synthetic_gpu_backing_runtime_available();

  SyntheticStreamCapabilityDowngradeCondition stream_condition{};
  stream_condition.device_hardware_id = "synthetic:0";
  stream_condition.has_stream_intent = true;
  stream_condition.stream_intent = StreamIntent::PREVIEW;
  stream_condition.width = 64;
  stream_condition.height = 48;
  stream_condition.format_fourcc = FOURCC_RGBA;
  stream_condition.target_fps = 30;

  SyntheticCaptureCapabilityDowngradeCondition capture_condition{};
  capture_condition.device_hardware_id = "synthetic:0";
  capture_condition.width = 80;
  capture_condition.height = 60;
  capture_condition.format_fourcc = FOURCC_RGBA;
  capture_condition.still_image_bundle =
      SyntheticStillImageBundleDiscriminator::MultiImage;

  auto verify_cpu_only_caps = [](ProducerBackingCapabilities caps) {
    return caps.cpu_backed_available &&
           !caps.gpu_backed_available &&
           !caps.gpu_with_cpu_sidecar_available &&
           caps.viable(CoreProductionPostureShape::CpuPrimary) &&
           !caps.viable(CoreProductionPostureShape::GpuPrimaryNoCpuSidecar) &&
           !caps.viable(CoreProductionPostureShape::GpuPrimaryWithCpuSidecar);
  };
  auto verify_mixed_caps = [runtime_gpu_gate](ProducerBackingCapabilities caps) {
    return caps.cpu_backed_available &&
           (caps.gpu_backed_available == runtime_gpu_gate) &&
           (caps.gpu_with_cpu_sidecar_available == runtime_gpu_gate);
  };

  auto make_stream_profile = [](uint32_t width,
                                uint32_t height,
                                uint32_t fps) {
    CaptureProfile profile{};
    profile.width = width;
    profile.height = height;
    profile.format_fourcc = FOURCC_RGBA;
    profile.target_fps_min = fps;
    profile.target_fps_max = fps;
    return profile;
  };

  auto verify_config = [&](SyntheticProducerOutputFormMode mode,
                           const char* label,
                           bool expect_match_downgrade) -> bool {
    RecorderCallbacks cb;
    SyntheticProviderConfig cfg{};
    cfg.endpoint_count = 1;
    cfg.nominal.width = 64;
    cfg.nominal.height = 48;
    cfg.nominal.format_fourcc = FOURCC_RGBA;
    cfg.producer_output_form_mode = mode;
    cfg.verification_stream_capability_downgrade_conditions.push_back(
        stream_condition);
    cfg.verification_capture_capability_downgrade_conditions.push_back(
        capture_condition);
    SyntheticProvider provider(cfg);
    if (!provider.initialize(&cb).ok() ||
        !provider.open_device("synthetic:0", 91, 9101).ok()) {
      std::cerr << "FAIL synthetic parent-context downgrade " << label
                << " setup failed\n";
      (void)provider.shutdown();
      return false;
    }

    const CaptureProfile matching_stream_profile = make_stream_profile(64, 48, 30);
    const CaptureProfile nonmatching_stream_profile = make_stream_profile(64, 48, 24);
    const PictureConfig picture{};
    CaptureRequest matching_capture_req =
        make_direct_provider_multi_member_still_capture_request(
            9201, 91, 80, 60, FOURCC_RGBA, {100});
    CaptureRequest nonmatching_capture_req =
        make_direct_provider_default_still_capture_request(
            9202, 91, 80, 60, FOURCC_RGBA);

    const ProducerBackingCapabilities stream_outer_caps =
        provider.stream_backing_capabilities(matching_stream_profile, picture);
    const ProducerBackingCapabilities stream_parent_match_caps =
        provider.stream_parent_context_backing_capabilities(
            91, 93, StreamIntent::PREVIEW, matching_stream_profile, picture);
    const ProducerBackingCapabilities stream_parent_nonmatch_caps =
        provider.stream_parent_context_backing_capabilities(
            91, 94, StreamIntent::VIEWFINDER, nonmatching_stream_profile, picture);
    const ProducerBackingCapabilities capture_outer_caps =
        provider.capture_backing_capabilities(matching_capture_req);
    const ProducerBackingCapabilities capture_parent_match_caps =
        provider.capture_parent_context_backing_capabilities(
            91, matching_capture_req);
    const ProducerBackingCapabilities capture_parent_nonmatch_caps =
        provider.capture_parent_context_backing_capabilities(
            91, nonmatching_capture_req);

    if (!provider.close_device(91).ok() || !provider.shutdown().ok()) {
      std::cerr << "FAIL synthetic parent-context downgrade " << label
                << " teardown failed\n";
      return false;
    }

    const bool expect_outer_mixed =
        mode == SyntheticProducerOutputFormMode::CpuAndGpu && runtime_gpu_gate;
    if ((expect_outer_mixed &&
         (!verify_mixed_caps(stream_outer_caps) ||
          !verify_mixed_caps(capture_outer_caps))) ||
        (!expect_outer_mixed &&
         (!verify_cpu_only_caps(stream_outer_caps) ||
          !verify_cpu_only_caps(capture_outer_caps)))) {
      std::cerr << "FAIL synthetic parent-context downgrade " << label
                << " outer-envelope truth mismatch\n";
      return false;
    }

    if (expect_match_downgrade) {
      if (!verify_cpu_only_caps(stream_parent_match_caps) ||
          !verify_cpu_only_caps(capture_parent_match_caps)) {
        std::cerr << "FAIL synthetic parent-context downgrade " << label
                  << " matching condition did not narrow to cpu_only\n";
        return false;
      }
    } else {
      if (!verify_cpu_only_caps(stream_parent_match_caps) ||
          !verify_cpu_only_caps(capture_parent_match_caps)) {
        std::cerr << "FAIL synthetic parent-context downgrade " << label
                  << " expected cpu_only inert truth mismatch\n";
        return false;
      }
    }

    if ((expect_outer_mixed &&
         (!verify_mixed_caps(stream_parent_nonmatch_caps) ||
          !verify_mixed_caps(capture_parent_nonmatch_caps))) ||
        (!expect_outer_mixed &&
         (!verify_cpu_only_caps(stream_parent_nonmatch_caps) ||
          !verify_cpu_only_caps(capture_parent_nonmatch_caps)))) {
      std::cerr << "FAIL synthetic parent-context downgrade " << label
                << " non-matching condition changed outer truth\n";
      return false;
    }
    return true;
  };

  if (!verify_config(
          SyntheticProducerOutputFormMode::CpuAndGpu,
          "cpu_gpu",
          runtime_gpu_gate)) {
    return false;
  }
  if (!verify_config(
          SyntheticProducerOutputFormMode::CpuOnly,
          "cpu_only",
          false)) {
    return false;
  }

  RecorderCallbacks gpu_only_cb;
  SyntheticProviderConfig gpu_only_cfg{};
  gpu_only_cfg.endpoint_count = 1;
  gpu_only_cfg.producer_output_form_mode = SyntheticProducerOutputFormMode::GpuOnly;
  gpu_only_cfg.verification_stream_capability_downgrade_conditions.push_back(
      stream_condition);
  SyntheticProvider gpu_only_provider(gpu_only_cfg);
  const ProviderResult gpu_only_init = gpu_only_provider.initialize(&gpu_only_cb);
  if (gpu_only_init.ok() ||
      gpu_only_init.code != ProviderError::ERR_INVALID_ARGUMENT) {
    std::cerr << "FAIL synthetic parent-context downgrade gpu_only contradictory configuration was not rejected\n";
    (void)gpu_only_provider.shutdown();
    return false;
  }

  return true;
}

bool run_synthetic_producer_output_form_mode_production_check() {
  auto make_req = [](uint64_t stream_id) {
    StreamRequest req{};
    req.stream_id = stream_id;
    req.device_instance_id = 81;
    req.intent = StreamIntent::PREVIEW;
    req.profile.width = 32;
    req.profile.height = 32;
    req.profile.format_fourcc = FOURCC_RGBA;
    req.profile.target_fps_min = 30;
    req.profile.target_fps_max = 30;
    return req;
  };

  auto verify_cpu_frame_mode = [&](SyntheticProducerOutputFormMode mode, const char* label, uint64_t stream_id) -> bool {
    RecorderCallbacks cb;
    SyntheticProviderConfig cfg{};
    cfg.endpoint_count = 1;
    cfg.nominal.width = 32;
    cfg.nominal.height = 32;
    cfg.nominal.format_fourcc = FOURCC_RGBA;
    cfg.nominal.start_stream_warmup_ns = 0;
    cfg.producer_output_form_mode = mode;
    SyntheticProvider provider(cfg);
    const StreamRequest req = make_req(stream_id);
    if (!provider.initialize(&cb).ok() ||
        !provider.open_device("synthetic:0", req.device_instance_id, 8101).ok() ||
        !provider.create_stream(req).ok() ||
        !provider.start_stream(req.stream_id, req.profile, req.picture).ok()) {
      std::cerr << "FAIL synthetic stream backing mode " << label << " setup failed\n";
      (void)provider.shutdown();
      return false;
    }
    provider.advance(0);
    EventRec frame{};
    const bool got_frame = wait_for_stream_frame(cb, req.stream_id, frame);
    if (!provider.stop_stream(req.stream_id).ok() ||
        !provider.destroy_stream(req.stream_id).ok() ||
        !provider.close_device(req.device_instance_id).ok() ||
        !provider.shutdown().ok()) {
      std::cerr << "FAIL synthetic stream backing mode " << label << " teardown failed\n";
      return false;
    }
    if (!got_frame) {
      std::cerr << "FAIL synthetic stream backing mode " << label << " emitted no frame\n";
      return false;
    }
    if (frame.primary_backing_kind != ProducerBackingKind::CPU ||
        frame.has_primary_backing_artifact ||
        !frame.retain_cpu_sidecar ||
        frame.payload_size_bytes == 0) {
      std::cerr << "FAIL synthetic stream backing mode " << label << " CPU frame truth mismatch\n";
      return false;
    }
    return true;
  };

  auto verify_cpu_capture_mode = [&](SyntheticProducerOutputFormMode mode, const char* label, uint64_t capture_id) -> bool {
    RecorderCallbacks cb;
    SyntheticProviderConfig cfg{};
    cfg.endpoint_count = 1;
    cfg.nominal.width = 32;
    cfg.nominal.height = 32;
    cfg.nominal.format_fourcc = FOURCC_RGBA;
    cfg.producer_output_form_mode = mode;
    SyntheticProvider provider(cfg);
    if (!provider.initialize(&cb).ok() ||
        !provider.open_device("synthetic:0", 81, 8101).ok()) {
      std::cerr << "FAIL synthetic capture output-form mode " << label << " setup failed\n";
      (void)provider.shutdown();
      return false;
    }
    CaptureRequest cap = make_direct_provider_default_still_capture_request(capture_id, 81, 32, 32, FOURCC_RGBA);
    if (!provider.trigger_capture(cap).ok() || !wait_for_capture_completed_with_frames(cb, capture_id, 1)) {
      std::cerr << "FAIL synthetic capture output-form mode " << label << " did not complete\n";
      (void)provider.shutdown();
      return false;
    }
    EventRec frame{};
    for (const auto& ev : cb.snapshot_events()) {
      if (ev.tag == "frame" && ev.capture_id == capture_id) {
        frame = ev;
        break;
      }
    }
    if (!provider.close_device(81).ok() || !provider.shutdown().ok()) {
      std::cerr << "FAIL synthetic capture output-form mode " << label << " teardown failed\n";
      return false;
    }
    if (frame.capture_id != capture_id ||
        frame.primary_backing_kind != ProducerBackingKind::CPU ||
        frame.has_primary_backing_artifact ||
        !frame.retain_cpu_sidecar ||
        frame.payload_size_bytes == 0) {
      std::cerr << "FAIL synthetic capture output-form mode " << label << " CPU frame truth mismatch\n";
      return false;
    }
    return true;
  };

  if (!verify_cpu_frame_mode(SyntheticProducerOutputFormMode::CpuOnly, "cpu_only", 8102)) return false;
  if (!verify_cpu_capture_mode(SyntheticProducerOutputFormMode::CpuOnly, "cpu_only", 8112)) return false;

  const bool runtime_gpu_gate = synthetic_gpu_backing_runtime_available();
  if (!runtime_gpu_gate) {
    auto verify_unavailable_gpu_refusal = [&](SyntheticProducerOutputFormMode mode, uint64_t stream_id, uint64_t capture_id) -> bool {
      RecorderCallbacks cb;
      SyntheticProviderConfig cfg{};
      cfg.endpoint_count = 1;
      cfg.nominal.width = 32;
      cfg.nominal.height = 32;
      cfg.nominal.format_fourcc = FOURCC_RGBA;
      cfg.producer_output_form_mode = mode;
      SyntheticProvider provider(cfg);
      const StreamRequest req = make_req(stream_id);
      if (!provider.initialize(&cb).ok() ||
          !provider.open_device("synthetic:0", req.device_instance_id, 8101).ok() ||
          !provider.create_stream(req).ok()) {
        std::cerr << "FAIL synthetic stream backing unavailable-gpu setup failed\n";
        (void)provider.shutdown();
        return false;
      }
      const ProviderResult start_result = provider.start_stream(req.stream_id, req.profile, req.picture);
      if (start_result.code != ProviderError::ERR_NOT_SUPPORTED) {
        std::cerr << "FAIL synthetic stream backing unavailable-gpu mode did not refuse clearly\n";
        (void)provider.shutdown();
        return false;
      }
      CaptureRequest cap = make_direct_provider_default_still_capture_request(capture_id, req.device_instance_id, 32, 32, FOURCC_RGBA);
      const ProviderResult capture_result = provider.trigger_capture(cap);
      if (capture_result.code != ProviderError::ERR_NOT_SUPPORTED) {
        std::cerr << "FAIL synthetic capture backing unavailable-gpu mode did not refuse clearly\n";
        (void)provider.shutdown();
        return false;
      }
      if (!provider.destroy_stream(req.stream_id).ok() ||
          !provider.close_device(req.device_instance_id).ok() ||
          !provider.shutdown().ok()) {
        std::cerr << "FAIL synthetic stream backing unavailable-gpu teardown failed\n";
        return false;
      }
      return true;
    };
    if (!verify_unavailable_gpu_refusal(SyntheticProducerOutputFormMode::GpuOnly, 8103, 8113)) return false;
    if (!verify_cpu_frame_mode(SyntheticProducerOutputFormMode::CpuAndGpu, "cpu_and_gpu_cpu_collapsed", 8104)) return false;
    if (!verify_cpu_capture_mode(SyntheticProducerOutputFormMode::CpuAndGpu, "cpu_and_gpu_cpu_collapsed", 8114)) return false;
    if (!verify_cpu_frame_mode(SyntheticProducerOutputFormMode::Auto, "auto_cpu_default", 8105)) return false;
    if (!verify_cpu_capture_mode(SyntheticProducerOutputFormMode::Auto, "auto_cpu_default", 8115)) return false;
    return true;
  }

  // In builds with a real Synthetic GPU runtime installed, assert production
  // truth directly. Host maintainer tools normally exercise the unavailable-GPU
  // refusal branch above.
  auto verify_gpu_frame_mode = [&](SyntheticProducerOutputFormMode mode,
                                   const char* label,
                                   uint64_t stream_id,
                                   bool expected_cpu_sidecar) -> bool {
    RecorderCallbacks cb;
    SyntheticProviderConfig cfg{};
    cfg.endpoint_count = 1;
    cfg.nominal.width = 32;
    cfg.nominal.height = 32;
    cfg.nominal.format_fourcc = FOURCC_RGBA;
    cfg.nominal.start_stream_warmup_ns = 0;
    cfg.producer_output_form_mode = mode;
    SyntheticProvider provider(cfg);
    const StreamRequest req = make_req(stream_id);
    if (!provider.initialize(&cb).ok() ||
        !provider.open_device("synthetic:0", req.device_instance_id, 8101).ok() ||
        !provider.create_stream(req).ok() ||
        !provider.start_stream(req.stream_id, req.profile, req.picture).ok()) {
      std::cerr << "FAIL synthetic stream backing mode " << label << " setup failed\n";
      (void)provider.shutdown();
      return false;
    }
    provider.advance(0);
    EventRec frame{};
    const bool got_frame = wait_for_stream_frame(cb, req.stream_id, frame);
    if (!provider.stop_stream(req.stream_id).ok() ||
        !provider.destroy_stream(req.stream_id).ok() ||
        !provider.close_device(req.device_instance_id).ok() ||
        !provider.shutdown().ok()) {
      std::cerr << "FAIL synthetic stream backing mode " << label << " teardown failed\n";
      return false;
    }
    if (!got_frame) {
      std::cerr << "FAIL synthetic stream backing mode " << label << " emitted no frame\n";
      return false;
    }
    if (frame.primary_backing_kind != ProducerBackingKind::GPU ||
        !frame.has_primary_backing_artifact ||
        frame.retain_cpu_sidecar != expected_cpu_sidecar ||
        ((frame.payload_size_bytes != 0) != expected_cpu_sidecar)) {
      std::cerr << "FAIL synthetic stream backing mode " << label << " GPU frame truth mismatch\n";
      return false;
    }
    return true;
  };

  auto verify_gpu_capture_mode = [&](SyntheticProducerOutputFormMode mode,
                                     const char* label,
                                     uint64_t capture_id,
                                     bool expected_cpu_sidecar) -> bool {
    RecorderCallbacks cb;
    SyntheticProviderConfig cfg{};
    cfg.endpoint_count = 1;
    cfg.nominal.width = 32;
    cfg.nominal.height = 32;
    cfg.nominal.format_fourcc = FOURCC_RGBA;
    cfg.producer_output_form_mode = mode;
    SyntheticProvider provider(cfg);
    if (!provider.initialize(&cb).ok() ||
        !provider.open_device("synthetic:0", 81, 8101).ok()) {
      std::cerr << "FAIL synthetic capture output-form mode " << label << " setup failed\n";
      (void)provider.shutdown();
      return false;
    }
    CaptureRequest cap = make_direct_provider_default_still_capture_request(capture_id, 81, 32, 32, FOURCC_RGBA);
    if (!provider.trigger_capture(cap).ok() || !wait_for_capture_completed_with_frames(cb, capture_id, 1)) {
      std::cerr << "FAIL synthetic capture output-form mode " << label << " did not complete\n";
      (void)provider.shutdown();
      return false;
    }
    EventRec frame{};
    for (const auto& ev : cb.snapshot_events()) {
      if (ev.tag == "frame" && ev.capture_id == capture_id) {
        frame = ev;
        break;
      }
    }
    if (!provider.close_device(81).ok() || !provider.shutdown().ok()) {
      std::cerr << "FAIL synthetic capture output-form mode " << label << " teardown failed\n";
      return false;
    }
    if (frame.capture_id != capture_id ||
        frame.primary_backing_kind != ProducerBackingKind::GPU ||
        !frame.has_primary_backing_artifact ||
        frame.retain_cpu_sidecar != expected_cpu_sidecar ||
        ((frame.payload_size_bytes != 0) != expected_cpu_sidecar)) {
      std::cerr << "FAIL synthetic capture output-form mode " << label << " GPU frame truth mismatch\n";
      return false;
    }
    return true;
  };

  if (!verify_gpu_frame_mode(SyntheticProducerOutputFormMode::GpuOnly, "gpu_only", 8106, false)) return false;
  if (!verify_gpu_capture_mode(SyntheticProducerOutputFormMode::GpuOnly, "gpu_only", 8116, false)) return false;
  if (!verify_gpu_frame_mode(SyntheticProducerOutputFormMode::CpuAndGpu, "cpu_and_gpu", 8107, true)) return false;
  if (!verify_gpu_capture_mode(SyntheticProducerOutputFormMode::CpuAndGpu, "cpu_and_gpu", 8117, true)) return false;
  if (!verify_gpu_frame_mode(SyntheticProducerOutputFormMode::Auto, "auto_gpu_default", 8108, true)) return false;
  if (!verify_gpu_capture_mode(SyntheticProducerOutputFormMode::Auto, "auto_gpu_default", 8118, true)) return false;
  return true;
}

// ===== Family E: Synthetic frame/picture integration compliance =====

bool run_synthetic_timeline_picture_appearance_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.synthetic_role = SyntheticRole::Timeline;
  cfg.timing_driver = TimingDriver::VirtualTime;
  cfg.endpoint_count = 1;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  cfg.nominal.fps_num = 30;
  cfg.nominal.fps_den = 1;
  cfg.nominal.start_stream_warmup_ns = 0;

  SyntheticProvider synthetic(cfg);
  if (!synthetic.initialize(&cb).ok()) return false;

  const uint64_t device_id = 51;
  const uint64_t root_id = 5201;
  const uint64_t stream_id = 52;
  const StreamTemplate st = synthetic.stream_template();
  const uint32_t fps_num = st.profile.target_fps_max != 0 ? st.profile.target_fps_max : st.profile.target_fps_min;
  if (fps_num == 0) {
    std::cerr << "FAIL synthetic picture check invalid fps\n";
    (void)synthetic.shutdown();
    return false;
  }
  const uint64_t period_ns = 1'000'000'000ull / static_cast<uint64_t>(fps_num);

  StreamRequest req{};
  req.stream_id = stream_id;
  req.device_instance_id = device_id;
  req.intent = StreamIntent::PREVIEW;
  req.profile = st.profile;
  req.picture = st.picture;

  if (!synthetic.open_device("synthetic:0", device_id, root_id).ok() ||
      !synthetic.create_stream(req).ok() ||
      !synthetic.start_stream(stream_id, req.profile, req.picture).ok()) {
    (void)synthetic.shutdown();
    return false;
  }

  SyntheticTimelineScenario scenario{};
  for (uint64_t i = 0; i < 3; ++i) {
    SyntheticScheduledEvent ev{};
    ev.at_ns = period_ns * i;
    ev.type = SyntheticEventType::EmitFrame;
    ev.stream_id = stream_id;
    scenario.events.push_back(ev);
  }

  if (!synthetic.set_timeline_scenario_for_host(scenario).ok() ||
      !synthetic.start_timeline_scenario_for_host().ok()) {
    (void)synthetic.shutdown();
    return false;
  }

  synthetic.advance(0);

  PictureConfig updated = req.picture;
  updated.preset = PatternPreset::Solid;
  updated.overlay_frame_index_offsets = false;
  updated.overlay_moving_bar = false;
  updated.solid_r = 25;
  updated.solid_g = 200;
  updated.solid_b = 75;
  updated.solid_a = 255;
  if (!synthetic.set_stream_picture_config(stream_id, updated).ok()) {
    (void)synthetic.shutdown();
    return false;
  }

  synthetic.advance(period_ns * 2);

  const auto cb_events_for_frames = cb.snapshot_events();
  const int f0 = find_frame_index_by_ts(cb_events_for_frames, 0);
  const int f1 = find_frame_index_by_ts(cb_events_for_frames, period_ns);
  const int f2 = find_frame_index_by_ts(cb_events_for_frames, period_ns * 2);
  if (f0 < 0 || f1 < 0 || f2 < 0) {
    std::cerr << "FAIL synthetic picture check frame evidence missing\n";
    (void)synthetic.shutdown();
    return false;
  }

  const uint32_t sig0 = cb_events_for_frames[static_cast<size_t>(f0)].pixel_sig;
  const uint32_t sig1 = cb_events_for_frames[static_cast<size_t>(f1)].pixel_sig;
  const uint32_t sig2 = cb_events_for_frames[static_cast<size_t>(f2)].pixel_sig;
  if (sig0 == sig1 || sig1 != sig2) {
    std::cerr << "FAIL synthetic picture check rendered appearance contract mismatch\n";
    (void)synthetic.shutdown();
    return false;
  }

  if (!synthetic.stop_stream(stream_id).ok() ||
      !synthetic.destroy_stream(stream_id).ok() ||
      !synthetic.close_device(device_id).ok() ||
      !synthetic.shutdown().ok()) {
    return false;
  }
  const auto cb_events_after_teardown = cb.snapshot_events();
  return assert_native_balance(cb_events_after_teardown, "synthetic_picture_appearance");
}

bool run_stub_provider_sanity_check() {
  RecorderCallbacks cb;
  StubProvider provider;

  StreamRequest req{};
  req.stream_id = 11;
  req.device_instance_id = 1;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 64;
  req.profile.height = 64;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;

  if (!provider.initialize(&cb).ok()) return false;
  if (!provider.open_device("stub0", 1, 1001).ok()) return false;
  if (!provider.create_stream(req).ok()) return false;
  if (!provider.start_stream(req.stream_id, req.profile, req.picture).ok()) return false;
  if (!provider.stop_stream(req.stream_id).ok()) return false;
  if (!provider.destroy_stream(req.stream_id).ok()) return false;
  if (!provider.close_device(req.device_instance_id).ok()) return false;
  if (!provider.shutdown().ok()) return false;

  const auto cb_events = cb.snapshot_events();
  const bool ok = assert_native_balance(cb_events, "stub");
  return ok;
}

bool run_synthetic_provider_direct_sanity_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider provider(cfg);

  StreamRequest req{};
  req.stream_id = 12;
  req.device_instance_id = 1;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 64;
  req.profile.height = 64;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;

  if (!provider.initialize(&cb).ok() ||
      !provider.open_device("synthetic:0", 1, 2001).ok() ||
      !provider.create_stream(req).ok() ||
      !provider.start_stream(req.stream_id, req.profile, req.picture).ok() ||
      !provider.stop_stream(req.stream_id).ok() ||
      !provider.destroy_stream(req.stream_id).ok() ||
      !provider.close_device(req.device_instance_id).ok() ||
      !provider.shutdown().ok()) {
    return false;
  }

  const auto cb_events = cb.snapshot_events();
  const int stopped = find_event_index(cb_events, "stream_stopped", req.stream_id);
  const int destroyed = find_event_index(cb_events, "stream_destroyed", req.stream_id);
  const int closed = find_event_index(cb_events, "device_closed", req.device_instance_id);
  const int stream_native_id = find_native_create_id(cb_events, static_cast<uint32_t>(NativeObjectType::Stream), req.stream_id);
  const int acquisition_session_native_id =
      find_native_create_id_by_type(cb_events, static_cast<uint32_t>(NativeObjectType::AcquisitionSession));
  const int device_native_id = find_native_create_id(cb_events, static_cast<uint32_t>(NativeObjectType::Device), 0);
  const int gpu_backing_created = count_events_by_tag_type_and_owner_stream(
      cb_events,
      "native_created",
      static_cast<uint32_t>(NativeObjectType::GpuBacking),
      req.stream_id);
  const int gpu_backing_destroyed = count_events_by_tag_type_and_owner_stream(
      cb_events,
      "native_destroyed",
      static_cast<uint32_t>(NativeObjectType::GpuBacking),
      req.stream_id);
  const int frame_buffer_lease_created = count_events_by_tag_type_and_owner_stream(
      cb_events,
      "native_created",
      static_cast<uint32_t>(NativeObjectType::FrameBufferLease),
      req.stream_id);
  const int frame_buffer_lease_destroyed = count_events_by_tag_type_and_owner_stream(
      cb_events,
      "native_destroyed",
      static_cast<uint32_t>(NativeObjectType::FrameBufferLease),
      req.stream_id);

  if (stopped < 0 || destroyed < 0 || closed < 0 ||
      stream_native_id < 0 || acquisition_session_native_id < 0 || device_native_id < 0) {
    std::cerr << "FAIL synthetic direct sanity missing callback/native evidence\n";
    return false;
  }
  const bool gpu_backing_realized_in_run = (gpu_backing_created > 0 || gpu_backing_destroyed > 0);
  if (gpu_backing_realized_in_run) {
    if (gpu_backing_created <= 0 || gpu_backing_destroyed <= 0) {
      std::cerr << "FAIL synthetic direct sanity incomplete gpu backing native support lifecycle\n";
      return false;
    }
  }
  const bool frame_buffer_lease_realized_in_run =
      (frame_buffer_lease_created > 0 || frame_buffer_lease_destroyed > 0);
  if (frame_buffer_lease_realized_in_run) {
    if (frame_buffer_lease_created <= 0 || frame_buffer_lease_destroyed <= 0) {
      std::cerr
          << "FAIL synthetic direct sanity incomplete frame buffer lease native support lifecycle\n";
      return false;
    }
  }

  return assert_native_balance(cb_events, "synthetic_direct");
}

bool run_synthetic_still_only_acquisition_session_truth_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider provider(cfg);

  if (!provider.initialize(&cb).ok() || !provider.open_device("synthetic:0", 41, 4101).ok()) {
    std::cerr << "FAIL synthetic still-only setup failed\n";
    return false;
  }

  CaptureRequest cap = make_direct_provider_default_still_capture_request(8001, 41, 64, 64, FOURCC_RGBA);
  if (!provider.trigger_capture(cap).ok()) {
    std::cerr << "FAIL synthetic still-only trigger_capture failed\n";
    return false;
  }
  if (!wait_for_capture_completed_with_frames(cb, cap.capture_id, 1)) {
    std::cerr << "FAIL synthetic still-only capture did not complete with frame evidence before close\n";
    return false;
  }

  if (!provider.close_device(41).ok() || !provider.shutdown().ok()) {
    std::cerr << "FAIL synthetic still-only teardown failed\n";
    return false;
  }

  const auto cb_events = cb.snapshot_events();
  const int capture_started_ix = find_event_index(cb_events, "capture_started", cap.capture_id);
  const int capture_completed_ix = find_event_index(cb_events, "capture_completed", cap.capture_id);
  const int acq_native_id =
      find_native_create_id_by_type(cb_events, static_cast<uint32_t>(NativeObjectType::AcquisitionSession));
  const int acq_create_ix = find_event_index(cb_events, "native_created", static_cast<uint64_t>(acq_native_id));
  const int acq_destroy_ix = find_event_index(cb_events, "native_destroyed", static_cast<uint64_t>(acq_native_id));

  if (capture_started_ix < 0 || capture_completed_ix < 0 || acq_native_id < 0 ||
      acq_create_ix < 0 || acq_destroy_ix < 0) {
    std::cerr << "FAIL synthetic still-only missing capture/session evidence\n";
    return false;
  }
  if (!(acq_create_ix < capture_started_ix &&
        capture_started_ix < capture_completed_ix &&
        capture_completed_ix < acq_destroy_ix)) {
    std::cerr << "FAIL synthetic still-only lifecycle ordering invalid\n";
    return false;
  }
  int frame_count = 0;
  for (const auto& ev : cb_events) {
    if (ev.tag == "frame") {
      ++frame_count;
      if (ev.capture_image_routing != CaptureImageRouting::DEFAULT_METERED) {
        std::cerr << "FAIL synthetic still-only frame routing expected DEFAULT_METERED\n";
        return false;
      }
    }
  }
  if (frame_count != 1) {
    std::cerr << "FAIL synthetic still-only expected exactly one frame\n";
    return false;
  }
  if (count_events_by_tag_and_type(
          cb_events, "native_created", static_cast<uint32_t>(NativeObjectType::Stream)) != 0) {
    std::cerr << "FAIL synthetic still-only unexpectedly realized stream native object\n";
    return false;
  }
  return assert_native_balance(cb_events, "synthetic_still_only");
}

bool run_synthetic_multi_member_still_sequence_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider provider(cfg);

  if (!provider.initialize(&cb).ok() || !provider.open_device("synthetic:0", 43, 4301).ok()) {
    std::cerr << "FAIL synthetic multi-member setup failed\n";
    return false;
  }

  CaptureRequest cap = make_direct_provider_multi_member_still_capture_request(
      8003, 43, 64, 64, FOURCC_RGBA, {1000});
  if (!is_valid_capture_still_image_bundle(cap.still_image_bundle, provider.supports_multi_image_still_sequence())) {
    std::cerr << "FAIL synthetic multi-member request validation rejected expected valid sequence\n";
    return false;
  }
  if (!provider.trigger_capture(cap).ok()) {
    std::cerr << "FAIL synthetic multi-member trigger_capture failed\n";
    return false;
  }
  if (!wait_for_capture_completed_with_frames(cb, cap.capture_id, 2)) {
    std::cerr << "FAIL synthetic multi-member capture did not complete with frame evidence before close\n";
    return false;
  }
  if (!provider.close_device(43).ok() || !provider.shutdown().ok()) {
    std::cerr << "FAIL synthetic multi-member teardown failed\n";
    return false;
  }

  int started_count = 0;
  int completed_count = 0;
  std::vector<EventRec> frame_events;
  const auto cb_events = cb.snapshot_events();
  for (const auto& ev : cb_events) {
    if (ev.tag == "capture_started" && ev.id == cap.capture_id) ++started_count;
    if (ev.tag == "capture_completed" && ev.id == cap.capture_id) ++completed_count;
    if (ev.tag == "frame") frame_events.push_back(ev);
  }
  if (started_count != 1 || completed_count != 1 || frame_events.size() != 2) {
    std::cerr << "FAIL synthetic multi-member lifecycle/frame count mismatch\n";
    return false;
  }
  if (frame_events[0].capture_image_routing != CaptureImageRouting::DEFAULT_METERED ||
      frame_events[1].capture_image_routing != CaptureImageRouting::ADDITIONAL_BRACKET) {
    std::cerr << "FAIL synthetic multi-member routing mismatch\n";
    return false;
  }
  if (frame_events[0].capture_image_member_index != 0 ||
      frame_events[0].capture_image_applied_exposure_compensation_milli_ev != 0 ||
      !frame_events[0].capture_image_has_realized_exposure_compensation_milli_ev ||
      frame_events[0].capture_image_realized_exposure_compensation_milli_ev !=
          frame_events[0].capture_image_applied_exposure_compensation_milli_ev ||
      frame_events[1].capture_image_member_index != 1 ||
      frame_events[1].capture_image_applied_exposure_compensation_milli_ev != 1000 ||
      !frame_events[1].capture_image_has_realized_exposure_compensation_milli_ev ||
      frame_events[1].capture_image_realized_exposure_compensation_milli_ev !=
          frame_events[1].capture_image_applied_exposure_compensation_milli_ev) {
    std::cerr << "FAIL synthetic multi-member emitted metadata mismatch\n";
    return false;
  }
  if (frame_events[0].payload_size_bytes == 0 || frame_events[1].payload_size_bytes == 0) {
    std::cerr << "FAIL synthetic multi-member expected non-empty payloads\n";
    return false;
  }
  if (frame_events[0].payload_size_bytes != frame_events[1].payload_size_bytes) {
    std::cerr << "FAIL synthetic multi-member expected equal-sized payloads\n";
    return false;
  }
  if (frame_events[0].payload_hash == frame_events[1].payload_hash) {
    std::cerr << "FAIL synthetic multi-member expected deterministic payload hash difference\n";
    return false;
  }
  return assert_native_balance(cb_events, "synthetic_multi_member_still_sequence");
}

bool run_synthetic_dynamic_still_bundle_shape_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider provider(cfg);
  if (!provider.initialize(&cb).ok() || !provider.open_device("synthetic:0", 144, 14401).ok()) {
    std::cerr << "FAIL synthetic dynamic bundle setup failed\n";
    return false;
  }
  auto fail_with_cleanup = [&](const char* msg) -> bool {
    std::cerr << msg << "\n";
    (void)provider.close_device(144);
    (void)provider.shutdown();
    return false;
  };
  auto run_capture_and_collect = [&](uint64_t capture_id,
                                     const std::vector<int32_t>& member_evs,
                                     std::vector<EventRec>& out_frames) -> bool {
    std::vector<int32_t> additional_member_evs;
    if (member_evs.size() > 1) {
      additional_member_evs.assign(member_evs.begin() + 1, member_evs.end());
    }
    CaptureRequest req = make_direct_provider_multi_member_still_capture_request(
        capture_id, 144, 64, 64, FOURCC_RGBA, additional_member_evs);
    if (!is_valid_capture_still_image_bundle(req.still_image_bundle, provider.supports_multi_image_still_sequence())) {
      return false;
    }
    if (!provider.trigger_capture(req).ok()) {
      return false;
    }
    bool completed = false;
    for (int iter = 0; iter < kMaxIters; ++iter) {
      size_t matched_frames = 0;
      completed = false;
      const auto events_snapshot = cb.snapshot_events();
      for (const auto& ev : events_snapshot) {
        if (ev.tag == "capture_failed" && ev.id == capture_id) {
          return false;
        }
        if (ev.tag == "capture_completed" && ev.id == capture_id) {
          completed = true;
        }
        if (ev.tag == "frame" &&
            ev.capture_id == capture_id &&
            ev.capture_image_member_index < member_evs.size() &&
            ev.capture_image_has_realized_exposure_compensation_milli_ev &&
            ev.capture_image_realized_exposure_compensation_milli_ev == ev.capture_image_applied_exposure_compensation_milli_ev &&
            ev.capture_image_applied_exposure_compensation_milli_ev == member_evs[ev.capture_image_member_index]) {
          matched_frames++;
        }
      }
      if (completed && matched_frames >= member_evs.size()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
    }
    if (!completed) {
      return false;
    }
    out_frames.clear();
    const auto events_snapshot = cb.snapshot_events();
    for (const auto& ev : events_snapshot) {
      if (ev.tag == "frame" &&
          ev.capture_id == capture_id &&
          ev.capture_image_member_index < member_evs.size() &&
          ev.capture_image_has_realized_exposure_compensation_milli_ev &&
          ev.capture_image_realized_exposure_compensation_milli_ev == ev.capture_image_applied_exposure_compensation_milli_ev &&
          ev.capture_image_applied_exposure_compensation_milli_ev == member_evs[ev.capture_image_member_index]) {
        out_frames.push_back(ev);
      }
    }
    return out_frames.size() >= member_evs.size();
  };

  {
    const std::vector<int32_t> evs{0};
    std::vector<EventRec> frames;
    if (!run_capture_and_collect(8101, evs, frames)) {
      return fail_with_cleanup("FAIL synthetic dynamic one-member capture failed");
    }
  }
  {
    const std::vector<int32_t> evs{0, -1000, 2000};
    std::vector<EventRec> frames;
    if (!run_capture_and_collect(8102, evs, frames)) {
      return fail_with_cleanup("FAIL synthetic dynamic asymmetric capture failed");
    }
    if (frames.size() < evs.size()) {
      return fail_with_cleanup("FAIL synthetic dynamic asymmetric expected member count");
    }
    for (size_t i = 0; i < evs.size(); ++i) {
      const auto& frame = frames[frames.size() - evs.size() + i];
      const CaptureImageRouting expected_routing = (i == 0)
          ? CaptureImageRouting::DEFAULT_METERED
          : CaptureImageRouting::ADDITIONAL_BRACKET;
      if (frame.capture_image_routing != expected_routing ||
          frame.capture_image_member_index != i ||
          !frame.capture_image_has_realized_exposure_compensation_milli_ev ||
          frame.capture_image_realized_exposure_compensation_milli_ev != frame.capture_image_applied_exposure_compensation_milli_ev ||
          frame.capture_image_applied_exposure_compensation_milli_ev != evs[i]) {
        return fail_with_cleanup("FAIL synthetic dynamic asymmetric member metadata mismatch");
      }
    }
    const auto& f0 = frames[frames.size() - 3];
    const auto& f1 = frames[frames.size() - 2];
    const auto& f2 = frames[frames.size() - 1];
    const double y0 = f0.sampled_luma;
    const double y1 = f1.sampled_luma;
    const double y2 = f2.sampled_luma;
    if (!(y1 < y0 && y2 > y0)) {
      std::cerr << "FAIL synthetic dynamic asymmetric expected deterministic EV brightness ordering"
                << " [m0 idx=" << f0.capture_image_member_index << " ev=" << f0.capture_image_applied_exposure_compensation_milli_ev
                << " fmt=" << f0.format_fourcc << " luma=" << y0
                << " rgb=(" << static_cast<int>(f0.sample_r) << "," << static_cast<int>(f0.sample_g) << "," << static_cast<int>(f0.sample_b) << ")]"
                << " [m1 idx=" << f1.capture_image_member_index << " ev=" << f1.capture_image_applied_exposure_compensation_milli_ev
                << " fmt=" << f1.format_fourcc << " luma=" << y1
                << " rgb=(" << static_cast<int>(f1.sample_r) << "," << static_cast<int>(f1.sample_g) << "," << static_cast<int>(f1.sample_b) << ")]"
                << " [m2 idx=" << f2.capture_image_member_index << " ev=" << f2.capture_image_applied_exposure_compensation_milli_ev
                << " fmt=" << f2.format_fourcc << " luma=" << y2
                << " rgb=(" << static_cast<int>(f2.sample_r) << "," << static_cast<int>(f2.sample_g) << "," << static_cast<int>(f2.sample_b) << ")]\n";
      return fail_with_cleanup("FAIL synthetic dynamic asymmetric expected deterministic EV brightness ordering");
    }
  }
  {
    const std::vector<int32_t> evs{0, -2000, -1000, 1000, 2000};
    std::vector<EventRec> frames;
    if (!run_capture_and_collect(8103, evs, frames)) {
      return fail_with_cleanup("FAIL synthetic dynamic large bundle capture failed");
    }
    if (frames.size() < evs.size()) {
      return fail_with_cleanup("FAIL synthetic dynamic large bundle expected member count");
    }
    for (size_t i = 0; i < evs.size(); ++i) {
      const auto& frame = frames[frames.size() - evs.size() + i];
      if (frame.capture_image_member_index != i ||
          !frame.capture_image_has_realized_exposure_compensation_milli_ev ||
          frame.capture_image_realized_exposure_compensation_milli_ev != frame.capture_image_applied_exposure_compensation_milli_ev ||
          frame.capture_image_applied_exposure_compensation_milli_ev != evs[i]) {
        return fail_with_cleanup("FAIL synthetic dynamic large bundle member order/metadata mismatch");
      }
    }
  }
  if (!provider.close_device(144).ok() || !provider.shutdown().ok()) {
    return fail_with_cleanup("FAIL synthetic dynamic bundle teardown failed");
  }
  const auto cb_events = cb.snapshot_events();
  return assert_native_balance(cb_events, "synthetic_dynamic_still_bundle_shape");
}

bool run_core_synthetic_three_member_capture_result_check() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "FAIL core synthetic three-member runtime start failed\n";
    return false;
  }
  if (!wait_for_core_runtime_live(rt)) {
    std::cerr << "FAIL core synthetic three-member runtime did not reach LIVE\n";
    rt.stop();
    return false;
  }

  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider provider(cfg);
  const auto fail_with_cleanup = [&](const char* msg) -> bool {
    std::cerr << msg << "\n";
    rt.stop();
    rt.attach_provider(nullptr);
    return false;
  };
  if (!provider.initialize(rt.provider_callbacks()).ok()) {
    return fail_with_cleanup("FAIL core synthetic three-member provider init failed");
  }
  rt.attach_provider(&provider);
  std::vector<CameraEndpoint> eps;
  if (!provider.enumerate_endpoints(eps).ok() || eps.empty()) {
    return fail_with_cleanup("FAIL core synthetic three-member enumerate failed");
  }

  const uint64_t device_id = 64;
  if (!provider.open_device(eps[0].hardware_id, device_id, 6401).ok()) {
    return fail_with_cleanup("FAIL core synthetic three-member open_device failed");
  }

  CaptureRequest req{};
  for (int i = 0; i < 50; ++i) {
    if (rt.materialize_capture_request(device_id, req)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (req.device_instance_id != device_id) {
    return fail_with_cleanup("FAIL core synthetic three-member materialize request failed");
  }
  CaptureRequest default_req = req;
  default_req.capture_id = 9600;
  if (!provider.trigger_capture(default_req).ok()) {
    return fail_with_cleanup("FAIL core synthetic default trigger_capture failed");
  }
  SharedCaptureResultData default_result;
  for (int i = 0; i < 50; ++i) {
    default_result = rt.get_capture_result(default_req.capture_id, device_id);
    if (default_result) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!default_result || default_result->image_member_count() != 1 || default_result->has_additional_images()) {
    return fail_with_cleanup("FAIL core synthetic default retained result member-count/additional mismatch");
  }

  req.capture_id = 9601;
  req.still_image_bundle = make_default_metered_still_image_bundle();
  req.still_image_bundle.members.push_back(
      CaptureStillImageMember{1u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, -1000});
  req.still_image_bundle.members.push_back(
      CaptureStillImageMember{2u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, +1000});
  if (!provider.trigger_capture(req).ok()) {
    return fail_with_cleanup("FAIL core synthetic three-member trigger_capture failed");
  }

  SharedCaptureResultData result;
  for (int i = 0; i < 50; ++i) {
    result = rt.get_capture_result(req.capture_id, device_id);
    if (result) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!result) {
    return fail_with_cleanup("FAIL core synthetic three-member result missing");
  }
  if (result->default_image.image_member_index != 0 ||
      result->default_image.role != CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED ||
      result->default_image.applied_exposure_compensation_milli_ev != 0 ||
      !result->default_image.has_realized_exposure_compensation_milli_ev ||
      result->default_image.realized_exposure_compensation_milli_ev !=
          result->default_image.applied_exposure_compensation_milli_ev) {
    return fail_with_cleanup("FAIL core synthetic three-member default member metadata mismatch");
  }
  if (result->additional_images.size() != 2 ||
      result->additional_images[0].image_member_index != 1 ||
      result->additional_images[0].role != CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET ||
      result->additional_images[0].applied_exposure_compensation_milli_ev != -1000 ||
      !result->additional_images[0].has_realized_exposure_compensation_milli_ev ||
      result->additional_images[0].realized_exposure_compensation_milli_ev !=
          result->additional_images[0].applied_exposure_compensation_milli_ev ||
      result->additional_images[1].image_member_index != 2 ||
      result->additional_images[1].role != CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET ||
      result->additional_images[1].applied_exposure_compensation_milli_ev != 1000 ||
      !result->additional_images[1].has_realized_exposure_compensation_milli_ev ||
      result->additional_images[1].realized_exposure_compensation_milli_ev !=
          result->additional_images[1].applied_exposure_compensation_milli_ev) {
    return fail_with_cleanup("FAIL core synthetic three-member additional member metadata mismatch");
  }
  if (result->image_member_count() != 3 || !result->has_additional_images()) {
    return fail_with_cleanup("FAIL core synthetic three-member member-count/additional-image contract mismatch");
  }
  const auto* m0 = result->image_member_at(0);
  const auto* m1 = result->image_member_at(1);
  const auto* m2 = result->image_member_at(2);
  const auto* m3 = result->image_member_at(3);
  if (!m0 || !m1 || !m2 || m3 != nullptr) {
    return fail_with_cleanup("FAIL core synthetic three-member image_member_at access contract mismatch");
  }
  if (m0->image_member_index != 0 || m1->image_member_index != 1 || m2->image_member_index != 2 ||
      m0->applied_exposure_compensation_milli_ev != 0 ||
      m1->applied_exposure_compensation_milli_ev != -1000 ||
      m2->applied_exposure_compensation_milli_ev != 1000 ||
      !m0->has_realized_exposure_compensation_milli_ev ||
      !m1->has_realized_exposure_compensation_milli_ev ||
      !m2->has_realized_exposure_compensation_milli_ev ||
      m0->realized_exposure_compensation_milli_ev != m0->applied_exposure_compensation_milli_ev ||
      m1->realized_exposure_compensation_milli_ev != m1->applied_exposure_compensation_milli_ev ||
      m2->realized_exposure_compensation_milli_ev != m2->applied_exposure_compensation_milli_ev) {
    return fail_with_cleanup("FAIL core synthetic three-member retained member index/ev mismatch");
  }

  const auto& d = result->default_image.payload;
  const auto& b1 = result->additional_images[0].payload;
  const auto& b2 = result->additional_images[1].payload;
  if (d.empty() || b1.empty() || b2.empty()) {
    return fail_with_cleanup("FAIL core synthetic three-member expected non-empty payloads");
  }
  if (!(d.size_bytes() == b1.size_bytes() && b1.size_bytes() == b2.size_bytes())) {
    return fail_with_cleanup("FAIL core synthetic three-member expected equal-sized payloads");
  }
  const uint64_t h0 = fnv1a64_hash_bytes(d.data(), d.size_bytes());
  const uint64_t h1 = fnv1a64_hash_bytes(b1.data(), b1.size_bytes());
  const uint64_t h2 = fnv1a64_hash_bytes(b2.data(), b2.size_bytes());
  if (h0 == h1 || h0 == h2 || h1 == h2) {
    return fail_with_cleanup("FAIL core synthetic three-member expected all payload hashes to differ");
  }
  // NOTE: CamBANGCaptureResult wrapper/member-access behavior is intentionally
  // verified in Godot/GDE-specific validation surfaces; this provider smoke
  // remains core/provider-only and must not depend on godot-cpp headers.

  rt.stop();
  rt.attach_provider(nullptr);
  return true;
}

bool run_core_synthetic_three_member_capture_result_realized_ev_mismatch_check() {
  // First, verify callback metadata divergence on direct provider callbacks.
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  cfg.verification_realized_exposure_compensation_override_by_member_index[2u] = 750;
  SyntheticProvider provider(cfg);
  if (!provider.initialize(&cb).ok() ||
      !provider.open_device("synthetic:0", 6500, 65001).ok()) {
    std::cerr << "FAIL core synthetic mismatch callback provider init/open failed\n";
    return false;
  }
  CaptureRequest cb_req = make_direct_provider_multi_member_still_capture_request(
      9700, 6500, 64, 64, FOURCC_RGBA, {-1000, +1000});
  if (!provider.trigger_capture(cb_req).ok()) {
    std::cerr << "FAIL core synthetic mismatch callback trigger failed\n";
    return false;
  }
  if (!wait_for_capture_completed_with_frames(cb, cb_req.capture_id, 3)) {
    std::cerr << "FAIL core synthetic mismatch callback completion/frame evidence missing\n";
    (void)provider.shutdown();
    return false;
  }
  if (!provider.close_device(6500).ok() || !provider.shutdown().ok()) {
    std::cerr << "FAIL core synthetic mismatch callback teardown failed\n";
    return false;
  }
  const auto callback_events = cb.snapshot_events();
  std::vector<EventRec> cap_frames;
  for (const auto& e : callback_events) {
    if (e.tag == "frame" && e.capture_id == cb_req.capture_id) {
      cap_frames.push_back(e);
    }
  }
  if (cap_frames.size() != 3) {
    std::cerr << "FAIL core synthetic mismatch callback expected three frame members\n";
    return false;
  }
  std::sort(cap_frames.begin(), cap_frames.end(), [](const EventRec& a, const EventRec& b) {
    return a.capture_image_member_index < b.capture_image_member_index;
  });
  for (size_t i = 0; i < cap_frames.size(); ++i) {
    const auto& frame = cap_frames[i];
    if (frame.capture_image_member_index != i ||
        !frame.capture_image_has_realized_exposure_compensation_milli_ev) {
      std::cerr << "FAIL core synthetic mismatch callback member index/realized-presence mismatch\n";
      return false;
    }
  }
  if (cap_frames[0].capture_image_applied_exposure_compensation_milli_ev != 0 ||
      cap_frames[0].capture_image_realized_exposure_compensation_milli_ev != 0 ||
      cap_frames[1].capture_image_applied_exposure_compensation_milli_ev != -1000 ||
      cap_frames[1].capture_image_realized_exposure_compensation_milli_ev != -1000 ||
      cap_frames[2].capture_image_applied_exposure_compensation_milli_ev != 1000 ||
      cap_frames[2].capture_image_realized_exposure_compensation_milli_ev != 750 ||
      cap_frames[2].capture_image_realized_exposure_compensation_milli_ev ==
          cap_frames[2].capture_image_applied_exposure_compensation_milli_ev) {
    std::cerr << "FAIL core synthetic mismatch callback member EV truth mismatch\n";
    return false;
  }

  // Then verify retained/Core result metadata divergence with normal shape/order.
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "FAIL core synthetic mismatch runtime start failed\n";
    return false;
  }
  if (!wait_for_core_runtime_live(rt)) {
    std::cerr << "FAIL core synthetic mismatch runtime did not reach LIVE\n";
    rt.stop();
    return false;
  }
  SyntheticProvider core_provider(cfg);
  const auto fail_with_cleanup = [&](const char* msg) -> bool {
    std::cerr << msg << "\n";
    rt.stop();
    rt.attach_provider(nullptr);
    return false;
  };
  if (!core_provider.initialize(rt.provider_callbacks()).ok()) {
    return fail_with_cleanup("FAIL core synthetic mismatch provider init failed");
  }
  rt.attach_provider(&core_provider);
  std::vector<CameraEndpoint> eps;
  if (!core_provider.enumerate_endpoints(eps).ok() || eps.empty()) {
    return fail_with_cleanup("FAIL core synthetic mismatch enumerate failed");
  }

  const uint64_t device_id = 65;
  if (!core_provider.open_device(eps[0].hardware_id, device_id, 6501).ok()) {
    return fail_with_cleanup("FAIL core synthetic mismatch open_device failed");
  }

  CaptureRequest req{};
  for (int i = 0; i < 50; ++i) {
    if (rt.materialize_capture_request(device_id, req)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (req.device_instance_id != device_id) {
    return fail_with_cleanup("FAIL core synthetic mismatch materialize request failed");
  }

  req.capture_id = 9701;
  req.still_image_bundle = make_default_metered_still_image_bundle();
  req.still_image_bundle.members.push_back(
      CaptureStillImageMember{1u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, -1000});
  req.still_image_bundle.members.push_back(
      CaptureStillImageMember{2u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, +1000});
  if (!core_provider.trigger_capture(req).ok()) {
    return fail_with_cleanup("FAIL core synthetic mismatch trigger_capture failed");
  }

  SharedCaptureResultData result;
  for (int i = 0; i < 50; ++i) {
    result = rt.get_capture_result(req.capture_id, device_id);
    if (result) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!result) {
    return fail_with_cleanup("FAIL core synthetic mismatch result missing");
  }
  if (result->image_member_count() != 3 || !result->has_additional_images()) {
    return fail_with_cleanup("FAIL core synthetic mismatch member-count/additional-image contract mismatch");
  }
  const auto* m0 = result->image_member_at(0);
  const auto* m1 = result->image_member_at(1);
  const auto* m2 = result->image_member_at(2);
  if (!m0 || !m1 || !m2) {
    return fail_with_cleanup("FAIL core synthetic mismatch retained members missing");
  }
  if (m0->applied_exposure_compensation_milli_ev != 0 ||
      !m0->has_realized_exposure_compensation_milli_ev ||
      m0->realized_exposure_compensation_milli_ev != m0->applied_exposure_compensation_milli_ev ||
      m1->applied_exposure_compensation_milli_ev != -1000 ||
      !m1->has_realized_exposure_compensation_milli_ev ||
      m1->realized_exposure_compensation_milli_ev != m1->applied_exposure_compensation_milli_ev ||
      m2->applied_exposure_compensation_milli_ev != 1000 ||
      !m2->has_realized_exposure_compensation_milli_ev ||
      m2->realized_exposure_compensation_milli_ev != 750 ||
      m2->realized_exposure_compensation_milli_ev == m2->applied_exposure_compensation_milli_ev) {
    return fail_with_cleanup("FAIL core synthetic mismatch retained member EV truth mismatch");
  }
  if (result->default_image.payload.empty() ||
      result->additional_images[0].payload.empty() ||
      result->additional_images[1].payload.empty()) {
    return fail_with_cleanup("FAIL core synthetic mismatch expected non-empty payloads");
  }

  rt.stop();
  rt.attach_provider(nullptr);
  return true;
}

bool run_synthetic_still_bundle_capability_gate_contract_check() {
  const CaptureStillImageBundle default_only = make_default_metered_still_image_bundle();
  if (!is_valid_capture_still_image_bundle(default_only, false)) {
    std::cerr << "FAIL still bundle capability gate: default-only bundle must remain valid when multi-image unsupported\n";
    return false;
  }

  CaptureStillImageBundle multi = make_default_metered_still_image_bundle();
  multi.members.push_back(CaptureStillImageMember{
      1u,
      CaptureStillImageMemberRole::ADDITIONAL_BRACKET,
      -1000});
  if (is_valid_capture_still_image_bundle(multi, false)) {
    std::cerr << "FAIL still bundle capability gate: multi-member bundle must be rejected when multi-image unsupported\n";
    return false;
  }
  if (!is_valid_capture_still_image_bundle(multi, true)) {
    std::cerr << "FAIL still bundle capability gate: multi-member bundle should be valid when multi-image supported\n";
    return false;
  }
  return true;
}

bool run_core_synthetic_three_member_realized_unknown_propagation_check() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "FAIL core synthetic realized-unknown runtime start failed\n";
    return false;
  }
  if (!wait_for_core_runtime_live(rt)) {
    std::cerr << "FAIL core synthetic realized-unknown runtime did not reach LIVE\n";
    rt.stop();
    return false;
  }
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  cfg.verification_has_realized_exposure_compensation_override_by_member_index[2u] = false;
  SyntheticProvider provider(cfg);
  const auto fail_with_cleanup = [&](const char* msg) -> bool {
    std::cerr << msg << "\n";
    rt.stop();
    rt.attach_provider(nullptr);
    return false;
  };
  if (!provider.initialize(rt.provider_callbacks()).ok()) {
    return fail_with_cleanup("FAIL core synthetic realized-unknown provider init failed");
  }
  rt.attach_provider(&provider);
  std::vector<CameraEndpoint> eps;
  if (!provider.enumerate_endpoints(eps).ok() || eps.empty()) {
    return fail_with_cleanup("FAIL core synthetic realized-unknown enumerate failed");
  }
  const uint64_t device_id = 66;
  if (!provider.open_device(eps[0].hardware_id, device_id, 6601).ok()) {
    return fail_with_cleanup("FAIL core synthetic realized-unknown open_device failed");
  }

  CaptureRequest req{};
  for (int i = 0; i < 50; ++i) {
    if (rt.materialize_capture_request(device_id, req)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (req.device_instance_id != device_id) {
    return fail_with_cleanup("FAIL core synthetic realized-unknown materialize request failed");
  }

  req.capture_id = 9702;
  req.still_image_bundle = make_default_metered_still_image_bundle();
  req.still_image_bundle.members.push_back(
      CaptureStillImageMember{1u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, -1000});
  req.still_image_bundle.members.push_back(
      CaptureStillImageMember{2u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, +1000});
  if (!provider.trigger_capture(req).ok()) {
    return fail_with_cleanup("FAIL core synthetic realized-unknown trigger_capture failed");
  }

  SharedCaptureResultData result;
  for (int i = 0; i < 50; ++i) {
    result = rt.get_capture_result(req.capture_id, device_id);
    if (result) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!result || result->image_member_count() != 3 || !result->has_additional_images()) {
    return fail_with_cleanup("FAIL core synthetic realized-unknown retained result missing/shape mismatch");
  }
  const auto* m0 = result->image_member_at(0);
  const auto* m1 = result->image_member_at(1);
  const auto* m2 = result->image_member_at(2);
  if (!m0 || !m1 || !m2) {
    return fail_with_cleanup("FAIL core synthetic realized-unknown retained members missing");
  }
  if (!m0->has_realized_exposure_compensation_milli_ev ||
      !m1->has_realized_exposure_compensation_milli_ev ||
      m0->realized_exposure_compensation_milli_ev != m0->applied_exposure_compensation_milli_ev ||
      m1->realized_exposure_compensation_milli_ev != m1->applied_exposure_compensation_milli_ev) {
    return fail_with_cleanup("FAIL core synthetic realized-unknown expected realized-known for members 0/1");
  }
  if (m2->has_realized_exposure_compensation_milli_ev) {
    return fail_with_cleanup("FAIL core synthetic realized-unknown expected has_realized=false for member 2");
  }

  rt.stop();
  rt.attach_provider(nullptr);
  return true;
}

bool run_synthetic_stream_plus_still_single_session_truth_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider provider(cfg);

  StreamRequest req{};
  req.stream_id = 72;
  req.device_instance_id = 42;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 64;
  req.profile.height = 64;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;

  CaptureRequest cap = make_direct_provider_default_still_capture_request(
      8002, req.device_instance_id, 64, 64, FOURCC_RGBA);

  if (!provider.initialize(&cb).ok() ||
      !provider.open_device("synthetic:0", req.device_instance_id, 4201).ok() ||
      !provider.create_stream(req).ok() ||
      !provider.start_stream(req.stream_id, req.profile, req.picture).ok() ||
      !provider.trigger_capture(cap).ok()) {
    std::cerr << "FAIL synthetic stream+still setup/trigger failed\n";
    return false;
  }
  if (!wait_for_capture_completed_with_frames(cb, cap.capture_id, 1)) {
    std::cerr << "FAIL synthetic stream+still capture did not complete with frame evidence before teardown\n";
    return false;
  }
  if (!provider.stop_stream(req.stream_id).ok() ||
      !provider.destroy_stream(req.stream_id).ok() ||
      !provider.close_device(req.device_instance_id).ok() ||
      !provider.shutdown().ok()) {
    std::cerr << "FAIL synthetic stream+still teardown failed\n";
    return false;
  }

  const auto cb_events = cb.snapshot_events();
  const int capture_started_ix = find_event_index(cb_events, "capture_started", cap.capture_id);
  const int capture_completed_ix = find_event_index(cb_events, "capture_completed", cap.capture_id);
  const int stream_destroy_ix = find_event_index(cb_events, "stream_destroyed", req.stream_id);
  const int acq_native_id =
      find_native_create_id_by_type(cb_events, static_cast<uint32_t>(NativeObjectType::AcquisitionSession));
  const int acq_create_ix = find_event_index(cb_events, "native_created", static_cast<uint64_t>(acq_native_id));
  const int acq_destroy_ix = find_event_index(cb_events, "native_destroyed", static_cast<uint64_t>(acq_native_id));
  const int acq_create_count = count_events_by_tag_and_type(
      cb_events, "native_created", static_cast<uint32_t>(NativeObjectType::AcquisitionSession));

  if (capture_started_ix < 0 || capture_completed_ix < 0 || stream_destroy_ix < 0 ||
      acq_native_id < 0 || acq_create_ix < 0 || acq_destroy_ix < 0) {
    std::cerr << "FAIL synthetic stream+still missing evidence\n";
    return false;
  }
  if (acq_create_count != 1) {
    std::cerr << "FAIL synthetic stream+still realized multiple acquisition sessions for one device\n";
    return false;
  }
  if (!(acq_create_ix < capture_started_ix &&
      capture_started_ix < capture_completed_ix &&
      capture_completed_ix < acq_destroy_ix)) {
    std::cerr << "FAIL synthetic stream+still ordering invalid\n";
    return false;
  }
  return assert_native_balance(cb_events, "synthetic_stream_plus_still");
}

bool run_core_measured_backing_plan_evaluation_check() {
  auto plan_equals = [](CoreRetainedProductionPlan plan,
                        CoreProductionPostureShape posture) {
    return plan.valid && plan.posture == posture;
  };
  auto report_entry_for_posture =
      [](const CoreBackingPlanEvaluationReport& report,
         CoreProductionPostureShape posture,
         CoreBackingPlanCandidateEvidenceReport& out) {
        for (const auto& entry : report.candidate_evidence) {
          if (entry.candidate.valid && entry.candidate.posture == posture) {
            out = entry;
            return true;
          }
        }
        return false;
      };
  auto find_capture_report = [](const CoreRuntime& rt,
                                uint64_t device_instance_id,
                                CoreBackingPlanEvaluationReport& out) {
    const auto reports = rt.backing_plan_evaluation_reports();
    for (const auto& report : reports) {
      if (report.target_kind != CoreBackingPlanEvaluationReport::TargetKind::Capture ||
          report.target_id != device_instance_id) {
        continue;
      }
      out = report;
      return true;
    }
    return false;
  };
  auto wait_until = [](const std::function<bool()>& predicate) {
    for (int i = 0; i < kMaxIters; ++i) {
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
    }
    return false;
  };

  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "FAIL core measured backing-plan evaluation runtime start failed\n";
    return false;
  }
  if (!wait_for_core_runtime_live(rt)) {
    std::cerr << "FAIL core measured backing-plan evaluation runtime did not reach LIVE\n";
    rt.stop();
    return false;
  }

  BackingPlanEvaluationTestProvider provider;
  const auto fail_with_cleanup = [&](const char* msg) -> bool {
    std::cerr << msg << "\n";
    (void)provider.shutdown();
    rt.stop();
    rt.attach_provider(nullptr);
    return false;
  };

  if (!provider.initialize(rt.provider_callbacks()).ok()) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation provider init failed");
  }
  rt.attach_provider(&provider);

  constexpr uint64_t kDeviceId = 71;
  constexpr uint64_t kRootId = 7101;
  constexpr uint64_t kReopenedDeviceId = 72;
  constexpr uint64_t kReopenedRootId = 7202;
  constexpr uint64_t kStreamId = 7201;
  constexpr uint64_t kCaptureEvalStreamId = 7203;
  if (rt.try_open_device("backing_plan_eval:0", kDeviceId, kRootId) !=
      TryOpenDeviceStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation open_device failed");
  }
  if (!wait_until([&]() {
        const auto* rec = rt.device_record(kDeviceId);
        return rec &&
               plan_equals(rec->requested_retained_plan,
                           CoreProductionPostureShape::CpuPrimary) &&
               !rec->steady_retained_plan.valid;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation default capture parent initial requested/steady mismatch");
  }

  if (rt.try_create_stream(
          kStreamId, kDeviceId, StreamIntent::PREVIEW, nullptr, nullptr, 0) !=
      TryCreateStreamStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation create_stream failed");
  }
  if (!wait_until([&]() {
        const auto* rec = rt.stream_record(kStreamId);
        return rec &&
               plan_equals(rec->requested_retained_plan,
                           CoreProductionPostureShape::GpuPrimaryNoCpuSidecar) &&
               !rec->steady_retained_plan.valid &&
               plan_equals(
                   provider.stream_requested_plan(kStreamId),
                   CoreProductionPostureShape::GpuPrimaryNoCpuSidecar);
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation stream initial requested/steady mismatch");
  }

  if (rt.try_start_stream(kStreamId) != TryStartStreamStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation start_stream failed");
  }
  if (!provider.emit_stream_frame(kStreamId, true)) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation first stream frame emit failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_latest_stream_result(kStreamId);
        return result &&
               result->payload_kind == ResultPayloadKind::GPU_SURFACE &&
               !result->access_posture.has_retained_cpu_payload;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation first posture result missing");
  }

  const SharedStreamResultData first_stream_result =
      rt.get_latest_stream_result(kStreamId);
  rt.report_stream_retained_display_view_observation(
      kStreamId,
      first_stream_result->access_posture.posture_id,
      first_stream_result->retained_access_truth.display_view,
      true,
      80);
  if (!wait_until([&]() {
        const auto* rec = rt.stream_record(kStreamId);
        return rec &&
               plan_equals(rec->requested_retained_plan,
                           CoreProductionPostureShape::GpuPrimaryWithCpuSidecar) &&
               !rec->steady_retained_plan.valid &&
               provider.stream_plan_update_count(kStreamId) >= 1 &&
               plan_equals(
                   provider.stream_requested_plan(kStreamId),
                   CoreProductionPostureShape::GpuPrimaryWithCpuSidecar);
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation did not advance stream candidate");
  }

  if (!provider.emit_stream_frame(kStreamId, true)) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation second stream frame emit failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_latest_stream_result(kStreamId);
        return result &&
               result->payload_kind == ResultPayloadKind::GPU_SURFACE &&
               result->access_posture.has_retained_cpu_payload;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation sidecar result missing");
  }

  const SharedStreamResultData second_stream_result =
      rt.get_latest_stream_result(kStreamId);
  rt.report_stream_retained_display_view_observation(
      kStreamId,
      second_stream_result->access_posture.posture_id,
      second_stream_result->retained_access_truth.display_view,
      true,
      90);
  if (!wait_until([&]() {
        const auto* rec = rt.stream_record(kStreamId);
        return rec &&
               plan_equals(rec->requested_retained_plan,
                           CoreProductionPostureShape::CpuPrimary) &&
               !rec->steady_retained_plan.valid &&
               provider.stream_plan_update_count(kStreamId) >= 2 &&
               plan_equals(
                   provider.stream_requested_plan(kStreamId),
                   CoreProductionPostureShape::CpuPrimary);
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation did not advance stream to cpu candidate");
  }

  if (!provider.emit_stream_frame(kStreamId, true)) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation third stream frame emit failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_latest_stream_result(kStreamId);
        return result &&
               result->payload_kind == ResultPayloadKind::CPU_PACKED &&
               !result->access_posture.has_retained_gpu_backing;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation cpu stream result missing");
  }

  const SharedStreamResultData third_stream_result =
      rt.get_latest_stream_result(kStreamId);
  rt.report_stream_retained_display_view_observation(
      kStreamId,
      third_stream_result->access_posture.posture_id,
      third_stream_result->retained_access_truth.display_view,
      true,
      120);
  if (!wait_until([&]() {
        const auto* rec = rt.stream_record(kStreamId);
        return rec &&
               plan_equals(rec->requested_retained_plan,
                           CoreProductionPostureShape::GpuPrimaryNoCpuSidecar) &&
               plan_equals(rec->steady_retained_plan,
                           CoreProductionPostureShape::GpuPrimaryNoCpuSidecar) &&
               provider.stream_plan_update_count(kStreamId) >= 3 &&
               plan_equals(
                   provider.stream_requested_plan(kStreamId),
                   CoreProductionPostureShape::GpuPrimaryNoCpuSidecar);
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation did not settle stream steady posture");
  }

  CaptureRequest independent_capture_req{};
  if (!wait_until([&]() {
        return rt.materialize_capture_request(kDeviceId, independent_capture_req) &&
               plan_equals(independent_capture_req.requested_retained_plan,
                           CoreProductionPostureShape::CpuPrimary);
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation capture parent did not remain independent of stream steady posture");
  }

  PictureConfig stream_picture = provider.stream_template().picture;
  stream_picture.seed = 77;
  if (rt.try_set_stream_picture_config(kStreamId, stream_picture) !=
      TrySetStreamPictureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation stream reset failed");
  }
  if (!wait_until([&]() {
        const auto* rec = rt.stream_record(kStreamId);
        return rec &&
               plan_equals(rec->requested_retained_plan,
                           CoreProductionPostureShape::GpuPrimaryNoCpuSidecar) &&
               !rec->steady_retained_plan.valid;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation stream rerun/reset mismatch");
  }

  CaptureRequest reset_independent_capture_req{};
  if (!wait_until([&]() {
        return rt.materialize_capture_request(kDeviceId, reset_independent_capture_req) &&
               plan_equals(reset_independent_capture_req.requested_retained_plan,
                           CoreProductionPostureShape::CpuPrimary);
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation capture parent drifted during stream rerun");
  }

  if (rt.try_stop_stream(kStreamId) != TryStopStreamStatus::OK ||
      rt.try_destroy_stream(kStreamId) != TryDestroyStreamStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation stream teardown before independent capture-parent reset failed");
  }

  CaptureRequest reopened_capture_req{};
  if (!wait_until([&]() {
        const auto* rec = rt.device_record(kDeviceId);
        return rec &&
               rt.materialize_capture_request(kDeviceId, reopened_capture_req) &&
               plan_equals(reopened_capture_req.requested_retained_plan,
                           CoreProductionPostureShape::CpuPrimary) &&
               !rec->steady_retained_plan.valid;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation did not preserve independent capture parent after stream teardown");
  }

  PictureConfig capture_picture = provider.capture_template().picture;
  capture_picture.seed = 88;
  if (rt.try_set_capture_picture_config(kDeviceId, capture_picture) !=
      TrySetCapturePictureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation capture reset failed");
  }
  if (!wait_until([&]() {
        const auto* rec = rt.device_record(kDeviceId);
        return rec &&
               plan_equals(rec->requested_retained_plan,
                           CoreProductionPostureShape::CpuPrimary) &&
               !rec->steady_retained_plan.valid;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation capture initial requested/steady mismatch");
  }
  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        return find_capture_report(rt, kDeviceId, report) &&
               report.parent_kind ==
                   CoreBackingPlanEvaluationReport::ParentKind::AcquisitionSession &&
               report.acquisition_session_id != 0 &&
               !report.provisional_parent &&
               plan_equals(report.requested,
                           CoreProductionPostureShape::CpuPrimary) &&
               !report.steady.valid &&
               provider.has_active_capture_parent_priming(kDeviceId) &&
               provider.active_capture_parent_session_id(kDeviceId) ==
                   report.acquisition_session_id;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation did not realize first-use still-only primed acquisition-session parent");
  }

  CaptureRequest capture_req{};
  if (!wait_until([&]() {
        return rt.materialize_capture_request(kDeviceId, capture_req) &&
               plan_equals(capture_req.requested_retained_plan,
                           CoreProductionPostureShape::CpuPrimary);
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation materialized capture request mismatch");
  }

  constexpr uint64_t kCaptureIdA = 8801;
  if (rt.try_trigger_device_capture_with_capture_id_for_server(
          kDeviceId, kCaptureIdA) != TryTriggerDeviceCaptureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation first trigger_capture failed");
  }
  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        return find_capture_report(rt, kDeviceId, report) &&
               report.parent_kind ==
                   CoreBackingPlanEvaluationReport::ParentKind::AcquisitionSession &&
               report.acquisition_session_id != 0 &&
               plan_equals(report.requested,
                           CoreProductionPostureShape::CpuPrimary) &&
               !report.steady.valid &&
               report.evaluator_active &&
               report.current_candidate_index == 0;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation did not mirror active capture evaluation onto real acquisition-session parent");
  }
  if (!provider.emit_pending_capture(kCaptureIdA)) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation first pending capture publish failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_capture_result(kCaptureIdA, kDeviceId);
        return result &&
               result->default_image.payload_kind == ResultPayloadKind::CPU_PACKED &&
               !result->default_image.access_posture.has_retained_gpu_backing;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation first capture result missing");
  }

  const SharedCaptureResultData first_capture_result =
      rt.get_capture_result(kCaptureIdA, kDeviceId);
  rt.report_capture_retained_to_image_observation(
      kDeviceId,
      kCaptureIdA,
      first_capture_result->acquisition_session_id,
      first_capture_result->default_image.access_posture.posture_id,
      first_capture_result->default_image.retained_access_truth.to_image,
      true,
      80'000'000,
      true,
      80'000'000);
  if (!wait_until([&]() {
        const auto* rec = rt.device_record(kDeviceId);
        return rec &&
               plan_equals(rec->requested_retained_plan,
                           CoreProductionPostureShape::GpuPrimaryNoCpuSidecar) &&
               !rec->steady_retained_plan.valid;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation did not advance capture candidate");
  }
  const uint64_t capture_phase_release_count_before_stream_start =
      provider.capture_parent_priming_release_count(kDeviceId);
  if (rt.try_create_stream(
          kCaptureEvalStreamId,
          kDeviceId,
          StreamIntent::PREVIEW,
          nullptr,
          nullptr,
          0) != TryCreateStreamStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation capture-phase create_stream failed");
  }
  if (rt.try_start_stream(kCaptureEvalStreamId) != TryStartStreamStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation capture-phase start_stream failed");
  }
  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        return find_capture_report(rt, kDeviceId, report) &&
               report.parent_kind ==
                   CoreBackingPlanEvaluationReport::ParentKind::AcquisitionSession &&
               report.acquisition_session_id ==
                   first_capture_result->acquisition_session_id &&
               report.evaluator_active &&
               report.current_candidate_index == 1 &&
               plan_equals(
                   report.requested,
                   CoreProductionPostureShape::GpuPrimaryNoCpuSidecar) &&
               provider.has_active_capture_parent_priming(kDeviceId) &&
               provider.active_capture_parent_session_id(kDeviceId) ==
                   first_capture_result->acquisition_session_id &&
               provider.capture_parent_priming_release_count(kDeviceId) ==
                   capture_phase_release_count_before_stream_start;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation stream start released priming before capture evaluation completion");
  }

  CaptureRequest capture_req_no_sidecar{};
  if (!wait_until([&]() {
        return rt.materialize_capture_request(kDeviceId, capture_req_no_sidecar) &&
               plan_equals(capture_req_no_sidecar.requested_retained_plan,
                           CoreProductionPostureShape::GpuPrimaryNoCpuSidecar);
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation no-sidecar capture request mismatch");
  }

  constexpr uint64_t kCaptureIdB = 8802;
  if (rt.try_trigger_device_capture_with_capture_id_for_server(
          kDeviceId, kCaptureIdB) != TryTriggerDeviceCaptureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation second trigger_capture failed");
  }
  if (!provider.emit_pending_capture(kCaptureIdB)) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation second pending capture publish failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_capture_result(kCaptureIdB, kDeviceId);
        return result &&
               result->default_image.payload_kind == ResultPayloadKind::GPU_SURFACE &&
               !result->default_image.access_posture.has_retained_cpu_payload;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation second capture result missing");
  }

  const SharedCaptureResultData second_capture_result =
      rt.get_capture_result(kCaptureIdB, kDeviceId);
  rt.report_capture_retained_to_image_observation(
      kDeviceId,
      kCaptureIdB,
      second_capture_result->acquisition_session_id,
      second_capture_result->default_image.access_posture.posture_id,
      second_capture_result->default_image.retained_access_truth.to_image,
      true,
      80'000'000,
      true,
      80'000'000);
  if (!wait_until([&]() {
        const auto* rec = rt.device_record(kDeviceId);
        return rec &&
               plan_equals(rec->requested_retained_plan,
                           CoreProductionPostureShape::GpuPrimaryWithCpuSidecar) &&
               !rec->steady_retained_plan.valid;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation did not advance capture to sidecar candidate");
  }

  CaptureRequest capture_req_sidecar{};
  if (!wait_until([&]() {
        return rt.materialize_capture_request(kDeviceId, capture_req_sidecar) &&
               plan_equals(capture_req_sidecar.requested_retained_plan,
                           CoreProductionPostureShape::GpuPrimaryWithCpuSidecar);
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation sidecar capture request mismatch");
  }

  constexpr uint64_t kCaptureIdC = 8803;
  if (rt.try_trigger_device_capture_with_capture_id_for_server(
          kDeviceId, kCaptureIdC) != TryTriggerDeviceCaptureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation third trigger_capture failed");
  }
  if (!provider.emit_pending_capture(kCaptureIdC)) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation third pending capture publish failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_capture_result(kCaptureIdC, kDeviceId);
        return result &&
               result->default_image.payload_kind == ResultPayloadKind::GPU_SURFACE &&
               result->default_image.access_posture.has_retained_cpu_payload;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation third capture result missing");
  }

  const SharedCaptureResultData third_capture_result =
      rt.get_capture_result(kCaptureIdC, kDeviceId);
  rt.report_capture_retained_to_image_observation(
      kDeviceId,
      kCaptureIdC,
      third_capture_result->acquisition_session_id,
      third_capture_result->default_image.access_posture.posture_id,
      third_capture_result->default_image.retained_access_truth.to_image,
      true,
      40'000'000,
      true,
      40'000'000);
  if (!wait_until([&]() {
        const auto* rec = rt.device_record(kDeviceId);
        return rec &&
               plan_equals(rec->requested_retained_plan,
                           CoreProductionPostureShape::GpuPrimaryWithCpuSidecar) &&
               plan_equals(rec->steady_retained_plan,
                           CoreProductionPostureShape::GpuPrimaryWithCpuSidecar);
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation did not settle capture steady posture");
  }
  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        return find_capture_report(rt, kDeviceId, report) &&
               report.parent_kind ==
                   CoreBackingPlanEvaluationReport::ParentKind::AcquisitionSession &&
               !report.provisional_parent &&
               plan_equals(report.requested,
                           CoreProductionPostureShape::GpuPrimaryWithCpuSidecar) &&
               plan_equals(report.steady,
                           CoreProductionPostureShape::GpuPrimaryWithCpuSidecar) &&
               report.decision_from_evaluation &&
               plan_equals(report.decision_selected,
                           CoreProductionPostureShape::GpuPrimaryWithCpuSidecar) &&
               report.completion_reason ==
                   BackingPlanEvaluationCompletionReason::
                       AllViableCandidatesEvaluated &&
               report.candidate_evidence.size() == 3u &&
               !provider.has_active_capture_parent_priming(kDeviceId) &&
               provider.capture_parent_priming_release_count(kDeviceId) ==
                   capture_phase_release_count_before_stream_start + 1u;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation did not preserve settled capture decision when acquisition-session parent retired");
  }
  {
    CoreBackingPlanEvaluationReport report{};
    if (!find_capture_report(rt, kDeviceId, report)) {
      return fail_with_cleanup(
          "FAIL core measured backing-plan evaluation final capture report missing");
    }
    CoreBackingPlanCandidateEvidenceReport cpu_entry{};
    CoreBackingPlanCandidateEvidenceReport gpu_entry{};
    CoreBackingPlanCandidateEvidenceReport sidecar_entry{};
    if (!report_entry_for_posture(
            report, CoreProductionPostureShape::CpuPrimary, cpu_entry) ||
        !report_entry_for_posture(
            report, CoreProductionPostureShape::GpuPrimaryNoCpuSidecar,
            gpu_entry) ||
        !report_entry_for_posture(
            report, CoreProductionPostureShape::GpuPrimaryWithCpuSidecar,
            sidecar_entry)) {
      return fail_with_cleanup(
          "FAIL core measured backing-plan evaluation final capture report missing candidate evidence");
    }
    if (!cpu_entry.evidence_accepted || !cpu_entry.evidence_complete ||
        !gpu_entry.evidence_accepted || !gpu_entry.evidence_complete ||
        !sidecar_entry.evidence_accepted || !sidecar_entry.evidence_complete ||
        !cpu_entry.has_total_elapsed_ns || !gpu_entry.has_total_elapsed_ns ||
        !sidecar_entry.has_total_elapsed_ns ||
        cpu_entry.observed_acquisition_session_id == 0 ||
        gpu_entry.observed_acquisition_session_id == 0 ||
        sidecar_entry.observed_acquisition_session_id == 0 ||
        cpu_entry.observed_capture_id == 0 ||
        gpu_entry.observed_capture_id == 0 ||
        sidecar_entry.observed_capture_id == 0 ||
        cpu_entry.observed_acquisition_session_id !=
            first_capture_result->acquisition_session_id ||
        gpu_entry.observed_acquisition_session_id !=
            first_capture_result->acquisition_session_id ||
        sidecar_entry.observed_acquisition_session_id !=
            first_capture_result->acquisition_session_id ||
        second_capture_result->acquisition_session_id !=
            first_capture_result->acquisition_session_id ||
        third_capture_result->acquisition_session_id !=
            first_capture_result->acquisition_session_id ||
        sidecar_entry.total_elapsed_ns >= gpu_entry.total_elapsed_ns ||
        sidecar_entry.total_elapsed_ns >= cpu_entry.total_elapsed_ns) {
      return fail_with_cleanup(
          "FAIL core measured backing-plan evaluation final capture evidence provenance/ordering mismatch");
    }
  }

  CaptureRequest settled_capture_req{};
  if (!wait_until([&]() {
        return rt.materialize_capture_request(kDeviceId, settled_capture_req) &&
               plan_equals(settled_capture_req.requested_retained_plan,
                           CoreProductionPostureShape::GpuPrimaryWithCpuSidecar);
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation did not reuse settled capture posture");
  }

  if (rt.try_stop_stream(kCaptureEvalStreamId) != TryStopStreamStatus::OK ||
      rt.try_destroy_stream(kCaptureEvalStreamId) !=
          TryDestroyStreamStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation capture-phase stream teardown failed");
  }

  if (rt.try_close_device(kDeviceId) != TryCloseDeviceStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation teardown failed");
  }
  if (rt.try_open_device("backing_plan_eval:0", kReopenedDeviceId, kReopenedRootId) !=
      TryOpenDeviceStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation reopen_device failed");
  }
  if (rt.try_set_capture_picture_config(kReopenedDeviceId, capture_picture) !=
      TrySetCapturePictureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation reopen capture picture restore failed");
  }

  CaptureRequest seeded_reopen_capture_req{};
  if (!wait_until([&]() {
        const auto* rec = rt.device_record(kReopenedDeviceId);
        CoreBackingPlanEvaluationReport report{};
        return rec &&
               find_capture_report(rt, kReopenedDeviceId, report) &&
               report.parent_kind ==
                   CoreBackingPlanEvaluationReport::ParentKind::AcquisitionSession &&
               report.acquisition_session_id != 0 &&
               !report.provisional_parent &&
               rt.materialize_capture_request(
                   kReopenedDeviceId, seeded_reopen_capture_req) &&
               plan_equals(
                   seeded_reopen_capture_req.requested_retained_plan,
                   CoreProductionPostureShape::GpuPrimaryWithCpuSidecar) &&
               report.candidate_evidence.size() == 3u &&
               !report.candidate_evidence[0].observation_seen &&
               !report.candidate_evidence[0].evidence_accepted &&
               !report.candidate_evidence[1].observation_seen &&
               !report.candidate_evidence[1].evidence_accepted &&
               !report.candidate_evidence[2].observation_seen &&
               !report.candidate_evidence[2].evidence_accepted &&
               !rec->steady_retained_plan.valid;
      })) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation did not seed same-hardware reopen from prior capture winner");
  }
  if (rt.try_close_device(kReopenedDeviceId) != TryCloseDeviceStatus::OK) {
    return fail_with_cleanup(
        "FAIL core measured backing-plan evaluation reopened teardown failed");
  }

  (void)provider.shutdown();
  rt.stop();
  rt.attach_provider(nullptr);
  return true;
}

bool run_core_capture_observation_regression_check() {
  auto plan_equals = [](CoreRetainedProductionPlan plan,
                        CoreProductionPostureShape posture) {
    return plan.valid && plan.posture == posture;
  };
  auto report_entry_for_posture =
      [](const CoreBackingPlanEvaluationReport& report,
         CoreProductionPostureShape posture,
         CoreBackingPlanCandidateEvidenceReport& out) {
        for (const auto& entry : report.candidate_evidence) {
          if (entry.candidate.valid && entry.candidate.posture == posture) {
            out = entry;
            return true;
          }
        }
        return false;
      };
  auto find_capture_report = [](const CoreRuntime& rt,
                                uint64_t device_instance_id,
                                CoreBackingPlanEvaluationReport& out) {
    const auto reports = rt.backing_plan_evaluation_reports();
    for (const auto& report : reports) {
      if (report.target_kind !=
              CoreBackingPlanEvaluationReport::TargetKind::Capture ||
          report.target_id != device_instance_id) {
        continue;
      }
      out = report;
      return true;
    }
    return false;
  };
  auto wait_until = [](const std::function<bool()>& predicate) {
    for (int i = 0; i < kMaxIters; ++i) {
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
    }
    return false;
  };

  auto run_direct_single_candidate_check = [&]() -> bool {
    CoreRuntime rt;
    if (!rt.start()) {
      std::cerr
          << "FAIL core capture observation regression single-candidate runtime start failed\n";
      return false;
    }
    if (!wait_for_core_runtime_live(rt)) {
      std::cerr
          << "FAIL core capture observation regression single-candidate runtime did not reach LIVE\n";
      rt.stop();
      return false;
    }
    BackingPlanEvaluationTestProvider provider;
    auto fail_with_cleanup = [&](const char* message) {
      std::cerr << message << "\n";
      (void)provider.shutdown();
      rt.stop();
      rt.attach_provider(nullptr);
      return false;
    };
    provider.set_capture_capabilities(
        ProducerBackingCapabilities{true, false, false});
    if (!provider.initialize(rt.provider_callbacks()).ok()) {
      return fail_with_cleanup(
          "FAIL core capture observation regression single-candidate provider init failed");
    }
    rt.attach_provider(&provider);
    constexpr uint64_t kDeviceId = 17201;
    constexpr uint64_t kRootId = 17202;
    if (rt.try_open_device("backing_plan_eval:0", kDeviceId, kRootId) !=
        TryOpenDeviceStatus::OK) {
      return fail_with_cleanup(
          "FAIL core capture observation regression single-candidate open_device failed");
    }
    PictureConfig capture_picture = provider.capture_template().picture;
    capture_picture.seed = 212;
    if (rt.try_set_capture_picture_config(kDeviceId, capture_picture) !=
        TrySetCapturePictureStatus::OK) {
      return fail_with_cleanup(
          "FAIL core capture observation regression single-candidate capture picture setup failed");
    }
    if (!wait_until([&]() {
          CoreBackingPlanEvaluationReport report{};
          return find_capture_report(rt, kDeviceId, report) &&
                 !report.evaluator_active &&
                 !report.decision_from_evaluation &&
                 plan_equals(
                     report.requested,
                     CoreProductionPostureShape::CpuPrimary) &&
                 report.completion_reason ==
                     BackingPlanEvaluationCompletionReason::
                         SingleViableCandidate &&
                 report.decision_candidate_sequence.size() == 1u &&
                 plan_equals(
                     report.decision_candidate_sequence[0],
                     CoreProductionPostureShape::CpuPrimary) &&
                 plan_equals(
                     report.decision_selected,
                     CoreProductionPostureShape::CpuPrimary) &&
                 !provider.has_active_capture_parent_priming(kDeviceId) &&
                 provider.capture_parent_priming_sync_count(kDeviceId) != 0 &&
                 provider.capture_parent_priming_release_count(kDeviceId) != 0;
        })) {
      CoreBackingPlanEvaluationReport report{};
      const bool have_report = find_capture_report(rt, kDeviceId, report);
      std::ostringstream oss;
      oss << "FAIL core capture observation regression single-candidate decision was not marked as non-evaluated"
          << " have_report=" << (have_report ? 1 : 0)
          << " parent_kind="
          << (have_report ? static_cast<unsigned>(report.parent_kind) : 0u)
          << " parent_id="
          << (have_report ? static_cast<unsigned long long>(report.parent_id) : 0ull)
          << " acquisition_session_id="
          << (have_report ? static_cast<unsigned long long>(report.acquisition_session_id)
                          : 0ull)
          << " evaluator_active="
          << (have_report && report.evaluator_active ? 1 : 0)
          << " decision_from_evaluation="
          << (have_report && report.decision_from_evaluation ? 1 : 0)
          << " requested_valid="
          << (have_report && report.requested.valid ? 1 : 0)
          << " requested_posture="
          << (have_report ? static_cast<int>(report.requested.posture) : 0)
          << " steady_valid="
          << (have_report && report.steady.valid ? 1 : 0)
          << " steady_posture="
          << (have_report ? static_cast<int>(report.steady.posture) : 0)
          << " completion_reason="
          << (have_report ? static_cast<unsigned>(report.completion_reason) : 0u)
          << " decision_candidate_count="
          << (have_report ? report.decision_candidate_sequence.size() : 0u)
          << " decision_selected_valid="
          << (have_report && report.decision_selected.valid ? 1 : 0)
          << " decision_selected_posture="
          << (have_report ? static_cast<int>(report.decision_selected.posture) : 0)
          << " priming_active="
          << (provider.has_active_capture_parent_priming(kDeviceId) ? 1 : 0)
          << " priming_sync_count="
          << static_cast<unsigned long long>(
                 provider.capture_parent_priming_sync_count(kDeviceId))
          << " priming_release_count="
          << static_cast<unsigned long long>(
                 provider.capture_parent_priming_release_count(kDeviceId));
      return fail_with_cleanup(oss.str().c_str());
    }
    const uint64_t initial_priming_release_count =
        provider.capture_parent_priming_release_count(kDeviceId);
    constexpr uint64_t kCaptureId = 17203;
    if (rt.try_trigger_device_capture_with_capture_id_for_server(
            kDeviceId, kCaptureId) != TryTriggerDeviceCaptureStatus::OK) {
      return fail_with_cleanup(
          "FAIL core capture observation regression single-candidate trigger_capture failed");
    }
    if (!provider.emit_pending_capture(kCaptureId)) {
      return fail_with_cleanup(
          "FAIL core capture observation regression single-candidate pending capture publish failed");
    }
    if (!wait_until([&]() {
          const auto result = rt.get_capture_result(kCaptureId, kDeviceId);
          return result &&
                 result->default_image.payload_kind == ResultPayloadKind::CPU_PACKED &&
                 !result->default_image.access_posture.has_retained_gpu_backing;
        })) {
      return fail_with_cleanup(
          "FAIL core capture observation regression single-candidate capture result missing");
    }
    if (!wait_until([&]() {
          CoreBackingPlanEvaluationReport report{};
          return find_capture_report(rt, kDeviceId, report) &&
                 report.parent_kind ==
                     CoreBackingPlanEvaluationReport::ParentKind::CapturePriming &&
                 report.parent_id == kDeviceId &&
                 report.acquisition_session_id == 0 &&
                 report.provisional_parent &&
                 !report.evaluator_active &&
                 !report.decision_from_evaluation &&
                 plan_equals(
                     report.requested,
                     CoreProductionPostureShape::CpuPrimary) &&
                 plan_equals(
                     report.steady,
                     CoreProductionPostureShape::CpuPrimary) &&
                 report.completion_reason ==
                     BackingPlanEvaluationCompletionReason::
                         SingleViableCandidate &&
                 report.decision_candidate_sequence.size() == 1u &&
                 report.candidate_evidence.size() == 1u &&
                 plan_equals(
                     report.decision_candidate_sequence[0],
                     CoreProductionPostureShape::CpuPrimary) &&
                 plan_equals(
                     report.decision_selected,
                     CoreProductionPostureShape::CpuPrimary) &&
                 !provider.has_active_capture_parent_priming(kDeviceId) &&
                 provider.capture_parent_priming_release_count(kDeviceId) ==
                     initial_priming_release_count;
        })) {
      CoreBackingPlanEvaluationReport report{};
      const bool have_report = find_capture_report(rt, kDeviceId, report);
      std::ostringstream oss;
      oss << "FAIL core capture observation regression single-candidate capture churn discarded the direct priming decision"
          << " have_report=" << (have_report ? 1 : 0)
          << " parent_kind="
          << (have_report ? static_cast<unsigned>(report.parent_kind) : 0u)
          << " parent_id="
          << (have_report ? static_cast<unsigned long long>(report.parent_id) : 0ull)
          << " acquisition_session_id="
          << (have_report ? static_cast<unsigned long long>(report.acquisition_session_id)
                          : 0ull)
          << " provisional_parent="
          << (have_report && report.provisional_parent ? 1 : 0)
          << " evaluator_active="
          << (have_report && report.evaluator_active ? 1 : 0)
          << " decision_from_evaluation="
          << (have_report && report.decision_from_evaluation ? 1 : 0)
          << " requested_valid="
          << (have_report && report.requested.valid ? 1 : 0)
          << " requested_posture="
          << (have_report ? static_cast<int>(report.requested.posture) : 0)
          << " steady_valid="
          << (have_report && report.steady.valid ? 1 : 0)
          << " steady_posture="
          << (have_report ? static_cast<int>(report.steady.posture) : 0)
          << " completion_reason="
          << (have_report ? static_cast<unsigned>(report.completion_reason) : 0u)
          << " decision_candidate_count="
          << (have_report ? report.decision_candidate_sequence.size() : 0u)
          << " candidate_evidence_count="
          << (have_report ? report.candidate_evidence.size() : 0u)
          << " decision_selected_valid="
          << (have_report && report.decision_selected.valid ? 1 : 0)
          << " decision_selected_posture="
          << (have_report ? static_cast<int>(report.decision_selected.posture) : 0)
          << " priming_active="
          << (provider.has_active_capture_parent_priming(kDeviceId) ? 1 : 0)
          << " priming_release_count="
          << static_cast<unsigned long long>(
                 provider.capture_parent_priming_release_count(kDeviceId))
          << " initial_priming_release_count="
          << static_cast<unsigned long long>(initial_priming_release_count);
      return fail_with_cleanup(oss.str().c_str());
    }
    if (provider.active_capture_parent_priming_count() != 0u) {
      return fail_with_cleanup(
          "FAIL core capture observation regression single-candidate path leaked a priming hold");
    }
    (void)provider.shutdown();
    rt.stop();
    rt.attach_provider(nullptr);
    return true;
  };

  auto run_rejected_single_candidate_check = [&]() -> bool {
    CoreRuntime rt;
    if (!rt.start()) {
      std::cerr
          << "FAIL core capture observation regression rejected-path runtime start failed\n";
      return false;
    }
    if (!wait_for_core_runtime_live(rt)) {
      std::cerr
          << "FAIL core capture observation regression rejected-path runtime did not reach LIVE\n";
      rt.stop();
      return false;
    }
    BackingPlanEvaluationTestProvider provider;
    auto fail_with_cleanup = [&](const char* message) {
      std::cerr << message << "\n";
      (void)provider.shutdown();
      rt.stop();
      rt.attach_provider(nullptr);
      return false;
    };
    provider.set_capture_capabilities(
        ProducerBackingCapabilities{false, false, false});
    if (!provider.initialize(rt.provider_callbacks()).ok()) {
      return fail_with_cleanup(
          "FAIL core capture observation regression rejected-path provider init failed");
    }
    rt.attach_provider(&provider);
    constexpr uint64_t kDeviceId = 17211;
    constexpr uint64_t kRootId = 17212;
    if (rt.try_open_device("backing_plan_eval:0", kDeviceId, kRootId) !=
        TryOpenDeviceStatus::OK) {
      return fail_with_cleanup(
          "FAIL core capture observation regression rejected-path open_device failed");
    }
    if (!wait_until([&]() {
          const auto* rec = rt.device_record(kDeviceId);
          return rec &&
                 !rec->requested_retained_plan.valid &&
                 !rec->steady_retained_plan.valid &&
                 !provider.has_active_capture_parent_priming(kDeviceId);
        })) {
      return fail_with_cleanup(
          "FAIL core capture observation regression rejected-path did not release the priming hold");
    }
    if (provider.active_capture_parent_priming_count() != 0u) {
      return fail_with_cleanup(
          "FAIL core capture observation regression rejected-path leaked a priming hold");
    }
    CaptureRequest impossible_req{};
    if (rt.materialize_capture_request(kDeviceId, impossible_req)) {
      return fail_with_cleanup(
          "FAIL core capture observation regression rejected-path unexpectedly materialized a capture request");
    }
    (void)provider.shutdown();
    rt.stop();
    rt.attach_provider(nullptr);
    return true;
  };

  auto run_runtime_stop_release_check = [&]() -> bool {
    CoreRuntime rt;
    if (!rt.start()) {
      std::cerr
          << "FAIL core capture observation regression runtime-stop release start failed\n";
      return false;
    }
    if (!wait_for_core_runtime_live(rt)) {
      std::cerr
          << "FAIL core capture observation regression runtime-stop release did not reach LIVE\n";
      rt.stop();
      return false;
    }
    BackingPlanEvaluationTestProvider provider;
    auto fail_with_cleanup = [&](const char* message) {
      std::cerr << message << "\n";
      rt.stop();
      rt.attach_provider(nullptr);
      return false;
    };
    provider.set_capture_capabilities(
        ProducerBackingCapabilities{true, true, true});
    if (!provider.initialize(rt.provider_callbacks()).ok()) {
      return fail_with_cleanup(
          "FAIL core capture observation regression runtime-stop release provider init failed");
    }
    rt.attach_provider(&provider);
    constexpr uint64_t kDeviceId = 17221;
    constexpr uint64_t kRootId = 17222;
    if (rt.try_open_device("backing_plan_eval:0", kDeviceId, kRootId) !=
        TryOpenDeviceStatus::OK) {
      return fail_with_cleanup(
          "FAIL core capture observation regression runtime-stop release open_device failed");
    }
    if (!wait_until([&]() {
          CoreBackingPlanEvaluationReport report{};
          return find_capture_report(rt, kDeviceId, report) &&
                 report.evaluator_active &&
                 provider.has_active_capture_parent_priming(kDeviceId);
        })) {
      return fail_with_cleanup(
          "FAIL core capture observation regression runtime-stop release did not establish an active primed evaluator");
    }
    rt.stop();
    if (provider.active_capture_parent_priming_count() != 0u ||
        provider.has_active_capture_parent_priming(kDeviceId)) {
      rt.attach_provider(nullptr);
      std::cerr
          << "FAIL core capture observation regression runtime-stop release leaked a priming hold through runtime shutdown\n";
      return false;
    }
    rt.attach_provider(nullptr);
    return true;
  };

  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "FAIL core capture observation regression runtime start failed\n";
    return false;
  }
  if (!wait_for_core_runtime_live(rt)) {
    std::cerr
        << "FAIL core capture observation regression runtime did not reach LIVE\n";
    rt.stop();
    return false;
  }

  BackingPlanEvaluationTestProvider provider;
  auto fail_with_cleanup = [&](const char* message) {
    std::cerr << message << "\n";
    (void)provider.shutdown();
    rt.stop();
    rt.attach_provider(nullptr);
    return false;
  };

  if (!provider.initialize(rt.provider_callbacks()).ok()) {
    return fail_with_cleanup(
        "FAIL core capture observation regression provider init failed");
  }
  rt.attach_provider(&provider);

  constexpr uint64_t kDeviceId = 17101;
  constexpr uint64_t kRootId = 17102;
  constexpr uint64_t kCaptureIdA = 17111;
  constexpr uint64_t kCaptureIdB = 17112;
  constexpr uint64_t kCaptureIdC = 17113;
  if (rt.try_open_device("backing_plan_eval:0", kDeviceId, kRootId) !=
      TryOpenDeviceStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture observation regression open_device failed");
  }
  PictureConfig capture_picture = provider.capture_template().picture;
  capture_picture.seed = 211;
  if (rt.try_set_capture_picture_config(kDeviceId, capture_picture) !=
      TrySetCapturePictureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture observation regression capture picture setup failed");
  }
  if (rt.try_trigger_device_capture_with_capture_id_for_server(
          kDeviceId, kCaptureIdA) != TryTriggerDeviceCaptureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture observation regression trigger_capture A failed");
  }
  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        return find_capture_report(rt, kDeviceId, report) &&
               report.evaluator_active &&
               report.current_candidate_index == 0 &&
               plan_equals(report.requested,
                           CoreProductionPostureShape::CpuPrimary);
      })) {
    return fail_with_cleanup(
        "FAIL core capture observation regression did not realize active cpu capture candidate");
  }
  if (!provider.emit_pending_capture(kCaptureIdA)) {
    return fail_with_cleanup(
        "FAIL core capture observation regression pending capture A publish failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_capture_result(kCaptureIdA, kDeviceId);
        return result &&
               result->default_image.payload_kind ==
                   ResultPayloadKind::CPU_PACKED;
      })) {
    return fail_with_cleanup(
        "FAIL core capture observation regression capture result A missing");
  }
  const SharedCaptureResultData capture_result_a =
      rt.get_capture_result(kCaptureIdA, kDeviceId);
  rt.report_capture_retained_to_image_observation(
      kDeviceId,
      kCaptureIdA,
      capture_result_a->acquisition_session_id,
      capture_result_a->default_image.access_posture.posture_id,
      capture_result_a->default_image.retained_access_truth.to_image,
      true,
      80'000'000,
      true,
      80'000'000);
  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        return find_capture_report(rt, kDeviceId, report) &&
               report.evaluator_active &&
               report.current_candidate_index == 1 &&
               plan_equals(report.requested,
                           CoreProductionPostureShape::
                               GpuPrimaryNoCpuSidecar);
      })) {
    return fail_with_cleanup(
        "FAIL core capture observation regression cpu candidate did not advance after settle");
  }

  rt.report_capture_retained_to_image_observation(
      kDeviceId,
      kCaptureIdA,
      capture_result_a->acquisition_session_id,
      capture_result_a->default_image.access_posture.posture_id,
      capture_result_a->default_image.retained_access_truth.to_image,
      true,
      80'000'000,
      true,
      80'000'000);
  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        CoreBackingPlanCandidateEvidenceReport gpu_entry{};
        return find_capture_report(rt, kDeviceId, report) &&
               report.evaluator_active &&
               report.current_candidate_index == 1 &&
               report_entry_for_posture(
                   report,
                   CoreProductionPostureShape::GpuPrimaryNoCpuSidecar,
                   gpu_entry) &&
               !gpu_entry.observation_seen &&
               !gpu_entry.evidence_accepted;
      })) {
    return fail_with_cleanup(
        "FAIL core capture observation regression stale result advanced or populated the next candidate");
  }

  if (rt.try_trigger_device_capture_with_capture_id_for_server(
          kDeviceId, kCaptureIdB) != TryTriggerDeviceCaptureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture observation regression trigger_capture B failed");
  }
  if (!provider.emit_pending_capture(kCaptureIdB)) {
    return fail_with_cleanup(
        "FAIL core capture observation regression pending capture B publish failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_capture_result(kCaptureIdB, kDeviceId);
        return result &&
               result->default_image.payload_kind ==
                   ResultPayloadKind::GPU_SURFACE &&
               !result->default_image.access_posture.has_retained_cpu_payload;
      })) {
    return fail_with_cleanup(
        "FAIL core capture observation regression capture result B missing");
  }
  const SharedCaptureResultData capture_result_b =
      rt.get_capture_result(kCaptureIdB, kDeviceId);

  if (rt.try_trigger_device_capture_with_capture_id_for_server(
          kDeviceId, kCaptureIdC) != TryTriggerDeviceCaptureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture observation regression trigger_capture C failed");
  }
  if (!provider.emit_pending_capture(kCaptureIdC)) {
    return fail_with_cleanup(
        "FAIL core capture observation regression pending capture C publish failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_capture_result(kCaptureIdC, kDeviceId);
        return result &&
               result->default_image.payload_kind ==
                   ResultPayloadKind::GPU_SURFACE;
      })) {
    return fail_with_cleanup(
        "FAIL core capture observation regression capture result C missing");
  }
  rt.report_capture_retained_to_image_observation(
      kDeviceId,
      kCaptureIdB,
      capture_result_b->acquisition_session_id,
      capture_result_b->default_image.access_posture.posture_id,
      capture_result_b->default_image.retained_access_truth.to_image,
      true,
      60'000'000,
      true,
      60'000'000);
  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        CoreBackingPlanCandidateEvidenceReport gpu_entry{};
        return find_capture_report(rt, kDeviceId, report) &&
               report.evaluator_active &&
               report.current_candidate_index == 1 &&
               report_entry_for_posture(
                   report,
                   CoreProductionPostureShape::GpuPrimaryNoCpuSidecar,
                   gpu_entry) &&
               gpu_entry.observation_seen &&
               gpu_entry.has_materialization_elapsed_ns &&
               !gpu_entry.has_capture_ready_elapsed_ns &&
               !gpu_entry.has_total_elapsed_ns &&
               !gpu_entry.evidence_complete &&
               !gpu_entry.evidence_accepted;
      })) {
    return fail_with_cleanup(
        "FAIL core capture observation regression mismatched readiness was paired with materialization or advanced the candidate");
  }

  (void)provider.shutdown();
  rt.stop();
  rt.attach_provider(nullptr);
  return run_direct_single_candidate_check() &&
         run_rejected_single_candidate_check() &&
         run_runtime_stop_release_check();
}

bool run_core_capture_parent_replacement_regression_check() {
  auto plan_equals = [](CoreRetainedProductionPlan plan,
                        CoreProductionPostureShape posture) {
    return plan.valid && plan.posture == posture;
  };
  auto report_entry_for_posture =
      [](const CoreBackingPlanEvaluationReport& report,
         CoreProductionPostureShape posture,
         CoreBackingPlanCandidateEvidenceReport& out) {
        for (const auto& entry : report.candidate_evidence) {
          if (entry.candidate.valid && entry.candidate.posture == posture) {
            out = entry;
            return true;
          }
        }
        return false;
      };
  auto find_capture_report = [](const CoreRuntime& rt,
                                uint64_t device_instance_id,
                                CoreBackingPlanEvaluationReport& out) {
    const auto reports = rt.backing_plan_evaluation_reports();
    for (const auto& report : reports) {
      if (report.target_kind !=
              CoreBackingPlanEvaluationReport::TargetKind::Capture ||
          report.target_id != device_instance_id) {
        continue;
      }
      out = report;
      return true;
    }
    return false;
  };
  auto wait_until = [](const std::function<bool()>& predicate) {
    for (int i = 0; i < kMaxIters; ++i) {
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
    }
    return false;
  };

  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr
        << "FAIL core capture parent replacement regression runtime start failed\n";
    return false;
  }
  if (!wait_for_core_runtime_live(rt)) {
    std::cerr
        << "FAIL core capture parent replacement regression runtime did not reach LIVE\n";
    rt.stop();
    return false;
  }

  BackingPlanEvaluationTestProvider provider;
  auto fail_with_cleanup = [&](const char* message) {
    std::cerr << message << "\n";
    (void)provider.shutdown();
    rt.stop();
    rt.attach_provider(nullptr);
    return false;
  };
  provider.set_capture_capabilities(
      ProducerBackingCapabilities{true, true, true});
  provider.set_backing_plan_evaluation_settle_delay_ns(0);
  if (!provider.initialize(rt.provider_callbacks()).ok()) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression provider init failed");
  }
  rt.attach_provider(&provider);

  constexpr uint64_t kDeviceId = 18101;
  constexpr uint64_t kRootId = 18102;
  if (rt.try_open_device("backing_plan_eval:0", kDeviceId, kRootId) !=
      TryOpenDeviceStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression open_device failed");
  }

  constexpr uint64_t kCaptureIdA = 18111;
  if (rt.try_trigger_device_capture_with_capture_id_for_server(
          kDeviceId, kCaptureIdA) != TryTriggerDeviceCaptureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression first trigger_capture failed");
  }
  if (!provider.emit_pending_capture(kCaptureIdA)) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression first pending capture publish failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_capture_result(kCaptureIdA, kDeviceId);
        return result &&
               result->default_image.payload_kind == ResultPayloadKind::CPU_PACKED;
      })) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression first capture result missing");
  }

  const SharedCaptureResultData first_capture_result =
      rt.get_capture_result(kCaptureIdA, kDeviceId);
  const uint64_t first_session_id = first_capture_result->acquisition_session_id;
  if (first_session_id == 0) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression first capture session missing");
  }
  rt.report_capture_retained_to_image_observation(
      kDeviceId,
      kCaptureIdA,
      first_session_id,
      first_capture_result->default_image.access_posture.posture_id,
      first_capture_result->default_image.retained_access_truth.to_image,
      true,
      90'000'000,
      true,
      90'000'000);
  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        return find_capture_report(rt, kDeviceId, report) &&
               report.evaluator_active &&
               report.current_candidate_index == 1 &&
               plan_equals(report.requested,
                           CoreProductionPostureShape::GpuPrimaryNoCpuSidecar);
      })) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression first candidate did not advance");
  }

  if (!provider.replace_primed_acquisition_session(kDeviceId)) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression could not replace primed acquisition session");
  }
  uint64_t replacement_session_id = 0;
  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        CoreBackingPlanCandidateEvidenceReport cpu_entry{};
        CoreBackingPlanCandidateEvidenceReport gpu_entry{};
        CoreBackingPlanCandidateEvidenceReport sidecar_entry{};
        if (!find_capture_report(rt, kDeviceId, report) ||
            report.parent_kind !=
                CoreBackingPlanEvaluationReport::ParentKind::AcquisitionSession ||
            report.acquisition_session_id == 0 ||
            report.acquisition_session_id == first_session_id ||
            !report.evaluator_active ||
            report.current_candidate_index != 0 ||
            !plan_equals(report.requested, CoreProductionPostureShape::CpuPrimary) ||
            !report_entry_for_posture(
                report, CoreProductionPostureShape::CpuPrimary, cpu_entry) ||
            !report_entry_for_posture(
                report, CoreProductionPostureShape::GpuPrimaryNoCpuSidecar,
                gpu_entry) ||
            !report_entry_for_posture(
                report, CoreProductionPostureShape::GpuPrimaryWithCpuSidecar,
                sidecar_entry)) {
          return false;
        }
        replacement_session_id = report.acquisition_session_id;
        return !cpu_entry.observation_seen &&
               !cpu_entry.evidence_accepted &&
               !gpu_entry.observation_seen &&
               !gpu_entry.evidence_accepted &&
               !sidecar_entry.observation_seen &&
               !sidecar_entry.evidence_accepted;
      })) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression did not restart a fresh non-measured epoch under the replacement session");
  }

  constexpr uint64_t kCaptureIdB = 18112;
  if (rt.try_trigger_device_capture_with_capture_id_for_server(
          kDeviceId, kCaptureIdB) != TryTriggerDeviceCaptureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression second trigger_capture failed");
  }
  if (!provider.emit_pending_capture(kCaptureIdB)) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression second pending capture publish failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_capture_result(kCaptureIdB, kDeviceId);
        return result &&
               result->default_image.payload_kind == ResultPayloadKind::CPU_PACKED;
      })) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression second capture result missing");
  }
  const SharedCaptureResultData second_capture_result =
      rt.get_capture_result(kCaptureIdB, kDeviceId);
  if (second_capture_result->acquisition_session_id != replacement_session_id) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression second capture used the wrong acquisition session");
  }
  rt.report_capture_retained_to_image_observation(
      kDeviceId,
      kCaptureIdB,
      replacement_session_id,
      second_capture_result->default_image.access_posture.posture_id,
      second_capture_result->default_image.retained_access_truth.to_image,
      true,
      70'000'000,
      true,
      70'000'000);
  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        return find_capture_report(rt, kDeviceId, report) &&
               report.evaluator_active &&
               report.current_candidate_index == 1 &&
               plan_equals(report.requested,
                           CoreProductionPostureShape::GpuPrimaryNoCpuSidecar);
      })) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression restarted epoch did not advance after the replacement-session CPU candidate");
  }

  constexpr uint64_t kCaptureIdC = 18113;
  if (rt.try_trigger_device_capture_with_capture_id_for_server(
          kDeviceId, kCaptureIdC) != TryTriggerDeviceCaptureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression third trigger_capture failed");
  }
  if (!provider.emit_pending_capture(kCaptureIdC)) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression third pending capture publish failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_capture_result(kCaptureIdC, kDeviceId);
        return result &&
               result->default_image.payload_kind == ResultPayloadKind::GPU_SURFACE &&
               !result->default_image.access_posture.has_retained_cpu_payload;
      })) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression third capture result missing");
  }
  const SharedCaptureResultData third_capture_result =
      rt.get_capture_result(kCaptureIdC, kDeviceId);
  if (third_capture_result->acquisition_session_id != replacement_session_id) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression third capture used the wrong acquisition session");
  }
  rt.report_capture_retained_to_image_observation(
      kDeviceId,
      kCaptureIdC,
      replacement_session_id,
      third_capture_result->default_image.access_posture.posture_id,
      third_capture_result->default_image.retained_access_truth.to_image,
      true,
      60'000'000,
      true,
      60'000'000);
  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        return find_capture_report(rt, kDeviceId, report) &&
               report.evaluator_active &&
               report.current_candidate_index == 2 &&
               plan_equals(report.requested,
                           CoreProductionPostureShape::GpuPrimaryWithCpuSidecar);
      })) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression restarted epoch did not advance after the replacement-session GPU candidate");
  }

  constexpr uint64_t kCaptureIdD = 18114;
  if (rt.try_trigger_device_capture_with_capture_id_for_server(
          kDeviceId, kCaptureIdD) != TryTriggerDeviceCaptureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression fourth trigger_capture failed");
  }
  if (!provider.emit_pending_capture(kCaptureIdD)) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression fourth pending capture publish failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_capture_result(kCaptureIdD, kDeviceId);
        return result &&
               result->default_image.payload_kind == ResultPayloadKind::GPU_SURFACE &&
               result->default_image.access_posture.has_retained_cpu_payload;
      })) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression fourth capture result missing");
  }
  const SharedCaptureResultData fourth_capture_result =
      rt.get_capture_result(kCaptureIdD, kDeviceId);
  if (fourth_capture_result->acquisition_session_id != replacement_session_id) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression fourth capture used the wrong acquisition session");
  }
  rt.report_capture_retained_to_image_observation(
      kDeviceId,
      kCaptureIdD,
      replacement_session_id,
      fourth_capture_result->default_image.access_posture.posture_id,
      fourth_capture_result->default_image.retained_access_truth.to_image,
      true,
      40'000'000,
      true,
      40'000'000);
  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        return find_capture_report(rt, kDeviceId, report) &&
               !report.evaluator_active &&
               plan_equals(report.steady,
                           CoreProductionPostureShape::GpuPrimaryWithCpuSidecar) &&
               plan_equals(report.decision_selected,
                           CoreProductionPostureShape::GpuPrimaryWithCpuSidecar);
      })) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression did not settle the restarted epoch");
  }

  {
    CoreBackingPlanEvaluationReport report{};
    if (!find_capture_report(rt, kDeviceId, report)) {
      return fail_with_cleanup(
          "FAIL core capture parent replacement regression final capture report missing");
    }
    CoreBackingPlanCandidateEvidenceReport cpu_entry{};
    CoreBackingPlanCandidateEvidenceReport gpu_entry{};
    CoreBackingPlanCandidateEvidenceReport sidecar_entry{};
    if (!report_entry_for_posture(
            report, CoreProductionPostureShape::CpuPrimary, cpu_entry) ||
        !report_entry_for_posture(
            report, CoreProductionPostureShape::GpuPrimaryNoCpuSidecar,
            gpu_entry) ||
        !report_entry_for_posture(
            report, CoreProductionPostureShape::GpuPrimaryWithCpuSidecar,
            sidecar_entry)) {
      return fail_with_cleanup(
          "FAIL core capture parent replacement regression final report missing candidate evidence");
    }
    if (cpu_entry.observed_acquisition_session_id != replacement_session_id ||
        gpu_entry.observed_acquisition_session_id != replacement_session_id ||
        sidecar_entry.observed_acquisition_session_id !=
            replacement_session_id ||
        cpu_entry.observed_acquisition_session_id == first_session_id ||
        gpu_entry.observed_acquisition_session_id == first_session_id ||
        sidecar_entry.observed_acquisition_session_id == first_session_id ||
        !cpu_entry.evidence_accepted || !gpu_entry.evidence_accepted ||
        !sidecar_entry.evidence_accepted) {
      return fail_with_cleanup(
          "FAIL core capture parent replacement regression final decision mixed evidence across acquisition-session ids");
    }
  }

  if (rt.try_close_device(kDeviceId) != TryCloseDeviceStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture parent replacement regression teardown failed");
  }

  (void)provider.shutdown();
  rt.stop();
  rt.attach_provider(nullptr);
  return true;
}

bool run_core_stream_partial_reporting_check() {
  auto plan_equals = [](CoreRetainedProductionPlan plan,
                        CoreProductionPostureShape posture) {
    return plan.valid && plan.posture == posture;
  };
  auto report_entry_for_posture =
      [](const CoreBackingPlanEvaluationReport& report,
         CoreProductionPostureShape posture,
         CoreBackingPlanCandidateEvidenceReport& out) {
        for (const auto& entry : report.candidate_evidence) {
          if (entry.candidate.valid && entry.candidate.posture == posture) {
            out = entry;
            return true;
          }
        }
        return false;
      };
  auto find_stream_report = [](const CoreRuntime& rt,
                               uint64_t stream_id,
                               CoreBackingPlanEvaluationReport& out) {
    const auto reports = rt.backing_plan_evaluation_reports();
    for (const auto& report : reports) {
      if (report.target_kind !=
              CoreBackingPlanEvaluationReport::TargetKind::Stream ||
          report.target_id != stream_id) {
        continue;
      }
      out = report;
      return true;
    }
    return false;
  };
  auto wait_until = [](const std::function<bool()>& predicate) {
    for (int i = 0; i < kMaxIters; ++i) {
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
    }
    return false;
  };

  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "FAIL core stream partial reporting runtime start failed\n";
    return false;
  }
  if (!wait_for_core_runtime_live(rt)) {
    std::cerr
        << "FAIL core stream partial reporting runtime did not reach LIVE\n";
    rt.stop();
    return false;
  }

  BackingPlanEvaluationTestProvider provider;
  auto fail_with_cleanup = [&](const char* message) {
    std::cerr << message << "\n";
    (void)provider.shutdown();
    rt.stop();
    rt.attach_provider(nullptr);
    return false;
  };
  if (!provider.initialize(rt.provider_callbacks()).ok()) {
    return fail_with_cleanup(
        "FAIL core stream partial reporting provider init failed");
  }
  rt.attach_provider(&provider);

  constexpr uint64_t kDeviceId = 17301;
  constexpr uint64_t kRootId = 17302;
  constexpr uint64_t kStreamId = 17303;
  if (rt.try_open_device("backing_plan_eval:0", kDeviceId, kRootId) !=
      TryOpenDeviceStatus::OK) {
    return fail_with_cleanup(
        "FAIL core stream partial reporting open_device failed");
  }
  if (rt.try_create_stream(
          kStreamId, kDeviceId, StreamIntent::PREVIEW, nullptr, nullptr, 0) !=
      TryCreateStreamStatus::OK) {
    return fail_with_cleanup(
        "FAIL core stream partial reporting create_stream failed");
  }
  if (rt.try_start_stream(kStreamId) != TryStartStreamStatus::OK) {
    return fail_with_cleanup(
        "FAIL core stream partial reporting start_stream failed");
  }
  if (!provider.emit_stream_frame(kStreamId, true)) {
    return fail_with_cleanup(
        "FAIL core stream partial reporting first stream frame emit failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_latest_stream_result(kStreamId);
        return result &&
               result->payload_kind == ResultPayloadKind::GPU_SURFACE &&
               !result->access_posture.has_retained_cpu_payload;
      })) {
    return fail_with_cleanup(
        "FAIL core stream partial reporting first result missing");
  }
  const SharedStreamResultData first_result =
      rt.get_latest_stream_result(kStreamId);
  rt.report_stream_retained_display_view_observation(
      kStreamId,
      first_result->access_posture.posture_id,
      first_result->retained_access_truth.display_view,
      true,
      80);
  if (!wait_until([&]() {
        const auto* rec = rt.stream_record(kStreamId);
        return rec &&
               plan_equals(rec->requested_retained_plan,
                           CoreProductionPostureShape::
                               GpuPrimaryWithCpuSidecar);
      })) {
    return fail_with_cleanup(
        "FAIL core stream partial reporting did not advance to sidecar candidate");
  }

  if (!provider.emit_stream_frame(kStreamId, true)) {
    return fail_with_cleanup(
        "FAIL core stream partial reporting second stream frame emit failed");
  }
  if (!wait_until([&]() {
        const auto result = rt.get_latest_stream_result(kStreamId);
        return result &&
               result->payload_kind == ResultPayloadKind::GPU_SURFACE &&
               result->access_posture.has_retained_cpu_payload;
      })) {
    return fail_with_cleanup(
        "FAIL core stream partial reporting sidecar result missing");
  }
  const SharedStreamResultData second_result =
      rt.get_latest_stream_result(kStreamId);
  rt.retain_stream_display_demand(kStreamId);
  rt.report_stream_retained_display_view_observation(
      kStreamId,
      second_result->access_posture.posture_id,
      second_result->retained_access_truth.display_view,
      true,
      90);
  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        CoreBackingPlanCandidateEvidenceReport cpu_entry{};
        CoreBackingPlanCandidateEvidenceReport sidecar_entry{};
        return find_stream_report(rt, kStreamId, report) &&
               !report.evaluator_active &&
               report.decision_from_evaluation &&
               report.completion_reason ==
                   BackingPlanEvaluationCompletionReason::
                       LiveDisplayDemandFamilyCrossing &&
               plan_equals(
                   report.decision_selected,
                   CoreProductionPostureShape::
                       GpuPrimaryNoCpuSidecar) &&
                report.candidate_evidence.size() == 3u &&
                report_entry_for_posture(
                   report, CoreProductionPostureShape::CpuPrimary,
                   cpu_entry) &&
                report_entry_for_posture(
                   report,
                   CoreProductionPostureShape::GpuPrimaryWithCpuSidecar,
                   sidecar_entry) &&
               !cpu_entry.observation_seen &&
               sidecar_entry.evidence_accepted;
      })) {
    return fail_with_cleanup(
        "FAIL core stream partial reporting did not preserve viable-vs-measured truth for live display demand");
  }
  rt.release_stream_display_demand(kStreamId);

  (void)provider.shutdown();
  rt.stop();
  rt.attach_provider(nullptr);
  return true;
}

bool run_core_capture_observation_after_device_close_check() {
  auto plan_equals = [](CoreRetainedProductionPlan plan,
                        CoreProductionPostureShape posture) {
    return plan.valid && plan.posture == posture;
  };
  auto find_capture_report = [](const CoreRuntime& rt,
                                uint64_t device_instance_id,
                                CoreBackingPlanEvaluationReport& out) {
    const auto reports = rt.backing_plan_evaluation_reports();
    for (const auto& report : reports) {
      if (report.target_kind != CoreBackingPlanEvaluationReport::TargetKind::Capture ||
          report.target_id != device_instance_id) {
        continue;
      }
      out = report;
      return true;
    }
    return false;
  };
  auto wait_until = [](const std::function<bool()>& predicate) {
    for (int i = 0; i < kMaxIters; ++i) {
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
    }
    return false;
  };

  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "FAIL core capture observation after device close runtime start failed\n";
    return false;
  }
  if (!wait_for_core_runtime_live(rt)) {
    std::cerr << "FAIL core capture observation after device close runtime did not reach LIVE\n";
    rt.stop();
    return false;
  }

  BackingPlanEvaluationTestProvider provider;
  auto fail_with_cleanup = [&](const char* message) {
    std::cerr << message << "\n";
    (void)provider.shutdown();
    rt.stop();
    rt.attach_provider(nullptr);
    return false;
  };

  if (!provider.initialize(rt.provider_callbacks()).ok()) {
    return fail_with_cleanup(
        "FAIL core capture observation after device close initialize provider failed");
  }
  rt.attach_provider(&provider);

  constexpr uint64_t kDeviceId = 19001;
  constexpr uint64_t kRootId = 29001;
  constexpr uint64_t kCaptureId = 39001;

  if (rt.try_open_device("backing_plan_eval:0", kDeviceId, kRootId) !=
      TryOpenDeviceStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture observation after device close open_device failed");
  }

  PictureConfig capture_picture = provider.capture_template().picture;
  capture_picture.seed = 123;
  if (rt.try_set_capture_picture_config(kDeviceId, capture_picture) !=
      TrySetCapturePictureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture observation after device close capture picture setup failed");
  }

  if (rt.try_trigger_device_capture_with_capture_id_for_server(
          kDeviceId, kCaptureId) != TryTriggerDeviceCaptureStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture observation after device close trigger_capture failed");
  }

  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        return find_capture_report(rt, kDeviceId, report) &&
               report.evaluator_active &&
               report.current_candidate_index == 0 &&
               plan_equals(report.requested,
                           CoreProductionPostureShape::CpuPrimary) &&
               !report.steady.valid;
      })) {
    return fail_with_cleanup(
        "FAIL core capture observation after device close did not realize active acquisition-session evaluator");
  }

  if (!provider.emit_pending_capture(kCaptureId)) {
    return fail_with_cleanup(
        "FAIL core capture observation after device close pending capture publish failed");
  }

  if (!wait_until([&]() {
        const auto result = rt.get_capture_result(kCaptureId, kDeviceId);
        return result &&
               result->default_image.payload_kind == ResultPayloadKind::CPU_PACKED &&
               !result->default_image.access_posture.has_retained_gpu_backing;
      })) {
    return fail_with_cleanup(
        "FAIL core capture observation after device close capture result missing");
  }

  const SharedCaptureResultData capture_result =
      rt.get_capture_result(kCaptureId, kDeviceId);
  if (!capture_result) {
    return fail_with_cleanup(
        "FAIL core capture observation after device close capture result retrieval failed");
  }

  if (rt.try_close_device(kDeviceId) != TryCloseDeviceStatus::OK) {
    return fail_with_cleanup(
        "FAIL core capture observation after device close close_device failed");
  }
  if (!wait_until([&]() {
        return !provider.has_active_capture_parent_priming(kDeviceId) &&
               provider.active_capture_parent_priming_count() == 0u;
      })) {
    return fail_with_cleanup(
        "FAIL core capture observation after device close leaked a priming hold through device teardown");
  }

  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        return rt.device_record(kDeviceId) == nullptr &&
               find_capture_report(rt, kDeviceId, report) &&
               report.evaluator_active &&
               report.current_candidate_index == 0 &&
               plan_equals(report.requested,
                           CoreProductionPostureShape::CpuPrimary) &&
               !report.steady.valid;
      })) {
    return fail_with_cleanup(
        "FAIL core capture observation after device close did not retain orphaned evaluator state");
  }

  rt.report_capture_retained_to_image_observation(
      kDeviceId,
      kCaptureId,
      capture_result->acquisition_session_id,
      capture_result->default_image.access_posture.posture_id,
      capture_result->default_image.retained_access_truth.to_image,
      true,
      80'000'000,
      true,
      80'000'000);

  if (!wait_until([&]() {
        CoreBackingPlanEvaluationReport report{};
        return rt.device_record(kDeviceId) == nullptr &&
               find_capture_report(rt, kDeviceId, report) &&
               report.evaluator_active &&
               report.current_candidate_index == 1 &&
               plan_equals(report.requested,
                           CoreProductionPostureShape::GpuPrimaryNoCpuSidecar) &&
               !report.steady.valid;
      })) {
    return fail_with_cleanup(
        "FAIL core capture observation after device close did not advance orphaned evaluator safely");
  }

  (void)provider.shutdown();
  rt.stop();
  rt.attach_provider(nullptr);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_opts(argc, argv, opt)) {
    return 2;
  }

  using CheckFn = std::function<bool()>;
  const std::vector<std::pair<const char*, CheckFn>> checks = {
      {"run_provider_access_preflight_check", [] { return run_provider_access_preflight_check(); }},
      {"run_synthetic_scenario_materialization_check", [] { return run_synthetic_scenario_materialization_check(); }},
      {"run_synthetic_builtin_scenario_library_build_check", [] { return run_synthetic_builtin_scenario_library_build_check(); }},
      {"run_synthetic_external_scenario_loader_check", [] { return run_synthetic_external_scenario_loader_check(); }},
      {"run_synthetic_external_scenario_loader_negative_check", [] { return run_synthetic_external_scenario_loader_negative_check(); }},
      {"run_synthetic_primitive_lifecycle_foundation_check", [] { return run_synthetic_primitive_lifecycle_foundation_check(); }},
      {"run_clustered_strict_branch_check", [] { return run_clustered_strict_branch_check(); }},
      {"run_clustered_completion_gated_branch_check", [] { return run_clustered_completion_gated_branch_check(); }},
      {"run_broker_timeline_host_surface_check", [] { return run_broker_timeline_host_surface_check(); }},
      {"run_synthetic_backing_capability_truth_check", [] { return run_synthetic_backing_capability_truth_check(); }},
      {"run_synthetic_parent_context_capability_downgrade_matrix_check", [] { return run_synthetic_parent_context_capability_downgrade_matrix_check(); }},
      {"run_synthetic_producer_output_form_mode_production_check", [] { return run_synthetic_producer_output_form_mode_production_check(); }},
      {"run_synthetic_timeline_picture_appearance_check", [] { return run_synthetic_timeline_picture_appearance_check(); }},
      {"run_stub_provider_sanity_check", [] { return run_stub_provider_sanity_check(); }},
      {"run_synthetic_provider_direct_sanity_check", [] { return run_synthetic_provider_direct_sanity_check(); }},
      {"run_synthetic_still_only_acquisition_session_truth_check", [] { return run_synthetic_still_only_acquisition_session_truth_check(); }},
      {"run_synthetic_multi_member_still_sequence_check", [] { return run_synthetic_multi_member_still_sequence_check(); }},
      {"run_synthetic_dynamic_still_bundle_shape_check", [] { return run_synthetic_dynamic_still_bundle_shape_check(); }},
      {"run_core_synthetic_three_member_capture_result_check", [] { return run_core_synthetic_three_member_capture_result_check(); }},
      {"run_core_synthetic_three_member_capture_result_realized_ev_mismatch_check", [] { return run_core_synthetic_three_member_capture_result_realized_ev_mismatch_check(); }},
      {"run_synthetic_still_bundle_capability_gate_contract_check", [] { return run_synthetic_still_bundle_capability_gate_contract_check(); }},
      {"run_core_synthetic_three_member_realized_unknown_propagation_check", [] { return run_core_synthetic_three_member_realized_unknown_propagation_check(); }},
      {"run_synthetic_stream_plus_still_single_session_truth_check", [] { return run_synthetic_stream_plus_still_single_session_truth_check(); }},
      {"run_core_measured_backing_plan_evaluation_check", [] { return run_core_measured_backing_plan_evaluation_check(); }},
      {"run_core_capture_observation_regression_check", [] { return run_core_capture_observation_regression_check(); }},
      {"run_core_capture_parent_replacement_regression_check", [] { return run_core_capture_parent_replacement_regression_check(); }},
      {"run_core_stream_partial_reporting_check", [] { return run_core_stream_partial_reporting_check(); }},
      {"run_core_capture_observation_after_device_close_check", [] { return run_core_capture_observation_after_device_close_check(); }},
  };

  if (!opt.only_check.empty()) {
    if (opt.only_check == "run_external_scenario_file_execution_check") {
      if (opt.external_scenario_file.empty()) {
        std::cerr << "FAIL run_external_scenario_file_execution_check requires --external_scenario_file=<path>\n";
        return 1;
      }
      if (!run_external_scenario_file_execution_check(opt.external_scenario_file)) return 1;
      std::cout << "PASS provider_compliance_verify\n";
      return 0;
    }
    for (const auto& check : checks) {
      if (opt.only_check == check.first) {
        if (!check.second()) return 1;
        std::cout << "PASS provider_compliance_verify\n";
        return 0;
      }
    }
    std::cerr << "Unknown --only_check value: " << opt.only_check << "\nAvailable check names:\n";
    for (const auto& check : checks) {
      std::cerr << "  " << check.first << "\n";
    }
    std::cerr << "  run_external_scenario_file_execution_check\n";
    return 1;
  }

  for (const auto& check : checks) {
    if (!check.second()) return 1;
  }
  if (!opt.external_scenario_file.empty()) {
    if (!run_external_scenario_file_execution_check(opt.external_scenario_file)) return 1;
  }

  std::cout << "PASS provider_compliance_verify\n";
  return 0;
}

// CamBANG android_camera2 platform provider implementation (Camera2 NDK).
// See camera2_camera_provider.h for the architectural overview and
// docs/provider_implementation_brief.md for the contract this adapts to.
//
// Backend surface: ACameraManager / ACameraDevice / ACameraCaptureSession
// (libcamera2ndk) with AImageReader (libmediandk) delivering YUV_420_888
// images. Requires the Android NDK.

#include "imaging/platform/android/camera2_camera_provider.h"

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

namespace cambang {
namespace camera2_detail {

namespace {

void log_line(const char* fmt, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  __android_log_print(ANDROID_LOG_INFO, "CamBANG", "[camera2_provider] %s", buffer);
}

ProviderError provider_error_from_camera_status(camera_status_t st) noexcept {
  switch (st) {
    case ACAMERA_OK:
      return ProviderError::OK;
    case ACAMERA_ERROR_PERMISSION_DENIED:
      // The app does not hold the CAMERA runtime permission. Nothing the
      // provider can do about it; it is a platform-imposed constraint.
      return ProviderError::ERR_PLATFORM_CONSTRAINT;
    case ACAMERA_ERROR_CAMERA_IN_USE:
    case ACAMERA_ERROR_MAX_CAMERA_IN_USE:
    case ACAMERA_ERROR_CAMERA_DISABLED:
      return ProviderError::ERR_BUSY;
    case ACAMERA_ERROR_CAMERA_DISCONNECTED:
      return ProviderError::ERR_TRANSIENT_FAILURE;
    case ACAMERA_ERROR_NOT_ENOUGH_MEMORY:
      return ProviderError::ERR_TRANSIENT_FAILURE;
    case ACAMERA_ERROR_METADATA_NOT_FOUND:
    case ACAMERA_ERROR_STREAM_CONFIGURE_FAIL:
    case ACAMERA_ERROR_UNSUPPORTED_OPERATION:
      return ProviderError::ERR_PLATFORM_CONSTRAINT;
    case ACAMERA_ERROR_INVALID_PARAMETER:
      return ProviderError::ERR_INVALID_ARGUMENT;
    case ACAMERA_ERROR_SESSION_CLOSED:
    case ACAMERA_ERROR_INVALID_OPERATION:
      return ProviderError::ERR_BAD_STATE;
    default:
      return ProviderError::ERR_PROVIDER_FAILED;
  }
}

} // namespace

// ---------------------------------------------------------------------------
// BoundedControlExecutor
// ---------------------------------------------------------------------------

BoundedControlExecutor::~BoundedControlExecutor() { stop(); }

bool BoundedControlExecutor::start() noexcept {
  if (running_.load(std::memory_order_acquire)) {
    return false;
  }
  try {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_requested_ = false;
      q_.clear();
    }
    worker_ = std::thread([this] { thread_main_(); });
  } catch (...) {
    return false;
  }
  running_.store(true, std::memory_order_release);
  return true;
}

void BoundedControlExecutor::stop() noexcept {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_requested_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    // The control thread only runs bounded backend jobs; a wedged HAL can
    // stall it indefinitely, in which case abandonment (detach) mirrors
    // Core's own wedged-thread posture rather than wedging teardown.
    try {
      worker_.join();
    } catch (...) {
      worker_.detach();
    }
  }
  running_.store(false, std::memory_order_release);
}

bool BoundedControlExecutor::run_bounded(
    std::function<void(const AbandonToken&)> job,
    std::shared_ptr<AbandonToken> token,
    uint32_t timeout_ms) noexcept {
  if (!running_.load(std::memory_order_acquire) || !job || !token) {
    return false;
  }
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> fut;
  try {
    fut = done->get_future();
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stop_requested_) {
        return false;
      }
      q_.push_back(Entry{std::move(job), token, done});
    }
  } catch (...) {
    return false;
  }
  cv_.notify_one();
  if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
    token->abandoned.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

void BoundedControlExecutor::thread_main_() noexcept {
  for (;;) {
    Entry entry;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_requested_ || !q_.empty(); });
      if (q_.empty()) {
        break; // stop requested and drained
      }
      entry = std::move(q_.front());
      q_.pop_front();
    }
    try {
      entry.job(*entry.token);
    } catch (...) {
      // Backend jobs contain their own failures; nothing may escape here.
    }
    entry.done->set_value();
  }
}

// ---------------------------------------------------------------------------
// Frame conversion (YUV_420_888 -> packed RGBA/BGRA)
// ---------------------------------------------------------------------------

namespace {

inline uint8_t clamp_u8(int32_t v) noexcept {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// BT.601 limited ("video") range, the range Camera2 YUV_420_888 output uses.
// Fixed point at 1/256 so a full frame stays integer-only.
//
// Handles both planar (uv_pixel_stride == 1) and semiplanar/NV12-NV21
// (uv_pixel_stride == 2) chroma layouts, which is the whole point of
// YUV_420_888: the format is a family, and the strides are the only truthful
// description of which member a given device handed over.
void convert_yuv420_to_packed(const uint8_t* y_plane,
                              int32_t y_row_stride,
                              const uint8_t* u_plane,
                              const uint8_t* v_plane,
                              int32_t uv_row_stride,
                              int32_t uv_pixel_stride,
                              uint32_t width,
                              uint32_t height,
                              uint32_t dst_fourcc,
                              uint8_t* dst) {
  const bool to_rgba = (dst_fourcc == FOURCC_RGBA);
  const size_t dst_row_bytes = static_cast<size_t>(width) * 4u;
  for (uint32_t row = 0; row < height; ++row) {
    const uint8_t* y_row = y_plane + static_cast<ptrdiff_t>(y_row_stride) * row;
    const ptrdiff_t uv_row_offset =
        static_cast<ptrdiff_t>(uv_row_stride) * static_cast<ptrdiff_t>(row / 2u);
    const uint8_t* u_row = u_plane + uv_row_offset;
    const uint8_t* v_row = v_plane + uv_row_offset;
    uint8_t* out = dst + dst_row_bytes * row;
    for (uint32_t col = 0; col < width; ++col) {
      const ptrdiff_t uv_index =
          static_cast<ptrdiff_t>(uv_pixel_stride) * static_cast<ptrdiff_t>(col / 2u);
      const int32_t c = static_cast<int32_t>(y_row[col]) - 16;
      const int32_t d = static_cast<int32_t>(u_row[uv_index]) - 128;
      const int32_t e = static_cast<int32_t>(v_row[uv_index]) - 128;
      const uint8_t r = clamp_u8((298 * c + 409 * e + 128) >> 8);
      const uint8_t g = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
      const uint8_t b = clamp_u8((298 * c + 516 * d + 128) >> 8);
      if (to_rgba) {
        out[4 * col + 0] = r;
        out[4 * col + 1] = g;
        out[4 * col + 2] = b;
      } else { // BGRA
        out[4 * col + 0] = b;
        out[4 * col + 1] = g;
        out[4 * col + 2] = r;
      }
      out[4 * col + 3] = 0xFF;
    }
  }
}

// Copies one acquired YUV_420_888 AImage into dst (width*height*4, requested
// fourcc). Returns false without touching dst on any shape mismatch, so a
// device that silently substituted geometry can never be published as if it
// had honoured the request.
bool convert_acquired_image(AImage* image,
                            uint32_t width,
                            uint32_t height,
                            uint32_t dst_fourcc,
                            uint8_t* dst) {
  if (!image) {
    return false;
  }
  int32_t img_w = 0, img_h = 0, img_format = 0;
  if (AImage_getWidth(image, &img_w) != AMEDIA_OK ||
      AImage_getHeight(image, &img_h) != AMEDIA_OK ||
      AImage_getFormat(image, &img_format) != AMEDIA_OK) {
    return false;
  }
  if (static_cast<uint32_t>(img_w) != width || static_cast<uint32_t>(img_h) != height ||
      img_format != AIMAGE_FORMAT_YUV_420_888) {
    return false;
  }

  uint8_t* y_data = nullptr;
  uint8_t* u_data = nullptr;
  uint8_t* v_data = nullptr;
  int y_len = 0, u_len = 0, v_len = 0;
  int32_t y_row_stride = 0, uv_row_stride = 0, uv_pixel_stride = 0;
  if (AImage_getPlaneData(image, 0, &y_data, &y_len) != AMEDIA_OK ||
      AImage_getPlaneData(image, 1, &u_data, &u_len) != AMEDIA_OK ||
      AImage_getPlaneData(image, 2, &v_data, &v_len) != AMEDIA_OK ||
      AImage_getPlaneRowStride(image, 0, &y_row_stride) != AMEDIA_OK ||
      AImage_getPlaneRowStride(image, 1, &uv_row_stride) != AMEDIA_OK ||
      AImage_getPlanePixelStride(image, 1, &uv_pixel_stride) != AMEDIA_OK) {
    return false;
  }
  if (!y_data || !u_data || !v_data || y_row_stride <= 0 || uv_row_stride <= 0 ||
      uv_pixel_stride <= 0) {
    return false;
  }
  // Bounds check before reading: a driver reporting strides inconsistent with
  // the buffer it handed over must fail the conversion, not walk off the end.
  const int64_t y_needed =
      static_cast<int64_t>(y_row_stride) * (height - 1) + width;
  const int64_t uv_needed = static_cast<int64_t>(uv_row_stride) * ((height / 2u) - 1) +
                            static_cast<int64_t>(uv_pixel_stride) * ((width / 2u) - 1) + 1;
  if (y_len < y_needed || u_len < uv_needed || v_len < uv_needed) {
    return false;
  }

  convert_yuv420_to_packed(y_data, y_row_stride, u_data, v_data, uv_row_stride,
                           uv_pixel_stride, width, height, dst_fourcc, dst);
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Per-device backend
// ---------------------------------------------------------------------------

// Device characteristics read exactly once, at open. Camera2 characteristics
// are static per camera id by definition, so caching them is not an
// optimisation shortcut -- it is what lets admission answer capability
// questions promptly and without backend I/O (brief §2, §6).
struct StaticCharacteristics {
  bool has_facing = false;
  uint8_t facing = 0; // ACAMERA_LENS_FACING_*
  bool has_orientation = false;
  int32_t orientation_degrees = 0;

  // ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE. REALTIME marks are on the same
  // boot-time base every camera on the device shares; UNKNOWN marks are only
  // meaningful within one device.
  bool timestamp_source_realtime = false;

  bool is_logical_multi_camera = false;

  // Device-constant optics: reported only when the device offers exactly one
  // possible value, which is Camera2's way of describing a prime lens or a
  // fixed iris. More than one value means the quantity is per-capture and
  // belongs in the per-image tier instead.
  bool has_single_focal_length_mm = false;
  float single_focal_length_mm = 0.0f;
  bool has_single_aperture = false;
  float single_aperture = 0.0f;

  // ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE == 0 is Camera2's documented
  // marker for a fixed-focus lens focused at infinity.
  bool fixed_focus_at_infinity = false;
  // ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION. UNCALIBRATED means the
  // reported focus distance is not in real diopters, so it must never be
  // converted to metres.
  bool focus_distance_is_metric = false;

  // Exposure compensation. Present only when the device reports a usable
  // range and a positive step; that is the authoritative bracket-support
  // answer, cached for prompt admission.
  bool has_exposure_compensation = false;
  int32_t ae_comp_min_steps = 0;
  int32_t ae_comp_max_steps = 0;
  double ae_comp_step_ev = 0.0;

  // The two sensor coordinate systems Camera2 expresses calibration in. Which
  // one applies to a given image depends on that capture's distortion
  // correction mode, so both are cached and the per-image path picks.
  bool has_pre_correction_array = false;
  uint32_t pre_correction_array_w = 0;
  uint32_t pre_correction_array_h = 0;
  bool has_active_array = false;
  uint32_t active_array_w = 0;
  uint32_t active_array_h = 0;

  // Physical mounting pose. Genuinely device-constant geometry, so it belongs
  // to the static tier; unlike intrinsics it carries no coordinate-domain
  // ambiguity that depends on a per-capture mode.
  bool has_pose_translation = false;
  float pose_translation_m[3] = {0.0f, 0.0f, 0.0f};
  bool has_pose_rotation = false;
  float pose_rotation_xyzw[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  bool has_pose_reference = false;
  uint8_t pose_reference = 0;

  // Bracket-execution capability. Read here so any future decision between
  // burst submission and manual-sensor bracketing is made from device-stated
  // capability rather than from what one handset happened to do.
  bool burst_capture_supported = false;
  bool manual_sensor_supported = false;
  // ACAMERA_SYNC_MAX_LATENCY: frames before a submitted control is reflected
  // in results. 0 (PER_FRAME_CONTROL) means per-request control lands
  // immediately; -1 (UNKNOWN) means the device will not say.
  bool has_sync_max_latency = false;
  int32_t sync_max_latency = 0;
  bool ae_lock_available = false;
  bool awb_lock_available = false;
  // Manual bracketing needs both ranges to compute an exposure/ISO pair.
  bool has_exposure_time_range = false;
  int64_t exposure_time_min_ns = 0;
  int64_t exposure_time_max_ns = 0;
  bool has_sensitivity_range = false;
  int32_t sensitivity_min = 0;
  int32_t sensitivity_max = 0;

  // True when the device lists a real correction mode (FAST/HIGH_QUALITY).
  // Camera2 documents that a device not supporting the API "will always list
  // only OFF", so a device without it cannot be correcting its output -- which
  // makes OFF a derivation from stated capability, not an assumption, when a
  // result omits ACAMERA_DISTORTION_CORRECTION_MODE.
  bool distortion_correction_supported = false;

  // Output geometries the device actually supports for YUV_420_888.
  std::vector<std::pair<uint32_t, uint32_t>> supported_yuv_sizes;

  bool supports_size(uint32_t w, uint32_t h) const noexcept {
    for (const auto& size : supported_yuv_sizes) {
      if (size.first == w && size.second == h) {
        return true;
      }
    }
    return false;
  }
};

// Realized capture-result facts for exactly one still member. Every field is
// read out of Camera2 capture *result* metadata, which reports what the
// sensor did rather than what the request asked for.
struct ResultFacts {
  bool has_exposure_ns = false;
  int64_t exposure_ns = 0;
  bool has_iso = false;
  int32_t iso = 0;
  bool has_aperture = false;
  float aperture = 0.0f;
  bool has_focal_length_mm = false;
  float focal_length_mm = 0.0f;
  bool has_focus_diopters = false;
  float focus_diopters = 0.0f;
  bool has_ae_comp_steps = false;
  int32_t ae_comp_steps = 0;

  // [f_x, f_y, c_x, c_y, s] in sensor-array pixels.
  bool has_intrinsics = false;
  float intrinsics[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  // [kappa_1, kappa_2, kappa_3, kappa_4, kappa_5]: three radial then two
  // tangential coefficients, which is exactly BrownConrady5Distortion's shape.
  bool has_distortion = false;
  float distortion[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  // Selects both the coordinate system the two above are expressed in and
  // whether the delivered pixels have already been corrected.
  bool has_distortion_correction_mode = false;
  uint8_t distortion_correction_mode = 0;

  // ACAMERA_SENSOR_TIMESTAMP. The only key that ties a result back to the
  // image it describes once a burst has several captures in flight at once.
  bool has_sensor_timestamp = false;
  int64_t sensor_timestamp_ns = 0;
};

struct StreamProduction {
  uint64_t stream_id = 0;
  uint64_t device_instance_id = 0;
  uint64_t acquisition_session_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fourcc = 0;
  CoreRetainedProductionPlan plan{};
  uint64_t frames_posted = 0;
  uint64_t convert_failures = 0;

  struct BufferSlot {
    std::vector<uint8_t> bytes;
    std::atomic<bool> in_use{false};
  };
  std::vector<std::shared_ptr<BufferSlot>> pool;
  size_t cursor = 0;

  // Guarded by DeviceBackend::m.
  bool producing = false;
  uint64_t pool_exhausted_drops = 0;
};

// Collector for one in-flight burst (a burst of one is the ordinary single
// capture). Images and result metadata arrive on two different NDK callback
// threads and, within a burst, several of each are in flight simultaneously —
// so neither can be paired with the other by arrival order. They are matched
// on ACAMERA_SENSOR_TIMESTAMP, which both sides carry for the same frame.
//
// The worker blocks until every expected image has landed; metadata is
// enrichment and never gates a frame, so a member whose result never arrives
// still yields truthful pixels with fewer facts.
struct BurstCollector {
  std::mutex m;
  std::condition_variable cv;

  size_t expected = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fourcc = 0;

  struct Image {
    std::shared_ptr<std::vector<uint8_t>> bytes;
    bool has_timestamp = false;
    int64_t timestamp_ns = 0;
  };
  std::vector<Image> images;      // arrival order == capture order
  size_t failed_count = 0;        // onCaptureFailed, per member
  std::vector<ResultFacts> results_in_order;      // results with no timestamp
  std::map<int64_t, ResultFacts> results_by_time; // results keyed by sensor ts

  bool settled() const {
    return images.size() + failed_count >= expected;
  }
};

struct DeviceBackend;

// Stable callback context. The NDK holds the raw pointer for the lifetime of
// the object it was registered on, so each context is owned by the backend
// and outlives every callback that can reference it (the backend is only
// released after ACameraDevice_close, which guarantees callback quiescence).
struct ListenerCtx {
  std::weak_ptr<DeviceBackend> backend;
};

struct DeviceBackend : std::enable_shared_from_this<DeviceBackend> {
  // Serializes session realization/teardown across the core thread and
  // capture workers without either holding provider state_mutex_.
  // Ordering: configure_mutex before m; state_mutex_ is never taken inside.
  std::mutex configure_mutex;
  // Held by a capture worker across a whole device capture (all members), so
  // two captures on one device cannot contend for the single still waiter.
  std::mutex still_capture_mutex;

  std::mutex m;

  ACameraDevice* device = nullptr;
  ACameraCaptureSession* session = nullptr;
  ACaptureSessionOutputContainer* output_container = nullptr;

  ACaptureSessionOutput* stream_output = nullptr;
  ACaptureSessionOutput* still_output = nullptr;
  AImageReader* stream_reader = nullptr;
  AImageReader* still_reader = nullptr;
  ANativeWindow* stream_window = nullptr;
  ANativeWindow* still_window = nullptr;
  ACameraOutputTarget* stream_target = nullptr;
  ACaptureRequest* repeating_request = nullptr;

  // Currently realized session output set.
  bool cfg_has_stream = false;
  uint32_t cfg_stream_w = 0;
  uint32_t cfg_stream_h = 0;
  bool cfg_has_still = false;
  uint32_t cfg_still_w = 0;
  uint32_t cfg_still_h = 0;
  bool repeating_active = false;

  std::unique_ptr<ListenerCtx> device_ctx;
  std::unique_ptr<ListenerCtx> session_ctx;
  std::unique_ptr<ListenerCtx> stream_reader_ctx;
  std::unique_ptr<ListenerCtx> still_reader_ctx;
  std::unique_ptr<ListenerCtx> capture_ctx;

  std::string hardware_id;
  uint64_t device_instance_id = 0;
  uint64_t root_id = 0;
  uint64_t acquisition_session_id = 0; // core-issued native id once realized
  CBProviderStrand* strand = nullptr;  // provider outlives all backends

  StaticCharacteristics chars{};

  bool closed = false; // set before Camera2 objects are released
  bool failed = false;
  std::atomic<uint64_t> image_arrived_count{0};

  std::shared_ptr<StreamProduction> stream;
  std::shared_ptr<BurstCollector> burst; // non-null only during a capture

  // Caller holds m. Latches backend failure and posts the truthful facts.
  void latch_failure_locked(ProviderError error) {
    if (failed) {
      return;
    }
    failed = true;
    if (strand) {
      strand->post_device_error(device_instance_id, error);
      if (stream && stream->producing) {
        stream->producing = false;
        strand->post_stream_error(stream->stream_id, error);
        strand->post_stream_stopped(stream->stream_id, error);
      }
    }
  }
};

// Frame release leases: FrameView.release must stay valid on any thread and
// with any provider-side storage teardown ordering, so each posted frame owns
// its backing through a heap lease (matches SyntheticProvider's pattern).
struct StreamFrameLease {
  std::shared_ptr<StreamProduction::BufferSlot> slot;
};

struct CaptureFrameLease {
  std::shared_ptr<std::vector<uint8_t>> bytes;
};

namespace {

void release_stream_frame(void* user, const FrameView* /*frame*/) {
  auto* lease = static_cast<StreamFrameLease*>(user);
  if (!lease) return;
  if (lease->slot) {
    lease->slot->in_use.store(false, std::memory_order_release);
  }
  delete lease;
}

void release_capture_frame(void* user, const FrameView* /*frame*/) {
  delete static_cast<CaptureFrameLease*>(user);
}

// Acquisition timing from AImage_getTimestamp, which carries the Camera2
// SENSOR_TIMESTAMP for that exact image: nanoseconds, marking the start of
// exposure of the first row.
//
// Comparability follows the device's declared timestamp source and nothing
// else. REALTIME means the mark shares the boot-time base every camera on the
// handset uses, so it is comparable across this provider's devices; UNKNOWN
// means a device-private monotonic base, which supports no claim wider than
// the one device. Never widen this on the assumption that two cameras on one
// phone must share a clock.
std::optional<SourcedFact<ImageAcquisitionTiming>> make_acquisition_timing(
    int64_t sensor_timestamp_ns, bool timestamp_source_realtime) {
  if (sensor_timestamp_ns < 0) {
    return std::nullopt;
  }
  const auto tick_period = TickPeriod::create(1, 1); // marks are nanoseconds
  if (!tick_period) {
    return std::nullopt;
  }
  const auto timing = ImageAcquisitionTiming::create(
      sensor_timestamp_ns,
      *tick_period,
      ImageAcquisitionClockDomain::PROVIDER_MONOTONIC,
      ImageAcquisitionReferenceEvent::EXPOSURE_START,
      timestamp_source_realtime ? ImageAcquisitionComparability::SAME_PROVIDER
                                : ImageAcquisitionComparability::SAME_DEVICE);
  if (!timing) {
    return std::nullopt;
  }
  return SourcedFact<ImageAcquisitionTiming>{*timing, FactOrigin::NATIVE_REPORTED};
}

// Routes one arrived stream image into the repeating stream pool. Caller
// holds backend.m. Still captures never come through here; they have their
// own reader and waiter.
void deliver_stream_image_locked(DeviceBackend& backend, AImage* image) {
  StreamProduction* s = backend.stream.get();
  if (!s || !s->producing || !backend.strand) {
    return;
  }

  std::shared_ptr<StreamProduction::BufferSlot> slot;
  const size_t n = s->pool.size();
  for (size_t probe = 0; probe < n; ++probe) {
    const size_t idx = (s->cursor + probe) % n;
    auto& cand = s->pool[idx];
    bool expected = false;
    if (cand && cand->in_use.compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
      slot = cand;
      s->cursor = (idx + 1) % n;
      break;
    }
  }
  if (!slot) {
    ++s->pool_exhausted_drops;
    if ((s->pool_exhausted_drops & (s->pool_exhausted_drops - 1)) == 0) {
      log_line("stream=%llu frame pool exhausted (drops=%llu)",
               static_cast<unsigned long long>(s->stream_id),
               static_cast<unsigned long long>(s->pool_exhausted_drops));
    }
    return; // repeating frames are lossy
  }

  if (!convert_acquired_image(image, s->width, s->height, s->fourcc, slot->bytes.data())) {
    slot->in_use.store(false, std::memory_order_release);
    ++s->convert_failures;
    if ((s->convert_failures & (s->convert_failures - 1)) == 0) {
      log_line("stream=%llu frame convert failed (expected %ux%u, failures=%llu)",
               static_cast<unsigned long long>(s->stream_id), s->width, s->height,
               static_cast<unsigned long long>(s->convert_failures));
    }
    return;
  }
  ++s->frames_posted;

  int64_t timestamp_ns = -1;
  if (AImage_getTimestamp(image, &timestamp_ns) != AMEDIA_OK) {
    timestamp_ns = -1;
  }

  FrameView fv{};
  fv.device_instance_id = s->device_instance_id;
  fv.stream_id = s->stream_id;
  fv.acquisition_session_id = s->acquisition_session_id;
  fv.capture_id = 0;
  fv.width = s->width;
  fv.height = s->height;
  fv.format_fourcc = s->fourcc;
  if (timestamp_ns >= 0) {
    fv.acquisition_timing =
        make_acquisition_timing(timestamp_ns, backend.chars.timestamp_source_realtime);
  }
  fv.data = slot->bytes.data();
  fv.size_bytes = slot->bytes.size();
  fv.stride_bytes = s->width * 4u;
  fv.requested_retained_plan = s->plan;
  fv.release = &release_stream_frame;
  fv.release_user = new StreamFrameLease{slot};
  backend.strand->post_frame(fv);
}

// ---- NDK callbacks --------------------------------------------------------

void on_device_disconnected(void* context, ACameraDevice* /*device*/) {
  auto* ctx = static_cast<ListenerCtx*>(context);
  if (!ctx) return;
  std::shared_ptr<DeviceBackend> backend = ctx->backend.lock();
  if (!backend) return;
  std::lock_guard<std::mutex> bl(backend->m);
  if (backend->closed) return;
  backend->latch_failure_locked(ProviderError::ERR_TRANSIENT_FAILURE);
}

void on_device_error(void* context, ACameraDevice* /*device*/, int error) {
  auto* ctx = static_cast<ListenerCtx*>(context);
  if (!ctx) return;
  std::shared_ptr<DeviceBackend> backend = ctx->backend.lock();
  if (!backend) return;
  std::lock_guard<std::mutex> bl(backend->m);
  if (backend->closed) return;
  const ProviderError mapped =
      (error == ERROR_CAMERA_DISABLED || error == ERROR_CAMERA_IN_USE ||
       error == ERROR_MAX_CAMERAS_IN_USE)
          ? ProviderError::ERR_BUSY
          : ProviderError::ERR_PROVIDER_FAILED;
  backend->latch_failure_locked(mapped);
}

void on_session_closed(void* /*context*/, ACameraCaptureSession* /*session*/) {}
void on_session_ready(void* /*context*/, ACameraCaptureSession* /*session*/) {}
void on_session_active(void* /*context*/, ACameraCaptureSession* /*session*/) {}

void on_stream_image_available(void* context, AImageReader* reader) {
  auto* ctx = static_cast<ListenerCtx*>(context);
  if (!ctx || !reader) return;
  std::shared_ptr<DeviceBackend> backend = ctx->backend.lock();
  if (!backend) return;
  backend->image_arrived_count.fetch_add(1, std::memory_order_relaxed);

  // Latest-only: repeating frames are lossy by contract, and taking the
  // newest keeps the preview from lagging behind the sensor when Core is
  // slower than the camera.
  AImage* image = nullptr;
  if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK || !image) {
    return;
  }
  {
    std::lock_guard<std::mutex> bl(backend->m);
    if (!backend->closed) {
      deliver_stream_image_locked(*backend, image);
    }
  }
  // The delivered bytes were copied into a pool slot above, so the AImage is
  // returned to the reader immediately rather than pinned for the frame's
  // retained lifetime.
  AImage_delete(image);
}

void on_still_image_available(void* context, AImageReader* reader) {
  auto* ctx = static_cast<ListenerCtx*>(context);
  if (!ctx || !reader) return;
  std::shared_ptr<DeviceBackend> backend = ctx->backend.lock();
  if (!backend) return;

  AImage* image = nullptr;
  if (AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK || !image) {
    return;
  }

  std::shared_ptr<BurstCollector> burst;
  {
    std::lock_guard<std::mutex> bl(backend->m);
    burst = backend->burst;
  }
  if (!burst) {
    // No capture is collecting (a late image from an abandoned burst). Drop it
    // rather than letting it satisfy some future member's wait.
    AImage_delete(image);
    return;
  }

  auto bytes = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(burst->width) * burst->height * 4u);
  const bool converted =
      convert_acquired_image(image, burst->width, burst->height, burst->fourcc,
                             bytes->data());
  int64_t timestamp_ns = -1;
  if (AImage_getTimestamp(image, &timestamp_ns) != AMEDIA_OK) {
    timestamp_ns = -1;
  }
  AImage_delete(image);

  {
    std::lock_guard<std::mutex> wl(burst->m);
    if (burst->images.size() + burst->failed_count >= burst->expected) {
      return; // burst already satisfied; this image belongs to nobody
    }
    if (converted) {
      BurstCollector::Image item;
      item.bytes = std::move(bytes);
      item.has_timestamp = (timestamp_ns >= 0);
      item.timestamp_ns = timestamp_ns;
      burst->images.push_back(std::move(item));
    } else {
      ++burst->failed_count;
    }
  }
  burst->cv.notify_all();
}

void extract_result_facts(const ACameraMetadata* result, ResultFacts& out) {
  if (!result) return;
  ACameraMetadata_const_entry entry{};
  if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_EXPOSURE_TIME, &entry) ==
          ACAMERA_OK && entry.count >= 1) {
    out.has_exposure_ns = true;
    out.exposure_ns = entry.data.i64[0];
  }
  if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_SENSITIVITY, &entry) ==
          ACAMERA_OK && entry.count >= 1) {
    out.has_iso = true;
    out.iso = entry.data.i32[0];
  }
  if (ACameraMetadata_getConstEntry(result, ACAMERA_LENS_APERTURE, &entry) ==
          ACAMERA_OK && entry.count >= 1) {
    out.has_aperture = true;
    out.aperture = entry.data.f[0];
  }
  if (ACameraMetadata_getConstEntry(result, ACAMERA_LENS_FOCAL_LENGTH, &entry) ==
          ACAMERA_OK && entry.count >= 1) {
    out.has_focal_length_mm = true;
    out.focal_length_mm = entry.data.f[0];
  }
  if (ACameraMetadata_getConstEntry(result, ACAMERA_LENS_FOCUS_DISTANCE, &entry) ==
          ACAMERA_OK && entry.count >= 1) {
    out.has_focus_diopters = true;
    out.focus_diopters = entry.data.f[0];
  }
  if (ACameraMetadata_getConstEntry(result, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION,
                                    &entry) == ACAMERA_OK && entry.count >= 1) {
    out.has_ae_comp_steps = true;
    out.ae_comp_steps = entry.data.i32[0];
  }
  if (ACameraMetadata_getConstEntry(result, ACAMERA_LENS_INTRINSIC_CALIBRATION, &entry) ==
          ACAMERA_OK && entry.count >= 5) {
    out.has_intrinsics = true;
    for (int i = 0; i < 5; ++i) out.intrinsics[i] = entry.data.f[i];
  }
  if (ACameraMetadata_getConstEntry(result, ACAMERA_LENS_DISTORTION, &entry) == ACAMERA_OK &&
      entry.count >= 5) {
    out.has_distortion = true;
    for (int i = 0; i < 5; ++i) out.distortion[i] = entry.data.f[i];
  }
  if (ACameraMetadata_getConstEntry(result, ACAMERA_DISTORTION_CORRECTION_MODE, &entry) ==
          ACAMERA_OK && entry.count >= 1) {
    out.has_distortion_correction_mode = true;
    out.distortion_correction_mode = entry.data.u8[0];
  }
  if (ACameraMetadata_getConstEntry(result, ACAMERA_SENSOR_TIMESTAMP, &entry) == ACAMERA_OK &&
      entry.count >= 1) {
    out.has_sensor_timestamp = true;
    out.sensor_timestamp_ns = entry.data.i64[0];
  }
}

void on_capture_completed(void* context,
                          ACameraCaptureSession* /*session*/,
                          ACaptureRequest* /*request*/,
                          const ACameraMetadata* result) {
  auto* ctx = static_cast<ListenerCtx*>(context);
  if (!ctx) return;
  std::shared_ptr<DeviceBackend> backend = ctx->backend.lock();
  if (!backend) return;
  std::shared_ptr<BurstCollector> burst;
  {
    std::lock_guard<std::mutex> bl(backend->m);
    burst = backend->burst;
  }
  if (!burst) return;
  ResultFacts facts{};
  extract_result_facts(result, facts);
  {
    std::lock_guard<std::mutex> wl(burst->m);
    // Keyed by sensor timestamp so the right result reaches the right image
    // even with several captures in flight. A device that omits the timestamp
    // cannot be correlated that way, so those results stay in arrival order
    // and are only used when the burst is a single capture.
    if (facts.has_sensor_timestamp) {
      burst->results_by_time[facts.sensor_timestamp_ns] = facts;
    } else {
      burst->results_in_order.push_back(facts);
    }
  }
  burst->cv.notify_all();
}

void on_capture_failed(void* context,
                       ACameraCaptureSession* /*session*/,
                       ACaptureRequest* /*request*/,
                       ACameraCaptureFailure* /*failure*/) {
  auto* ctx = static_cast<ListenerCtx*>(context);
  if (!ctx) return;
  std::shared_ptr<DeviceBackend> backend = ctx->backend.lock();
  if (!backend) return;
  std::shared_ptr<BurstCollector> burst;
  {
    std::lock_guard<std::mutex> bl(backend->m);
    burst = backend->burst;
  }
  if (!burst) return;
  {
    // One member of the burst failed; the rest may still deliver. Counting it
    // (rather than failing the whole collector) is what lets the wait settle
    // instead of riding out the full timeout.
    std::lock_guard<std::mutex> wl(burst->m);
    ++burst->failed_count;
  }
  burst->cv.notify_all();
}

// ---- Characteristics ------------------------------------------------------

void read_static_characteristics(const ACameraMetadata* meta, StaticCharacteristics& out) {
  if (!meta) return;
  ACameraMetadata_const_entry entry{};

  if (ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_FACING, &entry) == ACAMERA_OK &&
      entry.count >= 1) {
    out.has_facing = true;
    out.facing = entry.data.u8[0];
  }
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_ORIENTATION, &entry) == ACAMERA_OK &&
      entry.count >= 1) {
    out.has_orientation = true;
    out.orientation_degrees = entry.data.i32[0];
  }
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE, &entry) ==
          ACAMERA_OK && entry.count >= 1) {
    out.timestamp_source_realtime =
        (entry.data.u8[0] == ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME);
  }
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, &entry) ==
      ACAMERA_OK) {
    for (uint32_t i = 0; i < entry.count; ++i) {
      switch (entry.data.u8[i]) {
        case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA:
          out.is_logical_multi_camera = true;
          break;
        case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_BURST_CAPTURE:
          out.burst_capture_supported = true;
          break;
        case ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR:
          out.manual_sensor_supported = true;
          break;
        default:
          break;
      }
    }
  }
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_SYNC_MAX_LATENCY, &entry) == ACAMERA_OK &&
      entry.count >= 1) {
    out.has_sync_max_latency = true;
    out.sync_max_latency = entry.data.i32[0];
  }
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_CONTROL_AE_LOCK_AVAILABLE, &entry) ==
          ACAMERA_OK && entry.count >= 1) {
    out.ae_lock_available = (entry.data.u8[0] == ACAMERA_CONTROL_AE_LOCK_AVAILABLE_TRUE);
  }
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_CONTROL_AWB_LOCK_AVAILABLE, &entry) ==
          ACAMERA_OK && entry.count >= 1) {
    out.awb_lock_available = (entry.data.u8[0] == ACAMERA_CONTROL_AWB_LOCK_AVAILABLE_TRUE);
  }
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE, &entry) ==
          ACAMERA_OK && entry.count >= 2) {
    out.has_exposure_time_range = true;
    out.exposure_time_min_ns = entry.data.i64[0];
    out.exposure_time_max_ns = entry.data.i64[1];
  }
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE, &entry) ==
          ACAMERA_OK && entry.count >= 2) {
    out.has_sensitivity_range = true;
    out.sensitivity_min = entry.data.i32[0];
    out.sensitivity_max = entry.data.i32[1];
  }
  // Exactly one available value is Camera2's description of a fixed optic.
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS,
                                    &entry) == ACAMERA_OK && entry.count == 1) {
    out.has_single_focal_length_mm = true;
    out.single_focal_length_mm = entry.data.f[0];
  }
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_INFO_AVAILABLE_APERTURES, &entry) ==
          ACAMERA_OK && entry.count == 1) {
    out.has_single_aperture = true;
    out.single_aperture = entry.data.f[0];
  }
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE,
                                    &entry) == ACAMERA_OK && entry.count >= 1) {
    out.fixed_focus_at_infinity = (entry.data.f[0] == 0.0f);
  }
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
                                    &entry) == ACAMERA_OK && entry.count >= 1) {
    out.focus_distance_is_metric =
        (entry.data.u8[0] != ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED);
  }

  int32_t comp_min = 0, comp_max = 0;
  bool have_range = false;
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, &entry) ==
          ACAMERA_OK && entry.count >= 2) {
    comp_min = entry.data.i32[0];
    comp_max = entry.data.i32[1];
    have_range = (comp_max > comp_min);
  }
  if (have_range &&
      ACameraMetadata_getConstEntry(meta, ACAMERA_CONTROL_AE_COMPENSATION_STEP, &entry) ==
          ACAMERA_OK && entry.count >= 1) {
    const ACameraMetadata_rational step = entry.data.r[0];
    if (step.denominator != 0) {
      const double step_ev =
          static_cast<double>(step.numerator) / static_cast<double>(step.denominator);
      if (step_ev > 0.0) {
        out.has_exposure_compensation = true;
        out.ae_comp_min_steps = comp_min;
        out.ae_comp_max_steps = comp_max;
        out.ae_comp_step_ev = step_ev;
      }
    }
  }

  if (ACameraMetadata_getConstEntry(meta, ACAMERA_DISTORTION_CORRECTION_AVAILABLE_MODES,
                                    &entry) == ACAMERA_OK) {
    for (uint32_t i = 0; i < entry.count; ++i) {
      if (entry.data.u8[i] != ACAMERA_DISTORTION_CORRECTION_MODE_OFF) {
        out.distortion_correction_supported = true;
        break;
      }
    }
  }

  // Both sensor rectangles are {x, y, width, height}; only the size matters as
  // an intrinsics reference frame.
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE,
                                    &entry) == ACAMERA_OK && entry.count >= 4 &&
      entry.data.i32[2] > 0 && entry.data.i32[3] > 0) {
    out.has_pre_correction_array = true;
    out.pre_correction_array_w = static_cast<uint32_t>(entry.data.i32[2]);
    out.pre_correction_array_h = static_cast<uint32_t>(entry.data.i32[3]);
  }
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, &entry) ==
          ACAMERA_OK && entry.count >= 4 && entry.data.i32[2] > 0 && entry.data.i32[3] > 0) {
    out.has_active_array = true;
    out.active_array_w = static_cast<uint32_t>(entry.data.i32[2]);
    out.active_array_h = static_cast<uint32_t>(entry.data.i32[3]);
  }

  if (ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_POSE_TRANSLATION, &entry) ==
          ACAMERA_OK && entry.count >= 3) {
    out.has_pose_translation = true;
    for (int i = 0; i < 3; ++i) out.pose_translation_m[i] = entry.data.f[i];
  }
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_POSE_ROTATION, &entry) == ACAMERA_OK &&
      entry.count >= 4) {
    out.has_pose_rotation = true;
    for (int i = 0; i < 4; ++i) out.pose_rotation_xyzw[i] = entry.data.f[i];
  }
  if (ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_POSE_REFERENCE, &entry) == ACAMERA_OK &&
      entry.count >= 1) {
    out.has_pose_reference = true;
    out.pose_reference = entry.data.u8[0];
  }

  if (ACameraMetadata_getConstEntry(meta, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                                    &entry) == ACAMERA_OK) {
    // Packed as {format, width, height, input?} quadruples.
    for (uint32_t i = 0; i + 3 < entry.count; i += 4) {
      const int32_t format = entry.data.i32[i];
      const int32_t width = entry.data.i32[i + 1];
      const int32_t height = entry.data.i32[i + 2];
      const int32_t is_input = entry.data.i32[i + 3];
      if (is_input == ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT &&
          format == AIMAGE_FORMAT_YUV_420_888 && width > 0 && height > 0) {
        out.supported_yuv_sizes.emplace_back(static_cast<uint32_t>(width),
                                             static_cast<uint32_t>(height));
      }
    }
  }
}

} // namespace

} // namespace camera2_detail

// ---------------------------------------------------------------------------
// Camera2CameraProvider
// ---------------------------------------------------------------------------

using camera2_detail::BoundedControlExecutor;
using camera2_detail::CaptureFrameLease;
using camera2_detail::DeviceBackend;
using camera2_detail::ListenerCtx;
using camera2_detail::ResultFacts;
using camera2_detail::StaticCharacteristics;
using camera2_detail::BurstCollector;
using camera2_detail::StreamProduction;

struct Camera2CameraProvider::CapturedMemberFrame {
  bool ok = false;
  ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
  std::shared_ptr<std::vector<uint8_t>> bytes;
  bool has_timestamp = false;
  int64_t timestamp_ns = 0;
  bool has_facts = false;
  ResultFacts facts{};
};

struct Camera2CameraProvider::MemberRequestSpec {
  // Manual: AE off, this exact exposure/sensitivity. The bracket is then exact
  // by construction with nothing to converge, which is also what makes
  // bursting safe.
  bool manual = false;
  int64_t exposure_ns = 0;
  int32_t sensitivity = 0;
  // Auto: bias the AE target by this many compensation steps. Retained for
  // devices without MANUAL_SENSOR; measured to land well short of the request.
  bool apply_ae_compensation = false;
  int32_t ae_comp_steps = 0;
};

namespace {

// ACameraManager held in a shared_ptr<void> so no platform type appears in the
// header.
std::shared_ptr<void> make_managed_camera_manager() {
  ACameraManager* raw = ACameraManager_create();
  if (!raw) {
    return nullptr;
  }
  return std::shared_ptr<void>(raw, [](void* p) {
    if (p) ACameraManager_delete(static_cast<ACameraManager*>(p));
  });
}

ACameraManager* as_manager(const std::shared_ptr<void>& handle) noexcept {
  return static_cast<ACameraManager*>(handle.get());
}

const char* facing_label(uint8_t facing) noexcept {
  switch (facing) {
    case ACAMERA_LENS_FACING_FRONT: return "front";
    case ACAMERA_LENS_FACING_BACK: return "back";
    case ACAMERA_LENS_FACING_EXTERNAL: return "external";
    default: return "unknown";
  }
}

} // namespace

Camera2CameraProvider::~Camera2CameraProvider() {
  if (initialized_.load(std::memory_order_acquire)) {
    (void)shutdown();
  }
}

ProviderAccessStatus Camera2CameraProvider::check_access_readiness() noexcept {
  ACameraManager* manager = ACameraManager_create();
  if (!manager) {
    return ProviderAccessStatus::failure(
        ProviderAccessCode::CheckFailed, "camera_manager_unavailable", true);
  }
  ACameraIdList* ids = nullptr;
  const camera_status_t st = ACameraManager_getCameraIdList(manager, &ids);
  const int num_cameras = (st == ACAMERA_OK && ids) ? ids->numCameras : 0;
  if (ids) {
    ACameraManager_deleteCameraIdList(ids);
  }
  ACameraManager_delete(manager);

  if (st != ACAMERA_OK) {
    return ProviderAccessStatus::failure(
        ProviderAccessCode::CheckFailed, "camera_id_list_failed", true);
  }
  if (num_cameras <= 0) {
    return ProviderAccessStatus::failure(
        ProviderAccessCode::AccessUnavailable, "no_camera_devices", false);
  }
  // Deliberately Ready even though the CAMERA runtime permission has not been
  // observed: the NDK offers no way to observe it, and claiming a denial we
  // cannot see would mask real hardware from an app that does hold it. A
  // genuine denial surfaces at open_device().
  return ProviderAccessStatus::ready("android_camera2_ready");
}

StreamTemplate Camera2CameraProvider::stream_template() const {
  // Deterministic default; no backend I/O (brief §2). 640x480 is in Camera2's
  // guaranteed YUV_420_888 output set at every supported hardware level.
  StreamTemplate t{};
  t.profile.width = 640;
  t.profile.height = 480;
  t.profile.format_fourcc = FOURCC_RGBA;
  t.profile.target_fps_min = 0;
  t.profile.target_fps_max = 30;
  t.picture = PictureConfig{};
  return t;
}

CaptureTemplate Camera2CameraProvider::capture_template() const {
  CaptureTemplate t{};
  t.profile = stream_template().profile;
  t.picture = PictureConfig{};
  return t;
}

ProducerBackingCapabilities Camera2CameraProvider::stream_backing_capabilities(
    const CaptureProfile& /*profile*/,
    const PictureConfig& /*picture*/) const noexcept {
  return ProducerBackingCapabilities{true, false, false};
}

ProducerBackingCapabilities Camera2CameraProvider::capture_backing_capabilities(
    const CaptureRequest& /*req*/) const noexcept {
  return ProducerBackingCapabilities{true, false, false};
}

uint64_t Camera2CameraProvider::alloc_native_id_(NativeObjectType type) {
  return callbacks_ ? callbacks_->allocate_native_id(type) : 0;
}

void Camera2CameraProvider::emit_native_created_(
    uint64_t native_id,
    NativeObjectType type,
    uint64_t root_id,
    uint64_t owner_device_id,
    uint64_t owner_acquisition_session_id,
    uint64_t owner_stream_id) {
  if (!callbacks_ || native_id == 0) {
    return;
  }
  NativeObjectCreateInfo info{};
  info.native_id = native_id;
  info.type = static_cast<uint32_t>(type);
  info.root_id = root_id;
  info.owner_device_instance_id = owner_device_id;
  info.owner_acquisition_session_id = owner_acquisition_session_id;
  info.owner_stream_id = owner_stream_id;
  info.owner_provider_native_id = provider_native_id_;
  info.has_created_ns = true;
  info.created_ns = callbacks_->core_monotonic_now_ns();
  strand_.post_native_object_created(info);
}

void Camera2CameraProvider::emit_native_destroyed_(uint64_t native_id) {
  if (!callbacks_ || native_id == 0) {
    return;
  }
  NativeObjectDestroyInfo info{};
  info.native_id = native_id;
  info.has_destroyed_ns = true;
  info.destroyed_ns = callbacks_->core_monotonic_now_ns();
  strand_.post_native_object_destroyed(info);
}

ProviderResult Camera2CameraProvider::initialize(IProviderCallbacks* callbacks) {
  if (initialized_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (!callbacks) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  manager_ = make_managed_camera_manager();
  if (!manager_) {
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }

  if (!control_.start()) {
    manager_.reset();
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }

  callbacks_ = callbacks;
  if (!strand_.start(callbacks_, "camera2_provider")) {
    callbacks_ = nullptr;
    control_.stop();
    manager_.reset();
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }

  if (!start_capture_executor_()) {
    strand_.stop();
    callbacks_ = nullptr;
    control_.stop();
    manager_.reset();
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }

  provider_native_id_ = alloc_native_id_(NativeObjectType::Provider);
  emit_native_created_(provider_native_id_, NativeObjectType::Provider, 0, 0, 0, 0);

  shutting_down_.store(false, std::memory_order_release);
  initialized_.store(true, std::memory_order_release);
  return ProviderResult::success();
}

ProviderResult Camera2CameraProvider::enumerate_endpoints(
    std::vector<CameraEndpoint>& out_endpoints) {
  if (!initialized_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  struct EnumResult {
    std::vector<CameraEndpoint> endpoints;
    ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
    bool ok = false;
  };
  auto result = std::make_shared<EnumResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  ACameraManager* manager = as_manager(manager_);
  const bool completed = control_.run_bounded(
      [result, manager](const BoundedControlExecutor::AbandonToken& t) {
        EnumResult local;
        ACameraIdList* ids = nullptr;
        const camera_status_t st = ACameraManager_getCameraIdList(manager, &ids);
        if (st != ACAMERA_OK || !ids) {
          local.error = camera2_detail::provider_error_from_camera_status(st);
        } else {
          for (int i = 0; i < ids->numCameras; ++i) {
            const char* id = ids->cameraIds[i];
            if (!id || id[0] == '\0') {
              continue;
            }
            CameraEndpoint ep;
            ep.hardware_id = id;
            // Camera2 has no human-readable device name; compose a stable
            // label from the id and the facing the platform reports.
            const char* facing = "unknown";
            ACameraMetadata* meta = nullptr;
            if (ACameraManager_getCameraCharacteristics(manager, id, &meta) == ACAMERA_OK &&
                meta) {
              ACameraMetadata_const_entry entry{};
              if (ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_FACING, &entry) ==
                      ACAMERA_OK && entry.count >= 1) {
                facing = facing_label(entry.data.u8[0]);
              }
              ACameraMetadata_free(meta);
            }
            ep.name = std::string("Camera ") + id + " (" + facing + ")";
            local.endpoints.push_back(std::move(ep));
          }
          local.ok = true;
        }
        if (ids) {
          ACameraManager_deleteCameraIdList(ids);
        }
        if (!t.abandoned.load(std::memory_order_acquire)) {
          *result = std::move(local);
        }
      },
      token, kControlJobTimeoutMs);
  if (!completed) {
    return ProviderResult::failure(ProviderError::ERR_TIMEOUT);
  }
  if (!result->ok) {
    return ProviderResult::failure(result->error);
  }
  out_endpoints = std::move(result->endpoints);
  return ProviderResult::success();
}

ProviderResult Camera2CameraProvider::open_device(
    const std::string& hardware_id,
    uint64_t device_instance_id,
    uint64_t root_id) {
  if (!initialized_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (hardware_id.empty() || device_instance_id == 0 || root_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  std::lock_guard<std::mutex> state_lock(state_mutex_);
  auto& dev = devices_[device_instance_id];
  if (dev.open) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }

  auto backend = std::make_shared<DeviceBackend>();
  backend->hardware_id = hardware_id;
  backend->device_instance_id = device_instance_id;
  backend->root_id = root_id;
  backend->strand = &strand_;
  // Every NDK callback context is allocated up front and owned by the
  // backend. The NDK keeps the raw pointer for the lifetime of the object it
  // was registered on, and the backend outlives all of them: it is released
  // only after ACameraDevice_close(), which is what guarantees callback
  // quiescence.
  backend->device_ctx = std::make_unique<ListenerCtx>();
  backend->session_ctx = std::make_unique<ListenerCtx>();
  backend->stream_reader_ctx = std::make_unique<ListenerCtx>();
  backend->still_reader_ctx = std::make_unique<ListenerCtx>();
  backend->capture_ctx = std::make_unique<ListenerCtx>();
  backend->device_ctx->backend = backend;
  backend->session_ctx->backend = backend;
  backend->stream_reader_ctx->backend = backend;
  backend->still_reader_ctx->backend = backend;
  backend->capture_ctx->backend = backend;

  struct OpenResult {
    ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
    bool ok = false;
  };
  auto result = std::make_shared<OpenResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  ACameraManager* manager = as_manager(manager_);
  const bool completed = control_.run_bounded(
      [result, backend, manager, hardware_id](const BoundedControlExecutor::AbandonToken& t) {
        OpenResult local;

        // Characteristics first: they are needed by admission and by static
        // facts, and a device whose characteristics cannot be read is not one
        // this provider can honestly configure.
        StaticCharacteristics chars{};
        ACameraMetadata* meta = nullptr;
        camera_status_t st =
            ACameraManager_getCameraCharacteristics(manager, hardware_id.c_str(), &meta);
        if (st != ACAMERA_OK || !meta) {
          local.error = camera2_detail::provider_error_from_camera_status(st);
          if (!t.abandoned.load(std::memory_order_acquire)) {
            *result = local;
          }
          return;
        }
        camera2_detail::read_static_characteristics(meta, chars);
        ACameraMetadata_free(meta);

        ACameraDevice_StateCallbacks device_cbs{};
        device_cbs.context = backend->device_ctx.get();
        device_cbs.onDisconnected = &camera2_detail::on_device_disconnected;
        device_cbs.onError = &camera2_detail::on_device_error;

        ACameraDevice* device = nullptr;
        st = ACameraManager_openCamera(manager, hardware_id.c_str(), &device_cbs, &device);
        if (st != ACAMERA_OK || !device) {
          local.error = camera2_detail::provider_error_from_camera_status(st);
          camera2_detail::log_line("openCamera failed id=%s status=%d",
                                   hardware_id.c_str(), static_cast<int>(st));
          if (device) {
            ACameraDevice_close(device);
          }
          if (!t.abandoned.load(std::memory_order_acquire)) {
            *result = local;
          }
          return;
        }

        if (t.abandoned.load(std::memory_order_acquire)) {
          // Caller gave up: the acquisition is orphaned; release it here.
          ACameraDevice_close(device);
          return;
        }
        {
          std::lock_guard<std::mutex> bl(backend->m);
          backend->device = device;
          backend->chars = std::move(chars);
        }
        local.ok = true;
        *result = local;
      },
      token, kControlJobTimeoutMs);
  if (!completed) {
    return ProviderResult::failure(ProviderError::ERR_TIMEOUT);
  }
  if (!result->ok) {
    return ProviderResult::failure(result->error);
  }

  dev.hardware_id = hardware_id;
  dev.device_instance_id = device_instance_id;
  dev.root_id = root_id;
  dev.open = true;
  dev.stream_id = 0;
  dev.native_id = alloc_native_id_(NativeObjectType::Device);
  dev.backend = backend;

  emit_native_created_(dev.native_id, NativeObjectType::Device, root_id,
                       device_instance_id, 0, 0);
  strand_.post_device_opened(device_instance_id);
  post_static_camera_facts_best_effort_(device_instance_id, backend);
  return ProviderResult::success();
}

void Camera2CameraProvider::post_static_camera_facts_best_effort_(
    uint64_t device_instance_id,
    const std::shared_ptr<DeviceBackend>& backend) {
  if (!backend) {
    return;
  }
  StaticCharacteristics chars;
  {
    std::lock_guard<std::mutex> bl(backend->m);
    chars = backend->chars;
  }

  CameraStaticFacts facts{};

  if (chars.has_facing) {
    switch (chars.facing) {
      case ACAMERA_LENS_FACING_FRONT:
        facts.facing = SourcedFact<CameraFacing>{CameraFacing::FRONT, FactOrigin::NATIVE_REPORTED};
        break;
      case ACAMERA_LENS_FACING_BACK:
        facts.facing = SourcedFact<CameraFacing>{CameraFacing::BACK, FactOrigin::NATIVE_REPORTED};
        break;
      case ACAMERA_LENS_FACING_EXTERNAL:
        facts.facing =
            SourcedFact<CameraFacing>{CameraFacing::EXTERNAL, FactOrigin::NATIVE_REPORTED};
        break;
      default:
        break; // no vocabulary match; omit rather than guess
    }
  }

  // A camera Camera2 enumerates and is not a logical multi-camera is backed by
  // one physical sensor. Derived rather than native-reported -- the platform
  // stated capabilities, not a nature. A logical multi-camera synthesizes its
  // view from several physical cameras, which is neither cleanly PHYSICAL nor
  // VIRTUAL in CamBANG's vocabulary, so no nature is claimed for one.
  if (!chars.is_logical_multi_camera) {
    facts.nature = SourcedFact<CameraNature>{CameraNature::PHYSICAL, FactOrigin::DERIVED};
  }

  if (chars.has_orientation) {
    // ACAMERA_SENSOR_ORIENTATION is documented to be one of 0/90/180/270, so
    // an out-of-vocabulary value is a driver bug, not something to round.
    switch (((chars.orientation_degrees % 360) + 360) % 360) {
      case 0:
        facts.sensor_orientation = SourcedFact<SensorOrientationDegrees>{
            SensorOrientationDegrees::DEGREES_0, FactOrigin::NATIVE_REPORTED};
        break;
      case 90:
        facts.sensor_orientation = SourcedFact<SensorOrientationDegrees>{
            SensorOrientationDegrees::DEGREES_90, FactOrigin::NATIVE_REPORTED};
        break;
      case 180:
        facts.sensor_orientation = SourcedFact<SensorOrientationDegrees>{
            SensorOrientationDegrees::DEGREES_180, FactOrigin::NATIVE_REPORTED};
        break;
      case 270:
        facts.sensor_orientation = SourcedFact<SensorOrientationDegrees>{
            SensorOrientationDegrees::DEGREES_270, FactOrigin::NATIVE_REPORTED};
        break;
      default:
        break;
    }
  }

  // Device-constant optics. Reported here only because the device advertised
  // exactly one possible value; anything with a range is per-capture truth and
  // reaches Core through the per-image tier instead (brief §8).
  if (chars.has_single_focal_length_mm) {
    if (const auto fact = FocalLengthMm::create(
            static_cast<double>(chars.single_focal_length_mm))) {
      facts.focal_length_mm =
          SourcedFact<FocalLengthMm>{*fact, FactOrigin::NATIVE_REPORTED};
    }
  }
  if (chars.has_single_aperture) {
    if (const auto fact =
            ApertureFNumber::create(static_cast<double>(chars.single_aperture))) {
      facts.aperture_f_number =
          SourcedFact<ApertureFNumber>{*fact, FactOrigin::NATIVE_REPORTED};
    }
  }
  if (chars.fixed_focus_at_infinity) {
    // Camera2 documents a zero minimum focus distance as "fixed-focus lens,
    // focused at infinity" -- a lifetime-constant fact, not a reading.
    facts.focus_state =
        SourcedFact<FocusState>{FocusAtInfinity{}, FactOrigin::NATIVE_REPORTED};
  }

  // Physical mounting pose. Static rather than per-image: it describes where
  // this camera sits on the device, which does not change per capture. Both
  // halves are required -- a translation without a rotation is not a pose --
  // and an absent/UNDEFINED reference is reported as unknown rather than
  // guessed, since the numbers are meaningless without knowing what they are
  // measured from.
  if (chars.has_pose_translation && chars.has_pose_rotation) {
    PoseReference reference{PoseReferenceUnknown{}};
    if (chars.has_pose_reference) {
      switch (chars.pose_reference) {
        case ACAMERA_LENS_POSE_REFERENCE_PRIMARY_CAMERA:
          reference = PoseReferencePrimaryCamera{};
          break;
        case ACAMERA_LENS_POSE_REFERENCE_GYROSCOPE:
          reference = PoseReferenceDeviceMotionSensor{};
          break;
        case ACAMERA_LENS_POSE_REFERENCE_AUTOMOTIVE:
          reference = PoseReferenceAutomotive{};
          break;
        default: // UNDEFINED and any future value
          break;
      }
    }
    // Camera2 reports the rotation as a quaternion ordered [x, y, z, w].
    if (const auto pose = CameraPose::create(
            std::move(reference),
            PoseConventionAndroidCamera2{},
            Vec3Meters{static_cast<double>(chars.pose_translation_m[0]),
                       static_cast<double>(chars.pose_translation_m[1]),
                       static_cast<double>(chars.pose_translation_m[2])},
            QuaternionXyzw{static_cast<double>(chars.pose_rotation_xyzw[0]),
                           static_cast<double>(chars.pose_rotation_xyzw[1]),
                           static_cast<double>(chars.pose_rotation_xyzw[2]),
                           static_cast<double>(chars.pose_rotation_xyzw[3])})) {
      facts.pose = SourcedFact<CameraPose>{*pose, FactOrigin::NATIVE_REPORTED};
    }
  }

  if (facts.facing || facts.nature || facts.sensor_orientation || facts.focal_length_mm ||
      facts.aperture_f_number || facts.focus_state || facts.pose) {
    strand_.post_camera_static_facts(device_instance_id, ProviderCameraFacts{facts});
  } else {
    camera2_detail::log_line(
        "static camera facts unavailable for device=%llu (device reports none)",
        static_cast<unsigned long long>(device_instance_id));
  }

  // Calibration keys are optional in Camera2 and absent on many phone
  // cameras, so record which reference frames and pose this device offers.
  // Without it, "no intrinsics in the result" is ambiguous between a device
  // that reports none and a provider that failed to read them.
  camera2_detail::log_line(
      "device=%llu calibration availability: pre_correction_array=%s active_array=%s pose=%s",
      static_cast<unsigned long long>(device_instance_id),
      chars.has_pre_correction_array ? "yes" : "no",
      chars.has_active_array ? "yes" : "no",
      (chars.has_pose_translation && chars.has_pose_rotation) ? "yes" : "no");

  // Bracket-execution capability, recorded per device so the choice between
  // burst submission and manual-sensor bracketing rests on stated capability
  // rather than on one handset's observed behaviour.
  camera2_detail::log_line(
      "device=%llu bracket capability: burst=%s manual_sensor=%s sync_max_latency=%s%d "
      "ae_lock=%s awb_lock=%s exposure_ns=[%lld..%lld] iso=[%d..%d] ae_comp_steps=[%d..%d]",
      static_cast<unsigned long long>(device_instance_id),
      chars.burst_capture_supported ? "yes" : "no",
      chars.manual_sensor_supported ? "yes" : "no",
      chars.has_sync_max_latency ? "" : "absent:",
      chars.has_sync_max_latency ? chars.sync_max_latency : 0,
      chars.ae_lock_available ? "yes" : "no",
      chars.awb_lock_available ? "yes" : "no",
      static_cast<long long>(chars.has_exposure_time_range ? chars.exposure_time_min_ns : 0),
      static_cast<long long>(chars.has_exposure_time_range ? chars.exposure_time_max_ns : 0),
      chars.has_sensitivity_range ? chars.sensitivity_min : 0,
      chars.has_sensitivity_range ? chars.sensitivity_max : 0,
      chars.has_exposure_compensation ? chars.ae_comp_min_steps : 0,
      chars.has_exposure_compensation ? chars.ae_comp_max_steps : 0);
  // Step size decides what the step range actually spans in EV, which is what
  // determines whether a requested bracket is reachable at all on this device
  // or is being clamped to a narrower hardware range.
  if (chars.has_exposure_compensation) {
    camera2_detail::log_line(
        "device=%llu ae_compensation: step_ev=%.4f range_ev=[%.2f..%.2f]",
        static_cast<unsigned long long>(device_instance_id), chars.ae_comp_step_ev,
        chars.ae_comp_step_ev * static_cast<double>(chars.ae_comp_min_steps),
        chars.ae_comp_step_ev * static_cast<double>(chars.ae_comp_max_steps));
  }
}

void Camera2CameraProvider::teardown_session_locked_(
    const std::shared_ptr<DeviceBackend>& backend) {
  // Caller holds backend->configure_mutex.
  if (!backend) {
    return;
  }
  uint64_t session_native_id = 0;
  {
    std::lock_guard<std::mutex> bl(backend->m);
    if (!backend->session && !backend->stream_reader && !backend->still_reader) {
      return;
    }
    session_native_id = backend->acquisition_session_id;
    backend->acquisition_session_id = 0;
    backend->repeating_active = false;
    backend->cfg_has_stream = false;
    backend->cfg_has_still = false;
  }

  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  (void)control_.run_bounded(
      [backend](const BoundedControlExecutor::AbandonToken& /*t*/) {
        ACameraCaptureSession* session = nullptr;
        ACaptureSessionOutputContainer* container = nullptr;
        ACaptureSessionOutput* stream_output = nullptr;
        ACaptureSessionOutput* still_output = nullptr;
        AImageReader* stream_reader = nullptr;
        AImageReader* still_reader = nullptr;
        ACameraOutputTarget* stream_target = nullptr;
        ACaptureRequest* repeating_request = nullptr;
        {
          std::lock_guard<std::mutex> bl(backend->m);
          session = std::exchange(backend->session, nullptr);
          container = std::exchange(backend->output_container, nullptr);
          stream_output = std::exchange(backend->stream_output, nullptr);
          still_output = std::exchange(backend->still_output, nullptr);
          stream_reader = std::exchange(backend->stream_reader, nullptr);
          still_reader = std::exchange(backend->still_reader, nullptr);
          stream_target = std::exchange(backend->stream_target, nullptr);
          repeating_request = std::exchange(backend->repeating_request, nullptr);
          backend->stream_window = nullptr;
          backend->still_window = nullptr;
        }
        // Ordering is load-bearing: stop the flow, close the session (which
        // is what guarantees the HAL has released the reader windows), and
        // only then delete the readers those windows belong to.
        if (session) {
          ACameraCaptureSession_stopRepeating(session);
          ACameraCaptureSession_close(session);
        }
        if (repeating_request) {
          ACaptureRequest_free(repeating_request);
        }
        if (stream_target) {
          ACameraOutputTarget_free(stream_target);
        }
        if (container) {
          if (stream_output) ACaptureSessionOutputContainer_remove(container, stream_output);
          if (still_output) ACaptureSessionOutputContainer_remove(container, still_output);
          ACaptureSessionOutputContainer_free(container);
        }
        if (stream_output) ACaptureSessionOutput_free(stream_output);
        if (still_output) ACaptureSessionOutput_free(still_output);
        if (stream_reader) {
          AImageReader_setImageListener(stream_reader, nullptr);
          AImageReader_delete(stream_reader);
        }
        if (still_reader) {
          AImageReader_setImageListener(still_reader, nullptr);
          AImageReader_delete(still_reader);
        }
      },
      token, kControlJobTimeoutMs);

  if (session_native_id != 0) {
    emit_native_destroyed_(session_native_id);
  }
}

ProviderResult Camera2CameraProvider::ensure_session_configured_(
    const std::shared_ptr<DeviceBackend>& backend,
    bool want_stream,
    uint32_t stream_width,
    uint32_t stream_height,
    bool want_still,
    uint32_t still_width,
    uint32_t still_height) {
  if (!backend) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (!want_stream && !want_still) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  std::lock_guard<std::mutex> configure_lock(backend->configure_mutex);
  {
    std::lock_guard<std::mutex> bl(backend->m);
    if (backend->closed || backend->failed || !backend->device) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    const bool matches = backend->session != nullptr &&
                         backend->cfg_has_stream == want_stream &&
                         backend->cfg_has_still == want_still &&
                         (!want_stream || (backend->cfg_stream_w == stream_width &&
                                           backend->cfg_stream_h == stream_height)) &&
                         (!want_still || (backend->cfg_still_w == still_width &&
                                          backend->cfg_still_h == still_height));
    if (matches) {
      return ProviderResult::success();
    }
    // Rebuilding the session cancels the repeating request, so a live stream
    // pins the current output set. Deterministic refusal, never a silent
    // stream interruption.
    if (backend->repeating_active) {
      return ProviderResult::failure(ProviderError::ERR_PLATFORM_CONSTRAINT);
    }
    // Geometry the device does not actually offer for YUV_420_888 must fail
    // here rather than be silently substituted by the HAL (brief §6).
    if (want_stream && !backend->chars.supports_size(stream_width, stream_height)) {
      camera2_detail::log_line("no supported YUV output matches stream %ux%u",
                               stream_width, stream_height);
      return ProviderResult::failure(ProviderError::ERR_PLATFORM_CONSTRAINT);
    }
    if (want_still && !backend->chars.supports_size(still_width, still_height)) {
      camera2_detail::log_line("no supported YUV output matches still %ux%u",
                               still_width, still_height);
      return ProviderResult::failure(ProviderError::ERR_PLATFORM_CONSTRAINT);
    }
  }

  teardown_session_locked_(backend);

  struct ConfigureResult {
    ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
    bool ok = false;
  };
  auto result = std::make_shared<ConfigureResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const bool completed = control_.run_bounded(
      [result, backend, want_stream, stream_width, stream_height, want_still,
       still_width, still_height](const BoundedControlExecutor::AbandonToken& t) {
        ConfigureResult local;

        ACameraDevice* device = nullptr;
        {
          std::lock_guard<std::mutex> bl(backend->m);
          device = backend->device;
        }
        if (!device) {
          local.error = ProviderError::ERR_BAD_STATE;
          if (!t.abandoned.load(std::memory_order_acquire)) *result = local;
          return;
        }

        AImageReader* stream_reader = nullptr;
        AImageReader* still_reader = nullptr;
        ANativeWindow* stream_window = nullptr;
        ANativeWindow* still_window = nullptr;
        ACaptureSessionOutput* stream_output = nullptr;
        ACaptureSessionOutput* still_output = nullptr;
        ACaptureSessionOutputContainer* container = nullptr;
        ACameraCaptureSession* session = nullptr;

        // Any failure below unwinds everything acquired so far; a partially
        // configured session must never be latched onto the backend.
        const auto unwind = [&]() {
          if (session) ACameraCaptureSession_close(session);
          if (container) ACaptureSessionOutputContainer_free(container);
          if (stream_output) ACaptureSessionOutput_free(stream_output);
          if (still_output) ACaptureSessionOutput_free(still_output);
          if (stream_reader) {
            AImageReader_setImageListener(stream_reader, nullptr);
            AImageReader_delete(stream_reader);
          }
          if (still_reader) {
            AImageReader_setImageListener(still_reader, nullptr);
            AImageReader_delete(still_reader);
          }
        };

        media_status_t ms = AMEDIA_OK;
        camera_status_t cs = ACAMERA_OK;

        if (want_stream) {
          ms = AImageReader_new(static_cast<int32_t>(stream_width),
                                static_cast<int32_t>(stream_height),
                                AIMAGE_FORMAT_YUV_420_888, kStreamReaderMaxImages,
                                &stream_reader);
          if (ms != AMEDIA_OK || !stream_reader) {
            local.error = ProviderError::ERR_PLATFORM_CONSTRAINT;
            unwind();
            if (!t.abandoned.load(std::memory_order_acquire)) *result = local;
            return;
          }
          AImageReader_ImageListener listener{};
          listener.context = backend->stream_reader_ctx.get();
          listener.onImageAvailable = &camera2_detail::on_stream_image_available;
          AImageReader_setImageListener(stream_reader, &listener);
          if (AImageReader_getWindow(stream_reader, &stream_window) != AMEDIA_OK ||
              !stream_window) {
            local.error = ProviderError::ERR_PROVIDER_FAILED;
            unwind();
            if (!t.abandoned.load(std::memory_order_acquire)) *result = local;
            return;
          }
        }
        if (want_still) {
          ms = AImageReader_new(static_cast<int32_t>(still_width),
                                static_cast<int32_t>(still_height),
                                AIMAGE_FORMAT_YUV_420_888, kStillReaderMaxImages,
                                &still_reader);
          if (ms != AMEDIA_OK || !still_reader) {
            local.error = ProviderError::ERR_PLATFORM_CONSTRAINT;
            unwind();
            if (!t.abandoned.load(std::memory_order_acquire)) *result = local;
            return;
          }
          AImageReader_ImageListener listener{};
          listener.context = backend->still_reader_ctx.get();
          listener.onImageAvailable = &camera2_detail::on_still_image_available;
          AImageReader_setImageListener(still_reader, &listener);
          if (AImageReader_getWindow(still_reader, &still_window) != AMEDIA_OK ||
              !still_window) {
            local.error = ProviderError::ERR_PROVIDER_FAILED;
            unwind();
            if (!t.abandoned.load(std::memory_order_acquire)) *result = local;
            return;
          }
        }

        cs = ACaptureSessionOutputContainer_create(&container);
        if (cs != ACAMERA_OK || !container) {
          local.error = camera2_detail::provider_error_from_camera_status(cs);
          unwind();
          if (!t.abandoned.load(std::memory_order_acquire)) *result = local;
          return;
        }
        if (stream_window) {
          if (ACaptureSessionOutput_create(stream_window, &stream_output) != ACAMERA_OK ||
              ACaptureSessionOutputContainer_add(container, stream_output) != ACAMERA_OK) {
            local.error = ProviderError::ERR_PLATFORM_CONSTRAINT;
            unwind();
            if (!t.abandoned.load(std::memory_order_acquire)) *result = local;
            return;
          }
        }
        if (still_window) {
          if (ACaptureSessionOutput_create(still_window, &still_output) != ACAMERA_OK ||
              ACaptureSessionOutputContainer_add(container, still_output) != ACAMERA_OK) {
            local.error = ProviderError::ERR_PLATFORM_CONSTRAINT;
            unwind();
            if (!t.abandoned.load(std::memory_order_acquire)) *result = local;
            return;
          }
        }

        ACameraCaptureSession_stateCallbacks session_cbs{};
        session_cbs.context = backend->session_ctx.get();
        session_cbs.onClosed = &camera2_detail::on_session_closed;
        session_cbs.onReady = &camera2_detail::on_session_ready;
        session_cbs.onActive = &camera2_detail::on_session_active;

        cs = ACameraDevice_createCaptureSession(device, container, &session_cbs, &session);
        if (cs != ACAMERA_OK || !session) {
          local.error = camera2_detail::provider_error_from_camera_status(cs);
          camera2_detail::log_line("createCaptureSession failed status=%d",
                                   static_cast<int>(cs));
          unwind();
          if (!t.abandoned.load(std::memory_order_acquire)) *result = local;
          return;
        }

        if (t.abandoned.load(std::memory_order_acquire)) {
          unwind();
          return;
        }
        {
          std::lock_guard<std::mutex> bl(backend->m);
          if (backend->closed) {
            local.error = ProviderError::ERR_BAD_STATE;
            unwind();
            *result = local;
            return;
          }
          backend->stream_reader = stream_reader;
          backend->still_reader = still_reader;
          backend->stream_window = stream_window;
          backend->still_window = still_window;
          backend->stream_output = stream_output;
          backend->still_output = still_output;
          backend->output_container = container;
          backend->session = session;
          backend->cfg_has_stream = want_stream;
          backend->cfg_stream_w = stream_width;
          backend->cfg_stream_h = stream_height;
          backend->cfg_has_still = want_still;
          backend->cfg_still_w = still_width;
          backend->cfg_still_h = still_height;
        }
        local.ok = true;
        *result = local;
      },
      token, kControlJobTimeoutMs);
  if (!completed) {
    camera2_detail::log_line("session configuration timed out (device=%llu)",
                             static_cast<unsigned long long>(backend->device_instance_id));
    return ProviderResult::failure(ProviderError::ERR_TIMEOUT);
  }
  if (!result->ok) {
    return ProviderResult::failure(result->error);
  }

  // AcquisitionSession native truth: the concretely realized capture session.
  const uint64_t session_id = alloc_native_id_(NativeObjectType::AcquisitionSession);
  uint64_t root_id = 0;
  uint64_t device_instance_id = 0;
  {
    std::lock_guard<std::mutex> bl(backend->m);
    backend->acquisition_session_id = session_id;
    root_id = backend->root_id;
    device_instance_id = backend->device_instance_id;
  }
  emit_native_created_(session_id, NativeObjectType::AcquisitionSession, root_id,
                       device_instance_id, 0, 0);
  return ProviderResult::success();
}

ProviderResult Camera2CameraProvider::close_device(uint64_t device_instance_id) {
  if (!initialized_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  // In-flight capture jobs pin the device (they registered at admission and
  // erase only at terminal). Entry calls are core-thread serialized, so no
  // new admission can slip in after this check within this call.
  {
    std::lock_guard<std::mutex> capture_lock(capture_mutex_);
    for (const auto& [key, gen] : in_flight_captures_) {
      (void)gen;
      if (key.device_instance_id == device_instance_id) {
        return ProviderResult::failure(ProviderError::ERR_BUSY);
      }
    }
  }

  std::lock_guard<std::mutex> state_lock(state_mutex_);
  auto it = devices_.find(device_instance_id);
  if (it == devices_.end() || !it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (it->second.stream_id != 0) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  DeviceState& dev = it->second;
  std::shared_ptr<DeviceBackend> backend = dev.backend;
  if (backend) {
    // Session teardown emits its own AcquisitionSession destruction fact and
    // must complete before the device is closed.
    {
      std::lock_guard<std::mutex> configure_lock(backend->configure_mutex);
      teardown_session_locked_(backend);
    }
    {
      std::lock_guard<std::mutex> bl(backend->m);
      backend->closed = true;
    }
    auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
    (void)control_.run_bounded(
        [backend](const BoundedControlExecutor::AbandonToken& /*t*/) {
          ACameraDevice* device = nullptr;
          {
            std::lock_guard<std::mutex> bl(backend->m);
            device = std::exchange(backend->device, nullptr);
          }
          if (device) {
            // Returns only once the HAL has released the camera and no
            // further device callbacks can fire.
            ACameraDevice_close(device);
          }
        },
        token, kControlJobTimeoutMs);
  }

  dev.open = false;
  dev.backend.reset();
  strand_.post_device_closed(device_instance_id);
  emit_native_destroyed_(dev.native_id);
  dev.native_id = 0;
  return ProviderResult::success();
}

ProviderResult Camera2CameraProvider::create_stream(const StreamRequest& req) {
  if (!initialized_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (req.stream_id == 0 || req.device_instance_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  std::lock_guard<std::mutex> state_lock(state_mutex_);
  auto dev_it = devices_.find(req.device_instance_id);
  if (dev_it == devices_.end() || !dev_it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (dev_it->second.stream_id != 0) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }
  auto& st = streams_[req.stream_id];
  if (st.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  st.req = req;
  st.created = true;
  st.started = false;
  st.native_id = alloc_native_id_(NativeObjectType::Stream);
  dev_it->second.stream_id = req.stream_id;

  uint64_t session_native_id = 0;
  if (dev_it->second.backend) {
    std::lock_guard<std::mutex> bl(dev_it->second.backend->m);
    session_native_id = dev_it->second.backend->acquisition_session_id;
  }
  emit_native_created_(st.native_id, NativeObjectType::Stream, dev_it->second.root_id,
                       req.device_instance_id, session_native_id, req.stream_id);
  strand_.post_stream_created(req.stream_id);
  return ProviderResult::success();
}

ProviderResult Camera2CameraProvider::destroy_stream(uint64_t stream_id) {
  if (!initialized_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (stream_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  std::lock_guard<std::mutex> state_lock(state_mutex_);
  auto st_it = streams_.find(stream_id);
  if (st_it == streams_.end() || !st_it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (st_it->second.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  const uint64_t dev_id = st_it->second.req.device_instance_id;
  auto dev_it = devices_.find(dev_id);
  if (dev_it != devices_.end()) {
    if (dev_it->second.backend) {
      std::lock_guard<std::mutex> bl(dev_it->second.backend->m);
      dev_it->second.backend->stream.reset();
    }
    if (dev_it->second.stream_id == stream_id) {
      dev_it->second.stream_id = 0;
    }
  }

  // Quiescence: posted frames referencing this stream drain before the stream
  // record disappears (slot storage itself is lease-owned and always safe).
  strand_.flush();

  const uint64_t native_id = st_it->second.native_id;
  streams_.erase(st_it);
  strand_.post_stream_destroyed(stream_id);
  emit_native_destroyed_(native_id);
  return ProviderResult::success();
}

ProviderResult Camera2CameraProvider::start_stream(
    uint64_t stream_id,
    const CaptureProfile& profile,
    const PictureConfig& /*picture*/) {
  if (!initialized_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (profile.width == 0 || profile.height == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (profile.format_fourcc != FOURCC_RGBA && profile.format_fourcc != FOURCC_BGRA) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }

  std::lock_guard<std::mutex> state_lock(state_mutex_);
  auto st_it = streams_.find(stream_id);
  if (st_it == streams_.end() || !st_it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (st_it->second.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  auto dev_it = devices_.find(st_it->second.req.device_instance_id);
  if (dev_it == devices_.end() || !dev_it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  DeviceState& dev = dev_it->second;
  if (!dev.backend) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  // Provision the still output alongside the stream at the same geometry, so
  // still capture while streaming works without a session rebuild. A capture
  // at a different geometry is refused while producing; see the header.
  ProviderResult pr = ensure_session_configured_(dev.backend, true, profile.width,
                                                 profile.height, true, profile.width,
                                                 profile.height);
  if (!pr.ok()) {
    return pr;
  }

  StreamState& st = st_it->second;
  st.req.profile = profile;

  uint64_t session_native_id = 0;
  {
    std::lock_guard<std::mutex> bl(dev.backend->m);
    session_native_id = dev.backend->acquisition_session_id;
  }

  auto production = std::make_shared<StreamProduction>();
  production->stream_id = stream_id;
  production->device_instance_id = dev.device_instance_id;
  production->acquisition_session_id = session_native_id;
  production->width = profile.width;
  production->height = profile.height;
  production->fourcc = profile.format_fourcc;
  production->plan = st.req.requested_retained_plan;
  const size_t frame_bytes =
      static_cast<size_t>(profile.width) * static_cast<size_t>(profile.height) * 4u;
  production->pool.reserve(kStreamPoolSlots);
  for (size_t i = 0; i < kStreamPoolSlots; ++i) {
    auto slot = std::make_shared<StreamProduction::BufferSlot>();
    slot->bytes.resize(frame_bytes);
    production->pool.push_back(std::move(slot));
  }

  // Build and submit the repeating request on the control thread; it is a
  // backend call and must stay off the core thread's own stack.
  struct StartResult {
    ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
    bool ok = false;
  };
  auto result = std::make_shared<StartResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  std::shared_ptr<DeviceBackend> backend = dev.backend;
  const bool completed = control_.run_bounded(
      [result, backend](const BoundedControlExecutor::AbandonToken& t) {
        StartResult local;
        ACameraDevice* device = nullptr;
        ACameraCaptureSession* session = nullptr;
        ANativeWindow* window = nullptr;
        {
          std::lock_guard<std::mutex> bl(backend->m);
          device = backend->device;
          session = backend->session;
          window = backend->stream_window;
        }
        if (!device || !session || !window) {
          local.error = ProviderError::ERR_BAD_STATE;
          if (!t.abandoned.load(std::memory_order_acquire)) *result = local;
          return;
        }

        ACaptureRequest* request = nullptr;
        ACameraOutputTarget* target = nullptr;
        camera_status_t cs =
            ACameraDevice_createCaptureRequest(device, TEMPLATE_PREVIEW, &request);
        if (cs != ACAMERA_OK || !request) {
          local.error = camera2_detail::provider_error_from_camera_status(cs);
          if (!t.abandoned.load(std::memory_order_acquire)) *result = local;
          return;
        }
        if (ACameraOutputTarget_create(window, &target) != ACAMERA_OK || !target ||
            ACaptureRequest_addTarget(request, target) != ACAMERA_OK) {
          if (target) ACameraOutputTarget_free(target);
          ACaptureRequest_free(request);
          local.error = ProviderError::ERR_PROVIDER_FAILED;
          if (!t.abandoned.load(std::memory_order_acquire)) *result = local;
          return;
        }

        cs = ACameraCaptureSession_setRepeatingRequest(session, nullptr, 1, &request, nullptr);
        if (cs != ACAMERA_OK) {
          ACameraOutputTarget_free(target);
          ACaptureRequest_free(request);
          local.error = camera2_detail::provider_error_from_camera_status(cs);
          camera2_detail::log_line("setRepeatingRequest failed status=%d",
                                   static_cast<int>(cs));
          if (!t.abandoned.load(std::memory_order_acquire)) *result = local;
          return;
        }

        if (t.abandoned.load(std::memory_order_acquire)) {
          // Caller gave up: stop the flow we just started rather than leaving
          // the camera streaming into a stream nobody will publish.
          ACameraCaptureSession_stopRepeating(session);
          ACameraOutputTarget_free(target);
          ACaptureRequest_free(request);
          return;
        }
        {
          std::lock_guard<std::mutex> bl(backend->m);
          backend->repeating_request = request;
          backend->stream_target = target;
          backend->repeating_active = true;
        }
        local.ok = true;
        *result = local;
      },
      token, kControlJobTimeoutMs);
  if (!completed) {
    return ProviderResult::failure(ProviderError::ERR_TIMEOUT);
  }
  if (!result->ok) {
    return ProviderResult::failure(result->error);
  }

  {
    std::lock_guard<std::mutex> bl(dev.backend->m);
    if (dev.backend->failed || dev.backend->closed) {
      return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
    }
    production->producing = true;
    dev.backend->stream = production;
  }

  st.started = true;
  strand_.post_stream_started(stream_id);
  return ProviderResult::success();
}

ProviderResult Camera2CameraProvider::stop_stream(uint64_t stream_id) {
  if (!initialized_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  std::lock_guard<std::mutex> state_lock(state_mutex_);
  auto st_it = streams_.find(stream_id);
  if (st_it == streams_.end() || !st_it->second.created || !st_it->second.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  auto dev_it = devices_.find(st_it->second.req.device_instance_id);
  bool already_stopped_by_error = false;
  std::shared_ptr<DeviceBackend> backend;
  if (dev_it != devices_.end()) {
    backend = dev_it->second.backend;
  }
  if (backend) {
    {
      std::lock_guard<std::mutex> bl(backend->m);
      auto& stream = backend->stream;
      if (stream && stream->stream_id == stream_id) {
        already_stopped_by_error = !stream->producing;
        stream->producing = false;
      }
    }
    // Stop the repeating flow for real; leaving it running would keep the
    // sensor and the reader busy behind a stream Core considers stopped.
    auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
    (void)control_.run_bounded(
        [backend](const BoundedControlExecutor::AbandonToken& /*t*/) {
          ACameraCaptureSession* session = nullptr;
          {
            std::lock_guard<std::mutex> bl(backend->m);
            session = backend->session;
          }
          if (session) {
            ACameraCaptureSession_stopRepeating(session);
          }
          std::lock_guard<std::mutex> bl(backend->m);
          backend->repeating_active = false;
        },
        token, kControlJobTimeoutMs);
  }

  st_it->second.started = false;
  if (!already_stopped_by_error) {
    // Posting after producing=false under the backend lock guarantees no
    // frame for this stream lands after the stopped fact.
    strand_.post_stream_stopped(stream_id, ProviderError::OK);
  }
  return ProviderResult::success();
}

ProviderResult Camera2CameraProvider::update_stream_retained_production_plan(
    uint64_t stream_id,
    CoreRetainedProductionPlan requested_retained_plan) {
  if (!initialized_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (!requested_retained_plan.valid) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  std::lock_guard<std::mutex> state_lock(state_mutex_);
  auto st_it = streams_.find(stream_id);
  if (st_it == streams_.end() || !st_it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  const ProducerBackingCapabilities caps =
      stream_backing_capabilities(st_it->second.req.profile, st_it->second.req.picture);
  if (!caps.viable(requested_retained_plan.posture)) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  st_it->second.req.requested_retained_plan = requested_retained_plan;

  auto dev_it = devices_.find(st_it->second.req.device_instance_id);
  if (dev_it != devices_.end() && dev_it->second.backend) {
    std::lock_guard<std::mutex> bl(dev_it->second.backend->m);
    auto& stream = dev_it->second.backend->stream;
    if (stream && stream->stream_id == stream_id) {
      stream->plan = requested_retained_plan;
    }
  }
  return ProviderResult::success();
}

ProviderResult Camera2CameraProvider::set_stream_picture_config(
    uint64_t /*stream_id*/, const PictureConfig& /*picture*/) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult Camera2CameraProvider::set_capture_picture_config(
    uint64_t /*device_instance_id*/, const PictureConfig& /*picture*/) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

// ---------------------------------------------------------------------------
// Still capture
// ---------------------------------------------------------------------------

bool Camera2CameraProvider::start_capture_executor_() noexcept {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  capture_admission_closed_ = false;
  capture_stop_requested_ = false;
  capture_queue_.clear();
  capture_active_jobs_ = 0;
  in_flight_captures_.clear();
  try {
    capture_workers_.reserve(kCaptureWorkerCount);
    for (size_t i = 0; i < kCaptureWorkerCount; ++i) {
      capture_workers_.emplace_back([this] { capture_worker_main_(); });
    }
  } catch (...) {
    capture_admission_closed_ = true;
    capture_stop_requested_ = true;
    capture_cv_.notify_all();
    for (auto& w : capture_workers_) {
      if (w.joinable()) w.join();
    }
    capture_workers_.clear();
    return false;
  }
  return true;
}

void Camera2CameraProvider::stop_and_join_capture_executor_() noexcept {
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    capture_admission_closed_ = true;
    capture_stop_requested_ = true;
  }
  capture_cv_.notify_all();
  for (auto& w : capture_workers_) {
    if (w.joinable()) {
      try {
        w.join();
      } catch (...) {
        w.detach();
      }
    }
  }
  capture_workers_.clear();
}

void Camera2CameraProvider::capture_worker_main_() noexcept {
  for (;;) {
    DeviceCaptureJob job;
    {
      std::unique_lock<std::mutex> lock(capture_mutex_);
      capture_cv_.wait(lock, [this] {
        return capture_stop_requested_ || !capture_queue_.empty();
      });
      if (capture_queue_.empty()) {
        return; // stop requested and queue fully drained
      }
      job = std::move(capture_queue_.front());
      capture_queue_.pop_front();
      ++capture_active_jobs_;
    }
    run_device_capture_job_(job);
    {
      std::lock_guard<std::mutex> lock(capture_mutex_);
      --capture_active_jobs_;
      in_flight_captures_.erase(
          InFlightKey{job.request.capture_id, job.request.device_instance_id});
    }
    capture_cv_.notify_all();
  }
}

bool Camera2CameraProvider::capture_burst_(
    const std::shared_ptr<DeviceBackend>& backend,
    uint32_t width,
    uint32_t height,
    uint32_t format_fourcc,
    const std::vector<MemberRequestSpec>& specs,
    std::vector<CapturedMemberFrame>& out_frames) noexcept {
  out_frames.assign(specs.size(), CapturedMemberFrame{});
  if (!backend || specs.empty()) {
    for (auto& f : out_frames) f.error = ProviderError::ERR_BAD_STATE;
    return false;
  }

  auto burst = std::make_shared<BurstCollector>();
  burst->expected = specs.size();
  burst->width = width;
  burst->height = height;
  burst->fourcc = format_fourcc;

  {
    std::lock_guard<std::mutex> bl(backend->m);
    if (backend->closed || backend->failed) {
      for (auto& f : out_frames) f.error = ProviderError::ERR_PROVIDER_FAILED;
      return false;
    }
    backend->burst = burst;
  }
  // Whatever happens below, this capture must stop owning the collector slot,
  // or a later capture would be satisfied by this one's stale images.
  struct CollectorSlotGuard {
    std::shared_ptr<DeviceBackend> backend;
    ~CollectorSlotGuard() {
      std::lock_guard<std::mutex> bl(backend->m);
      backend->burst.reset();
    }
  } collector_guard{backend};

  struct SubmitResult {
    bool ok = false;
    ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
  };
  auto submit = std::make_shared<SubmitResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const bool submitted = control_.run_bounded(
      [submit, backend, &specs](const BoundedControlExecutor::AbandonToken& t) {
        SubmitResult local;
        ACameraDevice* device = nullptr;
        ACameraCaptureSession* session = nullptr;
        ANativeWindow* window = nullptr;
        bool awb_lock_available = false;
        {
          std::lock_guard<std::mutex> bl(backend->m);
          device = backend->device;
          session = backend->session;
          window = backend->still_window;
          awb_lock_available = backend->chars.awb_lock_available;
        }
        if (!device || !session || !window) {
          local.error = ProviderError::ERR_BAD_STATE;
          if (!t.abandoned.load(std::memory_order_acquire)) *submit = local;
          return;
        }

        std::vector<ACaptureRequest*> requests;
        std::vector<ACameraOutputTarget*> targets;
        // Everything acquired here is freed on every path; Camera2 copies what
        // it needs at submission, so none of it outlives this job.
        const auto unwind = [&]() {
          for (ACaptureRequest* r : requests) {
            if (r) ACaptureRequest_free(r);
          }
          for (ACameraOutputTarget* tg : targets) {
            if (tg) ACameraOutputTarget_free(tg);
          }
          requests.clear();
          targets.clear();
        };

        camera_status_t cs = ACAMERA_OK;
        for (const MemberRequestSpec& spec : specs) {
          ACaptureRequest* request = nullptr;
          ACameraOutputTarget* target = nullptr;
          cs = ACameraDevice_createCaptureRequest(device, TEMPLATE_STILL_CAPTURE, &request);
          if (cs != ACAMERA_OK || !request) {
            local.error = camera2_detail::provider_error_from_camera_status(cs);
            unwind();
            if (!t.abandoned.load(std::memory_order_acquire)) *submit = local;
            return;
          }
          requests.push_back(request);
          if (ACameraOutputTarget_create(window, &target) != ACAMERA_OK || !target ||
              ACaptureRequest_addTarget(request, target) != ACAMERA_OK) {
            if (target) ACameraOutputTarget_free(target);
            local.error = ProviderError::ERR_PROVIDER_FAILED;
            unwind();
            if (!t.abandoned.load(std::memory_order_acquire)) *submit = local;
            return;
          }
          targets.push_back(target);

          if (spec.manual) {
            // AE off plus an explicit exposure/sensitivity pair. Nothing has to
            // converge, so this member's exposure is exact and independent of
            // how close in time its neighbours are -- which is precisely what
            // makes bursting safe here and unsafe on the AE path.
            const uint8_t ae_off = ACAMERA_CONTROL_AE_MODE_OFF;
            const int64_t exposure = spec.exposure_ns;
            const int32_t sensitivity = spec.sensitivity;
            if (ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AE_MODE, 1, &ae_off) !=
                    ACAMERA_OK ||
                ACaptureRequest_setEntry_i64(request, ACAMERA_SENSOR_EXPOSURE_TIME, 1,
                                             &exposure) != ACAMERA_OK ||
                ACaptureRequest_setEntry_i32(request, ACAMERA_SENSOR_SENSITIVITY, 1,
                                             &sensitivity) != ACAMERA_OK) {
              local.error = ProviderError::ERR_PLATFORM_CONSTRAINT;
              unwind();
              if (!t.abandoned.load(std::memory_order_acquire)) *submit = local;
              return;
            }
            // Hold white balance across the bracket. Members differing in
            // colour as well as exposure cannot be combined, and Camera2
            // guarantees this lock is available on burst-capable devices.
            if (awb_lock_available) {
              const uint8_t awb_lock = ACAMERA_CONTROL_AWB_LOCK_ON;
              (void)ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AWB_LOCK, 1,
                                                &awb_lock);
            }
          } else if (spec.apply_ae_compensation) {
            const int32_t comp_steps = spec.ae_comp_steps;
            if (ACaptureRequest_setEntry_i32(
                    request, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &comp_steps) !=
                ACAMERA_OK) {
              local.error = ProviderError::ERR_PLATFORM_CONSTRAINT;
              unwind();
              if (!t.abandoned.load(std::memory_order_acquire)) *submit = local;
              return;
            }
          }
        }

        ACameraCaptureSession_captureCallbacks capture_cbs{};
        capture_cbs.context = backend->capture_ctx.get();
        capture_cbs.onCaptureCompleted = &camera2_detail::on_capture_completed;
        capture_cbs.onCaptureFailed = &camera2_detail::on_capture_failed;

        // One submission for the whole bundle: Camera2 runs the requests
        // back-to-back on consecutive sensor frames instead of the caller
        // reintroducing a pipeline round trip between each member.
        cs = ACameraCaptureSession_capture(session, &capture_cbs,
                                           static_cast<int>(requests.size()),
                                           requests.data(), nullptr);
        unwind();
        if (cs != ACAMERA_OK) {
          local.error = camera2_detail::provider_error_from_camera_status(cs);
          camera2_detail::log_line("burst capture submit failed status=%d members=%zu",
                                   static_cast<int>(cs), specs.size());
          if (!t.abandoned.load(std::memory_order_acquire)) *submit = local;
          return;
        }
        local.ok = true;
        if (!t.abandoned.load(std::memory_order_acquire)) *submit = local;
      },
      token, kControlJobTimeoutMs);
  if (!submitted) {
    for (auto& f : out_frames) f.error = ProviderError::ERR_TIMEOUT;
    return false;
  }
  if (!submit->ok) {
    for (auto& f : out_frames) f.error = submit->error;
    return false;
  }

  // Wait for every member's image. Metadata never gates a frame: a device that
  // delivers pixels but no result still produces truthful pixels, just with
  // fewer facts.
  //
  // The wait is confined to this scope. Lock order across the provider is
  // backend->m before burst->m (both NDK callbacks read the collector under
  // backend->m and then take burst->m), and CollectorSlotGuard's destructor
  // takes backend->m -- so burst->m must be released before returning, or the
  // guard would invert that order against a concurrently arriving image.
  std::vector<BurstCollector::Image> images;
  std::map<int64_t, ResultFacts> results_by_time;
  std::vector<ResultFacts> results_in_order;
  bool settled = false;
  {
    std::unique_lock<std::mutex> wl(burst->m);
    settled = burst->cv.wait_for(wl, std::chrono::milliseconds(kCaptureSampleWaitMs),
                                 [&burst] { return burst->settled(); });
    images = burst->images;
    results_by_time = burst->results_by_time;
    results_in_order = burst->results_in_order;
  }

  // Sensor timestamps are monotonic within a device, so ascending timestamp is
  // submission order, which is member order.
  std::sort(images.begin(), images.end(),
            [](const BurstCollector::Image& a, const BurstCollector::Image& b) {
              return a.timestamp_ns < b.timestamp_ns;
            });

  for (size_t i = 0; i < out_frames.size(); ++i) {
    if (i >= images.size()) {
      out_frames[i].error =
          settled ? ProviderError::ERR_PROVIDER_FAILED : ProviderError::ERR_TIMEOUT;
      continue;
    }
    const BurstCollector::Image& img = images[i];
    if (!img.bytes) {
      out_frames[i].error = ProviderError::ERR_PROVIDER_FAILED;
      continue;
    }
    out_frames[i].ok = true;
    out_frames[i].bytes = img.bytes;
    out_frames[i].has_timestamp = img.has_timestamp;
    out_frames[i].timestamp_ns = img.timestamp_ns;

    // Pair by sensor timestamp. Positional pairing is only sound for a single
    // capture, where there is nothing to confuse it with; within a real burst
    // an unmatched result is dropped rather than attached to the wrong image.
    bool paired = false;
    if (img.has_timestamp) {
      const auto it = results_by_time.find(img.timestamp_ns);
      if (it != results_by_time.end()) {
        out_frames[i].has_facts = true;
        out_frames[i].facts = it->second;
        paired = true;
      }
    }
    if (!paired && out_frames.size() == 1) {
      if (!results_in_order.empty()) {
        out_frames[i].has_facts = true;
        out_frames[i].facts = results_in_order.front();
      } else if (!results_by_time.empty()) {
        out_frames[i].has_facts = true;
        out_frames[i].facts = results_by_time.begin()->second;
      }
    }
  }
  return true;
}

bool Camera2CameraProvider::meter_manual_baseline_(
    const std::shared_ptr<DeviceBackend>& backend,
    uint32_t width,
    uint32_t height,
    uint32_t format_fourcc,
    double& out_exposure_ns,
    double& out_sensitivity,
    ProviderError& out_error) noexcept {
  // One ordinary auto-exposed capture. Its realized exposure/sensitivity are
  // the reference the whole bracket is derived from, so this is the only place
  // auto-exposure is paid for -- once per capture rather than once per member.
  std::vector<MemberRequestSpec> specs(1);
  std::vector<CapturedMemberFrame> frames;
  if (!capture_burst_(backend, width, height, format_fourcc, specs, frames) ||
      frames.empty()) {
    out_error = frames.empty() ? ProviderError::ERR_PROVIDER_FAILED : frames[0].error;
    return false;
  }
  const CapturedMemberFrame& metered = frames[0];
  if (!metered.ok) {
    out_error = metered.error;
    return false;
  }
  if (!metered.has_facts || !metered.facts.has_exposure_ns || !metered.facts.has_iso ||
      metered.facts.exposure_ns <= 0 || metered.facts.iso <= 0) {
    // Without a realized baseline there is nothing to derive members from, and
    // inventing one would build the bracket around a fictional exposure.
    out_error = ProviderError::ERR_NOT_SUPPORTED;
    return false;
  }
  out_exposure_ns = static_cast<double>(metered.facts.exposure_ns);
  out_sensitivity = static_cast<double>(metered.facts.iso);
  return true;
}

void Camera2CameraProvider::run_device_capture_job_(const DeviceCaptureJob& job) noexcept {
  const uint64_t capture_id = job.request.capture_id;
  const uint64_t device_id = job.request.device_instance_id;
  bool terminal_posted = false;
  auto fail = [&](ProviderError error) {
    if (!terminal_posted) {
      terminal_posted = true;
      strand_.post_capture_failed(capture_id, device_id, error);
    }
  };

  try {
    // Every admitted device job emits capture_started and exactly one
    // terminal fact, shutdown-cancellation included.
    strand_.post_capture_started(capture_id, device_id);

    bool generation_closed = false;
    {
      std::lock_guard<std::mutex> lock(capture_mutex_);
      generation_closed = (job.generation != capture_generation_);
    }
    if (generation_closed) {
      fail(ProviderError::ERR_SHUTTING_DOWN);
      return;
    }

    // Fetch the backend under a short state lock only; session realization
    // serializes on the backend's own configure mutex so this worker never
    // holds state_mutex_ across bounded backend jobs.
    std::shared_ptr<DeviceBackend> backend;
    {
      std::lock_guard<std::mutex> state_lock(state_mutex_);
      auto dev_it = devices_.find(device_id);
      if (dev_it == devices_.end() || !dev_it->second.open) {
        fail(ProviderError::ERR_BAD_STATE);
        return;
      }
      backend = dev_it->second.backend;
    }
    if (!backend) {
      fail(ProviderError::ERR_BAD_STATE);
      return;
    }

    // One device capture at a time per device: the burst collector is a single
    // slot, and two concurrent captures would steal each other's images.
    std::lock_guard<std::mutex> still_lock(backend->still_capture_mutex);

    bool stream_producing = false;
    uint32_t stream_w = 0, stream_h = 0;
    {
      std::lock_guard<std::mutex> bl(backend->m);
      if (backend->closed || backend->failed) {
        fail(ProviderError::ERR_PROVIDER_FAILED);
        return;
      }
      if (backend->stream && backend->stream->producing) {
        stream_producing = true;
        stream_w = backend->stream->width;
        stream_h = backend->stream->height;
      }
    }

    // While a stream produces, the session output set is pinned; the capture
    // must fit it. Otherwise the session is rebuilt still-only at the
    // requested geometry.
    ProviderResult pr =
        stream_producing
            ? ensure_session_configured_(backend, true, stream_w, stream_h, true,
                                         job.request.width, job.request.height)
            : ensure_session_configured_(backend, false, 0, 0, true, job.request.width,
                                         job.request.height);
    if (!pr.ok()) {
      fail(pr.code);
      return;
    }

    uint64_t session_id = 0;
    StaticCharacteristics chars;
    {
      std::lock_guard<std::mutex> bl(backend->m);
      if (backend->closed || backend->failed) {
        fail(ProviderError::ERR_PROVIDER_FAILED);
        return;
      }
      session_id = backend->acquisition_session_id;
      chars = backend->chars;
    }

    const auto& members = job.request.still_image_bundle.members;

    // Bracket execution has two independent requirements, met by two different
    // mechanisms that only compose in one direction.
    //
    // EXPOSURE ACCURACY. Auto-exposure compensation does not deliver the
    // requested separation. Measured across four cameras on two handsets, a
    // realized +/-2 EV control produced between 0.0 and 1.6 EV of actual image
    // change, sometimes in the wrong direction -- because AE has not converged
    // when the member is taken, and because exposure pins at the frame-duration
    // ceiling so only sensitivity can move. MANUAL_SENSOR removes the problem
    // rather than mitigating it: AE off with an explicit exposure/sensitivity
    // pair per request is exact by construction, with nothing to converge.
    //
    // TEMPORAL COHERENCE. Submitting members one at a time, each waiting for
    // its own image, measured ~234ms apart (~468ms across three members). At
    // that spacing the members no longer describe the same scene -- subjects
    // move, the camera shifts -- so combining them is invalid however exact the
    // exposures are. One burst submission puts them on consecutive sensor
    // frames instead (~33ms apart at 30fps).
    //
    // The two compose in one direction only: manual exposure is what makes
    // bursting safe. Back-to-back members leave AE no time to converge, so
    // bursting the AE path would make exposure accuracy worse; they leave a
    // fixed manual exposure nothing to converge, so it costs nothing.
    //
    // Devices without MANUAL_SENSOR keep the sequential AE path. It is weaker
    // on both counts and known to be so; the realized values published below
    // report that shortfall rather than concealing it.
    const bool manual_bracket_capable = chars.manual_sensor_supported &&
                                        chars.has_exposure_time_range &&
                                        chars.has_sensitivity_range;
    const bool use_manual_burst = manual_bracket_capable && members.size() > 1;

    std::vector<CapturedMemberFrame> frames;
    if (use_manual_burst) {
      double baseline_exposure = 0.0;
      double baseline_sensitivity = 0.0;
      ProviderError meter_error = ProviderError::ERR_PROVIDER_FAILED;
      if (!meter_manual_baseline_(backend, job.request.width, job.request.height,
                                  job.request.format_fourcc, baseline_exposure,
                                  baseline_sensitivity, meter_error)) {
        fail(meter_error);
        return;
      }

      // Each member is baseline * 2^EV. Exposure time absorbs the change
      // first, because lengthening exposure is photometrically cleaner than
      // raising gain; sensitivity takes only the residual exposure could not
      // reach within the device's own range. Both are clamped to the ranges
      // the device reported, so an unreachable request yields the closest
      // achievable exposure rather than a refused capture -- and the realized
      // values published below state what that clamp actually delivered.
      std::vector<MemberRequestSpec> specs;
      specs.reserve(members.size());
      for (const CaptureStillImageMember& member : members) {
        const double ev =
            static_cast<double>(member.intended_exposure_compensation_milli_ev) / 1000.0;
        const double target_scale = std::pow(2.0, ev);

        double exposure = baseline_exposure * target_scale;
        exposure = std::clamp(exposure,
                              static_cast<double>(chars.exposure_time_min_ns),
                              static_cast<double>(chars.exposure_time_max_ns));
        const double exposure_scale =
            baseline_exposure > 0.0 ? (exposure / baseline_exposure) : 1.0;
        const double residual_scale =
            exposure_scale > 0.0 ? (target_scale / exposure_scale) : 1.0;
        double sensitivity = baseline_sensitivity * residual_scale;
        sensitivity = std::clamp(sensitivity,
                                 static_cast<double>(chars.sensitivity_min),
                                 static_cast<double>(chars.sensitivity_max));

        MemberRequestSpec spec;
        spec.manual = true;
        spec.exposure_ns = static_cast<int64_t>(std::llround(exposure));
        spec.sensitivity = static_cast<int32_t>(std::lround(sensitivity));
        specs.push_back(spec);
      }

      if (!capture_burst_(backend, job.request.width, job.request.height,
                          job.request.format_fourcc, specs, frames)) {
        fail(frames.empty() ? ProviderError::ERR_PROVIDER_FAILED : frames[0].error);
        return;
      }
    } else {
      // Sequential auto-exposure fallback: one submission per member, each
      // carrying its own compensation bias clamped to the device's step grid.
      frames.reserve(members.size());
      for (const CaptureStillImageMember& member : members) {
        MemberRequestSpec spec;
        if (member.role == CaptureStillImageMemberRole::ADDITIONAL_BRACKET &&
            chars.has_exposure_compensation) {
          const double requested_ev =
              static_cast<double>(member.intended_exposure_compensation_milli_ev) / 1000.0;
          const long rounded = std::lround(requested_ev / chars.ae_comp_step_ev);
          spec.apply_ae_compensation = true;
          spec.ae_comp_steps = static_cast<int32_t>(
              std::clamp<long>(rounded, chars.ae_comp_min_steps, chars.ae_comp_max_steps));
        }
        const std::vector<MemberRequestSpec> one{spec};
        std::vector<CapturedMemberFrame> single;
        if (!capture_burst_(backend, job.request.width, job.request.height,
                            job.request.format_fourcc, one, single) ||
            single.empty()) {
          fail(single.empty() ? ProviderError::ERR_PROVIDER_FAILED : single[0].error);
          return;
        }
        frames.push_back(single[0]);
      }
    }

    if (frames.size() != members.size()) {
      // Defensive: publishing frames against mismatched member indices would
      // attach one member's pixels to another's identity.
      fail(ProviderError::ERR_PROVIDER_FAILED);
      return;
    }

    // Realized exposure of the default-metered member. The bundle contract
    // guarantees members[0] is DEFAULT_METERED at index 0, and frames are in
    // member order, so this reference exists before any bracket member needs it.
    bool have_baseline_exposure = false;
    double baseline_exposure_ns = 0.0;
    double baseline_iso = 0.0;
    bool baseline_has_aperture = false;
    double baseline_aperture = 0.0;
    int64_t bundle_first_timestamp_ns = 0;
    bool have_bundle_first_timestamp = false;

    for (size_t member_index = 0; member_index < members.size(); ++member_index) {
      const CaptureStillImageMember& member = members[member_index];
      const bool is_bracket =
          member.role == CaptureStillImageMemberRole::ADDITIONAL_BRACKET;

      const CapturedMemberFrame& captured = frames[member_index];
      if (!captured.ok || !captured.bytes) {
        fail(captured.error);
        return;
      }

      // Realized compensation is what the *image* got, not what the control
      // was set to -- that distinction is the whole point of separating
      // requested from realized state. The AE compensation control reads back
      // its clamped set-point (measured at +/-2 EV while the image moved
      // 0.06-1.64 EV), so it is not a realized value and is deliberately not
      // used here.
      //
      // The truthful quantity is the exposure the sensor actually delivered,
      // relative to the default-metered member of this same bundle:
      //
      //   dEV = log2(t/t0) + log2(ISO/ISO0) - 2*log2(N/N0)
      //
      // The aperture term is included only when both members report an
      // f-number; a device that reports none has fixed optics, where the term
      // is zero. Any member missing exposure or sensitivity yields no realized
      // value at all -- absence is the correct report (brief section 8), and a
      // clamped request is fine precisely because this number stays honest
      // about what the clamp actually produced.
      const bool member_has_exposure_facts =
          captured.has_facts && captured.facts.has_exposure_ns &&
          captured.facts.has_iso && captured.facts.exposure_ns > 0 &&
          captured.facts.iso > 0;

      if (!is_bracket && member_has_exposure_facts) {
        have_baseline_exposure = true;
        baseline_exposure_ns = static_cast<double>(captured.facts.exposure_ns);
        baseline_iso = static_cast<double>(captured.facts.iso);
        baseline_has_aperture = captured.facts.has_aperture && captured.facts.aperture > 0.0f;
        baseline_aperture = static_cast<double>(captured.facts.aperture);
      }

      bool has_realized_ev = false;
      int32_t realized_milli_ev = 0;
      if (!is_bracket) {
        // The metered member defines the reference, so its realized offset
        // from that reference is zero by construction.
        if (member_has_exposure_facts) {
          has_realized_ev = true;
          realized_milli_ev = 0;
        }
      } else if (have_baseline_exposure && member_has_exposure_facts) {
        double delta_ev =
            std::log2(static_cast<double>(captured.facts.exposure_ns) / baseline_exposure_ns) +
            std::log2(static_cast<double>(captured.facts.iso) / baseline_iso);
        if (baseline_has_aperture && captured.facts.has_aperture &&
            captured.facts.aperture > 0.0f) {
          delta_ev -=
              2.0 * std::log2(static_cast<double>(captured.facts.aperture) / baseline_aperture);
        }
        if (std::isfinite(delta_ev)) {
          realized_milli_ev = static_cast<int32_t>(std::lround(delta_ev * 1000.0));
          has_realized_ev = true;
        }
      }

      FrameView fv{};
      fv.device_instance_id = device_id;
      fv.stream_id = 0;
      fv.acquisition_session_id = session_id;
      fv.capture_id = capture_id;
      fv.capture_image.routing = is_bracket ? CaptureImageRouting::ADDITIONAL_BRACKET
                                            : CaptureImageRouting::DEFAULT_METERED;
      fv.capture_image.image_member_index = member.image_member_index;
      fv.capture_image.applied_exposure_compensation_milli_ev =
          member.intended_exposure_compensation_milli_ev;
      fv.capture_image.has_realized_exposure_compensation_milli_ev = has_realized_ev;
      fv.capture_image.realized_exposure_compensation_milli_ev = realized_milli_ev;
      fv.width = job.request.width;
      fv.height = job.request.height;
      fv.format_fourcc = job.request.format_fourcc;
      if (captured.has_timestamp) {
        fv.acquisition_timing = camera2_detail::make_acquisition_timing(
            captured.timestamp_ns, chars.timestamp_source_realtime);
      }
      fv.data = captured.bytes->data();
      fv.size_bytes = captured.bytes->size();
      fv.stride_bytes = job.request.width * 4u;
      // Fresh immutable allocation: publish for zero-copy retention (brief §4).
      fv.cpu_payload_owner = captured.bytes;
      fv.requested_retained_plan = job.request.requested_retained_plan;
      fv.release = &camera2_detail::release_capture_frame;
      fv.release_user = new camera2_detail::CaptureFrameLease{captured.bytes};
      strand_.post_frame(fv);

      // Realized per-image camera facts, straight out of this member's own
      // capture result metadata. Camera2 result metadata describes what the
      // sensor did for that frame -- it is not the request's set-point -- so
      // these are realized readings, not restated requests (brief §8). This
      // is a supply-side addition only: Core's precedence (external ADC
      // ingestion > provider per-image > provider static) is untouched.
      if (captured.has_facts) {
        ProviderCaptureImageFacts image_facts{};
        if (captured.facts.has_exposure_ns) {
          if (const auto fact = ExposureTime::create(
                  static_cast<double>(captured.facts.exposure_ns))) {
            image_facts.exposure_time =
                SourcedFact<ExposureTime>{*fact, FactOrigin::NATIVE_REPORTED};
          }
        }
        if (captured.facts.has_iso) {
          if (const auto fact = SensorSensitivityIso::create(
                  static_cast<double>(captured.facts.iso))) {
            image_facts.sensor_sensitivity_iso =
                SourcedFact<SensorSensitivityIso>{*fact, FactOrigin::NATIVE_REPORTED};
          }
        }
        if (captured.facts.has_aperture) {
          if (const auto fact = ApertureFNumber::create(
                  static_cast<double>(captured.facts.aperture))) {
            image_facts.aperture_f_number =
                SourcedFact<ApertureFNumber>{*fact, FactOrigin::NATIVE_REPORTED};
          }
        }
        if (captured.facts.has_focal_length_mm) {
          if (const auto fact = FocalLengthMm::create(
                  static_cast<double>(captured.facts.focal_length_mm))) {
            image_facts.focal_length_mm =
                SourcedFact<FocalLengthMm>{*fact, FactOrigin::NATIVE_REPORTED};
          }
        }
        // Focus distance is reported in diopters, and only devices whose
        // calibration is APPROXIMATE or CALIBRATED report it in real ones. An
        // UNCALIBRATED reading is a hardware-dependent number with no unit, so
        // converting it to metres would be exactly the unit fabrication the
        // brief forbids -- it is omitted instead.
        if (captured.facts.has_focus_diopters && chars.focus_distance_is_metric) {
          const float diopters = captured.facts.focus_diopters;
          if (diopters == 0.0f) {
            image_facts.focus_state =
                SourcedFact<FocusState>{FocusAtInfinity{}, FactOrigin::NATIVE_REPORTED};
          } else if (diopters > 0.0f) {
            if (const auto fact =
                    FocusAtDistance::create(1.0 / static_cast<double>(diopters))) {
              image_facts.focus_state =
                  SourcedFact<FocusState>{*fact, FactOrigin::NATIVE_REPORTED};
            }
          }
        }
        // Intrinsics and distortion are per-image rather than static because
        // the coordinate system they are expressed in is decided per capture
        // by ACAMERA_DISTORTION_CORRECTION_MODE, which is only knowable from
        // that capture's own result metadata:
        //   OFF      -> pre-correction active array, delivered pixels DISTORTED
        //   FAST/HQ  -> active array, delivered pixels RECTIFIED
        // Both arrays are sensor rectangles, not the delivered image. The
        // delivered frame is cropped and scaled from them, so these f/c values
        // are NOT valid in delivered-image pixels. They are published in the
        // sensor domain they were measured in and left there: rescaling them
        // to the output size would be exactly the intrinsic rescaling and
        // coordinate-domain conversion CamBANG does not do. A consumer that
        // needs delivered-image intrinsics must do that conversion itself,
        // knowing the crop it asked for.
        //
        // A result may omit the mode entirely -- real hardware does; the S20
        // reports intrinsics and distortion on every capture but never the
        // correction mode. That omission is only ambiguous for a device that
        // *can* correct. Camera2 documents that a device not supporting the
        // correction API "will always list only OFF", so for a device
        // advertising no correction capability, OFF is derived from stated
        // capability rather than assumed. When the device can correct but did
        // not say whether it did, the domain genuinely is unknowable and both
        // facts are omitted rather than published against a guessed frame.
        const bool correction_mode_known =
            captured.facts.has_distortion_correction_mode ||
            !chars.distortion_correction_supported;
        if (correction_mode_known) {
          const bool corrected =
              captured.facts.has_distortion_correction_mode &&
              captured.facts.distortion_correction_mode !=
                  ACAMERA_DISTORTION_CORRECTION_MODE_OFF;
          const bool have_reference =
              corrected ? chars.has_active_array : chars.has_pre_correction_array;
          const uint32_t ref_w =
              corrected ? chars.active_array_w : chars.pre_correction_array_w;
          const uint32_t ref_h =
              corrected ? chars.active_array_h : chars.pre_correction_array_h;
          const CoordinateDomain domain =
              corrected ? CoordinateDomain{CoordinateDomainAndroidSensorActiveArray{}}
                        : CoordinateDomain{
                              CoordinateDomainAndroidSensorPreCorrectionActiveArray{}};

          if (have_reference && captured.facts.has_intrinsics) {
            // [f_x, f_y, c_x, c_y, s] -- Camera2 reports skew, which the
            // WinRT backend cannot, so it is carried rather than dropped.
            if (const auto intrinsics_fact = Intrinsics::create(
                    static_cast<double>(captured.facts.intrinsics[0]),
                    static_cast<double>(captured.facts.intrinsics[1]),
                    static_cast<double>(captured.facts.intrinsics[2]),
                    static_cast<double>(captured.facts.intrinsics[3]),
                    static_cast<double>(captured.facts.intrinsics[4]),
                    ref_w, ref_h, domain)) {
              image_facts.intrinsics =
                  SourcedFact<Intrinsics>{*intrinsics_fact, FactOrigin::NATIVE_REPORTED};
            }
          }
          if (have_reference && captured.facts.has_distortion) {
            // [kappa_1..kappa_3] radial, [kappa_4, kappa_5] tangential.
            if (const auto distortion_fact = BrownConrady5Distortion::create(
                    static_cast<double>(captured.facts.distortion[0]),
                    static_cast<double>(captured.facts.distortion[1]),
                    static_cast<double>(captured.facts.distortion[2]),
                    static_cast<double>(captured.facts.distortion[3]),
                    static_cast<double>(captured.facts.distortion[4]),
                    ref_w, ref_h, domain,
                    corrected ? DistortionImageState::RECTIFIED
                              : DistortionImageState::DISTORTED)) {
              image_facts.distortion = SourcedFact<Distortion>{
                  Distortion{*distortion_fact}, FactOrigin::NATIVE_REPORTED};
            }
          }
        }

        // Reported after construction, not merely after the metadata read: the
        // create() validators reject non-finite or degenerately-referenced
        // values, so "device supplied it" and "we published it" are different
        // claims and only the second one is what Core saw.
        camera2_detail::log_line(
            "capture=%llu member=%u calibration reported=%s/%s published=%s/%s "
            "(intrinsics/distortion), correction_capable=%s mode_in_result=%s",
            static_cast<unsigned long long>(capture_id), member.image_member_index,
            captured.facts.has_intrinsics ? "yes" : "no",
            captured.facts.has_distortion ? "yes" : "no",
            image_facts.intrinsics ? "yes" : "no",
            image_facts.distortion ? "yes" : "no",
            chars.distortion_correction_supported ? "yes" : "no",
            captured.facts.has_distortion_correction_mode ? "yes" : "no");

        // Realized exposure per member, beside the compensation that was
        // requested for it. This is the measurement that decides whether
        // bracketing actually separates exposures on a device, rather than
        // reporting a control value the sensor never acted on (brief §8): if
        // these do not move across members, the bracket is nominal only.
        // control_milli_ev is logged beside realized_milli_ev purely so the
        // gap between what the AE control claims and what the image received
        // stays observable; only the latter is published.
        camera2_detail::log_line(
            "capture=%llu member=%u bracket: role=%s requested_milli_ev=%d "
            "realized_milli_ev=%s%d control_milli_ev=%s%d exposure_ns=%lld iso=%d",
            static_cast<unsigned long long>(capture_id), member.image_member_index,
            is_bracket ? "bracket" : "default",
            member.intended_exposure_compensation_milli_ev,
            has_realized_ev ? "" : "absent:", has_realized_ev ? realized_milli_ev : 0,
            (captured.facts.has_ae_comp_steps && chars.has_exposure_compensation) ? "" : "absent:",
            (captured.facts.has_ae_comp_steps && chars.has_exposure_compensation)
                ? static_cast<int32_t>(std::lround(
                      static_cast<double>(captured.facts.ae_comp_steps) *
                      chars.ae_comp_step_ev * 1000.0))
                : 0,
            static_cast<long long>(
                captured.facts.has_exposure_ns ? captured.facts.exposure_ns : -1),
            captured.facts.has_iso ? captured.facts.iso : -1);

        // Sensor-timestamp offset from the first member. This is the temporal
        // spread the burst exists to close: members far apart in time do not
        // describe the same scene, however exact their exposures.
        if (captured.has_timestamp && member_index == 0) {
          bundle_first_timestamp_ns = captured.timestamp_ns;
          have_bundle_first_timestamp = true;
        }
        if (captured.has_timestamp && have_bundle_first_timestamp) {
          camera2_detail::log_line(
              "capture=%llu member=%u sensor_offset_ms=%.1f path=%s",
              static_cast<unsigned long long>(capture_id), member.image_member_index,
              static_cast<double>(captured.timestamp_ns - bundle_first_timestamp_ns) / 1.0e6,
              use_manual_burst ? "manual_burst" : "ae_sequential");
        }

        if (image_facts.exposure_time || image_facts.sensor_sensitivity_iso ||
            image_facts.aperture_f_number || image_facts.focal_length_mm ||
            image_facts.focus_state || image_facts.intrinsics || image_facts.distortion) {
          strand_.post_capture_image_facts(capture_id, device_id,
                                           member.image_member_index, image_facts);
        }
      }
    }

    terminal_posted = true;
    strand_.post_capture_completed(capture_id, device_id);
  } catch (...) {
    fail(ProviderError::ERR_PROVIDER_FAILED);
  }
}

ProviderResult Camera2CameraProvider::validate_and_admit_submission_locked_(
    const CaptureSubmission& submission,
    std::vector<DeviceCaptureJob>& out_jobs) {
  // Caller holds capture_mutex_ then state_mutex_.
  if (capture_admission_closed_) {
    return ProviderResult::failure(ProviderError::ERR_SHUTTING_DOWN);
  }
  if (submission.capture_id == 0 || submission.device_requests.empty()) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (capture_queue_.size() + capture_active_jobs_ + submission.device_requests.size() >
      kCaptureQueueCapacity) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }

  out_jobs.clear();
  out_jobs.reserve(submission.device_requests.size());
  for (const CaptureRequest& req : submission.device_requests) {
    if (req.capture_id != submission.capture_id || req.device_instance_id == 0) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    if (req.width == 0 || req.height == 0) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    if (req.format_fourcc != FOURCC_RGBA && req.format_fourcc != FOURCC_BGRA) {
      return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
    }
    if (!is_valid_capture_still_image_bundle(req.still_image_bundle,
                                             supports_multi_image_still_sequence())) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    if (in_flight_captures_.count(
            InFlightKey{req.capture_id, req.device_instance_id}) != 0) {
      return ProviderResult::failure(ProviderError::ERR_BUSY);
    }
    auto dev_it = devices_.find(req.device_instance_id);
    if (dev_it == devices_.end() || !dev_it->second.open) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    if (!dev_it->second.backend) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }

    // Characteristics were cached at open, so every check below is a memory
    // read: admission stays prompt and performs no backend I/O (brief §2),
    // while still being authoritative rather than optimistic.
    {
      std::lock_guard<std::mutex> bl(dev_it->second.backend->m);
      const StaticCharacteristics& chars = dev_it->second.backend->chars;
      if (!chars.supports_size(req.width, req.height)) {
        return ProviderResult::failure(ProviderError::ERR_PLATFORM_CONSTRAINT);
      }
      // A started stream pins the session output set; a capture that needs a
      // different geometry cannot execute without rebuilding the session and
      // dropping the live stream.
      const auto& stream = dev_it->second.backend->stream;
      if (stream && stream->producing &&
          (stream->width != req.width || stream->height != req.height)) {
        return ProviderResult::failure(ProviderError::ERR_PLATFORM_CONSTRAINT);
      }
      if (req.still_image_bundle.members.size() > 1) {
        if (req.still_image_bundle.members.size() > kMaxBracketMembers) {
          return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
        }
        // Either bracket mechanism will do: manual exposure (exact, preferred)
        // or auto-exposure compensation (the weaker fallback). A device with
        // neither cannot separate the members at all and is refused rather
        // than silently returning a bundle of identical exposures.
        const bool manual_capable = chars.manual_sensor_supported &&
                                    chars.has_exposure_time_range &&
                                    chars.has_sensitivity_range;
        if (!manual_capable && !chars.has_exposure_compensation) {
          return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
        }
      }
    }

    DeviceCaptureJob job;
    job.request = req;
    job.generation = capture_generation_;
    out_jobs.push_back(std::move(job));
  }
  return ProviderResult::success();
}

ProviderResult Camera2CameraProvider::trigger_capture(const CaptureRequest& req) {
  CaptureSubmission submission;
  submission.capture_id = req.capture_id;
  submission.origin = req.rig_id != 0 ? CaptureSubmissionOrigin::RIG_CAPTURE
                                      : CaptureSubmissionOrigin::DEVICE_CAPTURE;
  submission.rig_id = req.rig_id;
  submission.device_requests.push_back(req);
  return trigger_capture_submission(submission);
}

ProviderResult Camera2CameraProvider::trigger_capture_submission(
    const CaptureSubmission& submission) {
  if (!initialized_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  std::vector<DeviceCaptureJob> jobs;
  {
    // Atomic grouped admission: every member device job or none, and a
    // rejected submission emits no facts (brief §5).
    std::lock_guard<std::mutex> capture_lock(capture_mutex_);
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    const ProviderResult pr = validate_and_admit_submission_locked_(submission, jobs);
    if (!pr.ok()) {
      return pr;
    }
    for (DeviceCaptureJob& job : jobs) {
      in_flight_captures_[InFlightKey{job.request.capture_id,
                                      job.request.device_instance_id}] = job.generation;
      capture_queue_.push_back(std::move(job));
    }
  }
  capture_cv_.notify_all();
  return ProviderResult::success();
}

ProviderResult Camera2CameraProvider::abort_capture(uint64_t /*capture_id*/) {
  if (!initialized_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  // Deterministic: ACameraCaptureSession_abortCaptures() cancels every
  // in-flight request on the session, including members of other captures and
  // the repeating stream, so it cannot implement a per-capture abort. Refuse
  // rather than cancel work the caller did not ask to cancel.
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult Camera2CameraProvider::apply_camera_spec_patch(
    const std::string& hardware_id,
    uint64_t /*new_camera_spec_version*/,
    SpecPatchView /*patch*/) {
  if (!initialized_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (hardware_id.empty()) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  return ProviderResult::success();
}

ProviderResult Camera2CameraProvider::apply_imaging_spec_patch(
    uint64_t /*new_imaging_spec_version*/,
    SpecPatchView /*patch*/) {
  if (!initialized_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return ProviderResult::success();
}

ProviderResult Camera2CameraProvider::shutdown() {
  if (!initialized_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (shutting_down_.exchange(true, std::memory_order_acq_rel)) {
    return ProviderResult::success();
  }

  // 1. Close capture admission and invalidate the generation so queued jobs
  //    terminalize as ERR_SHUTTING_DOWN at their next checkpoint.
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    capture_admission_closed_ = true;
    ++capture_generation_;
  }
  capture_cv_.notify_all();

  // 2. Join the workers (each admitted job terminalizes exactly once inside
  //    the worker; an in-flight member rides out its bounded sample wait).
  stop_and_join_capture_executor_();

  {
  std::lock_guard<std::mutex> state_lock(state_mutex_);

  // 3. Stop started streams.
  for (auto& [stream_id, st] : streams_) {
    if (!st.started) {
      continue;
    }
    auto dev_it = devices_.find(st.req.device_instance_id);
    if (dev_it != devices_.end() && dev_it->second.backend) {
      std::lock_guard<std::mutex> bl(dev_it->second.backend->m);
      if (dev_it->second.backend->stream) {
        dev_it->second.backend->stream->producing = false;
      }
    }
    st.started = false;
    strand_.post_stream_stopped(stream_id, ProviderError::OK);
  }

  // Quiescence barrier while stream records are still alive.
  strand_.flush();

  // 4. Destroy streams.
  for (auto it = streams_.begin(); it != streams_.end();) {
    const uint64_t stream_id = it->first;
    const uint64_t native_id = it->second.native_id;
    auto dev_it = devices_.find(it->second.req.device_instance_id);
    if (dev_it != devices_.end()) {
      if (dev_it->second.backend) {
        std::lock_guard<std::mutex> bl(dev_it->second.backend->m);
        dev_it->second.backend->stream.reset();
      }
      dev_it->second.stream_id = 0;
    }
    it = streams_.erase(it);
    strand_.post_stream_destroyed(stream_id);
    emit_native_destroyed_(native_id);
  }

  // 5. Close devices with real Camera2 release on the control thread.
  for (auto& [dev_id, dev] : devices_) {
    if (!dev.open) {
      continue;
    }
    std::shared_ptr<DeviceBackend> backend = dev.backend;
    if (backend) {
      {
        std::lock_guard<std::mutex> configure_lock(backend->configure_mutex);
        teardown_session_locked_(backend);
      }
      {
        std::lock_guard<std::mutex> bl(backend->m);
        backend->closed = true;
      }
      auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
      (void)control_.run_bounded(
          [backend](const BoundedControlExecutor::AbandonToken& /*t*/) {
            ACameraDevice* device = nullptr;
            {
              std::lock_guard<std::mutex> bl(backend->m);
              device = std::exchange(backend->device, nullptr);
            }
            if (device) {
              ACameraDevice_close(device);
            }
          },
          token, kControlJobTimeoutMs);
    }
    dev.open = false;
    dev.backend.reset();
    strand_.post_device_closed(dev_id);
    emit_native_destroyed_(dev.native_id);
    dev.native_id = 0;
  }

  emit_native_destroyed_(provider_native_id_);
  provider_native_id_ = 0;
  } // release state_mutex_ before draining the strand (brief §10)

  // 6. With provider state settled and no locks held: flush and stop the
  //    strand, then stop the control thread and release the manager.
  strand_.flush();
  strand_.stop();
  control_.stop();
  manager_.reset();

  callbacks_ = nullptr;
  initialized_.store(false, std::memory_order_release);
  return ProviderResult::success();
}

} // namespace cambang

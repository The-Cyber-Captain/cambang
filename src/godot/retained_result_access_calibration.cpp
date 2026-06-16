#include "godot/retained_result_access_calibration.h"

#include <mutex>
#include <set>
#include <string>

#include "godot/cambang_capture_result.h"
#include "godot/cambang_stream_result.h"

namespace cambang::retained_result_access_calibration {
namespace {

struct CalibrationIdentity final {
  std::string surface;
  uint64_t posture_id = 0;
  uint64_t stream_id = 0;
  uint64_t capture_id = 0;
  uint32_t image_member_index = 0;

  bool operator<(const CalibrationIdentity& other) const noexcept {
    if (surface != other.surface) return surface < other.surface;
    if (posture_id != other.posture_id) return posture_id < other.posture_id;
    if (stream_id != other.stream_id) return stream_id < other.stream_id;
    if (capture_id != other.capture_id) return capture_id < other.capture_id;
    return image_member_index < other.image_member_index;
  }
};

std::mutex g_mutex;
std::set<CalibrationIdentity> g_completed;

bool mark_needed(const CalibrationIdentity& identity) {
  if (identity.posture_id == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_completed.insert(identity).second;
}

} // namespace

void calibrate_stream_result(const SharedStreamResultData& data) {
  if (!data) {
    return;
  }
  const CalibrationIdentity display_identity{
      "stream_display_view", data->access_posture.posture_id, data->stream_id, 0, 0};
  if (data->retained_access_truth.display_view != ResultCapability::UNSUPPORTED &&
      mark_needed(display_identity)) {
    (void)CamBANGStreamResult::calibrate_display_view_for_retained_access(data);
  }

  const CalibrationIdentity image_identity{
      "stream_to_image", data->access_posture.posture_id, data->stream_id, 0, 0};
  if (data->retained_access_truth.to_image != ResultCapability::UNSUPPORTED &&
      mark_needed(image_identity)) {
    (void)CamBANGStreamResult::calibrate_to_image_for_retained_access(data);
  }
}

void calibrate_capture_result(const SharedCaptureResultData& data) {
  if (!data) {
    return;
  }
  for (uint32_t i = 0; i < data->image_member_count(); ++i) {
    const auto* member = data->image_member_at(i);
    if (!member || member->retained_access_truth.to_image == ResultCapability::UNSUPPORTED) {
      continue;
    }
    const CalibrationIdentity identity{
        "capture_to_image", member->access_posture.posture_id, member->access_posture.stream_id,
        data->capture_id, member->image_member_index};
    if (mark_needed(identity)) {
      (void)CamBANGCaptureResult::calibrate_to_image_member_for_retained_access(data, member->image_member_index);
    }
  }
}

void clear() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_completed.clear();
}

} // namespace cambang::retained_result_access_calibration

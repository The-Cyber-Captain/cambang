#include "godot/cambang_capture_result_set.h"

#include "godot/cambang_capture_result.h"

namespace cambang {

void CamBANGCaptureResultSet::set_results(std::vector<SharedCaptureResultData> results) {
  results_by_device_.clear();
  for (auto& r : results) {
    if (r) {
      results_by_device_[r->device_instance_id] = std::move(r);
    }
  }
}

int CamBANGCaptureResultSet::size() const {
  return static_cast<int>(results_by_device_.size());
}

bool CamBANGCaptureResultSet::is_empty() const {
  return results_by_device_.empty();
}

godot::Array CamBANGCaptureResultSet::get_results() const {
  godot::Array out;
  for (const auto& [_, data] : results_by_device_) {
    godot::Ref<CamBANGCaptureResult> r;
    r.instantiate();
    r->set_data(data);
    out.push_back(r);
  }
  return out;
}

godot::Ref<CamBANGCaptureResult> CamBANGCaptureResultSet::get_result_for_device(uint64_t device_instance_id) const {
  auto it = results_by_device_.find(device_instance_id);
  if (it == results_by_device_.end() || !it->second) {
    return godot::Ref<CamBANGCaptureResult>();
  }
  godot::Ref<CamBANGCaptureResult> r;
  r.instantiate();
  r->set_data(it->second);
  return r;
}

void CamBANGCaptureResultSet::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_id"), &CamBANGCaptureResultSet::get_capture_id);
  godot::ClassDB::bind_method(godot::D_METHOD("size"), &CamBANGCaptureResultSet::size);
  godot::ClassDB::bind_method(godot::D_METHOD("is_empty"), &CamBANGCaptureResultSet::is_empty);
  godot::ClassDB::bind_method(godot::D_METHOD("get_results"), &CamBANGCaptureResultSet::get_results);
  godot::ClassDB::bind_method(godot::D_METHOD("get_result_for_device", "device_instance_id"), &CamBANGCaptureResultSet::get_result_for_device);
}

} // namespace cambang

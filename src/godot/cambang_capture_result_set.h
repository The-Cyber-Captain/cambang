#pragma once

#include <map>

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>

#include "core/core_result_store.h"

namespace cambang {

class CamBANGCaptureResult;

class CamBANGCaptureResultSet final : public godot::Object {
  GDCLASS(CamBANGCaptureResultSet, godot::Object)

public:
  CamBANGCaptureResultSet() = default;

  void set_capture_id(uint64_t capture_id) { capture_id_ = capture_id; }
  void set_results(std::vector<SharedCaptureResultData> results);

  uint64_t get_capture_id() const { return capture_id_; }
  int size() const;
  bool is_empty() const;
  godot::Array get_results() const;
  CamBANGCaptureResult* get_result_for_device(uint64_t device_instance_id) const;

  static void _bind_methods();

private:
  uint64_t capture_id_ = 0;
  std::map<uint64_t, SharedCaptureResultData> results_by_device_;
};

} // namespace cambang

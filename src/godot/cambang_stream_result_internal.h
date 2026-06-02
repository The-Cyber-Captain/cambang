#pragma once

#include <cstdint>

#include <godot_cpp/classes/ref_counted.hpp>

namespace cambang {

class DisplayDemandToken : public godot::RefCounted {
  GDCLASS(DisplayDemandToken, godot::RefCounted);

public:
  DisplayDemandToken() = default;
  ~DisplayDemandToken() override;

  void init(uint64_t stream_id, bool gpu_display_view);

private:
  static void _bind_methods() {}
  uint64_t stream_id_ = 0;
  bool gpu_display_view_ = false;
  bool tracker_registered_ = false;
};

// Internal-only helper class registration required for Ref<DisplayDemandToken>::instantiate().
void register_stream_result_internal_classes();

// Diagnostic-only warning for live GPU StreamResult display-view metadata tokens.
void warn_if_outstanding_gpu_display_views_before_stop();

} // namespace cambang

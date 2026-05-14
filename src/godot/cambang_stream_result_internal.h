#pragma once

#include <cstdint>

#include <godot_cpp/classes/ref_counted.hpp>

namespace cambang {

class DisplayDemandToken : public godot::RefCounted {
  GDCLASS(DisplayDemandToken, godot::RefCounted);

public:
  DisplayDemandToken() = default;
  ~DisplayDemandToken() override;

  void init(uint64_t stream_id);

private:
  static void _bind_methods() {}
  uint64_t stream_id_ = 0;
};

// Internal-only helper class registration required for Ref<DisplayDemandToken>::instantiate().
void register_stream_result_internal_classes();

} // namespace cambang

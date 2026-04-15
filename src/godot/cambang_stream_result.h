#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "core/core_result_store.h"
#include "core/result_capability.h"

namespace cambang {
class CoreRuntime;

class CamBANGStreamResult final : public godot::RefCounted {
  GDCLASS(CamBANGStreamResult, godot::RefCounted)

public:
  static constexpr int CAPABILITY_READY = static_cast<int>(ResultCapability::READY);
  static constexpr int CAPABILITY_CHEAP = static_cast<int>(ResultCapability::CHEAP);
  static constexpr int CAPABILITY_EXPENSIVE = static_cast<int>(ResultCapability::EXPENSIVE);
  static constexpr int CAPABILITY_UNSUPPORTED = static_cast<int>(ResultCapability::UNSUPPORTED);

  CamBANGStreamResult() = default;

  void set_data(SharedStreamResultData data) { data_ = std::move(data); }

  uint32_t get_width() const;
  uint32_t get_height() const;
  uint32_t get_format() const;
  int get_payload_kind() const;
  uint64_t get_capture_timestamp() const;
  uint64_t get_stream_id() const;
  uint64_t get_device_instance_id() const;
  int get_intent() const;

  bool has_image_properties() const;
  bool has_capture_attributes() const;
  bool has_location_attributes() const;
  bool has_optical_calibration() const;

  godot::Dictionary get_image_properties() const;
  godot::Dictionary get_capture_attributes() const;
  godot::Dictionary get_location_attributes() const;
  godot::Dictionary get_optical_calibration() const;

  godot::Dictionary get_image_properties_provenance() const;
  godot::Dictionary get_capture_attributes_provenance() const;
  godot::Dictionary get_location_attributes_provenance() const;
  godot::Dictionary get_optical_calibration_provenance() const;

  int can_get_display_view() const;
  int can_to_image() const;

  godot::Variant get_display_view() const;
  godot::Ref<godot::Image> to_image() const;

  static void refresh_live_stream_cpu_display_views(const CoreRuntime& runtime);
  static void clear_live_stream_cpu_display_views();

  static void _bind_methods();

private:
  SharedStreamResultData data_;
};

} // namespace cambang

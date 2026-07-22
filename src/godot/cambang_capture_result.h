#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include "core/core_result_store.h"
#include "core/result_capability.h"

namespace cambang {

class CamBANGServer;

class CamBANGCaptureResult final : public godot::RefCounted {
  GDCLASS(CamBANGCaptureResult, godot::RefCounted)

public:
  static constexpr int CAPABILITY_READY = static_cast<int>(ResultCapability::READY);
  static constexpr int CAPABILITY_CHEAP = static_cast<int>(ResultCapability::CHEAP);
  static constexpr int CAPABILITY_EXPENSIVE = static_cast<int>(ResultCapability::EXPENSIVE);
  static constexpr int CAPABILITY_UNSUPPORTED = static_cast<int>(ResultCapability::UNSUPPORTED);
  static constexpr int IMAGE_ROLE_DEFAULT_METERED = 0;
  static constexpr int IMAGE_ROLE_ADDITIONAL_BRACKET = 1;

  CamBANGCaptureResult() = default;

  void set_data(SharedCaptureResultData data) { data_ = std::move(data); }
  void set_server(CamBANGServer* server) { server_ = server; }

  uint32_t get_width() const;
  uint32_t get_height() const;
  uint32_t get_format() const;
  int get_payload_kind() const;
  int64_t get_capture_datetime_unix_nanoseconds() const;
  uint64_t get_device_instance_id() const;
  uint64_t get_capture_id() const;
  bool has_geolocation() const;
  godot::Dictionary get_geolocation() const;

  bool has_image_properties() const;

  godot::Dictionary get_image_properties() const;

  godot::Dictionary get_image_properties_provenance() const;

  int can_get_display_view() const;
  int can_to_image() const;
  int get_image_count() const;
  bool has_additional_images() const;
  godot::Dictionary get_image_member(int image_member_index) const;
  int can_to_image_member(int image_member_index) const;
  godot::Ref<godot::Image> to_image_member(int image_member_index) const;
  int can_get_encoded_bytes() const;

  godot::Variant get_display_view() const;
  godot::Ref<godot::Image> to_image() const;
  godot::PackedByteArray get_encoded_bytes() const;

  static godot::Ref<godot::Image> calibrate_to_image_member_for_retained_access(
      const SharedCaptureResultData& data,
      uint32_t image_member_index);
  static godot::Ref<godot::Image> calibrate_to_image_member_cpu_payload_for_retained_access(
      const SharedCaptureResultData& data,
      uint32_t image_member_index);
  static godot::Ref<godot::Image> calibrate_to_image_member_gpu_materializer_for_retained_access(
      const SharedCaptureResultData& data,
      uint32_t image_member_index);

  static void _bind_methods();

private:
  SharedCaptureResultData data_;
  CamBANGServer* server_ = nullptr;
};

} // namespace cambang

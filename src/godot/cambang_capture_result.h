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
  // Where this result came from (capture_identity_and_lifecycle.md 2.3).
  // BRANCH ON THIS, never on whether a rig field looks empty: a rig capture
  // whose member fields happen to be defaulted is not a device capture.
  static constexpr int CAPTURE_ORIGIN_DEVICE = 0;
  static constexpr int CAPTURE_ORIGIN_RIG = 1;
  static constexpr int IMAGE_ROLE_DEFAULT_METERED = 0;
  static constexpr int IMAGE_ROLE_ADDITIONAL_BRACKET = 1;

  CamBANGCaptureResult() = default;

  void set_data(SharedCaptureResultData data) { data_ = std::move(data); }
  void set_server(CamBANGServer* server) { server_ = server; }

  // Identity of this result, with every key always present so a caller reads
  // one shape (capture_identity_and_lifecycle.md 2.3):
  //
  //   { capture_origin: int, device_capture_id: int, rig_capture_id: int,
  //     rig_member_hardware_id: String, rig_member_index: int,
  //     device_instance_id: int }
  //
  // Origin DEVICE leaves rig_capture_id 0, rig_member_hardware_id empty and
  // rig_member_index -1. Those are not "missing" values to test for; they are
  // what the fields mean when there is no rig, which is why capture_origin
  // exists to be branched on instead.
  //
  // Participation identity is the hardware id. rig_member_index is a
  // convenience for this session only -- membership order is not guaranteed
  // stable across runs, so an index is meaningless against a reloaded result.
  // device_instance_id is session-scoped and must never be used as durable
  // identity.
  //
  // Ids are session-scoped integers. 2.2's durable dc_/rc_ string form is not
  // implemented, so none of these survive the session.
  godot::Dictionary get_capture_identity() const;

  uint32_t get_width() const;
  uint32_t get_height() const;
  uint32_t get_format() const;
  int get_payload_kind() const;
  int64_t get_capture_datetime_unix_nanoseconds() const;
  uint64_t get_device_instance_id() const;
  // get_capture_id() was retired with 2.2. Section 1 says the unqualified term
  // should not appear in code, and the value it returned -- the internal
  // uint64 -- was never the caller's identity. Use get_capture_identity()'s
  // device_capture_id, which carries the durable dc_ form.
  //
  // Its dominant use was a staleness guard (`id > previous`), which section 8
  // says harnesses should delete in favour of the capture_finished signal
  // rather than reimplement against strings.
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

// Diagnostic: capture member materialisation counts, split by whether the
// conversion was performed for the application or by the access-cost
// calibration probe. Both are recorded under one evidence route.
godot::Dictionary capture_materialization_stats();

} // namespace cambang

#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <vector>
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

  static constexpr int DISPLAY_PATH_NONE = 0;
  static constexpr int DISPLAY_PATH_RETAINED_GPU_BACKING = 1;
  static constexpr int DISPLAY_PATH_STREAM_LIVE_CPU_DISPLAY_VIEW = 2;

  CamBANGStreamResult() = default;

  void set_data(SharedStreamResultData data) { data_ = std::move(data); }

  uint32_t get_width() const;
  uint32_t get_height() const;
  uint32_t get_format() const;
  int get_payload_kind() const;
  uint64_t get_stream_id() const;
  uint64_t get_device_instance_id() const;
  int get_intent() const;
  godot::Dictionary get_camera_facts() const;

  bool has_image_properties() const;

  godot::Dictionary get_image_properties() const;

  godot::Dictionary get_image_properties_provenance() const;

  // Stream Compute Texture -- an additional, deliberately rawer surface than
  // get_display_view(), not a replacement for it. Frame-frozen and plane-wise;
  // see stream_compute_texture.h.
  int can_get_compute_texture() const;
  int get_compute_texture_plane_count() const;
  godot::Ref<godot::Texture2D> get_compute_texture_plane(int plane_index) const;

  // Colour interpretation of the planes above, as the provider declared it.
  // A caller doing its own Y'CbCr maths needs this, and "declared" tells it
  // whether CamBANG was told or is simply unaware.
  godot::Dictionary get_colorimetry() const;

  int can_get_display_view() const;
  int can_to_image() const;

  int get_display_view_path_kind() const;
  godot::Variant get_display_view() const;
  godot::Ref<godot::Image> to_image() const;

  static void refresh_live_stream_cpu_display_views(const CoreRuntime& runtime);
  static void remove_live_stream_cpu_display_view(uint64_t stream_id);
  static void clear_live_stream_cpu_display_views();
  static godot::Dictionary get_live_stream_cpu_display_metrics_snapshot();
  static godot::Variant calibrate_display_view_for_retained_access(const SharedStreamResultData& data);
  static godot::Ref<godot::Image> calibrate_to_image_for_retained_access(const SharedStreamResultData& data);
  static godot::Ref<godot::Image> calibrate_to_image_cpu_payload_for_retained_access(const SharedStreamResultData& data);
  static godot::Ref<godot::Image> calibrate_to_image_gpu_materializer_for_retained_access(const SharedStreamResultData& data);

  static void _bind_methods();

private:
  SharedStreamResultData data_;
  // Plane textures for THIS result, produced on first request and kept for as
  // long as the caller keeps the result.
  //
  // Deliberately per-result rather than a module-global cache. The cache key
  // a global map would need -- stream, device, retained frame -- IS this
  // result's identity, so holding the textures here stores the same thing
  // without an eviction policy, a fixed entry cap, or contention between
  // streams. A previous global FIFO of 12 entries produced no hits at all in
  // the ordinary access pattern (each frame's planes are requested once, and
  // the frame id is part of the key), while holding up to 12 textures of
  // mixed vintage alive -- ~75 MB at 4K -- and halving each stream's
  // residency as soon as a second stream ran, which is the baseline case.
  //
  // Mutable because production is lazy behind const accessors. Not thread
  // safe: like the rest of this surface it is main-thread only.
  mutable std::vector<godot::Ref<godot::Texture2D>> compute_texture_planes_;
};

} // namespace cambang

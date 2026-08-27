#pragma once

#include <cstdint>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/string.hpp>

namespace cambang {

class CamBANGServer;
class CamBANGStreamResult;

class CamBANGStream final : public godot::RefCounted {
  GDCLASS(CamBANGStream, godot::RefCounted)

public:
  static constexpr int INTENT_PREVIEW = 0;
  static constexpr int INTENT_VIEWFINDER = 1;

  CamBANGStream() = default;

  void set_identity(CamBANGServer* server,
                    const godot::String& hardware_id,
                    uint64_t device_instance_id,
                    uint64_t stream_id) {
    server_ = server;
    hardware_id_ = hardware_id;
    device_instance_id_ = device_instance_id;
    stream_id_ = stream_id;
    result_live_ = false;
    _register_with_server_();
  }

  // The format_fourcc this stream was CREATED with, or 0 when the caller
  // pinned none and left the choice to Core.
  //
  // Exists to be compared against CamBANGStreamResult.get_format(). A pin is a
  // PREFERENCE on platform providers, not a guarantee: Camera2 can only open a
  // reader as AIMAGE_FORMAT_YUV_420_888, whose 4:2:0 memory layout the device
  // chooses, so a stream pinned to NV12 is delivered as NV21 on every handset
  // measured. The delivered format is authoritative; this is what was asked
  // for, so a caller can assert the two agree instead of discovering a swapped
  // chroma order in its own shader.
  void set_requested_format(uint32_t fourcc) { requested_format_fourcc_ = fourcc; }
  uint32_t get_requested_format() const { return requested_format_fourcc_; }

  uint64_t get_stream_id() const { return stream_id_; }
  uint64_t get_device_instance_id() const { return device_instance_id_; }
  godot::String get_hardware_id() const { return hardware_id_; }
  bool is_result_live() const { return result_live_; }
  bool is_valid_stream_handle() const {
    return !destroy_requested_ && server_ != nullptr && stream_id_ != 0 && device_instance_id_ != 0;
  }
  godot::Error start();
  godot::Error stop();
  godot::Error destroy();
  godot::Ref<CamBANGStreamResult> get_result() const;

protected:
  static void _bind_methods();

private:
  friend class CamBANGServer;
  void _register_with_server_();
  void _set_result_live_from_server_(bool live);

  CamBANGServer* server_ = nullptr;
  godot::String hardware_id_;
  uint64_t device_instance_id_ = 0;
  uint64_t stream_id_ = 0;
  uint32_t requested_format_fourcc_ = 0;
  bool destroy_requested_ = false;
  bool result_live_ = false;
};

} // namespace cambang

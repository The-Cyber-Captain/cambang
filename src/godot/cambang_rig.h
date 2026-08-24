#pragma once

#include <cstdint>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace cambang {

class CamBANGServer;
class CamBANGCaptureResult;
class CamBANGDevice;

class CamBANGRig final : public godot::RefCounted {
  GDCLASS(CamBANGRig, godot::RefCounted)

public:
  CamBANGRig() = default;

  void set_server_and_id(CamBANGServer* server, uint64_t rig_id) {
    server_ = server;
    rig_id_ = rig_id;
    current_rig_capture_id_ = 0;
    _register_with_server_();
  }

  uint64_t get_id() const { return rig_id_; }

  // Membership mutation (capture_identity_and_lifecycle.md 5.1). Takes a
  // device handle rather than a hardware-id string: the caller already holds
  // CamBANGDevice objects, and making them round-trip through strings invites
  // typos the type system would otherwise catch. The hardware id remains the
  // participation identity internally (2.3).
  //
  // Declarative, not imperative. Both are accepted while the rig is live and
  // apply FROM THE NEXT TRIGGER; neither disturbs a capture already in flight,
  // and neither returns ERR_BUSY. Both are idempotent -- adding an existing
  // member or removing a non-member succeeds without bumping the version.
  //
  // ERR_INVALID_PARAMETER when the handle names no device, the rig is unknown,
  // or a removal would empty the rig.
  godot::Error add_member(const godot::Ref<CamBANGDevice>& device);
  godot::Error remove_member(const godot::Ref<CamBANGDevice>& device);

  // Returns { id: int, members: Dictionary, error: int }
  // (capture_identity_and_lifecycle.md 4.1). All three keys always present.
  //
  // `id` is the Rig Capture Id; `members` maps each member's hardware id to
  // that member's own Device Capture Id. A map rather than a list because a
  // rig trigger is conceptually every member being pressed at once, and the
  // caller needs something to correlate each arriving result against
  // immediately -- an index into a list is meaningless once the rig object is
  // gone.
  //
  // On refusal `id` is 0 and `members` is empty: a refused trigger mints
  // nothing.
  godot::Dictionary trigger_capture();
  godot::TypedArray<CamBANGCaptureResult> get_result() const;

  // What became of each member of this rig's most recent capture
  // (capture_identity_and_lifecycle.md 4.3). One entry per member whether or
  // not it produced an image, so a short result set explains itself:
  //
  //   { hardware_id, device_instance_id, device_capture_id,
  //     disposition, error_code }
  //
  // Pair it with capture_finished -- outcomes are decided at closure, and
  // that signal is the announcement of closure.
  godot::Array get_member_outcomes() const;

protected:
  static void _bind_methods();

private:
  // Out-of-line: CamBANGServer is only forward-declared here.
  void _register_with_server_();

private:
  CamBANGServer* server_ = nullptr;
  uint64_t rig_id_ = 0;
  // Rig Capture Id of this rig's most recent accepted trigger. Never a Device
  // Capture Id -- the two spaces are disjoint and interchanging them silently
  // resolves the wrong record.
  uint64_t current_rig_capture_id_ = 0;
};

} // namespace cambang

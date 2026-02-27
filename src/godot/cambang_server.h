#pragma once

#include <cstdint>
#include <memory>

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "core/core_runtime.h"
#include "core/state_snapshot_buffer.h"
#include "core/snapshot/state_snapshot.h"
#include "godot/cambang_state_snapshot.h"

namespace cambang {

class CamBANGServerTickNode;

// CamBANGServer is the release-facing lifecycle owner.
//
// Engine singleton lifetime:
// - Created at GDExtension initialization (scene init level)
// - Destroyed at GDExtension termination
//
// Work lifetime:
// - User calls start()/stop() to enable/disable core processing.
//
// Threading:
// - CoreRuntime publishes snapshots on the core thread.
// - Godot signals are emitted on the Godot main thread via an internal tick node.
class CamBANGServer final : public godot::Object {
  GDCLASS(CamBANGServer, godot::Object)

public:
  CamBANGServer();
  ~CamBANGServer() override;

  // User-facing control of core processing.
  void start();
  void stop();

  static CamBANGServer* get_singleton() noexcept { return singleton_; }

  // Return a Godot wrapper snapshot object. Safe to call on the Godot main thread.
  godot::Ref<cambang::CamBANGStateSnapshotGD> get_state_snapshot() const {
    godot::Ref<cambang::CamBANGStateSnapshotGD> out;
    out.instantiate();
    out->_init_from_core(latest_);
    return out;
  }

#if defined(CAMBANG_ENABLE_DEV_NODES)
  // Dev-only escape hatch: allow dev scaffolding nodes to drive provider bring-up.
  CoreRuntime* runtime_for_dev() noexcept { return &runtime_; }
#endif

protected:
  static void _bind_methods();

private:
  friend class CamBANGServerTickNode;

  // Called on the Godot main thread by the internal tick node.
  void _on_godot_tick();

  static CamBANGServer* singleton_;

  CoreRuntime runtime_;
  StateSnapshotBuffer snapshot_buffer_;

  // Godot-thread cached snapshot.
  std::shared_ptr<const CamBANGStateSnapshot> latest_;
  uint64_t last_emitted_gen_ = 0;
};

} // namespace cambang
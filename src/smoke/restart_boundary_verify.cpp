#include "dev/cli_log.h"

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "restart_boundary_verify: build with -DCAMBANG_INTERNAL_SMOKE=1 (via SCons: smoke=1)."
#endif

#include "smoke/verify_case/verify_case_harness.h"

namespace cambang {
namespace {

bool fail(const char* msg) {
  cli::error("FAIL: ", msg);
  return false;
}

bool check(bool cond, const char* msg) {
  if (!cond) {
    return fail(msg);
  }
  return true;
}


const NativeObjectRecord* find_native_of_type(const CamBANGStateSnapshot& s, NativeObjectType type) {
  const uint32_t want = static_cast<uint32_t>(type);
  for (const auto& n : s.native_objects) {
    if (n.type == want) {
      return &n;
    }
  }
  return nullptr;
}

} // namespace
} // namespace cambang

int main() {
  using namespace cambang;

  VerifyCaseHarness h(VerifyCaseProviderKind::Synthetic);
  std::string error;

  if (!h.start_runtime(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!h.wait_for_core_snapshot([](const CamBANGStateSnapshot&) { return true; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  // Before first Godot-boundary-equivalent tick, public snapshot must be NIL.
  if (!check(h.observed().is_nil, "expected NIL snapshot before first publish")) {
    return 1;
  }
  cli::line("step 0 OK");

  // First publish for generation 0.
  h.tick();
  if (!check(!h.observed().is_nil && h.observed().gen == 0 && h.observed().version == 0,
             "expected first baseline publish for generation 0")) {
    return 1;
  }
  cli::line("step 1 OK");

  // Introduce additional observable state so shutdown teardown publication has
  // something meaningful to surface at the boundary.
  if (!h.open_device(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!h.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
        const NativeObjectRecord* provider_native = find_native_of_type(s, NativeObjectType::Provider);
        const NativeObjectRecord* device_native = find_native_of_type(s, NativeObjectType::Device);
        return VerifyCaseHarness::has_device(s, VerifyCaseHarness::kDeviceId) &&
               provider_native != nullptr &&
               device_native != nullptr &&
               device_native->owner_provider_native_id == provider_native->native_id &&
               device_native->owner_rig_id == 0;
      }, error, 500, 5, "timed out waiting for device + native ownership visibility")) {
    cli::error("FAIL: ", error);
    return 1;
  }
  h.tick();
  if (!check(!h.observed().is_nil && h.observed().gen == 0 && h.observed().device_count > 0,
             "expected non-empty observable state before stop")) {
    return 1;
  }
  cli::line("step 1b OK");

  {
    const auto& obs = h.observed();
    if (!check(!obs.is_nil && obs.raw != nullptr, "expected raw snapshot after device open tick")) {
      return 1;
    }
    const NativeObjectRecord* provider_native = find_native_of_type(*obs.raw, NativeObjectType::Provider);
    const NativeObjectRecord* device_native = find_native_of_type(*obs.raw, NativeObjectType::Device);
    if (!check(provider_native != nullptr, "expected provider native object")) {
      return 1;
    }
    if (!check(device_native != nullptr, "expected device native object")) {
      return 1;
    }
    if (!check(device_native->owner_provider_native_id == provider_native->native_id,
               "expected device native owner_provider_native_id to reference provider native")) {
      return 1;
    }
    if (!check(device_native->owner_rig_id == 0,
               "expected owner_rig_id to remain unknown (0) in this slice")) {
      return 1;
    }
  }
  cli::line("step 1b-ownership OK");

  // Callback-context-equivalent restart flow: stop/start immediately while
  // handling the first publish in the same control flow.
  h.stop_runtime();

  // Stop boundary must surface one final prior-generation observation (if changed)
  // before public snapshot state is cleared to NIL.
  if (!check(!h.last_snapshot_before_stop_clear().is_nil,
             "expected final observable snapshot before stop clear")) {
    return 1;
  }
  if (!check(h.last_snapshot_before_stop_clear().gen == 0,
             "expected final pre-clear snapshot to belong to generation 0")) {
    return 1;
  }
  if (!check(h.last_snapshot_before_stop_clear().device_count == 0,
             "expected final pre-clear snapshot to include shutdown-teardown changes")) {
    return 1;
  }
  cli::line("step 1c OK");

  // After completed stop, public snapshot must be NIL.
  if (!check(h.observed().is_nil, "expected NIL snapshot after completed stop")) {
    return 1;
  }
  cli::line("step 2 OK");

  if (!h.start_runtime(error)) {
    cli::error("FAIL: ", error);
    return 1;
  }

  // Even if core already has a new-run snapshot buffered, the boundary-visible
  // snapshot must remain NIL until the next boundary tick.
  if (!h.wait_for_core_snapshot([](const CamBANGStateSnapshot& s) { return s.gen == 1; }, error)) {
    cli::error("FAIL: ", error);
    return 1;
  }
  if (!check(h.observed().is_nil, "expected NIL snapshot before first post-restart publish")) {
    return 1;
  }
  cli::line("step 3 OK");

  // First publish after restart must produce non-NIL generation 1 baseline.
  h.tick();
  if (!check(!h.observed().is_nil && h.observed().gen == 1 && h.observed().version == 0,
             "expected non-NIL generation 1 baseline on first post-restart publish")) {
    return 1;
  }
  cli::line("step 4 OK");

  cli::line("OK: restart_boundary_verify passed");
  return 0;
}

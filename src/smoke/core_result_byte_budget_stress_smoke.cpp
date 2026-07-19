// Ledger #53 follow-up: the byte-budget eviction path
// (CoreResultStore::evict_over_byte_budget()) had zero test coverage --
// no existing scenario produces anywhere near a real budget's worth of
// retained bytes, so the eviction logic itself (as opposed to its plumbing)
// had never executed under test. This smoke drives it directly, at volume,
// synthetic-only, mirroring the exact snapshot-then-evict pattern
// CoreRuntime::on_core_timer_tick() uses (see core_runtime.cpp and
// CoreCaptureAssemblyRegistry::terminal_capture_device_pairs()'s doc
// comment): take a terminal-pairs snapshot from CoreCaptureAssemblyRegistry
// BEFORE touching CoreResultStore's lock, then evict by that snapshot.
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "core/core_capture_assembly_registry.h"
#include "core/core_result_store.h"

using namespace cambang;

namespace {

constexpr uint64_t kDeviceInstanceId = 1;
constexpr uint32_t kCapWidth = 64;
constexpr uint32_t kCapHeight = 64;
constexpr uint64_t kCpuCaptureBytes = uint64_t(kCapWidth) * kCapHeight * 4; // RGBA, 16384 bytes.
constexpr uint32_t kGpuStrideBytes = 256;
constexpr uint32_t kGpuHeight = 64;
constexpr uint64_t kGpuCaptureBytes = uint64_t(kGpuStrideBytes) * kGpuHeight; // 16384 bytes, deliberately equal.

FrameView make_cpu_capture_frame(uint64_t capture_id, std::vector<uint8_t>& bytes) {
  bytes.assign(static_cast<size_t>(kCpuCaptureBytes), 0x5A);
  FrameView frame{};
  frame.device_instance_id = kDeviceInstanceId;
  frame.capture_id = capture_id;
  frame.width = kCapWidth;
  frame.height = kCapHeight;
  frame.format_fourcc = FOURCC_RGBA;
  frame.data = bytes.data();
  frame.size_bytes = bytes.size();
  frame.capture_image.routing = CaptureImageRouting::DEFAULT_METERED;
  frame.capture_image.image_member_index = 0;
  return frame;
}

FrameView make_gpu_only_capture_frame(uint64_t capture_id) {
  FrameView frame{};
  frame.device_instance_id = kDeviceInstanceId;
  frame.capture_id = capture_id;
  frame.width = kGpuStrideBytes / 4;
  frame.height = kGpuHeight;
  frame.format_fourcc = FOURCC_RGBA;
  // build_retained_gpu_backing_descriptor() (core_result_store.cpp) normalizes
  // the retained descriptor's width/height/stride_bytes/format_fourcc from
  // these top-level FrameView fields, not from the nested descriptor below --
  // stride_bytes MUST be set here for the descriptor's estimated-byte
  // contribution (stride_bytes * height) to be non-zero.
  frame.stride_bytes = kGpuStrideBytes;
  frame.primary_backing_kind = ProducerBackingKind::GPU;
  frame.primary_backing_artifact = std::make_shared<int>(static_cast<int>(capture_id));
  frame.retain_cpu_sidecar = false;
  frame.retained_gpu_backing_descriptor.valid = true;
  frame.retained_gpu_backing_descriptor.materialization_available = true;
  frame.retained_gpu_backing_descriptor.stride_bytes = kGpuStrideBytes;
  frame.retained_gpu_backing_descriptor.height = kGpuHeight;
  frame.capture_image.routing = CaptureImageRouting::DEFAULT_METERED;
  frame.capture_image.image_member_index = 0;
  return frame;
}

// Mirrors CoreRuntime::on_core_timer_tick()'s production wiring exactly:
// snapshot terminal pairs from the assembly registry BEFORE ever touching
// CoreResultStore's lock, then hand evict_over_byte_budget an in-memory
// predicate that does no locking of its own.
std::vector<CoreResultStore::EvictedCaptureResult> run_eviction_sweep(
    CoreResultStore& store, CoreCaptureAssemblyRegistry& assembly, uint64_t byte_budget) {
  if (store.total_estimated_capture_bytes() <= byte_budget) {
    return {};
  }
  const auto terminal_pairs = assembly.terminal_capture_device_pairs();
  const std::set<std::pair<uint64_t, uint64_t>> terminal_set(terminal_pairs.begin(), terminal_pairs.end());
  auto evicted = store.evict_over_byte_budget(
      byte_budget,
      [&terminal_set](uint64_t capture_id, uint64_t device_instance_id) {
        return terminal_set.count(std::make_pair(capture_id, device_instance_id)) != 0;
      });
  for (const auto& e : evicted) {
    assembly.remove_assembly(e.capture_id, e.device_instance_id);
  }
  return evicted;
}

CoreRetainedProductionPlan cpu_plan() {
  CoreRetainedProductionPlan plan{};
  plan.valid = true;
  plan.posture = CoreProductionPostureShape::CpuPrimary;
  return plan;
}

CoreRetainedProductionPlan gpu_no_sidecar_plan() {
  CoreRetainedProductionPlan plan{};
  plan.valid = true;
  plan.posture = CoreProductionPostureShape::GpuPrimaryNoCpuSidecar;
  return plan;
}

} // namespace

int main() {
  const CoreRetainedProductionPlan requested_cpu = cpu_plan();
  const CoreRetainedProductionPlan requested_gpu = gpu_no_sidecar_plan();

  // ---- Deterministic, hand-traceable scenario -----------------------------
  // 12 captures, ascending capture_id 1..12. Every 4th is left non-terminal
  // (simulating still in-flight assembly); the rest alternate CPU/GPU-backed
  // terminal captures. Every entry is deliberately the same size (16384
  // bytes) so eviction counts translate to exact byte arithmetic.
  {
    CoreResultStore store;
    CoreCaptureAssemblyRegistry assembly;
    std::vector<uint64_t> in_flight_ids;
    std::vector<uint64_t> terminal_ids;

    for (uint64_t capture_id = 1; capture_id <= 12; ++capture_id) {
      if (capture_id % 4 == 0) {
        std::vector<uint8_t> bytes;
        FrameView frame = make_cpu_capture_frame(capture_id, bytes);
        assert(store.retain_frame(frame, std::nullopt, 0, 1, {}, requested_cpu));
        assembly.mark_default_image_retained(capture_id, kDeviceInstanceId);
        in_flight_ids.push_back(capture_id);
      } else if (capture_id % 2 == 0) {
        FrameView frame = make_gpu_only_capture_frame(capture_id);
        assert(store.retain_frame(frame, std::nullopt, 0, 1, {}, requested_gpu));
        assembly.mark_default_image_retained(capture_id, kDeviceInstanceId);
        assembly.mark_capture_completed(capture_id, kDeviceInstanceId);
        terminal_ids.push_back(capture_id);
      } else {
        std::vector<uint8_t> bytes;
        FrameView frame = make_cpu_capture_frame(capture_id, bytes);
        assert(store.retain_frame(frame, std::nullopt, 0, 1, {}, requested_cpu));
        assembly.mark_default_image_retained(capture_id, kDeviceInstanceId);
        assembly.mark_capture_completed(capture_id, kDeviceInstanceId);
        terminal_ids.push_back(capture_id);
      }
    }
    assert(in_flight_ids.size() == 3);  // 4, 8, 12
    assert(terminal_ids.size() == 9);   // 1,2,3,5,6,7,9,10,11

    const uint64_t expected_total = 12ull * kCpuCaptureBytes;
    assert(kCpuCaptureBytes == kGpuCaptureBytes);
    assert(store.total_estimated_capture_bytes() == expected_total);
    for (uint64_t id : in_flight_ids) assert(store.get_capture_result(id, kDeviceInstanceId));
    for (uint64_t id : terminal_ids) assert(store.get_capture_result(id, kDeviceInstanceId));

    // Budget chosen so eviction must stop EXACTLY after captures 1,2,3,5,6
    // are evicted (5 terminal entries; skipping in-flight capture 4 along
    // the way), leaving 7 entries (4,7,8,9,10,11,12) = 7 * 16384 bytes.
    const uint64_t test_byte_budget = 7ull * kCpuCaptureBytes;
    const auto evicted = run_eviction_sweep(store, assembly, test_byte_budget);

    assert(evicted.size() == 5);
    const std::vector<uint64_t> expected_evicted = {1, 2, 3, 5, 6};
    for (size_t i = 0; i < expected_evicted.size(); ++i) {
      assert(evicted[i].capture_id == expected_evicted[i]);
      assert(evicted[i].device_instance_id == kDeviceInstanceId);
    }
    assert(store.total_estimated_capture_bytes() == test_byte_budget);

    // Evicted terminal entries are gone from both registries.
    for (uint64_t id : {uint64_t(1), uint64_t(2), uint64_t(3), uint64_t(5), uint64_t(6)}) {
      assert(!store.get_capture_result(id, kDeviceInstanceId));
      assert(!assembly.is_assembly_successful(id, kDeviceInstanceId));
    }
    // Surviving terminal entries (budget satisfied before reaching them) are untouched.
    for (uint64_t id : {uint64_t(7), uint64_t(9), uint64_t(10), uint64_t(11)}) {
      assert(store.get_capture_result(id, kDeviceInstanceId));
      assert(assembly.is_assembly_successful(id, kDeviceInstanceId));
    }
    // In-flight entries are NEVER evicted, regardless of age or budget pressure.
    for (uint64_t id : in_flight_ids) {
      assert(store.get_capture_result(id, kDeviceInstanceId));
      assert(!assembly.is_assembly_successful(id, kDeviceInstanceId)); // never terminal.
    }

    // Re-running the same sweep at the same budget is a no-op: already at
    // budget, so no further (and definitely no incorrect) eviction occurs.
    const auto second_pass = run_eviction_sweep(store, assembly, test_byte_budget);
    assert(second_pass.empty());
    assert(store.total_estimated_capture_bytes() == test_byte_budget);

    // A budget already satisfied triggers no eviction at all (cheap no-walk path).
    const auto satisfied_pass = run_eviction_sweep(store, assembly, test_byte_budget + 1);
    assert(satisfied_pass.empty());

    // Driving the budget to zero evicts every remaining EVICTABLE (terminal)
    // entry, but the in-flight floor is never touched even though the store
    // remains over budget afterward -- this is the central safety property
    // established across #52/#53: a persistently-over-budget store with
    // only non-terminal entries left does not corrupt or crash, it simply
    // cannot evict further.
    const auto zero_budget_pass = run_eviction_sweep(store, assembly, 0);
    assert(zero_budget_pass.size() == 4); // 7, 9, 10, 11
    const uint64_t in_flight_floor_bytes = 3ull * kCpuCaptureBytes;
    assert(store.total_estimated_capture_bytes() == in_flight_floor_bytes);
    for (uint64_t id : in_flight_ids) {
      assert(store.get_capture_result(id, kDeviceInstanceId));
    }
    // Calling again at budget 0 is safe and idempotent: nothing left is
    // evictable, so the walk finds nothing to do despite still being over
    // budget -- no crash, no incorrect eviction of in-flight data, no drift.
    const auto stuck_over_budget_pass = run_eviction_sweep(store, assembly, 0);
    assert(stuck_over_budget_pass.empty());
    assert(store.total_estimated_capture_bytes() == in_flight_floor_bytes);
  }

  // ---- Volume stress ------------------------------------------------------
  // A much larger synthetic multi-camera repeated-capture session: thousands
  // of captures across several devices, swept in batches (as repeated timer
  // ticks would), composing the byte-budget sweep with the capture-admission
  // watchdog (#48) exactly as CoreRuntime::on_core_timer_tick() does. A
  // fraction of every batch is admitted but never marked terminal (still
  // in-flight); left alone these would accumulate without bound and make the
  // budget permanently unenforceable (byte-budget eviction only ever touches
  // terminal entries -- see CoreCaptureAssemblyRegistry::
  // terminal_capture_device_pairs()'s doc comment). The watchdog is what
  // closes that loop in production: any admission left non-terminal past its
  // timeout is forced to FAILED (terminal), which is exactly the composition
  // this stress verifies -- that the two mechanisms together, not byte-budget
  // eviction alone, are what keep memory bounded indefinitely.
  {
    CoreResultStore store;
    CoreCaptureAssemblyRegistry assembly;
    constexpr uint64_t kDevices = 4;
    constexpr uint64_t kCapturesPerBatch = 50;
    constexpr uint64_t kBatches = 40; // 2000 captures total across 4 devices.
    constexpr uint64_t kStressByteBudget = 32ull * kCpuCaptureBytes; // room for 32 retained captures.
    constexpr uint64_t kBatchPeriodNs = 1'000'000'000ull; // 1 synthetic second per batch.
    constexpr uint64_t kWatchdogTimeoutNs = 3 * kBatchPeriodNs; // stuck > 3 batches => forced FAILED.

    uint64_t next_capture_id = 1;
    uint64_t total_admitted = 0;
    uint64_t now_ns = 0;
    for (uint64_t batch = 0; batch < kBatches; ++batch) {
      now_ns += kBatchPeriodNs;
      for (uint64_t i = 0; i < kCapturesPerBatch; ++i) {
        const uint64_t capture_id = next_capture_id++;
        const uint64_t device_instance_id = 100 + (capture_id % kDevices);
        std::vector<uint8_t> bytes;
        bytes.assign(static_cast<size_t>(kCpuCaptureBytes), 0x11);
        FrameView frame{};
        frame.device_instance_id = device_instance_id;
        frame.capture_id = capture_id;
        frame.width = kCapWidth;
        frame.height = kCapHeight;
        frame.format_fourcc = FOURCC_RGBA;
        frame.data = bytes.data();
        frame.size_bytes = bytes.size();
        frame.capture_image.routing = CaptureImageRouting::DEFAULT_METERED;
        frame.capture_image.image_member_index = 0;
        assert(store.retain_frame(frame, std::nullopt, 0, 1, {}, requested_cpu));
        assembly.mark_default_image_retained(capture_id, device_instance_id);
        // Every admission gets an admission context/timestamp so the
        // watchdog can act on it, mirroring CoreRuntime's real admission
        // call sites (record_admission_context(..., ns_since_epoch_())).
        assembly.record_admission_context(
            capture_id, device_instance_id, CaptureAdmissionContext{},
            make_default_metered_still_image_bundle(), now_ns);
        // A small fraction stay in-flight (never marked terminal by a real
        // completion/failure fact), mirroring a session that always has a
        // handful of still-assembling captures -- these rely entirely on
        // the watchdog below to eventually resolve.
        if (i % 10 != 0) {
          assembly.mark_capture_completed(capture_id, device_instance_id);
        }
        ++total_admitted;
      }
      // Same order as CoreRuntime::on_core_timer_tick(): admission watchdog
      // first (forces long-stuck in-flight admissions to terminal FAILED),
      // then byte-budget eviction (which can now also reclaim those).
      (void)assembly.sweep_admission_timeouts(now_ns, kWatchdogTimeoutNs);
      run_eviction_sweep(store, assembly, kStressByteBudget);
      // Every remaining entry's byte accounting must stay an exact whole
      // number of capture-sized units -- no partial/corrupted accounting
      // from repeated retain/evict cycles at volume.
      assert(store.total_estimated_capture_bytes() % kCpuCaptureBytes == 0);
    }
    (void)total_admitted;

    // After the full stress run -- once the watchdog has had several
    // timeout windows to resolve every stuck admission -- the store must be
    // back at or under budget: nothing can remain non-terminal forever.
    assert(store.total_estimated_capture_bytes() <= kStressByteBudget);
    assert(store.total_estimated_capture_bytes() % kCpuCaptureBytes == 0);
  }

  std::cout << "PASS core_result_byte_budget_stress_smoke\n";
  return 0;
}

/*
CamBANG Maintainer Utility

Tool: phase3_snapshot_verify

Purpose
-------
Phase 3 verifier expansion for snapshot/native-object/publication semantics:
- detached-root visibility
- retirement sweep observability
- topology_version structural transitions
- timestamp preservation/fallback semantics
- no-sink delivered vs dropped accounting

This tool intentionally verifies core-facing truth; Godot-facing NIL-before-baseline
is covered by dedicated Godot scene checks.
*/

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "phase3_snapshot_verify: build with -DCAMBANG_INTERNAL_SMOKE=1 (via SCons: smoke=1)."
#endif

#include "core/core_dispatcher.h"
#include "core/core_device_registry.h"
#include "core/latest_frame_mailbox.h"
#include "core/core_mailbox.h"
#include "core/core_native_object_registry.h"
#include "core/core_runtime.h"
#include "core/core_stream_registry.h"
#include "core/snapshot/snapshot_builder.h"
#include "core/snapshot/state_snapshot.h"
#include "core/state_snapshot_buffer.h"

using namespace cambang;

namespace {

constexpr uint64_t kDeviceId = 100;
constexpr uint64_t kStreamId = 200;
constexpr uint64_t kRootId = 900;

static bool wait_until(const std::function<bool()>& pred, int max_iters = 500, int sleep_ms = 5) {
  for (int i = 0; i < max_iters; ++i) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  return false;
}

static std::shared_ptr<const CamBANGStateSnapshot> snapshot_copy(StateSnapshotBuffer& buf) {
  return buf.snapshot_copy();
}

static bool contains_u64(const std::vector<uint64_t>& v, uint64_t x) {
  for (uint64_t a : v) {
    if (a == x) return true;
  }
  return false;
}

static const NativeObjectRecord* find_native(const CamBANGStateSnapshot& s, uint64_t native_id) {
  for (const auto& no : s.native_objects) {
    if (no.native_id == native_id) return &no;
  }
  return nullptr;
}

static bool has_stream(const CamBANGStateSnapshot& s, uint64_t stream_id) {
  for (const auto& st : s.streams) {
    if (st.stream_id == stream_id) return true;
  }
  return false;
}

static bool has_device(const CamBANGStateSnapshot& s, uint64_t device_id) {
  for (const auto& d : s.devices) {
    if (d.instance_id == device_id) return true;
  }
  return false;
}

static const CamBANGStreamState* find_stream(const CamBANGStateSnapshot& s, uint64_t stream_id) {
  for (const auto& st : s.streams) {
    if (st.stream_id == stream_id) {
      return &st;
    }
  }
  return nullptr;
}

class MailboxSink final : public ICoreFrameSink {
public:
  explicit MailboxSink(LatestFrameMailbox* mailbox) : mailbox_(mailbox) {}

  CoreVisibilityPath on_frame(FrameView frame) override {
    if (!mailbox_) {
      frame.release_now();
      return CoreVisibilityPath::NONE;
    }
    return mailbox_->write_from_core(std::move(frame));
  }

private:
  LatestFrameMailbox* mailbox_ = nullptr;
};

struct OwnedFrame {
  std::vector<uint8_t> bytes;
  int releases = 0;

  static void release_fn(void* user, const FrameView*) {
    int* release_count = static_cast<int*>(user);
    (*release_count)++;
  }

  FrameView view(uint64_t stream_id,
                 uint64_t device_id,
                 uint32_t width,
                 uint32_t height,
                 uint32_t format,
                 size_t size_bytes,
                 uint32_t stride_bytes = 0) {
    FrameView frame{};
    frame.stream_id = stream_id;
    frame.device_instance_id = device_id;
    frame.width = width;
    frame.height = height;
    frame.format_fourcc = format;
    frame.capture_timestamp.domain = CaptureTimestampDomain::CORE_MONOTONIC;
    frame.capture_timestamp.value = 123;
    frame.capture_timestamp.tick_ns = 1;
    frame.data = bytes.data();
    frame.size_bytes = size_bytes;
    frame.stride_bytes = stride_bytes;
    frame.release = &OwnedFrame::release_fn;
    frame.release_user = &releases;
    return frame;
  }
};

static int test_visibility_diagnostics_snapshot_truth() {
  CoreDeviceRegistry devices;
  CoreStreamRegistry streams;
  CoreNativeObjectRegistry native_objects;
  LatestFrameMailbox mailbox;
  MailboxSink sink(&mailbox);
  uint64_t gen = 0;
  uint64_t now_ns = 1000;
  CoreDispatcher dispatcher(&streams, &devices, &native_objects, &gen, [&now_ns]() {
    return now_ns++;
  });
  dispatcher.set_frame_sink(&sink);

  StreamRequest req{};
  req.stream_id = 77;
  req.device_instance_id = 88;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 2;
  req.profile.height = 1;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;
  req.profile_version = 1;
  if (!streams.declare_stream_effective(req) ||
      !streams.on_stream_created(req.stream_id) ||
      !streams.on_stream_started(req.stream_id)) {
    std::cerr << "FAIL: visibility diagnostics setup failed\n";
    return 1;
  }

  auto dispatch_frame = [&](FrameView frame) {
    CoreCommand cmd{};
    cmd.type = CoreCommandType::PROVIDER_FRAME;
    cmd.payload = CmdProviderFrame{frame};
    dispatcher.dispatch(std::move(cmd));
  };

  SnapshotBuilder builder;
  SnapshotBuilder::Inputs in;
  in.devices = &devices;
  in.streams = &streams;
  in.native_objects = &native_objects;

  OwnedFrame rgba;
  rgba.bytes = {
      10, 20, 30, 40,
      50, 60, 70, 80,
  };
  dispatch_frame(rgba.view(req.stream_id, req.device_instance_id, 2, 1, FOURCC_RGBA, rgba.bytes.size()));
  if (rgba.releases != 1) {
    std::cerr << "FAIL: RGBA frame not released exactly once\n";
    return 1;
  }

  auto rgba_snapshot = builder.build(in, 0, 0, 0, 10);
  const auto* rgba_stream = find_stream(rgba_snapshot, req.stream_id);
  if (!rgba_stream) {
    std::cerr << "FAIL: RGBA snapshot missing stream\n";
    return 1;
  }
  if (rgba_stream->visibility_frames_presented != 1 ||
      rgba_stream->visibility_last_path != CBVisibilityLastPath::RGBA_DIRECT) {
    std::cerr << "FAIL: RGBA visibility counters incorrect presented="
              << rgba_stream->visibility_frames_presented
              << " path=" << static_cast<int>(rgba_stream->visibility_last_path) << "\n";
    return 1;
  }
  if (rgba_stream->frames_received != 1 || rgba_stream->frames_delivered != 1 ||
      rgba_stream->frames_dropped != 0 || rgba_stream->last_frame_ts_ns != 123) {
    std::cerr << "FAIL: RGBA existing counters changed semantics received="
              << rgba_stream->frames_received
              << " delivered=" << rgba_stream->frames_delivered
              << " dropped=" << rgba_stream->frames_dropped
              << " ts=" << rgba_stream->last_frame_ts_ns << "\n";
    return 1;
  }

  OwnedFrame bgra;
  bgra.bytes = {
      1, 2, 3, 4,
      5, 6, 7, 8,
  };
  dispatch_frame(bgra.view(req.stream_id, req.device_instance_id, 2, 1, FOURCC_BGRA, bgra.bytes.size()));
  auto bgra_snapshot = builder.build(in, 0, 1, 0, 11);
  const auto* bgra_stream = find_stream(bgra_snapshot, req.stream_id);
  if (!bgra_stream ||
      bgra_stream->visibility_frames_presented != 2 ||
      bgra_stream->visibility_last_path != CBVisibilityLastPath::BGRA_SWIZZLED) {
    std::cerr << "FAIL: BGRA visibility counters incorrect\n";
    return 1;
  }

  OwnedFrame unsupported;
  unsupported.bytes = {0, 1, 2, 3};
  dispatch_frame(unsupported.view(req.stream_id,
                                  req.device_instance_id,
                                  1,
                                  1,
                                  make_fourcc('N', 'V', '1', '2'),
                                  unsupported.bytes.size()));
  auto unsupported_snapshot = builder.build(in, 0, 2, 0, 12);
  const auto* unsupported_stream = find_stream(unsupported_snapshot, req.stream_id);
  if (!unsupported_stream ||
      unsupported_stream->visibility_frames_rejected_unsupported != 1 ||
      unsupported_stream->visibility_last_path != CBVisibilityLastPath::REJECTED_UNSUPPORTED) {
    std::cerr << "FAIL: unsupported visibility counters incorrect\n";
    return 1;
  }

  OwnedFrame invalid;
  invalid.bytes = {1, 2, 3};
  dispatch_frame(invalid.view(req.stream_id, req.device_instance_id, 2, 1, FOURCC_RGBA, invalid.bytes.size()));
  auto invalid_snapshot = builder.build(in, 0, 3, 0, 13);
  const auto* invalid_stream = find_stream(invalid_snapshot, req.stream_id);
  if (!invalid_stream ||
      invalid_stream->visibility_frames_rejected_invalid != 1 ||
      invalid_stream->visibility_last_path != CBVisibilityLastPath::REJECTED_INVALID) {
    std::cerr << "FAIL: invalid visibility counters incorrect\n";
    return 1;
  }

  const uint64_t topo_before = builder.compute_topology_signature(in);
  OwnedFrame extra_rgba;
  extra_rgba.bytes = {
      9, 9, 9, 9,
      9, 9, 9, 9,
  };
  dispatch_frame(extra_rgba.view(req.stream_id, req.device_instance_id, 2, 1, FOURCC_RGBA, extra_rgba.bytes.size()));
  const uint64_t topo_after = builder.compute_topology_signature(in);
  if (topo_before != topo_after) {
    std::cerr << "FAIL: visibility-only updates changed topology signature\n";
    return 1;
  }
  if (!dispatcher.consume_relevant_state_changed()) {
    std::cerr << "FAIL: visibility path did not mark dispatcher state dirty\n";
    return 1;
  }

  return 0;
}

static int test_destroyed_retention_does_not_cross_generation_baseline() {
  CoreRuntime rt;
  StateSnapshotBuffer buf;
  rt.set_snapshot_publisher(&buf);

  if (!rt.start()) {
    std::cerr << "FAIL: runtime start (gen0)\n";
    return 1;
  }
  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        return s && s->gen == 0 && s->version == 0;
      })) {
    std::cerr << "FAIL: missing baseline snapshot for gen0\n";
    rt.stop();
    return 1;
  }

  auto* cb = rt.provider_callbacks();
  NativeObjectCreateInfo c{};
  c.native_id = 91001;
  c.type = static_cast<uint32_t>(NativeObjectType::Stream);
  c.root_id = 91002;
  c.owner_device_instance_id = 0;
  c.owner_stream_id = 0;
  c.has_created_ns = true;
  c.created_ns = 111;
  cb->on_native_object_created(c);

  NativeObjectDestroyInfo d{};
  d.native_id = c.native_id;
  d.has_destroyed_ns = true;
  d.destroyed_ns = 222;
  cb->on_native_object_destroyed(d);

  rt.request_publish();
  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        auto* n = s ? find_native(*s, c.native_id) : nullptr;
        return s && s->gen == 0 && n && n->phase == CBLifecyclePhase::DESTROYED;
      })) {
    std::cerr << "FAIL: destroyed native not retained in live gen0\n";
    rt.stop();
    return 1;
  }

  rt.stop();

  if (!rt.start()) {
    std::cerr << "FAIL: runtime restart (gen1)\n";
    return 1;
  }
  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        return s && s->gen == 1 && s->version == 0;
      })) {
    std::cerr << "FAIL: missing baseline snapshot for gen1\n";
    rt.stop();
    return 1;
  }

  {
    auto s1 = snapshot_copy(buf);
    if (!s1) {
      std::cerr << "FAIL: missing gen1 baseline snapshot copy\n";
      rt.stop();
      return 1;
    }
    if (find_native(*s1, c.native_id) != nullptr) {
      std::cerr << "FAIL: gen1 baseline leaked retained destroyed native from gen0\n";
      rt.stop();
      return 1;
    }
  }

  rt.stop();
  return 0;
}

static int test_live_session_retirement_expiry_publication() {
  CoreRuntime rt;
  StateSnapshotBuffer buf;
  rt.set_snapshot_publisher(&buf);

  if (!rt.start()) {
    std::cerr << "FAIL: runtime start (live retention expiry)\n";
    return 1;
  }

  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        return s && s->version == 0;
      })) {
    std::cerr << "FAIL: missing baseline snapshot\n";
    rt.stop();
    return 1;
  }

  auto baseline = snapshot_copy(buf);
  if (!baseline) {
    std::cerr << "FAIL: missing baseline snapshot copy\n";
    rt.stop();
    return 1;
  }

  const uint64_t gen0 = baseline->gen;
  const uint64_t version_before_destroy = baseline->version;

  NativeObjectCreateInfo c{};
  c.native_id = 93001;
  c.type = static_cast<uint32_t>(NativeObjectType::Stream);
  c.root_id = 93002;
  c.has_created_ns = true;
  c.created_ns = 100;
  rt.provider_callbacks()->on_native_object_created(c);
  rt.request_publish();

  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        return s && s->gen == gen0 && find_native(*s, c.native_id) != nullptr;
      })) {
    std::cerr << "FAIL: native object not visible after create\n";
    rt.stop();
    return 1;
  }

  NativeObjectDestroyInfo d{};
  d.native_id = c.native_id;
  d.has_destroyed_ns = true;
  d.destroyed_ns = 200;
  rt.provider_callbacks()->on_native_object_destroyed(d);
  rt.request_publish();

  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        const NativeObjectRecord* n = s ? find_native(*s, c.native_id) : nullptr;
        return s && s->gen == gen0 && n && n->phase == CBLifecyclePhase::DESTROYED;
      })) {
    std::cerr << "FAIL: destroyed native record not retained during retention window\n";
    rt.stop();
    return 1;
  }

  auto retained = snapshot_copy(buf);
  if (!retained) {
    std::cerr << "FAIL: missing retained-destroyed snapshot copy\n";
    rt.stop();
    return 1;
  }
  const uint64_t version_with_retained_destroyed = retained->version;
  const size_t retained_native_count = retained->native_objects.size();

  // Retention window in CoreRuntime is 5 seconds. During live operation, timer-driven
  // retirement sweep should remove the expired destroyed record and trigger a publish.
  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        if (!s) return false;
        if (s->gen != gen0) return false;
        if (find_native(*s, c.native_id) != nullptr) return false;
        return s->version > version_with_retained_destroyed;
      }, /*max_iters=*/1700, /*sleep_ms=*/5)) {
    std::cerr << "FAIL: destroyed native record did not retire during live session\n";
    rt.stop();
    return 1;
  }

  auto retired = snapshot_copy(buf);
  if (!retired) {
    std::cerr << "FAIL: missing post-retirement snapshot copy\n";
    rt.stop();
    return 1;
  }

  const uint64_t version_after_retirement = retired->version;
  if (retired->gen != gen0) {
    std::cerr << "FAIL: generation changed during live-session retirement expiry\n";
    rt.stop();
    return 1;
  }
  if (!(version_with_retained_destroyed > version_before_destroy)) {
    std::cerr << "FAIL: retained-destroyed snapshot version did not advance after destroy\n";
    rt.stop();
    return 1;
  }
  if (!(version_after_retirement > version_with_retained_destroyed)) {
    std::cerr << "FAIL: version did not advance on retirement removal publish\n";
    rt.stop();
    return 1;
  }
  if (find_native(*retired, c.native_id) != nullptr) {
    std::cerr << "FAIL: stale retained destroyed record remains after retirement\n";
    rt.stop();
    return 1;
  }
  if (!(retired->native_objects.size() < retained_native_count)) {
    std::cerr << "FAIL: native object count did not reflect retirement removal\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_topology_detached_and_retirement() {
  CoreRuntime rt;
  StateSnapshotBuffer buf;
  rt.set_snapshot_publisher(&buf);

  if (!rt.start()) {
    std::cerr << "FAIL: runtime start\n";
    return 1;
  }

  if (!wait_until([&]() { auto s = snapshot_copy(buf); return s && s->version == 0; })) {
    std::cerr << "FAIL: missing baseline snapshot\n";
    rt.stop();
    return 1;
  }

  auto* cb = rt.provider_callbacks();
  const uint64_t topo0 = snapshot_copy(buf)->topology_version;

  cb->on_device_opened(kDeviceId);
  rt.request_publish();
  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        return s && has_device(*s, kDeviceId) && s->topology_version > topo0;
      })) {
    std::cerr << "FAIL: device appearance topology transition missing\n";
    rt.stop();
    return 1;
  }

  const uint64_t topo1 = snapshot_copy(buf)->topology_version;
  cb->on_stream_created(kStreamId);
  rt.request_publish();
  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        return s && has_stream(*s, kStreamId) && s->topology_version > topo1;
      })) {
    std::cerr << "FAIL: stream appearance topology transition missing\n";
    rt.stop();
    return 1;
  }

  NativeObjectCreateInfo c{};
  c.native_id = 5001;
  c.type = static_cast<uint32_t>(NativeObjectType::Stream);
  c.root_id = kRootId;
  c.owner_device_instance_id = kDeviceId;
  c.owner_stream_id = kStreamId;
  c.has_created_ns = true;
  c.created_ns = 101;
  cb->on_native_object_created(c);

  NativeObjectDestroyInfo d{};
  d.native_id = c.native_id;
  d.has_destroyed_ns = true;
  d.destroyed_ns = 202;
  cb->on_native_object_destroyed(d);

  rt.request_publish();
  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        return s && find_native(*s, c.native_id) != nullptr;
      })) {
    std::cerr << "FAIL: native record not visible\n";
    rt.stop();
    return 1;
  }

  const uint64_t topo2 = snapshot_copy(buf)->topology_version;
  cb->on_stream_destroyed(kStreamId);
  rt.request_publish();
  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        return s && !has_stream(*s, kStreamId) && s->topology_version > topo2;
      })) {
    std::cerr << "FAIL: stream disappearance topology transition missing\n";
    rt.stop();
    return 1;
  }

  {
    auto s = snapshot_copy(buf);
    if (!s) {
      std::cerr << "FAIL: missing snapshot after stream destroy\n";
      rt.stop();
      return 1;
    }
    if (contains_u64(s->detached_root_ids, kRootId)) {
      std::cerr << "FAIL: root falsely marked detached while device lineage is attached\n";
      rt.stop();
      return 1;
    }
  }

  const uint64_t topo3 = snapshot_copy(buf)->topology_version;
  cb->on_device_closed(kDeviceId);
  rt.request_publish();
  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        return s && !has_device(*s, kDeviceId) && contains_u64(s->detached_root_ids, kRootId) && s->topology_version > topo3;
      })) {
    std::cerr << "FAIL: detached-root appearance or device disappearance transition missing\n";
    rt.stop();
    return 1;
  }

  const uint64_t topo_detached = snapshot_copy(buf)->topology_version;

  // Retention window is 5s in runtime. Wait for retirement sweep-driven disappearance.
  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        if (!s) return false;
        if (find_native(*s, c.native_id) != nullptr) return false;
        if (contains_u64(s->detached_root_ids, kRootId)) return false;
        return s->topology_version > topo_detached;
      }, /*max_iters=*/1700, /*sleep_ms=*/5)) {
    std::cerr << "FAIL: retirement-driven native/detached removal or topology transition missing\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_timestamp_preservation_and_fallback() {
  CoreRuntime rt;
  StateSnapshotBuffer buf;
  rt.set_snapshot_publisher(&buf);

  if (!rt.start()) {
    std::cerr << "FAIL: runtime start\n";
    return 1;
  }
  if (!wait_until([&]() { auto s = snapshot_copy(buf); return s && s->version == 0; })) {
    std::cerr << "FAIL: missing baseline snapshot\n";
    rt.stop();
    return 1;
  }

  auto* cb = rt.provider_callbacks();

  NativeObjectCreateInfo c0{};
  c0.native_id = 6001;
  c0.type = static_cast<uint32_t>(NativeObjectType::Device);
  c0.root_id = 7001;
  c0.owner_device_instance_id = 0;
  c0.owner_stream_id = 0;
  c0.has_created_ns = true;
  c0.created_ns = 0; // truthful provider zero
  cb->on_native_object_created(c0);

  NativeObjectDestroyInfo d0{};
  d0.native_id = c0.native_id;
  d0.has_destroyed_ns = true;
  d0.destroyed_ns = 0; // truthful provider zero
  cb->on_native_object_destroyed(d0);

  NativeObjectCreateInfo c1{};
  c1.native_id = 6002;
  c1.type = static_cast<uint32_t>(NativeObjectType::Stream);
  c1.root_id = 7002;
  c1.owner_device_instance_id = 1;
  c1.owner_stream_id = 2;
  c1.has_created_ns = false; // absent -> fallback
  c1.created_ns = 123;
  cb->on_native_object_created(c1);

  NativeObjectDestroyInfo d1{};
  d1.native_id = c1.native_id;
  d1.has_destroyed_ns = false; // absent -> fallback
  d1.destroyed_ns = 456;
  cb->on_native_object_destroyed(d1);

  rt.request_publish();

  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        return s && find_native(*s, 6001) && find_native(*s, 6002);
      })) {
    std::cerr << "FAIL: timestamp test native records missing\n";
    rt.stop();
    return 1;
  }

  auto s = snapshot_copy(buf);
  const NativeObjectRecord* n0 = find_native(*s, 6001);
  const NativeObjectRecord* n1 = find_native(*s, 6002);

  if (!n0 || !n1) {
    std::cerr << "FAIL: native lookup failed\n";
    rt.stop();
    return 1;
  }

  if (n0->created_ns != 0 || n0->destroyed_ns != 0 || n0->phase != CBLifecyclePhase::DESTROYED) {
    std::cerr << "FAIL: provider-supplied zero timestamps were not preserved\n";
    rt.stop();
    return 1;
  }

  if (n1->created_ns == 123 || n1->destroyed_ns == 456) {
    std::cerr << "FAIL: absent provider timestamps did not fallback to integration time\n";
    rt.stop();
    return 1;
  }

  if (n1->created_ns == 0 || n1->destroyed_ns == 0) {
    std::cerr << "FAIL: fallback timestamps unexpectedly zero\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_no_sink_delivered_vs_dropped_accounting() {
  CoreStreamRegistry streams;
  CoreDeviceRegistry devices;
  CoreNativeObjectRegistry native_objects;
  uint64_t gen = 0;
  uint64_t now_ns = 1000;

  CoreDispatcher dispatcher(&streams, &devices, &native_objects, &gen, [&now_ns]() {
    return now_ns++;
  });

  StreamRequest req{};
  req.stream_id = 77;
  req.device_instance_id = 88;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 320;
  req.profile.height = 180;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;
  req.profile_version = 1;
  if (!streams.declare_stream_effective(req) || !streams.on_stream_created(req.stream_id)) {
    std::cerr << "FAIL: dispatcher no-sink setup failed\n";
    return 1;
  }

  int releases = 0;
  FrameView frame{};
  frame.stream_id = req.stream_id;
  frame.release = [](void* user, const FrameView*) {
    int* p = static_cast<int*>(user);
    (*p)++;
  };
  frame.release_user = &releases;

  CoreCommand cmd{};
  cmd.type = CoreCommandType::PROVIDER_FRAME;
  cmd.payload = CmdProviderFrame{frame};
  dispatcher.dispatch(std::move(cmd));

  if (releases != 1) {
    std::cerr << "FAIL: frame was not released exactly once in no-sink path\n";
    return 1;
  }

  const auto* rec = streams.find(req.stream_id);
  if (!rec) {
    std::cerr << "FAIL: stream missing after no-sink frame dispatch\n";
    return 1;
  }
  if (rec->frames_received != 1 || rec->frames_released != 0 || rec->frames_dropped != 1) {
    std::cerr << "FAIL: no-sink accounting mismatch received=" << rec->frames_received
              << " released=" << rec->frames_released
              << " dropped=" << rec->frames_dropped << "\n";
    return 1;
  }

  return 0;
}

} // namespace

int main() {
  if (int r = test_topology_detached_and_retirement()) return r;
  if (int r = test_destroyed_retention_does_not_cross_generation_baseline()) return r;
  if (int r = test_live_session_retirement_expiry_publication()) return r;
  if (int r = test_timestamp_preservation_and_fallback()) return r;
  if (int r = test_no_sink_delivered_vs_dropped_accounting()) return r;
  if (int r = test_visibility_diagnostics_snapshot_truth()) return r;

  std::cout << "OK: phase3_snapshot_verify passed\n";
  return 0;
}

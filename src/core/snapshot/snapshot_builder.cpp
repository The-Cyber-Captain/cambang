#include "core/snapshot/snapshot_builder.h"

#include <cstring>

#include "core/core_device_registry.h"
#include "core/core_stream_registry.h"
#include "core/core_native_object_registry.h"

namespace cambang {

namespace {
// Simple 64-bit FNV-1a for deterministic signature.
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

inline void fnv1a_u64(uint64_t& h, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        uint8_t b = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
        h ^= b;
        h *= kFnvPrime;
    }
}

} // namespace

CamBANGStateSnapshot SnapshotBuilder::build(const Inputs& in,
                                            uint64_t gen,
                                            uint64_t version,
                                            uint64_t topology_version,
                                            uint64_t timestamp_ns) const {
    CamBANGStateSnapshot snap;
    snap.schema_version = CamBANGStateSnapshot::kSchemaVersion;
    snap.gen = gen;
    snap.version = version;
    snap.topology_version = topology_version;
    snap.timestamp_ns = timestamp_ns;

    // imaging_spec_version is not implemented in this scaffolding slice.
    snap.imaging_spec_version = 0;

    // Devices
    if (in.devices) {
        snap.devices.reserve(in.devices->all().size());
        for (const auto& [id, rec] : in.devices->all()) {
            (void)rec;
            CamBANGDeviceState d;
            d.instance_id = id;
            d.hardware_id = std::string(); // unknown in current scaffolding

            // Map minimal state.
            d.phase = CBLifecyclePhase::LIVE;
            d.mode = CBDeviceMode::IDLE;
            d.engaged = rec.open;
            d.last_error_code = static_cast<int32_t>(rec.last_error_code);
            d.errors_count = (rec.last_error_code != 0) ? 1u : 0u;

            snap.devices.push_back(std::move(d));
        }
    }

    // Streams
    if (in.streams) {
        snap.streams.reserve(in.streams->all().size());
        for (const auto& [sid, rec] : in.streams->all()) {
            CamBANGStreamState s;
            s.stream_id = sid;
            s.device_instance_id = 0; // not tracked in current scaffolding
            s.phase = rec.created ? CBLifecyclePhase::LIVE : CBLifecyclePhase::CREATED;

            s.intent = cambang::StreamIntent::PREVIEW;

            if (!rec.created) {
                s.mode = CBStreamMode::STOPPED;
            } else {
                s.mode = rec.started ? CBStreamMode::FLOWING : CBStreamMode::STOPPED;
            }

            s.stop_reason = CBStreamStopReason::NONE;
            s.profile_version = 0;

            s.width = 0;
            s.height = 0;
            s.format = 0;
            s.target_fps_min = 0;
            s.target_fps_max = 0;

            s.frames_received = rec.frames_received;
            s.frames_delivered = rec.frames_released; // in this slice, release == delivered to sink
            s.frames_dropped = rec.frames_dropped;
            s.queue_depth = 0;

            // last_frame_ts_ns requires mapping capture timestamps into core timebase.
            // Not implemented in this slice.
            s.last_frame_ts_ns = 0;

            snap.streams.push_back(std::move(s));
        }
    }


// Native objects (provider-reported lifecycle truth).
if (in.native_objects) {
    snap.native_objects.reserve(in.native_objects->all().size());
    for (const auto& [nid, rec] : in.native_objects->all()) {
        (void)nid;
        NativeObjectRecord n;
        n.native_id = rec.native_id;
        n.type = rec.type;
        n.root_id = rec.root_id;

        if (!rec.created) {
            n.phase = CBLifecyclePhase::CREATED;
        } else if (rec.destroyed) {
            n.phase = CBLifecyclePhase::DESTROYED;
        } else {
            n.phase = CBLifecyclePhase::LIVE;
        }

        n.owner_device_instance_id = 0;
        n.owner_stream_id = 0;

        n.creation_gen = rec.creation_gen;

        n.created_ns = rec.created_ns;
        n.destroyed_ns = rec.destroyed_ns;

        n.bytes_allocated = 0;
        n.buffers_in_use = 0;

        snap.native_objects.push_back(std::move(n));
    }
}

// Rigs and detached_root_ids are not implemented in this scaffolding slice.
return snap;
}

uint64_t SnapshotBuilder::compute_topology_signature(const Inputs& in) const {
    uint64_t h = kFnvOffset;
    if (in.devices) {
        fnv1a_u64(h, static_cast<uint64_t>(in.devices->all().size()));
        for (const auto& [id, rec] : in.devices->all()) {
            (void)rec;
            fnv1a_u64(h, id);
        }
    }
    if (in.streams) {
        fnv1a_u64(h, static_cast<uint64_t>(in.streams->all().size()));
        for (const auto& [sid, rec] : in.streams->all()) {
            (void)rec;
            fnv1a_u64(h, sid);
        }
    }
    return h;
}

} // namespace cambang

#include "core/snapshot/snapshot_builder.h"

#include <cstring>
#include <set>

#include "core/core_device_registry.h"
#include "core/core_stream_registry.h"
#include "core/core_native_object_registry.h"
#include "core/provider_callback_ingress.h"

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

std::set<uint64_t> compute_detached_roots(const SnapshotBuilder::Inputs& in) {
    std::set<uint64_t> detached_roots;
    if (!in.native_objects) {
        return detached_roots;
    }

    std::set<uint64_t> active_device_ids;
    if (in.devices) {
        for (const auto& [device_id, rec] : in.devices->all()) {
            (void)rec;
            active_device_ids.insert(device_id);
        }
    }

    std::set<uint64_t> active_stream_ids;
    if (in.streams) {
        for (const auto& [stream_id, rec] : in.streams->all()) {
            (void)rec;
            active_stream_ids.insert(stream_id);
        }
    }

    struct RootStatus {
        bool has_records = false;
        bool has_controlling_owner = false;
        bool has_attached_controlling_owner = false;
    };
    std::map<uint64_t, RootStatus> status_by_root;

    for (const auto& [native_id, rec] : in.native_objects->all()) {
        (void)native_id;
        if (rec.root_id == 0) {
            continue;
        }

        RootStatus& root = status_by_root[rec.root_id];
        root.has_records = true;

        if (rec.owner_device_instance_id != 0) {
            root.has_controlling_owner = true;
            if (active_device_ids.find(rec.owner_device_instance_id) != active_device_ids.end()) {
                root.has_attached_controlling_owner = true;
            }
        }

        if (rec.owner_stream_id != 0) {
            root.has_controlling_owner = true;
            if (active_stream_ids.find(rec.owner_stream_id) != active_stream_ids.end()) {
                root.has_attached_controlling_owner = true;
            }
        }
    }

    for (const auto& [root_id, status] : status_by_root) {
        if (status.has_records && status.has_controlling_owner && !status.has_attached_controlling_owner) {
            detached_roots.insert(root_id);
        }
    }

    return detached_roots;
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
            d.phase = rec.open ? CBLifecyclePhase::LIVE : CBLifecyclePhase::CREATED;
            d.mode = (in.streams && in.streams->has_flowing_stream_for_device(id))
                         ? CBDeviceMode::STREAMING
                         : CBDeviceMode::IDLE;
            d.engaged = rec.open;
            d.last_error_code = static_cast<int32_t>(rec.last_error_code);
            d.errors_count = rec.errors_count;

            snap.devices.push_back(std::move(d));
        }
    }

    // Streams
    if (in.streams) {
        snap.streams.reserve(in.streams->all().size());
        for (const auto& [sid, rec] : in.streams->all()) {
            CamBANGStreamState s;
            s.stream_id = sid;
            s.device_instance_id = rec.device_instance_id;
            s.phase = rec.created ? CBLifecyclePhase::LIVE : CBLifecyclePhase::CREATED;

            s.intent = rec.intent;

            if (!rec.created) {
                s.mode = CBStreamMode::STOPPED;
            } else {
                // Packet A intentionally projects only STOPPED/FLOWING from currently
                // retained runtime truth. STARVED/ERROR modes need richer retained
                // state and remain future work.
                s.mode = rec.started ? CBStreamMode::FLOWING : CBStreamMode::STOPPED;
            }

            if (rec.started || rec.last_stop_origin == CoreStreamRegistry::StopOrigin::None) {
                s.stop_reason = CBStreamStopReason::NONE;
            } else if (rec.last_stop_origin == CoreStreamRegistry::StopOrigin::User) {
                s.stop_reason = CBStreamStopReason::USER;
            } else if (rec.last_error_code != 0) {
                s.stop_reason = CBStreamStopReason::PROVIDER;
            } else {
                s.stop_reason = CBStreamStopReason::PROVIDER;
            }
            s.profile_version = rec.profile_version;

            s.width = rec.profile.width;
            s.height = rec.profile.height;
            s.format = rec.profile.format_fourcc;
            s.target_fps_min = rec.profile.target_fps_min;
            s.target_fps_max = rec.profile.target_fps_max;

            s.frames_received = rec.frames_received;
            s.frames_delivered = rec.frames_released; // in this slice, release == delivered to sink
            s.frames_dropped = rec.frames_dropped;
            s.queue_depth = in.ingress ? in.ingress->ingress_depth_for_stream(sid) : 0;
            s.last_frame_ts_ns = rec.last_frame_ts_ns;

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

        if (rec.destroyed) {
            n.phase = CBLifecyclePhase::DESTROYED;
        } else if (!rec.created) {
            n.phase = CBLifecyclePhase::CREATED;
        } else {
            n.phase = CBLifecyclePhase::LIVE;
        }

        n.owner_device_instance_id = rec.owner_device_instance_id;
        n.owner_stream_id = rec.owner_stream_id;

        n.creation_gen = rec.creation_gen;

        n.created_ns = rec.created_ns;
        n.destroyed_ns = rec.destroyed_ns;

        n.bytes_allocated = rec.bytes_allocated;
        n.buffers_in_use = rec.buffers_in_use;

        snap.native_objects.push_back(std::move(n));
    }

    const std::set<uint64_t> detached_roots = compute_detached_roots(in);
    snap.detached_root_ids.assign(detached_roots.begin(), detached_roots.end());
}

// Rigs are not implemented in this scaffolding slice.
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

    if (in.native_objects) {
        std::set<uint64_t> root_ids;

        for (const auto& [native_id, rec] : in.native_objects->all()) {
            (void)native_id;
            if (rec.root_id == 0) {
                continue;
            }
            root_ids.insert(rec.root_id);
        }

        const std::set<uint64_t> detached_root_ids = compute_detached_roots(in);

        fnv1a_u64(h, static_cast<uint64_t>(root_ids.size()));
        for (uint64_t root_id : root_ids) {
            fnv1a_u64(h, root_id);
        }

        fnv1a_u64(h, static_cast<uint64_t>(detached_root_ids.size()));
        for (uint64_t root_id : detached_root_ids) {
            fnv1a_u64(h, root_id);
        }
    }
    return h;
}

} // namespace cambang

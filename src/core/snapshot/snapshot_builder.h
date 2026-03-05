#pragma once

#include <cstdint>

#include "core/snapshot/state_snapshot.h"

namespace cambang {

class CoreDeviceRegistry;
class CoreStreamRegistry;
class ProviderCallbackIngress;
class CoreNativeObjectRegistry;

// Minimal deterministic builder for schema v1 state snapshot.
// Populates implemented fields from current registries; all others use
// canonical defaults (0/empty/STOPPED/etc.).
class SnapshotBuilder final {
public:
    struct Inputs {
        const CoreDeviceRegistry* devices = nullptr;
        const CoreStreamRegistry* streams = nullptr;
        // Provider ingress stats may be used for queue_depth in future.
        const ProviderCallbackIngress* ingress = nullptr;
        const CoreNativeObjectRegistry* native_objects = nullptr;
    };

    CamBANGStateSnapshot build(const Inputs& in,
                              uint64_t gen,
                              uint64_t topology_gen,
                              uint64_t timestamp_ns) const;

    // Topology signature used to decide when topology_gen increments.
    // This is deliberately simple in v1 scaffolding: it tracks existence of
    // device instances and streams by ID.
    uint64_t compute_topology_signature(const Inputs& in) const;
};

} // namespace cambang

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "core/i_state_snapshot_publisher.h"

// Thread-safe "latest snapshot" buffer.
//
// Writer: core thread via publish().
// Readers: any thread via snapshot_copy()/gen()/topology_gen().
//
// This is suitable for smoke and for Godot-side bridging (where Godot thread
// polls and emits signals).

class StateSnapshotBuffer final : public IStateSnapshotPublisher {
public:
    void publish(std::shared_ptr<const CamBANGStateSnapshot> snapshot) override {
        std::lock_guard<std::mutex> lock(mu_);
        latest_ = std::move(snapshot);
    }

    std::shared_ptr<const CamBANGStateSnapshot> snapshot_copy() const {
        std::lock_guard<std::mutex> lock(mu_);
        return latest_;
    }

private:
    mutable std::mutex mu_;
    std::shared_ptr<const CamBANGStateSnapshot> latest_;
};

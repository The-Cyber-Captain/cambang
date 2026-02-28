#pragma once

#include <memory>

// Core-facing publication interface for the public release truth surface.
//
// publish() is called from the CamBANG core thread.
// Implementations MUST NOT touch Godot APIs.

struct CamBANGStateSnapshot;

struct IStateSnapshotPublisher {
    virtual ~IStateSnapshotPublisher() = default;
    virtual void publish(std::shared_ptr<const CamBANGStateSnapshot> snapshot) = 0;
};

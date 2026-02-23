// src/core/core_publisher.h
#pragma once

#include "core/core_snapshot.h"

namespace cambang {

// Publication boundary for core snapshots.
//
// For this build slice, publication is internal and synchronous.
// All publish() calls occur on the core thread.
class ICorePublisher {
public:
  virtual ~ICorePublisher() = default;
  ICorePublisher(const ICorePublisher&) = delete;
  ICorePublisher& operator=(const ICorePublisher&) = delete;

  virtual void publish(CoreSnapshot snapshot) = 0;

protected:
  ICorePublisher() = default;
};

} // namespace cambang

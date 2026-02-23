// src/core/core_publisher_buffer.h
#pragma once

#include "core/core_publisher.h"

#include <mutex>
#include <utility>

namespace cambang {

// A tiny in-memory publisher used during scaffolding.
//
// Core thread calls publish(); other threads may inspect via snapshot_copy().
class CorePublisherBuffer final : public ICorePublisher {
public:
  struct Stats final {
    std::uint64_t publishes = 0;
    std::uint64_t last_seq = 0;
  };

  CorePublisherBuffer() = default;

  void publish(CoreSnapshot snapshot) override {
    std::lock_guard<std::mutex> lock(mu_);
    ++stats_.publishes;
    stats_.last_seq = snapshot.seq;
    last_ = std::move(snapshot);
  }

  // Thread-safe copy of last snapshot (may be empty if never published).
  CoreSnapshot snapshot_copy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_;
  }

  Stats stats_copy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
  }

private:
  mutable std::mutex mu_;
  Stats stats_{};
  CoreSnapshot last_{};
};

} // namespace cambang

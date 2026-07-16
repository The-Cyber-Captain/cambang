#pragma once

#include <cstdint>
#include <mutex>

namespace cambang {

// Keeps an installed target alive for the complete synchronous callback. The
// release callback is intentionally limited to Core-safe owner cleanup.
class DisplayDemandDispatcher final {
public:
  using RetainCallback = uint64_t (*)(void* target, uint64_t stream_id) noexcept;
  using ReleaseCallback = void (*)(void* target, uint64_t lease_token) noexcept;

  void install(
      void* target,
      RetainCallback retain,
      ReleaseCallback release) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      target_ = target;
      retain_ = retain;
      release_ = release;
    } catch (...) {
    }
  }

  void uninstall(void* target) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      if (target_ == target) {
        target_ = nullptr;
        retain_ = nullptr;
        release_ = nullptr;
      }
    } catch (...) {
    }
  }

  uint64_t retain(uint64_t stream_id) noexcept {
    if (stream_id == 0) {
      return 0;
    }
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!target_ || !retain_) {
        return 0;
      }
      return retain_(target_, stream_id);
    } catch (...) {
      return 0;
    }
  }

  void release(uint64_t lease_token) noexcept {
    if (lease_token == 0) {
      return;
    }
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!target_ || !release_) {
        return;
      }
      release_(target_, lease_token);
    } catch (...) {
    }
  }

private:
  std::mutex mutex_;
  void* target_ = nullptr;
  RetainCallback retain_ = nullptr;
  ReleaseCallback release_ = nullptr;
};

} // namespace cambang

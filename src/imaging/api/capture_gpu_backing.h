#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace cambang {

// The provider reserves the actual allocation size before allocating.
// Accounting stays alive across capture eviction and provider shutdown while
// any backing or in-flight work still owns a reservation.
class CaptureGpuBackingBudget final {
  struct State {
    explicit State(uint64_t byte_limit) : limit(byte_limit) {}
    std::mutex mutex;
    const uint64_t limit;
    uint64_t used = 0;
  };

public:
  class Reservation final {
  public:
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    Reservation& operator=(Reservation&&) = delete;
    Reservation(Reservation&&) noexcept = default;
    ~Reservation() {
      if (state_) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->used -= bytes_;
      }
    }

  private:
    friend class CaptureGpuBackingBudget;
    Reservation(std::shared_ptr<State> state, uint64_t bytes)
        : state_(std::move(state)), bytes_(bytes) {}
    std::shared_ptr<State> state_;
    uint64_t bytes_;
  };

  explicit CaptureGpuBackingBudget(uint64_t byte_limit)
      : state_(std::make_shared<State>(byte_limit)) {}

  std::optional<Reservation> try_reserve(uint64_t bytes) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (bytes == 0 || bytes > state_->limit - state_->used) {
      return std::nullopt;
    }
    state_->used += bytes;
    return Reservation(state_, bytes);
  }

private:
  std::shared_ptr<State> state_;
};

enum class CaptureGpuComputeStatus : uint8_t {
  Ok, InvalidArgument, Unavailable, Busy, Cancelled, Failed,
};

struct CaptureGpuComputeRequest {
  static constexpr uint32_t max_shader_bytes = 64 * 1024;
  static constexpr uint32_t max_parameter_bytes = 128;
  static constexpr uint32_t max_output_bytes = 4096;
  static constexpr uint32_t max_workgroups = 4096;

  std::vector<uint32_t> shader;
  std::array<uint8_t, max_parameter_bytes> parameters{};
  std::array<uint32_t, 3> workgroups{};
  uint32_t output_bytes = 0;
};

using CaptureGpuComputeCompletion =
    std::function<void(CaptureGpuComputeStatus, std::vector<uint8_t>)>;

// Provider-owned immutable GPU pixels. Core holds only the ownership token.
// GPU jobs hold the backing until physical completion. The derived object
// releases its allocation before the base releases the reservation.
class CaptureGpuBacking {
public:
  virtual ~CaptureGpuBacking() = default;
  CaptureGpuBacking(const CaptureGpuBacking&) = delete;
  CaptureGpuBacking& operator=(const CaptureGpuBacking&) = delete;

  // Returning Ok transfers completion to the provider. Its worker may invoke
  // the callback on any thread after physically retiring accepted GPU work.
  virtual CaptureGpuComputeStatus submit_compute(CaptureGpuComputeRequest,
                                                  CaptureGpuComputeCompletion) {
    return CaptureGpuComputeStatus::Unavailable;
  }

protected:
  explicit CaptureGpuBacking(CaptureGpuBackingBudget::Reservation reservation)
      : reservation_(std::move(reservation)) {}

private:
  CaptureGpuBackingBudget::Reservation reservation_;
};

enum class CaptureGpuClaimStatus : uint8_t {
  Claimed,
  Unavailable,
  BudgetExhausted,
  Expired,
  AlreadyClaimed,
};

// A provider puts an offer on the exact still member associated with it.
// Claim consumes the offer. Copies of the FrameView or result share the same
// offer, so copying a result cannot manufacture another claim. Provider
// maintenance expires unclaimed offers even if a result wrapper still exists.
class CaptureGpuBackingOffer final {
public:
  using Clock = std::chrono::steady_clock;
  struct Claim {
    CaptureGpuClaimStatus status;
    std::shared_ptr<CaptureGpuBacking> backing;
  };

  CaptureGpuBackingOffer(std::shared_ptr<CaptureGpuBacking> backing,
                        Clock::time_point claim_deadline)
      : deadline_(claim_deadline), backing_(std::move(backing)) {}

  explicit CaptureGpuBackingOffer(CaptureGpuClaimStatus failure)
      : status_(failure == CaptureGpuClaimStatus::BudgetExhausted
                    ? failure : CaptureGpuClaimStatus::Unavailable) {}

  bool is_available(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backing_ && now < deadline_;
  }

  Claim claim(Clock::time_point now) {
    std::shared_ptr<CaptureGpuBacking> expired;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!backing_) {
      return {status_, {}};
    }
    if (now >= deadline_) {
      status_ = CaptureGpuClaimStatus::Expired;
      expired = std::move(backing_);
      return {status_, {}};
    }
    status_ = CaptureGpuClaimStatus::AlreadyClaimed;
    return {CaptureGpuClaimStatus::Claimed, std::move(backing_)};
  }

  // Returns true when there is nothing left for the expiry scheduler to track.
  bool expire_if_due(Clock::time_point now) {
    std::shared_ptr<CaptureGpuBacking> expired;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!backing_) {
        return true;
      }
      if (now < deadline_) {
        return false;
      }
      status_ = CaptureGpuClaimStatus::Expired;
      expired = std::move(backing_);
    }
    // Backend destruction can release memory or enqueue retirement. The offer
    // lock is already free when the last reference is dropped here.
    return true;
  }

private:
  const Clock::time_point deadline_{};
  mutable std::mutex mutex_;
  std::shared_ptr<CaptureGpuBacking> backing_;
  CaptureGpuClaimStatus status_ = CaptureGpuClaimStatus::Unavailable;
};

} // namespace cambang

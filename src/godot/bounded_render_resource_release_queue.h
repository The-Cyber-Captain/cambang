#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace cambang {

// Fixed storage makes destructor-originating admission allocation-free. Items
// are extracted one at a time so destruction always occurs after unlocking.
template <typename Item, size_t Capacity>
class BoundedRenderResourceReleaseQueue final {
  static_assert(Capacity > 0);
  static_assert(std::is_nothrow_move_constructible_v<Item>);

public:
  enum class AdmissionResult : unsigned char {
    Accepted,
    Closed,
    Full,
    AllocationFailure,
  };

  void open() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    LockMarker marker;
    accepting_ = true;
    terminal_ = false;
  }

  AdmissionResult admit(Item&& item) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    LockMarker marker;
#if defined(CAMBANG_INTERNAL_SMOKE)
    if (force_allocation_failure_once_) {
      force_allocation_failure_once_ = false;
      return AdmissionResult::AllocationFailure;
    }
    if (force_full_once_) {
      force_full_once_ = false;
      return AdmissionResult::Full;
    }
#endif
    if (!accepting_ || terminal_) {
      return AdmissionResult::Closed;
    }
    if (count_ == Capacity) {
      return AdmissionResult::Full;
    }
    slots_[tail_].emplace(std::move(item));
    tail_ = (tail_ + 1u) % Capacity;
    ++count_;
    return AdmissionResult::Accepted;
  }

  std::optional<Item> take_one() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    LockMarker marker;
    if (count_ == 0) {
      return std::nullopt;
    }
    std::optional<Item> out(std::move(slots_[head_]));
    slots_[head_].reset();
    head_ = (head_ + 1u) % Capacity;
    --count_;
    return out;
  }

  void close_terminal() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    LockMarker marker;
    accepting_ = false;
    terminal_ = true;
  }

  bool has_pending() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    LockMarker marker;
    return count_ != 0;
  }

  size_t size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    LockMarker marker;
    return count_;
  }

  static constexpr size_t capacity() noexcept { return Capacity; }
  static bool lock_held_by_current_thread() noexcept { return lock_held_; }

#if defined(CAMBANG_INTERNAL_SMOKE)
  void force_next_full_for_smoke() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    LockMarker marker;
    force_full_once_ = true;
  }

  void force_next_allocation_failure_for_smoke() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    LockMarker marker;
    force_allocation_failure_once_ = true;
  }
#endif

private:
  struct LockMarker final {
    LockMarker() noexcept { lock_held_ = true; }
    ~LockMarker() { lock_held_ = false; }
  };

  inline static thread_local bool lock_held_ = false;
  mutable std::mutex mutex_;
  std::array<std::optional<Item>, Capacity> slots_{};
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t count_ = 0;
  bool accepting_ = false;
  bool terminal_ = false;
#if defined(CAMBANG_INTERNAL_SMOKE)
  bool force_full_once_ = false;
  bool force_allocation_failure_once_ = false;
#endif
};

} // namespace cambang

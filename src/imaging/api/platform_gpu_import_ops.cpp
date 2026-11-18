#include "imaging/api/platform_gpu_import_ops.h"

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace cambang {
namespace {

// Admission-counted registry: a call that has been admitted always completes
// against the table it was admitted with, and clear() returns only after every
// such call has. That is what lets the Godot layer uninstall its table and then
// tear down the device state the table's functions use.
class Registry final {
public:
  class Lease final {
  public:
    Lease(Registry& registry, const PlatformGpuImportOps* ops) noexcept
        : registry_(registry), ops_(ops) {}
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    ~Lease() {
      if (ops_) registry_.release_();
    }
    const PlatformGpuImportOps* ops() const noexcept { return ops_; }
    explicit operator bool() const noexcept { return ops_ != nullptr; }

  private:
    Registry& registry_;
    const PlatformGpuImportOps* ops_ = nullptr;
  };

  void install(const PlatformGpuImportOps* ops) noexcept {
    clear();
    std::lock_guard<std::mutex> lock(mutex_);
    ops_ = ops;
  }

  void clear() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    ops_ = nullptr;
    changed_.wait(lock, [this] { return in_flight_ == 0; });
  }

  Lease acquire() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ops_) return Lease(*this, nullptr);
    ++in_flight_;
    return Lease(*this, ops_);
  }

private:
  void release_() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    --in_flight_;
    changed_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  const PlatformGpuImportOps* ops_ = nullptr;
  std::size_t in_flight_ = 0;
};

// Never destroyed. Camera callback threads and the render thread can call in
// while the process is exiting, after static destructors have begun; a
// destroyed registry mutex would then abort in pthread_mutex_lock (the
// FORTIFY "destroyed mutex" abort).
Registry& registry() noexcept {
  static Registry* const instance = new Registry();
  return *instance;
}

} // namespace

void set_platform_gpu_import_ops(const PlatformGpuImportOps* ops) noexcept {
  registry().install(ops);
}

void clear_platform_gpu_import_ops() noexcept {
  registry().clear();
}

bool platform_gpu_import_can_copy_native_buffers() noexcept {
  const Registry::Lease lease = registry().acquire();
  return lease && lease.ops()->can_copy_native_buffers &&
         lease.ops()->can_copy_native_buffers();
}

std::shared_ptr<void> platform_gpu_import_begin_copy(const PlatformGpuCopyRequest& request) noexcept {
  const Registry::Lease lease = registry().acquire();
  if (!lease || !lease.ops()->begin_copy) return {};
  return lease.ops()->begin_copy(request);
}

std::shared_ptr<void> platform_gpu_import_finish_copy(const std::shared_ptr<void>& job,
                                                      uint64_t timeout_ns) noexcept {
  if (!job) return {};
  const Registry::Lease lease = registry().acquire();
  if (!lease || !lease.ops()->finish_copy) return {};
  return lease.ops()->finish_copy(job, timeout_ns);
}

} // namespace cambang

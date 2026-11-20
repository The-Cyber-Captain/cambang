#pragma once

#include "imaging/api/capture_gpu_backing.h"

#include <cstdint>
#include <functional>
#include <memory>

struct AImage;

namespace cambang::camera2_detail {

class Camera2GpuDevice;

// Holds the reader together with the acquired image. Submission consumes the
// image only after a successful queue submit. The copy keeps the source until
// it can return it with a release fence or the GPU no longer uses it.
struct Camera2GpuSource {
  std::shared_ptr<void> reader;
  AImage* image = nullptr;
};

class Camera2GpuCopy {
public:
  ~Camera2GpuCopy();
  Camera2GpuCopy(const Camera2GpuCopy&) = delete;
  Camera2GpuCopy& operator=(const Camera2GpuCopy&) = delete;

  // nullptr while the copy is running. A completed offer is returned once.
  std::shared_ptr<CaptureGpuBackingOffer> poll();

  // The CPU path may read the source only after poll() reports retirement.
  Camera2GpuSource take_retired_source();

private:
  struct State;
  explicit Camera2GpuCopy(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
  friend class Camera2GpuDevice;
};

class Camera2GpuDevice {
public:
  using Dispatch = std::function<CaptureGpuComputeStatus(std::function<void(bool)>)>;
  static std::shared_ptr<Camera2GpuDevice> open(uint64_t backing_budget, Dispatch dispatch = {});

  ~Camera2GpuDevice();
  Camera2GpuDevice(const Camera2GpuDevice&) = delete;
  Camera2GpuDevice& operator=(const Camera2GpuDevice&) = delete;

  struct Submission {
    std::unique_ptr<Camera2GpuCopy> copy;
    CaptureGpuClaimStatus failure = CaptureGpuClaimStatus::Unavailable;
  };

  // The provider worker serializes submits and polls. Source must have been
  // synchronously acquired and must not have been CPU-locked.
  // When keep_source is set, the retired source can be taken for CPU access.
  Submission submit(Camera2GpuSource& source, uint32_t width, uint32_t height,
                    bool keep_source = false);

private:
  struct State;
  explicit Camera2GpuDevice(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
  friend class Camera2GpuCopy;
};

// Serializes access to the provider's private Vulkan queue. A source can stay
// queued only while it has exclusive permission to use a retained reader slot.
class Camera2CaptureGpuService {
public:
  using Offer = std::shared_ptr<CaptureGpuBackingOffer>;
  using OwnedImage = std::unique_ptr<AImage, void (*)(AImage*)>;
  // Invoked on the provider worker after the Vulkan source read retires.
  // The image may be null when cancelling or following a device loss.
  using SourceCompletion = std::function<void(OwnedImage, Offer)>;

  Camera2CaptureGpuService();
  ~Camera2CaptureGpuService();
  Camera2CaptureGpuService(const Camera2CaptureGpuService&) = delete;
  Camera2CaptureGpuService& operator=(const Camera2CaptureGpuService&) = delete;

  bool available_for_capture();
  bool enqueue(Camera2GpuSource& source, uint32_t width, uint32_t height,
               SourceCompletion completed);
  void stop();

private:
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace cambang::camera2_detail

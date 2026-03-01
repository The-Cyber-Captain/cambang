#include "imaging/synthetic/provider.h"

namespace cambang {

ProviderResult SyntheticProvider::initialize(IProviderCallbacks* callbacks) {
  if (!callbacks) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  callbacks_ = callbacks;
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) {
  (void)out_endpoints;
  // No endpoints until the synthetic scenarios/pattern module is implemented.
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::open_device(const std::string&, uint64_t, uint64_t) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult SyntheticProvider::close_device(uint64_t) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult SyntheticProvider::create_stream(const StreamRequest&) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult SyntheticProvider::destroy_stream(uint64_t) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult SyntheticProvider::start_stream(uint64_t) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult SyntheticProvider::stop_stream(uint64_t) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult SyntheticProvider::trigger_capture(const CaptureRequest&) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult SyntheticProvider::abort_capture(uint64_t) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult SyntheticProvider::apply_camera_spec_patch(const std::string&, uint64_t, SpecPatchView) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult SyntheticProvider::apply_imaging_spec_patch(uint64_t, SpecPatchView) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult SyntheticProvider::shutdown() {
  callbacks_ = nullptr;
  return ProviderResult::success();
}

} // namespace cambang

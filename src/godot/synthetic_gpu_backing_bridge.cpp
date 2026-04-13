#include "imaging/synthetic/gpu_backing_runtime.h"

#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <vector>

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace cambang {
namespace {

struct RetainedDisplaySurface final {
  godot::Ref<godot::Texture2D> texture;
};

bool gpu_trace_enabled() noexcept {
  const char* value = std::getenv("CAMBANG_DEV_SYNTH_GPU_TRACE");
  return value && value[0] != '\0' && value[0] != '0';
}

void trace_gpu(const char* message) {
  if (!gpu_trace_enabled()) {
    return;
  }
  godot::UtilityFunctions::print("[CamBANG][SyntheticGPU] ", message);
}

void trace_runtime_query(bool global_rd_ptr, bool runtime_truth_gpu_available) {
  if (!gpu_trace_enabled()) {
    return;
  }
  godot::UtilityFunctions::print(
      "[CamBANG][SyntheticGPU] runtime_query global_rd_ptr=",
      global_rd_ptr,
      " runtime_truth_gpu_available=",
      runtime_truth_gpu_available);
}

bool global_rd_available() noexcept {
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    trace_runtime_query(false, false);
    return false;
  }
  godot::RenderingDevice* rd = rs->get_rendering_device();
  const bool available = rd != nullptr;
  trace_runtime_query(available, available);
  return available;
}

bool global_rd_roundtrip_rgba8(
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes,
    std::vector<uint8_t>& out) noexcept {
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs || !src || width == 0 || height == 0) {
    trace_gpu("roundtrip_result texture_create=false readback=false success=false reason=invalid_input_or_server");
    return false;
  }
  godot::RenderingDevice* rd = rs->get_rendering_device();
  if (!rd) {
    trace_gpu("roundtrip_result texture_create=false readback=false success=false reason=no_global_rd");
    return false;
  }
  if (stride_bytes != width * 4u) {
    trace_gpu("roundtrip_result texture_create=false readback=false success=false reason=unexpected_stride");
    return false;
  }

  godot::Ref<godot::RDTextureFormat> format;
  format.instantiate();
  format->set_width(static_cast<int64_t>(width));
  format->set_height(static_cast<int64_t>(height));
  format->set_format(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
  format->set_usage_bits(
      godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
      godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
      godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);

  godot::Ref<godot::RDTextureView> view;
  view.instantiate();

  godot::PackedByteArray initial;
  const size_t expected_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  initial.resize(static_cast<int64_t>(expected_size));
  std::memcpy(initial.ptrw(), src, static_cast<size_t>(initial.size()));

  godot::Array data;
  data.push_back(initial);
  const godot::RID tex = rd->texture_create(format, view, data);
  if (!tex.is_valid()) {
    trace_gpu("roundtrip_result texture_create=false readback=false success=false");
    return false;
  }

  const godot::PackedByteArray readback = rd->texture_get_data(tex, 0);
  rd->free_rid(tex);
  if (readback.size() <= 0) {
    trace_gpu("roundtrip_result texture_create=true readback=false success=false");
    return false;
  }
  out.resize(expected_size);
  if (static_cast<size_t>(readback.size()) < out.size()) {
    trace_gpu("roundtrip_result texture_create=true readback=false success=false reason=short_readback");
    return false;
  }
  std::memcpy(out.data(), readback.ptr(), out.size());
  trace_gpu("roundtrip_result texture_create=true readback=true success=true");
  return true;
}

std::shared_ptr<void> retain_display_surface_rgba8(
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  if (!src || width == 0 || height == 0 || stride_bytes != width * 4u) {
    return {};
  }

  godot::Ref<godot::Image> image;
  image.instantiate();
  if (image.is_null()) {
    return {};
  }
  godot::PackedByteArray bytes;
  bytes.resize(static_cast<int64_t>(stride_bytes) * static_cast<int64_t>(height));
  std::memcpy(bytes.ptrw(), src, static_cast<size_t>(bytes.size()));
  image->set_data(static_cast<int64_t>(width),
                  static_cast<int64_t>(height),
                  false,
                  godot::Image::FORMAT_RGBA8,
                  bytes);

  godot::Ref<godot::ImageTexture> texture = godot::ImageTexture::create_from_image(image);
  if (texture.is_null()) {
    return {};
  }

  auto surface = std::make_shared<RetainedDisplaySurface>();
  surface->texture = texture;
  return std::static_pointer_cast<void>(surface);
}

const SyntheticGpuBackingRuntimeOps kOps{
    &global_rd_available,
    &global_rd_roundtrip_rgba8,
    &retain_display_surface_rgba8,
};

} // namespace

void install_synthetic_gpu_backing_godot_bridge() {
  set_synthetic_gpu_backing_runtime_ops(&kOps);
  trace_gpu("bridge_install runtime_ops_registered=true");
}

void uninstall_synthetic_gpu_backing_godot_bridge() {
  clear_synthetic_gpu_backing_runtime_ops();
  trace_gpu("bridge_uninstall runtime_ops_registered=false");
}

godot::Ref<godot::Texture2D> synthetic_gpu_backing_display_texture(const std::shared_ptr<void>& surface) {
  if (!surface) {
    return {};
  }
  const std::shared_ptr<RetainedDisplaySurface> retained = std::static_pointer_cast<RetainedDisplaySurface>(surface);
  return retained ? retained->texture : godot::Ref<godot::Texture2D>();
}

} // namespace cambang

#include "imaging/synthetic/gpu_backing_runtime.h"

#include <cstring>
#include <cstdint>
#include <vector>

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace cambang {
namespace {

bool global_rd_available() noexcept {
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) return false;
  godot::RenderingDevice* rd = rs->get_rendering_device();
  return rd != nullptr;
}

bool global_rd_roundtrip_rgba8(
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes,
    std::vector<uint8_t>& out) noexcept {
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs || !src || width == 0 || height == 0) return false;
  godot::RenderingDevice* rd = rs->get_rendering_device();
  if (!rd) return false;
  if (stride_bytes != width * 4u) return false;

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
    return false;
  }

  const godot::PackedByteArray readback = rd->texture_get_data(tex, 0);
  rd->free_rid(tex);
  if (readback.size() <= 0) {
    return false;
  }
  out.resize(expected_size);
  if (static_cast<size_t>(readback.size()) < out.size()) {
    return false;
  }
  std::memcpy(out.data(), readback.ptr(), out.size());
  return true;
}

const SyntheticGpuBackingRuntimeOps kOps{
    &global_rd_available,
    &global_rd_roundtrip_rgba8,
};

} // namespace

void install_synthetic_gpu_backing_godot_bridge() {
  set_synthetic_gpu_backing_runtime_ops(&kOps);
}

void uninstall_synthetic_gpu_backing_godot_bridge() {
  clear_synthetic_gpu_backing_runtime_ops();
}

} // namespace cambang

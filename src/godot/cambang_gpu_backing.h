#pragma once

#include "imaging/api/capture_gpu_backing.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/vector3i.hpp>

namespace cambang {

class CamBANGGpuBacking final : public godot::RefCounted {
  GDCLASS(CamBANGGpuBacking, godot::RefCounted)

public:
  static godot::Dictionary claim_offer(const std::shared_ptr<CaptureGpuBackingOffer>& offer);

  godot::Error submit_compute(const godot::Ref<godot::RDShaderSPIRV>& shader,
                              const godot::PackedByteArray& parameters,
                              const godot::Vector3i& workgroups, int output_bytes,
                              const godot::Callable& completed) const;

protected:
  static void _bind_methods();

private:
  std::shared_ptr<CaptureGpuBacking> backing_;
};

} // namespace cambang

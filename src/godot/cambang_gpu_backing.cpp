#include "godot/cambang_gpu_backing.h"

#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rendering_device.hpp>

#include <cstring>

namespace cambang {
namespace {

godot::Error compute_error(CaptureGpuComputeStatus status) {
  switch (status) {
    case CaptureGpuComputeStatus::Ok: return godot::OK;
    case CaptureGpuComputeStatus::InvalidArgument: return godot::ERR_INVALID_PARAMETER;
    case CaptureGpuComputeStatus::Unavailable: return godot::ERR_UNAVAILABLE;
    case CaptureGpuComputeStatus::Busy: return godot::ERR_BUSY;
    case CaptureGpuComputeStatus::Cancelled: return godot::ERR_SKIP;
    case CaptureGpuComputeStatus::Failed: return godot::FAILED;
  }
  return godot::FAILED;
}

} // namespace

godot::Dictionary CamBANGGpuBacking::claim_offer(
    const std::shared_ptr<CaptureGpuBackingOffer>& offer) {
  godot::Dictionary out;
  out["backing"] = godot::Variant();
  if (!offer) {
    out["error"] = godot::ERR_UNAVAILABLE;
    return out;
  }
  auto claim = offer->claim(CaptureGpuBackingOffer::Clock::now());
  switch (claim.status) {
    case CaptureGpuClaimStatus::Claimed: {
      godot::Ref<CamBANGGpuBacking> backing;
      backing.instantiate();
      backing->backing_ = std::move(claim.backing);
      out["backing"] = backing;
      out["error"] = godot::OK;
      break;
    }
    case CaptureGpuClaimStatus::AlreadyClaimed:
      out["error"] = godot::ERR_ALREADY_IN_USE;
      break;
    case CaptureGpuClaimStatus::Expired:
      out["error"] = godot::ERR_TIMEOUT;
      break;
    case CaptureGpuClaimStatus::BudgetExhausted:
      out["error"] = godot::ERR_OUT_OF_MEMORY;
      break;
    case CaptureGpuClaimStatus::Unavailable:
      out["error"] = godot::ERR_UNAVAILABLE;
      break;
  }
  return out;
}

godot::Error CamBANGGpuBacking::submit_compute(
    const godot::Ref<godot::RDShaderSPIRV>& shader,
    const godot::PackedByteArray& parameters, const godot::Vector3i& workgroups,
    int output_bytes, const godot::Callable& completed) const {
  if (!backing_) return godot::ERR_UNAVAILABLE;
  if (shader.is_null() || !completed.is_valid() ||
      output_bytes <= 0 || output_bytes > CaptureGpuComputeRequest::max_output_bytes ||
      output_bytes % 4 != 0 || parameters.size() % 4 != 0 ||
      parameters.size() > CaptureGpuComputeRequest::max_parameter_bytes ||
      workgroups.x < 1 || workgroups.y < 1 || workgroups.z < 1 ||
      workgroups.x > CaptureGpuComputeRequest::max_workgroups ||
      workgroups.y > CaptureGpuComputeRequest::max_workgroups ||
      workgroups.z > CaptureGpuComputeRequest::max_workgroups ||
      int64_t(workgroups.x) * workgroups.y * workgroups.z > CaptureGpuComputeRequest::max_workgroups) {
    return godot::ERR_INVALID_PARAMETER;
  }

  const auto bytecode = shader->get_stage_bytecode(godot::RenderingDevice::SHADER_STAGE_COMPUTE);
  if (bytecode.size() < 20 || bytecode.size() > CaptureGpuComputeRequest::max_shader_bytes ||
      bytecode.size() % 4 != 0) {
    return godot::ERR_INVALID_PARAMETER;
  }

  CaptureGpuComputeRequest request;
  request.shader.resize(bytecode.size() / sizeof(uint32_t));
  std::memcpy(request.shader.data(), bytecode.ptr(), bytecode.size());
  if (!parameters.is_empty()) {
    std::memcpy(request.parameters.data(), parameters.ptr(), parameters.size());
  }
  request.workgroups = {uint32_t(workgroups.x), uint32_t(workgroups.y), uint32_t(workgroups.z)};
  request.output_bytes = static_cast<uint32_t>(output_bytes);
  return compute_error(backing_->submit_compute(
      std::move(request), [completed](CaptureGpuComputeStatus result, std::vector<uint8_t> bytes) {
        godot::PackedByteArray output;
        if (!bytes.empty()) {
          if (output.resize(bytes.size()) != godot::OK) {
            completed.call_deferred(godot::ERR_OUT_OF_MEMORY, godot::PackedByteArray());
            return;
          }
          std::memcpy(output.ptrw(), bytes.data(), bytes.size());
        }
        completed.call_deferred(compute_error(result), output);
      }));
}

void CamBANGGpuBacking::_bind_methods() {
  godot::ClassDB::bind_method(
      godot::D_METHOD("submit_compute", "shader", "parameters", "workgroups", "output_bytes", "completed"),
      &CamBANGGpuBacking::submit_compute);
}

} // namespace cambang

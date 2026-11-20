#include "camera2_compute_validation.h"

#include <spirv-tools/libspirv.h>
#include <spirv/unified1/spirv.hpp>
#include <unordered_map>

namespace cambang::camera2_detail {
namespace {

constexpr uint32_t kUnset = UINT32_MAX;

struct Object {
  const uint32_t* type = nullptr;
  uint16_t type_words = 0;
  uint32_t storage = kUnset;
  uint32_t pointer = 0;
  uint32_t set = kUnset;
  uint32_t binding = kUnset;
  bool block = false;
  bool buffer_block = false;
  std::unordered_map<uint32_t, uint32_t> offsets;
};

using Objects = std::unordered_map<uint32_t, Object>;

const Object* get(const Objects& objects, uint32_t id) {
  const auto it = objects.find(id);
  return it == objects.end() ? nullptr : &it->second;
}

spv::Op opcode(const Object* object) {
  return object && object->type ? static_cast<spv::Op>(object->type[0] & 0xffff) : spv::OpNop;
}

uint32_t push_member_size(const Objects& objects, const Object* item) {
  switch (opcode(item)) {
    case spv::OpTypeFloat:
    case spv::OpTypeInt:
      return item->type[2] == 32 ? 4 : 0;
    case spv::OpTypeVector: {
      const auto width = push_member_size(objects, get(objects, item->type[2]));
      return width * item->type[3];
    }
    default:
      return 0;
  }
}

bool is_sampled_plane(const Objects& objects, const Object* item) {
  if (opcode(item) != spv::OpTypeSampledImage) return false;
  const auto* image = get(objects, item->type[2]);
  if (opcode(image) != spv::OpTypeImage) return false;
  const auto* scalar = get(objects, image->type[2]);
  return opcode(scalar) == spv::OpTypeFloat && scalar->type[2] == 32 &&
         image->type[3] == spv::Dim2D && image->type[4] == 0 && image->type[5] == 0 &&
         image->type[6] == 0 && image->type[7] == 1;
}

bool reflect_compute(const CaptureGpuComputeRequest& request, const VkPhysicalDeviceLimits& limits) {
  Objects objects;
  uint32_t entry = 0;
  uint32_t entry_count = 0;
  uint32_t local_entry = 0;
  uint32_t builtin_workgroup = 0;
  std::array<uint32_t, 3> local{};
  for (size_t offset = 5; offset < request.shader.size();) {
    const auto* words = request.shader.data() + offset;
    const auto count = static_cast<uint16_t>(words[0] >> 16);
    offset += count;
    switch (static_cast<spv::Op>(words[0] & 0xffff)) {
      case spv::OpCapability:
        if (words[1] != spv::CapabilityShader && words[1] != spv::CapabilityImageQuery) return false;
        break;
      case spv::OpExtension:
      case spv::OpExecutionModeId:
      case spv::OpGroupDecorate:
      case spv::OpGroupMemberDecorate:
        return false;
      case spv::OpEntryPoint:
        if (++entry_count != 1 || words[1] != spv::ExecutionModelGLCompute ||
            count < 5 || words[3] != 0x6e69616d || words[4] != 0) return false; // "main"
        entry = words[2];
        break;
      case spv::OpExecutionMode:
        if (words[2] != spv::ExecutionModeLocalSize) return false;
        local_entry = words[1];
        local = {words[3], words[4], words[5]};
        break;
      case spv::OpTypeFloat:
      case spv::OpTypeInt:
      case spv::OpTypeVector:
      case spv::OpTypeImage:
      case spv::OpTypeSampledImage:
      case spv::OpTypeStruct:
      case spv::OpTypePointer:
        objects[words[1]].type = words;
        objects[words[1]].type_words = count;
        break;
      case spv::OpConstant:
      case spv::OpConstantComposite:
        objects[words[2]].type = words;
        objects[words[2]].type_words = count;
        break;
      case spv::OpVariable:
        objects[words[2]].storage = words[3];
        objects[words[2]].pointer = words[1];
        break;
      case spv::OpDecorate: {
        auto& value = objects[words[1]];
        switch (words[2]) {
          case spv::DecorationDescriptorSet: value.set = words[3]; break;
          case spv::DecorationBinding: value.binding = words[3]; break;
          case spv::DecorationBlock: value.block = true; break;
          case spv::DecorationBufferBlock: value.buffer_block = true; break;
          case spv::DecorationBuiltIn:
            if (words[3] == spv::BuiltInWorkgroupSize) builtin_workgroup = words[1];
            break;
          default: break;
        }
        break;
      }
      case spv::OpMemberDecorate:
        if (words[3] == spv::DecorationOffset) objects[words[1]].offsets[words[2]] = words[4];
        break;
      default:
        break;
    }
  }
  if (entry == 0 || local_entry != entry) return false;
  if (builtin_workgroup) {
    const auto* group = get(objects, builtin_workgroup);
    if (opcode(group) != spv::OpConstantComposite || group->type_words != 6) return false;
    for (size_t i = 0; i < local.size(); ++i) {
      const auto* component = get(objects, group->type[i + 3]);
      if (opcode(component) != spv::OpConstant || component->type_words != 4 ||
          component->type[3] != local[i]) return false;
    }
  }
  uint64_t invocations = 1;
  for (size_t axis = 0; axis < 3; ++axis) {
    if (!local[axis] || local[axis] > limits.maxComputeWorkGroupSize[axis] ||
        request.workgroups[axis] > limits.maxComputeWorkGroupCount[axis]) return false;
    invocations *= local[axis];
  }
  if (invocations > limits.maxComputeWorkGroupInvocations) return false;

  bool seen_bindings[] = {false, false, false};
  bool seen_push = false;
  for (const auto& [id, variable] : objects) {
    const auto storage = variable.storage;
    if (storage == kUnset || storage == spv::StorageClassFunction || storage == spv::StorageClassPrivate ||
        storage == spv::StorageClassWorkgroup || storage == spv::StorageClassInput ||
        storage == spv::StorageClassOutput) continue;
    const auto* pointer = get(objects, variable.pointer);
    if (opcode(pointer) != spv::OpTypePointer) return false;
    const auto* target = get(objects, pointer->type[3]);
    if (storage == spv::StorageClassPushConstant) {
      if (seen_push || opcode(target) != spv::OpTypeStruct) return false;
      seen_push = true;
      for (uint32_t member = 0; member + 2 < target->type_words; ++member) {
        const auto size = push_member_size(objects, get(objects, target->type[member + 2]));
        const auto start = target->offsets.find(member);
        if (size == 0 || start == target->offsets.end() ||
            start->second > CaptureGpuComputeRequest::max_parameter_bytes - size) return false;
      }
      continue;
    }
    if (variable.set != 0 || variable.binding >= 3 || seen_bindings[variable.binding]) return false;
    seen_bindings[variable.binding] = true;
    if (variable.binding < 2) {
      if (storage != spv::StorageClassUniformConstant || !is_sampled_plane(objects, target)) return false;
    } else if (opcode(target) != spv::OpTypeStruct ||
               !((storage == spv::StorageClassStorageBuffer && target->block) ||
                 (storage == spv::StorageClassUniform && target->buffer_block))) {
      return false;
    }
  }
  return seen_bindings[2];
}

} // namespace

bool validate_capture_compute(const CaptureGpuComputeRequest& request,
                              const VkPhysicalDeviceLimits& limits) {
  auto context = spvContextCreate(SPV_ENV_VULKAN_1_1);
  if (!context) return false;
  const auto valid = spvValidateBinary(context, request.shader.data(), request.shader.size(), nullptr);
  spvContextDestroy(context);
  return valid == SPV_SUCCESS && reflect_compute(request, limits);
}

} // namespace cambang::camera2_detail

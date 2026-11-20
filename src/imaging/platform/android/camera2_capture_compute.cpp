#include "camera2_capture_compute.h"

#include <cstring>

namespace cambang::camera2_detail {
namespace {

template<class T>
T structure(VkStructureType kind) {
  T value{};
  value.sType = kind;
  return value;
}

struct DispatchResources {
  VkDevice device;
  VkSampler sampler{};
  VkBuffer output{};
  VkDeviceMemory memory{};
  void* mapped = nullptr;
  VkShaderModule shader{};
  VkDescriptorSetLayout bindings{};
  VkDescriptorPool descriptors{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{};
  VkCommandPool commands{};
  VkFence fence{};

  ~DispatchResources() {
    if (mapped) vkUnmapMemory(device, memory);
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, commands, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorPool(device, descriptors, nullptr);
    vkDestroyDescriptorSetLayout(device, bindings, nullptr);
    vkDestroyShaderModule(device, shader, nullptr);
    vkDestroySampler(device, sampler, nullptr);
    vkDestroyBuffer(device, output, nullptr);
    vkFreeMemory(device, memory, nullptr);
  }

  VkResult initialize_output(const VkPhysicalDeviceMemoryProperties& properties, uint32_t size);
  VkResult initialize_program(const CaptureGpuComputeRequest& request,
                              const std::array<VkImageView, 2>& planes, VkDescriptorSet& set);
};

VkResult DispatchResources::initialize_output(const VkPhysicalDeviceMemoryProperties& properties,
                                               uint32_t size) {
  auto buffer = structure<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
  buffer.size = size;
  buffer.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (const auto r = vkCreateBuffer(device, &buffer, nullptr, &output); r != VK_SUCCESS) return r;
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(device, output, &needs);
  auto alloc = structure<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
  alloc.allocationSize = needs.size;
  constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (; alloc.memoryTypeIndex < properties.memoryTypeCount; ++alloc.memoryTypeIndex) {
    if ((needs.memoryTypeBits & (1u << alloc.memoryTypeIndex)) &&
        (properties.memoryTypes[alloc.memoryTypeIndex].propertyFlags & flags) == flags) break;
  }
  if (alloc.memoryTypeIndex == properties.memoryTypeCount) return VK_ERROR_FEATURE_NOT_PRESENT;
  if (const auto r = vkAllocateMemory(device, &alloc, nullptr, &memory); r != VK_SUCCESS) return r;
  if (const auto r = vkBindBufferMemory(device, output, memory, 0); r != VK_SUCCESS) return r;
  if (const auto r = vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped); r != VK_SUCCESS) return r;
  std::memset(mapped, 0, size);
  return VK_SUCCESS;
}

VkResult DispatchResources::initialize_program(const CaptureGpuComputeRequest& request,
                                                const std::array<VkImageView, 2>& planes,
                                                VkDescriptorSet& set) {
  auto samp = structure<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
  samp.addressModeU = samp.addressModeV = samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (const auto r = vkCreateSampler(device, &samp, nullptr, &sampler); r != VK_SUCCESS) return r;

  const VkDescriptorSetLayoutBinding slots[] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  };
  auto binding_info = structure<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
  binding_info.bindingCount = std::size(slots);
  binding_info.pBindings = slots;
  if (const auto r = vkCreateDescriptorSetLayout(device, &binding_info, nullptr, &bindings); r != VK_SUCCESS) return r;
  constexpr VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, CaptureGpuComputeRequest::max_parameter_bytes};
  auto layout_info = structure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &bindings;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &range;
  if (const auto r = vkCreatePipelineLayout(device, &layout_info, nullptr, &layout); r != VK_SUCCESS) return r;
  auto code = structure<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
  code.codeSize = request.shader.size() * sizeof(uint32_t);
  code.pCode = request.shader.data();
  if (const auto r = vkCreateShaderModule(device, &code, nullptr, &shader); r != VK_SUCCESS) return r;
  auto create = structure<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
  create.layout = layout;
  create.stage = structure<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
  create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  create.stage.module = shader;
  create.stage.pName = "main";
  if (const auto r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline); r != VK_SUCCESS) return r;

  constexpr VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
  auto pool = structure<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
  pool.maxSets = 1;
  pool.poolSizeCount = std::size(sizes);
  pool.pPoolSizes = sizes;
  if (const auto r = vkCreateDescriptorPool(device, &pool, nullptr, &descriptors); r != VK_SUCCESS) return r;
  auto pick = structure<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
  pick.descriptorPool = descriptors;
  pick.descriptorSetCount = 1;
  pick.pSetLayouts = &bindings;
  if (const auto r = vkAllocateDescriptorSets(device, &pick, &set); r != VK_SUCCESS) return r;
  const VkDescriptorImageInfo images[]{{sampler, planes[0], VK_IMAGE_LAYOUT_GENERAL},
                                        {sampler, planes[1], VK_IMAGE_LAYOUT_GENERAL}};
  const VkDescriptorBufferInfo out{output, 0, request.output_bytes};
  VkWriteDescriptorSet writes[3]{};
  for (uint32_t i = 0; i < std::size(writes); ++i) {
    auto& write = writes[i];
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = i;
    write.descriptorCount = 1;
    write.descriptorType = slots[i].descriptorType;
    if (i < 2) write.pImageInfo = &images[i];
    else write.pBufferInfo = &out;
  }
  vkUpdateDescriptorSets(device, std::size(writes), writes, 0, nullptr);
  return VK_SUCCESS;
}

} // namespace

VkResult run_capture_compute(VkDevice device, VkQueue queue, uint32_t family,
                             const VkPhysicalDeviceMemoryProperties& memory,
                             const std::array<VkImageView, 2>& planes,
                             const CaptureGpuComputeRequest& request, std::vector<uint8_t>& output) {
  DispatchResources job{device};
  if (const auto r = job.initialize_output(memory, request.output_bytes); r != VK_SUCCESS) return r;
  VkDescriptorSet set{};
  if (const auto r = job.initialize_program(request, planes, set); r != VK_SUCCESS) return r;

  auto pool = structure<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
  pool.queueFamilyIndex = family;
  pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  if (const auto r = vkCreateCommandPool(device, &pool, nullptr, &job.commands); r != VK_SUCCESS) return r;
  auto allocate = structure<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
  allocate.commandPool = job.commands;
  allocate.commandBufferCount = 1;
  VkCommandBuffer commands{};
  if (const auto r = vkAllocateCommandBuffers(device, &allocate, &commands); r != VK_SUCCESS) return r;
  auto begin = structure<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (const auto r = vkBeginCommandBuffer(commands, &begin); r != VK_SUCCESS) return r;
  vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, job.pipeline);
  vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, job.layout, 0, 1, &set, 0, nullptr);
  vkCmdPushConstants(commands, job.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     CaptureGpuComputeRequest::max_parameter_bytes, request.parameters.data());
  vkCmdDispatch(commands, request.workgroups[0], request.workgroups[1], request.workgroups[2]);
  auto read = structure<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
  read.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  read.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       0, 1, &read, 0, nullptr, 0, nullptr);
  if (const auto r = vkEndCommandBuffer(commands); r != VK_SUCCESS) return r;
  auto fence = structure<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
  if (const auto r = vkCreateFence(device, &fence, nullptr, &job.fence); r != VK_SUCCESS) return r;
  auto submit = structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &commands;
  if (const auto r = vkQueueSubmit(queue, 1, &submit, job.fence); r != VK_SUCCESS) return r;

  // The request owns both the result resources and input until GPU retirement.
  // This is the private provider worker. The Godot thread never waits here.
  const auto retired = vkWaitForFences(device, 1, &job.fence, VK_TRUE, UINT64_MAX);
  if (retired != VK_SUCCESS) {
    if (retired != VK_ERROR_DEVICE_LOST) vkDeviceWaitIdle(device);
    return retired;
  }
  const auto* bytes = static_cast<const uint8_t*>(job.mapped);
  output.assign(bytes, bytes + request.output_bytes);
  return VK_SUCCESS;
}

} // namespace cambang::camera2_detail

#include "camera2_gpu_device.h"
#include "camera2_capture_compute.h"
#include "camera2_compute_validation.h"

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace cambang::camera2_detail {
namespace {

#include "camera2_gpu_copy_spirv.inc"

template<typename T>
T vk_info(VkStructureType type) {
  T value{};
  value.sType = type;
  return value;
}

struct Plane {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;

  void release(VkDevice device) {
    vkDestroyImageView(device, view, nullptr);
    vkDestroyImage(device, image, nullptr);
    vkFreeMemory(device, memory, nullptr);
  }
};

bool has_device_extension(const std::vector<VkExtensionProperties>& extensions,
                          const char* name) {
  for (const auto& extension : extensions) {
    if (std::strcmp(extension.extensionName, name) == 0) return true;
  }
  return false;
}

constexpr const char* kDeviceExtensions[] = {
    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
};

} // namespace

struct Camera2GpuDevice::State {
  State(uint64_t limit, Camera2GpuDevice::Dispatch schedule)
      : dispatch(std::move(schedule)), budget(limit) {}

  ~State() {
    if (device) {
      vkDestroyShaderModule(device, split_shader, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
  }

  struct Backing final : CaptureGpuBacking, std::enable_shared_from_this<Backing> {
    Backing(std::shared_ptr<State> device_state, CaptureGpuBackingBudget::Reservation reservation)
        : CaptureGpuBacking(std::move(reservation)), state(std::move(device_state)) {}
    ~Backing() override {
      for (auto& plane : planes) plane.release(state->device);
    }

    CaptureGpuComputeStatus submit_compute(CaptureGpuComputeRequest request,
                                           CaptureGpuComputeCompletion completed) override;

    std::shared_ptr<State> state;
    std::array<Plane, 2> planes{};
  };

  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties properties{};
  VkPhysicalDeviceMemoryProperties memory{};
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkShaderModule split_shader = VK_NULL_HANDLE;
  PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_buffer_properties = nullptr;
  PFN_vkGetSemaphoreFdKHR export_fd = nullptr;
  bool device_lost = false;
  Camera2GpuDevice::Dispatch dispatch;
  CaptureGpuBackingBudget budget;

  bool initialize();
};

bool Camera2GpuDevice::State::initialize() {
  auto app = vk_info<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
  app.pApplicationName = "CamBANG Camera2";
  app.apiVersion = VK_API_VERSION_1_1;
  auto instance_info = vk_info<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
  instance_info.pApplicationInfo = &app;
  if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) return false;

  uint32_t count = 0;
  if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) return false;
  std::vector<VkPhysicalDevice> physical_devices(count);
  if (vkEnumeratePhysicalDevices(instance, &count, physical_devices.data()) != VK_SUCCESS) return false;

  for (auto physical : physical_devices) {
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_1) continue;
    uint32_t extension_count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, nullptr) != VK_SUCCESS ||
        extension_count == 0) continue;
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &extension_count, extensions.data()) != VK_SUCCESS ||
        !has_device_extension(extensions, kDeviceExtensions[0]) ||
        !has_device_extension(extensions, kDeviceExtensions[1]) ||
        !has_device_extension(extensions, kDeviceExtensions[2])) continue;

    auto external = vk_info<VkPhysicalDeviceExternalSemaphoreInfo>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO);
    external.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    auto semaphore_props = vk_info<VkExternalSemaphoreProperties>(
        VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES);
    vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &external, &semaphore_props);
    if (!(semaphore_props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) continue;

    auto ycbcr = vk_info<VkPhysicalDeviceSamplerYcbcrConversionFeatures>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES);
    auto features = vk_info<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    features.pNext = &ycbcr;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!ycbcr.samplerYcbcrConversion || !features.features.shaderStorageImageExtendedFormats ||
        !features.features.robustBufferAccess) continue;
    VkFormatProperties y{}, uv{};
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8_UNORM, &y);
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8_UNORM, &uv);
    constexpr auto required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((y.optimalTilingFeatures & required) != required ||
        (uv.optimalTilingFeatures & required) != required) continue;

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, families.data());
    for (family = 0; family < queue_count; ++family) {
      if (families[family].queueCount && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) break;
    }
    if (family == queue_count) continue;

    constexpr float priority = 0.0f;
    auto queue_info = vk_info<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderStorageImageExtendedFormats = VK_TRUE;
    enabled.robustBufferAccess = features.features.robustBufferAccess;
    auto device_info = vk_info<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    device_info.pNext = &ycbcr;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = std::size(kDeviceExtensions);
    device_info.ppEnabledExtensionNames = kDeviceExtensions;
    device_info.pEnabledFeatures = &enabled;
    if (vkCreateDevice(physical, &device_info, nullptr, &device) != VK_SUCCESS) continue;

    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    vkGetDeviceQueue(device, family, 0, &queue);
    get_buffer_properties = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        vkGetDeviceProcAddr(device, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    export_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
    auto shader = vk_info<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    shader.codeSize = sizeof(kPlatformGpuImportYcbcrSplitSpv);
    shader.pCode = kPlatformGpuImportYcbcrSplitSpv;
    return get_buffer_properties && export_fd &&
           vkCreateShaderModule(device, &shader, nullptr, &split_shader) == VK_SUCCESS;
  }
  return false;
}

CaptureGpuComputeStatus Camera2GpuDevice::State::Backing::submit_compute(
    CaptureGpuComputeRequest request, CaptureGpuComputeCompletion completed) {
  if (!state->dispatch) return CaptureGpuComputeStatus::Unavailable;
  return state->dispatch(
      [backing = shared_from_this(), request = std::move(request), completed = std::move(completed)](bool cancelled) {
        if (cancelled) {
          completed(CaptureGpuComputeStatus::Cancelled, {});
          return;
        }
        auto& gpu = *backing->state;
        if (gpu.device_lost) {
          completed(CaptureGpuComputeStatus::Unavailable, {});
          return;
        }
        if (!validate_capture_compute(request, gpu.properties.limits)) {
          completed(CaptureGpuComputeStatus::InvalidArgument, {});
          return;
        }
        std::vector<uint8_t> output;
        const auto result = run_capture_compute(gpu.device, gpu.queue, gpu.family, gpu.memory,
            {backing->planes[0].view, backing->planes[1].view}, request, output);
        gpu.device_lost |= result == VK_ERROR_DEVICE_LOST;
        completed(result == VK_SUCCESS ? CaptureGpuComputeStatus::Ok : CaptureGpuComputeStatus::Failed,
                  std::move(output));
      });
}

struct Camera2GpuCopy::State {
  using Device = Camera2GpuDevice::State;
  explicit State(std::shared_ptr<Device> gpu) : gpu(std::move(gpu)) {}
  ~State();

  std::shared_ptr<Device> gpu;
  std::shared_ptr<Device::Backing> backing;
  Camera2GpuSource source{};
  Plane imported{};
  VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout bindings = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool descriptors = VK_NULL_HANDLE;
  VkCommandPool commands = VK_NULL_HANDLE;
  VkFence finished = VK_NULL_HANDLE;
  VkSemaphore released = VK_NULL_HANDLE;
  bool submitted = false;
  bool reported = false;
  bool keep_source = false;

  void return_source(int fence_fd);
  bool import_source(AImage* image, uint32_t width, uint32_t height);
  CaptureGpuClaimStatus allocate_planes(uint32_t width, uint32_t height);
  bool submit_commands(uint32_t width, uint32_t height);
};

void Camera2GpuCopy::State::return_source(int fence_fd) {
  if (source.image) {
    AImage_deleteAsync(std::exchange(source.image, nullptr), fence_fd);
    source.reader.reset();
  }
}

Camera2GpuCopy::State::~State() {
  const auto device = gpu->device;
  if (submitted) {
    // Only the provider worker destroys submitted copies. The camera allocation
    // and all Vulkan objects remain alive until the queue stops using them.
    vkWaitForFences(device, 1, &finished, VK_TRUE, std::numeric_limits<uint64_t>::max());
  }
  return_source(-1);
  vkDestroySemaphore(device, released, nullptr);
  vkDestroyFence(device, finished, nullptr);
  vkDestroyCommandPool(device, commands, nullptr);
  vkDestroyDescriptorPool(device, descriptors, nullptr);
  vkDestroyPipeline(device, pipeline, nullptr);
  vkDestroyPipelineLayout(device, layout, nullptr);
  vkDestroyDescriptorSetLayout(device, bindings, nullptr);
  imported.release(device);
  vkDestroySampler(device, sampler, nullptr);
  vkDestroySamplerYcbcrConversion(device, conversion, nullptr);
}

bool Camera2GpuCopy::State::import_source(AImage* image, uint32_t width, uint32_t height) {
  AHardwareBuffer* buffer = nullptr;
  if (AImage_getHardwareBuffer(image, &buffer) != AMEDIA_OK || !buffer) return false;
  AHardwareBuffer_Desc description{};
  AHardwareBuffer_describe(buffer, &description);
  if (!(description.usage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE) ||
      description.width != width || description.height != height || description.layers != 1) return false;

  const auto device = gpu->device;
  auto format = vk_info<VkAndroidHardwareBufferFormatPropertiesANDROID>(
      VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID);
  auto properties = vk_info<VkAndroidHardwareBufferPropertiesANDROID>(
      VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID);
  properties.pNext = &format;
  if (gpu->get_buffer_properties(device, buffer, &properties) != VK_SUCCESS ||
      !format.externalFormat || !properties.memoryTypeBits ||
      !(format.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return false;

  auto external_format = vk_info<VkExternalFormatANDROID>(VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID);
  external_format.externalFormat = format.externalFormat;
  auto conversion_info = vk_info<VkSamplerYcbcrConversionCreateInfo>(
      VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO);
  conversion_info.pNext = &external_format;
  conversion_info.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
  conversion_info.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
  conversion_info.components = format.samplerYcbcrConversionComponents;
  conversion_info.xChromaOffset = format.suggestedXChromaOffset;
  conversion_info.yChromaOffset = format.suggestedYChromaOffset;
  conversion_info.chromaFilter = VK_FILTER_NEAREST;
  if (vkCreateSamplerYcbcrConversion(device, &conversion_info, nullptr, &conversion) != VK_SUCCESS) return false;

  auto conversion_binding = vk_info<VkSamplerYcbcrConversionInfo>(VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO);
  conversion_binding.conversion = conversion;
  auto sampler_info = vk_info<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
  sampler_info.pNext = &conversion_binding;
  sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) return false;

  auto external_memory = vk_info<VkExternalMemoryImageCreateInfo>(VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
  external_memory.pNext = &external_format;
  external_memory.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
  auto image_info = vk_info<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
  image_info.pNext = &external_memory;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.extent = {description.width, description.height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  if (vkCreateImage(device, &image_info, nullptr, &imported.image) != VK_SUCCESS) return false;

  auto dedicated = vk_info<VkMemoryDedicatedAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
  dedicated.image = imported.image;
  auto import = vk_info<VkImportAndroidHardwareBufferInfoANDROID>(
      VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID);
  import.pNext = &dedicated;
  import.buffer = buffer;
  auto allocation = vk_info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
  allocation.pNext = &import;
  allocation.allocationSize = properties.allocationSize;
  allocation.memoryTypeIndex = static_cast<uint32_t>(__builtin_ctz(properties.memoryTypeBits));
  if (vkAllocateMemory(device, &allocation, nullptr, &imported.memory) != VK_SUCCESS ||
      vkBindImageMemory(device, imported.image, imported.memory, 0) != VK_SUCCESS) return false;
  auto view = vk_info<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
  view.pNext = &conversion_binding;
  view.image = imported.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return vkCreateImageView(device, &view, nullptr, &imported.view) == VK_SUCCESS;
}

CaptureGpuClaimStatus Camera2GpuCopy::State::allocate_planes(uint32_t width, uint32_t height) {
  const auto device = gpu->device;
  std::array<Plane, 2> planes{};
  std::array<VkMemoryRequirements, 2> needs{};
  const auto release_planes = [&] {
    for (auto& plane : planes) plane.release(device);
  };
  VkDeviceSize bytes = 0;
  for (size_t i = 0; i < planes.size(); ++i) {
    auto info = vk_info<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent = i == 0 ? VkExtent3D{width, height, 1}
                         : VkExtent3D{(width + 1) / 2, (height + 1) / 2, 1};
    info.format = i == 0 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(device, &info, nullptr, &planes[i].image) != VK_SUCCESS) {
      release_planes();
      return CaptureGpuClaimStatus::Unavailable;
    }
    vkGetImageMemoryRequirements(device, planes[i].image, &needs[i]);
    bytes += needs[i].size;
  }

  auto reservation = gpu->budget.try_reserve(bytes);
  if (!reservation) {
    release_planes();
    return CaptureGpuClaimStatus::BudgetExhausted;
  }
  backing = std::make_shared<Device::Backing>(gpu, std::move(*reservation));
  backing->planes = planes;
  for (size_t i = 0; i < planes.size(); ++i) {
    auto& plane = backing->planes[i];
    auto allocation = vk_info<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocation.allocationSize = needs[i].size;
    for (; allocation.memoryTypeIndex < gpu->memory.memoryTypeCount; ++allocation.memoryTypeIndex) {
      const auto flags = gpu->memory.memoryTypes[allocation.memoryTypeIndex].propertyFlags;
      if ((needs[i].memoryTypeBits & (1u << allocation.memoryTypeIndex)) &&
          (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) break;
    }
    if (allocation.memoryTypeIndex == gpu->memory.memoryTypeCount ||
        vkAllocateMemory(device, &allocation, nullptr, &plane.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, plane.image, plane.memory, 0) != VK_SUCCESS) {
      return CaptureGpuClaimStatus::Unavailable;
    }
    auto view = vk_info<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    view.image = plane.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = i == 0 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &view, nullptr, &plane.view) != VK_SUCCESS) {
      return CaptureGpuClaimStatus::Unavailable;
    }
  }
  return CaptureGpuClaimStatus::Claimed;
}

bool Camera2GpuCopy::State::submit_commands(uint32_t width, uint32_t height) {
  const auto device = gpu->device;
  const VkDescriptorSetLayoutBinding binding[] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &sampler},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  };
  auto binding_info = vk_info<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
  binding_info.bindingCount = std::size(binding);
  binding_info.pBindings = binding;
  if (vkCreateDescriptorSetLayout(device, &binding_info, nullptr, &bindings) != VK_SUCCESS) return false;
  const VkPushConstantRange constants{VK_SHADER_STAGE_COMPUTE_BIT, 0, 3 * sizeof(uint32_t)};
  auto layout_info = vk_info<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &bindings;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &constants;
  if (vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) != VK_SUCCESS) return false;
  auto pipeline_info = vk_info<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
  pipeline_info.layout = layout;
  pipeline_info.stage = vk_info<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
  pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline_info.stage.module = gpu->split_shader;
  pipeline_info.stage.pName = "main";
  if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) return false;

  // Vulkan charges a multi-planar immutable sampler as multiple descriptors.
  const VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
  };
  auto descriptor_info = vk_info<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
  descriptor_info.maxSets = 1;
  descriptor_info.poolSizeCount = std::size(pool_sizes);
  descriptor_info.pPoolSizes = pool_sizes;
  if (vkCreateDescriptorPool(device, &descriptor_info, nullptr, &descriptors) != VK_SUCCESS) return false;
  auto descriptor_alloc = vk_info<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
  descriptor_alloc.descriptorPool = descriptors;
  descriptor_alloc.descriptorSetCount = 1;
  descriptor_alloc.pSetLayouts = &bindings;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(device, &descriptor_alloc, &set) != VK_SUCCESS) return false;
  const VkDescriptorImageInfo images[] = {
      {VK_NULL_HANDLE, imported.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, backing->planes[0].view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, backing->planes[1].view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet writes[3]{};
  for (uint32_t i = 0; i < std::size(writes); ++i) {
    auto& write = writes[i];
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = i;
    write.descriptorCount = 1;
    write.descriptorType = binding[i].descriptorType;
    write.pImageInfo = &images[i];
  }
  vkUpdateDescriptorSets(device, std::size(writes), writes, 0, nullptr);

  auto pool_info = vk_info<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = gpu->family;
  if (vkCreateCommandPool(device, &pool_info, nullptr, &commands) != VK_SUCCESS) return false;
  auto command_info = vk_info<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
  command_info.commandPool = commands;
  command_info.commandBufferCount = 1;
  VkCommandBuffer command = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device, &command_info, &command) != VK_SUCCESS) return false;
  auto begin = vk_info<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return false;
  VkImageMemoryBarrier barriers[3]{};
  for (size_t i = 0; i < std::size(barriers); ++i) {
    auto& barrier = barriers[i];
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = i ? backing->planes[i - 1].image : imported.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.oldLayout = i ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = i ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = i ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.dstQueueFamilyIndex = i ? VK_QUEUE_FAMILY_IGNORED : gpu->family;
    barrier.dstAccessMask = i ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
  }
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 0, nullptr, 0, nullptr, std::size(barriers), barriers);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
  const uint32_t dimensions[] = {width, height, 0}; // Interleaved Cb, Cr.
  vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dimensions), dimensions);
  vkCmdDispatch(command, (width + 7) / 8, (height + 7) / 8, 1);

  auto release = barriers[0];
  release.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  release.srcQueueFamilyIndex = gpu->family;
  release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
  release.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  release.dstAccessMask = 0;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &release);
  auto visible = vk_info<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
  visible.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  visible.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 1, &visible, 0, nullptr, 0, nullptr);
  if (vkEndCommandBuffer(command) != VK_SUCCESS) return false;

  auto export_info = vk_info<VkExportSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO);
  export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  auto semaphore_info = vk_info<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
  semaphore_info.pNext = &export_info;
  auto fence_info = vk_info<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
  if (vkCreateFence(device, &fence_info, nullptr, &finished) != VK_SUCCESS ||
      vkCreateSemaphore(device, &semaphore_info, nullptr, &released) != VK_SUCCESS) return false;
  auto submission = vk_info<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
  submission.commandBufferCount = 1;
  submission.pCommandBuffers = &command;
  submission.signalSemaphoreCount = 1;
  submission.pSignalSemaphores = &released;
  const auto result = vkQueueSubmit(gpu->queue, 1, &submission, finished);
  gpu->device_lost = result == VK_ERROR_DEVICE_LOST;
  submitted = result == VK_SUCCESS;
  return submitted;
}

Camera2GpuCopy::Camera2GpuCopy(std::unique_ptr<State> state) : state_(std::move(state)) {}
Camera2GpuCopy::~Camera2GpuCopy() = default;

std::shared_ptr<CaptureGpuBackingOffer> Camera2GpuCopy::poll() {
  if (state_->reported) return {};
  const auto result = vkGetFenceStatus(state_->gpu->device, state_->finished);
  if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) return {};
  state_->gpu->device_lost |= result == VK_ERROR_DEVICE_LOST;
  state_->submitted = false;
  state_->reported = true;
  if (!state_->keep_source || result == VK_ERROR_DEVICE_LOST) state_->return_source(-1);
  if (result != VK_SUCCESS) {
    return std::make_shared<CaptureGpuBackingOffer>(CaptureGpuClaimStatus::Unavailable);
  }
  return std::make_shared<CaptureGpuBackingOffer>(
      std::move(state_->backing), CaptureGpuBackingOffer::Clock::now() + std::chrono::seconds(5));
}

Camera2GpuSource Camera2GpuCopy::take_retired_source() {
  if (!state_->reported) return {};
  return Camera2GpuSource{std::move(state_->source.reader),
                          std::exchange(state_->source.image, nullptr)};
}

Camera2GpuDevice::Camera2GpuDevice(std::shared_ptr<State> state) : state_(std::move(state)) {}
Camera2GpuDevice::~Camera2GpuDevice() = default;

std::shared_ptr<Camera2GpuDevice> Camera2GpuDevice::open(uint64_t backing_budget, Dispatch dispatch) {
  auto state = std::make_shared<State>(backing_budget, std::move(dispatch));
  if (!state->initialize()) return {};
  return std::shared_ptr<Camera2GpuDevice>(new Camera2GpuDevice(std::move(state)));
}

Camera2GpuDevice::Submission Camera2GpuDevice::submit(
    Camera2GpuSource& source, uint32_t width, uint32_t height, bool keep_source) {
  const auto& limits = state_->properties.limits;
  int32_t image_width = 0, image_height = 0;
  AImageCropRect crop{};
  if (state_->device_lost || !source.image || !source.reader ||
      !width || !height || width > limits.maxImageDimension2D || height > limits.maxImageDimension2D ||
      (width + 7) / 8 > limits.maxComputeWorkGroupCount[0] ||
      (height + 7) / 8 > limits.maxComputeWorkGroupCount[1] ||
      AImage_getWidth(source.image, &image_width) != AMEDIA_OK ||
      AImage_getHeight(source.image, &image_height) != AMEDIA_OK ||
      AImage_getCropRect(source.image, &crop) != AMEDIA_OK || crop.left != 0 || crop.top != 0 ||
      crop.right != image_width || crop.bottom != image_height ||
      width != static_cast<uint32_t>(image_width) || height != static_cast<uint32_t>(image_height)) {
    return {};
  }
  auto copy = std::make_unique<Camera2GpuCopy::State>(state_);
  if (!copy->import_source(source.image, width, height)) return {};
  const auto status = copy->allocate_planes(width, height);
  if (status != CaptureGpuClaimStatus::Claimed) return {nullptr, status};
  // Construct the owning handle before submitting. Driver work can outlive a
  // failed export, and the handle always keeps the image valid in that case.
  auto job = std::unique_ptr<Camera2GpuCopy>(new Camera2GpuCopy(std::move(copy)));
  if (!job->state_->submit_commands(width, height)) return {};
  auto& running = *job->state_;
  running.source.reader = std::move(source.reader);
  running.source.image = std::exchange(source.image, nullptr);
  running.keep_source = keep_source;
  if (!keep_source) {
    auto fd_info = vk_info<VkSemaphoreGetFdInfoKHR>(VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR);
    fd_info.semaphore = running.released;
    fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    int fd = -1;
    if (state_->export_fd(state_->device, &fd_info, &fd) == VK_SUCCESS) running.return_source(fd);
  }
  return {std::move(job), CaptureGpuClaimStatus::Unavailable};
}


struct Camera2CaptureGpuService::State : std::enable_shared_from_this<Camera2CaptureGpuService::State> {
  // Includes work in flight. Only one image per reader may be held because
  // the Camera2 still reader has room for two acquired images.
  static constexpr size_t kCapacity = 4;
  static constexpr size_t kComputeCapacity = 8;
  static constexpr uint64_t kBudgetBytes = 64 * 1024 * 1024;
  using Clock = CaptureGpuBackingOffer::Clock;

  struct Job {
    Camera2GpuSource source{};
    uint32_t width = 0;
    uint32_t height = 0;
    SourceCompletion completed;
  };
  enum class Availability { Starting, Available, Unavailable };

  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Job> queue;
  std::deque<std::function<void(bool)>> compute_queue;
  size_t unfinished_compute = 0;
  std::set<const void*> using_readers;
  Availability availability = Availability::Starting;
  bool closing = false;
  std::vector<std::weak_ptr<CaptureGpuBackingOffer>> provisional;
  std::thread worker;

  void start() { worker = std::thread([this] { run(); }); }
  ~State() { stop(); }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      closing = true;
    }
    wake.notify_all();
    if (worker.joinable()) worker.join();
  }

  CaptureGpuComputeStatus queue_compute(std::function<void(bool)> job) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (closing || availability != Availability::Available) return CaptureGpuComputeStatus::Unavailable;
      if (unfinished_compute >= kComputeCapacity) return CaptureGpuComputeStatus::Busy;
      compute_queue.push_back(std::move(job));
      ++unfinished_compute;
    }
    wake.notify_one();
    return CaptureGpuComputeStatus::Ok;
  }

  void run();
  Offer copy(const std::shared_ptr<Camera2GpuDevice>& device, Job& job);
  void expire(Clock::time_point now) {
    std::erase_if(provisional, [now](const auto& weak_offer) {
      auto offer = weak_offer.lock();
      return !offer || offer->expire_if_due(now);
    });
  }
};

Camera2CaptureGpuService::Offer Camera2CaptureGpuService::State::copy(
    const std::shared_ptr<Camera2GpuDevice>& device, Job& job) {
  auto submission = device->submit(job.source, job.width, job.height, /*keep_source=*/true);
  if (!submission.copy) {
    return std::make_shared<CaptureGpuBackingOffer>(submission.failure);
  }
  // Keep retiring submitted operations even if the capture has timed out.
  // Reader and GPU resources remain pinned by the copy until its fence.
  for (;;) {
    if (auto offer = submission.copy->poll()) {
      job.source = submission.copy->take_retired_source();
      return offer;
    }
    expire(Clock::now());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void Camera2CaptureGpuService::State::run() {
  const auto device = Camera2GpuDevice::open(kBudgetBytes,
      [service = weak_from_this()](std::function<void(bool)> work) {
        const auto live = service.lock();
        return live ? live->queue_compute(std::move(work)) : CaptureGpuComputeStatus::Unavailable;
      });
  {
    std::lock_guard<std::mutex> lock(mutex);
    availability = device ? Availability::Available : Availability::Unavailable;
  }
  wake.notify_all();

  for (;;) {
    expire(Clock::now());
    Job job;
    std::function<void(bool)> compute;
    bool cancel = false;
    {
      std::unique_lock<std::mutex> lock(mutex);
      const auto ready = [&] { return closing || !queue.empty() || !compute_queue.empty(); };
      if (!ready()) {
        if (provisional.empty()) wake.wait(lock, ready);
        else wake.wait_for(lock, std::chrono::milliseconds(100), ready);
      }
      if (!queue.empty()) {
        job = std::move(queue.front());
        queue.pop_front();
      } else if (!compute_queue.empty()) {
        compute = std::move(compute_queue.front());
        compute_queue.pop_front();
      } else if (closing) {
        break;
      } else {
        continue;
      }
      cancel = closing;
    }
    if (compute) {
      compute(cancel);
      std::lock_guard<std::mutex> lock(mutex);
      --unfinished_compute;
      continue;
    }
    auto reader = job.source.reader;
    Offer offer;
    if (!cancel) {
      offer = copy(device, job);
      provisional.emplace_back(offer);
    }
    // The copy has relinquished Vulkan ownership. AImage keeps any CPU plane
    // lock until deletion, so reading CPU planes before submission would
    // overlap external ownership even if the memcpy had already returned.
    OwnedImage image(
        std::exchange(job.source.image, nullptr), &AImage_delete);
    if (cancel) image.reset();
    job.completed(std::move(image), std::move(offer));
    {
      std::lock_guard<std::mutex> lock(mutex);
      using_readers.erase(reader.get());
    }
  }
  expire(Clock::time_point::max());
}

Camera2CaptureGpuService::Camera2CaptureGpuService() : state_(std::make_shared<State>()) {
  state_->start();
}
Camera2CaptureGpuService::~Camera2CaptureGpuService() { stop(); }

void Camera2CaptureGpuService::stop() { state_->stop(); }

bool Camera2CaptureGpuService::available_for_capture() {
  std::unique_lock<std::mutex> lock(state_->mutex);
  state_->wake.wait_for(lock, std::chrono::milliseconds(100), [&] {
    return state_->availability != State::Availability::Starting;
  });
  return state_->availability == State::Availability::Available;
}

bool Camera2CaptureGpuService::enqueue(
    Camera2GpuSource& source, uint32_t width, uint32_t height, SourceCompletion completed) {
  if (!completed || !source.image || !source.reader) return false;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closing || state_->availability != State::Availability::Available ||
        state_->using_readers.size() >= State::kCapacity ||
        state_->using_readers.contains(source.reader.get())) {
      return false;
    }
    State::Job& job = state_->queue.emplace_back();
    state_->using_readers.insert(source.reader.get());
    job.source.reader = std::move(source.reader);
    job.source.image = std::exchange(source.image, nullptr);
    job.width = width;
    job.height = height;
    job.completed = std::move(completed);
    state_->wake.notify_one();
    return true;
  }
}

} // namespace cambang::camera2_detail

#include "godot/platform_gpu_import_android.h"

#if defined(__ANDROID__)

#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/xr_interface.hpp>
#include <godot_cpp/classes/xr_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "godot/gpu_compute_sidecar.h"
#include "imaging/api/platform_gpu_import_ops.h"

namespace cambang {

#include "godot/platform_gpu_import_android_spirv.inc"

// Render-thread callback target. A plain Object, never deleted: a callback the
// RenderingServer has queued may still run after uninstall, and finds the
// importer closed rather than a freed helper.
class PlatformGpuImportAndroidHelper : public godot::Object {
  GDCLASS(PlatformGpuImportAndroidHelper, godot::Object);

public:
  void rt_create_device();
  void rt_import(int64_t job_id);
  void rt_replenish(int64_t width, int64_t height);

  static void _bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("rt_create_device"),
                                &PlatformGpuImportAndroidHelper::rt_create_device);
    godot::ClassDB::bind_method(godot::D_METHOD("rt_import", "job_id"),
                                &PlatformGpuImportAndroidHelper::rt_import);
    godot::ClassDB::bind_method(godot::D_METHOD("rt_replenish", "width", "height"),
                                &PlatformGpuImportAndroidHelper::rt_replenish);
  }
};

namespace {

using Clock = std::chrono::steady_clock;

std::string fmt(const char* f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof(buf), f, ap);
  va_end(ap);
  return buf;
}

// Pre-cleared RD destinations must be at least this many drawn frames old, so
// RD has submitted the clear that leaves its tracker at TRANSFER_DST before
// our copy (submitted later on the same queue) relies on that layout.
constexpr int64_t kPoolFrameMargin = 2;
// Pre-cleared destination pairs kept per extent (stereo still capture).
constexpr size_t kPoolTargetPerExtent = 2;

struct Fns {
  PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
  PFN_vkCreateImage vkCreateImage = nullptr;
  PFN_vkDestroyImage vkDestroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
  PFN_vkAllocateMemory vkAllocateMemory = nullptr;
  PFN_vkFreeMemory vkFreeMemory = nullptr;
  PFN_vkBindImageMemory vkBindImageMemory = nullptr;
  PFN_vkCreateImageView vkCreateImageView = nullptr;
  PFN_vkDestroyImageView vkDestroyImageView = nullptr;
  PFN_vkGetMemoryFdKHR vkGetMemoryFdKHR = nullptr;
  PFN_vkGetAndroidHardwareBufferPropertiesANDROID vkGetAndroidHardwareBufferPropertiesANDROID = nullptr;
  PFN_vkCreateSamplerYcbcrConversion vkCreateSamplerYcbcrConversion = nullptr;
  PFN_vkDestroySamplerYcbcrConversion vkDestroySamplerYcbcrConversion = nullptr;
  PFN_vkCreateSampler vkCreateSampler = nullptr;
  PFN_vkDestroySampler vkDestroySampler = nullptr;
  PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
  PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
  PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
  PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
  PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
  PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
  PFN_vkCreateComputePipelines vkCreateComputePipelines = nullptr;
  PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;
  PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
  PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
  PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
  PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;
  PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
  PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
  PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
  PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
  PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
  PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
  PFN_vkCmdPushConstants vkCmdPushConstants = nullptr;
  PFN_vkCmdDispatch vkCmdDispatch = nullptr;
  PFN_vkCmdCopyImage vkCmdCopyImage = nullptr;
  PFN_vkCreateFence vkCreateFence = nullptr;
  PFN_vkDestroyFence vkDestroyFence = nullptr;
  PFN_vkWaitForFences vkWaitForFences = nullptr;
  PFN_vkQueueSubmit vkQueueSubmit = nullptr;
  PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
  PFN_vkDestroyDevice vkDestroyDevice = nullptr;
};

std::string load_fns(PFN_vkGetDeviceProcAddr gdpa, VkDevice d, Fns& f, bool ours) {
  std::string missing;
#define PGI_LOAD(n) do { f.n = reinterpret_cast<PFN_##n>(gdpa(d, #n)); if (!f.n) missing += std::string(missing.empty() ? "" : ",") + #n; } while (0)
  PGI_LOAD(vkCreateImage); PGI_LOAD(vkDestroyImage); PGI_LOAD(vkGetImageMemoryRequirements);
  PGI_LOAD(vkAllocateMemory); PGI_LOAD(vkFreeMemory); PGI_LOAD(vkBindImageMemory);
  PGI_LOAD(vkCreateCommandPool); PGI_LOAD(vkDestroyCommandPool); PGI_LOAD(vkAllocateCommandBuffers);
  PGI_LOAD(vkFreeCommandBuffers); PGI_LOAD(vkBeginCommandBuffer); PGI_LOAD(vkEndCommandBuffer);
  PGI_LOAD(vkCmdPipelineBarrier); PGI_LOAD(vkCmdCopyImage); PGI_LOAD(vkCreateFence); PGI_LOAD(vkDestroyFence);
  PGI_LOAD(vkWaitForFences); PGI_LOAD(vkQueueSubmit);
  if (ours) {
    PGI_LOAD(vkGetDeviceQueue); PGI_LOAD(vkCreateImageView); PGI_LOAD(vkDestroyImageView);
    PGI_LOAD(vkGetMemoryFdKHR); PGI_LOAD(vkGetAndroidHardwareBufferPropertiesANDROID);
    PGI_LOAD(vkCreateSampler); PGI_LOAD(vkDestroySampler); PGI_LOAD(vkCreateShaderModule);
    PGI_LOAD(vkDestroyShaderModule); PGI_LOAD(vkCreateDescriptorSetLayout); PGI_LOAD(vkDestroyDescriptorSetLayout);
    PGI_LOAD(vkCreatePipelineLayout); PGI_LOAD(vkDestroyPipelineLayout); PGI_LOAD(vkCreateComputePipelines);
    PGI_LOAD(vkDestroyPipeline); PGI_LOAD(vkCreateDescriptorPool); PGI_LOAD(vkDestroyDescriptorPool);
    PGI_LOAD(vkAllocateDescriptorSets); PGI_LOAD(vkUpdateDescriptorSets); PGI_LOAD(vkCmdBindPipeline);
    PGI_LOAD(vkCmdBindDescriptorSets); PGI_LOAD(vkCmdPushConstants); PGI_LOAD(vkCmdDispatch);
    PGI_LOAD(vkDeviceWaitIdle); PGI_LOAD(vkDestroyDevice);
    f.vkCreateSamplerYcbcrConversion = reinterpret_cast<PFN_vkCreateSamplerYcbcrConversion>(gdpa(d, "vkCreateSamplerYcbcrConversion"));
    if (!f.vkCreateSamplerYcbcrConversion)
      f.vkCreateSamplerYcbcrConversion = reinterpret_cast<PFN_vkCreateSamplerYcbcrConversion>(gdpa(d, "vkCreateSamplerYcbcrConversionKHR"));
    f.vkDestroySamplerYcbcrConversion = reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(gdpa(d, "vkDestroySamplerYcbcrConversion"));
    if (!f.vkDestroySamplerYcbcrConversion)
      f.vkDestroySamplerYcbcrConversion = reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(gdpa(d, "vkDestroySamplerYcbcrConversionKHR"));
    if (!f.vkCreateSamplerYcbcrConversion || !f.vkDestroySamplerYcbcrConversion)
      missing += std::string(missing.empty() ? "" : ",") + "vkCreate/DestroySamplerYcbcrConversion";
  }
#undef PGI_LOAD
  return missing;
}

// One YCbCr conversion + immutable sampler + pipeline per AHB external format.
struct ConvPipe {
  uint64_t external_format = 0;
  VkSamplerYcbcrConversion conv = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  VkPipelineLayout pl = VK_NULL_HANDLE;
  VkPipeline pipe = VK_NULL_HANDLE;
};

// A pre-cleared pair of RD-owned destinations. The sidecar already owns the
// RIDs (realised through the synthetic GPU backing bridge's release drain), so
// a pair that is never used releases itself when the pool drops it.
struct PoolEntry {
  std::shared_ptr<void> sidecar;
  VkImage dst[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  int64_t cleared_frame = 0;
};

enum class GodotSide { Pending, Never, Queued, Running, Submitted, Done };

struct Job {
  uint64_t id = 0;
  PlatformGpuCopyRequest req{};
  uint32_t w = 0, h = 0;
  // Our device.
  VkImage src_img = VK_NULL_HANDLE;
  VkDeviceMemory src_mem = VK_NULL_HANDLE;
  VkImageView src_view = VK_NULL_HANDLE;
  VkImage out_img[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDeviceMemory out_mem[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkImageView out_view[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDeviceSize out_size[2] = {0, 0};
  uint32_t out_w[2] = {0, 0}, out_h[2] = {0, 0};
  VkFormat out_fmt[2] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM};
  uint32_t out_mtype = 0;
  VkDescriptorPool dp = VK_NULL_HANDLE;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  int fds[2] = {-1, -1};
  // Godot's device.
  VkImage g_img[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDeviceMemory g_mem[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkCommandBuffer gcb = VK_NULL_HANDLE;
  VkFence gfence = VK_NULL_HANDLE;
  std::shared_ptr<void> sidecar;
  // State, under m.
  std::mutex m;
  std::condition_variable cv;
  bool our_done = false;      // our fence signalled, source released
  bool outputs_destroyed = false;
  GodotSide gside = GodotSide::Pending;
};

// What the provider holds. Dropping it without finish_copy decides that the
// Godot side never runs; the source is released by the worker regardless.
struct JobHandle {
  std::shared_ptr<Job> job;
  ~JobHandle();
};

struct State {
  // Leaked with the State (never destroyed): nothing here may be destroyed while
  // a camera, render or worker thread could still touch it.
  std::mutex m;
  std::atomic<bool> installed{false};
  std::atomic<bool> xr_gate{false};
  std::atomic<int> quest{-1};  // -1 unknown, 0 no, 1 yes
  std::atomic<int> device_state{0};  // 0 none, 1 creating, 2 ready, 3 failed
  PlatformGpuImportAndroidHelper* helper = nullptr;

  // Godot's handles (read on the render thread).
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice gdev = VK_NULL_HANDLE;
  VkQueue gqueue = VK_NULL_HANDLE;
  uint32_t gqfam = 0;
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  PFN_vkGetDeviceProcAddr gdpa = nullptr;
  Fns g;
  VkCommandPool gpool = VK_NULL_HANDLE;  // render thread only
  std::vector<VkCommandBuffer> gcb_to_free;  // finished; freed on the render thread (under m)

  // Our device.
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue q = VK_NULL_HANDLE;
  uint32_t qfam = 0, qidx = 0;
  Fns o;
  VkShaderModule sm = VK_NULL_HANDLE;
  std::mutex submit_m;  // our command pool + queue
  VkCommandPool pool = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties mem_props{};

  // Pipelines by external format (worker creates, camera threads read).
  std::map<uint64_t, ConvPipe> pipes;
  std::set<uint64_t> pipes_pending;

  // Pre-cleared RD destinations by extent (render thread fills and takes).
  std::map<std::pair<uint32_t, uint32_t>, std::deque<PoolEntry>> pool_entries;
  std::set<std::pair<uint32_t, uint32_t>> replenish_pending;

  // Jobs by id (render-thread callbacks look them up).
  std::map<uint64_t, std::shared_ptr<Job>> jobs;
  uint64_t next_job_id = 1;

  // Worker.
  std::thread worker;
  std::mutex wm;
  std::condition_variable wcv;
  std::deque<std::function<void()>> tasks;
  bool worker_stop = false;

  // Log once per cause.
  std::mutex log_m;
  std::set<std::string> logged;
};

State& st() {
  static State* const s = new State();  // never destroyed (see State)
  return *s;
}

void log_once(const std::string& cause, const std::string& detail) {
  State& s = st();
  {
    std::lock_guard<std::mutex> l(s.log_m);
    if (!s.logged.insert(cause).second) return;
  }
  godot::UtilityFunctions::print(godot::String("[CamBANG][PlatformGpuImport] ") + cause.c_str() + ": " + detail.c_str());
}

void post_task(std::function<void()> t) {
  State& s = st();
  {
    std::lock_guard<std::mutex> l(s.wm);
    s.tasks.push_back(std::move(t));
  }
  s.wcv.notify_one();
}

void worker_main() {
  State& s = st();
  for (;;) {
    std::function<void()> t;
    {
      std::unique_lock<std::mutex> l(s.wm);
      s.wcv.wait(l, [&] { return !s.tasks.empty() || s.worker_stop; });
      if (s.tasks.empty()) return;  // stop requested and drained
      t = std::move(s.tasks.front());
      s.tasks.pop_front();
    }
    t();
  }
}

int find_memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
  const auto& mp = st().mem_props;
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return static_cast<int>(i);
  return -1;
}

// Destroys a job's remaining objects on our device once our fence has signalled
// and the Godot side has either finished reading the outputs or will never run.
// Worker thread only.
void reap(const std::shared_ptr<Job>& job) {
  State& s = st();
  {
    std::lock_guard<std::mutex> l(job->m);
    if (job->outputs_destroyed || !job->our_done) return;
    if (job->gside != GodotSide::Never && job->gside != GodotSide::Done) return;
    job->outputs_destroyed = true;
  }
  for (int k = 0; k < 2; ++k) {
    if (job->fds[k] >= 0) { close(job->fds[k]); job->fds[k] = -1; }
    if (job->out_view[k]) s.o.vkDestroyImageView(s.dev, job->out_view[k], nullptr);
    if (job->out_img[k]) s.o.vkDestroyImage(s.dev, job->out_img[k], nullptr);
    if (job->out_mem[k]) s.o.vkFreeMemory(s.dev, job->out_mem[k], nullptr);
    job->out_view[k] = VK_NULL_HANDLE; job->out_img[k] = VK_NULL_HANDLE; job->out_mem[k] = VK_NULL_HANDLE;
  }
  if (job->dp) { s.o.vkDestroyDescriptorPool(s.dev, job->dp, nullptr); job->dp = VK_NULL_HANDLE; }
  {
    std::lock_guard<std::mutex> l(s.submit_m);
    if (job->cb) { s.o.vkFreeCommandBuffers(s.dev, s.pool, 1, &job->cb); job->cb = VK_NULL_HANDLE; }
  }
  if (job->fence) { s.o.vkDestroyFence(s.dev, job->fence, nullptr); job->fence = VK_NULL_HANDLE; }
  std::lock_guard<std::mutex> l(s.m);
  s.jobs.erase(job->id);
}

// Decides that the Godot side will never run (if still undecided or merely
// queued) and lets the worker reap. Returns true when it made that decision.
bool abandon_godot_side(const std::shared_ptr<Job>& job) {
  bool decided = false;
  {
    std::lock_guard<std::mutex> l(job->m);
    if (job->gside == GodotSide::Pending || job->gside == GodotSide::Queued) {
      job->gside = GodotSide::Never;
      decided = true;
    }
  }
  job->cv.notify_all();
  if (decided) post_task([job] { reap(job); });
  return decided;
}

JobHandle::~JobHandle() {
  if (job) abandon_godot_side(job);
}

// Worker: wait for our copy to execute, then release the AHB import and the
// camera's buffer. Never depends on Godot's render or main thread.
void wait_ours_and_release_source(const std::shared_ptr<Job>& job) {
  State& s = st();
  for (;;) {
    const VkResult r = s.o.vkWaitForFences(s.dev, 1, &job->fence, VK_TRUE, 100000000ull);
    if (r == VK_SUCCESS) break;
    if (r != VK_TIMEOUT) {
      log_once(fmt("our fence wait failed (%d)", r), "the device is lost; the source is released without the copy");
      break;
    }
  }
  if (job->src_view) s.o.vkDestroyImageView(s.dev, job->src_view, nullptr);
  if (job->src_img) s.o.vkDestroyImage(s.dev, job->src_img, nullptr);
  if (job->src_mem) s.o.vkFreeMemory(s.dev, job->src_mem, nullptr);
  job->src_view = VK_NULL_HANDLE; job->src_img = VK_NULL_HANDLE; job->src_mem = VK_NULL_HANDLE;
  if (job->req.on_source_released) job->req.on_source_released(job->req.user);
  {
    std::lock_guard<std::mutex> l(job->m);
    job->our_done = true;
  }
  job->cv.notify_all();
  reap(job);
}

// Worker: wait for Godot's copy, then destroy the imports on Godot's device.
void wait_godot_and_release_imports(const std::shared_ptr<Job>& job) {
  State& s = st();
  for (;;) {
    const VkResult r = s.g.vkWaitForFences(s.gdev, 1, &job->gfence, VK_TRUE, 100000000ull);
    if (r == VK_SUCCESS) break;
    if (r != VK_TIMEOUT) {
      log_once(fmt("Godot-side fence wait failed (%d)", r), "imports destroyed without confirmation");
      break;
    }
  }
  for (int k = 0; k < 2; ++k) {
    if (job->g_img[k]) s.g.vkDestroyImage(s.gdev, job->g_img[k], nullptr);
    if (job->g_mem[k]) s.g.vkFreeMemory(s.gdev, job->g_mem[k], nullptr);
    job->g_img[k] = VK_NULL_HANDLE; job->g_mem[k] = VK_NULL_HANDLE;
  }
  if (job->gfence) { s.g.vkDestroyFence(s.gdev, job->gfence, nullptr); job->gfence = VK_NULL_HANDLE; }
  // The command buffer's pool is the render thread's: hand it back to be freed there.
  if (job->gcb) {
    std::lock_guard<std::mutex> l(s.m);
    s.gcb_to_free.push_back(job->gcb);
    job->gcb = VK_NULL_HANDLE;
  }
  {
    std::lock_guard<std::mutex> l(job->m);
    job->gside = GodotSide::Done;
  }
  job->cv.notify_all();
  reap(job);
}

// Worker: create the YCbCr conversion, sampler and pipeline for an external
// format (G6 measured 14 ms; never on Godot's main/render thread).
void create_pipe(uint64_t external_format, VkAndroidHardwareBufferFormatPropertiesANDROID fp) {
  State& s = st();
  ConvPipe p{};
  p.external_format = external_format;
  std::string err;
  do {
    VkExternalFormatANDROID xf{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
    xf.externalFormat = external_format;
    VkSamplerYcbcrConversionCreateInfo yci{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
    yci.pNext = &xf;
    yci.format = VK_FORMAT_UNDEFINED;
    yci.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
    yci.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    yci.components = fp.samplerYcbcrConversionComponents;
    yci.xChromaOffset = fp.suggestedXChromaOffset;
    yci.yChromaOffset = fp.suggestedYChromaOffset;
    yci.chromaFilter = VK_FILTER_NEAREST;
    VkResult r = s.o.vkCreateSamplerYcbcrConversion(s.dev, &yci, nullptr, &p.conv);
    if (r != VK_SUCCESS) { err = fmt("vkCreateSamplerYcbcrConversion=%d", r); break; }
    VkSamplerYcbcrConversionInfo yinfo{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    yinfo.conversion = p.conv;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.pNext = &yinfo;
    sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f; sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    r = s.o.vkCreateSampler(s.dev, &sci, nullptr, &p.sampler);
    if (r != VK_SUCCESS) { err = fmt("vkCreateSampler(ycbcr)=%d", r); break; }
    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; b[0].pImmutableSamplers = &p.sampler;
    for (int k = 1; k < 3; ++k) {
      b[k].binding = static_cast<uint32_t>(k); b[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      b[k].descriptorCount = 1; b[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 3; dli.pBindings = b;
    r = s.o.vkCreateDescriptorSetLayout(s.dev, &dli, nullptr, &p.dsl);
    if (r != VK_SUCCESS) { err = fmt("vkCreateDescriptorSetLayout=%d", r); break; }
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &p.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    r = s.o.vkCreatePipelineLayout(s.dev, &pli, nullptr, &p.pl);
    if (r != VK_SUCCESS) { err = fmt("vkCreatePipelineLayout=%d", r); break; }
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpi.stage.module = s.sm; cpi.stage.pName = "main";
    cpi.layout = p.pl;
    r = s.o.vkCreateComputePipelines(s.dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &p.pipe);
    if (r != VK_SUCCESS) { err = fmt("vkCreateComputePipelines=%d", r); break; }
  } while (false);
  std::lock_guard<std::mutex> l(s.m);
  s.pipes_pending.erase(external_format);
  if (!err.empty()) {
    if (p.pipe) s.o.vkDestroyPipeline(s.dev, p.pipe, nullptr);
    if (p.pl) s.o.vkDestroyPipelineLayout(s.dev, p.pl, nullptr);
    if (p.dsl) s.o.vkDestroyDescriptorSetLayout(s.dev, p.dsl, nullptr);
    if (p.sampler) s.o.vkDestroySampler(s.dev, p.sampler, nullptr);
    if (p.conv) s.o.vkDestroySamplerYcbcrConversion(s.dev, p.conv, nullptr);
    log_once(fmt("pipeline for external format 0x%llx failed", static_cast<unsigned long long>(external_format)),
             err + "; stills stay CPU_UPLOAD");
    // Not retried: a later still re-queues only if no entry is pending; keep a
    // tombstone so the failure does not repeat per still.
    ConvPipe dead{};
    dead.external_format = external_format;
    s.pipes[external_format] = dead;
    return;
  }
  s.pipes[external_format] = p;
}

// ---------------------------------------------------------------------------
// Seam table.
// ---------------------------------------------------------------------------

bool can_copy_native_buffers() noexcept {
  State& s = st();
  return s.installed.load(std::memory_order_acquire) && s.device_state.load(std::memory_order_acquire) == 2 &&
         s.xr_gate.load(std::memory_order_acquire) && s.quest.load(std::memory_order_acquire) == 1;
}

std::shared_ptr<void> begin_copy(const PlatformGpuCopyRequest& req) noexcept {
  State& s = st();
  if (!can_copy_native_buffers() || !req.native_buffer || req.width < 2 || req.height < 2 || !req.on_source_released) return {};
  auto* ahb = static_cast<AHardwareBuffer*>(req.native_buffer);
  AHardwareBuffer_Desc ad{};
  AHardwareBuffer_describe(ahb, &ad);
  if (ad.width != req.width || ad.height != req.height) {
    log_once("buffer extent mismatch", fmt("AHB %ux%u vs member %ux%u", ad.width, ad.height, req.width, req.height));
    return {};
  }
  VkAndroidHardwareBufferFormatPropertiesANDROID fp{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
  VkAndroidHardwareBufferPropertiesANDROID pp{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
  pp.pNext = &fp;
  VkResult r = s.o.vkGetAndroidHardwareBufferPropertiesANDROID(s.dev, ahb, &pp);
  if (r != VK_SUCCESS || fp.externalFormat == 0) {
    log_once(fmt("AHB properties unavailable (%d, external format 0x%llx)", r, static_cast<unsigned long long>(fp.externalFormat)),
             "stills stay CPU_UPLOAD");
    return {};
  }
  ConvPipe pipe{};
  bool queue_pipe = false;
  bool queue_replenish = false;
  const auto extent = std::make_pair(req.width, req.height);
  {
    std::lock_guard<std::mutex> l(s.m);
    auto it = s.pipes.find(fp.externalFormat);
    if (it != s.pipes.end()) pipe = it->second;
    else if (s.pipes_pending.insert(fp.externalFormat).second) queue_pipe = true;
    if (s.pool_entries[extent].size() < kPoolTargetPerExtent && s.replenish_pending.insert(extent).second) queue_replenish = true;
  }
  if (queue_replenish && s.helper) {
    godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
    if (rs) rs->call_on_render_thread(godot::Callable(s.helper, godot::StringName("rt_replenish")).bind(static_cast<int64_t>(req.width), static_cast<int64_t>(req.height)));
  }
  if (queue_pipe) {
    const uint64_t xf = fp.externalFormat;
    post_task([xf, fp] { create_pipe(xf, fp); });
    log_once(fmt("first still for external format 0x%llx", static_cast<unsigned long long>(xf)),
             "its pipeline is created on the importer's worker; this still stays CPU_UPLOAD");
    return {};
  }
  if (!pipe.pipe) return {};  // being created, or failed (logged once)

  auto job = std::make_shared<Job>();
  job->req = req;
  job->w = req.width; job->h = req.height;
  job->out_w[0] = req.width; job->out_h[0] = req.height;
  job->out_w[1] = req.width / 2; job->out_h[1] = req.height / 2;
  std::string err;
  bool submitted = false;
  do {
    // Import the AHB with its external format.
    VkExternalFormatANDROID xf{VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID};
    xf.externalFormat = fp.externalFormat;
    VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emi.pNext = &xf;
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &emi;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_UNDEFINED; ici.extent = {ad.width, ad.height, 1};
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT; ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = s.o.vkCreateImage(s.dev, &ici, nullptr, &job->src_img);
    if (r != VK_SUCCESS) { err = fmt("vkCreateImage(AHB)=%d", r); break; }
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = job->src_img;
    VkImportAndroidHardwareBufferInfoANDROID imp{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    imp.pNext = &ded;
    imp.buffer = ahb;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &imp;
    mai.allocationSize = pp.allocationSize;
    mai.memoryTypeIndex = static_cast<uint32_t>(__builtin_ctz(pp.memoryTypeBits));
    r = s.o.vkAllocateMemory(s.dev, &mai, nullptr, &job->src_mem);
    if (r != VK_SUCCESS) { err = fmt("vkAllocateMemory(import AHB)=%d", r); break; }
    r = s.o.vkBindImageMemory(s.dev, job->src_img, job->src_mem, 0);
    if (r != VK_SUCCESS) { err = fmt("vkBindImageMemory(AHB)=%d", r); break; }
    VkSamplerYcbcrConversionInfo yinfo{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    yinfo.conversion = pipe.conv;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.pNext = &yinfo;
    vci.image = job->src_img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_UNDEFINED;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    r = s.o.vkCreateImageView(s.dev, &vci, nullptr, &job->src_view);
    if (r != VK_SUCCESS) { err = fmt("vkCreateImageView(ycbcr)=%d", r); break; }

    // Exportable outputs (OPAQUE_FD): R8 luma, RG8 chroma.
    for (int k = 0; k < 2 && err.empty(); ++k) {
      VkExternalMemoryImageCreateInfo oemi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
      oemi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      VkImageCreateInfo oci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      oci.pNext = &oemi;
      oci.imageType = VK_IMAGE_TYPE_2D; oci.format = job->out_fmt[k]; oci.extent = {job->out_w[k], job->out_h[k], 1};
      oci.mipLevels = 1; oci.arrayLayers = 1; oci.samples = VK_SAMPLE_COUNT_1_BIT; oci.tiling = VK_IMAGE_TILING_OPTIMAL;
      oci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      oci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; oci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      r = s.o.vkCreateImage(s.dev, &oci, nullptr, &job->out_img[k]);
      if (r != VK_SUCCESS) { err = fmt("vkCreateImage(output %d)=%d", k, r); break; }
      VkMemoryRequirements mr{};
      s.o.vkGetImageMemoryRequirements(s.dev, job->out_img[k], &mr);
      const int mt = find_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (mt < 0) { err = "no device-local memory type"; break; }
      VkMemoryDedicatedAllocateInfo oded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
      oded.image = job->out_img[k];
      VkExportMemoryAllocateInfo exa{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
      exa.pNext = &oded;
      exa.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      VkMemoryAllocateInfo omai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      omai.pNext = &exa; omai.allocationSize = mr.size; omai.memoryTypeIndex = static_cast<uint32_t>(mt);
      r = s.o.vkAllocateMemory(s.dev, &omai, nullptr, &job->out_mem[k]);
      if (r != VK_SUCCESS) { err = fmt("vkAllocateMemory(export %d)=%d", k, r); break; }
      r = s.o.vkBindImageMemory(s.dev, job->out_img[k], job->out_mem[k], 0);
      if (r != VK_SUCCESS) { err = fmt("vkBindImageMemory(output %d)=%d", k, r); break; }
      job->out_size[k] = mr.size;
      job->out_mtype = static_cast<uint32_t>(mt);
      VkImageViewCreateInfo ovci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      ovci.image = job->out_img[k]; ovci.viewType = VK_IMAGE_VIEW_TYPE_2D; ovci.format = job->out_fmt[k];
      ovci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      r = s.o.vkCreateImageView(s.dev, &ovci, nullptr, &job->out_view[k]);
      if (r != VK_SUCCESS) { err = fmt("vkCreateImageView(output %d)=%d", k, r); break; }
    }
    if (!err.empty()) break;

    // Descriptors.
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    r = s.o.vkCreateDescriptorPool(s.dev, &dpi, nullptr, &job->dp);
    if (r != VK_SUCCESS) { err = fmt("vkCreateDescriptorPool=%d", r); break; }
    VkDescriptorSet ds = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = job->dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &pipe.dsl;
    r = s.o.vkAllocateDescriptorSets(s.dev, &dai, &ds);
    if (r != VK_SUCCESS) { err = fmt("vkAllocateDescriptorSets=%d", r); break; }
    VkDescriptorImageInfo dii[3]{};
    dii[0].imageView = job->src_view; dii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dii[1].imageView = job->out_view[0]; dii[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    dii[2].imageView = job->out_view[1]; dii[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet wds[3]{};
    for (int k = 0; k < 3; ++k) {
      wds[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wds[k].dstSet = ds; wds[k].dstBinding = static_cast<uint32_t>(k);
      wds[k].descriptorCount = 1;
      wds[k].descriptorType = k == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      wds[k].pImageInfo = &dii[k];
    }
    s.o.vkUpdateDescriptorSets(s.dev, 3, wds, 0, nullptr);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    r = s.o.vkCreateFence(s.dev, &fci, nullptr, &job->fence);
    if (r != VK_SUCCESS) { err = fmt("vkCreateFence=%d", r); break; }

    std::lock_guard<std::mutex> sl(s.submit_m);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = s.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    r = s.o.vkAllocateCommandBuffers(s.dev, &cai, &job->cb);
    if (r != VK_SUCCESS) { err = fmt("vkAllocateCommandBuffers=%d", r); break; }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    s.o.vkBeginCommandBuffer(job->cb, &bi);
    VkImageMemoryBarrier pre[3]{};
    for (int k = 0; k < 3; ++k) {
      pre[k].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      pre[k].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      pre[k].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    // Acquire the camera's buffer from the foreign (camera HAL) owner.
    pre[0].image = job->src_img; pre[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    pre[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT; pre[0].dstQueueFamilyIndex = s.qfam;
    pre[0].srcAccessMask = 0; pre[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    for (int k = 1; k < 3; ++k) {
      pre[k].image = job->out_img[k - 1]; pre[k].newLayout = VK_IMAGE_LAYOUT_GENERAL;
      pre[k].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; pre[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      pre[k].srcAccessMask = 0; pre[k].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    }
    s.o.vkCmdPipelineBarrier(job->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 3, pre);
    s.o.vkCmdBindPipeline(job->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pipe);
    s.o.vkCmdBindDescriptorSets(job->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.pl, 0, 1, &ds, 0, nullptr);
    const uint32_t pc[3] = {req.width, req.height, req.chroma_v_first ? 1u : 0u};
    s.o.vkCmdPushConstants(job->cb, pipe.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, pc);
    s.o.vkCmdDispatch(job->cb, (req.width + 7) / 8, (req.height + 7) / 8, 1);
    VkImageMemoryBarrier rel[3]{};
    for (int k = 0; k < 3; ++k) {
      rel[k].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      rel[k].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    // Release the camera's buffer back to its foreign owner.
    rel[0].image = job->src_img;
    rel[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; rel[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    rel[0].srcQueueFamilyIndex = s.qfam; rel[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    rel[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT; rel[0].dstAccessMask = 0;
    // Release the outputs to the external (Godot's device) owner, GENERAL ->
    // TRANSFER_SRC_OPTIMAL; the importer's acquire names the same layouts.
    for (int k = 1; k < 3; ++k) {
      rel[k].image = job->out_img[k - 1];
      rel[k].oldLayout = VK_IMAGE_LAYOUT_GENERAL; rel[k].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      rel[k].srcQueueFamilyIndex = s.qfam; rel[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
      rel[k].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; rel[k].dstAccessMask = 0;
    }
    s.o.vkCmdPipelineBarrier(job->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 3, rel);
    r = s.o.vkEndCommandBuffer(job->cb);
    if (r != VK_SUCCESS) { err = fmt("vkEndCommandBuffer=%d", r); break; }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &job->cb;
    r = s.o.vkQueueSubmit(s.q, 1, &si, job->fence);
    if (r != VK_SUCCESS) { err = fmt("vkQueueSubmit=%d", r); break; }
    submitted = true;
  } while (false);

  if (!submitted) {
    log_once(err.substr(0, err.find('=')), err + "; stills stay CPU_UPLOAD for this cause");
    // Nothing reached the GPU: destroy here; the provider frees the source.
    std::lock_guard<std::mutex> sl(s.submit_m);
    if (job->cb) s.o.vkFreeCommandBuffers(s.dev, s.pool, 1, &job->cb);
    if (job->fence) s.o.vkDestroyFence(s.dev, job->fence, nullptr);
    if (job->dp) s.o.vkDestroyDescriptorPool(s.dev, job->dp, nullptr);
    for (int k = 0; k < 2; ++k) {
      if (job->out_view[k]) s.o.vkDestroyImageView(s.dev, job->out_view[k], nullptr);
      if (job->out_img[k]) s.o.vkDestroyImage(s.dev, job->out_img[k], nullptr);
      if (job->out_mem[k]) s.o.vkFreeMemory(s.dev, job->out_mem[k], nullptr);
    }
    if (job->src_view) s.o.vkDestroyImageView(s.dev, job->src_view, nullptr);
    if (job->src_img) s.o.vkDestroyImage(s.dev, job->src_img, nullptr);
    if (job->src_mem) s.o.vkFreeMemory(s.dev, job->src_mem, nullptr);
    return {};
  }
  {
    std::lock_guard<std::mutex> l(s.m);
    job->id = s.next_job_id++;
    s.jobs[job->id] = job;
  }
  post_task([job] { wait_ours_and_release_source(job); });
  auto handle = std::make_shared<JobHandle>();
  handle->job = job;
  return std::static_pointer_cast<void>(handle);
}

std::shared_ptr<void> finish_copy(const std::shared_ptr<void>& opaque, uint64_t timeout_ns) noexcept {
  State& s = st();
  if (!opaque) return {};
  const std::shared_ptr<Job> job = std::static_pointer_cast<JobHandle>(opaque)->job;
  if (!job) return {};
  const auto deadline = Clock::now() + std::chrono::nanoseconds(timeout_ns);
  {
    std::unique_lock<std::mutex> l(job->m);
    if (!job->cv.wait_until(l, deadline, [&] { return job->our_done; })) {
      l.unlock();
      abandon_godot_side(job);
      log_once("copy missed the finish bound", "member stays CPU-only");
      return {};
    }
    if (job->gside != GodotSide::Pending) return {};  // finish called twice, or handle already dropped
  }
  // Export one fd per output; the import on Godot's device takes ownership.
  for (int k = 0; k < 2; ++k) {
    VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    gfi.memory = job->out_mem[k]; gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    const VkResult r = s.o.vkGetMemoryFdKHR(s.dev, &gfi, &fd);
    if (r != VK_SUCCESS || fd < 0) {
      log_once(fmt("vkGetMemoryFdKHR failed (%d)", r), "member stays CPU-only");
      abandon_godot_side(job);
      return {};
    }
    job->fds[k] = fd;
  }
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs || !s.helper || !s.installed.load(std::memory_order_acquire)) { abandon_godot_side(job); return {}; }
  {
    std::lock_guard<std::mutex> l(job->m);
    job->gside = GodotSide::Queued;
  }
  // Runs inline when this thread is already the render thread; otherwise queued
  // and awaited only until the bound. A render thread that is Godot's blocked
  // main thread simply misses the bound: the member stays CPU-only.
  rs->call_on_render_thread(godot::Callable(s.helper, godot::StringName("rt_import")).bind(static_cast<int64_t>(job->id)));
  std::unique_lock<std::mutex> l(job->m);
  job->cv.wait_until(l, deadline, [&] { return job->gside != GodotSide::Queued && job->gside != GodotSide::Running; });
  if (job->gside == GodotSide::Running) {
    // Started inside the bound: the remaining work is a bounded record + submit.
    job->cv.wait(l, [&] { return job->gside != GodotSide::Running; });
  }
  if (job->gside == GodotSide::Queued) {
    l.unlock();
    abandon_godot_side(job);
    log_once("render thread missed the finish bound", "member stays CPU-only; the queued import is skipped");
    return {};
  }
  if (job->gside == GodotSide::Submitted || job->gside == GodotSide::Done) return job->sidecar;
  return {};
}

const PlatformGpuImportOps kOps{
    &can_copy_native_buffers,
    &begin_copy,
    &finish_copy,
};

} // namespace

// ---------------------------------------------------------------------------
// Render-thread callbacks.
// ---------------------------------------------------------------------------

void PlatformGpuImportAndroidHelper::rt_create_device() {
  State& s = st();
  if (!s.installed.load(std::memory_order_acquire) || s.device_state.load() != 1) return;
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  godot::RenderingDevice* rd = rs ? rs->get_rendering_device() : nullptr;
  std::string err;
  std::string info;
  do {
    if (!rd) { err = "no RenderingDevice"; break; }
    if (rs->get_current_rendering_driver_name() != godot::String("vulkan")) { err = "rendering driver is not Vulkan"; break; }
    s.gdev = reinterpret_cast<VkDevice>(rd->get_driver_resource(godot::RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, godot::RID(), 0));
    s.phys = reinterpret_cast<VkPhysicalDevice>(rd->get_driver_resource(godot::RenderingDevice::DRIVER_RESOURCE_PHYSICAL_DEVICE, godot::RID(), 0));
    s.instance = reinterpret_cast<VkInstance>(rd->get_driver_resource(godot::RenderingDevice::DRIVER_RESOURCE_TOPMOST_OBJECT, godot::RID(), 0));
    s.gqueue = reinterpret_cast<VkQueue>(rd->get_driver_resource(godot::RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE, godot::RID(), 0));
    s.gqfam = static_cast<uint32_t>(rd->get_driver_resource(godot::RenderingDevice::DRIVER_RESOURCE_QUEUE_FAMILY, godot::RID(), 0));
    if (!s.gdev || !s.phys || !s.instance || !s.gqueue) { err = "Godot's Vulkan handles unavailable"; break; }
    void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    s.gipa = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr")) : nullptr;
    if (!s.gipa) { err = "vkGetInstanceProcAddr unavailable"; break; }
    s.gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(s.gipa(s.instance, "vkGetDeviceProcAddr"));
    auto qfp = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(s.gipa(s.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto enum_ext = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(s.gipa(s.instance, "vkEnumerateDeviceExtensionProperties"));
    auto feat2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(s.gipa(s.instance, "vkGetPhysicalDeviceFeatures2"));
    auto memp = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(s.gipa(s.instance, "vkGetPhysicalDeviceMemoryProperties"));
    auto create = reinterpret_cast<PFN_vkCreateDevice>(s.gipa(s.instance, "vkCreateDevice"));
    if (!s.gdpa || !qfp || !enum_ext || !feat2 || !memp || !create) { err = "instance functions unavailable"; break; }
    const std::string gmiss = load_fns(s.gdpa, s.gdev, s.g, false);
    if (!gmiss.empty()) { err = "Godot-device functions missing: " + gmiss; break; }
    memp(s.phys, &s.mem_props);

    // Queue: a compute-capable family other than Godot's, else Godot's family
    // at another queue index when it has one.
    uint32_t nq = 0;
    qfp(s.phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    qfp(s.phys, &nq, qf.data());
    int other = -1;
    for (uint32_t i = 0; i < nq; ++i)
      if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && i != s.gqfam) { other = static_cast<int>(i); break; }
    if (other >= 0) { s.qfam = static_cast<uint32_t>(other); s.qidx = 0; }
    else if (s.gqfam < nq) { s.qfam = s.gqfam; s.qidx = qf[s.gqfam].queueCount > 1 ? 1u : 0u; }
    else { err = "Godot's queue family out of range"; break; }

    uint32_t ne = 0;
    enum_ext(s.phys, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> ex(ne);
    enum_ext(s.phys, nullptr, &ne, ex.data());
    auto listed = [&](const char* n) { for (auto& e : ex) if (std::strcmp(e.extensionName, n) == 0) return true; return false; };
    const char* required[] = {VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                              VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
                              VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
    const char* deps[] = {VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                          VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
                          VK_KHR_MAINTENANCE_1_EXTENSION_NAME};
    std::vector<const char*> en;
    for (const char* n : required) {
      if (!listed(n)) { err = std::string("device extension not listed: ") + n; break; }
      en.push_back(n);
    }
    if (!err.empty()) break;
    for (const char* n : deps) if (listed(n)) en.push_back(n);

    VkPhysicalDeviceSamplerYcbcrConversionFeatures ysup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
    VkPhysicalDeviceFeatures2 fsup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    fsup.pNext = &ysup;
    feat2(s.phys, &fsup);
    if (!ysup.samplerYcbcrConversion || !fsup.features.shaderStorageImageExtendedFormats) {
      err = "samplerYcbcrConversion or shaderStorageImageExtendedFormats unsupported";
      break;
    }
    VkPhysicalDeviceSamplerYcbcrConversionFeatures yf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
    yf.samplerYcbcrConversion = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &yf;
    f2.features.shaderStorageImageExtendedFormats = VK_TRUE;
    std::vector<float> prio(s.qidx + 1, 0.5f);
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = s.qfam; qci.queueCount = s.qidx + 1; qci.pQueuePriorities = prio.data();
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(en.size()); dci.ppEnabledExtensionNames = en.data();
    VkResult r = create(s.phys, &dci, nullptr, &s.dev);
    if (r != VK_SUCCESS) { s.dev = VK_NULL_HANDLE; err = fmt("vkCreateDevice=%d", r); break; }
    const std::string omiss = load_fns(s.gdpa, s.dev, s.o, true);
    if (!omiss.empty()) { err = "our-device functions missing: " + omiss; break; }
    s.o.vkGetDeviceQueue(s.dev, s.qfam, s.qidx, &s.q);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = s.qfam;
    r = s.o.vkCreateCommandPool(s.dev, &pci, nullptr, &s.pool);
    if (r != VK_SUCCESS) { err = fmt("vkCreateCommandPool=%d", r); break; }
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(kPlatformGpuImportYcbcrSplitSpv); smi.pCode = kPlatformGpuImportYcbcrSplitSpv;
    r = s.o.vkCreateShaderModule(s.dev, &smi, nullptr, &s.sm);
    if (r != VK_SUCCESS) { err = fmt("vkCreateShaderModule=%d", r); break; }
    VkCommandPoolCreateInfo gpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    gpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; gpci.queueFamilyIndex = s.gqfam;
    r = s.g.vkCreateCommandPool(s.gdev, &gpci, nullptr, &s.gpool);
    if (r != VK_SUCCESS) { err = fmt("vkCreateCommandPool(Godot device)=%d", r); break; }
    info = fmt("second VkDevice ready: queue family %u index %u (Godot's family %u); %zu extensions enabled",
               s.qfam, s.qidx, s.gqfam, en.size());
  } while (false);
  if (!err.empty()) {
    if (s.sm && s.dev) { s.o.vkDestroyShaderModule(s.dev, s.sm, nullptr); s.sm = VK_NULL_HANDLE; }
    if (s.pool && s.dev) { s.o.vkDestroyCommandPool(s.dev, s.pool, nullptr); s.pool = VK_NULL_HANDLE; }
    if (s.dev && s.o.vkDestroyDevice) s.o.vkDestroyDevice(s.dev, nullptr);
    s.dev = VK_NULL_HANDLE;
    s.device_state.store(3, std::memory_order_release);
    log_once("second device unavailable", err + "; captures stay CPU_UPLOAD");
    return;
  }
  {
    std::lock_guard<std::mutex> l(s.wm);
    s.worker_stop = false;
  }
  s.worker = std::thread(worker_main);
  s.device_state.store(2, std::memory_order_release);
  log_once("ready", info);
}

void PlatformGpuImportAndroidHelper::rt_replenish(int64_t width, int64_t height) {
  State& s = st();
  const auto extent = std::make_pair(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
  if (!s.installed.load(std::memory_order_acquire) || s.device_state.load() != 2) {
    std::lock_guard<std::mutex> l(s.m);
    s.replenish_pending.erase(extent);
    return;
  }
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  godot::RenderingDevice* rd = rs ? rs->get_rendering_device() : nullptr;
  const int64_t frame = godot::Engine::get_singleton() ? godot::Engine::get_singleton()->get_frames_drawn() : 0;
  size_t have = 0;
  {
    std::lock_guard<std::mutex> l(s.m);
    have = s.pool_entries[extent].size();
  }
  while (rd && have < kPoolTargetPerExtent) {
    GpuComputeSidecarPlaneRid planes[2]{};
    const uint32_t pw[2] = {extent.first, extent.first / 2};
    const uint32_t ph[2] = {extent.second, extent.second / 2};
    const godot::RenderingDevice::DataFormat rf[2] = {godot::RenderingDevice::DATA_FORMAT_R8_UNORM,
                                                       godot::RenderingDevice::DATA_FORMAT_R8G8_UNORM};
    const GpuBackingPlaneFormat sf[2] = {GpuBackingPlaneFormat::R8_UNORM, GpuBackingPlaneFormat::RG8_UNORM};
    PoolEntry e{};
    bool ok = true;
    for (int k = 0; k < 2; ++k) {
      godot::Ref<godot::RDTextureFormat> tf;
      tf.instantiate();
      tf->set_texture_type(godot::RenderingDevice::TEXTURE_TYPE_2D);
      tf->set_format(rf[k]);
      tf->set_width(pw[k]); tf->set_height(ph[k]); tf->set_depth(1); tf->set_array_layers(1); tf->set_mipmaps(1);
      tf->set_usage_bits(godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
                         godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
                         godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
      godot::Ref<godot::RDTextureView> tv;
      tv.instantiate();
      planes[k].rd_texture = rd->texture_create(tf, tv, godot::TypedArray<godot::PackedByteArray>());
      planes[k].shape = GpuBackingPlaneDescriptor{sf[k], pw[k], ph[k]};
      if (!planes[k].rd_texture.is_valid()) { ok = false; continue; }
      e.dst[k] = reinterpret_cast<VkImage>(rd->get_driver_resource(godot::RenderingDevice::DRIVER_RESOURCE_TEXTURE, planes[k].rd_texture, 0));
      // Leaves RD's tracker at TRANSFER_DST, the layout our copy writes in.
      rd->texture_clear(planes[k].rd_texture, godot::Color(0, 0, 0, 0), 0, 1, 0, 1);
    }
    // Adopted even on failure, so any created RID is released through the drain.
    e.sidecar = gpu_compute_sidecar_adopt_rd_textures(planes, 2, {});
    if (!ok || !e.sidecar || !e.dst[0] || !e.dst[1]) {
      log_once("pre-cleared destination creation failed", "stills stay CPU_UPLOAD until it succeeds");
      break;
    }
    e.cleared_frame = frame;
    std::lock_guard<std::mutex> l(s.m);
    s.pool_entries[extent].push_back(std::move(e));
    have = s.pool_entries[extent].size();
  }
  std::lock_guard<std::mutex> l(s.m);
  s.replenish_pending.erase(extent);
}

void PlatformGpuImportAndroidHelper::rt_import(int64_t job_id) {
  State& s = st();
  std::shared_ptr<Job> job;
  {
    std::lock_guard<std::mutex> l(s.m);
    auto it = s.jobs.find(static_cast<uint64_t>(job_id));
    if (it != s.jobs.end()) job = it->second;
  }
  if (!job) return;
  {
    std::lock_guard<std::mutex> l(job->m);
    if (job->gside != GodotSide::Queued) return;  // abandoned at the bound
    job->gside = GodotSide::Running;
  }
  {
    // Free finished Godot-side command buffers (this thread owns their pool).
    std::vector<VkCommandBuffer> done;
    {
      std::lock_guard<std::mutex> l(s.m);
      done.swap(s.gcb_to_free);
    }
    if (!done.empty()) s.g.vkFreeCommandBuffers(s.gdev, s.gpool, static_cast<uint32_t>(done.size()), done.data());
  }
  const auto extent = std::make_pair(job->w, job->h);
  const int64_t frame = godot::Engine::get_singleton() ? godot::Engine::get_singleton()->get_frames_drawn() : 0;
  PoolEntry entry{};
  bool have_entry = false;
  bool queue_replenish = false;
  {
    std::lock_guard<std::mutex> l(s.m);
    auto& dq = s.pool_entries[extent];
    if (!dq.empty() && frame >= dq.front().cleared_frame + kPoolFrameMargin) {
      entry = std::move(dq.front());
      dq.pop_front();
      have_entry = true;
    }
    if (dq.size() < kPoolTargetPerExtent && s.replenish_pending.insert(extent).second) queue_replenish = true;
  }
  if (queue_replenish) {
    godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
    if (rs) rs->call_on_render_thread(godot::Callable(this, godot::StringName("rt_replenish")).bind(static_cast<int64_t>(job->w), static_cast<int64_t>(job->h)));
  }
  std::string err;
  bool submitted = false;
  do {
    if (!s.installed.load(std::memory_order_acquire)) { err = "importer closing"; break; }
    if (!have_entry) { err = "no pre-cleared destination ready yet"; break; }
    for (int k = 0; k < 2; ++k) {
      VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
      emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ici.pNext = &emi;
      ici.imageType = VK_IMAGE_TYPE_2D; ici.format = job->out_fmt[k]; ici.extent = {job->out_w[k], job->out_h[k], 1};
      ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
      ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VkResult r = s.g.vkCreateImage(s.gdev, &ici, nullptr, &job->g_img[k]);
      if (r != VK_SUCCESS) { err = fmt("vkCreateImage(Godot import %d)=%d", k, r); break; }
      VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
      ded.image = job->g_img[k];
      VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
      imp.pNext = &ded; imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT; imp.fd = job->fds[k];
      VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      mai.pNext = &imp; mai.allocationSize = job->out_size[k]; mai.memoryTypeIndex = job->out_mtype;
      r = s.g.vkAllocateMemory(s.gdev, &mai, nullptr, &job->g_mem[k]);
      if (r != VK_SUCCESS) { err = fmt("vkAllocateMemory(Godot import fd %d)=%d", k, r); break; }
      job->fds[k] = -1;  // the driver owns it now
      r = s.g.vkBindImageMemory(s.gdev, job->g_img[k], job->g_mem[k], 0);
      if (r != VK_SUCCESS) { err = fmt("vkBindImageMemory(Godot import %d)=%d", k, r); break; }
    }
    if (!err.empty()) break;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkResult r = s.g.vkCreateFence(s.gdev, &fci, nullptr, &job->gfence);
    if (r != VK_SUCCESS) { err = fmt("vkCreateFence(Godot)=%d", r); break; }
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = s.gpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    r = s.g.vkAllocateCommandBuffers(s.gdev, &cai, &job->gcb);
    if (r != VK_SUCCESS) { err = fmt("vkAllocateCommandBuffers(Godot)=%d", r); break; }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    s.g.vkBeginCommandBuffer(job->gcb, &bi);
    VkImageMemoryBarrier acq[4]{};
    for (int k = 0; k < 4; ++k) {
      acq[k].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      acq[k].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    for (int k = 0; k < 2; ++k) {
      // Acquire from the external owner, matching our release.
      acq[k].image = job->g_img[k];
      acq[k].oldLayout = VK_IMAGE_LAYOUT_GENERAL; acq[k].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      acq[k].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL; acq[k].dstQueueFamilyIndex = s.gqfam;
      acq[k].srcAccessMask = 0; acq[k].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      // RD's destination: stays in TRANSFER_DST, the layout RD's tracker holds.
      acq[2 + k].image = entry.dst[k];
      acq[2 + k].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; acq[2 + k].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      acq[2 + k].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; acq[2 + k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      acq[2 + k].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; acq[2 + k].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    s.g.vkCmdPipelineBarrier(job->gcb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 4, acq);
    for (int k = 0; k < 2; ++k) {
      VkImageCopy rgn{};
      rgn.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      rgn.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      rgn.extent = {job->out_w[k], job->out_h[k], 1};
      s.g.vkCmdCopyImage(job->gcb, job->g_img[k], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, entry.dst[k],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    }
    VkImageMemoryBarrier post[2]{};
    for (int k = 0; k < 2; ++k) {
      post[k].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; post[k].image = entry.dst[k];
      post[k].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      post[k].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; post[k].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      post[k].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; post[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      post[k].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      post[k].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
    s.g.vkCmdPipelineBarrier(job->gcb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                             nullptr, 2, post);
    r = s.g.vkEndCommandBuffer(job->gcb);
    if (r != VK_SUCCESS) { err = fmt("vkEndCommandBuffer(Godot)=%d", r); break; }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &job->gcb;
    r = s.g.vkQueueSubmit(s.gqueue, 1, &si, job->gfence);
    if (r != VK_SUCCESS) { err = fmt("vkQueueSubmit(Godot queue)=%d", r); break; }
    submitted = true;
  } while (false);

  if (!submitted) {
    if (err != "no pre-cleared destination ready yet" && err != "importer closing")
      log_once(err.substr(0, err.find('=')), err + "; stills stay CPU_UPLOAD for this cause");
    for (int k = 0; k < 2; ++k) {
      if (job->g_img[k]) s.g.vkDestroyImage(s.gdev, job->g_img[k], nullptr);
      if (job->g_mem[k]) s.g.vkFreeMemory(s.gdev, job->g_mem[k], nullptr);
      job->g_img[k] = VK_NULL_HANDLE; job->g_mem[k] = VK_NULL_HANDLE;
    }
    if (job->gfence) { s.g.vkDestroyFence(s.gdev, job->gfence, nullptr); job->gfence = VK_NULL_HANDLE; }
    if (job->gcb) { s.g.vkFreeCommandBuffers(s.gdev, s.gpool, 1, &job->gcb); job->gcb = VK_NULL_HANDLE; }
    if (have_entry) {
      // Unwritten and still at TRANSFER_DST: back to the pool.
      std::lock_guard<std::mutex> l(s.m);
      s.pool_entries[extent].push_front(std::move(entry));
    }
    {
      std::lock_guard<std::mutex> l(job->m);
      job->gside = GodotSide::Never;
    }
    job->cv.notify_all();
    post_task([job] { reap(job); });
    return;
  }
  {
    std::lock_guard<std::mutex> l(job->m);
    job->sidecar = std::move(entry.sidecar);
    job->gside = GodotSide::Submitted;
  }
  job->cv.notify_all();
  post_task([job] { wait_godot_and_release_imports(job); });
}

// ---------------------------------------------------------------------------
// Install / uninstall / tick.
// ---------------------------------------------------------------------------

void register_platform_gpu_import_android_internal_classes() {
  godot::ClassDB::register_class<PlatformGpuImportAndroidHelper>();
}

void install_platform_gpu_import_android() {
  State& s = st();
  if (s.installed.load()) return;
  if (!s.helper) s.helper = memnew(PlatformGpuImportAndroidHelper);  // never deleted (see the class)
  s.device_state.store(0);
  s.installed.store(true, std::memory_order_release);
  set_platform_gpu_import_ops(&kOps);
}

void platform_gpu_import_android_main_thread_tick() {
  State& s = st();
  if (!s.installed.load(std::memory_order_acquire)) return;
  if (s.quest.load() < 0) {
    godot::OS* os = godot::OS::get_singleton();
    const godot::String model = os ? os->get_model_name() : godot::String();
    s.quest.store(model.to_lower().find("quest") >= 0 ? 1 : 0);
  }
  bool xr = false;
  if (godot::XRServer* xs = godot::XRServer::get_singleton()) {
    const godot::Ref<godot::XRInterface> pi = xs->get_primary_interface();
    xr = pi.is_valid() && pi->is_initialized() && pi->get_name() == godot::StringName("OpenXR");
  }
  s.xr_gate.store(xr, std::memory_order_release);
  if (xr && s.quest.load() == 1) {
    int expected = 0;
    if (s.device_state.compare_exchange_strong(expected, 1)) {
      godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
      if (rs && s.helper) rs->call_on_render_thread(godot::Callable(s.helper, godot::StringName("rt_create_device")));
      else s.device_state.store(0);
    }
  }
}

void uninstall_platform_gpu_import_android() {
  State& s = st();
  if (!s.installed.load()) return;
  // 1. Close admission; wait for every in-flight seam call to return.
  clear_platform_gpu_import_ops();
  s.installed.store(false, std::memory_order_release);
  // 2. Queued render-thread imports may never run now: decide them.
  std::vector<std::shared_ptr<Job>> jobs;
  {
    std::lock_guard<std::mutex> l(s.m);
    for (auto& kv : s.jobs) jobs.push_back(kv.second);
  }
  for (auto& j : jobs) abandon_godot_side(j);
  // 3. Drain the worker: every source released, every fence waited, every job reaped.
  if (s.worker.joinable()) {
    {
      std::lock_guard<std::mutex> l(s.wm);
      s.worker_stop = true;
    }
    s.wcv.notify_all();
    s.worker.join();
  }
  // 4. Drop pre-cleared destinations (their RIDs go through the release drain).
  {
    std::lock_guard<std::mutex> l(s.m);
    s.pool_entries.clear();
    s.replenish_pending.clear();
    s.jobs.clear();
    s.gcb_to_free.clear();  // freed with their pool below
  }
  // 5. Destroy our device. Godot-device objects of ours: the command pool.
  if (s.dev) {
    s.o.vkDeviceWaitIdle(s.dev);
    {
      std::lock_guard<std::mutex> l(s.m);
      for (auto& kv : s.pipes) {
        ConvPipe& p = kv.second;
        if (p.pipe) s.o.vkDestroyPipeline(s.dev, p.pipe, nullptr);
        if (p.pl) s.o.vkDestroyPipelineLayout(s.dev, p.pl, nullptr);
        if (p.dsl) s.o.vkDestroyDescriptorSetLayout(s.dev, p.dsl, nullptr);
        if (p.sampler) s.o.vkDestroySampler(s.dev, p.sampler, nullptr);
        if (p.conv) s.o.vkDestroySamplerYcbcrConversion(s.dev, p.conv, nullptr);
      }
      s.pipes.clear();
      s.pipes_pending.clear();
    }
    if (s.sm) s.o.vkDestroyShaderModule(s.dev, s.sm, nullptr);
    if (s.pool) s.o.vkDestroyCommandPool(s.dev, s.pool, nullptr);
    s.o.vkDestroyDevice(s.dev, nullptr);
    s.sm = VK_NULL_HANDLE; s.pool = VK_NULL_HANDLE; s.dev = VK_NULL_HANDLE;
  }
  if (s.gpool && s.gdev) {
    s.g.vkDestroyCommandPool(s.gdev, s.gpool, nullptr);  // every job's Godot fence was waited above
    s.gpool = VK_NULL_HANDLE;
  }
  s.device_state.store(0);
  s.xr_gate.store(false);
}

} // namespace cambang

#else  // !__ANDROID__

namespace cambang {
void register_platform_gpu_import_android_internal_classes() {}
void install_platform_gpu_import_android() {}
void uninstall_platform_gpu_import_android() {}
void platform_gpu_import_android_main_thread_tick() {}
} // namespace cambang

#endif

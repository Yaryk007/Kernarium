// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// What the tests that drive the Venus renderer in-process share: the
// renderer's entry points, and a little-endian encoder for the guest's command
// stream in the Venus driver's layout. The guest chooses every object id, so a
// test needs no reply stream to use an object.

#include <cstdint>
#include <cstring>
#include <vector>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

// vkr_renderer.h's public entry points, declared here to avoid its internal
// include web. The renderer runs the Venus dispatch synchronously on the
// calling thread and returns false once the context is lost.
extern "C" {
constexpr std::uint32_t vkrRendererThreadSync = 1u << 0;
constexpr std::uint32_t vkrRendererAsyncFenceCb = 1u << 1;
constexpr std::uint32_t virtgpuDrmCapsetVenus = 4;
constexpr std::uint32_t blobFlagUseMappable = 1;
struct vkr_renderer_callbacks {
    void (*debug_logger)(int, const char*, void*);
    void (*retire_fence)(std::uint32_t, std::uint32_t, std::uint64_t);
    void (*context_fatal)(std::uint32_t);
};
bool vkr_renderer_init(std::uint32_t flags, const vkr_renderer_callbacks* cbs);
void vkr_renderer_fini();
bool vkr_renderer_create_context(
    std::uint32_t ctxId, std::uint32_t ctxFlags, std::uint32_t nlen, const char* name);
void vkr_renderer_destroy_context(std::uint32_t ctxId);
bool vkr_renderer_submit_cmd(std::uint32_t ctxId, void* cmd, std::uint32_t size);
// vulkanInfo is a struct virgl_resource_vulkan_info the renderer fills only
// for an opaque resource; the shared-memory reply stream leaves it alone.
bool vkr_renderer_create_resource(std::uint32_t ctxId, std::uint32_t resId,
    std::uint64_t blobId, std::uint64_t blobSize, std::uint32_t blobFlags,
    int* fdType, int* fd, std::uint32_t* mapInfo, void* vulkanInfo);
void vkr_renderer_destroy_resource(std::uint32_t ctxId, std::uint32_t resId);
}

namespace uml::gpu::testing::venus {

// The Venus command type numbers the tests use.
enum Cmd : std::uint32_t {
    createInstance = 0, enumeratePhysicalDevices = 2, createDevice = 11,
    allocateMemory = 21, bindBufferMemory = 28, bindImageMemory = 29,
    getBufferMemoryRequirements = 30, getImageMemoryRequirements = 31,
    createSemaphore = 40, createQueryPool = 47, createBuffer = 50, destroyBuffer = 51,
    createImage = 54, destroyImage = 55, createImageView = 57,
    destroyImageView = 58, createShaderModule = 59, createPipelineCache = 61,
    destroyPipelineCache = 62, getPipelineCacheData = 63, mergePipelineCaches = 64,
    createGraphicsPipelines = 65,
    createComputePipelines = 66, createPipelineLayout = 68,
    freeDescriptorSets = 78, createFramebuffer = 80,
    createRenderPass = 82,
    createCommandPool = 85, resetCommandPool = 87, allocateCommandBuffers = 88,
    freeCommandBuffers = 89, beginCommandBuffer = 90, endCommandBuffer = 91,
    cmdCopyBuffer = 112,
    cmdCopyBufferToImage = 115, cmdBeginRenderPass = 133,
    cmdEndRenderPass = 135, bindBufferMemory2 = 138, bindImageMemory2 = 139,
    getImageMemoryRequirements2 = 144, getBufferMemoryRequirements2 = 145,
    cmdBeginRenderPass2 = 168,
    setReplyCommandStream = 178, getImageDrmFormatModifierProperties = 187,
    importSemaphoreResource = 246, copyImageToImage = 295, transitionImageLayout = 296,
    copyImageToMemory = 297, copyMemoryToImage = 298,
    cmdCopyMemoryToImage = 356, cmdCopyImageToMemory = 357,
    cmdCopyQueryPoolResultsToMemory = 360,
    createPipelineBinaries = 371, destroyPipelineBinary = 372, getPipelineKey = 373,
    getPipelineBinaryData = 374, releaseCapturedPipelineData = 375,
};

// A little-endian Venus wire encoder in the driver's layout.
class Stream {
public:
    void u32(std::uint32_t v) { append(&v, 4); }
    void u64(std::uint64_t v) { append(&v, 8); }
    void ptr(bool present) { u64(present ? 1 : 0); }
    void arraySize(std::uint64_t n) { u64(n); }
    void f32(float v) { std::uint32_t b; std::memcpy(&b, &v, 4); u32(b); }
    // A blob array of `size` bytes, padded to a multiple of four.
    void blob(const void* data, std::size_t size)
    {
        append(data, size);
        static constexpr std::uint8_t pad[4] = {};
        append(pad, (4 - size % 4) % 4);
    }
    // A string in an array of strings: its size with the terminator, then its
    // characters.
    void string(const char* text)
    {
        const std::size_t size = std::strlen(text) + 1;
        arraySize(size);
        blob(text, size);
    }
    void command(Cmd type, bool reply = false)
    {
        u32(type);
        u32(reply ? 1 : 0 /* VK_COMMAND_GENERATE_REPLY_BIT_EXT */);
    }
    [[nodiscard]] std::vector<std::uint8_t>& bytes() { return bytes_; }
    [[nodiscard]] std::uint32_t size() const
    {
        return static_cast<std::uint32_t>(bytes_.size());
    }

private:
    void append(const void* data, std::size_t n)
    {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes_.insert(bytes_.end(), p, p + n);
    }
    std::vector<std::uint8_t> bytes_;
};

// What a device enables beyond its one queue.
struct DeviceExtras {
    std::vector<const char*> extensions;   // device extension names
    bool pipelineBinaries = false;         // VkPhysicalDevicePipelineBinaryFeaturesKHR, on
    // The type of every other feature structure to chain, each a single
    // VkBool32 that is on.
    std::vector<std::uint32_t> features;
    // A VkDeviceGroupDeviceCreateInfo of one physical device, which is the null
    // handle.
    bool groupOfTheNullPhysicalDevice = false;
};

// vkCreateDevice of the physical device physDevId, a device with one queue and
// the extensions and features `extras` names, under the guest's id deviceId.
inline void encodeCreateDevice(Stream& s, std::uint64_t physDevId, std::uint64_t deviceId,
    const DeviceExtras& extras = {}, bool reply = false)
{
    s.command(createDevice, reply);
    s.u64(physDevId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    // Each chained structure encodes its type, then the rest of the chain, then
    // its own fields, so the fields follow the chain in reverse.
    std::vector<std::uint32_t> chain = extras.features;
    if (extras.pipelineBinaries)
        chain.push_back(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR);
    if (extras.groupOfTheNullPhysicalDevice)
        chain.push_back(VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO);
    for (const std::uint32_t type : chain) {
        s.ptr(true);
        s.u32(type);
    }
    s.ptr(false);
    if (extras.groupOfTheNullPhysicalDevice) {
        s.u32(1); s.arraySize(1); s.u64(0);
    }
    for (std::size_t i = 0; i < chain.size() - (extras.groupOfTheNullPhysicalDevice ? 1 : 0); ++i)
        s.u32(VK_TRUE);
    s.u32(0);                       // flags
    s.u32(1); s.arraySize(1);       // one queue family
    s.u32(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO); s.u64(0);
    s.u32(0); s.u32(0); s.u32(1);
    s.arraySize(1); s.f32(1.0F);
    s.u32(0); s.arraySize(0);       // layers
    s.u32(static_cast<std::uint32_t>(extras.extensions.size()));
    s.arraySize(extras.extensions.size());
    for (const char* extension : extras.extensions)
        s.string(extension);
    s.ptr(false);                   // pEnabledFeatures
    s.ptr(false);                   // pAllocator
    s.ptr(true); s.u64(deviceId);
}

// An instance of the given API version, its one physical device, and a device
// with one queue and the extensions and features `extras` names (none by
// default), under the ids the caller chooses.
inline void encodeInstanceAndDevice(Stream& s, std::uint32_t apiVersion,
    std::uint64_t instanceId, std::uint64_t physDevId, std::uint64_t deviceId,
    const DeviceExtras& extras = {})
{
    s.command(createInstance);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO); s.u64(0);
    s.u32(0);                       // flags
    s.ptr(true);                    // pApplicationInfo
    s.u32(VK_STRUCTURE_TYPE_APPLICATION_INFO); s.u64(0);
    s.arraySize(0); s.u32(0); s.arraySize(0); s.u32(0);
    s.u32(apiVersion);
    s.u32(0); s.arraySize(0);       // layers
    s.u32(0); s.arraySize(0);       // extensions
    s.ptr(false);                   // pAllocator
    s.ptr(true); s.u64(instanceId);

    s.command(enumeratePhysicalDevices);
    s.u64(instanceId);
    s.ptr(true); s.u32(1);
    s.arraySize(1); s.u64(physDevId);

    encodeCreateDevice(s, physDevId, deviceId, extras);
}

} // namespace uml::gpu::testing::venus

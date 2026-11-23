// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Imports CPU-written hardware buffers into the product's Vulkan HAL.
 *
 * Each case allocates a buffer of one format and width, fills it through the
 * CPU mapping, rewrites a rectangle of it through a second lock of only that
 * rectangle, imports it into Vulkan as a dedicated allocation and copies the
 * image into a host-visible buffer, which must hold what the CPU wrote. The
 * widths include the ones whose tightly packed rows are not a multiple of the
 * 16 bytes Metal reads a texture over a buffer at. A run that reports every
 * case imported and every byte read back is a proof that the row pitch gralloc
 * reports is a pitch the driver can import and the renderer reads the rows at.
 *
 * Once the hardware-buffer cases are done and their verdict is printed, it
 * checks that the driver shares memory as a file descriptor beside the
 * hardware buffer: both fd extensions are offered and enabled on the same
 * device, and for each fd handle type host-visible memory is allocated for
 * export, exported, mapped in this process through Vulkan and through the fd,
 * written from there and from another process, read back by the GPU, and
 * imported again from a second fd and read back by the GPU and the CPU. Three
 * more cases follow for each type and size. Two hand the fd to another
 * instance of this tool, started with exec, which holds nothing of this
 * process but that one fd and creates an instance and a device of its own: it
 * imports the fd, maps it, reads it with the GPU and writes a quarter of it. In
 * the first the exporter is alive and reads that quarter; in the second it has
 * freed its memory and closed its fd before the other instance imports, so that
 * only the other instance's fd keeps the memory. In the third a fd and a
 * mapping of it outlive the exporting device and a device that imported the
 * fd, which are destroyed, and still read the bytes.
 *
 * Usage: kernarium_vk_ahb_import [width...]
 * With no argument it runs a set of widths for each format. The exit status is
 * 0 when every case that could be allocated passed.
 *
 * It prints one line for each hardware-buffer case, "FORMAT WxH stride S
 * (B bytes)", and under it the verdict "  PASS" or "  FAIL" (a failed step is
 * printed above FAIL). When the hardware-buffer cases are done it prints
 * "hardware-buffer verdict: PASS|FAIL: N cases imported, F failed, K not
 * allocated". Then it prints one line for each extension ("memory-fd
 * extension NAME: offered" or "not offered"), then for each memory-fd case
 * "memory-fd TYPE N bytes", with ", imported by another instance", ", imported
 * after the exporter was freed" or ", kept past both devices" after it for the
 * three more cases, and the same indented verdict. The last line is
 * "PASS|FAIL: N cases imported, F failed, K not allocated; memory fd M cases,
 * G failed" on one line (tests/integration/android/run_android_vulkan_smoke.py
 * reads it).
 *
 * The tool starts itself again for the other instance with the argument
 * --memory-fd-child; that argument is private to it.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>

#include <android/hardware_buffer.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

namespace {

struct FormatCase {
    uint32_t ahbFormat;
    const char* name;
    uint32_t bytesPerPixel;
};

const FormatCase kFormats[] = {
        {AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8", 4},
        {AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM, "R5G6B5", 2},
        {AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16F", 8},
        {AHARDWAREBUFFER_FORMAT_R8_UNORM, "R8", 1},
};

const uint32_t kDefaultWidths[] = {640, 642, 333, 100, 17, 1};
constexpr uint32_t kHeight = 48;

// What the CPU writes at a byte of a row.
uint8_t pattern(uint32_t x, uint32_t y, uint32_t byte) {
    return static_cast<uint8_t>(x * 7 + y * 13 + byte * 31 + 1);
}

// The part of a buffer a second, partial lock rewrites.
struct Update {
    uint32_t x, y, width, height;
    bool contains(uint32_t px, uint32_t py) const {
        return px >= x && px < x + width && py >= y && py < y + height;
    }
};

// What a byte holds once the update has been written over the pattern.
uint8_t expected(const Update& update, uint32_t x, uint32_t y, uint32_t byte) {
    return update.contains(x, y) ? static_cast<uint8_t>(pattern(x, y, byte) ^ 0x5a) : pattern(x, y, byte);
}

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory = {};
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getProperties = nullptr;
    // The guest fd extensions the driver offers, and the entry points of the first.
    bool fdOffered = false;
    bool dmaBufOffered = false;
    PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;
    PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties = nullptr;
};

constexpr const char* kFdExtension = "VK_KHR_external_memory_fd";
constexpr const char* kDmaBufExtension = "VK_EXT_external_memory_dma_buf";

bool check(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return true;
    printf("    %s -> %d\n", what, static_cast<int>(result));
    return false;
}

bool createDevice(Device* d) {
    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (!check(vkCreateInstance(&ici, nullptr, &d->instance), "vkCreateInstance")) return false;

    uint32_t count = 1;
    VkResult r = vkEnumeratePhysicalDevices(d->instance, &count, &d->physical);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
        printf("    no physical device (%d)\n", static_cast<int>(r));
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(d->physical, &d->memory);

    uint32_t offeredCount = 0;
    std::vector<VkExtensionProperties> offered;
    if (vkEnumerateDeviceExtensionProperties(d->physical, nullptr, &offeredCount, nullptr) == VK_SUCCESS) {
        offered.resize(offeredCount);
        if (vkEnumerateDeviceExtensionProperties(d->physical, nullptr, &offeredCount, offered.data()) !=
            VK_SUCCESS) {
            offeredCount = 0;
        }
    }
    for (uint32_t i = 0; i < offeredCount; i++) {
        if (!strcmp(offered[i].extensionName, kFdExtension)) d->fdOffered = true;
        if (!strcmp(offered[i].extensionName, kDmaBufExtension)) d->dmaBufOffered = true;
    }

    // The hardware-buffer extensions, and the fd extensions the driver offers on the same device.
    std::vector<const char*> extensions = {
            "VK_ANDROID_external_memory_android_hardware_buffer",
            "VK_KHR_sampler_ycbcr_conversion",
            "VK_KHR_external_memory",
            "VK_KHR_dedicated_allocation",
            "VK_KHR_get_memory_requirements2",
            "VK_KHR_bind_memory2",
            "VK_KHR_maintenance1",
            "VK_EXT_queue_family_foreign",
    };
    if (d->fdOffered) extensions.push_back(kFdExtension);
    if (d->fdOffered && d->dmaBufOffered) extensions.push_back(kDmaBufExtension);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr = {};
    ycbcr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    ycbcr.samplerYcbcrConversion = VK_TRUE;
    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &ycbcr;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    dci.ppEnabledExtensionNames = extensions.data();
    if (!check(vkCreateDevice(d->physical, &dci, nullptr, &d->device), "vkCreateDevice")) return false;

    vkGetDeviceQueue(d->device, 0, 0, &d->queue);
    d->getProperties = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
            vkGetDeviceProcAddr(d->device, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    if (!d->getProperties) {
        printf("    vkGetAndroidHardwareBufferPropertiesANDROID is not exposed\n");
        return false;
    }
    if (d->fdOffered) {
        d->getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(d->device, "vkGetMemoryFdKHR"));
        d->getMemoryFdProperties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
                vkGetDeviceProcAddr(d->device, "vkGetMemoryFdPropertiesKHR"));
    }

    VkCommandPoolCreateInfo pci = {};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = 0;
    return check(vkCreateCommandPool(d->device, &pci, nullptr, &d->pool), "vkCreateCommandPool");
}

// Destroys what createDevice made, whether it finished or not.
void destroyDevice(Device* d) {
    if (d->device) {
        vkDeviceWaitIdle(d->device);
        if (d->pool) vkDestroyCommandPool(d->device, d->pool, nullptr);
        vkDestroyDevice(d->device, nullptr);
    }
    if (d->instance) vkDestroyInstance(d->instance, nullptr);
    *d = Device();
}

bool findMemoryType(const Device& d, uint32_t bits, VkMemoryPropertyFlags want, uint32_t* index) {
    for (uint32_t i = 0; i < d.memory.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (d.memory.memoryTypes[i].propertyFlags & want) == want) {
            *index = i;
            return true;
        }
    }
    return false;
}

// Writes the pattern through the CPU mapping, row by row at the stride gralloc reports.
bool fill(AHardwareBuffer* buffer, const AHardwareBuffer_Desc& desc, uint32_t bytesPerPixel) {
    void* mapped = nullptr;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &mapped) != 0 ||
        !mapped) {
        printf("    AHardwareBuffer_lock failed\n");
        return false;
    }
    uint8_t* bytes = static_cast<uint8_t*>(mapped);
    memset(bytes, 0xee, static_cast<size_t>(desc.stride) * desc.height * bytesPerPixel);
    for (uint32_t y = 0; y < desc.height; y++) {
        for (uint32_t x = 0; x < desc.width; x++) {
            for (uint32_t b = 0; b < bytesPerPixel; b++) {
                bytes[(static_cast<size_t>(y) * desc.stride + x) * bytesPerPixel + b] = pattern(x, y, b);
            }
        }
    }
    AHardwareBuffer_unlock(buffer, nullptr);
    return true;
}

// Rewrites a rectangle through a second CPU lock of just that rectangle.
bool rewrite(AHardwareBuffer* buffer, const AHardwareBuffer_Desc& desc, uint32_t bytesPerPixel,
             const Update& update) {
    const ARect rect = {static_cast<int32_t>(update.x), static_cast<int32_t>(update.y),
                        static_cast<int32_t>(update.x + update.width),
                        static_cast<int32_t>(update.y + update.height)};
    void* mapped = nullptr;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, &rect, &mapped) != 0 || !mapped) {
        printf("    AHardwareBuffer_lock of a rectangle failed\n");
        return false;
    }
    uint8_t* bytes = static_cast<uint8_t*>(mapped);
    for (uint32_t y = update.y; y < update.y + update.height; y++) {
        for (uint32_t x = update.x; x < update.x + update.width; x++) {
            for (uint32_t b = 0; b < bytesPerPixel; b++) {
                bytes[(static_cast<size_t>(y) * desc.stride + x) * bytesPerPixel + b] =
                        expected(update, x, y, b);
            }
        }
    }
    AHardwareBuffer_unlock(buffer, nullptr);
    return true;
}

// Imports the buffer, copies its image into a host-visible buffer and compares it with the pattern
// as the update left it.
bool importAndRead(const Device& d, AHardwareBuffer* buffer, const AHardwareBuffer_Desc& desc,
                   uint32_t bytesPerPixel, const Update& update) {
    VkAndroidHardwareBufferFormatPropertiesANDROID format = {};
    format.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID props = {};
    props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    props.pNext = &format;
    if (!check(d.getProperties(d.device, buffer, &props), "vkGetAndroidHardwareBufferPropertiesANDROID")) {
        return false;
    }
    if (format.format == VK_FORMAT_UNDEFINED) {
        printf("    the buffer has only an external format\n");
        return false;
    }

    VkExternalMemoryImageCreateInfo external = {};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &external;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format.format;
    ici.extent = {desc.width, desc.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool ok = false;

    do {
        if (!check(vkCreateImage(d.device, &ici, nullptr, &image), "vkCreateImage")) break;

        VkImportAndroidHardwareBufferInfoANDROID import = {};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
        import.buffer = buffer;
        VkMemoryDedicatedAllocateInfo dedicated = {};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.pNext = &import;
        dedicated.image = image;
        uint32_t type = 0;
        if (!findMemoryType(d, props.memoryTypeBits, 0, &type)) {
            printf("    no memory type for the buffer\n");
            break;
        }
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext = &dedicated;
        mai.allocationSize = props.allocationSize;
        mai.memoryTypeIndex = type;
        if (!check(vkAllocateMemory(d.device, &mai, nullptr, &imageMemory), "vkAllocateMemory")) break;
        if (!check(vkBindImageMemory(d.device, image, imageMemory, 0), "vkBindImageMemory")) break;

        const VkDeviceSize readbackSize = static_cast<VkDeviceSize>(desc.width) * desc.height * bytesPerPixel;
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = readbackSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check(vkCreateBuffer(d.device, &bci, nullptr, &readback), "vkCreateBuffer")) break;
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(d.device, readback, &requirements);
        VkMemoryAllocateInfo rai = {};
        rai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        rai.allocationSize = requirements.size;
        if (!findMemoryType(d, requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &rai.memoryTypeIndex)) {
            printf("    no host-visible memory type\n");
            break;
        }
        if (!check(vkAllocateMemory(d.device, &rai, nullptr, &readbackMemory), "vkAllocateMemory (readback)")) break;
        if (!check(vkBindBufferMemory(d.device, readback, readbackMemory, 0), "vkBindBufferMemory")) break;

        VkCommandBufferAllocateInfo cai = {};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = d.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(d.device, &cai, &commands), "vkAllocateCommandBuffers")) break;
        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!check(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer")) break;

        // Take the image from the foreign owner without discarding what the CPU wrote.
        VkImageMemoryBarrier acquire = {};
        acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acquire.srcAccessMask = 0;
        acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        acquire.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        acquire.dstQueueFamilyIndex = 0;
        acquire.image = image;
        acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &acquire);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {desc.width, desc.height, 1};
        vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
        VkBufferMemoryBarrier visible = {};
        visible.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        visible.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        visible.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        visible.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        visible.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        visible.buffer = readback;
        visible.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                             &visible, 0, nullptr);
        if (!check(vkEndCommandBuffer(commands), "vkEndCommandBuffer")) break;

        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!check(vkCreateFence(d.device, &fci, nullptr, &fence), "vkCreateFence")) break;
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        if (!check(vkQueueSubmit(d.queue, 1, &submit, fence), "vkQueueSubmit")) break;
        if (!check(vkWaitForFences(d.device, 1, &fence, VK_TRUE, 30ull * 1000 * 1000 * 1000), "vkWaitForFences")) {
            break;
        }

        void* mapped = nullptr;
        if (!check(vkMapMemory(d.device, readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory")) break;
        const uint8_t* bytes = static_cast<const uint8_t*>(mapped);
        uint64_t wrong = 0;
        uint32_t firstX = 0, firstY = 0;
        uint8_t firstGot = 0, firstWant = 0;
        for (uint32_t y = 0; y < desc.height; y++) {
            for (uint32_t x = 0; x < desc.width; x++) {
                for (uint32_t b = 0; b < bytesPerPixel; b++) {
                    if (bytes[(static_cast<size_t>(y) * desc.width + x) * bytesPerPixel + b] != expected(update, x, y, b)) {
                        if (!wrong) {
                            firstX = x;
                            firstY = y;
                            firstGot = bytes[(static_cast<size_t>(y) * desc.width + x) * bytesPerPixel + b];
                            firstWant = expected(update, x, y, b);
                        }
                        wrong++;
                    }
                }
            }
        }
        vkUnmapMemory(d.device, readbackMemory);
        if (wrong) {
            printf("    %" PRIu64 " bytes read back differ from what the CPU wrote, first at (%u,%u): read %u, wrote %u\n",
                   wrong, firstX, firstY, firstGot, firstWant);
            break;
        }
        ok = true;
    } while (false);

    if (fence) vkDestroyFence(d.device, fence, nullptr);
    if (commands) vkFreeCommandBuffers(d.device, d.pool, 1, &commands);
    if (readback) vkDestroyBuffer(d.device, readback, nullptr);
    if (readbackMemory) vkFreeMemory(d.device, readbackMemory, nullptr);
    if (image) vkDestroyImage(d.device, image, nullptr);
    if (imageMemory) vkFreeMemory(d.device, imageMemory, nullptr);
    return ok;
}

// ---- Memory shared as a file descriptor ----------------------------------------------------------

struct FdType {
    VkExternalMemoryHandleTypeFlagBits type;
    const char* name;
};

const FdType kFdTypes[] = {
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, "opaque-fd"},
        {VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, "dma-buf"},
};

// A size that is not a multiple of a page, and one of several pages.
const VkDeviceSize kFdSizes[] = {65540, 4u << 20};

// A byte of one writer's pattern.
uint8_t fdByte(uint32_t seed, VkDeviceSize i) {
    return static_cast<uint8_t>(i * 131 + (i >> 8) * 17 + seed * 37 + 1);
}

// What byte i of a buffer of `size` bytes holds once the stage's writers are done. Stage 0: the Vulkan mapping filled
// all of it (pattern 1). Stage 1: a mapping of the fd rewrote the upper half (2). Stage 2: another process rewrote the
// last quarter (3). Stage 3 is a case of its own, from stage 0: another instance rewrote the first quarter (4).
uint8_t fdExpected(int stage, VkDeviceSize size, VkDeviceSize i) {
    if (stage == 3) return i < size / 4 ? fdByte(4, i) : fdByte(1, i);
    if (stage >= 2 && i >= size - size / 4) return fdByte(3, i);
    if (stage >= 1 && i >= size / 2) return fdByte(2, i);
    return fdByte(1, i);
}

void fdFill(uint8_t* bytes, VkDeviceSize from, VkDeviceSize to, uint32_t seed) {
    for (VkDeviceSize i = from; i < to; i++) bytes[i] = fdByte(seed, i);
}

// How many of the first `size` bytes are not what the stage expects, and the first of them.
uint64_t fdWrong(const uint8_t* bytes, int stage, VkDeviceSize size, VkDeviceSize* first) {
    uint64_t wrong = 0;
    for (VkDeviceSize i = 0; i < size; i++) {
        if (bytes[i] != fdExpected(stage, size, i)) {
            if (!wrong) *first = i;
            wrong++;
        }
    }
    return wrong;
}

bool fdVerify(const uint8_t* bytes, int stage, VkDeviceSize size, const char* what) {
    VkDeviceSize first = 0;
    const uint64_t wrong = fdWrong(bytes, stage, size, &first);
    if (!wrong) return true;
    printf("    %s: %" PRIu64 " of %" PRIu64 " bytes differ, first at %" PRIu64 ": read %u, wrote %u\n", what, wrong,
           static_cast<uint64_t>(size), static_cast<uint64_t>(first), bytes[first], fdExpected(stage, size, first));
    return false;
}

// How long a process the tool started may take before it is killed. The child's own alarm is shorter, so a hang in it
// reads as a signal and a hang the alarm cannot end is still bounded here.
constexpr unsigned kChildSeconds = 60;
constexpr int kWaitSeconds = 90;

// Another process: it maps the fd, checks what the others wrote and writes the last quarter. It runs in the child of
// a fork, so it maps, compares and exits, and is killed if it does not finish.
[[noreturn]] void fdChild(int fd, VkDeviceSize size, size_t mapSize) {
    alarm(kChildSeconds);
    void* view = mmap(nullptr, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) _exit(10);
    uint8_t* bytes = static_cast<uint8_t*>(view);
    VkDeviceSize first = 0;
    if (fdWrong(bytes, 1, size, &first)) _exit(11);
    fdFill(bytes, size - size / 4, size, 3);
    munmap(view, mapSize);
    _exit(0);
}

// What a status of the process from fdChild means.
const char* forkedMeaning(int status) {
    switch (status) {
        case 10: return "it could not map the fd";
        case 11: return "it read what the others wrote wrongly";
        default: return "unknown";
    }
}

// Waits for a process the tool started for at most kWaitSeconds and kills one that does not finish. True when it exited
// with status 0; otherwise a line says how it ended.
bool finishProcess(pid_t pid, const char* who, const char* (*meaning)(int)) {
    int status = 0;
    pid_t done = 0;
    for (int tick = 0; tick <= kWaitSeconds * 100; tick++) {
        done = waitpid(pid, &status, WNOHANG);
        if (done != 0 && !(done < 0 && errno == EINTR)) break;
        done = 0;
        usleep(10 * 1000);
    }
    if (done == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        printf("    %s did not finish in %d s and was killed\n", who, kWaitSeconds);
        return false;
    }
    if (done < 0) {
        printf("    waitpid failed: %s\n", strerror(errno));
        return false;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
    if (WIFEXITED(status)) {
        printf("    %s failed with %d (%s)\n", who, WEXITSTATUS(status), meaning(WEXITSTATUS(status)));
    } else {
        printf("    %s was killed by signal %d\n", who, WTERMSIG(status));
    }
    return false;
}

// A buffer bound to its own host-visible, coherent memory, mapped.
struct Blob {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkMemoryRequirements requirements = {};
};

void destroyBlob(const Device& d, Blob* blob) {
    if (blob->mapped) vkUnmapMemory(d.device, blob->memory);
    if (blob->buffer) vkDestroyBuffer(d.device, blob->buffer, nullptr);
    if (blob->memory) vkFreeMemory(d.device, blob->memory, nullptr);
    *blob = Blob();
}

// Creates the buffer and its memory: allocated for export as `type` when importFd is negative, imported from importFd
// otherwise. A successful import takes the fd (`consumed`). The memory type is the first host-visible, coherent one the
// buffer and `typeMask` allow (an import on another device passes the types the fd is compatible with).
bool createBlob(const Device& d, VkDeviceSize size, VkExternalMemoryHandleTypeFlagBits type, int importFd,
                uint32_t typeMask, uint32_t* typeIndex, bool* consumed, Blob* blob) {
    VkExternalMemoryBufferCreateInfo external = {};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    external.handleTypes = type;
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext = &external;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateBuffer(d.device, &bci, nullptr, &blob->buffer), "vkCreateBuffer")) return false;
    vkGetBufferMemoryRequirements(d.device, blob->buffer, &blob->requirements);
    if (!findMemoryType(d, blob->requirements.memoryTypeBits & typeMask,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, typeIndex)) {
        printf("    no host-visible memory type (buffer types %#x, fd types %#x)\n",
               blob->requirements.memoryTypeBits, typeMask);
        return false;
    }

    VkMemoryDedicatedAllocateInfo dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.buffer = blob->buffer;
    VkExportMemoryAllocateInfo exported = {};
    exported.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exported.handleTypes = type;
    VkImportMemoryFdInfoKHR imported = {};
    imported.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    imported.handleType = type;
    imported.fd = importFd;
    dedicated.pNext = importFd < 0 ? static_cast<const void*>(&exported) : static_cast<const void*>(&imported);
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &dedicated;
    mai.allocationSize = blob->requirements.size;
    mai.memoryTypeIndex = *typeIndex;
    if (!check(vkAllocateMemory(d.device, &mai, nullptr, &blob->memory), importFd < 0 ? "vkAllocateMemory (export)"
                                                                                      : "vkAllocateMemory (import)")) {
        return false;
    }
    if (importFd >= 0) *consumed = true;
    if (!check(vkBindBufferMemory(d.device, blob->buffer, blob->memory, 0), "vkBindBufferMemory")) return false;
    return check(vkMapMemory(d.device, blob->memory, 0, VK_WHOLE_SIZE, 0, &blob->mapped), "vkMapMemory");
}

// Copies `source` to a host-visible buffer with the GPU and compares the copy with the stage's pattern.
bool gpuRead(const Device& d, VkBuffer source, VkDeviceSize size, int stage, const char* what) {
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool ok = false;

    do {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check(vkCreateBuffer(d.device, &bci, nullptr, &readback), "vkCreateBuffer (readback)")) break;
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(d.device, readback, &requirements);
        VkMemoryAllocateInfo rai = {};
        rai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        rai.allocationSize = requirements.size;
        if (!findMemoryType(d, requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &rai.memoryTypeIndex)) {
            printf("    no host-visible memory type\n");
            break;
        }
        if (!check(vkAllocateMemory(d.device, &rai, nullptr, &readbackMemory), "vkAllocateMemory (readback)")) break;
        if (!check(vkBindBufferMemory(d.device, readback, readbackMemory, 0), "vkBindBufferMemory (readback)")) break;

        VkCommandBufferAllocateInfo cai = {};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = d.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(d.device, &cai, &commands), "vkAllocateCommandBuffers")) break;
        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!check(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer")) break;

        VkMemoryBarrier written = {};
        written.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        written.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        written.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &written, 0,
                             nullptr, 0, nullptr);
        const VkBufferCopy region = {0, 0, size};
        vkCmdCopyBuffer(commands, source, readback, 1, &region);
        VkBufferMemoryBarrier visible = {};
        visible.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        visible.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        visible.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        visible.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        visible.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        visible.buffer = readback;
        visible.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                             &visible, 0, nullptr);
        if (!check(vkEndCommandBuffer(commands), "vkEndCommandBuffer")) break;

        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!check(vkCreateFence(d.device, &fci, nullptr, &fence), "vkCreateFence")) break;
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        if (!check(vkQueueSubmit(d.queue, 1, &submit, fence), "vkQueueSubmit")) break;
        if (!check(vkWaitForFences(d.device, 1, &fence, VK_TRUE, 30ull * 1000 * 1000 * 1000), "vkWaitForFences")) {
            break;
        }

        void* mapped = nullptr;
        if (!check(vkMapMemory(d.device, readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory (readback)")) {
            break;
        }
        ok = fdVerify(static_cast<const uint8_t*>(mapped), stage, size, what);
        vkUnmapMemory(d.device, readbackMemory);
    } while (false);

    if (fence) vkDestroyFence(d.device, fence, nullptr);
    if (commands) vkFreeCommandBuffers(d.device, d.pool, 1, &commands);
    if (readback) vkDestroyBuffer(d.device, readback, nullptr);
    if (readbackMemory) vkFreeMemory(d.device, readbackMemory, nullptr);
    return ok;
}

// What the driver says about sharing a buffer's memory as this fd type: it exports and imports it, names the type
// among the types it is compatible with, and leaves the hardware buffer out of them (a hardware buffer is not a blob).
bool fdProperties(const Device& d, const FdType& fd) {
    VkPhysicalDeviceExternalBufferInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.handleType = fd.type;
    VkExternalBufferProperties props = {};
    props.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    vkGetPhysicalDeviceExternalBufferProperties(d.physical, &info, &props);
    const VkExternalMemoryProperties& memory = props.externalMemoryProperties;
    printf("    features %#x, compatible handle types %#x, exportable from %#x\n", memory.externalMemoryFeatures,
           memory.compatibleHandleTypes, memory.exportFromImportedHandleTypes);
    bool ok = true;
    if (!(memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ||
        !(memory.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        printf("    the type is not both exportable and importable\n");
        ok = false;
    }
    if ((memory.compatibleHandleTypes & fd.type) != static_cast<VkExternalMemoryHandleTypeFlags>(fd.type)) {
        printf("    the type is not compatible with itself\n");
        ok = false;
    }
    if ((memory.compatibleHandleTypes | memory.exportFromImportedHandleTypes) &
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) {
        printf("    the type is described as compatible with a hardware buffer\n");
        ok = false;
    }
    return ok;
}

// A fd of the memory, one more each time it is called.
bool exportFd(const Device& d, const FdType& fd, VkDeviceMemory memory, int* out, const char* what) {
    VkMemoryGetFdInfoKHR get = {};
    get.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    get.memory = memory;
    get.handleType = fd.type;
    if (!check(d.getMemoryFd(d.device, &get, out), what)) return false;
    if (*out < 0) {
        printf("    vkGetMemoryFdKHR returned the fd %d\n", *out);
        *out = -1;
        return false;
    }
    return true;
}

// The fd cases of another instance ----------------------------------------------------------------------

constexpr const char* kChildFlag = "--memory-fd-child";

// The exit statuses of the other instance.
enum ChildExit {
    kChildNoDevice = 10,
    kChildNoImport = 11,
    kChildWrongBytes = 12,
    kChildWrongRead = 13,
    kChildWrongWrite = 14,
    kChildNotReleased = 15,
    kChildBadArguments = 16,
    kChildNotStarted = 127,
};

const char* childMeaning(int status) {
    switch (status) {
        case kChildNoDevice: return "it could not create an instance and a device";
        case kChildNoImport: return "it could not import the fd";
        case kChildWrongBytes: return "its mapping of the imported memory held the wrong bytes";
        case kChildWrongRead: return "its GPU read the wrong bytes";
        case kChildWrongWrite: return "its GPU did not read what it wrote";
        case kChildNotReleased: return "it was never released";
        case kChildBadArguments: return "it was started with arguments it does not know";
        case kChildNotStarted: return "it could not be started";
        default: return "unknown";
    }
}

const FdType* findFdType(const char* name) {
    for (const FdType& fd : kFdTypes) {
        if (!strcmp(fd.name, name)) return &fd;
    }
    return nullptr;
}

// The other instance, run by main for --memory-fd-child TYPE FD SIZE STAGE WAIT. It is this tool started anew with exec,
// so it creates an instance and a device of its own, and it holds the one fd it is given. With WAIT it parks on its
// standard input, a pipe, until the starter has freed what it frees, and only then touches the fd's memory. It imports
// the fd, checks the memory is at STAGE through its Vulkan mapping and through the GPU, rewrites the first quarter and
// has the GPU read the memory at stage 3. Its exit status is 0 or a ChildExit.
int otherInstance(int argc, char** argv) {
    alarm(kChildSeconds);
    if (argc != 7) return kChildBadArguments;
    const FdType* fd = findFdType(argv[2]);
    const int memoryFd = atoi(argv[3]);
    const VkDeviceSize size = strtoull(argv[4], nullptr, 10);
    const int stage = atoi(argv[5]);
    const bool wait = atoi(argv[6]) != 0;
    if (!fd || memoryFd < 3 || size == 0) return kChildBadArguments;
    if (wait) {
        char go = 0;
        ssize_t got;
        do {
            got = read(0, &go, 1);
        } while (got < 0 && errno == EINTR);
        if (got != 1) {
            printf("    the pipe that releases the other instance closed without a release\n");
            return kChildNotReleased;
        }
    }

    Device d;
    if (!createDevice(&d)) return kChildNoDevice;
    if (!d.fdOffered || !d.getMemoryFdProperties ||
        (fd->type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT && !d.dmaBufOffered)) {
        printf("    the fd extensions are not offered to the other instance\n");
        return kChildNoDevice;
    }
    uint32_t types = ~0u;
    if (fd->type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
        VkMemoryFdPropertiesKHR props = {};
        props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        if (!check(d.getMemoryFdProperties(d.device, fd->type, memoryFd, &props), "vkGetMemoryFdPropertiesKHR")) {
            return kChildNoImport;
        }
        types = props.memoryTypeBits;
    }
    Blob imported;
    uint32_t typeIndex = 0;
    bool consumed = false;
    if (!createBlob(d, size, fd->type, memoryFd, types, &typeIndex, &consumed, &imported)) return kChildNoImport;
    uint8_t* bytes = static_cast<uint8_t*>(imported.mapped);
    if (!fdVerify(bytes, stage, size, "the other instance's mapping of the imported memory")) return kChildWrongBytes;
    if (!gpuRead(d, imported.buffer, size, stage, "the other instance's GPU read of the imported memory")) {
        return kChildWrongRead;
    }
    fdFill(bytes, 0, size / 4, 4);
    if (!gpuRead(d, imported.buffer, size, 3, "the other instance's GPU read after it wrote the first quarter")) {
        return kChildWrongWrite;
    }
    destroyBlob(d, &imported);
    destroyDevice(&d);
    return 0;
}

// The path the tool starts itself from.
char gSelfPath[4096];

void findSelf(const char* argv0) {
    const ssize_t length = readlink("/proc/self/exe", gSelfPath, sizeof(gSelfPath) - 1);
    if (length > 0) {
        gSelfPath[length] = '\0';
    } else {
        snprintf(gSelfPath, sizeof(gSelfPath), "%s", argv0);
    }
}

// The other instance as its starter sees it: its process, and the end of its standard input pipe.
struct OtherInstance {
    pid_t pid = -1;
    int release = -1;
};

// Starts the other instance with fork and exec. It holds the standard streams and `memoryFd`, and nothing else of this
// process: not a mapping, not a device, not the render node. With `wait` it does not touch the memory until released.
bool startOtherInstance(const FdType& fd, int memoryFd, VkDeviceSize size, int stage, bool wait,
                        OtherInstance* other) {
    char fdText[16], sizeText[32], stageText[16];
    snprintf(fdText, sizeof(fdText), "%d", memoryFd);
    snprintf(sizeText, sizeof(sizeText), "%" PRIu64, static_cast<uint64_t>(size));
    snprintf(stageText, sizeof(stageText), "%d", stage);
    char* args[] = {gSelfPath,
                    const_cast<char*>(kChildFlag),
                    const_cast<char*>(fd.name),
                    fdText,
                    sizeText,
                    stageText,
                    const_cast<char*>(wait ? "1" : "0"),
                    nullptr};
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit < 64) limit = 1024;
    if (limit > 4096) limit = 4096;
    int pipes[2];
    if (pipe(pipes) != 0) {
        printf("    pipe failed: %s\n", strerror(errno));
        return false;
    }
    fflush(stdout);
    const pid_t pid = fork();
    if (pid < 0) {
        printf("    fork failed: %s\n", strerror(errno));
        close(pipes[0]);
        close(pipes[1]);
        return false;
    }
    if (pid == 0) {
        if (dup2(pipes[0], 0) < 0) _exit(kChildNotStarted);
        // The driver exports a memory fd close-on-exec; exec would close it, so the child keeps it across exec.
        if (fcntl(memoryFd, F_SETFD, 0) != 0) _exit(kChildNotStarted);
        for (int i = 3; i < limit; i++) {
            if (i != memoryFd) close(i);
        }
        execv(gSelfPath, args);
        _exit(kChildNotStarted);
    }
    close(pipes[0]);
    if (wait) {
        other->release = pipes[1];
    } else {
        close(pipes[1]);
    }
    other->pid = pid;
    return true;
}

// Lets a waiting instance go. False when it is already gone.
bool releaseOtherInstance(OtherInstance* other) {
    if (other->release < 0) return true;
    const char go = 'g';
    const ssize_t put = write(other->release, &go, 1);
    const int why = errno;
    close(other->release);
    other->release = -1;
    if (put != 1) {
        printf("    the other instance had already exited (%s)\n", put < 0 ? strerror(why) : "short write");
        return false;
    }
    return true;
}

bool finishOtherInstance(OtherInstance* other) {
    if (other->release >= 0) {
        close(other->release);
        other->release = -1;
    }
    if (other->pid < 0) return false;
    const pid_t pid = other->pid;
    other->pid = -1;
    return finishProcess(pid, "the other instance", childMeaning);
}

// Ends an instance a failed case leaves behind.
void abandonOtherInstance(OtherInstance* other) {
    if (other->release >= 0) close(other->release);
    other->release = -1;
    if (other->pid < 0) return;
    kill(other->pid, SIGKILL);
    int status = 0;
    waitpid(other->pid, &status, 0);
    other->pid = -1;
}

// Host-visible memory shared as a fd of this type, through the whole path an application uses: the Vulkan mapping
// fills it, an export gives a fd, a mapping of the fd sees it and rewrites half of it, another process rewrites a
// quarter, the GPU reads it, and an import of a second fd of the memory is read by the GPU and the CPU.
bool fdCase(const Device& d, const FdType& fd, VkDeviceSize size) {
    Blob exported, imported;
    int first = -1, second = -1;
    bool consumed = false;
    void* view = MAP_FAILED;
    size_t viewSize = 0;
    uint32_t typeIndex = 0;
    bool ok = false;

    do {
        if (!createBlob(d, size, fd.type, -1, ~0u, &typeIndex, &consumed, &exported)) break;
        uint8_t* bytes = static_cast<uint8_t*>(exported.mapped);
        fdFill(bytes, 0, size, 1);

        if (!exportFd(d, fd, exported.memory, &first, "vkGetMemoryFdKHR")) break;
        if (fd.type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
            VkMemoryFdPropertiesKHR props = {};
            props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
            if (!check(d.getMemoryFdProperties(d.device, fd.type, first, &props), "vkGetMemoryFdPropertiesKHR")) break;
            if (!(props.memoryTypeBits & (1u << typeIndex))) {
                printf("    the dma-buf's memory types %#x lack the exported type %u\n", props.memoryTypeBits,
                       typeIndex);
                break;
            }
        }

        // The fd is a view of the memory's pages: mapped, it holds what the Vulkan mapping holds.
        const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        viewSize = (static_cast<size_t>(exported.requirements.size) + page - 1) / page * page;
        view = mmap(nullptr, viewSize, PROT_READ | PROT_WRITE, MAP_SHARED, first, 0);
        if (view == MAP_FAILED) {
            printf("    mmap of the fd (%zu bytes) failed: %s\n", viewSize, strerror(errno));
            break;
        }
        uint8_t* seen = static_cast<uint8_t*>(view);
        if (!fdVerify(seen, 0, size, "the mapping of the fd after the Vulkan mapping wrote")) break;
        fdFill(seen, size / 2, size, 2);
        if (!fdVerify(bytes, 1, size, "the Vulkan mapping after the mapping of the fd wrote")) break;

        const pid_t child = fork();
        if (child < 0) {
            printf("    fork failed: %s\n", strerror(errno));
            break;
        }
        if (child == 0) fdChild(first, size, viewSize);
        if (!finishProcess(child, "the other process", forkedMeaning)) break;
        if (!fdVerify(bytes, 2, size, "the Vulkan mapping after another process wrote through the fd")) break;
        if (!fdVerify(seen, 2, size, "the mapping of the fd after another process wrote through it")) break;
        if (!gpuRead(d, exported.buffer, size, 2, "the GPU's read of the exported memory")) break;

        // A second fd of the same memory, imported as a memory object of its own.
        if (!exportFd(d, fd, exported.memory, &second, "vkGetMemoryFdKHR (second)")) break;
        const bool imports = createBlob(d, size, fd.type, second, ~0u, &typeIndex, &consumed, &imported);
        if (consumed) second = -1;  // the driver owns it from here
        if (!imports) break;
        if (!fdVerify(static_cast<const uint8_t*>(imported.mapped), 2, size, "the mapping of the imported memory")) {
            break;
        }
        if (!gpuRead(d, imported.buffer, size, 2, "the GPU's read of the imported memory")) break;
        ok = true;
    } while (false);

    destroyBlob(d, &imported);
    destroyBlob(d, &exported);
    if (view != MAP_FAILED) munmap(view, viewSize);
    if (second >= 0) close(second);
    if (first >= 0) close(first);
    return ok;
}

// Memory handed to another instance of the tool: the fd is all that is handed over (see startOtherInstance), and the
// other instance creates an instance and a device of its own. It imports the fd, maps it, has the GPU read it and
// rewrites the first quarter. With `freed` this process frees the exporting memory and closes its own fd after it has
// started the other instance and before it releases it, so only the other instance's fd keeps the memory; otherwise this
// process reads the quarter the other instance wrote, through its Vulkan mapping and the GPU.
bool fdOtherInstanceCase(const Device& d, const FdType& fd, VkDeviceSize size, bool freed) {
    Blob exported;
    OtherInstance other;
    int handed = -1;
    bool consumed = false;
    uint32_t typeIndex = 0;
    bool ok = false;

    do {
        if (!createBlob(d, size, fd.type, -1, ~0u, &typeIndex, &consumed, &exported)) break;
        uint8_t* bytes = static_cast<uint8_t*>(exported.mapped);
        fdFill(bytes, 0, size, 1);
        if (!exportFd(d, fd, exported.memory, &handed, "vkGetMemoryFdKHR")) break;
        if (!startOtherInstance(fd, handed, size, 0, freed, &other)) break;
        close(handed);  // the other instance holds its own copy of the fd
        handed = -1;
        if (freed) {
            destroyBlob(d, &exported);
            if (!releaseOtherInstance(&other)) {
                finishOtherInstance(&other);
                break;
            }
        }
        if (!finishOtherInstance(&other)) break;
        if (!freed) {
            if (!fdVerify(bytes, 3, size, "the Vulkan mapping after the other instance wrote")) break;
            if (!gpuRead(d, exported.buffer, size, 3, "the GPU's read of the exported memory after the other instance wrote")) {
                break;
            }
        }
        ok = true;
    } while (false);

    abandonOtherInstance(&other);
    destroyBlob(d, &exported);
    if (handed >= 0) close(handed);
    return ok;
}

// A fd of exported memory that outlives both devices that held the memory: one device exports it, a second device
// imports it and rewrites the first quarter, both are destroyed with their instances, and the fd and a mapping of it made
// before, and another made after, still hold the bytes.
bool fdOutlivesDevicesCase(const FdType& fd, VkDeviceSize size) {
    Device exporter, importer;
    Blob exported, imported;
    int kept = -1, handed = -1;
    void* view = MAP_FAILED;
    void* later = MAP_FAILED;
    size_t viewSize = 0;
    uint32_t typeIndex = 0;
    bool consumed = false;
    bool ok = false;

    do {
        if (!createDevice(&exporter) || !createDevice(&importer)) break;
        if (!exporter.fdOffered || !importer.fdOffered || !exporter.getMemoryFd || !importer.getMemoryFdProperties ||
            (fd.type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT && !importer.dmaBufOffered)) {
            printf("    the fd extensions are not offered to the new devices\n");
            break;
        }
        if (!createBlob(exporter, size, fd.type, -1, ~0u, &typeIndex, &consumed, &exported)) break;
        fdFill(static_cast<uint8_t*>(exported.mapped), 0, size, 1);
        if (!exportFd(exporter, fd, exported.memory, &kept, "vkGetMemoryFdKHR")) break;
        if (!exportFd(exporter, fd, exported.memory, &handed, "vkGetMemoryFdKHR (second)")) break;

        const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        viewSize = (static_cast<size_t>(exported.requirements.size) + page - 1) / page * page;
        view = mmap(nullptr, viewSize, PROT_READ | PROT_WRITE, MAP_SHARED, kept, 0);
        if (view == MAP_FAILED) {
            printf("    mmap of the fd (%zu bytes) failed: %s\n", viewSize, strerror(errno));
            break;
        }
        if (!fdVerify(static_cast<const uint8_t*>(view), 0, size, "the mapping of the fd before the second device")) break;

        uint32_t types = ~0u;
        if (fd.type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
            VkMemoryFdPropertiesKHR props = {};
            props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
            if (!check(importer.getMemoryFdProperties(importer.device, fd.type, handed, &props),
                       "vkGetMemoryFdPropertiesKHR")) {
                break;
            }
            types = props.memoryTypeBits;
        }
        const bool imports = createBlob(importer, size, fd.type, handed, types, &typeIndex, &consumed, &imported);
        if (consumed) handed = -1;  // the driver owns it from here
        if (!imports) break;
        uint8_t* written = static_cast<uint8_t*>(imported.mapped);
        if (!fdVerify(written, 0, size, "the second device's mapping of the imported memory")) break;
        fdFill(written, 0, size / 4, 4);
        if (!fdVerify(static_cast<const uint8_t*>(view), 3, size, "the mapping of the fd after the second device wrote")) {
            break;
        }

        destroyBlob(importer, &imported);
        destroyDevice(&importer);
        destroyBlob(exporter, &exported);
        destroyDevice(&exporter);

        if (!fdVerify(static_cast<const uint8_t*>(view), 3, size, "the mapping of the fd after both devices were destroyed")) {
            break;
        }
        later = mmap(nullptr, viewSize, PROT_READ | PROT_WRITE, MAP_SHARED, kept, 0);
        if (later == MAP_FAILED) {
            printf("    mmap of the fd after both devices were destroyed failed: %s\n", strerror(errno));
            break;
        }
        if (!fdVerify(static_cast<const uint8_t*>(later), 3, size, "a new mapping of the fd after both devices were destroyed")) {
            break;
        }
        ok = true;
    } while (false);

    destroyBlob(importer, &imported);
    destroyDevice(&importer);
    destroyBlob(exporter, &exported);
    destroyDevice(&exporter);
    if (later != MAP_FAILED) munmap(later, viewSize);
    if (view != MAP_FAILED) munmap(view, viewSize);
    if (handed >= 0) close(handed);
    if (kept >= 0) close(kept);
    return ok;
}

// The fd cases: the extensions are offered (and were enabled on the device), then each type's properties and each
// type shared at each size, with the cases of another instance.
void runMemoryFd(const Device& d, int* run, int* failed) {
    printf("memory-fd extension %s: %s\n", kFdExtension, d.fdOffered ? "offered" : "not offered");
    printf("memory-fd extension %s: %s\n", kDmaBufExtension, d.dmaBufOffered ? "offered" : "not offered");
    const bool entryPoints = d.getMemoryFd && d.getMemoryFdProperties;
    if (d.fdOffered && !entryPoints) printf("    vkGetMemoryFdKHR or vkGetMemoryFdPropertiesKHR is not exposed\n");
    const bool offered = d.fdOffered && d.dmaBufOffered && entryPoints;
    (*run)++;
    printf("  %s\n", offered ? "PASS" : "FAIL");
    if (!offered) (*failed)++;
    if (!d.fdOffered || !entryPoints) return;

    for (const FdType& fd : kFdTypes) {
        if (fd.type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT && !d.dmaBufOffered) continue;
        printf("memory-fd %s properties\n", fd.name);
        (*run)++;
        const bool good = fdProperties(d, fd);
        printf("  %s\n", good ? "PASS" : "FAIL");
        if (!good) (*failed)++;
        for (VkDeviceSize size : kFdSizes) {
            printf("memory-fd %s %" PRIu64 " bytes\n", fd.name, static_cast<uint64_t>(size));
            (*run)++;
            bool shared = fdCase(d, fd, size);
            printf("  %s\n", shared ? "PASS" : "FAIL");
            if (!shared) (*failed)++;

            printf("memory-fd %s %" PRIu64 " bytes, imported by another instance\n", fd.name,
                   static_cast<uint64_t>(size));
            (*run)++;
            shared = fdOtherInstanceCase(d, fd, size, false);
            printf("  %s\n", shared ? "PASS" : "FAIL");
            if (!shared) (*failed)++;

            printf("memory-fd %s %" PRIu64 " bytes, imported after the exporter was freed\n", fd.name,
                   static_cast<uint64_t>(size));
            (*run)++;
            shared = fdOtherInstanceCase(d, fd, size, true);
            printf("  %s\n", shared ? "PASS" : "FAIL");
            if (!shared) (*failed)++;

            printf("memory-fd %s %" PRIu64 " bytes, kept past both devices\n", fd.name, static_cast<uint64_t>(size));
            (*run)++;
            shared = fdOutlivesDevicesCase(fd, size);
            printf("  %s\n", shared ? "PASS" : "FAIL");
            if (!shared) (*failed)++;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    // A line is printed when it is done: the verdict of the hardware buffers must not be lost with a later crash, and an
    // instance started with fork must not carry a buffer of this one.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    // A write to the pipe of an instance that has died is an error to report, not a signal.
    signal(SIGPIPE, SIG_IGN);
    findSelf(argv[0]);
    if (argc > 1 && !strcmp(argv[1], kChildFlag)) return otherInstance(argc, argv);

    uint32_t widths[32];
    size_t widthCount = 0;
    for (int i = 1; i < argc && widthCount < sizeof(widths) / sizeof(widths[0]); i++) {
        widths[widthCount++] = static_cast<uint32_t>(strtoul(argv[i], nullptr, 0));
    }
    if (widthCount == 0) {
        for (uint32_t width : kDefaultWidths) widths[widthCount++] = width;
    }

    Device device;
    if (!createDevice(&device)) {
        printf("FAIL: no Vulkan device to import into\n");
        return 2;
    }

    int run = 0, failed = 0, skipped = 0;
    for (const FormatCase& format : kFormats) {
        for (size_t w = 0; w < widthCount; w++) {
            const uint32_t width = widths[w];
            AHardwareBuffer_Desc want = {};
            want.width = width;
            want.height = kHeight;
            want.layers = 1;
            want.format = format.ahbFormat;
            want.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                         AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
            AHardwareBuffer* buffer = nullptr;
            if (AHardwareBuffer_allocate(&want, &buffer) != 0 || !buffer) {
                printf("%-14s %4ux%-3u SKIP: the buffer cannot be allocated\n", format.name, width, kHeight);
                skipped++;
                continue;
            }
            AHardwareBuffer_Desc got;
            AHardwareBuffer_describe(buffer, &got);
            printf("%-14s %4ux%-3u stride %u (%u bytes)\n", format.name, width, kHeight, got.stride,
                   got.stride * format.bytesPerPixel);
            run++;
            // A rectangle off the left and top edges, so that a partial lock has to find where its first
            // row starts and where each row after it does.
            Update update = {0, 0, 0, 0};
            if (got.width > 4 && got.height > 6) {
                update.x = got.width / 5 + 1;
                update.y = 3;
                update.width = got.width - update.x - 1;
                update.height = got.height - 6;
            }
            bool ok = fill(buffer, got, format.bytesPerPixel) &&
                      (update.width == 0 || rewrite(buffer, got, format.bytesPerPixel, update)) &&
                      importAndRead(device, buffer, got, format.bytesPerPixel, update);
            printf("  %s\n", ok ? "PASS" : "FAIL");
            if (!ok) failed++;
            AHardwareBuffer_release(buffer);
        }
    }

    printf("hardware-buffer verdict: %s: %d cases imported, %d failed, %d not allocated\n", failed ? "FAIL" : "PASS",
           run, failed, skipped);

    int fdRun = 0, fdFailed = 0;
    runMemoryFd(device, &fdRun, &fdFailed);

    const bool bad = failed || fdFailed;
    printf("%s: %d cases imported, %d failed, %d not allocated; memory fd %d cases, %d failed\n",
           bad ? "FAIL" : "PASS", run, failed, skipped, fdRun, fdFailed);
    return bad ? 1 : 0;
}

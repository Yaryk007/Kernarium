// SPDX-License-Identifier: GPL-2.0-or-later

// A transfer hands Metal the Metal object of every buffer and image plane it
// names, and Apple's GPU drivers dereference a nil one: the host process ends
// inside the blit encoder. An image that was never bound, one whose bind the
// driver refused, and a buffer whose bind it refused have no Metal object, and
// a Venus guest can send a copy to any of them because its binds are
// asynchronous and always report success.
//
// The driver must instead refuse the command: vkEndCommandBuffer returns the
// error the command recorded, and the process lives. A failed bind reports its
// own result, in the return value and in VkBindMemoryStatus, and logs one line
// that says what Metal was asked for, because no other report names the
// reason; a successful bind logs nothing. The control copies show that the
// recording is right and that a bound image and buffer still copy.
//
// On a driver without the checks the first copy to an image without storage
// ends this process in the blit encoder, which fails the test.

#include "scratch_directory.hpp"
#include "vulkan_draw_test.hpp"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace {

using uml::gpu::test::check;
using uml::gpu::test::instance_function;
using uml::gpu::test::memory_type;
using uml::gpu::test::structure;
using uml::testing::require;

constexpr std::uint32_t extent = 64;
constexpr VkDeviceSize imageBytes = VkDeviceSize{extent} * extent * 4;
// An offset far past the end of any allocation the test makes.
constexpr VkDeviceSize kFarOffset = VkDeviceSize{1} << 32;

// The driver's own log, which Mesa writes to the file MESA_LOG_FILE names.
std::string logPath;

[[nodiscard]] std::size_t logLines(const char* needle)
{
    std::ifstream file(logPath);
    std::size_t count = 0;
    for (std::string line; std::getline(file, line);)
        count += line.find(needle) != std::string::npos ? 1 : 0;
    return count;
}

// The last line of the log that holds `needle`, or none.
[[nodiscard]] std::string lastLogLine(const char* needle)
{
    std::ifstream file(logPath);
    std::string found;
    for (std::string line; std::getline(file, line);) {
        if (line.find(needle) != std::string::npos)
            found = line;
    }
    return found;
}

struct Device {
    VkInstance instance {};
    VkPhysicalDevice physical {};
    VkDevice device {};
    VkQueue queue {};
    VkCommandPool pool {};
    VkCommandBuffer command {};
    VkPhysicalDeviceMemoryProperties memory {};
    PFN_vkGetDeviceProcAddr getDevice {};

    template <typename T>
    T function(const char* name) const
    {
        const auto result = reinterpret_cast<T>(getDevice(device, name));
        require(result != nullptr, name);
        return result;
    }

#define CALL(name, ...) function<PFN_vk##name>("vk" #name)(__VA_ARGS__)
#define CHECK(name, ...) check(CALL(name, __VA_ARGS__), "vk" #name)

    Device()
    {
        auto app = structure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        app.apiVersion = VK_API_VERSION_1_4;
        auto info = structure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        info.pApplicationInfo = &app;
        check(instance_function<PFN_vkCreateInstance>(nullptr, "vkCreateInstance")(
                  &info, nullptr, &instance),
            "create instance");
        std::uint32_t count = 1;
        check(instance_function<PFN_vkEnumeratePhysicalDevices>(
                  instance, "vkEnumeratePhysicalDevices")(instance, &count, &physical),
            "physical device");
        instance_function<PFN_vkGetPhysicalDeviceMemoryProperties>(
            instance, "vkGetPhysicalDeviceMemoryProperties")(physical, &memory);
        float priority = 1;
        auto queueInfo = structure<VkDeviceQueueCreateInfo>(
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        auto deviceInfo = structure<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        check(instance_function<PFN_vkCreateDevice>(instance, "vkCreateDevice")(
                  physical, &deviceInfo, nullptr, &device),
            "create device");
        getDevice = instance_function<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
        CALL(GetDeviceQueue, device, 0, 0, &queue);
        auto poolInfo = structure<VkCommandPoolCreateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        CHECK(CreateCommandPool, device, &poolInfo, nullptr, &pool);
        auto allocate = structure<VkCommandBufferAllocateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        CHECK(AllocateCommandBuffers, device, &allocate, &command);
    }

    ~Device()
    {
        if (device != VK_NULL_HANDLE) {
            (void)CALL(DeviceWaitIdle, device);
            CALL(DestroyCommandPool, device, pool, nullptr);
            CALL(DestroyDevice, device, nullptr);
        }
        if (instance != VK_NULL_HANDLE)
            instance_function<PFN_vkDestroyInstance>(instance, "vkDestroyInstance")(
                instance, nullptr);
    }

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] VkDeviceMemory allocate(
        const VkMemoryRequirements& requirements, VkMemoryPropertyFlags flags)
    {
        auto info = structure<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        info.allocationSize = requirements.size;
        const auto type = memory_type(memory, requirements.memoryTypeBits, flags);
        require(type.has_value(), "no memory type for the allocation");
        info.memoryTypeIndex = *type;
        VkDeviceMemory result {};
        CHECK(AllocateMemory, device, &info, nullptr, &result);
        return result;
    }

    // An image or buffer, its memory and whether to bind it, and where.
    struct Image {
        VkImage image {};
        VkDeviceMemory memory {};
        VkMemoryRequirements requirements {};
    };

    [[nodiscard]] Image makeImage()
    {
        auto info = structure<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {extent, extent, 1};
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        Image result;
        CHECK(CreateImage, device, &info, nullptr, &result.image);
        CALL(GetImageMemoryRequirements, device, result.image, &result.requirements);
        result.memory = allocate(result.requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        return result;
    }

    struct Buffer {
        VkBuffer buffer {};
        VkDeviceMemory memory {};
        VkMemoryRequirements requirements {};
    };

    [[nodiscard]] Buffer makeBuffer()
    {
        auto info = structure<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        info.size = imageBytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Buffer result;
        CHECK(CreateBuffer, device, &info, nullptr, &result.buffer);
        CALL(GetBufferMemoryRequirements, device, result.buffer, &result.requirements);
        result.memory = allocate(result.requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        return result;
    }

    // The result of one bind, as the call returns it and as the status
    // structure of the bind reports it.
    struct Bind {
        VkResult returned;
        VkResult reported;
    };

    [[nodiscard]] Bind bindImage(const Image& image, VkDeviceSize offset)
    {
        auto status = structure<VkBindMemoryStatus>(VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS);
        VkResult reported = VK_RESULT_MAX_ENUM;
        status.pResult = &reported;
        auto info = structure<VkBindImageMemoryInfo>(VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO);
        info.pNext = &status;
        info.image = image.image;
        info.memory = image.memory;
        info.memoryOffset = offset;
        return {CALL(BindImageMemory2, device, 1, &info), reported};
    }

    [[nodiscard]] Bind bindBuffer(const Buffer& buffer, VkDeviceSize offset)
    {
        auto status = structure<VkBindMemoryStatus>(VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS);
        VkResult reported = VK_RESULT_MAX_ENUM;
        status.pResult = &reported;
        auto info = structure<VkBindBufferMemoryInfo>(VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO);
        info.pNext = &status;
        info.buffer = buffer.buffer;
        info.memory = buffer.memory;
        info.memoryOffset = offset;
        return {CALL(BindBufferMemory2, device, 1, &info), reported};
    }

    // Records `commands` in the command buffer and returns what ending it
    // reports. The command buffer is reset afterwards.
    [[nodiscard]] VkResult record(const std::function<void(VkCommandBuffer)>& commands)
    {
        auto begin = structure<VkCommandBufferBeginInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        CHECK(BeginCommandBuffer, command, &begin);
        commands(command);
        const VkResult ended = CALL(EndCommandBuffer, command);
        CHECK(ResetCommandBuffer, command, 0);
        return ended;
    }

    void copyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer, VkImage image)
    {
        VkBufferImageCopy region {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent, extent, 1};
        CALL(CmdCopyBufferToImage, cmd, buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    }

    void copyImageToBuffer(VkCommandBuffer cmd, VkImage image, VkBuffer buffer)
    {
        VkBufferImageCopy region {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent, extent, 1};
        CALL(CmdCopyImageToBuffer, cmd, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
    }

    // Submits a copy of the buffer to the image and waits for it.
    void submitCopy(VkBuffer buffer, VkImage image)
    {
        auto begin = structure<VkCommandBufferBeginInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        CHECK(BeginCommandBuffer, command, &begin);
        copyBufferToImage(command, buffer, image);
        CHECK(EndCommandBuffer, command);
        auto submit = structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        CHECK(QueueSubmit, queue, 1, &submit, VK_NULL_HANDLE);
        CHECK(QueueWaitIdle, queue);
        CHECK(ResetCommandBuffer, command, 0);
    }

    void copyBuffer(VkCommandBuffer cmd, VkBuffer from, VkBuffer to)
    {
        const VkBufferCopy region {0, 0, imageBytes};
        CALL(CmdCopyBuffer, cmd, from, to, 1, &region);
    }

    void copyImage(VkCommandBuffer cmd, VkImage from, VkImage to)
    {
        VkImageCopy region {};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {extent, extent, 1};
        CALL(CmdCopyImage, cmd, from, VK_IMAGE_LAYOUT_GENERAL, to, VK_IMAGE_LAYOUT_GENERAL, 1,
            &region);
    }

    void clearImage(VkCommandBuffer cmd, VkImage image)
    {
        const VkClearColorValue color {};
        const VkImageSubresourceRange range {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CALL(CmdClearColorImage, cmd, image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
    }

    void destroy(const Image& image)
    {
        CALL(DestroyImage, device, image.image, nullptr);
        CALL(FreeMemory, device, image.memory, nullptr);
    }

    void destroy(const Buffer& buffer)
    {
        CALL(DestroyBuffer, device, buffer.buffer, nullptr);
        CALL(FreeMemory, device, buffer.memory, nullptr);
    }
};

void expectRefused(const char* what, VkResult ended)
{
    std::fprintf(stderr, "  %s: vkEndCommandBuffer %d\n", what, ended);
    require(ended != VK_SUCCESS, what);
}

void controls(Device& d)
{
    std::fprintf(stderr, "bound image and buffer\n");
    const auto image = d.makeImage();
    const auto other = d.makeImage();
    const auto buffer = d.makeBuffer();
    const auto beforeBind = logLines("bind failed");
    const auto imageBind = d.bindImage(image, 0);
    const auto otherBind = d.bindImage(other, 0);
    const auto bufferBind = d.bindBuffer(buffer, 0);
    require(imageBind.returned == VK_SUCCESS && imageBind.reported == VK_SUCCESS &&
            otherBind.returned == VK_SUCCESS && bufferBind.returned == VK_SUCCESS &&
            bufferBind.reported == VK_SUCCESS,
        "a bind of memory that fits did not succeed");
    require(logLines("bind failed") == beforeBind, "a bind that succeeded logged a failure");

    require(d.record([&](VkCommandBuffer cmd) { d.copyBufferToImage(cmd, buffer.buffer, image.image); }) ==
            VK_SUCCESS,
        "a copy to a bound image was refused");
    require(d.record([&](VkCommandBuffer cmd) { d.copyImageToBuffer(cmd, image.image, buffer.buffer); }) ==
            VK_SUCCESS,
        "a copy from a bound image was refused");
    require(d.record([&](VkCommandBuffer cmd) { d.copyImage(cmd, image.image, other.image); }) ==
            VK_SUCCESS,
        "a copy between bound images was refused");
    require(d.record([&](VkCommandBuffer cmd) { d.clearImage(cmd, image.image); }) == VK_SUCCESS,
        "a clear of a bound image was refused");

    // The recording is the driver's work as well: submit one that copies.
    d.submitCopy(buffer.buffer, image.image);

    d.destroy(buffer);
    d.destroy(other);
    d.destroy(image);
}

// Every transfer that names the image, in both directions, between images and
// as a clear, is refused. `image` has no Metal texture; `ready` has one.
void refuseEveryTransfer(Device& d, const char* label, const Device::Image& image)
{
    std::fprintf(stderr, "%s\n", label);
    const auto ready = d.makeImage();
    const auto buffer = d.makeBuffer();
    require(d.bindImage(ready, 0).returned == VK_SUCCESS, "the bound image did not bind");
    require(d.bindBuffer(buffer, 0).returned == VK_SUCCESS, "the bound buffer did not bind");

    expectRefused("buffer to image",
        d.record([&](VkCommandBuffer cmd) { d.copyBufferToImage(cmd, buffer.buffer, image.image); }));
    expectRefused("image to buffer",
        d.record([&](VkCommandBuffer cmd) { d.copyImageToBuffer(cmd, image.image, buffer.buffer); }));
    expectRefused("image to bound image",
        d.record([&](VkCommandBuffer cmd) { d.copyImage(cmd, image.image, ready.image); }));
    expectRefused("bound image to image",
        d.record([&](VkCommandBuffer cmd) { d.copyImage(cmd, ready.image, image.image); }));
    expectRefused("clear",
        d.record([&](VkCommandBuffer cmd) { d.clearImage(cmd, image.image); }));

    d.destroy(buffer);
    d.destroy(ready);
}

void neverBound(Device& d)
{
    const auto image = d.makeImage();
    refuseEveryTransfer(d, "an image that was never bound", image);
    d.destroy(image);
}

void imageBindRefused(Device& d)
{
    const auto image = d.makeImage();
    const auto before = logLines("image bind failed");
    const auto reasons = logLines("Metal refused the image texture");
    // The bind starts where the allocation ends, so Metal has no room for the
    // texture and refuses it.
    const auto bind = d.bindImage(image, image.requirements.size);
    std::fprintf(stderr, "a refused image bind: returned %d, reported %d\n", bind.returned,
        bind.reported);
    require(bind.returned == VK_ERROR_OUT_OF_DEVICE_MEMORY,
        "a bind past the end of the memory did not fail");
    require(bind.reported == bind.returned,
        "VkBindMemoryStatus did not carry the result of the bind");
    require(logLines("image bind failed") == before + 1,
        "a refused image bind must log exactly one line");
    require(logLines("Metal refused the image texture") == reasons + 1,
        "the line does not say which path refused the bind");
    std::fprintf(stderr, "  log: %s\n", lastLogLine("image bind failed").c_str());
    refuseEveryTransfer(d, "an image whose bind was refused", image);
    d.destroy(image);
}

void bufferBindRefused(Device& d)
{
    const auto buffer = d.makeBuffer();
    const auto before = logLines("buffer bind failed");
    const auto bind = d.bindBuffer(buffer, kFarOffset);
    std::fprintf(stderr, "a refused buffer bind: returned %d, reported %d\n", bind.returned,
        bind.reported);
    require(bind.returned == VK_ERROR_OUT_OF_DEVICE_MEMORY,
        "a buffer bind past the end of the memory did not fail");
    require(bind.reported == bind.returned,
        "VkBindMemoryStatus did not carry the result of the bind");
    require(logLines("buffer bind failed") == before + 1,
        "a refused buffer bind must log exactly one line");
    std::fprintf(stderr, "  log: %s\n", lastLogLine("buffer bind failed").c_str());

    const auto ready = d.makeBuffer();
    const auto image = d.makeImage();
    require(d.bindBuffer(ready, 0).returned == VK_SUCCESS, "the bound buffer did not bind");
    require(d.bindImage(image, 0).returned == VK_SUCCESS, "the bound image did not bind");
    expectRefused("buffer to buffer",
        d.record([&](VkCommandBuffer cmd) { d.copyBuffer(cmd, buffer.buffer, ready.buffer); }));
    expectRefused("buffer from buffer",
        d.record([&](VkCommandBuffer cmd) { d.copyBuffer(cmd, ready.buffer, buffer.buffer); }));
    expectRefused("buffer to image",
        d.record([&](VkCommandBuffer cmd) { d.copyBufferToImage(cmd, buffer.buffer, image.image); }));
    expectRefused("image to buffer",
        d.record([&](VkCommandBuffer cmd) { d.copyImageToBuffer(cmd, image.image, buffer.buffer); }));
    d.destroy(image);
    d.destroy(ready);
    d.destroy(buffer);
}

} // namespace

// With no argument every scenario runs; with a scenario's name, that one alone.
int main(int argc, char** argv)
{
    // The driver reads its log destination once, on its first message.
    static const uml::testing::ScratchSubdirectory scratch("kosmickrisp-unbound-XXXXXX");
    logPath = (scratch.path() / "mesa.log").string();
    ::setenv("MESA_LOG_FILE", logPath.c_str(), 1);

    struct Scenario {
        const char* name;
        void (*run)(Device&);
    };
    constexpr Scenario scenarios[] = {
        {"controls", controls},
        {"never-bound", neverBound},
        {"image-bind-refused", imageBindRefused},
        {"buffer-bind-refused", bufferBindRefused},
    };
    Device device;
    bool ran = false;
    for (const Scenario& scenario : scenarios) {
        if (argc > 1 && std::strcmp(argv[1], scenario.name) != 0)
            continue;
        ran = true;
        scenario.run(device);
    }
    require(ran, "no scenario has the name given");
    return 0;
}

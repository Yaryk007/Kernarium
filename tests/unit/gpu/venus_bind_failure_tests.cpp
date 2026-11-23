// SPDX-License-Identifier: GPL-2.0-or-later

// A guest drives the Venus renderer, so a guest command stream must never make
// the host hand Metal an object that does not exist. The guest binds image and
// buffer memory asynchronously: its vkBindImageMemory and vkBindBufferMemory
// return success whatever the host driver answers, so the guest never learns
// that a bind failed, and its next copy names an image or buffer that has no
// Metal texture or buffer behind it. The driver passed that nil to the Metal
// copy encoder, which dereferences it and ended the host process (an A14
// iPhone crash in copyFromBuffer:toTexture:).
//
// A bind the driver refused must end the guest's context, and a transfer that
// names storage the driver never made must be refused by the driver. This test
// drives the real vkr renderer in-process against the real host Vulkan driver
// and submits a hand-encoded Venus command stream, in the style of
// venus_render_pass_lifetime_tests.cpp. Each case runs in a context of its own
// and checks whether the context stays live:
//
//  - a buffer copied into an image, and a buffer copied into a buffer, whose
//    memory bound without trouble leave the context live (the controls);
//  - a bind that names an offset beyond the allocation, for an image and for a
//    buffer, marks the context lost before the copy that follows it in the
//    same stream runs, and does not crash the host; and
//  - a copy into an image that was never bound is refused by the driver and
//    does not crash the host, so the context stays live (an application bug,
//    not an attack on the host).
//
// On a renderer and driver without the guards the bind and never-bound cases
// crash this process, which fails the test.

#include "require.hpp"
#include "venus_wire.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using uml::testing::require;

using namespace uml::gpu::testing::venus;

// The object ids the guest assigns.
enum Id : std::uint64_t {
    instanceId = 1, physDevId = 2, deviceId = 3, imageId = 10,
    imageMemoryId = 20, bufferMemoryId = 21, otherMemoryId = 22,
    bufferId = 30, otherBufferId = 31, poolId = 50, cmdBufId = 60,
};
constexpr std::uint32_t extent = 64;
constexpr std::uint64_t imageBytes = std::uint64_t{extent} * extent * 4;
constexpr std::uint64_t memorySize = 64 * 1024;
// An offset beyond any allocation this test makes.
constexpr std::uint64_t farOffset = std::uint64_t{1} << 32;

void encodeSetup(Stream& s)
{
    encodeInstanceAndDevice(s, VK_API_VERSION_1_2, instanceId, physDevId, deviceId);

    s.command(createImage);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO); s.u64(0);
    s.u32(0);
    s.u32(VK_IMAGE_TYPE_2D); s.u32(VK_FORMAT_R8G8B8A8_UNORM);
    s.u32(extent); s.u32(extent); s.u32(1);
    s.u32(1); s.u32(1);
    s.u32(VK_SAMPLE_COUNT_1_BIT); s.u32(VK_IMAGE_TILING_OPTIMAL);
    s.u32(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    s.u32(VK_SHARING_MODE_EXCLUSIVE); s.u32(0); s.arraySize(0);
    s.u32(VK_IMAGE_LAYOUT_UNDEFINED);
    s.ptr(false);
    s.ptr(true); s.u64(imageId);

    s.command(getImageMemoryRequirements);
    s.u64(deviceId); s.u64(imageId); s.ptr(true);

    for (const std::uint64_t memory : {imageMemoryId, bufferMemoryId, otherMemoryId}) {
        s.command(allocateMemory);
        s.u64(deviceId);
        s.ptr(true);
        s.u32(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO); s.u64(0);
        s.u64(memorySize); s.u32(0);
        s.ptr(false);
        s.ptr(true); s.u64(memory);
    }

    for (const std::uint64_t buffer : {bufferId, otherBufferId}) {
        s.command(createBuffer);
        s.u64(deviceId);
        s.ptr(true);
        s.u32(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO); s.u64(0);
        s.u32(0); s.u64(imageBytes);
        s.u32(VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        s.u32(VK_SHARING_MODE_EXCLUSIVE); s.u32(0); s.arraySize(0);
        s.ptr(false);
        s.ptr(true); s.u64(buffer);
    }

    s.command(createCommandPool);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO); s.u64(0);
    s.u32(0); s.u32(0);
    s.ptr(false);
    s.ptr(true); s.u64(poolId);

    s.command(allocateCommandBuffers);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO); s.u64(0);
    s.u64(poolId); s.u32(VK_COMMAND_BUFFER_LEVEL_PRIMARY); s.u32(1);
    s.arraySize(1); s.u64(cmdBufId);
}

// What the guest binds before it copies, and what it copies.
enum class Scenario {
    imageCopyIntact,
    bufferCopyIntact,
    imageBindPastEnd,
    bufferBindPastEnd,
    imageNeverBound,
};

[[nodiscard]] constexpr bool copiesToImage(Scenario scenario)
{
    return scenario != Scenario::bufferCopyIntact &&
        scenario != Scenario::bufferBindPastEnd;
}

void encodeBindBuffer(Stream& s, std::uint64_t buffer, std::uint64_t memory,
    std::uint64_t offset)
{
    s.command(bindBufferMemory);
    s.u64(deviceId); s.u64(buffer); s.u64(memory); s.u64(offset);
}

void encodeRecord(Stream& s, Scenario scenario)
{
    if (scenario != Scenario::imageNeverBound) {
        s.command(bindImageMemory);
        s.u64(deviceId); s.u64(imageId); s.u64(imageMemoryId);
        s.u64(scenario == Scenario::imageBindPastEnd ? farOffset : 0);
    }
    encodeBindBuffer(s, bufferId, bufferMemoryId, 0);
    encodeBindBuffer(s, otherBufferId, otherMemoryId,
        scenario == Scenario::bufferBindPastEnd ? farOffset : 0);

    s.command(beginCommandBuffer);
    s.u64(cmdBufId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO); s.u64(0);
    s.u32(0); s.ptr(false);

    if (copiesToImage(scenario)) {
        s.command(cmdCopyBufferToImage);
        s.u64(cmdBufId); s.u64(bufferId); s.u64(imageId);
        s.u32(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        s.u32(1); s.arraySize(1);
        s.u64(0); s.u32(0); s.u32(0);
        s.u32(VK_IMAGE_ASPECT_COLOR_BIT); s.u32(0); s.u32(0); s.u32(1);
        s.u32(0); s.u32(0); s.u32(0);
        s.u32(extent); s.u32(extent); s.u32(1);
    } else {
        s.command(cmdCopyBuffer);
        s.u64(cmdBufId); s.u64(otherBufferId); s.u64(bufferId);
        s.u32(1); s.arraySize(1);
        s.u64(0); s.u64(0); s.u64(imageBytes);
    }

    s.command(endCommandBuffer);
    s.u64(cmdBufId);
}

// The renderer says why it refuses a command; the log makes a failure here
// read as one.
void logRenderer(int, const char* message, void*)
{
    std::fprintf(stderr, "  renderer: %s", message);
}
void retireFence(std::uint32_t, std::uint32_t, std::uint64_t) {}
void contextFatal(std::uint32_t) {}

// Brings a context up through setup, binds and copies as scenario says, and
// returns whether the context stayed live.
[[nodiscard]] bool runContext(std::uint32_t ctxId, Scenario scenario)
{
    require(vkr_renderer_create_context(
                ctxId, virtgpuDrmCapsetVenus, 4, "test"),
        "the Venus context could not be created");

    Stream setup;
    encodeSetup(setup);
    require(vkr_renderer_submit_cmd(ctxId, setup.bytes().data(), setup.size()),
        "the context was lost bringing the device up; the host driver may be "
        "unavailable in this environment");

    Stream record;
    encodeRecord(record, scenario);
    const bool live =
        vkr_renderer_submit_cmd(ctxId, record.bytes().data(), record.size());

    vkr_renderer_destroy_context(ctxId);
    return live;
}

struct Case {
    const char* name;
    Scenario scenario;
    bool live;
    const char* failure;
};

constexpr Case cases[] = {
    {"image-copy-intact", Scenario::imageCopyIntact, true,
        "a copy into an image that bound lost the context"},
    {"buffer-copy-intact", Scenario::bufferCopyIntact, true,
        "a copy between buffers that bound lost the context"},
    // The bind fails, the guest is not told, and the copy that follows it in
    // the same stream must not run.
    {"image-bind-past-end", Scenario::imageBindPastEnd, false,
        "a bind the driver refused did not lose the context of an image"},
    {"buffer-bind-past-end", Scenario::bufferBindPastEnd, false,
        "a bind the driver refused did not lose the context of a buffer"},
    // The guest never bound the image: the driver refuses the copy and the
    // guest's context is unaffected.
    {"image-never-bound", Scenario::imageNeverBound, true,
        "a copy into an image that was never bound lost the context"},
};

} // namespace

// With no argument every case runs; with a case's name, that case alone.
int main(int argc, char** argv)
{
    const vkr_renderer_callbacks callbacks {
        .debug_logger = logRenderer,
        .retire_fence = retireFence,
        .context_fatal = contextFatal,
    };
    require(vkr_renderer_init(
                vkrRendererThreadSync | vkrRendererAsyncFenceCb, &callbacks),
        "the Venus renderer could not be initialised");

    std::uint32_t ctxId = 1;
    bool ran = false;
    for (const Case& c : cases) {
        if (argc > 1 && std::strcmp(argv[1], c.name) != 0)
            continue;
        ran = true;
        std::fprintf(stderr, "bind case: %s\n", c.name);
        const bool live = runContext(ctxId++, c.scenario);
        std::fprintf(stderr, "  live=%d\n", live ? 1 : 0);
        require(live == c.live, c.failure);
    }
    require(ran, "no case has the name given");

    vkr_renderer_fini();
    return 0;
}

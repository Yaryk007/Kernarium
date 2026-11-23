// SPDX-License-Identifier: GPL-2.0-or-later

// A guest drives the Venus renderer, so a guest command stream must never make
// the host dereference freed memory. A framebuffer keeps only the handles of
// its attachment image views, and the driver reads every view of the pass, and
// the image of each, when a render pass begins. A guest that destroys an
// attachment's image view (or its image) and then begins a render pass on that
// framebuffer made KosmicKrisp read the freed view and end the host process in
// transition_attachment. The renderer must instead mark the context lost and
// keep running.
//
// The same holds for a command the device does not expose. The driver leaves
// the entry points of a version the instance did not ask for NULL, and the
// renderer used to call through NULL when a guest sent such a command. A
// command a guest really sends on a host that lacks it, such as
// vkGetImageDrmFormatModifierPropertiesEXT on an Apple host, is answered
// with VK_ERROR_EXTENSION_NOT_PRESENT and the context stays live.
//
// This test drives the real vkr renderer in-process against the real host
// Vulkan driver and submits a hand-encoded Venus command stream. The guest
// chooses every Venus object id, so no reply stream is needed. Each case runs
// in a context of its own and checks whether the context stays live:
//
//  - a render pass begun with every attachment intact, through both
//    vkCmdBeginRenderPass and vkCmdBeginRenderPass2, leaves the context live
//    (the controls);
//  - a render pass begun after the attachment's view or its image was
//    destroyed marks the context lost and does not crash the host; and
//  - vkCmdBeginRenderPass2 sent to a Vulkan 1.1 device, where it is not core
//    and no extension enabled it, marks the context lost and does not crash
//    the host; and
//  - vkGetImageDrmFormatModifierPropertiesEXT, which the host answers itself,
//    gets its reply with the extension-not-present result and a live context.
//
// On a renderer without the defensive checks the view case and the 1.1 case
// crash this process, which fails the test.

#include "require.hpp"
#include "venus_wire.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

namespace {

using uml::testing::require;

using namespace uml::gpu::testing::venus;

// The object ids the guest assigns.
enum Id : std::uint64_t {
    instanceId = 1, physDevId = 2, deviceId = 3, imageId = 10, memoryId = 20,
    viewId = 30, renderPassId = 40, framebufferId = 41, poolId = 50, cmdBufId = 60,
};
constexpr std::uint32_t extent = 64;
constexpr std::uint64_t memorySize = 64 * 1024;

void encodeSetup(Stream& s, std::uint32_t apiVersion)
{
    encodeInstanceAndDevice(s, apiVersion, instanceId, physDevId, deviceId);

    s.command(createImage);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO); s.u64(0);
    s.u32(0);
    s.u32(VK_IMAGE_TYPE_2D); s.u32(VK_FORMAT_R8G8B8A8_UNORM);
    s.u32(extent); s.u32(extent); s.u32(1);
    s.u32(1); s.u32(1);
    s.u32(VK_SAMPLE_COUNT_1_BIT); s.u32(VK_IMAGE_TILING_OPTIMAL);
    s.u32(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    s.u32(VK_SHARING_MODE_EXCLUSIVE); s.u32(0); s.arraySize(0);
    s.u32(VK_IMAGE_LAYOUT_UNDEFINED);
    s.ptr(false);
    s.ptr(true); s.u64(imageId);

    s.command(getImageMemoryRequirements);
    s.u64(deviceId); s.u64(imageId); s.ptr(true);

    s.command(allocateMemory);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO); s.u64(0);
    s.u64(memorySize); s.u32(0);
    s.ptr(false);
    s.ptr(true); s.u64(memoryId);

    s.command(bindImageMemory);
    s.u64(deviceId); s.u64(imageId); s.u64(memoryId); s.u64(0);

    s.command(createImageView);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO); s.u64(0);
    s.u32(0); s.u64(imageId);
    s.u32(VK_IMAGE_VIEW_TYPE_2D); s.u32(VK_FORMAT_R8G8B8A8_UNORM);
    s.u32(VK_COMPONENT_SWIZZLE_IDENTITY); s.u32(VK_COMPONENT_SWIZZLE_IDENTITY);
    s.u32(VK_COMPONENT_SWIZZLE_IDENTITY); s.u32(VK_COMPONENT_SWIZZLE_IDENTITY);
    s.u32(VK_IMAGE_ASPECT_COLOR_BIT); s.u32(0); s.u32(1); s.u32(0); s.u32(1);
    s.ptr(false);
    s.ptr(true); s.u64(viewId);

    s.command(createRenderPass);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO); s.u64(0);
    s.u32(0);
    s.u32(1); s.arraySize(1);       // one attachment
    s.u32(0); s.u32(VK_FORMAT_R8G8B8A8_UNORM); s.u32(VK_SAMPLE_COUNT_1_BIT);
    s.u32(VK_ATTACHMENT_LOAD_OP_CLEAR); s.u32(VK_ATTACHMENT_STORE_OP_STORE);
    s.u32(VK_ATTACHMENT_LOAD_OP_DONT_CARE); s.u32(VK_ATTACHMENT_STORE_OP_DONT_CARE);
    s.u32(VK_IMAGE_LAYOUT_UNDEFINED); s.u32(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    s.u32(1); s.arraySize(1);       // one subpass
    s.u32(0); s.u32(VK_PIPELINE_BIND_POINT_GRAPHICS);
    s.u32(0); s.arraySize(0);       // input
    s.u32(1); s.arraySize(1);       // colour
    s.u32(0); s.u32(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    s.arraySize(0);                 // resolve absent
    s.ptr(false);                   // depth/stencil
    s.u32(0); s.arraySize(0);       // preserve
    s.u32(0); s.arraySize(0);       // dependencies
    s.ptr(false);
    s.ptr(true); s.u64(renderPassId);

    s.command(createFramebuffer);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO); s.u64(0);
    s.u32(0); s.u64(renderPassId);
    s.u32(1); s.arraySize(1); s.u64(viewId);
    s.u32(extent); s.u32(extent); s.u32(1);
    s.ptr(false);
    s.ptr(true); s.u64(framebufferId);

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

// What the guest does to the framebuffer's attachments before it begins the
// render pass, and which begin command it uses.
enum class Scenario {
    intact,
    intactBegin2,
    destroyView,
    destroyViewBegin2,
    destroyImage,
};

[[nodiscard]] constexpr bool usesBegin2(Scenario scenario)
{
    return scenario == Scenario::intactBegin2 ||
        scenario == Scenario::destroyViewBegin2;
}

void encodeRenderPassBeginInfo(Stream& s)
{
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO); s.u64(0);
    s.u64(renderPassId); s.u64(framebufferId);
    s.u32(0); s.u32(0); s.u32(extent); s.u32(extent);
    s.u32(1); s.arraySize(1);
    s.u32(0);                       // VkClearValue tag
    s.u32(2); s.arraySize(4); s.u32(0); s.u32(0); s.u32(0); s.u32(0);
}

// Records a render pass after the guest has treated the attachments as the
// scenario says.
void encodeRecord(Stream& s, Scenario scenario)
{
    switch (scenario) {
    case Scenario::destroyView:
    case Scenario::destroyViewBegin2:
        s.command(destroyImageView);
        s.u64(deviceId); s.u64(viewId); s.ptr(false);
        break;
    case Scenario::destroyImage:
        s.command(destroyImage);
        s.u64(deviceId); s.u64(imageId); s.ptr(false);
        break;
    case Scenario::intact:
    case Scenario::intactBegin2:
        break;
    }

    s.command(beginCommandBuffer);
    s.u64(cmdBufId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO); s.u64(0);
    s.u32(0); s.ptr(false);

    if (usesBegin2(scenario)) {
        s.command(cmdBeginRenderPass2);
        s.u64(cmdBufId);
        encodeRenderPassBeginInfo(s);
        s.ptr(true);
        s.u32(VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO); s.u64(0);
        s.u32(VK_SUBPASS_CONTENTS_INLINE);
    } else {
        s.command(cmdBeginRenderPass);
        s.u64(cmdBufId);
        encodeRenderPassBeginInfo(s);
        s.u32(VK_SUBPASS_CONTENTS_INLINE);
    }

    s.command(cmdEndRenderPass);
    s.u64(cmdBufId);

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

// Brings a context up through setup, records a render pass as scenario says,
// and returns whether the context stayed live.
[[nodiscard]] bool runContext(
    std::uint32_t ctxId, Scenario scenario, std::uint32_t apiVersion)
{
    require(vkr_renderer_create_context(
                ctxId, virtgpuDrmCapsetVenus, 4, "test"),
        "the Venus context could not be created");

    Stream setup;
    encodeSetup(setup, apiVersion);
    require(vkr_renderer_submit_cmd(ctxId, setup.bytes().data(),
                static_cast<std::uint32_t>(setup.bytes().size())),
        "the context was lost bringing the device up; the host driver may be "
        "unavailable in this environment");

    Stream record;
    encodeRecord(record, scenario);
    const bool live = vkr_renderer_submit_cmd(ctxId, record.bytes().data(),
        static_cast<std::uint32_t>(record.bytes().size()));

    vkr_renderer_destroy_context(ctxId);
    return live;
}

// A command the guest sends with a reply request, whose answer the test reads
// from the reply stream.
enum class Query {
    resetCommandPool,
    imageDrmFormatModifier,
};

struct Answer {
    bool live;
    std::int32_t result;
};

constexpr std::uint32_t replyResourceId = 900;
constexpr std::uint64_t replySize = 16384;       // one host page

// Brings a context up, gives it a shared-memory reply stream, sends the query
// and reads the result that came back. The result is 0 unless the context is
// live.
[[nodiscard]] Answer runQuery(std::uint32_t ctxId, Query query)
{
    require(vkr_renderer_create_context(
                ctxId, virtgpuDrmCapsetVenus, 4, "test"),
        "the Venus context could not be created");

    Stream setup;
    encodeSetup(setup, VK_API_VERSION_1_2);
    require(vkr_renderer_submit_cmd(ctxId, setup.bytes().data(),
                static_cast<std::uint32_t>(setup.bytes().size())),
        "the context was lost bringing the device up; the host driver may be "
        "unavailable in this environment");

    int fdType = 0;
    int fd = -1;
    std::uint32_t mapInfo = 0;
    alignas(16) unsigned char vulkanInfo[256] {};
    require(vkr_renderer_create_resource(ctxId, replyResourceId, 0, replySize,
                blobFlagUseMappable, &fdType, &fd, &mapInfo, vulkanInfo),
        "the reply stream could not be created");
    void* reply = mmap(nullptr, replySize, PROT_READ | PROT_WRITE, MAP_SHARED,
        fd, 0);
    require(reply != MAP_FAILED, "the reply stream could not be mapped");

    Stream stream;
    stream.command(setReplyCommandStream);
    stream.ptr(true);
    stream.u32(replyResourceId); stream.u64(0); stream.u64(replySize);
    Cmd command = resetCommandPool;
    switch (query) {
    case Query::resetCommandPool:
        stream.command(command, true);
        stream.u64(deviceId); stream.u64(poolId); stream.u32(0);
        break;
    case Query::imageDrmFormatModifier:
        command = getImageDrmFormatModifierProperties;
        stream.command(command, true);
        stream.u64(deviceId); stream.u64(imageId);
        stream.ptr(true);
        stream.u32(VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT);
        stream.u64(0);
        break;
    }
    const bool live = vkr_renderer_submit_cmd(ctxId, stream.bytes().data(),
        static_cast<std::uint32_t>(stream.bytes().size()));

    // The reply starts with the command's type, then its VkResult.
    std::int32_t words[2] {};
    std::memcpy(words, reply, sizeof(words));
    if (live)
        require(words[0] == static_cast<std::int32_t>(command),
            "the reply names another command");

    munmap(reply, replySize);
    close(fd);
    vkr_renderer_destroy_resource(ctxId, replyResourceId);
    vkr_renderer_destroy_context(ctxId);
    return {live, live ? words[1] : 0};
}

// What the host must answer a query with.
struct QueryCase {
    const char* name;
    Query query;
    std::int32_t result;
    const char* failure;
};

constexpr QueryCase queryCases[] = {
    // The control: a command that succeeds on every host shows the reply
    // stream and the reply request are encoded right.
    {"reply-reset-command-pool", Query::resetCommandPool, VK_SUCCESS,
        "vkResetCommandPool did not answer VK_SUCCESS on a live context"},
    // An Apple host has no DRM format modifiers, so the renderer answers the
    // command itself and the context stays live.
    {"reply-drm-format-modifier", Query::imageDrmFormatModifier,
        VK_ERROR_EXTENSION_NOT_PRESENT,
        "vkGetImageDrmFormatModifierPropertiesEXT did not answer "
        "VK_ERROR_EXTENSION_NOT_PRESENT on a live context"},
};

// One guest behaviour and what the renderer must do about it.
struct Case {
    const char* name;
    Scenario scenario;
    // The Vulkan version the guest's instance asks for. vkCmdBeginRenderPass2
    // is core from 1.2.
    std::uint32_t apiVersion;
    // Whether the context stays live. Every case that is not a control must
    // lose it.
    bool live;
    const char* failure;
};

constexpr Case cases[] = {
    // Controls: every attachment is intact, so the render pass records
    // through either begin command and the context stays live. They also show
    // that the wire encoding of both commands is accepted, so a lost context
    // in the cases below is the check's doing.
    {"intact", Scenario::intact, VK_API_VERSION_1_2, true,
        "recording a render pass with live attachments lost the context"},
    {"intact-begin2", Scenario::intactBegin2, VK_API_VERSION_1_2, true,
        "recording a render pass 2 with live attachments lost the context"},
    // The regression: the attachment's view is destroyed before the render
    // pass begins. The renderer must mark the context lost rather than read
    // the freed view and crash the host.
    {"destroyed-view", Scenario::destroyView, VK_API_VERSION_1_2, false,
        "a render pass whose attachment view was destroyed did not lose the "
        "context"},
    {"destroyed-view-begin2", Scenario::destroyViewBegin2, VK_API_VERSION_1_2,
        false,
        "a render pass 2 whose attachment view was destroyed did not lose the "
        "context"},
    // The image of a live view is as much a use of freed host memory.
    {"destroyed-image", Scenario::destroyImage, VK_API_VERSION_1_2, false,
        "a render pass whose attachment image was destroyed did not lose the "
        "context"},
    // A command the device does not expose. The control shows the 1.1 device
    // records a render pass through the command it does expose.
    {"api-1.1-intact", Scenario::intact, VK_API_VERSION_1_1, true,
        "recording a render pass on a Vulkan 1.1 device lost the context"},
    {"api-1.1-begin2", Scenario::intactBegin2, VK_API_VERSION_1_1, false,
        "vkCmdBeginRenderPass2 on a Vulkan 1.1 device did not lose the "
        "context"},
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
        std::fprintf(stderr, "render pass case: %s\n", c.name);
        require(runContext(ctxId++, c.scenario, c.apiVersion) == c.live,
            c.failure);
    }
    for (const QueryCase& q : queryCases) {
        if (argc > 1 && std::strcmp(argv[1], q.name) != 0)
            continue;
        ran = true;
        std::fprintf(stderr, "query case: %s\n", q.name);
        const Answer answer = runQuery(ctxId++, q.query);
        std::fprintf(stderr, "  live=%d result=%d\n", answer.live ? 1 : 0,
            answer.result);
        require(answer.live && answer.result == q.result, q.failure);
    }
    require(ran, "no case has the name given");

    vkr_renderer_fini();
    return 0;
}

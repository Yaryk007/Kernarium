// SPDX-License-Identifier: GPL-2.0-or-later

// A guest drives the Venus renderer, and in the product the renderer runs in
// the app's own process, so no guest command stream may hand the host driver
// bytes the guest chose for a parser that is not written for hostile input.
// VK_KHR_pipeline_binary imports a binary under a key and data the guest
// names, with no header at all, and builds a pipeline from it. The renderer
// serves none of the extension: it does not offer it, and a guest that
// enables the extension on a device by name anyway, which the renderer passes
// to the driver as it does every name, still has each command of it end the
// context, the host living on, so that no key or data reaches the driver:
//
//  - the five commands of the extension, each sent the way a guest sends them
//    (an import under a key of every size, a binary named by the null handle,
//    by an id never made and by one that was never offered).
//  - a pipeline creation, graphics or compute, whose create info chains
//    pipeline binaries, whatever it names.
//  - the extension is absent from the list the renderer reports, and the list
//    still holds the extension beside it that the renderer does serve.
//
// And the rows of the renderer's object table, which the binaries once served
// as the vehicle of: a second object under an id that is already in use, or
// under id 0, loses the context, and tearing it down must still be clean: the
// object the context refused is freed whichever order its owner and the owner
// of the other object are torn down in, and the other object is left alone.
//
// The test drives the real vkr renderer in-process against the real host
// Vulkan driver and submits hand-encoded Venus command streams, in the style of
// venus_render_pass_lifetime_tests.cpp. A row asks for a reply and finds that
// the context was lost.
//
// Each row runs in a child process under Guard Malloc, and the rows that name
// a leak run a second time under the leak checker, as venus_row_runner.hpp
// describes.

#include "require.hpp"
#include "venus_pipeline_wire.hpp"
#include "venus_row_runner.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

extern "C" std::uint32_t vkr_extension_get_spec_version(const char* name);

namespace {

using uml::testing::require;
using namespace uml::gpu::testing::venus;


// The object ids the guest assigns.
enum Id : std::uint64_t {
    instanceId = 1, physDevId = 2, deviceId = 3, binaryId = 10, otherBinaryId = 11,
    pipelineId = 20, cacheId = 25, poolId = 30, otherPoolId = 31, commandBufferId = 40,
    neverMadeId = 77,
};
constexpr std::uint32_t keyBytes = VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
constexpr std::size_t binaryDataBytes = 8;

// A request to import binaries, or to make them from another source.
struct CreateRequest {
    bool keysAndData = false;                    // pKeysAndDataInfo: one binary
    std::uint32_t keySize = keyBytes;            // ... under a key of this size
    std::uint64_t pipeline = 0;                  // pipeline
    bool createInfo = false;                     // pPipelineCreateInfo
    std::uint64_t binary = binaryId;             // the guest's id for the binary
};

// vkCreatePipelineBinariesKHR for one binary under the guest's object id
// request.binary, over a key array of the 32 bytes the codec holds and eight
// bytes of data.
void encodeCreate(Stream& s, const CreateRequest& request)
{
    s.command(createPipelineBinaries, true);
    s.u64(deviceId);
    s.ptr(true);                                // pCreateInfo
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR); s.ptr(false);
    s.ptr(request.keysAndData);                 // pKeysAndDataInfo
    if (request.keysAndData) {
        s.u32(1);                               // binaryCount
        s.arraySize(1);                         // pPipelineBinaryKeys
        s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR); s.ptr(false);
        s.u32(request.keySize);
        s.arraySize(keyBytes);
        std::array<std::uint8_t, keyBytes> key{};
        for (std::uint32_t i = 0; i < keyBytes; ++i)
            key[i] = static_cast<std::uint8_t>(0x40 + i);
        s.blob(key.data(), key.size());
        s.arraySize(1);                         // pPipelineBinaryData
        constexpr std::array<std::uint8_t, binaryDataBytes> data = {1, 2, 3, 4, 5, 6, 7, 8};
        s.u64(data.size());
        s.arraySize(data.size());
        s.blob(data.data(), data.size());
    }
    s.u64(request.pipeline);                    // pipeline
    s.ptr(request.createInfo);                  // pPipelineCreateInfo
    if (request.createInfo) {
        s.u32(VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR);
        s.ptr(true);                            // pNext: the flags of the pipeline
        s.u32(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO); s.ptr(false);
        s.u64(0);
    }
    s.ptr(false);                               // pAllocator
    s.ptr(true);                                // pBinaries
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR); s.ptr(false);
    s.u32(1);                                   // pipelineBinaryCount
    s.arraySize(1); s.u64(request.binary);
}

// vkGetPipelineBinaryDataKHR asking how large the binary's data is.
void encodeGetData(Stream& s, std::uint64_t binary)
{
    s.command(getPipelineBinaryData, true);
    s.u64(deviceId);
    s.ptr(true);                                // pInfo
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR); s.ptr(false);
    s.u64(binary);
    s.ptr(true);                                // pPipelineBinaryKey
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR); s.ptr(false);
    s.ptr(true); s.u64(0);                      // pPipelineBinaryDataSize
    s.arraySize(0);                             // pPipelineBinaryData: none
}

void encodeDestroy(Stream& s, std::uint64_t binary)
{
    s.command(destroyPipelineBinary);
    s.u64(deviceId);
    s.u64(binary);
    s.ptr(false);                               // pAllocator
}

void encodeRelease(Stream& s, std::uint64_t pipeline)
{
    s.command(releaseCapturedPipelineData, true);
    s.u64(deviceId);
    s.ptr(true);                                // pInfo
    s.u32(VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR); s.ptr(false);
    s.u64(pipeline);
    s.ptr(false);                               // pAllocator
}

// vkCreateComputePipelines for one pipeline, its guest id `pipelineId`, that
// reads one pipeline binary the guest names by id. The stage has no module
// and the layout is none: the binary is what the driver builds the pipeline
// from.
void encodeComputePipelineFromBinary(Stream& s, std::uint64_t binary)
{
    s.command(createComputePipelines, true);
    s.u64(deviceId);
    s.u64(0);                                   // pipelineCache
    s.u32(1); s.arraySize(1);                   // pCreateInfos
    encodeComputeInfo(s, {}, 0, {.binaries = true, .binary = binary});
    s.ptr(false);                               // pAllocator
    s.arraySize(1); s.u64(pipelineId);          // pPipelines
}

// vkCreateGraphicsPipelines for one pipeline, its guest id `pipelineId`, that
// reads one pipeline binary the guest names by id. The create info is one the
// runtime accepts up to the point where it reads the binary: a complete
// pipeline whose rasterization state discards every primitive, with one vertex
// stage that has no module (the binary is what the driver builds the pipeline
// from), no layout and no render pass. Anything less faults earlier, in the
// runtime's own reading of the create info, and the row would not show the
// check it names.
void encodeGraphicsPipelineFromBinary(Stream& s, std::uint64_t binary)
{
    s.command(createGraphicsPipelines, true);
    s.u64(deviceId);
    s.u64(0);                                   // pipelineCache
    s.u32(1); s.arraySize(1);                   // pCreateInfos
    encodeGraphicsInfo(s, {.vertex = true}, {.binaries = true, .binary = binary});
    s.ptr(false);                               // pAllocator
    s.arraySize(1); s.u64(pipelineId);          // pPipelines
}

// vkGetPipelineKeyKHR of the device, which names no pipeline.
void encodeGetKey(Stream& s)
{
    s.command(getPipelineKey, true);
    s.u64(deviceId);
    s.ptr(false);                               // pPipelineCreateInfo
    s.ptr(true);                                // pPipelineKey
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR); s.ptr(false);
}

// vkCreatePipelineCache under the guest's id, with no data.
void encodeCreateCache(Stream& s, std::uint64_t cache)
{
    s.command(createPipelineCache);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO); s.ptr(false);
    s.u32(0);                                   // flags
    s.u64(0);                                   // initialDataSize
    s.arraySize(0);                             // pInitialData: none
    s.ptr(false);                               // pAllocator
    s.ptr(true); s.u64(cache);
}

// A command pool of the device, under the guest's id.
void encodeCreatePool(Stream& s, std::uint64_t pool)
{
    s.command(createCommandPool);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO); s.u64(0);
    s.u32(0); s.u32(0);
    s.ptr(false);
    s.ptr(true); s.u64(pool);
}

// One command buffer of a pool, under the guest's id.
void encodeAllocateCommandBuffer(Stream& s, std::uint64_t pool, std::uint64_t commandBuffer)
{
    s.command(allocateCommandBuffers);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO); s.u64(0);
    s.u64(pool); s.u32(VK_COMMAND_BUFFER_LEVEL_PRIMARY); s.u32(1);
    s.arraySize(1); s.u64(commandBuffer);
}

void encodeSetup(Stream& s)
{
    DeviceExtras extras;
    extras.extensions = {VK_KHR_PIPELINE_BINARY_EXTENSION_NAME};
    extras.pipelineBinaries = true;
    encodeInstanceAndDevice(s, VK_API_VERSION_1_3, instanceId, physDevId, deviceId, extras);
}


// A command that a guest sends for an extension the renderer does not serve
// loses the context, and the host lives on: a context of its own is served
// afterwards.
Row contextLostRow(const char* name, std::function<void(Stream&)> encode)
{
    return {name, [encode = std::move(encode)](Session& session) {
        Stream s;
        encode(s);
        const Answer answer = session.ask(s);
        require(!answer.submitted, "a command of an extension the renderer does not serve was answered");
        Session fresh(2, encodeSetup, replyResourceId + 1);
        Stream next;
        encodeCreateCache(next, cacheId);
        require(fresh.send(next), "a context of its own was not served after another was lost");
    }};
}

const std::vector<Row>& rows()
{
    static const std::vector<Row> all = {
        // What the renderer reports.
        {"the-extension-is-not-offered", [](Session&) {
            require(vkr_extension_get_spec_version(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) == 0,
                "the renderer reports VK_KHR_pipeline_binary");
            require(vkr_extension_get_spec_version(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) != 0,
                "the renderer reports no extension at all");
        }},

        // The five commands, sent to a device that named the extension.
        contextLostRow("import-under-a-key-of-32-bytes",
            [](Stream& s) { encodeCreate(s, {.keysAndData = true}); }),
        contextLostRow("import-under-a-key-of-4096-bytes",
            [](Stream& s) { encodeCreate(s, {.keysAndData = true, .keySize = 4096}); }),
        contextLostRow("create-from-a-pipeline",
            [](Stream& s) { encodeCreate(s, {.pipeline = neverMadeId}); }),
        contextLostRow("create-from-a-create-info",
            [](Stream& s) { encodeCreate(s, {.createInfo = true}); }),
        contextLostRow("get-data-of-the-null-handle", [](Stream& s) { encodeGetData(s, 0); }),
        contextLostRow("get-data-of-an-id-never-made", [](Stream& s) { encodeGetData(s, neverMadeId); }),
        contextLostRow("destroy-the-null-handle", [](Stream& s) { encodeDestroy(s, 0); }),
        contextLostRow("destroy-an-id-never-made", [](Stream& s) { encodeDestroy(s, neverMadeId); }),
        contextLostRow("release-the-null-pipeline", [](Stream& s) { encodeRelease(s, 0); }),
        contextLostRow("release-a-pipeline-never-made", [](Stream& s) { encodeRelease(s, neverMadeId); }),
        contextLostRow("get-the-key-of-the-device", [](Stream& s) { encodeGetKey(s); }),

        // The same, to a device that never named it.
        {"import-to-a-device-that-never-named-the-extension", [](Session& session) {
            Stream s;
            encodeCreate(s, {.keysAndData = true});
            require(!session.ask(s).submitted, "an import was answered by a device without the extension");
        }, false, [](Stream& s) {
            encodeInstanceAndDevice(s, VK_API_VERSION_1_3, instanceId, physDevId, deviceId);
        }},

        // A pipeline creation reads the binaries its create info names, which
        // no object can be.
        contextLostRow("compute-pipeline-from-the-null-binary",
            [](Stream& s) { encodeComputePipelineFromBinary(s, 0); }),
        contextLostRow("compute-pipeline-from-a-binary-never-made",
            [](Stream& s) { encodeComputePipelineFromBinary(s, neverMadeId); }),
        contextLostRow("graphics-pipeline-from-the-null-binary",
            [](Stream& s) { encodeGraphicsPipelineFromBinary(s, 0); }),
        contextLostRow("graphics-pipeline-from-a-binary-never-made",
            [](Stream& s) { encodeGraphicsPipelineFromBinary(s, neverMadeId); }),

        // An object id the guest already used.
        {"create-again-under-a-used-id", [](Session& session) {
            Stream first;
            encodeCreateCache(first, cacheId);
            require(session.send(first), "a pipeline cache was refused");
            Stream s;
            encodeCreateCache(s, cacheId);
            require(!session.send(s), "a second object under a used id was accepted");
            // The session's end destroys the context with both objects in it.
        }, true},

        // An object the context refuses is freed whichever owner is torn
        // down first.  Id 0 names no object and finds none in the table.
        {"command-buffer-under-id-0", [](Session& session) {
            Stream pool;
            encodeCreatePool(pool, poolId);
            require(session.send(pool), "a command pool was refused");
            Stream s;
            encodeAllocateCommandBuffer(s, poolId, 0);
            require(!session.send(s), "a command buffer under id 0 was accepted");
        }, true},
        // The older pool holds the duplicate and the newer one the original,
        // and the device tears the newer pool down first: the original leaves
        // the table before the duplicate is removed.
        {"command-buffer-in-another-pool-under-a-used-id", [](Session& session) {
            Stream pools;
            encodeCreatePool(pools, poolId);
            encodeCreatePool(pools, otherPoolId);
            require(session.send(pools), "a command pool was refused");
            Stream original;
            encodeAllocateCommandBuffer(original, otherPoolId, commandBufferId);
            require(session.send(original), "the first command buffer was refused");
            Stream duplicate;
            encodeAllocateCommandBuffer(duplicate, poolId, commandBufferId);
            require(!session.send(duplicate), "a second command buffer under a used id was accepted");
        }, true},
        // The same order in one pool: the original is removed with the pool
        // while the duplicate is still tracked by it.
        {"command-buffer-in-the-same-pool-under-a-used-id", [](Session& session) {
            Stream pool;
            encodeCreatePool(pool, poolId);
            require(session.send(pool), "a command pool was refused");
            Stream original;
            encodeAllocateCommandBuffer(original, poolId, commandBufferId);
            require(session.send(original), "the first command buffer was refused");
            Stream duplicate;
            encodeAllocateCommandBuffer(duplicate, poolId, commandBufferId);
            require(!session.send(duplicate), "a second command buffer under a used id was accepted");
        }, true},
    };
    return all;
}

} // namespace

int main(int argc, char** argv)
{
    const row_runner::Suite suite {rows(), encodeSetup, "pipeline binary"};
    return row_runner::main(argc, argv, suite);
}

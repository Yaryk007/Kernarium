// SPDX-License-Identifier: GPL-2.0-or-later

// A guest drives the Venus renderer, and in the product the renderer runs in
// the app's own process. vkCreatePipelineCache takes initial data, which the
// host driver parses when the cache is made and again when a pipeline looks an
// object up, as if its own serializer had written it; everything such a blob
// must carry is computable by the guest, and the parsers behind it are not
// written for hostile input. The renderer therefore makes every cache empty,
// whatever data comes with it, and each row sends the cache the way a guest
// does:
//
//  - data the guest was given and passes back, data under a header the guest
//    forged, a header cut short, an object the guest planted under a header
//    that is the host's own, and an object that claims more than the data
//    holds: the cache is made and works, the renderer says how many bytes it
//    ignored, and what the guest reads back of it is the blob of an empty
//    cache, so no planted byte is in it. A size that the data does not hold
//    is refused by the codec, which ends the context.
//  - what the guest reads back: the blob of an empty cache whatever the cache
//    has compiled since, a header the guest can store and pass back, and the
//    two-call size idiom: no buffer asks the size, a short buffer gets its
//    prefix and VK_INCOMPLETE, a larger one gets the whole blob and
//    VK_SUCCESS.
//  - vkMergePipelineCaches between caches of one context is served.
//
// The test drives the real vkr renderer in-process against the real host
// Vulkan driver and submits hand-encoded Venus command streams, in the style of
// venus_render_pass_lifetime_tests.cpp. Each row runs in a child process under
// Guard Malloc, as venus_row_runner.hpp describes.

#include "require.hpp"
#include "venus_pipeline_wire.hpp"
#include "venus_row_runner.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

using uml::testing::require;
using namespace uml::gpu::testing::venus;

// The object ids the guest assigns.
enum Id : std::uint64_t {
    instanceId = 1, physDevId = 2, deviceId = 3, cacheId = 10, otherCacheId = 11,
    layoutId = 20, pipelineId = 30, otherPipelineId = 31,
};

void encodeSetup(Stream& s)
{
    encodeInstanceAndDevice(s, VK_API_VERSION_1_3, instanceId, physDevId, deviceId);
}

// What a pipeline cache is made with. `declared` overrides the size the guest
// says the data has, and `noData` sends the size with no data behind it.
struct CacheData {
    std::vector<std::uint8_t> bytes;
    std::uint64_t declared = 0;
    bool useDeclared = false;
    bool noData = false;
};

void encodeCreateCache(Stream& s, std::uint64_t cache, const CacheData& data = {})
{
    s.command(createPipelineCache, true);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO); s.ptr(false);
    s.u32(0);                                   // flags
    s.u64(data.useDeclared ? data.declared : data.bytes.size());
    if (!data.noData && !data.bytes.empty()) {
        s.arraySize(data.bytes.size());
        s.blob(data.bytes.data(), data.bytes.size());
    } else {
        s.arraySize(0);
    }
    s.ptr(false);                               // pAllocator
    s.ptr(true); s.u64(cache);
}

// vkGetPipelineCacheData with a buffer of `size` bytes, or without one.
void encodeGetCacheData(Stream& s, std::uint64_t cache, std::uint64_t size, bool buffer)
{
    s.command(getPipelineCacheData, true);
    s.u64(deviceId);
    s.u64(cache);
    s.ptr(true); s.u64(size);
    s.arraySize(buffer ? size : 0);
}

void encodeMerge(Stream& s, std::uint64_t destination, std::uint64_t source)
{
    s.command(mergePipelineCaches, true);
    s.u64(deviceId);
    s.u64(destination);
    s.u32(1);
    s.arraySize(1); s.u64(source);
}

void encodeLayout(Stream& s, std::uint64_t layout)
{
    s.command(createPipelineLayout);
    s.u64(deviceId);
    s.ptr(true);
    s.u32(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO); s.u64(0);
    s.u32(0);
    s.u32(0); s.arraySize(0);
    s.u32(0); s.arraySize(0);
    s.ptr(false);
    s.ptr(true); s.u64(layout);
}

// vkCreateComputePipelines of the empty shader, through the cache.
void encodePipelineThrough(Stream& s, std::uint64_t cache, std::uint64_t pipeline)
{
    s.command(createComputePipelines, true);
    s.u64(deviceId);
    s.u64(cache);
    s.u32(1); s.arraySize(1);
    encodeComputeInfo(s, {.moduleInfo = true}, layoutId);
    s.ptr(false);
    s.arraySize(1); s.u64(pipeline);
}

// The reply of vkGetPipelineCacheData: the command, the result, the pointer
// to the size and the size, then the array: its size and its bytes.
constexpr std::size_t replySizeOffset = 16;
constexpr std::size_t replyBlobOffset = 32;

std::uint64_t sizeOf(const Answer& answer)
{
    std::uint64_t size = 0;
    std::memcpy(&size, answer.raw.data() + replySizeOffset, 8);
    return size;
}

std::vector<std::uint8_t> blobOf(const Answer& answer)
{
    const std::uint64_t size = std::min<std::uint64_t>(sizeOf(answer), answer.raw.size() - replyBlobOffset);
    return {answer.raw.begin() + replyBlobOffset, answer.raw.begin() + replyBlobOffset + size};
}

// The cache of the guest's id, made with nothing, and the blob it reads back.
std::vector<std::uint8_t> emptyBlob(Session& session)
{
    Stream make;
    encodeCreateCache(make, cacheId);
    requireAnswered(session.ask(make), createPipelineCache, VK_SUCCESS, "an empty cache was refused");
    Stream size;
    encodeGetCacheData(size, cacheId, 0, false);
    const Answer asked = session.ask(size);
    requireAnswered(asked, getPipelineCacheData, VK_SUCCESS, "the size of an empty cache was refused");
    Stream data;
    encodeGetCacheData(data, cacheId, sizeOf(asked), true);
    const Answer got = session.ask(data);
    requireAnswered(got, getPipelineCacheData, VK_SUCCESS, "the data of an empty cache was refused");
    return blobOf(got);
}

// The renderer's one line about data it did not use.
std::string ignoringLine(std::size_t bytes)
{
    return "ignoring the " + std::to_string(bytes) + " bytes of data the guest sent";
}

// A cache made with this data is made, works, and reads back as the empty
// cache did, the one the row made first: the header and no object, so nothing
// of what the guest sent.
void requireCacheIgnoresData(Session& session, const std::vector<std::uint8_t>& empty,
    const CacheData& data)
{
    Stream make;
    encodeCreateCache(make, otherCacheId, data);
    requireAnswered(session.ask(make), createPipelineCache, VK_SUCCESS,
        "a cache made with the guest's data was refused");

    Stream size;
    encodeGetCacheData(size, otherCacheId, 0, false);
    const Answer asked = session.ask(size);
    requireAnswered(asked, getPipelineCacheData, VK_SUCCESS, "the size of the cache was refused");
    require(sizeOf(asked) == empty.size(), "the cache reads back as more than an empty one");
    Stream read;
    encodeGetCacheData(read, otherCacheId, empty.size(), true);
    const Answer got = session.ask(read);
    requireAnswered(got, getPipelineCacheData, VK_SUCCESS, "the data of the cache was refused");
    require(blobOf(got) == empty, "the cache reads back as something other than an empty one");

    Stream layout;
    encodeLayout(layout, layoutId);
    require(session.send(layout), "the pipeline layout was refused");
    Stream pipeline;
    encodePipelineThrough(pipeline, otherCacheId, pipelineId);
    requireAnswered(session.ask(pipeline), createComputePipelines, VK_SUCCESS,
        "a pipeline through the cache was refused");
    if (!data.bytes.empty() || data.useDeclared) {
        require(rendererSaid(ignoringLine(data.useDeclared ? data.declared : data.bytes.size()).c_str()),
            "the renderer did not say it ignored the data");
    }
}

std::vector<std::uint8_t> withHeaderOf(const std::vector<std::uint8_t>& empty, std::size_t bytes)
{
    std::vector<std::uint8_t> blob(bytes, 0);
    std::copy_n(empty.begin(), std::min(bytes, empty.size()), blob.begin());
    return blob;
}

void put32(std::vector<std::uint8_t>& blob, std::uint32_t value)
{
    const std::size_t at = blob.size();
    blob.resize(at + 4);
    std::memcpy(blob.data() + at, &value, 4);
}

// The header of the host's own empty cache, then one object of a type of the
// guest's choice, under a key and with data it chose: the blob a guest can
// build from what it was given.
std::vector<std::uint8_t> plantedObject(const std::vector<std::uint8_t>& empty)
{
    std::vector<std::uint8_t> blob(empty.begin(), empty.begin() + 32);
    put32(blob, 1);                             // one object
    put32(blob, 0);                             // its type
    put32(blob, 32);                            // key size
    put32(blob, 24);                            // data size
    blob.insert(blob.end(), 32, 0xa5);          // key
    blob.insert(blob.end(), 24, 0x5a);          // data
    return blob;
}

const std::vector<Row>& rows()
{
    static const std::vector<Row> all = {
        // What the guest reads back of a cache.
        {"an-empty-cache-reads-back-a-header", [](Session& session) {
            const std::vector<std::uint8_t> empty = emptyBlob(session);
            require(empty.size() >= 32, "an empty cache reads back less than a header");
            std::uint32_t headerSize = 0, version = 0;
            std::memcpy(&headerSize, empty.data(), 4);
            std::memcpy(&version, empty.data() + 4, 4);
            require(headerSize == 32 && version == VK_PIPELINE_CACHE_HEADER_VERSION_ONE,
                "an empty cache reads back no header of version one");
        }},
        {"the-two-call-idiom-over-the-blob", [](Session& session) {
            const std::vector<std::uint8_t> empty = emptyBlob(session);
            Stream shortBuffer;
            encodeGetCacheData(shortBuffer, cacheId, 16, true);
            const Answer cut = session.ask(shortBuffer);
            requireAnswered(cut, getPipelineCacheData, VK_INCOMPLETE, "a short buffer was not answered incomplete");
            require(sizeOf(cut) == 16, "a short buffer was not told its size");
            require(blobOf(cut) == std::vector<std::uint8_t>(empty.begin(), empty.begin() + 16),
                "a short buffer did not get the start of the blob");
            Stream largeBuffer;
            encodeGetCacheData(largeBuffer, cacheId, 64, true);
            const Answer whole = session.ask(largeBuffer);
            requireAnswered(whole, getPipelineCacheData, VK_SUCCESS, "a large buffer was refused");
            require(sizeOf(whole) == empty.size() && blobOf(whole) == empty,
                "a large buffer did not get the whole blob and its size");
        }},
        // What a cache that has compiled a pipeline reads back is no larger.
        {"a-cache-that-compiled-a-pipeline-reads-back-no-more", [](Session& session) {
            const std::vector<std::uint8_t> empty = emptyBlob(session);
            Stream layout;
            encodeLayout(layout, layoutId);
            require(session.send(layout), "the pipeline layout was refused");
            Stream pipeline;
            encodePipelineThrough(pipeline, cacheId, pipelineId);
            requireAnswered(session.ask(pipeline), createComputePipelines, VK_SUCCESS,
                "a pipeline through the cache was refused");
            Stream size;
            encodeGetCacheData(size, cacheId, 0, false);
            const Answer asked = session.ask(size);
            requireAnswered(asked, getPipelineCacheData, VK_SUCCESS, "the size of the cache was refused");
            require(sizeOf(asked) == empty.size(),
                "a cache that compiled a pipeline reads back more than a header");
        }},

        // Data the guest sends with a cache.
        {"data-the-guest-was-given-passed-back", [](Session& session) {
            const std::vector<std::uint8_t> empty = emptyBlob(session);
            requireCacheIgnoresData(session, empty, {.bytes = empty});
        }},
        {"data-under-a-forged-header", [](Session& session) {
            const std::vector<std::uint8_t> empty = emptyBlob(session);
            std::vector<std::uint8_t> forged(64, 0xab);
            requireCacheIgnoresData(session, empty, {.bytes = forged});
        }},
        {"a-header-cut-short", [](Session& session) {
            const std::vector<std::uint8_t> empty = emptyBlob(session);
            requireCacheIgnoresData(session, empty, {.bytes = withHeaderOf(empty, 16)});
        }},
        {"an-object-planted-under-the-hosts-header", [](Session& session) {
            const std::vector<std::uint8_t> empty = emptyBlob(session);
            requireCacheIgnoresData(session, empty, {.bytes = plantedObject(empty)});
        }},
        {"an-object-that-claims-more-than-it-has", [](Session& session) {
            const std::vector<std::uint8_t> empty = emptyBlob(session);
            std::vector<std::uint8_t> blob(empty.begin(), empty.begin() + 32);
            put32(blob, 1000);                  // objects
            put32(blob, 0);
            put32(blob, 0x10000000);            // key size
            put32(blob, 0x10000000);            // data size
            requireCacheIgnoresData(session, empty, {.bytes = blob});
        }},
        // A size that the data does not hold is refused by the codec, which
        // decodes the array as long as the size says, before the renderer
        // sees the command: the context is lost, as for any array that is not
        // as long as its size.
        {"a-size-with-no-data-ends-the-context", [](Session& session) {
            Stream make;
            encodeCreateCache(make, cacheId, {.declared = 4096, .useDeclared = true, .noData = true});
            require(!session.ask(make).submitted, "a size with no data behind it was served");
        }},
        {"a-size-longer-than-the-data-ends-the-context", [](Session& session) {
            Stream make;
            encodeCreateCache(make, cacheId, {.bytes = std::vector<std::uint8_t>(36, 0), .declared = 4096,
                .useDeclared = true});
            require(!session.ask(make).submitted, "a size longer than its data was served");
        }},

        // Caches of one context merge.
        {"caches-of-one-context-merge", [](Session& session) {
            const std::vector<std::uint8_t> empty = emptyBlob(session);
            Stream layout;
            encodeLayout(layout, layoutId);
            require(session.send(layout), "the pipeline layout was refused");
            Stream pipeline;
            encodePipelineThrough(pipeline, cacheId, pipelineId);
            requireAnswered(session.ask(pipeline), createComputePipelines, VK_SUCCESS,
                "a pipeline through the cache was refused");
            Stream other;
            encodeCreateCache(other, otherCacheId, {.bytes = plantedObject(empty)});
            requireAnswered(session.ask(other), createPipelineCache, VK_SUCCESS,
                "a cache made with the guest's data was refused");
            Stream merge;
            encodeMerge(merge, cacheId, otherCacheId);
            requireAnswered(session.ask(merge), mergePipelineCaches, VK_SUCCESS, "the merge was refused");
            Stream again;
            encodePipelineThrough(again, cacheId, otherPipelineId);
            requireAnswered(session.ask(again), createComputePipelines, VK_SUCCESS,
                "a pipeline through the merged cache was refused");
            Stream size;
            encodeGetCacheData(size, cacheId, 0, false);
            const Answer asked = session.ask(size);
            requireAnswered(asked, getPipelineCacheData, VK_SUCCESS, "the size of the merged cache was refused");
            require(sizeOf(asked) == empty.size(), "the merged cache reads back as more than an empty one");
        }},

        // The commands that name a cache.
        {"data-of-the-null-cache", [](Session& session) {
            Stream s;
            encodeGetCacheData(s, 0, 0, false);
            require(!session.ask(s).submitted, "the data of the null cache was answered");
        }},
        {"data-of-a-cache-never-made", [](Session& session) {
            Stream s;
            encodeGetCacheData(s, 77, 0, false);
            require(!session.ask(s).submitted, "the data of a cache never made was answered");
        }},
    };
    return all;
}

} // namespace

int main(int argc, char** argv)
{
    const row_runner::Suite suite {rows(), encodeSetup, "pipeline cache"};
    return row_runner::main(argc, argv, suite);
}

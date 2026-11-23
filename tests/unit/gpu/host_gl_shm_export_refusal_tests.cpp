// SPDX-License-Identifier: GPL-2.0-or-later

// A texture the guest hands to another context is exported through a shared
// memory segment, and the renderer copies what GL drew into the segment before
// the importer can look at it: when the importer asks for the segment, and at
// the next fence after a draw. The copy is a read-back, which the host driver
// can refuse when its memory budget is full.
//
// The contract under test, for a read-back the host refused:
//
//  - at a fence it is not forgotten. The texture stays the newer copy, and
//    the next fence writes it into the segment once the memory is back, so
//    the importer never keeps an older picture than GL holds for longer than
//    the budget stayed full; and
//  - at the first export it is not made up for later. The importer may write
//    the segment before any fence, as a Venus producer does, and GL's older
//    texture must not replace that frame, in GL's own reads or in the
//    segment.

#include "host_gl_stream_support.hpp"
#include "host_gl_test_support.hpp"
#include "host_gpu_memory.hpp"
#include "require.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace uml::gpu::testing::stream;
using uml::gpu::testing::skipWithoutRenderer;
using uml::testing::require;
using namespace std::chrono_literals;

constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
constexpr std::uint64_t roomBytes = 192 * mebibyte;
// 16 MiB: a read-back stages the whole texture, which is more than the
// uploader's small buffers and so comes from the budget itself.
constexpr std::uint32_t side = 2048;
constexpr std::uint32_t commandClear = 7;
constexpr std::uint32_t exportedId = 20;
constexpr std::uint32_t firstExportId = 21;
constexpr std::uint32_t firstFillerId = 1000;

// Set when the renderer hands out no segment for a texture.
bool exportUnavailable = false;

// The room the renderer's budget has left before it refuses.
[[nodiscard]] std::uint64_t budgetRoom()
{
    const auto memory = uml::gpu::VirglRenderer::deviceMemory();
    require(memory.has_value(), "the renderer has no device memory budget");
    return memory->ceilingBytes > memory->usageBytes
        ? memory->ceilingBytes - memory->usageBytes
        : 0;
}

[[nodiscard]] std::uint64_t used() noexcept
{
    return std::max(uml::platform::hostGpuAllocatedBytes(),
        uml::platform::hostGraphicsFootprintBytes());
}

struct Color {
    float red;
    float green;
    float blue;
    std::array<std::uint8_t, 3> bytes;
};

// The segment's first row, where a clear to this colour shows.
constexpr Color first {0.25F, 0.5F, 0.75F, {64, 128, 191}};
constexpr Color second {0.9F, 0.1F, 0.4F, {230, 26, 102}};

[[nodiscard]] bool near(const std::uint8_t* pixel, const Color& color)
{
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const int difference = static_cast<int>(pixel[channel]) -
            static_cast<int>(color.bytes[channel]);
        if (difference > 2 || difference < -2)
            return false;
    }
    return true;
}

// Draws the colour over the whole texture without waiting: no fence follows.
void clear(uml::gpu::VirglRenderer& renderer, std::uint32_t texture,
    std::uint32_t surface, const Color& color)
{
    Commands commands;
    commands.add(commandCreateObject, objectSurface,
        {surface, texture, virglFormatR8g8b8a8Unorm, 0, 0});
    commands.add(commandSetFramebufferState, 0, {1, 0, surface});
    commands.add(commandClear, 0,
        {4U, bits(color.red), bits(color.green), bits(color.blue), bits(1.0F),
            0, 0, 0});
    require(commands.submit(renderer) == 0, "the clear was refused");
}

void fence(uml::gpu::VirglRenderer& renderer, std::uint64_t& counter)
{
    require(renderer.createFenceAndWait(contextId, 0, ++counter, nullptr) == 0,
        "the fence did not complete");
}

// A segment of an exported texture, mapped for the importer: read and written.
class Segment final {
public:
    explicit Segment(int descriptor)
    {
        struct stat info {};
        require(::fstat(descriptor, &info) == 0, "the segment has no size");
        size_ = static_cast<std::size_t>(info.st_size);
        mapping_ = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED,
            descriptor, 0);
        require(mapping_ != MAP_FAILED, "the segment could not be mapped");
        ::close(descriptor);
    }

    ~Segment() { ::munmap(mapping_, size_); }
    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    [[nodiscard]] const std::uint8_t* pixel(std::size_t x, std::size_t y) const
    {
        // The rows are the texture's width, which is a whole number of 16 byte
        // groups already, and four bytes a pixel.
        return static_cast<const std::uint8_t*>(mapping_) +
            (y * side + x) * 4U;
    }

    // The importer renders a whole frame of one colour into the segment.
    void fill(const Color& color)
    {
        auto* bytes = static_cast<std::uint8_t*>(mapping_);
        for (std::size_t offset = 0; offset + 4U <= size_; offset += 4U) {
            bytes[offset] = color.bytes[0];
            bytes[offset + 1] = color.bytes[1];
            bytes[offset + 2] = color.bytes[2];
            bytes[offset + 3] = 255;
        }
    }

private:
    void* mapping_ = nullptr;
    std::size_t size_ = 0;
};

// Exports a texture and maps its segment; null on a host that does not export
// a texture this way, where the test is skipped.
[[nodiscard]] std::unique_ptr<Segment> exportTexture(
    uml::gpu::VirglRenderer& renderer, std::uint32_t id)
{
    std::uint32_t type = 0;
    int descriptor = -1;
    if (renderer.exportResourceBlob(id, type, descriptor) != 0 ||
        descriptor < 0)
        return nullptr;
    return std::make_unique<Segment>(descriptor);
}

void createTexture(uml::gpu::VirglRenderer& renderer, std::uint32_t id)
{
    require(renderer.createResource3d(
                uml::gpu::VirglRenderer::Resource3d {
                    .resourceId = id,
                    .target = pipeTexture2d,
                    .format = virglFormatR8g8b8a8Unorm,
                    .bind = virglBindRenderTarget | virglBindSamplerView,
                    .width = side,
                    .height = side,
                    .depth = 1,
                    .arraySize = 1,
                }) == 0,
        "the texture could not be created");
    require(renderer.attachResource(contextId, id) == 0,
        "the texture could not be attached");
}

// Fills the budget with textures until what is left is less than a read-back's
// staging buffer and more than the small allocations that follow need (the
// host driver takes those from blocks of a few megabytes).
[[nodiscard]] std::vector<std::uint32_t> fillBudget(
    uml::gpu::VirglRenderer& renderer)
{
    struct Step {
        std::uint32_t side;
        std::uint64_t stopBelow;
    };
    std::vector<std::uint32_t> fillers;
    std::uint32_t fillerId = firstFillerId;
    for (const Step step : {Step {2048, 30 * mebibyte},
             Step {1024, 14 * mebibyte}}) {
        while (budgetRoom() > step.stopBelow && fillers.size() < 128) {
            if (renderer.createResource3d(
                    uml::gpu::VirglRenderer::Resource3d {
                        .resourceId = fillerId,
                        .target = pipeTexture2d,
                        .format = virglFormatR8g8b8a8Unorm,
                        .bind = virglBindRenderTarget | virglBindSamplerView,
                        .width = step.side,
                        .height = step.side,
                        .depth = 1,
                        .arraySize = 1,
                    }) != 0)
                break;
            fillers.push_back(fillerId++);
        }
    }
    std::fprintf(stderr,
        "filled the budget with %zu textures, %.1f MiB of room left\n",
        fillers.size(), static_cast<double>(budgetRoom()) / mebibyte);
    return fillers;
}

// Releases the fillers and waits until the budget has room for two read-backs
// again. The host driver frees a released texture when its context next reuses
// the batch state of the texture's last use, so the contexts are trimmed as
// they would be once idle, and the drivers' caches are given back with them.
void releaseFillers(uml::gpu::VirglRenderer& renderer, std::uint64_t& counter,
    std::vector<std::uint32_t>& fillers)
{
    for (const std::uint32_t id : fillers)
        renderer.unrefResource(id);
    fillers.clear();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        fence(renderer, counter);
        if (const auto delay = renderer.nextIdleTrimDelay())
            std::this_thread::sleep_for(*delay + 5ms);
        renderer.trimIdleContexts();
        static_cast<void>(uml::gpu::VirglRenderer::reclaimCachedMemory());
        if (budgetRoom() >= 2 * (side * side * 4ULL))
            return;
        require(std::chrono::steady_clock::now() < deadline,
            "the budget did not count the released textures");
    }
}

// The first export is refused its read-back, and the importer then renders a
// frame into the segment before any fence: GL must show that frame and the
// segment must keep it.
[[nodiscard]] bool checkFirstExport(uml::gpu::VirglRenderer& renderer,
    Pipeline& pipeline, std::uint64_t& counter, std::uint32_t& surface)
{
    createTexture(renderer, firstExportId);
    // GL drew the texture before the importer asked for it.
    clear(renderer, firstExportId, surface++, first);
    fence(renderer, counter);

    std::vector<std::uint32_t> fillers = fillBudget(renderer);
    const std::unique_ptr<Segment> segment =
        exportTexture(renderer, firstExportId);
    if (!segment) {
        exportUnavailable = true;
        return true;
    }
    // A segment is fresh memory: the picture here means the read-back ran.
    require(!near(segment->pixel(7, 9), first),
        "the first read-back was not refused, so nothing was proven");

    // The importer renders its frame while the budget is still full, and GL
    // samples it; the room comes back only afterwards.
    segment->fill(second);
    const std::uint32_t view =
        pipeline.createView(firstExportId, virglFormatR8g8b8a8Unorm, 0);
    const std::vector<std::uint8_t> texels = pipeline.sample(view, 64, 64);
    const bool sawFrame = near(&texels[(10U * 64U + 10U) * 4U], second);
    releaseFillers(renderer, counter, fillers);
    fence(renderer, counter);
    const bool keptFrame = near(segment->pixel(7, 9), second) &&
        near(segment->pixel(side - 1, side - 1), second);
    std::fprintf(stderr,
        "first export refused: GL shows the importer's frame: %s, the segment "
        "keeps it: %s\n",
        sawFrame ? "yes" : "no", keptFrame ? "yes" : "no");
    if (!sawFrame || !keptFrame) {
        std::fputs("Shm export refusal: a texture GL drew before its first "
                   "export replaced the importer's frame\n",
            stderr);
        return false;
    }
    return true;
}

// A read-back at a fence is refused after GL drew a new colour, and written
// once the memory is back.
[[nodiscard]] bool checkFence(uml::gpu::VirglRenderer& renderer,
    std::uint64_t& counter, std::uint32_t& surface)
{
    createTexture(renderer, exportedId);
    const std::unique_ptr<Segment> segment =
        exportTexture(renderer, exportedId);
    if (!segment) {
        exportUnavailable = true;
        return true;
    }

    // The machinery: a clear followed by a fence reaches the segment.
    clear(renderer, exportedId, surface++, first);
    fence(renderer, counter);
    require(near(segment->pixel(7, 9), first),
        "a clear and a fence did not reach the segment");

    // GL draws a new colour; the segment still has the old one.
    clear(renderer, exportedId, surface++, second);

    std::vector<std::uint32_t> fillers = fillBudget(renderer);

    // The importer asks for the segment again: the renderer hands out what GL
    // drew, which needs the read-back the full budget refuses.
    int again = -1;
    std::uint32_t againType = 0;
    const int exported =
        renderer.exportResourceBlob(exportedId, againType, again);
    if (again >= 0)
        ::close(again);
    require(exported == 0, "the second export failed");
    require(near(segment->pixel(7, 9), first),
        "the read-back was not refused, so nothing was proven");

    releaseFillers(renderer, counter, fillers);

    // With the memory back, the next fence writes the texture into the segment.
    fence(renderer, counter);
    const bool written = near(segment->pixel(7, 9), second) &&
        near(segment->pixel(side - 1, side - 1), second);
    std::fprintf(stderr, "segment after the refusal and a fence: %s\n",
        written ? "the newer picture" : "the older picture");
    if (!written) {
        std::fputs("Shm export refusal: a refused read-back was dropped\n",
            stderr);
        return false;
    }
    return true;
}

} // namespace

int main()
{
    // A context is idle, and trimmed, 20 ms after its last use.
    ::setenv("UML_GPU_IDLE_TRIM_MS", "20", 1);
    uml::gpu::VirglRenderer::setDeviceMemoryLimit(used() + roomBytes);
    std::unique_ptr<uml::gpu::VirglRenderer> renderer;
    try {
        renderer = std::make_unique<uml::gpu::VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }
    require(renderer->createContext(contextId, virgl2CapsetId, "shm export") ==
            0,
        "the guest context could not be created");
    Pipeline pipeline(*renderer);

    std::uint64_t counter = 1000;
    std::uint32_t surface = firstTransientHandle + 100;
    bool ok = checkFirstExport(*renderer, pipeline, counter, surface);
    ok = checkFence(*renderer, counter, surface) && ok;
    if (exportUnavailable)
        return uml::gpu::testing::skip(
            "this host does not export a texture through a segment");
    if (!ok)
        return 1;
    std::fputs("Shm export refusal: ok\n", stderr);
    return 0;
}

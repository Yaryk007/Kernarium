// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "require.hpp"
#include "shaders/transform_feedback_passes_spirv.hpp"

#import <Metal/Metal.h>
#import <objc/runtime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/* A GPU particle simulation, as a GL application on a virtualized stack
 * submits it, steps its particles by transform feedback with the rasterizer
 * discarded: every step is a render pass over the frame's attachment (loaded
 * and stored, never drawn into), one discard draw that captures the next
 * state, and a barrier. A frame can hold hundreds of them, and the last pass
 * draws the particles.
 *
 * On a Metal backend the draw that captures runs compute (the vertex stage)
 * before its render draw (the capture's stores), and a compute encoder that
 * follows a render encoder's start cannot share its command buffer's place: it
 * gets a command buffer of its own, and the render pass is split to put the
 * compute before it. A command buffer that starts with a render encoder
 * allocates 2.5 MiB of command storage, so hundreds of passes recorded that
 * way are over a gigabyte in one recording, held until the GPU finishes it.
 * The driver instead starts the encoder of a pass whose attachments are all
 * loaded at its first draw, after the compute that draw launches, and stops it
 * for the counter copies around a capture. A recording's passes then share one
 * main command buffer, in which each further encoder costs a few hundred KiB.
 *
 * The guard counts what that costs instead of timing it. It replaces three
 * selectors of the Metal 4 command buffer class (beginning a command buffer,
 * and making a render or a compute encoder) and the allocator's reset, which
 * the driver calls once the GPU has finished with an allocator's storage; the
 * allocator's size at that reset is the storage the recording held. The counts
 * depend on what the driver records, not on how busy the host is.
 *
 * The same chains pin what the late start and the stops must not change: the
 * capture, the pixels, the depth, and the queries that span them. */

namespace {

using uml::gpu::test::ChainOp;
using uml::gpu::test::DrawContext;
using uml::gpu::test::DrawExtensions;
using uml::gpu::test::FeedbackChainConfig;
using uml::gpu::test::FeedbackChainResult;
using uml::testing::require;
using Kind = ChainOp::Kind;

constexpr uint32_t kExtent = 16;
constexpr auto kCompletionDeadline = std::chrono::seconds(30);
/* Metal command buffers in a recording: a few, however many passes it has.
 * The variants record in 1, the long chain in 3 (its statistics query adds two
 * of its own); a driver that opens one per pass records hundreds. */
constexpr uint32_t kMaxCommandBuffers = 4;
/* The largest allocator when the GPU is done with the recording. Command
 * storage still grows with the passes, at about 0.6 MiB a pass (a render and a
 * compute encoder that share a command buffer): the 129-pass chain holds 86
 * MiB, and the bound leaves half again for the Metal versions and devices that
 * size encoders differently. The driver that opened a command buffer per pass
 * held 656 MiB for the same chain. The 25-pass variants hold 18 to 20 MiB
 * against 130 MiB. */
constexpr uint64_t kMaxChainAllocatorBytes = 128ull << 20;
constexpr uint64_t kMaxVariantAllocatorBytes = 32ull << 20;
/* The geometry heap the driver starts with, in MiB. A rasterizing draw of
 * 250000 uint8 indices reserves 1000000 bytes of it for its unrolled indices,
 * and an indexed capture before it in the same pass reserves a few hundred KiB
 * more: the two do not fit together, and the driver knows the first draw's
 * share only by the segment's accounting, which has to span them to grow the
 * heap for both. */
constexpr const char *kHeapMiB = "1";
constexpr uint32_t kHeapCaptureIndices = 4000;
constexpr uint32_t kHeapRasterIndices = 250000;

struct Counters {
    std::atomic<uint32_t> commandBuffers{0};
    std::atomic<uint32_t> renderEncoders{0};
    std::atomic<uint32_t> computeEncoders{0};
};
Counters counters;

/* The allocators a recording began command buffers on, and for each when it
 * was last begun on, last reset and how large it was at that reset. */
struct AllocatorInfo {
    uint64_t beganAt = 0;
    uint64_t resetAt = 0;
    uint64_t resetSize = 0;
};
std::mutex mutex;
std::map<const void *, AllocatorInfo> allocators;
std::set<const void *> inRecording;
uint64_t sequence = 0;
bool armed = false;

IMP beginImplementation = nullptr;
IMP renderImplementation = nullptr;
IMP computeImplementation = nullptr;
IMP resetImplementation = nullptr;

/* The replacements forward to the originals through untyped function
 * pointers, with the objects that cross the call as plain pointers. ARC
 * still retains and releases each receiver around a replacement, in a
 * balanced pair. The two make-selectors return their encoder as it came, an
 * autoreleased +0 reference that a plain pointer passes on without a retain
 * or a release, and the descriptor and the allocator are borrowed. The reset
 * of an allocator reads the receiver as an object. */
void countedBegin(id receiver, SEL command, void *allocator)
{
    counters.commandBuffers.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(mutex);
        allocators[allocator].beganAt = ++sequence;
        if (armed)
            inRecording.insert(allocator);
    }
    reinterpret_cast<void (*)(id, SEL, void *)>(beginImplementation)(receiver, command, allocator);
}

void *countedRender(id receiver, SEL command, void *descriptor)
{
    counters.renderEncoders.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<void *(*)(id, SEL, void *)>(renderImplementation)(receiver, command,
                                                                              descriptor);
}

void *countedCompute(id receiver, SEL command)
{
    counters.computeEncoders.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<void *(*)(id, SEL)>(computeImplementation)(receiver, command);
}

void countedReset(id receiver, SEL command)
{
    id<MTL4CommandAllocator> allocator = (id<MTL4CommandAllocator>)receiver;
    const uint64_t before = [allocator allocatedSize];
    reinterpret_cast<void (*)(id, SEL)>(resetImplementation)(receiver, command);
    const uint64_t size = std::max<uint64_t>(before, [allocator allocatedSize]);
    std::lock_guard lock(mutex);
    AllocatorInfo &info = allocators[(__bridge const void *)receiver];
    info.resetAt = ++sequence;
    info.resetSize = size;
}

Method method_of(id object, SEL selector, const char *what)
{
    Method method = class_getInstanceMethod(object_getClass(object), selector);
    require(method != nullptr, std::string("the Metal object lacks ") + what);
    return method;
}

/* The classes the default device's command buffers and allocators belong to
 * are learned from samples made before the replacement. */
void count_metal()
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    require(device != nil, "no Metal device");
    id<MTL4CommandBuffer> buffer = [device newCommandBuffer];
    id<MTL4CommandAllocator> allocator = [device newCommandAllocator];
    require(buffer != nil && allocator != nil, "the device made no Metal 4 command buffer");
    beginImplementation = method_setImplementation(
        method_of(buffer, @selector(beginCommandBufferWithAllocator:), "begin"),
        reinterpret_cast<IMP>(countedBegin));
    renderImplementation = method_setImplementation(
        method_of(buffer, @selector(renderCommandEncoderWithDescriptor:), "render encoders"),
        reinterpret_cast<IMP>(countedRender));
    computeImplementation = method_setImplementation(
        method_of(buffer, @selector(computeCommandEncoder), "compute encoders"),
        reinterpret_cast<IMP>(countedCompute));
    resetImplementation = method_setImplementation(
        method_of(allocator, @selector(reset), "reset"), reinterpret_cast<IMP>(countedReset));
}

struct Spent {
    uint32_t commandBuffers = 0;
    uint32_t renderEncoders = 0;
    uint32_t computeEncoders = 0;
    uint32_t allocators = 0;
    uint64_t largestAllocator = 0;
};

/* Records and runs the chain, then waits for the driver to hand back every
 * allocator the recording used: the point at which its storage is known. */
Spent run(DrawContext &context, FeedbackChainConfig config, FeedbackChainResult &result)
{
    config.step_vertex_shader = feedback_step_vertex;
    config.raster_vertex_shader = feedback_raster_vertex;
    config.raster_fragment_shader = feedback_raster_fragment;
    Spent spent;
    uint32_t commandBuffers = 0, renderEncoders = 0, computeEncoders = 0;
    config.recording_begins = [&] {
        {
            std::lock_guard lock(mutex);
            inRecording.clear();
            armed = true;
        }
        commandBuffers = counters.commandBuffers.load();
        renderEncoders = counters.renderEncoders.load();
        computeEncoders = counters.computeEncoders.load();
    };
    config.recording_complete = [&] {
        {
            std::lock_guard lock(mutex);
            armed = false;
        }
        spent.commandBuffers = counters.commandBuffers.load() - commandBuffers;
        spent.renderEncoders = counters.renderEncoders.load() - renderEncoders;
        spent.computeEncoders = counters.computeEncoders.load() - computeEncoders;
    };
    result = context.feedback_chain(config);

    const auto deadline = std::chrono::steady_clock::now() + kCompletionDeadline;
    for (;;) {
        bool done = true;
        {
            std::lock_guard lock(mutex);
            spent.allocators = uint32_t(inRecording.size());
            spent.largestAllocator = 0;
            for (const void *key : inRecording) {
                const AllocatorInfo &info = allocators[key];
                if (info.resetAt < info.beganAt) {
                    done = false;
                    continue;
                }
                spent.largestAllocator = std::max(spent.largestAllocator, info.resetSize);
            }
        }
        if (done)
            break;
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("the recording's command storage was not handed back");
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return spent;
}

using Pixel = std::array<uint8_t, 4>;

/* What a chain must have produced, besides what its configuration says. */
struct Expect {
    /* A command cleared the whole attachment blue. */
    bool blueBackground = false;
    /* The draws that paint the row of points, whatever else they capture. */
    uint32_t rasterizingDraws = 1;
    /* Samples the draws outside the row of points count. */
    uint64_t extraSamples = 0;
    /* The statistics query counts the default chain's draws. */
    bool defaultChain = false;
};

uint32_t step_word(uint32_t word, uint32_t steps)
{
    uint32_t value = word + 1u;
    for (uint32_t step = 0; step < steps; ++step)
        value = value * 3u + (word % 4) + 1u;
    return value;
}

/* What the chain must have produced. Every capture maps a word x to 3x + c
 * with c = 1, 2, 3, 4 for the four words of a vertex, and the seed is 1, 2,
 * 3, ... */
void check_results(const FeedbackChainConfig &config, const FeedbackChainResult &result,
                   const Expect &expect, const std::string &what)
{
    require(result.captured.size() == size_t(config.points) * 4,
            what + ": the capture has the wrong size");
    for (size_t word = 0; word < result.captured.size(); ++word) {
        const uint32_t value = step_word(uint32_t(word), result.captures);
        require(result.captured[word] == value,
                what + ": word " + std::to_string(word) + " captured " +
                    std::to_string(result.captured[word]) + ", expected " + std::to_string(value));
    }

    /* The discard passes leave the attachment as it was, whatever painted it
     * last; the rasterizing draws add one green point per vertex on row 8. */
    const Pixel background = expect.blueBackground ? Pixel{0, 0, 255, 255} : Pixel{0, 0, 0, 255};
    const Pixel point{0, 255, 0, 255};
    require(result.pixels.size() == size_t(kExtent) * kExtent, what + ": wrong pixel count");
    for (uint32_t y = 0; y < kExtent; ++y) {
        for (uint32_t x = 0; x < kExtent; ++x) {
            const Pixel &got = result.pixels[size_t(y) * kExtent + x];
            const Pixel &want = (y == 8 && x < config.points) ? point : background;
            require(got == want,
                    what + ": pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") is " +
                        std::to_string(got[0]) + "," + std::to_string(got[1]) + "," +
                        std::to_string(got[2]) + "," + std::to_string(got[3]));
        }
    }
    if (config.depth) {
        /* The rasterizing draws write depth 0 where they paint; the discard
         * passes loaded and stored the cleared 1 everywhere else. */
        require(result.depth.size() == size_t(kExtent) * kExtent, what + ": wrong depth count");
        for (uint32_t y = 0; y < kExtent; ++y) {
            for (uint32_t x = 0; x < kExtent; ++x) {
                const float got = result.depth[size_t(y) * kExtent + x];
                const float want = (y == 8 && x < config.points) ? 0.0f : 1.0f;
                require(got == want,
                        what + ": depth (" + std::to_string(x) + ", " + std::to_string(y) +
                            ") is " + std::to_string(got));
            }
        }
    }

    /* The query spans the passes and counts the samples of the draws that
     * rasterize. A boolean one reads some non-zero value. */
    const uint64_t samples =
        uint64_t(config.points) * uint32_t(config.samples) * expect.rasterizingDraws +
        expect.extraSamples;
    if (config.precise_occlusion) {
        require(result.occlusion_samples == samples,
                what + ": the occlusion query counted " +
                    std::to_string(result.occlusion_samples) + " samples, expected " +
                    std::to_string(samples));
    } else {
        require(result.occlusion_samples != 0,
                what + ": the boolean occlusion query read zero for visible draws");
    }
    if (!result.statistics.empty()) {
        /* Input assembly vertices, vertex shader invocations: every draw
         * assembles and shades its vertices, discarded or not. */
        require(expect.defaultChain, what + ": the statistics need the default chain");
        const uint64_t vertices = uint64_t(config.points) * (config.steps + 2);
        require(result.statistics.size() == 2 && result.statistics[0] == vertices &&
                    result.statistics[1] == vertices,
                what + ": the statistics query counted " + std::to_string(result.statistics[0]) +
                    " and " + std::to_string(result.statistics[1]) + ", expected " +
                    std::to_string(vertices));
    }
}

/* What a HeapCapture captured: each of its uvec4 maps a word x of the chain's
 * state when it ran to 3x + c, as a capture does. */
void check_heap_capture(const FeedbackChainConfig &config, const FeedbackChainResult &result,
                        const std::string &what)
{
    require(result.heap_captured.size() == size_t(kHeapCaptureIndices) * 4,
            what + ": the capture has the wrong size");
    for (size_t word = 0; word < result.heap_captured.size(); ++word) {
        const size_t vertex = (word / 4) % config.points;
        const uint32_t input =
            step_word(uint32_t(vertex * 4 + word % 4), result.heap_captures);
        const uint32_t expected = input * 3u + uint32_t(word % 4) + 1u;
        require(result.heap_captured[word] == expected,
                what + ": word " + std::to_string(word) + " captured " +
                    std::to_string(result.heap_captured[word]) + ", expected " +
                    std::to_string(expected));
    }
}

/* Timestamps written in recording order read increasing times, each its own.
 * Vulkan needs less where no command lies between two writes, which may read
 * one time or either order, but the driver keeps recording order for them and
 * the cases that have no command between them guard that. */
void check_timestamps(const FeedbackChainResult &result, size_t count, const std::string &what)
{
    require(result.timestamps.size() == count, what + ": wrong number of timestamps");
    std::string values;
    for (uint64_t value : result.timestamps)
        values += " " + std::to_string(value);
    for (size_t i = 0; i < count; ++i) {
        require(result.timestamps[i] != 0, what + ": timestamp " + std::to_string(i) +
                                               " was never written:" + values);
        require(i == 0 || result.timestamps[i] > result.timestamps[i - 1],
                what + ": timestamp " + std::to_string(i) + " does not follow the one before:" +
                    values);
    }
}

void report(const char *what, const FeedbackChainConfig &config, const Spent &spent)
{
    std::printf("transform feedback passes: %s, %u passes: %u command buffers, %u render and %u "
                "compute encoders, %u allocators, largest %.2f MiB\n",
                what, config.steps + 1, spent.commandBuffers, spent.renderEncoders,
                spent.computeEncoders, spent.allocators,
                double(spent.largestAllocator) / double(1 << 20));
    std::fflush(stdout);
}

void check_spent(const char *what, const FeedbackChainConfig &config, const Spent &spent,
                 uint64_t maxAllocatorBytes)
{
    report(what, config, spent);
    require(spent.commandBuffers <= kMaxCommandBuffers,
            std::string(what) + ": " + std::to_string(spent.commandBuffers) +
                " Metal command buffers, over the bound of " +
                std::to_string(kMaxCommandBuffers));
    require(spent.largestAllocator <= maxAllocatorBytes,
            std::string(what) + ": an allocator held " +
                std::to_string(spent.largestAllocator >> 20) + " MiB when the GPU finished, over " +
                std::to_string(maxAllocatorBytes >> 20) + " MiB");
}

ChainOp stamp(VkPipelineStageFlags2 stage, uint32_t query)
{
    return {Kind::Timestamp, stage, query};
}

/* A chain of 25 passes, the variants' size. */
FeedbackChainConfig variant()
{
    FeedbackChainConfig config;
    config.steps = 24;
    return config;
}

/* Runs a variant chain and checks its results and, unless told not to, the
 * storage it cost. */
FeedbackChainResult run_variant(DrawContext &context, const char *what,
                                const FeedbackChainConfig &config, const Expect &expect,
                                bool boundStorage = true)
{
    FeedbackChainResult result;
    const Spent spent = run(context, config, result);
    check_results(config, result, expect, what);
    if (boundStorage)
        check_spent(what, config, spent, kMaxVariantAllocatorBytes);
    else
        report(what, config, spent);
    std::printf("PASS transform feedback passes: %s\n", what);
    std::fflush(stdout);
    return result;
}

} // namespace

int main(int argc, char **argv)
{
    /* With an argument, only the scenarios whose name contains it run: a
     * failure that appears on a loaded host is run alone, and one that
     * aborts the binary does not hide the scenarios after it. */
    const auto selected = [&](std::string_view name) {
        return argc < 2 || name.find(argv[1]) != std::string_view::npos;
    };
    try {
        /* The geometry heap starts at 1 MiB for the whole run; see kHeapMiB. */
        setenv("MESA_KK_HEAP_SIZE_MIB", kHeapMiB, 1);
        count_metal();
        DrawExtensions extensions;
        extensions.index_type_uint8 = true;
        DrawContext context(false, true, false, extensions);
        require(context.capabilities().driver_id == VK_DRIVER_ID_MESA_KOSMICKRISP,
                "the guard counts the KosmicKrisp driver's Metal calls");
        require(context.capabilities().transform_feedback, "transform feedback unavailable");
        const bool statistics = context.capabilities().pipeline_statistics;

        /* The burst: a long chain with every query live over it. */
        if (selected("chain")) {
            FeedbackChainConfig config;
            config.steps = 128;
            config.statistics_query = statistics;
            FeedbackChainResult result;
            const Spent spent = run(context, config, result);
            Expect expect;
            expect.defaultChain = true;
            check_results(config, result, expect, "chain");
            check_spent("chain", config, spent, kMaxChainAllocatorBytes);
            std::puts("PASS transform feedback passes: chain");
        }

        /* An all-LOAD pass over a multisampled attachment: the encoder that
         * starts at the rasterizing draw must be the attachment's sample count,
         * not a default. */
        if (selected("multisampled")) {
            FeedbackChainConfig config = variant();
            config.samples = VK_SAMPLE_COUNT_4_BIT;
            run_variant(context, "multisampled", config, {});
        }

        /* A pass with no draw at all starts no encoder and leaves the
         * attachment alone. */
        if (selected("empty pass")) {
            FeedbackChainConfig config = variant();
            config.empty_pass = true;
            run_variant(context, "empty pass", config, {});
        }

        /* A clear in the middle pass touches the attachment, so that pass
         * starts its encoder: the cost is that one pass, not the chain. */
        if (selected("clear in pass")) {
            FeedbackChainConfig config = variant();
            config.passes[config.steps / 2] = {{Kind::Clear}, {Kind::Capture}};
            Expect expect;
            expect.blueBackground = true;
            run_variant(context, "clear in pass", config, expect);
        }

        /* A timestamp of a render stage belongs to the pass's render encoder,
         * which it starts. */
        if (selected("timestamp in pass")) {
            FeedbackChainConfig config = variant();
            config.timestamp_queries = 2;
            config.before_pass[0] = {stamp(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0)};
            config.passes[config.steps / 2] = {
                stamp(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, 1), {Kind::Capture}};
            const FeedbackChainResult result =
                run_variant(context, "timestamp in pass", config, {});
            check_timestamps(result, 2, "timestamp in pass");
        }

        /* Attachments and draws the late start must carry over alike. */
        if (selected("depth attachment")) {
            FeedbackChainConfig config = variant();
            config.depth = true;
            run_variant(context, "depth attachment", config, {});
        }
        if (selected("rasterizing capture")) {
            /* The middle pass's draw rasterizes as well as captures, and
             * starts the pass's encoder as the first draw. */
            FeedbackChainConfig config = variant();
            config.passes[config.steps / 2] = {{Kind::RasterCapture}};
            Expect expect;
            expect.rasterizingDraws = 2;
            run_variant(context, "rasterizing capture", config, expect);
        }
        if (selected("rasterize then capture")) {
            /* The last pass rasterizes before it captures: the encoder is
             * open when the capture's compute comes. */
            FeedbackChainConfig config = variant();
            config.passes[config.steps] = {{Kind::Raster}, {Kind::Capture}};
            run_variant(context, "rasterize then capture", config, {});
        }
        if (selected("indexed capture")) {
            /* Draws that unroll from the geometry heap, so their reset follows
             * the pass: not a bound on storage, only on the results. */
            FeedbackChainConfig config = variant();
            config.step_draw = FeedbackChainConfig::StepDraw::Indexed;
            run_variant(context, "indexed capture", config, {}, false);
        }
        if (selected("indirect capture")) {
            FeedbackChainConfig config = variant();
            config.step_draw = FeedbackChainConfig::StepDraw::Indirect;
            run_variant(context, "indirect capture", config, {}, false);
        }

        /* An occlusion query across the encoders of its passes. Each sequence
         * ends a Metal encoder between the draws that rasterize and the rest,
         * and the query must keep the samples every encoder counted. */
        struct OcclusionCase {
            const char *name;
            std::vector<ChainOp> last;
            uint32_t rasterizingDraws;
        };
        const OcclusionCase occlusionCases[] = {
            /* A discard draw whose pipeline masks every attachment restarts
             * the encoder. */
            {"occlusion: raster, discard draw", {{Kind::Raster}, {Kind::DiscardNoCapture}}, 1},
            /* A capture begun and ended with no draw stops the encoder for its
             * counter copy. */
            {"occlusion: raster, empty capture, raster",
             {{Kind::Raster}, {Kind::EmptyCapture}, {Kind::Raster}}, 2},
            {"occlusion: raster, rasterizing capture, raster",
             {{Kind::Raster}, {Kind::RasterCapture}, {Kind::Raster}}, 3},
            {"occlusion: raster, empty capture, discard draw",
             {{Kind::Raster}, {Kind::EmptyCapture}, {Kind::DiscardNoCapture}}, 1},
            {"occlusion: raster, empty capture, clipped draw",
             {{Kind::Raster}, {Kind::EmptyCapture}, {Kind::ClippedRaster}}, 1},
        };
        for (const OcclusionCase &occlusion : occlusionCases) {
            for (bool precise : {true, false}) {
                const std::string name =
                    std::string(occlusion.name) + (precise ? ", precise" : ", boolean");
                if (!selected(name))
                    continue;
                FeedbackChainConfig config = variant();
                config.passes[config.steps] = occlusion.last;
                config.precise_occlusion = precise;
                Expect expect;
                expect.rasterizingDraws = occlusion.rasterizingDraws;
                run_variant(context, name.c_str(), config, expect, false);
            }
        }
        for (bool precise : {true, false}) {
            /* Across passes: the first pass rasterizes, the middle ones only
             * capture, and the last draws points that clip away. */
            const char *name = precise ? "occlusion: across passes, precise"
                                       : "occlusion: across passes, boolean";
            if (!selected(name))
                continue;
            FeedbackChainConfig config = variant();
            config.passes[0] = {{Kind::Capture}, {Kind::Raster}};
            config.passes[config.steps] = {{Kind::Capture}, {Kind::ClippedRaster}};
            config.precise_occlusion = precise;
            run_variant(context, name, config, {}, false);
        }
        if (selected("occlusion: a reset query")) {
            /* A query reset and run again reads only its own samples, which is
             * what lets the encoders add to its slot. */
            FeedbackChainConfig config = variant();
            config.reuse_occlusion = true;
            run_variant(context, "occlusion: a reset query", config, {}, false);
        }

        /* Timestamps that belong to no render stage, in the places a pass puts
         * them. vkCmdWriteTimestamp2 latches the timer when the commands
         * before it have completed its stage, so a query never reads a time
         * after commands recorded later than it: the case with copies between
         * the writes and "after a capture: top, transform feedback" are the
         * ones that check this. The others have no command between their
         * writes, which Vulkan lets read alike or either way round, and keep
         * the driver's recording order as a regression guard. */
        struct TimestampCase {
            const char *name;
            std::vector<ChainOp> beforeMiddle;
            std::vector<ChainOp> middle;
        };
        const auto top = [](uint32_t q) { return stamp(VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, q); };
        const auto bottom = [](uint32_t q) {
            return stamp(VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, q);
        };
        const auto xfb = [](uint32_t q) {
            return stamp(VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT, q);
        };
        const TimestampCase timestampCases[] = {
            /* Three in a row between two passes. */
            {"timestamps between passes", {top(0), bottom(1), xfb(2)}, {{Kind::Capture}}},
            /* With a copy before and between: each reads a time that follows
             * the copies before it and precedes the ones after it. */
            {"timestamps between passes with copies between",
             {top(0), {Kind::Copy}, bottom(1), {Kind::Copy}, top(2)}, {{Kind::Capture}}},
            /* In a pass that has no encoder yet. */
            {"timestamps before a pass's first draw",
             {top(0)}, {bottom(1), xfb(2), {Kind::Capture}}},
            /* After the capture, once the encoder has been stopped. */
            {"timestamps after a capture: top, bottom",
             {top(0)}, {{Kind::Capture}, top(1), bottom(2)}},
            {"timestamps after a capture: top, transform feedback",
             {top(0)}, {{Kind::Capture}, top(1), xfb(2)}},
            /* In a pass whose encoder a clear started: the pass's end. */
            {"timestamps in a started pass: bottom, bottom",
             {top(0)}, {{Kind::Clear}, bottom(1), bottom(2), {Kind::Capture}}},
            {"timestamps in a started pass: bottom, transform feedback",
             {top(0)}, {{Kind::Clear}, bottom(1), xfb(2), {Kind::Capture}}},
        };
        for (const TimestampCase &timestamps : timestampCases) {
            if (!selected(timestamps.name))
                continue;
            FeedbackChainConfig config = variant();
            const uint32_t middle = config.steps / 2;
            config.timestamp_queries = 4;
            config.before_pass[middle] = timestamps.beforeMiddle;
            config.passes[middle] = timestamps.middle;
            config.before_pass[config.steps + 1] = {top(3)};
            Expect expect;
            expect.blueBackground = std::any_of(
                timestamps.middle.begin(), timestamps.middle.end(),
                [](const ChainOp &op) { return op.kind == Kind::Clear; });
            const FeedbackChainResult result =
                run_variant(context, timestamps.name, config, expect, false);
            check_timestamps(result, 4, timestamps.name);
        }

        /* A draw that reserves from the geometry heap before its pass's
         * encoder starts, and a second that reserves beside it: the heap resets
         * after both, and the second finds room. */
        if (selected("heap segment")) {
            FeedbackChainConfig config = variant();
            config.heap_capture_indices = kHeapCaptureIndices;
            config.heap_raster_indices = kHeapRasterIndices;
            config.passes[config.steps / 2] = {
                {Kind::HeapCapture}, {Kind::HeapRaster}, {Kind::Capture}};
            Expect expect;
            expect.extraSamples = kHeapRasterIndices;
            const FeedbackChainResult result =
                run_variant(context, "heap segment", config, expect, false);
            check_heap_capture(config, result, "heap segment");
        }
        /* The same draws in a pass that begins with a clear load op: its
         * encoder starts when the pass begins and stays, so the draws launch
         * their compute with it open and the heap resets after the pass, as
         * they do outside a late start. The boolean query leaves the heap
         * draw's capture as the check that the heap served it. */
        for (bool precise : {true, false}) {
            const char *name = precise ? "heap segment in a started pass, precise"
                                       : "heap segment in a started pass, boolean";
            if (!selected(name))
                continue;
            FeedbackChainConfig config = variant();
            config.heap_capture_indices = kHeapCaptureIndices;
            config.heap_raster_indices = kHeapRasterIndices;
            config.clearing_pass = config.steps / 2;
            config.precise_occlusion = precise;
            config.passes[config.steps / 2] = {
                {Kind::HeapCapture}, {Kind::HeapRaster}, {Kind::Capture}};
            Expect expect;
            expect.blueBackground = true;
            expect.extraSamples = kHeapRasterIndices;
            const FeedbackChainResult result = run_variant(context, name, config, expect, false);
            check_heap_capture(config, result, name);
        }

        /* Two draws that unroll from the geometry heap, in a pass that follows
         * other work in its command buffer. The first draw's compute has to
         * precede the pass's encoder, which splits the pass, and the heap
         * segment goes on across the split: the second counts its demand
         * beside the first's and the heap grows for both, instead of the
         * second finding a heap that is not empty and being refused. */
        struct SplitCase {
            const char *name;
            bool clearing;
            bool raster_pass_before;
        };
        const SplitCase splitCases[] = {
            {"heap segment across a split: load-only pass", false, false},
            {"heap segment across a split: pass that begins with a clear", true, false},
            {"heap segment across a split: after a raster pass", false, true},
        };
        for (const SplitCase &split : splitCases) {
            if (!selected(split.name))
                continue;
            FeedbackChainConfig config = variant();
            const uint32_t middle = config.steps / 2;
            config.heap_raster_indices = kHeapRasterIndices;
            if (split.clearing)
                config.clearing_pass = middle;
            if (split.raster_pass_before)
                config.passes[middle - 1] = {{Kind::Raster}};
            config.passes[middle] = {{Kind::HeapRaster}, {Kind::HeapRaster}, {Kind::Capture}};
            Expect expect;
            expect.blueBackground = split.clearing;
            expect.rasterizingDraws = split.raster_pass_before ? 2 : 1;
            expect.extraSamples = 2u * kHeapRasterIndices;
            run_variant(context, split.name, config, expect, false);
        }
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL transform feedback passes: %s\n", error.what());
        return 1;
    }
}

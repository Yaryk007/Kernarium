// SPDX-License-Identifier: GPL-2.0-or-later

// A new texture holds zeros, whatever its kind. The renderer hands the guest a
// texture whose storage is memory the host driver gave out before, with the
// pixels of the resource that released it, which may have belonged to another
// guest process. gpu-texture-zero-fill covers the single-sample uncompressed
// textures; this test covers two more kinds:
//
//  - a multisampled texture, which the renderer's clear reaches and this test
//    guards: a guest resolves or blits one after drawing only part of it, and
//    a change to the clear must not lose it; and
//  - a compressed texture, which no clear reaches and the renderer fills with
//    zero blocks instead: a guest uploads one block by block and may sample it
//    before it has uploaded every block, down to a level that is part of one
//    block, in a 2D, array, cube or cube array texture.
//
// The contract under test: after textures of the kind that held a pattern are
// released, a new texture of the same shape holds what a texture written with
// zeros does, every sample of it. A compressed format decodes a block of zeros
// as it defines, which is not always a zero texel, so the comparison is with
// the decoding of a texture the guest wrote zero blocks to.

#include "host_gl_stream_support.hpp"
#include "host_gl_test_support.hpp"
#include "require.hpp"

#include <virgl_hw.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <vector>

namespace {

using namespace uml::gpu::testing::stream;
using uml::gpu::testing::skipWithoutRenderer;
using uml::testing::require;

int failureCount = 0;

void check(bool condition, const char* description)
{
    if (condition)
        return;
    std::fprintf(stderr, "Texture zero fill: %s\n", description);
    ++failureCount;
}

constexpr std::uint32_t commandClear = 7;
constexpr std::uint32_t commandBlit = 16;
constexpr std::uint8_t patternByte = 0xa5;
// Enough textures per round that one of them lands on memory an earlier
// round's textures released, and enough rounds that it does so whichever way
// the driver recycles.
constexpr std::uint32_t texturesPerRound = 6;
constexpr std::uint32_t rounds = 3;

// A multisampled render target of its own, attached to the guest context, that
// nothing has drawn to.
class MultisampledTarget final {
public:
    MultisampledTarget(VirglRenderer& renderer, std::uint32_t resourceId,
        std::uint32_t side, std::uint32_t samples)
        : renderer_ {renderer}
        , resourceId_ {resourceId}
    {
        require(renderer_.createResource3d(VirglRenderer::Resource3d {
                    .resourceId = resourceId_,
                    .target = pipeTexture2d,
                    .format = virglFormatR8g8b8a8Unorm,
                    .bind = virglBindRenderTarget | virglBindSamplerView,
                    .width = side,
                    .height = side,
                    .depth = 1,
                    .arraySize = 1,
                    .sampleCount = samples,
                }) == 0,
            "the multisampled texture could not be created");
        require(renderer_.attachResource(contextId, resourceId_) == 0,
            "the multisampled texture could not be attached");
    }

    ~MultisampledTarget()
    {
        (void)renderer_.detachResource(contextId, resourceId_);
        renderer_.unrefResource(resourceId_);
    }

    MultisampledTarget(const MultisampledTarget&) = delete;
    MultisampledTarget& operator=(const MultisampledTarget&) = delete;

    [[nodiscard]] std::uint32_t id() const noexcept { return resourceId_; }

private:
    VirglRenderer& renderer_;
    std::uint32_t resourceId_;
};

// Resolves `source` into `destination`, the way a guest's blit or glReadPixels
// of a multisampled framebuffer does, and waits for it.
void resolve(VirglRenderer& renderer, std::uint32_t source,
    std::uint32_t destination, std::uint32_t side, std::uint64_t& fence)
{
    Commands commands;
    commands.add(commandBlit, 0,
        {0xfU, 0, 0, destination, 0, virglFormatR8g8b8a8Unorm, 0, 0, 0, side,
            side, 1, source, 0, virglFormatR8g8b8a8Unorm, 0, 0, 0, side, side,
            1});
    require(commands.submit(renderer) == 0, "the resolve was refused");
    require(renderer.createFenceAndWait(contextId, 0, ++fence, nullptr) == 0,
        "the resolve did not finish");
}

// Clears multisampled render targets to a colour no zeroed texture holds and
// releases them, so the host driver's storage for the next textures of that
// shape holds the colour.
void dirtyMultisampled(VirglRenderer& renderer, std::uint32_t& resourceId,
    std::uint32_t& handle, std::uint32_t side, std::uint32_t samples,
    std::uint64_t& fence)
{
    std::vector<std::unique_ptr<MultisampledTarget>> written;
    for (std::uint32_t index = 0; index < texturesPerRound; ++index) {
        written.push_back(std::make_unique<MultisampledTarget>(
            renderer, resourceId++, side, samples));
        const std::uint32_t surface = handle++;
        Commands commands;
        commands.add(commandCreateObject, objectSurface,
            {surface, written.back()->id(), virglFormatR8g8b8a8Unorm, 0, 0});
        commands.add(commandSetFramebufferState, 0, {1, 0, surface});
        // PIPE_CLEAR_COLOR0, then red, green, blue, alpha and the depth and
        // stencil the clear does not use.
        commands.add(commandClear, 0,
            {4U, bits(0.5F), bits(0.25F), bits(0.75F), bits(1.0F), 0, 0, 0});
        require(commands.submit(renderer) == 0, "the clear was refused");
    }
    require(renderer.createFenceAndWait(contextId, 0, ++fence, nullptr) == 0,
        "the clears did not finish");
}

// Resolves `source` into a fresh single-sample image pre-filled with a pattern
// and returns true when any byte of the result is not zero.
[[nodiscard]] bool resolvesNonZero(VirglRenderer& renderer,
    std::uint32_t source, std::uint32_t& resourceId, std::uint32_t side,
    std::uint64_t& fence)
{
    Image resolved(renderer, resourceId++, pipeTexture2d,
        virglFormatR8g8b8a8Unorm, virglBindRenderTarget | virglBindSamplerView,
        side, side, 0, static_cast<std::size_t>(side) * side * 4U);
    std::fill(resolved.bytes().begin(), resolved.bytes().end(), patternByte);
    resolved.writeBox(side * 4U, 0, 0, side, side);
    resolve(renderer, source, resourceId - 1U, side, fence);
    require(resolved.readBox(side * 4U, 0, 0, side, side) == 0,
        "the resolved texture could not be read");
    return std::any_of(resolved.bytes().begin(), resolved.bytes().end(),
        [](std::uint8_t byte) { return byte != 0; });
}

void checkMultisampled(VirglRenderer& renderer, std::uint32_t& resourceId,
    std::uint32_t& handle, std::uint64_t& fence)
{
    constexpr std::uint32_t side = 128;
    constexpr std::uint32_t samples = 4;
    std::uint32_t nonZero = 0;
    std::uint32_t total = 0;
    for (std::uint32_t round = 0; round < rounds; ++round) {
        {
            // The control: the clears below do dirty a texture, so a fresh
            // one that reads zero is not a texture nothing was drawn to.
            MultisampledTarget probe(renderer, resourceId++, side, samples);
            const std::uint32_t surface = handle++;
            Commands commands;
            commands.add(commandCreateObject, objectSurface,
                {surface, probe.id(), virglFormatR8g8b8a8Unorm, 0, 0});
            commands.add(commandSetFramebufferState, 0, {1, 0, surface});
            commands.add(commandClear, 0,
                {4U, bits(0.5F), bits(0.25F), bits(0.75F), bits(1.0F), 0, 0,
                    0});
            require(commands.submit(renderer) == 0, "the clear was refused");
            require(resolvesNonZero(renderer, probe.id(), resourceId, side,
                        fence),
                "a cleared multisampled texture resolved to zeros");
        }
        dirtyMultisampled(renderer, resourceId, handle, side, samples, fence);
        std::vector<std::unique_ptr<MultisampledTarget>> fresh;
        for (std::uint32_t index = 0; index < texturesPerRound; ++index) {
            fresh.push_back(std::make_unique<MultisampledTarget>(
                renderer, resourceId++, side, samples));
            ++total;
            nonZero += resolvesNonZero(renderer, fresh.back()->id(),
                           resourceId, side, fence)
                ? 1U
                : 0U;
        }
    }
    std::fprintf(stderr, "multisampled: %u of %u new textures read non-zero\n",
        nonZero, total);
    check(nonZero == 0,
        "a new multisampled texture showed what a released one held");
}

// One block of each layout, repeated over a texture: the blocks the compressed
// formats test decodes, so each is a block the hardware accepts and none
// decodes to what a block of zeros does.
constexpr std::array<std::uint8_t, 8> etcColour {
    0x88, 0x44, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 16> etcAlphaColour {
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x88, 0x44, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr std::array<std::uint8_t, 16> astcConstant {
    0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x88, 0x88, 0x44, 0x44, 0x22, 0x22, 0xff, 0xff};

struct Kind {
    std::uint32_t format;
    const char* name;
    std::uint32_t blockWidth;
    std::uint32_t blockHeight;
    std::span<const std::uint8_t> pattern;
};

constexpr std::array kinds {
    Kind {VIRGL_FORMAT_ETC2_RGB8, "ETC2_RGB8", 4, 4, etcColour},
    Kind {VIRGL_FORMAT_ETC2_RGBA8, "ETC2_RGBA8", 4, 4, etcAlphaColour},
    Kind {VIRGL_FORMAT_ASTC_4x4, "ASTC_4x4", 4, 4, astcConstant},
    Kind {VIRGL_FORMAT_ASTC_8x8, "ASTC_8x8", 8, 8, astcConstant},
};

// The shapes of compressed texture the check runs, each with every level down
// to the one the size ends at. A side of 100 texels is a whole number of 4x4
// blocks and leaves a part block against 8x8 blocks, its levels (50, 25, 12, 6,
// 3, 1) leave one against both, and the last ones are smaller than a block. A
// side of 1100 texels is 275 blocks of 4x4, more than one tile of the host's
// zero fill across. The others have layers, each of which the host fills: a 2D
// array of three, a cube, and a cube array of two cubes.
struct Shape {
    std::uint32_t side;
    std::uint32_t lastLevel;
    std::uint32_t layers;
    std::uint32_t target;
};

constexpr std::uint32_t pipeTextureCube = 4;
constexpr std::uint32_t pipeTexture2dArray = 7;
constexpr std::uint32_t pipeTextureCubeArray = 8;

constexpr std::array shapes {Shape {100, 6, 1, pipeTexture2d},
    Shape {1100, 2, 1, pipeTexture2d}, Shape {70, 3, 3, pipeTexture2dArray},
    Shape {64, 2, 6, pipeTextureCube},
    Shape {36, 1, 12, pipeTextureCubeArray}};

// A compressed texture of the kind, with guest storage for every level.
class CompressedTexture final {
public:
    CompressedTexture(VirglRenderer& renderer, std::uint32_t resourceId,
        const Kind& kind, const Shape& shape)
        : renderer_ {renderer}
        , resourceId_ {resourceId}
        , kind_ {kind}
        , shape_ {shape}
    {
        std::size_t total = 0;
        for (std::uint32_t level = 0; level <= shape_.lastLevel; ++level) {
            offsets_.push_back(total);
            total += layerBytes(level) * shape_.layers;
        }
        storage_.resize(total);
        require(renderer_.createResource3d(VirglRenderer::Resource3d {
                    .resourceId = resourceId_,
                    .target = shape_.target,
                    .format = kind_.format,
                    .bind = virglBindSamplerView,
                    .width = shape_.side,
                    .height = shape_.side,
                    .depth = 1,
                    .arraySize = shape_.layers,
                    .lastLevel = shape_.lastLevel,
                }) == 0,
            "the compressed texture could not be created");
        // The renderer keeps this array, not a copy of it.
        vectors_[0] = {.iov_base = storage_.data(), .iov_len = storage_.size()};
        require(renderer_.attachBacking(resourceId_, vectors_) == 0,
            "the compressed texture's guest storage could not be attached");
        require(renderer_.attachResource(contextId, resourceId_) == 0,
            "the compressed texture could not be attached");
    }

    ~CompressedTexture()
    {
        (void)renderer_.detachResource(contextId, resourceId_);
        renderer_.detachBacking(resourceId_);
        renderer_.unrefResource(resourceId_);
    }

    CompressedTexture(const CompressedTexture&) = delete;
    CompressedTexture& operator=(const CompressedTexture&) = delete;

    [[nodiscard]] std::uint32_t id() const noexcept { return resourceId_; }

    // Writes every block of every level: the kind's pattern, or zeros.
    void fill(bool pattern)
    {
        for (std::size_t offset = 0; offset < storage_.size();
             offset += kind_.pattern.size()) {
            if (pattern)
                std::memcpy(storage_.data() + offset, kind_.pattern.data(),
                    kind_.pattern.size());
            else
                std::memset(storage_.data() + offset, 0, kind_.pattern.size());
        }
        for (std::uint32_t level = 0; level <= shape_.lastLevel; ++level) {
            for (std::uint32_t layer = 0; layer < shape_.layers; ++layer) {
                const VirglRenderer::Box box {
                    .z = layer,
                    .width = levelSize(level),
                    .height = levelSize(level),
                    .depth = 1,
                };
                require(renderer_.transferResource(true, resourceId_,
                            contextId, level,
                            static_cast<std::uint32_t>(blocksWide(level) *
                                kind_.pattern.size()),
                            static_cast<std::uint32_t>(layerBytes(level)), box,
                            offsets_[level] + layer * layerBytes(level)) == 0,
                    "the blocks could not be written to the texture");
            }
        }
    }

    [[nodiscard]] std::uint32_t levelSize(std::uint32_t level) const noexcept
    {
        return std::max(shape_.side >> level, 1U);
    }

private:
    [[nodiscard]] std::size_t layerBytes(std::uint32_t level) const noexcept
    {
        return static_cast<std::size_t>(blocksWide(level)) * blocksHigh(level) *
            kind_.pattern.size();
    }

    [[nodiscard]] std::uint32_t blocksWide(std::uint32_t level) const noexcept
    {
        return (levelSize(level) + kind_.blockWidth - 1U) / kind_.blockWidth;
    }
    [[nodiscard]] std::uint32_t blocksHigh(std::uint32_t level) const noexcept
    {
        return (levelSize(level) + kind_.blockHeight - 1U) / kind_.blockHeight;
    }

    VirglRenderer& renderer_;
    std::uint32_t resourceId_;
    const Kind& kind_;
    const Shape& shape_;
    std::vector<std::size_t> offsets_;
    std::vector<std::uint8_t> storage_;
    std::array<iovec, 1> vectors_ {};
};

// The texels a texture shows at one level, decoded by the hardware.
[[nodiscard]] std::vector<std::uint8_t> samplesOf(VirglRenderer& renderer,
    Pipeline& pipeline, const CompressedTexture& texture, const Kind& kind,
    std::uint32_t level, std::uint32_t layer)
{
    const std::uint32_t view = pipeline.newHandle();
    Commands commands;
    commands.add(commandCreateObject, objectSamplerView,
        {view, texture.id(), kind.format | (pipeTexture2d << 24U),
            layer | (layer << 16U), level | (level << 8U),
            0U | (1U << 3U) | (2U << 6U) | (3U << 9U)});
    require(commands.submit(renderer) == 0,
        "the sampler view could not be created");
    return pipeline.sample(
        view, texture.levelSize(level), texture.levelSize(level));
}

void checkCompressed(VirglRenderer& renderer, Pipeline& pipeline,
    std::uint32_t& resourceId, const Shape& shape)
{
    const std::uint32_t views = (shape.lastLevel + 1U) * shape.layers;
    for (const Kind& kind : kinds) {
        // What a block of zeros decodes to at each level of each layer, and
        // the control that the pattern decodes to something else.
        std::vector<std::vector<std::uint8_t>> zeros(views);
        {
            CompressedTexture zero(renderer, resourceId++, kind, shape);
            zero.fill(false);
            CompressedTexture pattern(renderer, resourceId++, kind, shape);
            pattern.fill(true);
            for (std::uint32_t view = 0; view < views; ++view) {
                const std::uint32_t level = view % (shape.lastLevel + 1U);
                const std::uint32_t layer = view / (shape.lastLevel + 1U);
                zeros[view] =
                    samplesOf(renderer, pipeline, zero, kind, level, layer);
                check(zeros[view] !=
                        samplesOf(renderer, pipeline, pattern, kind, level,
                            layer),
                    "the pattern and a block of zeros decode alike");
            }
        }
        std::uint32_t differing = 0;
        std::uint32_t total = 0;
        for (std::uint32_t round = 0; round < rounds; ++round) {
            {
                std::vector<std::unique_ptr<CompressedTexture>> written;
                for (std::uint32_t index = 0; index < texturesPerRound;
                     ++index) {
                    written.push_back(std::make_unique<CompressedTexture>(
                        renderer, resourceId++, kind, shape));
                    written.back()->fill(true);
                }
            }
            std::vector<std::unique_ptr<CompressedTexture>> fresh;
            for (std::uint32_t index = 0; index < texturesPerRound; ++index) {
                fresh.push_back(std::make_unique<CompressedTexture>(
                    renderer, resourceId++, kind, shape));
                ++total;
                bool differs = false;
                for (std::uint32_t view = 0; view < views; ++view)
                    differs = differs ||
                        samplesOf(renderer, pipeline, *fresh.back(), kind,
                            view % (shape.lastLevel + 1U),
                            view / (shape.lastLevel + 1U)) != zeros[view];
                differing += differs ? 1U : 0U;
            }
        }
        std::fprintf(stderr,
            "%s %ux%ux%u: %u of %u new textures decode other than zero "
            "blocks\n",
            kind.name, shape.side, shape.side, shape.layers, differing, total);
        check(differing == 0,
            "a new compressed texture showed what a released one held");
    }
}

} // namespace

int main()
{
    std::unique_ptr<VirglRenderer> renderer;
    try {
        renderer = std::make_unique<VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }
    require(renderer->createContext(contextId, virgl2CapsetId, "zero fill") == 0,
        "the guest context could not be created");
    std::uint32_t resourceId = 10;
    std::uint64_t fence = 0;
    std::uint32_t handle = firstTransientHandle;
    checkMultisampled(*renderer, resourceId, handle, fence);
    Pipeline pipeline(*renderer);
    for (const Shape& shape : shapes)
        checkCompressed(*renderer, pipeline, resourceId, shape);
    if (failureCount != 0)
        return 1;
    std::fputs("Texture zero fill (special): ok\n", stderr);
    return 0;
}

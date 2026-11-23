// SPDX-License-Identifier: GPL-2.0-or-later

// A new compressed texture is filled with zero blocks, in steps: a tile of
// zero blocks is made and uploaded, its blocks are copied into every level
// and layer, and the part blocks at the edge of a level are uploaded. The
// host driver can refuse room for any of them when its memory budget is full,
// by raising GL_OUT_OF_MEMORY.
//
// The contract under test: a refused step fails the creation with the error
// a refused storage meets, so that the renderer gives back what it can and
// makes the texture again; a request that still fails is refused, and the
// guest never gets a texture the fill left partly written. A texture that was
// created holds zero blocks in every byte, whether it has a few layers or
// hundreds.
//
// The refusals are injected into the GL calls the renderer makes, because a
// real budget refuses the storage of the texture before it refuses any of the
// fill, and the storage is poisoned as the host driver's recycled memory is:
// a fill that stops early leaves the poison to be read. Mesa raises
// GL_OUT_OF_MEMORY for an upload it cannot stage, so there is no step that
// drops its work without an error to inject.

#include "host_gl_stream_support.hpp"
#include "host_gl_test_support.hpp"
#include "require.hpp"

#include <virgl_hw.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <vector>

namespace {

using namespace uml::gpu::testing::stream;
using uml::gpu::testing::skipWithoutRenderer;
using uml::testing::require;

constexpr std::uint32_t pipeTexture2dArray = 7;
constexpr std::uint8_t poisonByte = 0x5a;
// 37 texels are nine blocks of 4x4 and a part block; the levels (18, 9, 4)
// leave a part block at the edge twice more, and the last is one block.
constexpr std::uint32_t side = 37;
constexpr std::uint32_t lastLevel = 3;
constexpr std::uint32_t blockBytes = 16;

enum class Step {
    none,
    tileStorage,
    tileUpload,
    partUpload,
    firstLayerCopy,
    laterLayerCopy
};

// What the hooks do for the creation under test.
struct Injection {
    Step step = Step::none;
    // Refuse at every attempt, not only the first.
    bool always = false;
    // The storages of the texture itself seen, one for each attempt.
    unsigned attempts = 0;
    unsigned injected = 0;
    // Copies seen in the current attempt.
    unsigned copies = 0;
    GLenum pending = 0;

    [[nodiscard]] bool refuseNow(Step now)
    {
        if (step != now || (injected != 0 && !always) ||
            (!always && attempts != 1))
            return false;
        ++injected;
        pending = GL_OUT_OF_MEMORY;
        return true;
    }
};

Injection injection;

PFNGLTEXSTORAGE2DPROC realStorage2d;
PFNGLTEXSTORAGE3DPROC realStorage3d;
PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC realUpload2d;
PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC realUpload3d;
PFNGLCOPYIMAGESUBDATAPROC realCopy;
PFNGLGETERRORPROC realError;

[[nodiscard]] std::size_t levelBytes(std::uint32_t level, std::uint32_t layers)
{
    const std::uint32_t size = std::max(side >> level, 1U);
    const std::size_t blocks = (size + 3U) / 4U;
    return blocks * blocks * blockBytes * layers;
}

// The storage of the texture comes from memory a texture released: every byte
// of every level holds a pattern until something writes it.
void poison(GLenum target, GLenum format, std::uint32_t layers)
{
    for (std::uint32_t level = 0; level <= lastLevel; ++level) {
        const std::uint32_t size = std::max(side >> level, 1U);
        const std::vector<std::uint8_t> bytes(
            levelBytes(level, layers), poisonByte);
        const auto count = static_cast<GLsizei>(bytes.size());
        if (target == GL_TEXTURE_2D)
            realUpload2d(target, static_cast<GLint>(level), 0, 0,
                static_cast<GLsizei>(size), static_cast<GLsizei>(size), format,
                count, bytes.data());
        else
            realUpload3d(target, static_cast<GLint>(level), 0, 0, 0,
                static_cast<GLsizei>(size), static_cast<GLsizei>(size),
                static_cast<GLsizei>(layers), format, count, bytes.data());
    }
}

void startAttempt()
{
    ++injection.attempts;
    injection.copies = 0;
}

GLenum APIENTRY hookError()
{
    if (injection.pending != 0) {
        const GLenum error = injection.pending;
        injection.pending = 0;
        return error;
    }
    return realError();
}

void APIENTRY hookStorage2d(
    GLenum target, GLsizei levels, GLenum format, GLsizei width, GLsizei height)
{
    // A texture with levels is the one under test, a texture of one level the
    // tile.
    if (levels == 1) {
        if (injection.refuseNow(Step::tileStorage))
            return;
        realStorage2d(target, levels, format, width, height);
        return;
    }
    startAttempt();
    realStorage2d(target, levels, format, width, height);
    poison(target, format, 1);
}

void APIENTRY hookStorage3d(GLenum target, GLsizei levels, GLenum format,
    GLsizei width, GLsizei height, GLsizei depth)
{
    startAttempt();
    realStorage3d(target, levels, format, width, height, depth);
    poison(target, format, static_cast<std::uint32_t>(depth));
}

void APIENTRY hookUpload2d(GLenum target, GLint level, GLint x, GLint y,
    GLsizei width, GLsizei height, GLenum format, GLsizei size,
    const void* data)
{
    // Before the first copy the renderer is writing the tile.
    if (injection.refuseNow(injection.copies == 0 ? Step::tileUpload
                                                   : Step::partUpload))
        return;
    realUpload2d(target, level, x, y, width, height, format, size, data);
}

void APIENTRY hookUpload3d(GLenum target, GLint level, GLint x, GLint y,
    GLint z, GLsizei width, GLsizei height, GLsizei depth, GLenum format,
    GLsizei size, const void* data)
{
    if (injection.refuseNow(Step::partUpload))
        return;
    realUpload3d(
        target, level, x, y, z, width, height, depth, format, size, data);
}

void APIENTRY hookCopy(GLuint source, GLenum sourceTarget, GLint sourceLevel,
    GLint sourceX, GLint sourceY, GLint sourceZ, GLuint destination,
    GLenum destinationTarget, GLint destinationLevel, GLint destinationX,
    GLint destinationY, GLint destinationZ, GLsizei width, GLsizei height,
    GLsizei depth)
{
    ++injection.copies;
    // The copies from the tile make the first layer, and then each later one.
    if (injection.refuseNow(
            destinationZ == 0 ? Step::firstLayerCopy : Step::laterLayerCopy))
        return;
    realCopy(source, sourceTarget, sourceLevel, sourceX, sourceY, sourceZ,
        destination, destinationTarget, destinationLevel, destinationX,
        destinationY, destinationZ, width, height, depth);
}

struct Case {
    const char* name;
    Step step;
    bool always;
    std::uint32_t layers;
};

constexpr std::array cases {
    Case {"control", Step::none, false, 1},
    Case {"tile storage refused", Step::tileStorage, false, 1},
    Case {"tile upload refused", Step::tileUpload, false, 1},
    Case {"part block upload refused", Step::partUpload, false, 1},
    Case {"copy from the tile refused", Step::firstLayerCopy, false, 1},
    Case {"layered control", Step::none, false, 3},
    Case {"layered tile upload refused", Step::tileUpload, false, 3},
    Case {"layered part block upload refused", Step::partUpload, false, 3},
    Case {"copy into a later layer refused", Step::laterLayerCopy, false, 3},
    Case {"tile storage always refused", Step::tileStorage, true, 1},
    Case {"tile upload always refused", Step::tileUpload, true, 1},
    Case {"part block upload always refused", Step::partUpload, true, 1},
    Case {"copy from the tile always refused", Step::firstLayerCopy, true, 1},
    Case {"copy into a later layer always refused", Step::laterLayerCopy, true,
        3},
    // A texture of many layers has more operations to refuse and to repeat.
    Case {"many layers control", Step::none, false, 200},
    Case {"many layers, part block upload refused", Step::partUpload, false,
        200},
    Case {"many layers, copy into a later layer refused", Step::laterLayerCopy,
        false, 200},
    Case {"many layers control after a retry", Step::none, false, 200},
    Case {"many layers, part block upload always refused", Step::partUpload,
        true, 200},
};

int failures = 0;

void check(bool condition, const char* name, const char* what)
{
    if (condition)
        return;
    std::fprintf(stderr, "Compressed zero refusal: %s: %s\n", name, what);
    ++failures;
}

// The bytes of the texture that are not zero.
[[nodiscard]] std::size_t nonZeroBytes(std::uint32_t resourceId,
    std::uint32_t layers)
{
    virgl_renderer_resource_info info {};
    require(virgl_renderer_resource_get_info(
                static_cast<int>(resourceId), &info) == 0,
        "the texture has no host object");
    const GLenum target = layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    glBindTexture(target, info.tex_id);
    std::size_t nonZero = 0;
    for (std::uint32_t level = 0; level <= lastLevel; ++level) {
        std::vector<std::uint8_t> bytes(levelBytes(level, layers), poisonByte);
        glGetCompressedTexImage(target, static_cast<GLint>(level), bytes.data());
        nonZero += static_cast<std::size_t>(std::count_if(bytes.begin(),
            bytes.end(), [](std::uint8_t byte) { return byte != 0; }));
    }
    glBindTexture(target, 0);
    while (glGetError() != GL_NO_ERROR) {
    }
    return nonZero;
}

void run(VirglRenderer& renderer, const Case& test, std::uint32_t resourceId)
{
    // Something destroyed lets the renderer reclaim once more after a
    // creation it failed to retry.
    injection = Injection {};
    require(renderer.createResource3d(VirglRenderer::Resource3d {
                .resourceId = resourceId + 1000,
                .target = pipeTexture2d,
                .format = virglFormatR8g8b8a8Unorm,
                .bind = virglBindSamplerView,
                .width = 4,
                .height = 4,
                .depth = 1,
                .arraySize = 1,
            }) == 0,
        "the scratch texture could not be created");
    renderer.unrefResource(resourceId + 1000);
    while (glGetError() != GL_NO_ERROR) {
    }

    injection.step = test.step;
    injection.always = test.always;
    const int result = renderer.createResource3d(VirglRenderer::Resource3d {
        .resourceId = resourceId,
        .target = test.layers > 1 ? pipeTexture2dArray
                                  : pipeTexture2d,
        .format = VIRGL_FORMAT_ASTC_4x4,
        .bind = virglBindSamplerView,
        .width = side,
        .height = side,
        .depth = 1,
        .arraySize = test.layers,
        .lastLevel = lastLevel,
    });
    virgl_renderer_resource_info info {};
    const bool exists =
        virgl_renderer_resource_get_info(static_cast<int>(resourceId), &info) ==
        0;
    std::size_t nonZero = 0;
    if (result == 0 && exists)
        nonZero = nonZeroBytes(resourceId, test.layers);
    std::fprintf(stderr,
        "%s: injected %u, storages %u, created %s, non-zero bytes %zu\n",
        test.name, injection.injected, injection.attempts,
        result == 0 ? "yes" : "no", nonZero);

    check(result != 0 || exists, test.name,
        "a creation that succeeded left no texture");
    check(nonZero == 0, test.name, "a texture was left partly written");
    if (test.step == Step::none) {
        check(result == 0 && injection.attempts == 1, test.name,
            "the control was not created at once");
        return;
    }
    check(injection.injected > 0, test.name, "no refusal was injected");
    if (test.always) {
        check(result == ENOMEM || result == -ENOMEM, test.name,
            "a fill refused every time was not answered with ENOMEM");
        check(!exists, test.name,
            "a texture whose fill was refused every time was created");
    } else {
        check(result == 0 && injection.attempts == 2, test.name,
            "the refused fill was not made again");
    }
    if (result == 0)
        renderer.unrefResource(resourceId);
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
    require(renderer->createContext(
                contextId, virgl2CapsetId, "compressed zero refusal") == 0,
        "the guest context could not be created");

    // The GL entry points resolve on their first call: make it, so that the
    // pointers the renderer calls through are the ones replaced below.
    glTexStorage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1);
    glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, GL_RGBA8, 0, nullptr);
    glCompressedTexSubImage3D(
        GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 0, 0, 0, GL_RGBA8, 0, nullptr);
    glCopyImageSubData(0, GL_TEXTURE_2D, 0, 0, 0, 0, 0, GL_TEXTURE_2D, 0, 0, 0, 0,
        1, 1, 1);
    while (glGetError() != GL_NO_ERROR) {
    }
    realStorage2d = epoxy_glTexStorage2D;
    realStorage3d = epoxy_glTexStorage3D;
    realUpload2d = epoxy_glCompressedTexSubImage2D;
    realUpload3d = epoxy_glCompressedTexSubImage3D;
    realCopy = epoxy_glCopyImageSubData;
    realError = epoxy_glGetError;
    epoxy_glTexStorage2D = hookStorage2d;
    epoxy_glTexStorage3D = hookStorage3d;
    epoxy_glCompressedTexSubImage2D = hookUpload2d;
    epoxy_glCompressedTexSubImage3D = hookUpload3d;
    epoxy_glCopyImageSubData = hookCopy;
    epoxy_glGetError = hookError;

    std::uint32_t resourceId = 10;
    for (const Case& test : cases)
        run(*renderer, test, resourceId++);

    epoxy_glTexStorage2D = realStorage2d;
    epoxy_glTexStorage3D = realStorage3d;
    epoxy_glCompressedTexSubImage2D = realUpload2d;
    epoxy_glCompressedTexSubImage3D = realUpload3d;
    epoxy_glCopyImageSubData = realCopy;
    epoxy_glGetError = realError;

    if (failures != 0)
        return 1;
    std::fputs("Compressed zero refusal: ok\n", stderr);
    return 0;
}

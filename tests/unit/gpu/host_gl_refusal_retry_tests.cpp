// SPDX-License-Identifier: GPL-2.0-or-later

// With a bounded device memory budget the host driver refuses an allocation
// the budget cannot hold, and a budget that is full is often one that still
// holds memory for work that has finished: the buffers of draws the GPU has
// run, the storage of a texture the application deleted. The host used to
// drop what the refused allocation was for without a word, so the
// application lost a draw, or kept a texture with its older contents. The
// contract under test is that it gives back what it can and tries once more
// before it drops anything, once and not again, that it loses nothing more
// when that is not enough, and that a refusal leaves nothing behind.
//
// The budget is a pool the test states for the driver (host_gl_injected_pool
// .hpp), which refuses a request of a mebibyte or more that would take the
// live device memory past the limit the test sets: no number of the host's
// enters the test.
//
//  - draw: the constants of 32 KiB a draw uploads fill, within a few
//    hundred draws, the 2 MiB uploader buffers that the draws in flight
//    still hold; no draw is refused over a thousand of them under a limit of
//    eight buffers, for the work that has finished gives its buffers back,
//    and every draw leaves a pixel of its own that neither another draw nor
//    the warm-up supplies; the pool must have refused something, or nothing
//    was proven;
//  - sustained: the same over 8192 draws, after which the memory the context
//    holds is back to what it held before, so that no refusal leaves a
//    buffer behind that the next reclaim cannot find;
//  - upload: a texture the application deleted after the GPU used it still
//    holds its memory, and an upload to a texture the GPU still uses is
//    staged through a buffer the limit has no room for; the upload lands,
//    because the memory of the deleted texture is given back first;
//  - buffer: the same for the update of a busy buffer, which the threaded
//    context sends through a map;
//  - persist: uploads that cannot fit however much is given back are
//    dropped, the host and the context survive them, the request is made
//    twice and no more (once, and once after the reclaim), and the same
//    upload lands once the limit is lifted; and
//  - cycle: a texture that an upload was refused for is deleted and its
//    memory comes back, three times over.
//
// One process makes one of the shapes: what an earlier shape left in the
// driver's caches would give the last one the room it must not have.

#include "host_gl_injected_pool.hpp"
#include "host_gl_test_support.hpp"
#include "require.hpp"
#include "virgl_renderer.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <array>
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

using uml::gpu::testing::InjectedPool;
using uml::gpu::testing::skipWithoutRenderer;
using uml::testing::require;
using namespace std::chrono_literals;

constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;

#if defined(UML_REFUSAL_RETRY_UPLOAD)
constexpr const char* shapeName = "upload";
#elif defined(UML_REFUSAL_RETRY_PERSIST)
constexpr const char* shapeName = "persist";
#elif defined(UML_REFUSAL_RETRY_CYCLE)
constexpr const char* shapeName = "cycle";
#elif defined(UML_REFUSAL_RETRY_BUFFER)
constexpr const char* shapeName = "buffer";
#elif defined(UML_REFUSAL_RETRY_SUSTAINED)
constexpr const char* shapeName = "sustained";
#else
constexpr const char* shapeName = "draw";
#endif

#if !defined(UML_REFUSAL_RETRY_UPLOAD) && !defined(UML_REFUSAL_RETRY_PERSIST) \
    && !defined(UML_REFUSAL_RETRY_CYCLE) && !defined(UML_REFUSAL_RETRY_BUFFER)
#define UML_REFUSAL_RETRY_DRAW 1
#endif

#if defined(UML_REFUSAL_RETRY_UPLOAD) || defined(UML_REFUSAL_RETRY_PERSIST) \
    || defined(UML_REFUSAL_RETRY_CYCLE)
#define UML_REFUSAL_RETRY_TEXTURE 1
#endif

#if defined(UML_REFUSAL_RETRY_DRAW)
// A draw writes the one pixel of a 128 by 64 target that its index names, in
// a colour that the index names too, so that no two of 8192 draws write the
// same pixel or colour: a pixel tells a draw that ran from one that was
// dropped.
constexpr const char* vertexShader = R"(#version 450 core
uniform vec4 values[2048];
uniform int index;
out vec4 tint;
void main()
{
    tint = values[index & 2047];
    tint.a = (255.0 - 64.0 * float((index >> 11) & 3)) / 255.0;
    gl_Position = vec4((float(index & 127) + 0.5) / 64.0 - 1.0,
        (float((index >> 7) & 63) + 0.5) / 32.0 - 1.0, 0.0, 1.0);
    gl_PointSize = 1.0;
}
)";

constexpr const char* fragmentShader = R"(#version 450 core
in vec4 tint;
out vec4 color;
void main()
{
    color = tint;
}
)";
#endif

using Pixel = std::array<std::uint8_t, 4>;

[[nodiscard, maybe_unused]] Pixel pixelAt(GLint x, GLint y)
{
    Pixel pixel {};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    return pixel;
}

#if defined(UML_REFUSAL_RETRY_DRAW)

constexpr GLsizei targetWidth = 128;
constexpr GLsizei targetHeight = 64;
constexpr int pixelCount = targetWidth * targetHeight;

// The colour of the draw of an index, as the shader writes it.
[[nodiscard]] Pixel colourOf(int index)
{
    const int slot = index & 2047;
    return {static_cast<std::uint8_t>(slot & 255),
        static_cast<std::uint8_t>(((slot >> 8) & 7) * 32 + 8),
        static_cast<std::uint8_t>(0),
        static_cast<std::uint8_t>(255 - 64 * ((index >> 11) & 3))};
}

#endif

#if defined(UML_REFUSAL_RETRY_TEXTURE)

constexpr GLsizei side = 2048;

// The picture an upload writes: every pixel is told apart by its place.
[[nodiscard, maybe_unused]] std::vector<std::uint8_t> picture(
    GLsizei width, unsigned seed)
{
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(width) * 4);
    for (GLsizei y = 0; y < width; ++y) {
        for (GLsizei x = 0; x < width; ++x) {
            std::uint8_t* const pixel = &bytes[(static_cast<std::size_t>(y)
                                                       * static_cast<std::size_t>(width)
                                                   + static_cast<std::size_t>(x))
                * 4];
            pixel[0] = static_cast<std::uint8_t>(x + seed);
            pixel[1] = static_cast<std::uint8_t>(y * 3 + seed);
            pixel[2] = static_cast<std::uint8_t>(
                (x >> 8) | ((y >> 8) << 2) | (seed << 4));
            pixel[3] = 255;
        }
    }
    return bytes;
}

[[nodiscard, maybe_unused]] Pixel expected(
    const std::vector<std::uint8_t>& bytes, GLsizei width, GLsizei x, GLsizei y)
{
    const std::size_t at = (static_cast<std::size_t>(y)
                                   * static_cast<std::size_t>(width)
                               + static_cast<std::size_t>(x))
        * 4;
    return {bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]};
}

struct Target {
    GLuint texture {};
    GLuint framebuffer {};
};

// A texture with storage for a whole picture, cleared to red and idle, and a
// framebuffer that renders to it. Nothing is uploaded to it, so that no
// staging buffer of its size is left in the driver's caches.
[[nodiscard, maybe_unused]] Target makeTarget(GLsizei width)
{
    Target target;
    glGenTextures(1, &target.texture);
    glBindTexture(GL_TEXTURE_2D, target.texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, width);
    glGenFramebuffers(1, &target.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
        target.texture, 0);
    glClearColor(1.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    require(glGetError() == GL_NO_ERROR, "the target was not made");
    return target;
}

// Writes the upper half of the picture over the texture while the GPU still
// uses it: the clear that comes first is work the upload has to wait for,
// and the upload does not cover all of the texture, so the clear is not
// dropped; the driver stages the upload instead of copying it in directly.
// It needs 8 MiB of staging.
[[maybe_unused]] void uploadWhileBusy(
    const Target& target, GLsizei width, const std::vector<std::uint8_t>& bytes)
{
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glClearColor(1.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindTexture(GL_TEXTURE_2D, target.texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, width / 2, GL_RGBA,
        GL_UNSIGNED_BYTE, bytes.data());
}

constexpr std::array<std::array<GLsizei, 2>, 4> samples {
    {{0, 0}, {701, 305}, {2047, 1023}, {5, 900}}};

// Whether the texture shows the picture at the places the samples name, all
// of them in the half the upload writes.
[[nodiscard, maybe_unused]] bool shows(const Target& target, GLsizei width,
    const std::vector<std::uint8_t>& bytes)
{
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    for (const auto& at : samples) {
        if (pixelAt(at[0], at[1]) != expected(bytes, width, at[0], at[1]))
            return false;
    }
    return true;
}

#endif

// Hands back what finished work and the driver's caches hold, as the host
// does when it trims an idle context, and gives the budget's own accounting
// the time it takes to follow.
[[maybe_unused]] void giveBack(uml::gpu::HostGlProvider& provider)
{
    using TrimFunction = EGLBoolean (*)();
    const auto trim = reinterpret_cast<TrimFunction>(
        eglGetProcAddress("eglTrimContextMESA"));
    require(trim != nullptr, "the driver cannot trim the context");
    for (int round = 0; round < 3; ++round) {
        glFinish();
        require(trim() != EGL_FALSE, "the context was not trimmed");
        glFinish();
        (void)provider.releaseCachedMemory();
        std::this_thread::sleep_for(100ms);
    }
}

} // namespace

int main()
{
    std::unique_ptr<uml::gpu::VirglRenderer> renderer;
    try {
        renderer = std::make_unique<uml::gpu::VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }
    // The renderer states its own pool when it starts, which this replaces:
    // the driver charges its allocations to this one from here on.
    InjectedPool pool;

    uml::gpu::HostGlProvider provider;
    const auto current = uml::gpu::testing::currentContext(provider, 4, 6);
    if (current.context == nullptr)
        return uml::gpu::testing::skip(current.failure);

#if defined(UML_REFUSAL_RETRY_DRAW)
    constexpr int drawsPerRound = 256;
#if defined(UML_REFUSAL_RETRY_SUSTAINED)
    constexpr int totalDraws = 8192;
#else
    constexpr int totalDraws = 1536;
#endif

    const GLuint program = uml::gpu::testing::linkProgram(
        {uml::gpu::testing::compileShader(GL_VERTEX_SHADER, vertexShader),
            uml::gpu::testing::compileShader(
                GL_FRAGMENT_SHADER, fragmentShader)});
    glUseProgram(program);
    const GLint indexLocation = glGetUniformLocation(program, "index");
    const GLint valuesLocation = glGetUniformLocation(program, "values");
    require(indexLocation >= 0 && valuesLocation >= 0,
        "the program's uniforms were not found");

    GLuint vertexArray = 0;
    glGenVertexArrays(1, &vertexArray);
    glBindVertexArray(vertexArray);
    const auto target = uml::gpu::testing::colourTarget(targetWidth,
        targetHeight, uml::gpu::testing::TextureStorage::immutable);
    require(target.complete, "the target is not complete");
    glViewport(0, 0, targetWidth, targetHeight);
    glEnable(GL_PROGRAM_POINT_SIZE);

    // The colours the draws read, one for each of 2048 indices; the shader
    // sets the alpha. The target is cleared to black, which none of them is.
    std::vector<GLfloat> values;
    values.reserve(2048 * 4);
    for (int slot = 0; slot < 2048; ++slot) {
        const Pixel colour = colourOf(slot);
        for (int channel = 0; channel < 3; ++channel)
            values.push_back(static_cast<GLfloat>(colour[channel]) / 255.0F);
        values.push_back(1.0F);
    }
    glUniform4fv(valuesLocation, 2048, values.data());
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    // One draw first, of index 0, so that what the program needs is made
    // before the limit is set.
    glUniform1i(indexLocation, 0);
    glDrawArrays(GL_POINTS, 0, 1);
    glFinish();
    require(glGetError() == GL_NO_ERROR, "the first draw raised an error");
    // Every draw uploads its constants again, 32 KiB of them, so the
    // uploader's 2 MiB buffers fill up and the draws in flight keep them.
    const auto runDraws = [&]() {
        int refused = 0;
        int count = 0;
        while (count < totalDraws) {
            for (int index = 0; index < drawsPerRound; ++index) {
                glUniform1i(indexLocation, ++count);
                glDrawArrays(GL_POINTS, 0, 1);
            }
            const GLenum error = glGetError();
            if (error == GL_OUT_OF_MEMORY) {
                ++refused;
            } else {
                require(error == GL_NO_ERROR,
                    "a draw raised an error other than out of memory");
            }
        }
        return refused;
    };
#if defined(UML_REFUSAL_RETRY_SUSTAINED)
    // What the context keeps after draws that nothing refuses, which the same
    // draws under the limit must not exceed by what the refusals left.
    require(runDraws() == 0, "a draw was refused with no limit");
    giveBack(provider);
    const std::uint64_t baseline = InjectedPool::live();
    // The draws above painted the pixels the pressured draws are to paint:
    // none of them may be left to pass for one of those.
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    require(pixelAt(0, 0) == Pixel {0, 0, 0, 255} && pixelAt(5, 0)
            == Pixel {0, 0, 0, 255},
        "the target was not cleared between the draws");
#endif

    // The limit is eight of the uploader's buffers.
    pool.limit(16 * mebibyte);
    const auto begin = std::chrono::steady_clock::now();
    const int draws = totalDraws;
    const int refusedRounds = runDraws();
    glFinish();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    std::fprintf(stderr,
        "%d draws in %lld ms; %d rounds of %d met a refused upload; the pool "
        "refused %u requests\n",
        draws,
        static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count()),
        refusedRounds, drawsPerRound, pool.refusals());
    pool.unlimit();
    require(pool.refusals() > 0, "nothing was refused, so nothing was proven");
    require(refusedRounds == 0,
        "a draw was refused for memory that the finished work gives back");

    // Every draw ran: the pixel of each index shows the colour of its draw,
    // which no earlier draw and not the warm-up had painted, and the pixels
    // no draw named are black.
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(pixelCount) * 4);
    glReadPixels(0, 0, targetWidth, targetHeight, GL_RGBA, GL_UNSIGNED_BYTE,
        pixels.data());
    int wrong = 0;
    for (int slot = 0; slot < pixelCount; ++slot) {
        Pixel want {0, 0, 0, 255};
        if (slot <= totalDraws)
            want = colourOf(slot);
        const std::size_t at = static_cast<std::size_t>(slot) * 4;
        const Pixel got {
            pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]};
        if (got != want) {
            if (wrong == 0)
                std::fprintf(stderr, "pixel %d is %u,%u,%u,%u, not %u,%u,%u,%u\n",
                    slot, got[0], got[1], got[2], got[3], want[0], want[1],
                    want[2], want[3]);
            ++wrong;
        }
    }
    require(wrong == 0, "a draw did not leave its pixel");

#if defined(UML_REFUSAL_RETRY_SUSTAINED)
    // What the context holds is what it held after draws that nothing
    // refused, give or take what its uploaders keep for the next draws: a
    // buffer a refusal left behind is 2 MiB, and the limit has room for
    // eight.
    giveBack(provider);
    const std::uint64_t after = InjectedPool::live();
    std::fprintf(stderr,
        "live device memory: %llu after draws that nothing refused, %llu after "
        "the same draws under the limit\n",
        static_cast<unsigned long long>(baseline),
        static_cast<unsigned long long>(after));
    require(after <= baseline + 8 * mebibyte,
        "the draws left device memory behind that was not given back");
#endif
#elif defined(UML_REFUSAL_RETRY_UPLOAD)
    // The picture the upload writes, over a texture that is cleared to red.
    const auto second = picture(side, 2);

    const Target target = makeTarget(side);
    require(!shows(target, side, second), "the second picture is there");

    // The garbage: a texture of the same size the GPU used and the
    // application deleted. It keeps its memory until the context that used it
    // recycles its batch.
    {
        GLuint garbage = 0;
        glGenTextures(1, &garbage);
        glBindTexture(GL_TEXTURE_2D, garbage);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, side, side);
        GLuint framebuffer = 0;
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, garbage, 0);
        glClearColor(0.5F, 0.5F, 0.5F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &garbage);
    }
    require(glGetError() == GL_NO_ERROR, "the garbage was not made");

    // An upload that needs 8 MiB of staging, with 2 MiB of room and 16 MiB of
    // garbage: the texture shows its new picture when the garbage has been
    // given back.
    pool.limit(2 * mebibyte);
    uploadWhileBusy(target, side, second);
    glFinish();
    const GLenum raised = glGetError();
    std::fprintf(stderr, "the upload raised %#x; the pool refused %u requests\n",
        raised, pool.refusals());
    pool.unlimit();
    require(pool.refusals() > 0, "nothing was refused, so nothing was proven");
    require(shows(target, side, second),
        "the upload was dropped for memory that the finished work gives back");
#elif defined(UML_REFUSAL_RETRY_BUFFER)
    // The update of a busy 16 MiB buffer, 8 MiB of it, which the threaded
    // context sends through a map of the buffer: the map needs a staging
    // buffer, the limit has room for none, and a texture the application
    // deleted holds the memory that makes room.
    constexpr GLsizeiptr bytes = 16 << 20;
    const std::vector<std::uint8_t> initial(static_cast<std::size_t>(bytes), 0x11);
    const std::vector<std::uint8_t> update(static_cast<std::size_t>(bytes / 2), 0x55);
    std::array<GLuint, 2> buffers {};
    glCreateBuffers(2, buffers.data());
    for (const GLuint buffer : buffers)
        glNamedBufferStorage(
            buffer, bytes, initial.data(), GL_DYNAMIC_STORAGE_BIT);
    glFinish();

    const auto anchor = uml::gpu::testing::colourTarget(
        4, 4, uml::gpu::testing::TextureStorage::immutable);
    require(anchor.complete, "the anchor is not complete");
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    {
        // 16 MiB of texture, of which the buffer's update needs half.
        constexpr GLsizei garbageSide = 2048;
        const auto garbage = uml::gpu::testing::colourTarget(garbageSide,
            garbageSide, uml::gpu::testing::TextureStorage::immutable);
        require(garbage.complete, "the garbage is not complete");
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        glBindFramebuffer(GL_FRAMEBUFFER, anchor.framebuffer);
        glDeleteFramebuffers(1, &garbage.framebuffer);
        glDeleteTextures(1, &garbage.texture);
    }
    glClear(GL_COLOR_BUFFER_BIT);
    require(glGetError() == GL_NO_ERROR, "the garbage was not made");

    pool.limit(2 * mebibyte);
    // The copy keeps the destination busy, so that a partial update of it
    // is staged.
    glCopyNamedBufferSubData(buffers[0], buffers[1], 0, 0, bytes);
    glNamedBufferSubData(buffers[1], 0, bytes / 2, update.data());
    glFinish();
    const GLenum raised = glGetError();
    std::array<std::uint8_t, 4> first {};
    std::array<std::uint8_t, 4> middle {};
    std::array<std::uint8_t, 4> last {};
    glGetNamedBufferSubData(buffers[1], 0, first.size(), first.data());
    glGetNamedBufferSubData(
        buffers[1], bytes / 2 - 4, middle.size(), middle.data());
    glGetNamedBufferSubData(buffers[1], bytes - 4, last.size(), last.data());
    std::fprintf(stderr,
        "the update raised %#x; the pool refused %u requests; the buffer holds "
        "%02x ... %02x | %02x\n",
        raised, pool.refusals(), first[0], middle[3], last[3]);
    pool.unlimit();
    require(pool.refusals() > 0, "nothing was refused, so nothing was proven");
    require(first[0] == 0x55 && middle[3] == 0x55,
        "the update was dropped for memory that the finished work gives back");
    require(last[3] == 0x11, "the update wrote past its range");
#elif defined(UML_REFUSAL_RETRY_CYCLE)
    // A texture an upload was refused for must not outlive its deletion: the
    // refused map held a reference to it, and to the staging buffer it had
    // made, and kept the texture's 16 MiB alive once the application had
    // deleted it.
    const auto bytes = picture(side, 3);
    const Target anchor = makeTarget(4);
    for (int round = 0; round < 3; ++round) {
        const std::uint64_t before = InjectedPool::live();
        const Target target = makeTarget(side);
        pool.limit(2 * mebibyte);
        uploadWhileBusy(target, side, bytes);
        glFinish();
        pool.unlimit();
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, anchor.framebuffer);
        glDeleteFramebuffers(1, &target.framebuffer);
        glDeleteTextures(1, &target.texture);
        glClear(GL_COLOR_BUFFER_BIT);
        giveBack(provider);
        const std::uint64_t after = InjectedPool::live();
        std::fprintf(stderr,
            "round %d: %llu bytes live before the texture, %llu after it was "
            "deleted; the pool refused %u requests\n",
            round, static_cast<unsigned long long>(before),
            static_cast<unsigned long long>(after), pool.refusals());
        require(after <= before + 2 * mebibyte,
            "a refused upload kept the memory of the texture it was for");
    }
    require(pool.refusals() > 0, "nothing was refused, so nothing was proven");
    require(glGetError() == GL_NO_ERROR, "the cycles raised an error");
#elif defined(UML_REFUSAL_RETRY_PERSIST)
    // An upload that needs 8 MiB of staging, with the limit 2 MiB above what the live textures
    // hold and nothing finished to give back. It is dropped, as it always
    // was, and the host, the context, and the time of the calls are unharmed.
    const auto picture3 = picture(side, 3);
    const Target target = makeTarget(side);
    pool.limit(2 * mebibyte);
    const auto begin = std::chrono::steady_clock::now();
    constexpr int refusedUploads = 8;
    unsigned refusedByFirst = 0;
    for (int count = 0; count < refusedUploads; ++count) {
        uploadWhileBusy(target, side, picture3);
        if (count == 0) {
            glFinish();
            refusedByFirst = pool.refusals();
        }
    }
    glFinish();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const auto elapsedMs = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    std::fprintf(stderr,
        "%d uploads that cannot fit took %lld ms; the pool refused %u "
        "requests; they raised %#x\n",
        refusedUploads, elapsedMs, pool.refusals(), glGetError());
    require(pool.refusals() > 0,
        "the uploads were not refused, so nothing was proven");
    // The first upload is the one that reclaims: the request is made, and
    // once more after the reclaim, so the pool refuses at least two
    // reservations; and each of the two reserves memory once or twice (the
    // driver releases its own caches and tries its allocation again), so it
    // refuses at most four. A request made six times or more would exceed
    // that. The helper's own test covers that nothing in it replays a request.
    std::fprintf(stderr, "the first upload met %u refusals\n", refusedByFirst);
    require(refusedByFirst >= 2 && refusedByFirst <= 4,
        "a refused upload was not made once, and once more after a reclaim");
    require(elapsed < 20s, "uploads that cannot fit took too long");
    require(!shows(target, side, picture3),
        "an upload that cannot fit landed, so nothing was proven");

    // The limit goes, and the same upload lands.
    pool.unlimit();
    uploadWhileBusy(target, side, picture3);
    glFinish();
    require(shows(target, side, picture3),
        "the upload did not land once the limit was lifted");
#endif

    std::fprintf(stderr, "Refusal retry (%s): ok\n", shapeName);
    return EXIT_SUCCESS;
}

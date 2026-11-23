// SPDX-License-Identifier: GPL-2.0-or-later

// Mesa's glthread copies the data of a glBufferSubData into a 1 MiB upload
// buffer that the host driver allocates for it, and lets the driver copy from
// that buffer into the destination later. With a bounded device memory budget
// the driver refuses the upload buffer once the budget is full. glthread then
// dereferenced the buffer it never got: the host faulted on the thread that
// serves the guest's transfers (vrend_transfer_write_storage ->
// glBufferSubData -> _mesa_glthread_upload), right after "zink: couldn't
// allocate memory", and took the emulator down.
//
// The contract under test is what the host does when that allocation is
// refused:
//
//  - the host survives however many uploads meet the refusal;
//  - the call itself raises at most GL_OUT_OF_MEMORY, which is what a driver
//    that cannot stage a write reports; glthread takes its ordinary path,
//    which copies the data into the command stream and needs no upload
//    buffer, so the guest may even get its data; and
//  - once the memory comes back, uploads land: the next ones allocate the
//    upload buffer again.
//
// The budget is filled with buffers of decreasing size until even 16 KiB is
// refused, so the upload buffer and the staging memory of a write both fail,
// and each round uploads more than two upload buffers' worth.

#include "host_gl_test_support.hpp"
#include "host_gpu_memory.hpp"
#include "require.hpp"
#include "virgl_renderer.hpp"

#include <epoxy/gl.h>

#include <algorithm>
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

using uml::gpu::testing::skipWithoutRenderer;
using uml::testing::require;
using namespace std::chrono_literals;

constexpr std::uint64_t kibibyte = 1024ULL;
constexpr std::uint64_t mebibyte = 1024ULL * kibibyte;
constexpr std::uint64_t roomBytes = 32 * mebibyte;
constexpr GLsizeiptr destinationBytes = static_cast<GLsizeiptr>(mebibyte);
constexpr GLsizeiptr chunkBytes = static_cast<GLsizeiptr>(64 * kibibyte);
// glthread's upload buffer is 1 MiB; 40 chunks of 64 KiB need three of them.
constexpr int chunksPerRound = 40;
constexpr int roundsWhileFull = 3;
constexpr int maximumFillers = 4096;

// What the pool counts: the larger of Metal's allocation and the graphics
// footprint, since nothing here is imported.
[[nodiscard]] std::uint64_t used() noexcept
{
    return std::max(uml::platform::hostGpuAllocatedBytes(),
        uml::platform::hostGraphicsFootprintBytes());
}

// The bytes chunk `index` of a round writes: distinct per round, so a stale
// copy from an earlier round cannot pass for a landed one.
void fillChunk(std::vector<std::uint8_t>& chunk, int round, int index)
{
    for (std::size_t byte = 0; byte < chunk.size(); ++byte)
        chunk[byte] = static_cast<std::uint8_t>(
            (byte * 7U + static_cast<unsigned>(index) * 13U +
                static_cast<unsigned>(round) * 31U) & 0xFFU);
}

// Takes the buffers of one size until the budget refuses the next, and keeps
// them. The error flag is cleared on the way out.
void fillBudget(std::vector<GLuint>& fillers, GLsizeiptr size)
{
    for (;;) {
        require(fillers.size() < maximumFillers,
            "the budget never filled, so nothing was proven");
        GLuint buffer = 0;
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_COPY_READ_BUFFER, buffer);
        glBufferData(GL_COPY_READ_BUFFER, size, nullptr, GL_STATIC_DRAW);
        glFinish();
        const GLenum error = glGetError();
        if (error == GL_OUT_OF_MEMORY) {
            glDeleteBuffers(1, &buffer);
            glFinish();
            (void)glGetError();
            return;
        }
        require(error == GL_NO_ERROR,
            "filling the budget raised an error other than out of memory");
        fillers.push_back(buffer);
    }
}

} // namespace

int main()
{
    // The pool takes its limit when the renderer starts, and counts everything
    // the process holds, so the limit is what is held now plus the room.
    uml::gpu::VirglRenderer::setDeviceMemoryLimit(used() + roomBytes);
    std::unique_ptr<uml::gpu::VirglRenderer> renderer;
    try {
        renderer = std::make_unique<uml::gpu::VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }

    uml::gpu::HostGlProvider provider;
    const auto current = uml::gpu::testing::currentContext(provider, 4, 6);
    if (current.context == nullptr)
        return uml::gpu::testing::skip(current.failure);

    // The destination exists before the budget fills, so only the upload
    // path can meet the refusal.
    GLuint destination = 0;
    glGenBuffers(1, &destination);
    glBindBuffer(GL_ARRAY_BUFFER, destination);
    glBufferData(GL_ARRAY_BUFFER, destinationBytes, nullptr, GL_DYNAMIC_DRAW);
    glFinish();
    require(glGetError() == GL_NO_ERROR, "the destination was not created");

    // Uploads with room to spare land, which also proves the uploads below are
    // the ones glthread takes through its upload buffer: offset > 0, data set.
    std::vector<std::uint8_t> chunk(static_cast<std::size_t>(chunkBytes));
    std::vector<std::uint8_t> readback(chunk.size());
    const auto landed = [&](int round, int index) {
        fillChunk(chunk, round, index);
        const GLintptr offset = static_cast<GLintptr>(4096 + index * 128);
        glBindBuffer(GL_ARRAY_BUFFER, destination);
        glBufferSubData(GL_ARRAY_BUFFER, offset, chunkBytes, chunk.data());
        glFinish();
        if (glGetError() != GL_NO_ERROR)
            return false;
        std::fill(readback.begin(), readback.end(), std::uint8_t {0});
        glGetBufferSubData(
            GL_ARRAY_BUFFER, offset, chunkBytes, readback.data());
        glFinish();
        return glGetError() == GL_NO_ERROR && readback == chunk;
    };
    require(landed(0, 0), "an upload with the budget free did not land");

    std::vector<GLuint> fillers;
    for (const GLsizeiptr size : {static_cast<GLsizeiptr>(mebibyte),
             static_cast<GLsizeiptr>(256 * kibibyte),
             static_cast<GLsizeiptr>(64 * kibibyte),
             static_cast<GLsizeiptr>(16 * kibibyte)})
        fillBudget(fillers, size);
    std::fprintf(stderr,
        "%zu buffers fill the budget (limit %.1f MiB over %.1f MiB)\n",
        fillers.size(), static_cast<double>(roomBytes) / mebibyte,
        static_cast<double>(used()) / mebibyte);

    // The storm. Each round uploads 2.5 MiB in 64 KiB pieces, so glthread
    // asks the driver for a new upload buffer at least twice per round, and
    // the driver has none to give.
    int outOfMemory = 0;
    for (int round = 1; round <= roundsWhileFull; ++round) {
        for (int index = 0; index < chunksPerRound; ++index) {
            fillChunk(chunk, round, index);
            glBindBuffer(GL_ARRAY_BUFFER, destination);
            glBufferSubData(GL_ARRAY_BUFFER,
                static_cast<GLintptr>(4096 + (index % 8) * 128), chunkBytes,
                chunk.data());
        }
        glFinish();
        const GLenum error = glGetError();
        if (error == GL_OUT_OF_MEMORY) {
            ++outOfMemory;
        } else {
            require(error == GL_NO_ERROR,
                "an upload raised an error other than out of memory");
        }
        (void)glGetError();
    }
    std::fprintf(stderr,
        "%d of %d rounds raised out of memory; the host survived\n",
        outOfMemory, roundsWhileFull);

    // The budget comes back: the GPU finishes the work in flight, the filler
    // buffers go, and the next uploads allocate their buffer again.
    glDeleteBuffers(static_cast<GLsizei>(fillers.size()), fillers.data());
    glFinish();
    (void)glGetError();
    bool recovered = false;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    int attempt = 0;
    while (!recovered && std::chrono::steady_clock::now() < deadline) {
        recovered = true;
        for (int index = 0; index < chunksPerRound && recovered; ++index)
            recovered = landed(100 + attempt, index % 8);
        ++attempt;
        if (!recovered)
            std::this_thread::sleep_for(50ms);
    }
    require(recovered, "uploads did not resume once the memory came back");

    std::fputs("glthread upload refusal: ok\n", stderr);
    return EXIT_SUCCESS;
}

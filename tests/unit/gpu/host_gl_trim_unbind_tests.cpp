// SPDX-License-Identifier: GPL-2.0-or-later

// A context the renderer trims right after it flushed, and that another
// context then replaces on its thread. The share group holds an EGL image, so
// the unbind flushes the old context synchronously, and that flush checks the
// fence of the batch the trim queued: Mesa's threaded context finds it
// signalled without taking a lock and goes on to read what the driver thread
// stored before it signalled, the fences the driver is to signal at its next
// flush. A load that was not ordered after the fence's flag saw the count of
// those fences without the fence itself, and the host's GL thread faulted in
// util_queue_fence_signal.
//
// The memory ordering cannot be made to fail from here: the stale read is a
// window of a few nanoseconds, and Mesa's util test u_queue_fence_order_test
// discriminates the fix. This test keeps the path that reached it exercised
// under load, on four threads, so a change to the flush notify, the trim or
// the unbind that breaks the path fails here.

#include "host_gl_test_support.hpp"
#include "require.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>

namespace {

using uml::testing::require;
using namespace std::chrono_literals;

typedef EGLBoolean (*TrimContext)();
typedef EGLBoolean (*DestroyImage)(EGLDisplay, EGLImageKHR);

constexpr int workerCount = 4;
constexpr int iterationsPerWorker = 20000;
// A slow host makes fewer iterations; only too few to prove anything fails.
constexpr auto timeBudget = 20s;
constexpr std::uint64_t maxDelayNanoseconds = 200000;

std::atomic<std::uint64_t> iterations {};
std::atomic<int> failures {};

void fail(const char* what, int worker, int iteration)
{
    std::fprintf(stderr, "worker %d iteration %d: %s\n", worker, iteration, what);
    ++failures;
}

void spinFor(std::uint64_t nanoseconds)
{
    const auto end = std::chrono::steady_clock::now() +
        std::chrono::nanoseconds(nanoseconds);
    while (std::chrono::steady_clock::now() < end) {
    }
}

void worker(
    uml::gpu::HostGlProvider& provider,
    int index,
    TrimContext trim,
    std::chrono::steady_clock::time_point deadline)
{
    void* const home = provider.createContext(
        uml::gpu::testing::glContextParameters(4, 6, true));
    if (home == nullptr || provider.makeCurrent(home) != 0) {
        fail("no home context", index, 0);
        return;
    }
    std::uint64_t state = 0x9e3779b97f4a7c15ULL ^ static_cast<std::uint64_t>(index);
    for (int i = 0; i < iterationsPerWorker && failures == 0 &&
         std::chrono::steady_clock::now() < deadline;
         ++i) {
        void* const context = provider.createContext(
            uml::gpu::testing::glContextParameters(4, 6, true));
        if (context == nullptr || provider.makeCurrent(context) != 0) {
            fail("no shared context", index, i);
            break;
        }
        glFlush();
        if (trim() != EGL_TRUE) {
            fail("trim failed", index, i);
            break;
        }
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        spinFor((state >> 11) % maxDelayNanoseconds);
        // Another context replaces it: the unbind flushes it on this thread.
        if (provider.makeCurrent(home) != 0) {
            fail("the home context would not bind again", index, i);
            break;
        }
        if (glGetError() != GL_NO_ERROR) {
            fail("GL error", index, i);
            break;
        }
        provider.destroyContext(context);
        ++iterations;
    }
    (void)provider.makeCurrent(nullptr);
    provider.destroyContext(home);
}

} // namespace

int main()
{
    uml::gpu::HostGlProvider provider;
    // The first context is the share root, as the renderer's context 0 is.
    const auto root = uml::gpu::testing::currentContext(provider, 4, 6);
    if (root.context == nullptr)
        return uml::gpu::testing::skip(root.failure);

    const EGLDisplay display = eglGetCurrentDisplay();
    require(display != EGL_NO_DISPLAY, "no current EGL display");
    require(epoxy_has_egl_extension(display, "EGL_MESA_context_trim"),
        "the driver does not advertise EGL_MESA_context_trim");
    const auto trim = reinterpret_cast<TrimContext>(
        eglGetProcAddress("eglTrimContextMESA"));
    require(trim != nullptr, "eglTrimContextMESA was not found");
    // Found now: epoxy resolves an EGL function for the display that is
    // current on the calling thread, and none is current here at the end.
    const auto destroyImage = reinterpret_cast<DestroyImage>(
        eglGetProcAddress("eglDestroyImageKHR"));
    require(destroyImage != nullptr, "eglDestroyImageKHR was not found");

    // An EGL image of a texture of the share group marks the group as holding
    // memory another API may use, which is what makes a flush synchronous.
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA,
        GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    const EGLImage image = eglCreateImage(display, eglGetCurrentContext(),
        EGL_GL_TEXTURE_2D,
        reinterpret_cast<EGLClientBuffer>(static_cast<std::uintptr_t>(texture)),
        nullptr);
    require(image != EGL_NO_IMAGE, "no EGL image of the share group's texture");
    glFinish();
    require(glGetError() == GL_NO_ERROR, "the share root reported an error");
    // The workers have their own contexts; this thread does no GL again until
    // they are done.
    require(provider.makeCurrent(nullptr) == 0, "the share root would not release");

    const auto deadline = std::chrono::steady_clock::now() + timeBudget;
    std::vector<std::thread> workers;
    for (int index = 0; index < workerCount; ++index)
        workers.emplace_back(worker, std::ref(provider), index, trim, deadline);
    for (auto& thread : workers)
        thread.join();

    require(failures == 0, "a worker failed");
    require(iterations >= static_cast<std::uint64_t>(workerCount) * 20,
        "the workers made too few iterations for the test to prove anything");

    require(destroyImage(display, image) == EGL_TRUE, "the image was not destroyed");
    provider.destroyContext(root.context);
    std::printf("PASS trim and unbind: %d threads, %llu iterations\n", workerCount,
        static_cast<unsigned long long>(iterations.load()));
    return EXIT_SUCCESS;
}

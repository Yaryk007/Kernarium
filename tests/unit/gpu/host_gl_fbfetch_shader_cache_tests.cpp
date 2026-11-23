// SPDX-License-Identifier: GPL-2.0-or-later

// Every guest context lives on one host zink screen, and the screen keeps one
// cache of the shaders its contexts created. A fragment shader that reads the
// framebuffer (the GLSL linker lowers KHR_blend_equation_advanced to
// framebuffer fetch) reads a descriptor binding that only a context's own push
// layout provides, and zink added that binding where the shader was created,
// so only to the context that missed the cache. A second context that drew
// the same fragment shader found it in the cache, never added the binding, and
// KosmicKrisp then read the sixth binding of a layout of five: a wild pointer
// the host process faulted on (a field crash in lookup_ycbcr_conversion).
//
// The read is past the end of a heap block, so it faults deterministically
// only under Guard Malloc, which ends every allocation at a page boundary.
// The test therefore runs its body in a child process that this one starts
// with Guard Malloc inserted and Mesa's shader cache disabled (a disk cache
// would hand the second context its compiled variants and skip the compile
// that reads the layout). The child brings up the host GL stack as the
// renderer does, a share root and a shared context, and has each draw with the
// same advanced-blend fragment shader and its own vertex shader. The parent
// passes when the child exits with status 0.
//
// It is not run under AddressSanitizer, whose allocator Guard Malloc cannot
// share a process with.

#include "host_gl_test_support.hpp"
#include "require.hpp"

#include <epoxy/gl.h>

#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UML_TEST_ADDRESS_SANITIZER 1
#endif
#endif

namespace {

using uml::testing::require;
using namespace std::chrono_literals;

constexpr const char* childArgument = "--child";
constexpr const char* guardMalloc = "/usr/lib/libgmalloc.dylib";
constexpr int guardMallocMissingExitCode = 2;
// The child needs a second or two; a loaded host, many times that.
constexpr auto childTimeBudget = 100s;

// The fragment shader reads the framebuffer to blend.
constexpr const char* fragmentSource = R"(#version 460
#extension GL_KHR_blend_equation_advanced : require
layout(blend_support_multiply) out;
layout(location = 0) out vec4 color;
void main() { color = vec4(0.5, 1.0, 0.0, 1.0); }
)";

// Two vertex shaders, so that the two contexts link different programs around
// the one fragment shader.
constexpr const char* vertexSourceFirst = R"(#version 460
void main() {
    vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0,
                  float((gl_VertexID & 2) << 1) - 1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";
constexpr const char* vertexSourceSecond = R"(#version 460
void main() {
    vec2 p = vec2(float((gl_VertexID & 2) << 1) - 1.0,
                  float((gl_VertexID & 1) << 2) - 1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

[[nodiscard]] bool guardMallocLoaded()
{
    for (std::uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const char* const name = _dyld_get_image_name(i);
        if (name != nullptr && std::strstr(name, "libgmalloc") != nullptr)
            return true;
    }
    return false;
}

// Draws one triangle over a cleared target with multiply blending, in the
// current context.
void drawWith(const char* vertexSource)
{
    const GLuint program = uml::gpu::testing::linkProgram({
        uml::gpu::testing::compileShader(GL_VERTEX_SHADER, vertexSource),
        uml::gpu::testing::compileShader(GL_FRAGMENT_SHADER, fragmentSource),
    });
    const auto target = uml::gpu::testing::colourTarget(
        16, 16, uml::gpu::testing::TextureStorage::immutable);
    require(target.complete, "the render target is incomplete");
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glViewport(0, 0, 16, 16);
    glDisable(GL_CULL_FACE);
    glClearColor(0.5F, 0.5F, 0.5F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendEquation(GL_MULTIPLY_KHR);
    glUseProgram(program);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    require(glGetError() == GL_NO_ERROR, "the draw raised a GL error");
}

// The body: runs in the child, with Guard Malloc.
int runChild()
{
    if (!guardMallocLoaded()) {
        std::fprintf(stderr, "Guard Malloc is not loaded in the child\n");
        return guardMallocMissingExitCode;
    }
    uml::gpu::HostGlProvider provider;
    // The first context is the share root, as the renderer's context 0 is.
    const auto root = uml::gpu::testing::currentContext(provider, 4, 6);
    if (root.context == nullptr)
        return uml::gpu::testing::skip(root.failure);
    if (!epoxy_has_gl_extension("GL_KHR_blend_equation_advanced"))
        return uml::gpu::testing::skip(
            "the host driver does not expose GL_KHR_blend_equation_advanced");
    void* const guest = provider.createContext(
        uml::gpu::testing::glContextParameters(4, 6, true));
    require(guest != nullptr, "no shared host GL context");

    // The root makes the shader and draws with it; the guest context then
    // draws with the same fragment shader, which the screen's cache serves.
    drawWith(vertexSourceFirst);
    require(provider.makeCurrent(guest) == 0,
        "the shared context would not bind");
    drawWith(vertexSourceSecond);

    (void)provider.makeCurrent(nullptr);
    provider.destroyContext(guest);
    provider.destroyContext(root.context);
    return 0;
}

[[nodiscard]] std::string executablePath()
{
    std::vector<char> path(4096);
    auto size = static_cast<std::uint32_t>(path.size());
    require(_NSGetExecutablePath(path.data(), &size) == 0,
        "the test's own path is too long");
    return path.data();
}

// Starts this binary as the child, under Guard Malloc and with Mesa's shader
// cache off, and returns its wait status.
[[nodiscard]] int runChildUnderGuardMalloc()
{
    std::vector<std::string> variables;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const std::string text = *entry;
        if (text.starts_with("DYLD_INSERT_LIBRARIES=") ||
            text.starts_with("MESA_SHADER_CACHE_DISABLE="))
            continue;
        variables.push_back(text);
    }
    variables.push_back(std::string("DYLD_INSERT_LIBRARIES=") + guardMalloc);
    variables.emplace_back("MESA_SHADER_CACHE_DISABLE=true");
    std::vector<char*> environment;
    for (std::string& text : variables)
        environment.push_back(text.data());
    environment.push_back(nullptr);

    const std::string path = executablePath();
    std::array<char*, 3> arguments {
        const_cast<char*>(path.c_str()),
        const_cast<char*>(childArgument),
        nullptr,
    };
    pid_t child = 0;
    const int spawned = posix_spawn(&child, path.c_str(), nullptr, nullptr,
        arguments.data(), environment.data());
    require(spawned == 0, "the child could not be started");

    const auto deadline = std::chrono::steady_clock::now() + childTimeBudget;
    int status = 0;
    for (;;) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            return status;
        require(waited == 0, "waiting for the child failed");
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(child, SIGKILL);
            (void)waitpid(child, &status, 0);
            uml::testing::fail("the child did not finish in time");
        }
        std::this_thread::sleep_for(50ms);
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], childArgument) == 0)
        return runChild();

#ifdef UML_TEST_ADDRESS_SANITIZER
    return uml::gpu::testing::skip(
        "Guard Malloc cannot share a process with AddressSanitizer");
#else
    if (::access(guardMalloc, R_OK) != 0)
        return uml::gpu::testing::skip("Guard Malloc is not installed");

    const int status = runChildUnderGuardMalloc();
    if (WIFSIGNALED(status)) {
        const int signal = WTERMSIG(status);
        std::fprintf(stderr, "the child ended with signal %d\n", signal);
        if (signal == SIGSEGV || signal == SIGBUS)
            uml::testing::fail(
                "the child faulted: a context that drew a cached "
                "framebuffer-fetch fragment shader read past the end of its "
                "descriptor layout");
        uml::testing::fail("the child was killed");
    }
    require(WIFEXITED(status), "the child neither exited nor was signalled");
    const int code = WEXITSTATUS(status);
    if (code == uml::gpu::testing::skippedExitCode)
        return code;
    require(code != guardMallocMissingExitCode,
        "the child ran without Guard Malloc, so it cannot show the fault");
    require(code == 0, "the child failed");
    return 0;
#endif
}

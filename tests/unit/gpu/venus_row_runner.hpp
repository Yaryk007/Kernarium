// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// What the tests that give the Venus renderer a guest's hand-encoded command
// streams share: a context with a device and a reply stream (Session), a table
// of rows, and the runner that gives each row a process of its own.
//
// The overruns and the stale pointers these rows provoke lie past the end of a
// heap block or in freed memory, so they fault deterministically only under
// Guard Malloc, which ends every allocation at a page boundary and unmaps what
// is freed. The runner therefore runs each row in a child process that this
// one starts with Guard Malloc inserted, and the child names the frames of a
// fault on its standard error. The parent passes when every child exits with
// status 0.
//
// Fresh memory from a fresh page is zero, which hides a reply that carries a
// byte the renderer never wrote. A row that looks for one sets `fillsMemory`,
// and its child runs with MALLOC_FILL_SPACE, which fills each block Guard
// Malloc hands out with a pattern, so a byte nobody wrote shows as the
// pattern. The fill slows every allocation, so the other rows go without it.
//
// An object the renderer fails to free leaves nothing for Guard Malloc to
// fault on, so a row that names one is run a second time under the leak
// checker (leaks --atExit), which examines the process when the row, the
// context and the renderer are gone, and fails the row when it reports a leak
// that a renderer function allocated.
//
// It is not run under AddressSanitizer, whose allocator Guard Malloc cannot
// share a process with.

#include "require.hpp"
#include "venus_wire.hpp"

#include <execinfo.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UML_TEST_ADDRESS_SANITIZER 1
#endif
#endif

namespace uml::gpu::testing::venus {

constexpr std::uint32_t replyResourceId = 900;
constexpr std::uint64_t replySize = 16384;       // one host page
constexpr std::size_t replyBytesKept = 1024;

// The renderer says why it refuses a command; the log makes a failure here
// read as one.
inline std::string& rendererLog()
{
    static std::string log;
    return log;
}
inline void logRenderer(int, const char* message, void*)
{
    std::fprintf(stderr, "  renderer: %s", message);
    rendererLog() += message;
}

// True when the renderer has logged a line with this text since the row began.
inline bool rendererSaid(const char* text)
{
    return rendererLog().find(text) != std::string::npos;
}

inline void retireFence(std::uint32_t, std::uint32_t, std::uint64_t) {}
inline void contextFatal(std::uint32_t) {}

// What the host did with a command that asked for a reply.
struct Answer {
    bool submitted = false;      // false: the renderer lost the context
    std::int32_t command = 0;    // the reply's command type, when there is one
    std::int32_t result = 0;     // ... and its VkResult
    // The first bytes of the reply stream, for a reply that holds more than a
    // result.
    std::array<std::uint8_t, replyBytesKept> raw {};
};

// A Venus context with a device and a shared-memory reply stream, all gone
// again when the session ends. `setup` encodes what brings the device up.
class Session {
public:
    Session(std::uint32_t ctxId, const std::function<void(Stream&)>& setup,
        std::uint32_t resourceId = replyResourceId)
        : ctxId_(ctxId), resourceId_(resourceId)
    {
        uml::testing::require(
            vkr_renderer_create_context(ctxId_, virtgpuDrmCapsetVenus, 4, "test"),
            "the Venus context could not be created");
        Stream stream;
        setup(stream);
        uml::testing::require(
            vkr_renderer_submit_cmd(ctxId_, stream.bytes().data(), stream.size()),
            "the context was lost bringing the device up; the host driver may be "
            "unavailable in this environment, or lacks what the row needs");

        int fdType = 0;
        int fd = -1;
        std::uint32_t mapInfo = 0;
        alignas(16) unsigned char vulkanInfo[256] {};
        uml::testing::require(
            vkr_renderer_create_resource(ctxId_, resourceId_, 0, replySize,
                blobFlagUseMappable, &fdType, &fd, &mapInfo, vulkanInfo),
            "the reply stream could not be created");
        fd_ = fd;
        reply_ = mmap(nullptr, replySize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        uml::testing::require(reply_ != MAP_FAILED, "the reply stream could not be mapped");
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session()
    {
        munmap(reply_, replySize);
        close(fd_);
        vkr_renderer_destroy_resource(ctxId_, resourceId_);
        vkr_renderer_destroy_context(ctxId_);
    }

    // Sends one command, with the reply stream set first, and reads the reply.
    Answer ask(Stream& command)
    {
        std::memset(reply_, 0, replyBytesKept);
        Stream stream;
        stream.command(setReplyCommandStream);
        stream.ptr(true);
        stream.u32(resourceId_); stream.u64(0); stream.u64(replySize);
        stream.bytes().insert(stream.bytes().end(), command.bytes().begin(),
            command.bytes().end());
        Answer answer;
        answer.submitted = vkr_renderer_submit_cmd(ctxId_, stream.bytes().data(), stream.size());
        std::memcpy(answer.raw.data(), reply_, answer.raw.size());
        std::int32_t words[2] {};
        std::memcpy(words, reply_, sizeof(words));
        answer.command = words[0];
        answer.result = words[1];
        return answer;
    }

    // Sends a command no reply is asked for; false when the context was lost.
    bool send(Stream& command)
    {
        return vkr_renderer_submit_cmd(ctxId_, command.bytes().data(), command.size());
    }

private:
    std::uint32_t ctxId_;
    std::uint32_t resourceId_;
    int fd_ = -1;
    void* reply_ = nullptr;
};

struct Row {
    const char* name;
    std::function<void(Session&)> run;
    bool leakChecked = false;  // also run under the leak checker
    // How this row's device comes up, when not as the suite's does.
    std::function<void(Stream&)> setup = {};
    // Run the child with each fresh block filled with a pattern, for a row that
    // asserts a reply holds only what was written.
    bool fillsMemory = false;
};

inline void requireAnswered(const Answer& answer, Cmd command, std::int32_t result, const char* what)
{
    uml::testing::require(answer.submitted,
        std::string(what) + ": the context was lost instead of the command being answered");
    uml::testing::require(answer.command == static_cast<std::int32_t>(command),
        std::string(what) + ": the reply names another command");
    if (answer.result != result)
        std::fprintf(stderr, "  answered %d, expected %d\n", answer.result, result);
    uml::testing::require(answer.result == result, what);
}

namespace row_runner {

constexpr const char* childArgument = "--child";
constexpr const char* leaksArgument = "--leaks";
constexpr const char* leaksTool = "/usr/bin/leaks";
constexpr const char* guardMalloc = "/usr/lib/libgmalloc.dylib";
constexpr int guardMallocMissingExitCode = 2;
constexpr int skippedExitCode = 77;
constexpr auto childTimeBudget = std::chrono::seconds(100);

// What one test gives the runner: its rows and how its device comes up.
struct Suite {
    const std::vector<Row>& rows;
    std::function<void(Stream&)> setup;
    const char* noun;       // "pipeline binary request" in "N pipeline binary request rows failed"
};

// Names the frames of a fault, so that a row that crashes the host says where.
inline void reportFault(int sig, siginfo_t* info, void*)
{
    char text[128];
    const int length = std::snprintf(text, sizeof(text),
        "FAULT: signal %d at address %p\n", sig, info->si_addr);
    (void)!write(STDERR_FILENO, text, static_cast<std::size_t>(length));
    void* frames[40];
    const int count = backtrace(frames, 40);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig);
}

inline void installFaultReport()
{
    struct sigaction action {};
    action.sa_sigaction = reportFault;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGABRT})
        sigaction(sig, &action, nullptr);
}

[[nodiscard]] inline bool guardMallocLoaded()
{
    for (std::uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const char* const name = _dyld_get_image_name(i);
        if (name != nullptr && std::strstr(name, "libgmalloc") != nullptr)
            return true;
    }
    return false;
}

// Runs one row in this process and ends the renderer, so that nothing but a
// leak is left of what the row made.
inline int runRow(const Suite& suite, const char* name)
{
    for (const Row& row : suite.rows) {
        if (std::strcmp(name, row.name) != 0)
            continue;
        const vkr_renderer_callbacks callbacks {
            .debug_logger = logRenderer,
            .retire_fence = retireFence,
            .context_fatal = contextFatal,
        };
        uml::testing::require(
            vkr_renderer_init(vkrRendererThreadSync | vkrRendererAsyncFenceCb, &callbacks),
            "the Venus renderer could not be initialised");
        {
            Session session(1, row.setup ? row.setup : suite.setup);
            row.run(session);
        }
        vkr_renderer_fini();
        return 0;
    }
    uml::testing::fail("no row has the name given");
}

// The body of a child: runs one row, with Guard Malloc.
inline int runChild(const Suite& suite, const char* name)
{
    if (!guardMallocLoaded()) {
        std::fprintf(stderr, "Guard Malloc is not loaded in the child\n");
        return guardMallocMissingExitCode;
    }
    installFaultReport();
    return runRow(suite, name);
}

[[nodiscard]] inline std::string executablePath()
{
    std::vector<char> path(4096);
    auto size = static_cast<std::uint32_t>(path.size());
    uml::testing::require(_NSGetExecutablePath(path.data(), &size) == 0,
        "the test's own path is too long");
    return path.data();
}

// Starts this binary as the child for one row, under Guard Malloc, and returns
// its wait status.
[[nodiscard]] inline int runChildUnderGuardMalloc(const char* name, bool fillsMemory)
{
    std::vector<std::string> variables;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        const std::string text = *entry;
        if (text.starts_with("DYLD_INSERT_LIBRARIES="))
            continue;
        variables.push_back(text);
    }
    variables.push_back(std::string("DYLD_INSERT_LIBRARIES=") + guardMalloc);
    if (fillsMemory)
        variables.push_back("MALLOC_FILL_SPACE=1");
    std::vector<char*> environment;
    for (std::string& text : variables)
        environment.push_back(text.data());
    environment.push_back(nullptr);

    const std::string path = executablePath();
    std::array<char*, 4> arguments {
        const_cast<char*>(path.c_str()),
        const_cast<char*>(childArgument),
        const_cast<char*>(name),
        nullptr,
    };
    pid_t child = 0;
    const int spawned = posix_spawn(&child, path.c_str(), nullptr, nullptr,
        arguments.data(), environment.data());
    uml::testing::require(spawned == 0, "the child could not be started");

    const auto deadline = std::chrono::steady_clock::now() + childTimeBudget;
    int status = 0;
    for (;;) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            return status;
        uml::testing::require(waited == 0, "waiting for the child failed");
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(child, SIGKILL);
            (void)waitpid(child, &status, 0);
            uml::testing::fail("the child did not finish in time");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// What the leak checker said about a row, run again under it.
struct LeakReport {
    int status = 0;          // the checker's wait status
    std::string text;        // its output, and the row's
};

// Runs a row under `leaks --atExit`, which starts this binary as the row's
// process and examines it as it exits.
[[nodiscard]] inline LeakReport runUnderLeakChecker(const char* name)
{
    int pipes[2] {};
    uml::testing::require(pipe(pipes) == 0, "the pipe for the leak checker could not be made");
    posix_spawn_file_actions_t actions;
    uml::testing::require(posix_spawn_file_actions_init(&actions) == 0, "spawn file actions");
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);

    const std::string path = executablePath();
    std::array<char*, 7> arguments {
        const_cast<char*>(leaksTool),
        const_cast<char*>("--atExit"),
        const_cast<char*>("--"),
        const_cast<char*>(path.c_str()),
        const_cast<char*>(leaksArgument),
        const_cast<char*>(name),
        nullptr,
    };
    pid_t child = 0;
    const int spawned = posix_spawn(&child, leaksTool, &actions, nullptr, arguments.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    uml::testing::require(spawned == 0, "the leak checker could not be started");

    LeakReport report;
    const auto deadline = std::chrono::steady_clock::now() + childTimeBudget;
    bool finished = false;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd descriptor {pipes[0], POLLIN, 0};
        const int ready = poll(&descriptor, 1, 1000);
        if (ready > 0) {
            char chunk[4096];
            const ssize_t count = read(pipes[0], chunk, sizeof(chunk));
            if (count <= 0) {
                finished = true;
                break;
            }
            report.text.append(chunk, static_cast<std::size_t>(count));
        }
    }
    close(pipes[0]);
    if (!finished)
        ::kill(child, SIGKILL);
    (void)waitpid(child, &report.status, 0);
    uml::testing::require(finished, "the leak checker did not finish in time");
    return report;
}

// What a leak that a renderer function allocated says, or nothing: the
// allocation site is a vkr_ function or object_array_init. The test ends when
// the checker did not examine the process at all.
[[nodiscard]] inline std::string rendererLeak(const char* name)
{
    const LeakReport report = runUnderLeakChecker(name);
    uml::testing::require(report.text.find("nodes malloced") != std::string::npos,
        "the leak checker did not examine the row's process");
    std::string leaked;
    std::size_t begin = 0;
    while (begin < report.text.size()) {
        std::size_t end = report.text.find('\n', begin);
        if (end == std::string::npos)
            end = report.text.size();
        const std::string line = report.text.substr(begin, end - begin);
        begin = end + 1;
        const bool leak = line.find("ROOT LEAK") != std::string::npos
            || line.find("ROOT CYCLE") != std::string::npos;
        if (leak && (line.find("vkr_") != std::string::npos
                || line.find("object_array") != std::string::npos))
            leaked += line + "\n";
    }
    return leaked;
}

// With no argument every row runs, each in a child of its own under Guard
// Malloc, and the rows that name a leak again under the leak checker; with
// `--only NAME`, that row alone the same way; with `--child NAME`, that row
// alone, in this process.
inline int main(int argc, char** argv, const Suite& suite)
{
    const char* only = argc > 2 && std::strcmp(argv[1], "--only") == 0 ? argv[2] : nullptr;
    if (argc > 2 && std::strcmp(argv[1], childArgument) == 0)
        return runChild(suite, argv[2]);
    if (argc > 2 && std::strcmp(argv[1], leaksArgument) == 0)
        return runRow(suite, argv[2]);

#ifdef UML_TEST_ADDRESS_SANITIZER
    std::fprintf(stderr,
        "Guard Malloc cannot share a process with AddressSanitizer; skipping\n");
    return skippedExitCode;
#else
    if (::access(guardMalloc, R_OK) != 0) {
        std::fprintf(stderr, "Guard Malloc is not installed; skipping\n");
        return skippedExitCode;
    }

    std::uint32_t failures = 0;
    for (const Row& row : suite.rows) {
        if (only != nullptr && std::strcmp(only, row.name) != 0)
            continue;
        const int status = runChildUnderGuardMalloc(row.name, row.fillsMemory);
        if (WIFSIGNALED(status)) {
            std::fprintf(stderr, "row %s: FAILED, the child ended with signal %d\n",
                row.name, WTERMSIG(status));
            ++failures;
            continue;
        }
        uml::testing::require(WIFEXITED(status), "the child neither exited nor was signalled");
        const int code = WEXITSTATUS(status);
        if (code == skippedExitCode)
            return code;
        uml::testing::require(code != guardMallocMissingExitCode,
            "the child ran without Guard Malloc, so it cannot show the fault");
        if (code != 0) {
            std::fprintf(stderr, "row %s: FAILED, status %d\n", row.name, code);
            ++failures;
            continue;
        }
        if (row.leakChecked) {
            const std::string leaked = rendererLeak(row.name);
            if (!leaked.empty()) {
                std::fprintf(stderr, "%srow %s: FAILED, the renderer leaked an object the row made\n",
                    leaked.c_str(), row.name);
                ++failures;
                continue;
            }
        }
        std::fprintf(stderr, "row %s: ok\n", row.name);
    }
    uml::testing::require(failures == 0,
        std::to_string(failures) + " " + suite.noun + " rows failed");
    return 0;
#endif
}

} // namespace row_runner

} // namespace uml::gpu::testing::venus

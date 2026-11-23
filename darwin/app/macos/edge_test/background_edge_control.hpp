// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "camera/background_video.hpp"
#include "gpu/in_process_service.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace uml::app {

/* The application's leaving and returning to the foreground, played to a launcher by a test through a named pipe (the
 * variable `UML_BACKGROUND_EDGE_CONTROL` names it, a line each: `leave`, `return`). Only the test-only launcher
 * (`uml-darwin-android17-edge-test`, `UML_APP_EDGE_TEST`) compiles it; no product launcher has it. The launcher has no
 * application to leave: `leave` does what the Kernarium runtime does when the application holds the guest's GPU requests
 * as it enters the background (the camera's running video ends and the guest has stopped it before the hold,
 * `holdAfterVideoEnds` over the real request gate), and `return` releases the requests. Only a run with the synthetic
 * camera reads it, and nothing else does. Every line the edge says carries the host's clock in seconds since the epoch,
 * for the test to set against the guest's. */
class BackgroundEdgeControl final {
public:
    static constexpr const char* variable = "UML_BACKGROUND_EDGE_CONTROL";

    BackgroundEdgeControl(const std::string& path, std::shared_ptr<uml::camera::BackgroundVideo> video,
                          uml::gpu::InProcessGpuService& gpu)
        : video_(std::move(video)), gpu_(gpu)
    {
        if (::mkfifo(path.c_str(), 0600) != 0 && errno != EEXIST) {
            std::fprintf(stderr, "edge control: cannot make the pipe: %s\n", std::strerror(errno));
            return;
        }
        descriptor_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (descriptor_ < 0) {
            std::fprintf(stderr, "edge control: cannot open the pipe: %s\n", std::strerror(errno));
            return;
        }
        thread_ = std::thread([this] { run(); });
    }
    ~BackgroundEdgeControl()
    {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (descriptor_ >= 0) ::close(descriptor_);
    }
    BackgroundEdgeControl(const BackgroundEdgeControl&) = delete;
    BackgroundEdgeControl& operator=(const BackgroundEdgeControl&) = delete;

private:
    /* The longest the hold waits for a request that is running, as the application's does. */
    static constexpr std::chrono::milliseconds holdBudget {1000};

    static double clock()
    {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    /* Says when the gate closes: `holdRequests` closes it at once and then waits for a request that is running, so the time
     * the call returns can be a second later than the hold. The line comes before the one the leaving ends with. The time
     * is when this thread saw the gate held, an observation: it polls every 100 microseconds, and the line is never earlier
     * than the closing, so a closure it saw late can only make a stop that came before it look earlier than it was. */
    class ClosureWatch final {
    public:
        explicit ClosureWatch(uml::gpu::InProcessGpuService& gpu) : gpu_(gpu), thread_([this] { watch(); }) {}
        ~ClosureWatch()
        {
            done_ = true;
            thread_.join();
        }
        ClosureWatch(const ClosureWatch&) = delete;
        ClosureWatch& operator=(const ClosureWatch&) = delete;

    private:
        void watch()
        {
            for (;;) {
                const bool finished = done_.load();
                if (gpu_.requestsHeld()) {
                    std::fprintf(stderr, "edge control: %.3f the GPU gate closed\n", clock());
                    std::fflush(stderr);
                    return;
                }
                if (finished) return;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }

        uml::gpu::InProcessGpuService& gpu_;
        std::atomic<bool> done_ {};
        std::thread thread_;
    };

    void leave()
    {
        std::fprintf(stderr, "edge control: %.3f the application leaves the foreground\n", clock());
        const auto began = uml::camera::BackgroundVideo::Clock::now();
        const auto result = [&] {
            ClosureWatch closing(gpu_);
            return uml::camera::holdAfterVideoEnds(
                *video_, [this] { return gpu_.holdRequests(holdBudget) ? 1 : 0; }, uml::camera::backgroundVideoBound,
                [](const std::string& line) { std::fprintf(stderr, "edge control: %s; the GPU is held as it is\n", line.c_str()); });
        }();
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(uml::camera::BackgroundVideo::Clock::now() - began);
        std::fprintf(stderr, "edge control: %.3f GPU requests drained (%d) after %lld ms: video streams %u, stopped %s, answered %s\n",
                     clock(), result.held, static_cast<long long>(waited.count()), result.video.streams,
                     result.video.stopped ? "yes" : "no", result.video.answered ? "yes" : "no");
        std::fflush(stderr);
    }

    void giveBack()
    {
        gpu_.releaseRequests();
        std::fprintf(stderr, "edge control: %.3f GPU requests released\n", clock());
        std::fflush(stderr);
    }

    void run()
    {
        std::string pending;
        char chunk[256];
        while (!stop_) {
            pollfd waiting {descriptor_, POLLIN, 0};
            if (::poll(&waiting, 1, 50) <= 0 || (waiting.revents & POLLIN) == 0) continue;
            const auto count = ::read(descriptor_, chunk, sizeof(chunk));
            if (count <= 0) continue;
            pending.append(chunk, std::size_t(count));
            for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
                const std::string line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (line == "leave") leave();
                else if (line == "return") giveBack();
                else std::fprintf(stderr, "edge control: unknown line\n");
            }
            if (pending.size() > 4096) pending.clear();
        }
    }

    std::shared_ptr<uml::camera::BackgroundVideo> video_;
    uml::gpu::InProcessGpuService& gpu_;
    int descriptor_ {-1};
    std::atomic<bool> stop_ {};
    std::thread thread_;
};

} // namespace uml::app

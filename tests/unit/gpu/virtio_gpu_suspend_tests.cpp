// SPDX-License-Identifier: GPL-2.0-or-later
// A guest's suspend with a fenced command still on the host. The guest
// suspends by disabling its two rings and resumes by enabling them; the
// production Virtio-GPU device behind the production transport, on a renderer
// whose fences the test retires, must keep its contexts through the suspend:
// the fenced command answers once, at the ring's enable or when its fence
// retires after it, and the command queued behind it runs against the context
// it was sent for. A guest that does not come back resets the device, and
// nothing is written to a ring it never enabled again.
#include "gpu_test_support.hpp"
#include "gpu_vhost_user_server_testing.hpp"
#include "virtio_gpu_device_testing.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uml::gpu::testing {
namespace {

using protocol::GpuCommand;
using protocol::GpuResponse;

constexpr std::uint32_t controlQueue = 0;
constexpr std::uint32_t cursorQueue = 1;
constexpr std::uint32_t contextId = 3;
constexpr std::uint64_t fenceId = 7;

/* A display that shows nothing: the device only reads its size. */
class PlainDisplay final : public Display {
public:
    PlainDisplay() = default;
    [[nodiscard]] std::uint32_t width() const noexcept override { return 64; }
    [[nodiscard]] std::uint32_t height() const noexcept override { return 64; }
    void clear() noexcept override {}
    void update(
        std::span<const std::uint8_t>,
        std::uint32_t,
        std::uint32_t,
        std::size_t,
        protocol::GpuFormat,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t) override
    {
    }
    void fillSelfTestPattern() override {}
};

/* A renderer that accepts every command and keeps each fence busy until the
 * test retires them all. It counts the contexts the device destroys and the
 * times it resets the renderer's fence state, which a reset of the device
 * does and a suspend does not. */
class FenceHoldingRenderer final : public VirtioGpuDeviceDependencies {
public:
    /* The fences retire, once, from now on. */
    void retireFences() { retired_.store(true, std::memory_order_release); }

    [[nodiscard]] unsigned contextsDestroyed() const
    {
        return destroyed_.load(std::memory_order_acquire);
    }
    [[nodiscard]] unsigned fenceStateResets() const
    {
        return fenceResets_.load(std::memory_order_acquire);
    }
    [[nodiscard]] unsigned submissions() const
    {
        return submissions_.load(std::memory_order_acquire);
    }
    /* How many times the device read a fence as retired. */
    [[nodiscard]] unsigned retirementsRead() const
    {
        return retirementsRead_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint32_t width() const noexcept override { return 64; }
    [[nodiscard]] std::uint32_t height() const noexcept override { return 64; }
    [[nodiscard]] bool guestScanoutChanged(std::uint32_t, std::uint32_t) noexcept override
    {
        return true;
    }
    void resetGuestSession() noexcept override {}
    void clear() noexcept override {}
    void update(
        std::span<const std::uint8_t>,
        std::uint32_t,
        std::uint32_t,
        std::size_t,
        protocol::GpuFormat,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t) override
    {
    }
    [[nodiscard]] VirglRenderer::Capset capset(std::uint32_t) const override
    {
        return {.maximumVersion = 1U, .data = {0, 0, 0, 0}};
    }
    [[nodiscard]] int createContext(
        std::uint32_t,
        std::uint32_t,
        std::string_view) noexcept override
    {
        return 0;
    }
    void destroyContext(std::uint32_t) noexcept override
    {
        destroyed_.fetch_add(1, std::memory_order_acq_rel);
    }
    [[nodiscard]] int submit(std::uint32_t, std::span<const std::uint8_t>) noexcept override
    {
        submissions_.fetch_add(1, std::memory_order_acq_rel);
        return 0;
    }
    [[nodiscard]] int createResource3d(const VirglRenderer::Resource3d&) noexcept override
    {
        return EINVAL;
    }
    [[nodiscard]] int createResourceBlob(const VirglRenderer::ResourceBlob&) noexcept override
    {
        return EINVAL;
    }
    [[nodiscard]] int exportResourceBlob(std::uint32_t, std::uint32_t&, int&) noexcept override
    {
        return EINVAL;
    }
    [[nodiscard]] int resourceMapInfo(std::uint32_t, std::uint32_t&) noexcept override
    {
        return EINVAL;
    }
    [[nodiscard]] int mapResource(std::uint32_t, void*&, std::uint64_t&) noexcept override
    {
        return EINVAL;
    }
    void unmapResource(std::uint32_t) noexcept override {}
    [[nodiscard]] int createBlobScanout(
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        void*&) noexcept override
    {
        return EINVAL;
    }
    void releaseBlobScanout(void*) noexcept override {}
    [[nodiscard]] int transferResource(
        bool,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        const VirglRenderer::Box&,
        std::uint64_t) noexcept override
    {
        return EINVAL;
    }
    [[nodiscard]] int readResourceRectangle(
        std::uint32_t,
        std::span<std::uint8_t>,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t) override
    {
        return EINVAL;
    }
    [[nodiscard]] int resourceMetalTexture(
        std::uint32_t, VirglRenderer::NativeTexture&) noexcept override
    {
        return EINVAL;
    }
    [[nodiscard]] PresentOutcome presentMetal(const MetalScanout&) noexcept override
    {
        return PresentOutcome::unsupported;
    }
    [[nodiscard]] bool needsFullCpuFallback() const noexcept override { return false; }
    void recordCpuFallbackFrame() noexcept override {}
    [[nodiscard]] int createFenceAndWait(
        std::uint32_t,
        std::uint32_t,
        std::uint64_t,
        const std::atomic_bool*) noexcept override
    {
        return 0;
    }
    [[nodiscard]] int createContextFence(
        std::uint32_t, std::uint32_t, std::uint64_t) noexcept override
    {
        return 0;
    }
    [[nodiscard]] int pollContextFences(std::uint32_t) noexcept override { return 0; }
    [[nodiscard]] int pollRenderer() noexcept override { return 0; }
    [[nodiscard]] bool hasPendingWork() noexcept override { return false; }
    [[nodiscard]] std::optional<std::chrono::steady_clock::duration>
        nextIdleTrimDelay() const noexcept override
    {
        return std::nullopt;
    }
    void trimIdleContexts() noexcept override {}
    [[nodiscard]] int contextFenceStatus(
        std::uint32_t, std::uint32_t, std::uint64_t) noexcept override
    {
        if (!retired_.load(std::memory_order_acquire))
            return EBUSY;
        retirementsRead_.fetch_add(1, std::memory_order_acq_rel);
        return 0;
    }
    void unrefResource(std::uint32_t) noexcept override {}
    [[nodiscard]] int attachBacking(std::uint32_t, std::span<iovec>) noexcept override
    {
        return EINVAL;
    }
    void detachBacking(std::uint32_t) noexcept override {}
    [[nodiscard]] int attachResource(std::uint32_t, std::uint32_t) noexcept override
    {
        return EINVAL;
    }
    [[nodiscard]] int detachResource(std::uint32_t, std::uint32_t) noexcept override
    {
        return EINVAL;
    }
    void resetFenceState() noexcept override
    {
        fenceResets_.fetch_add(1, std::memory_order_acq_rel);
    }

private:
    std::atomic_bool retired_ {};
    std::atomic_uint destroyed_ {};
    std::atomic_uint fenceResets_ {};
    std::atomic_uint submissions_ {};
    std::atomic_uint retirementsRead_ {};
};

/* Where the two queues' rings lie in the ring's one page, and the guest's
 * request and response buffers after them. */
struct Layout {
    std::size_t descriptors;
    std::size_t available;
    std::size_t used;
};
constexpr Layout controlLayout {0x400, 0x500, 0x600};
constexpr Layout cursorLayout {0x000, 0x100, 0x200};
constexpr std::size_t buffersOffset = 0x700;
constexpr std::size_t bufferStride = 0x80;
constexpr std::uint16_t queueDepth = 8;

/* The guest's driver for the two queues of one page: it places and enables
 * them, posts control commands and reads what the device wrote back. */
class GpuGuest {
public:
    GpuGuest(int socket, bool enable) : socket_(socket)
    {
        negotiateProtocolFeatures(
            socket_, protocol::protocolFeatureReplyAck | protocol::protocolFeatureConfig);
        configureMemory(socket_, ring_.descriptor());
        start(controlQueue, controlLayout, enable);
        start(cursorQueue, cursorLayout, enable);
    }

    /* The guest suspends, or resumes: both rings, in order. */
    void enableRings(bool on) const
    {
        enable(controlQueue, on);
        enable(cursorQueue, on);
    }

    /* Queues a control command and returns its number: its head in the
     * descriptor table is twice that. The device reads `request` and writes
     * a header-sized response. */
    std::size_t post(std::span<const std::uint8_t> request)
    {
        const auto number = posted_++;
        require(number < queueDepth / 2U);
        const auto requestOffset = buffersOffset + number * bufferStride;
        const auto responseOffset = requestOffset + bufferStride / 2U;
        std::memcpy(ring_.memory().data() + requestOffset, request.data(), request.size());
        std::memset(ring_.memory().data() + responseOffset, 0xa5, sizeof(protocol::GpuControlHeader));
        const auto head = static_cast<std::uint16_t>(2U * number);
        const auto chain = requestChain(
            requestOffset,
            static_cast<std::uint32_t>(request.size()),
            responseOffset,
            sizeof(protocol::GpuControlHeader),
            head);
        auto* memory = ring_.memory().data();
        std::memcpy(
            memory + controlLayout.descriptors + head * sizeof(protocol::VirtqueueDescriptor),
            chain.data(),
            chain.size() * sizeof(chain[0]));
        std::memcpy(
            memory + controlLayout.available + protocol::splitRingHeaderSize +
                (number % queueDepth) * sizeof(std::uint16_t),
            &head,
            sizeof(head));
        std::atomic_ref<std::uint16_t>(
            *reinterpret_cast<std::uint16_t*>(
                memory + controlLayout.available + sizeof(std::uint16_t)))
            .store(static_cast<std::uint16_t>(posted_), std::memory_order_release);
        return number;
    }
    void kickControl() const { writeKick(control_.kick); }

    [[nodiscard]] std::uint16_t controlUsed() const
    {
        return std::atomic_ref<std::uint16_t>(
                   *reinterpret_cast<std::uint16_t*>(
                       ring_.memory().data() + controlLayout.used + sizeof(std::uint16_t)))
            .load(std::memory_order_acquire);
    }
    /* The head the device returned in the used ring's entry `entry`. */
    [[nodiscard]] std::uint32_t controlUsedHead(std::size_t entry) const
    {
        protocol::VirtqueueUsedElement element {};
        std::memcpy(
            &element,
            ring_.memory().data() + controlLayout.used + protocol::splitRingHeaderSize +
                entry * sizeof(element),
            sizeof(element));
        return element.id;
    }
    /* The response header the device wrote for command `number`. */
    [[nodiscard]] protocol::GpuControlHeader responseTo(std::size_t number) const
    {
        protocol::GpuControlHeader header {};
        std::memcpy(
            &header,
            ring_.memory().data() + buffersOffset + number * bufferStride + bufferStride / 2U,
            sizeof(header));
        return header;
    }
    [[nodiscard]] GpuResponse responseType(std::size_t number) const
    {
        return static_cast<GpuResponse>(responseTo(number).type);
    }
    /* Whether the guest has been called since it last was. */
    [[nodiscard]] bool called(int timeoutMilliseconds = 100) const
    {
        pollfd call {.fd = control_.call.readEnd.get(), .events = POLLIN};
        if (::poll(&call, 1, timeoutMilliseconds) != 1)
            return false;
        std::uint64_t count {};
        readExact(control_.call.readEnd.get(), &count, sizeof(count));
        return true;
    }
    /* The guest has seen everything it was called for so far. */
    void takeCalls() const
    {
        while (called(0)) {}
    }

private:
    struct Channels {
        Pipe call;
        Pipe kick;
    };

    void start(std::uint32_t index, Layout layout, bool enableRing)
    {
        const protocol::VringState size {.index = index, .value = queueDepth};
        sendRequestAndReadAcknowledgement(
            socket_, protocol::VhostRequest::setVringNum, bytesOf(size));
        const protocol::VringAddress address {
            .index = index,
            .flags = 0,
            .descriptor = SplitRing::userBase + layout.descriptors,
            .used = SplitRing::userBase + layout.used,
            .available = SplitRing::userBase + layout.available,
            .log = 0,
        };
        sendRequestAndReadAcknowledgement(
            socket_, protocol::VhostRequest::setVringAddr, bytesOf(address));
        if (enableRing)
            enable(index, true);
        auto& channels = index == controlQueue ? control_ : cursor_;
        channels.call = makePipe();
        channels.kick = makePipe();
        const std::uint64_t queue = index;
        const std::array callDescriptors {channels.call.writeEnd.get()};
        sendRequestAndReadAcknowledgement(
            socket_, protocol::VhostRequest::setVringCall, bytesOf(queue), callDescriptors);
        const std::array kickDescriptors {channels.kick.readEnd.get()};
        sendRequestAndReadAcknowledgement(
            socket_, protocol::VhostRequest::setVringKick, bytesOf(queue), kickDescriptors);
    }
    void enable(std::uint32_t index, bool on) const
    {
        const protocol::VringState state {.index = index, .value = on ? 1U : 0U};
        sendRequestAndReadAcknowledgement(
            socket_, protocol::VhostRequest::setVringEnable, bytesOf(state));
    }

    int socket_;
    SplitRing ring_;
    Channels control_;
    Channels cursor_;
    std::size_t posted_ {};
};

[[nodiscard]] std::vector<std::uint8_t> contextCreate()
{
    protocol::GpuContextCreate create {};
    create.header.type = static_cast<std::uint32_t>(GpuCommand::contextCreate);
    create.header.contextId = contextId;
    create.nameLength = 1;
    create.contextInit = protocol::virglCapsetId;
    create.debugName[0] = 'g';
    const auto bytes = bytesOf(create);
    return {bytes.begin(), bytes.end()};
}

/* A submission of one command word, with the fence the device defers. */
[[nodiscard]] std::vector<std::uint8_t> fencedSubmit()
{
    protocol::GpuSubmit3d submit {};
    submit.header.type = static_cast<std::uint32_t>(GpuCommand::submit3d);
    submit.header.flags = protocol::gpuFlagFence;
    submit.header.fenceId = fenceId;
    submit.header.contextId = contextId;
    submit.size = sizeof(std::uint32_t);
    std::vector<std::uint8_t> bytes(sizeof(submit) + submit.size);
    std::memcpy(bytes.data(), &submit, sizeof(submit));
    return bytes;
}

/* The command that waits behind the fence and answers whether the context is
 * still there. */
[[nodiscard]] std::vector<std::uint8_t> contextDestroy()
{
    protocol::GpuControlHeader header {};
    header.type = static_cast<std::uint32_t>(GpuCommand::contextDestroy);
    header.contextId = contextId;
    const auto bytes = bytesOf(header);
    return {bytes.begin(), bytes.end()};
}

/* Returns once the server has handled every message sent before: its answer
 * to a configuration read comes after. Unlike GET_FEATURES, which a driver
 * sends to end, it asks nothing of the device. */
void settle(int socket)
{
    (void)readConfig(socket, 0, sizeof(protocol::GpuConfig));
}

/* The service's state once the guest's context has a fenced command on the
 * host and another behind it, in the ring: the fence is busy. */
struct Session {
    FenceHoldingRenderer& renderer;
    GpuGuest& guest;
    int socket;
    std::size_t submit;
    std::size_t destroy;
};

/* Runs `body` on a connection whose guest created a context, queued a fenced
 * submission and a context destroy behind it, and has them held by the
 * device. `afterEnd` runs once the connection has ended. */
void withHeldFence(
    const std::function<void(const Session&)>& body,
    const std::function<void(const Session&)>& afterEnd = {})
{
    FenceHoldingRenderer renderer;
    auto server = makeGpuVhostUserServerOnDependencies(
        std::make_shared<PlainDisplay>(), renderer);
    std::optional<GpuGuest> guest;
    std::size_t submit = 0;
    std::size_t destroy = 0;

    withServerConnection(*server, [&](int socket) {
        guest.emplace(socket, true);
        const auto create = guest->post(contextCreate());
        guest->kickControl();
        awaitCondition([&] { return guest->controlUsed() == 1; });
        require(guest->responseType(create) == GpuResponse::okNoData);

        submit = guest->post(fencedSubmit());
        destroy = guest->post(contextDestroy());
        guest->kickControl();
        awaitCondition([&] { return renderer.submissions() == 1; });
        /* The server has taken the submission once a message sent after it
         * is answered. */
        settle(socket);
        require(guest->controlUsed() == 1, "the fenced command answered before its fence retired");
        guest->takeCalls();
        body({renderer, *guest, socket, submit, destroy});
    });
    if (afterEnd)
        afterEnd({renderer, *guest, -1, submit, destroy});
}

void requireFenceAnsweredOnce(const Session& session)
{
    const auto& guest = session.guest;
    require(guest.controlUsed() == 3, "the ring did not carry the fence's answer and the destroy's");
    require(guest.controlUsedHead(1) == 2U * session.submit &&
                guest.controlUsedHead(2) == 2U * session.destroy,
            "the answers came in another order");
    const auto fence = guest.responseTo(session.submit);
    require(fence.type == static_cast<std::uint32_t>(GpuResponse::okNoData) &&
                (fence.flags & protocol::gpuFlagFence) != 0 && fence.fenceId == fenceId,
            "the fenced command's answer is not its fence's");
    require(guest.responseType(session.destroy) == GpuResponse::okNoData,
            "the command behind the fence did not find its context");
}

/* The guest suspends and wakes: the fence retires while the rings are
 * disabled, or after they are enabled again. Either way the device keeps its
 * context, the fence's answer reaches the guest once, at the enable or at the
 * retirement, and the command behind it runs and finds its context. */
void aSuspendKeepsTheContextAndAnswersTheFence(bool retiresWhileDisabled)
{
    withHeldFence([&](const Session& session) {
        auto& renderer = session.renderer;
        auto& guest = session.guest;
        const int socket = session.socket;
        const auto resets = renderer.fenceStateResets();
        const auto reads = renderer.retirementsRead();

        guest.enableRings(false);
        require(renderer.contextsDestroyed() == 0 && renderer.fenceStateResets() == resets,
                "the ring disable reset the device");
        if (retiresWhileDisabled) {
            renderer.retireFences();
            awaitCondition([&] { return renderer.retirementsRead() > reads; });
            /* The server has handled the retirement once a message sent
             * after it is answered. */
            settle(socket);
            require(guest.controlUsed() == 1, "a used ring was written while its ring was disabled");
            require(!guest.called(), "the guest was called while its rings were disabled");
        }

        guest.enableRings(true);
        if (retiresWhileDisabled) {
            require(guest.controlUsed() == 3, "the held answer and the queued command did not follow the enable");
        } else {
            require(guest.controlUsed() == 1, "the command behind a busy fence ran");
            renderer.retireFences();
            awaitCondition([&] { return guest.controlUsed() == 3; });
        }
        requireFenceAnsweredOnce(session);
        require(renderer.contextsDestroyed() == 1 && renderer.fenceStateResets() == resets,
                "the suspend reset the device");
        /* No further answer comes for the fence. */
        settle(socket);
        require(guest.controlUsed() == 3);
    });
}

/* A guest that never enables its rings again resets the device once its
 * driver ends, with the fence retired or not, and nothing is written to the
 * rings it left disabled. */
void aTeardownResetsTheDeviceAndWritesNothing(bool retiresBeforeTheEnd)
{
    withHeldFence([&](const Session& session) {
        auto& renderer = session.renderer;
        auto& guest = session.guest;
        const int socket = session.socket;
        const auto resets = renderer.fenceStateResets();
        const auto reads = renderer.retirementsRead();

        guest.enableRings(false);
        if (retiresBeforeTheEnd) {
            renderer.retireFences();
            awaitCondition([&] { return renderer.retirementsRead() > reads; });
        }
        (void)getFeatures(socket);
        require(renderer.contextsDestroyed() == 1 && renderer.fenceStateResets() == resets + 1,
                "the features request after the disables did not reset the device");
        renderer.retireFences();
        settle(socket);
        require(guest.controlUsed() == 1, "an answer was written to a ring the guest retired");
        require(!guest.called(), "the guest was called after its driver ended");
    });
}

/* A guest that is gone for good, whose connection ends with its rings
 * disabled, leaves the device reset. */
void theEndOfTheConnectionResetsTheDevice()
{
    withHeldFence(
        [&](const Session& session) {
            session.guest.enableRings(false);
            require(session.renderer.contextsDestroyed() == 0);
        },
        [&](const Session& session) {
            require(session.renderer.contextsDestroyed() == 1 &&
                        session.renderer.fenceStateResets() >= 1,
                    "the end of the connection did not reset the device");
            require(session.guest.controlUsed() == 1,
                    "an answer was written to a ring the guest never enabled again");
        });
}

} // namespace
} // namespace uml::gpu::testing

int main(int argc, char** argv)
{
    using namespace uml::gpu::testing;
    struct Case {
        std::string name;
        std::function<void()> run;
    };
    const std::vector<Case> cases {
        {"retires-while-disabled", [] { aSuspendKeepsTheContextAndAnswersTheFence(true); }},
        {"retires-after-enable", [] { aSuspendKeepsTheContextAndAnswersTheFence(false); }},
        {"teardown-fence-busy", [] { aTeardownResetsTheDeviceAndWritesNothing(false); }},
        {"teardown-fence-retired", [] { aTeardownResetsTheDeviceAndWritesNothing(true); }},
        {"connection-end", theEndOfTheConnectionResetsTheDevice},
    };
    try {
        for (const auto& test : cases) {
            if (argc > 1 && std::string {argv[1]} != test.name)
                continue;
            test.run();
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("gpu suspend held fence ok");
    return 0;
}

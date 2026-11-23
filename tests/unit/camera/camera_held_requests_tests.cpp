// SPDX-License-Identifier: GPL-2.0-or-later
// The camera device behind the real vhost-user transport, across a guest's
// suspend. The guest disables its request ring while the device holds queued
// buffers; the device finishes them meanwhile, as a stream's own timer does.
// Every one of them must reach the guest, in the order it was finished, when
// the ring is enabled again: none is dropped, none is written while the ring
// is off, and the suspend does not reset the device (a reset drops every
// request it holds and every stream, which the guest's driver then has to
// make again).
//
// The session is blocked, so the frames come from the device's own timer and
// no platform camera is opened. The front end is the GPU tests', so their
// support header is on the path.
#include "camera_device.hpp"
#include "gpu_test_support.hpp"
#include "require.hpp"
#include "synthetic_camera.hpp"
#include "wire_bytes.hpp"

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace uml::gpu::testing;
using uml::gpu::VhostUserServer;
using uml::testing::append;
using uml::testing::load;
using uml::testing::require;
namespace protocol = uml::gpu::protocol;
using namespace uml::camera;
using namespace uml::platform;

constexpr std::uint32_t requestQueue = 0;
constexpr std::uint16_t depth = 32;
constexpr std::uint64_t negotiated = protocol::protocolFeatureReplyAck |
    protocol::protocolFeatureConfig;
constexpr std::size_t memorySize = 4U << 20;
constexpr std::size_t availableOffset = 0x200;
constexpr std::size_t usedOffset = 0x400;
/* Each request and its answer have a slot of their own in the guest's memory. */
constexpr std::size_t slotOffset = 0x1000;
constexpr std::size_t slotStride = 0x400;
constexpr std::size_t responseOffset = 0x200;
constexpr std::size_t responseCapacity = 512;
constexpr std::size_t frameOffset = 0x10000;
constexpr std::uint32_t width = 640;
constexpr std::uint32_t height = 480;
constexpr std::uint32_t frameSize = width * height * 3 / 2;
constexpr std::uint32_t stream = 1;
constexpr auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;

/* The guest's one queue: rings and request slots in a region of memory the
 * server maps, fed and read the way a front end does. */
class Guest {
public:
    Guest() : backing_(memoryBackingName, memorySize)
    {
        void* mapped = ::mmap(nullptr, memorySize, PROT_READ | PROT_WRITE, MAP_SHARED,
                              backing_.descriptor(), 0);
        require(mapped != MAP_FAILED, "mmap");
        memory_ = {static_cast<std::uint8_t*>(mapped), memorySize};
    }
    ~Guest() { require(::munmap(memory_.data(), memorySize) == 0); }
    Guest(const Guest&) = delete;
    Guest& operator=(const Guest&) = delete;

    void start(int socket)
    {
        negotiateProtocolFeatures(socket, negotiated);
        configureMemory(socket, backing_.descriptor(), memorySize);
        socket_ = socket;
        const protocol::VringState size {.index = requestQueue, .value = depth};
        sendRequestAndReadAcknowledgement(socket, protocol::VhostRequest::setVringNum, bytesOf(size));
        const protocol::VringAddress address {
            .index = requestQueue,
            .flags = 0,
            .descriptor = SplitRing::userBase,
            .used = SplitRing::userBase + usedOffset,
            .available = SplitRing::userBase + availableOffset,
            .log = 0,
        };
        sendRequestAndReadAcknowledgement(socket, protocol::VhostRequest::setVringAddr, bytesOf(address));
        enable(true);
        call_ = makePipe();
        kick_ = makePipe();
        const std::uint64_t queue = requestQueue;
        const std::array callDescriptors {call_.writeEnd.get()};
        sendRequestAndReadAcknowledgement(socket, protocol::VhostRequest::setVringCall, bytesOf(queue),
                                          callDescriptors);
        const std::array kickDescriptors {kick_.readEnd.get()};
        sendRequestAndReadAcknowledgement(socket, protocol::VhostRequest::setVringKick, bytesOf(queue),
                                          kickDescriptors);
    }

    void enable(bool on) const
    {
        const protocol::VringState state {.index = requestQueue, .value = on ? 1U : 0U};
        sendRequestAndReadAcknowledgement(socket_, protocol::VhostRequest::setVringEnable, bytesOf(state));
    }

    /* Queues a request and kicks; the answer is read with answer(). */
    std::size_t post(const std::vector<std::uint8_t>& request)
    {
        require(request.size() <= responseOffset, "a request is longer than its slot");
        const std::size_t slot = posted_;
        require(slot < depth / 2, "the ring holds sixteen requests");
        const auto requestAt = slotOffset + slot * slotStride;
        std::memcpy(memory_.data() + requestAt, request.data(), request.size());
        std::memset(memory_.data() + requestAt + responseOffset, 0, responseCapacity);
        const auto head = static_cast<std::uint16_t>(2U * slot);
        const std::array<protocol::VirtqueueDescriptor, 2> chain {{
            {.address = SplitRing::guestBase + requestAt,
             .length = static_cast<std::uint32_t>(request.size()),
             .flags = protocol::descriptorNext,
             .next = static_cast<std::uint16_t>(head + 1U)},
            {.address = SplitRing::guestBase + requestAt + responseOffset,
             .length = static_cast<std::uint32_t>(responseCapacity),
             .flags = protocol::descriptorWrite,
             .next = 0}}};
        std::memcpy(memory_.data() + head * sizeof(protocol::VirtqueueDescriptor), chain.data(),
                    chain.size() * sizeof(chain[0]));
        std::memcpy(memory_.data() + availableOffset + protocol::splitRingHeaderSize +
                        (posted_ % depth) * sizeof(std::uint16_t),
                    &head, sizeof(head));
        ++posted_;
        std::atomic_ref<std::uint16_t>(
            *reinterpret_cast<std::uint16_t*>(memory_.data() + availableOffset + sizeof(std::uint16_t)))
            .store(static_cast<std::uint16_t>(posted_), std::memory_order_release);
        writeKick(kick_);
        return slot;
    }

    [[nodiscard]] std::uint16_t usedIndex() const
    {
        return std::atomic_ref<std::uint16_t>(
                   *reinterpret_cast<std::uint16_t*>(memory_.data() + usedOffset + sizeof(std::uint16_t)))
            .load(std::memory_order_acquire);
    }
    [[nodiscard]] protocol::VirtqueueUsedElement used(std::size_t index) const
    {
        protocol::VirtqueueUsedElement element {};
        std::memcpy(&element, memory_.data() + usedOffset + protocol::splitRingHeaderSize + index * sizeof(element),
                    sizeof(element));
        return element;
    }
    /* The answer the device wrote for the request in `slot`. */
    [[nodiscard]] std::vector<std::uint8_t> response(std::size_t slot) const
    {
        const auto* bytes = memory_.data() + slotOffset + slot * slotStride + responseOffset;
        return {bytes, bytes + responseCapacity};
    }
    /* A request that completes at once is answered before the next is posted. */
    std::vector<std::uint8_t> exchange(const std::vector<std::uint8_t>& request)
    {
        const auto before = usedIndex();
        const auto slot = post(request);
        awaitCondition([&] { return usedIndex() == before + 1; });
        return response(slot);
    }
    [[nodiscard]] std::span<const std::uint8_t> frame(std::size_t index) const
    {
        return std::span<const std::uint8_t>(memory_).subspan(frameOffset + index * frameSize, frameSize);
    }
    void awaitCall() const { uml::gpu::testing::awaitCall(call_.readEnd.get()); }

private:
    ScratchBacking backing_;
    std::span<std::uint8_t> memory_;
    int socket_ {-1};
    Pipe call_;
    Pipe kick_;
    std::size_t posted_ {};
};

[[nodiscard]] std::vector<std::uint8_t> createStream()
{
    virtio_video_camera_stream_create command {};
    command.hdr = {VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE, stream};
    command.camera_index = 0;
    command.kind = VIRTIO_VIDEO_CAMERA_STREAM_VIDEO;
    std::vector<std::uint8_t> request;
    append(request, command);
    return request;
}

[[nodiscard]] std::vector<std::uint8_t> setVideoParams()
{
    virtio_video_set_params set {};
    set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, stream};
    set.params.queue_type = output;
    set.params.format = VIRTIO_VIDEO_FORMAT_NV12;
    set.params.frame_width = width;
    set.params.frame_height = height;
    std::vector<std::uint8_t> request;
    append(request, set);
    return request;
}

/* A resource of one frame's size, in the guest memory after the rings. */
[[nodiscard]] std::vector<std::uint8_t> attach(std::uint32_t resource)
{
    std::vector<std::uint8_t> request;
    append(request, virtio_video_resource_attach {VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, stream, output, resource});
    append(request, std::uint32_t(1));
    append(request, std::uint32_t(0));
    append(request, virtio_video_resource_sg_entry {SplitRing::guestBase + frameOffset + (resource - 1) * frameSize,
                                                    frameSize, 0});
    return request;
}

[[nodiscard]] std::vector<std::uint8_t> queueBuffer(std::uint32_t resource)
{
    virtio_video_resource_queue command {};
    command.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
    command.stream_id = stream;
    command.queue_type = output;
    command.resource_id = resource;
    std::vector<std::uint8_t> request;
    append(request, command);
    return request;
}

[[nodiscard]] std::uint32_t responseType(const std::vector<std::uint8_t>& response)
{
    return load<virtio_video_cmd_hdr>(response).type;
}

[[nodiscard]] std::unique_ptr<VirtioCameraDevice> makeDevice()
{
    SyntheticCameraOptions synthetic;
    synthetic.manualClock = true;
    CameraDeviceOptions options;
    options.mode = CameraMode::blocked;
    options.calibration = std::make_shared<CalibrationStore>();
    return std::make_unique<VirtioCameraDevice>(makeSyntheticCamera(synthetic), std::move(options));
}

/* A guest with a video stream and `buffers` queued buffers the device holds,
 * suspended: its request ring disabled. */
struct Session {
    Guest& guest;
    int socket;
    /* The slots of the queued buffers, in the order they were queued. */
    std::vector<std::size_t> slots;
};

template<typename Body>
void withSuspendedGuest(std::uint32_t buffers, Body body)
{
    VhostUserServer server {makeDevice()};
    Guest guest;
    withServerConnection(server, [&](int socket) {
        guest.start(socket);
        require(responseType(guest.exchange(createStream())) == VIRTIO_VIDEO_RESP_OK_NODATA, "STREAM_CREATE");
        require(responseType(guest.exchange(setVideoParams())) == VIRTIO_VIDEO_RESP_OK_NODATA, "SET_PARAMS");
        for (std::uint32_t resource = 1; resource <= buffers; ++resource)
            require(responseType(guest.exchange(attach(resource))) == VIRTIO_VIDEO_RESP_OK_NODATA,
                    "RESOURCE_ATTACH");
        /* The device defers a queued buffer: nothing answers it at once, and a
         * frame needs the stream's timer, which has not fired yet. */
        std::vector<std::size_t> slots;
        for (std::uint32_t resource = 1; resource <= buffers; ++resource)
            slots.push_back(guest.post(queueBuffer(resource)));
        guest.enable(false);
        body(Session {guest, socket, std::move(slots)});
    });
}

/* Buffers the device finishes while the ring is off are written at its enable,
 * every one, in the order the device finished them, and the device is not
 * reset by the suspend (a reset drops them). */
void heldBuffersCompleteAtTheEnable(std::uint32_t buffers)
{
    withSuspendedGuest(buffers, [&](const Session& session) {
        auto& guest = session.guest;
        const auto before = guest.usedIndex();
        /* The device's timer keeps firing while the guest is suspended and
         * finishes every buffer it holds, black. */
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        (void)readConfig(session.socket, 0, 1);
        require(guest.usedIndex() == before, "a used ring was written while its ring was disabled");

        guest.enable(true);
        awaitCondition([&] { return guest.usedIndex() == before + buffers; });
        guest.awaitCall();
        for (std::uint32_t index = 0; index < buffers; ++index) {
            const auto element = guest.used(before + index);
            require(element.id == 2U * session.slots[index], "the held buffers did not keep their order");
            const auto answer = guest.response(session.slots[index]);
            require(responseType(answer) == VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE,
                    "a held buffer was answered with something else");
            const auto result = load<virtio_video_camera_queue_resp>(answer);
            require(result.base.flags == (VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLACK |
                                          VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLOCKED),
                    "a blocked frame is black and blocked");
            require(result.base.data_sizes[0] == frameSize, "a whole picture");
            const auto picture = guest.frame(index);
            require(std::all_of(picture.begin(), picture.begin() + width * height,
                                [](auto byte) { return byte == 0; }),
                    "the picture is black");
        }
    });
}

/* After the suspend the stream is still the device's: a buffer queued once the
 * ring is enabled again is answered, which a reset device (no stream) refuses. */
void theStreamSurvivesTheSuspend()
{
    withSuspendedGuest(1, [&](const Session& session) {
        auto& guest = session.guest;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto suspended = guest.usedIndex();
        guest.enable(true);
        awaitCondition([&] { return guest.usedIndex() == suspended + 1; });
        const auto before = guest.usedIndex();
        require(responseType(guest.response(session.slots[0])) == VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE,
                "the held buffer was not answered");
        const auto slot = guest.post(queueBuffer(1));
        awaitCondition([&] { return guest.usedIndex() == before + 1; });
        require(responseType(guest.response(slot)) == VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE,
                "a reset device answered the next buffer as it would a stream it does not have");
    });
}

}  // namespace

int main()
{
    try {
        heldBuffersCompleteAtTheEnable(1);
        heldBuffersCompleteAtTheEnable(3);
        theStreamSurvivesTheSuspend();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "camera-held-requests: %s\n", error.what());
        return 1;
    }
    std::puts("camera-held-requests: the buffers held across a ring disable were completed");
    return 0;
}

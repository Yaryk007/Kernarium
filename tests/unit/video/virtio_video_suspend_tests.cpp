// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu_test_support.hpp"
#include "virtio_video_device.hpp"
#include "video_capabilities.hpp"
#include "wire_bytes.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

/* The transport and the Virtio video decoder together, across the ring
 * disable and enable a guest's suspend sends. The guest posts the event
 * queue's buffer once and expects the device to answer it whenever an event
 * comes. A device that held the buffer, as the video devices used to, left the
 * transport with a request still pending, and the ring disable reset the
 * device: the stream was closed and no event ever reached the guest again. A
 * device that leaves the buffer in the ring is not reset, and the event raised
 * after the resume is written into the buffer the guest posted before the
 * suspend. */

namespace {
using namespace uml::gpu::testing;
using uml::gpu::VhostUserServer;
using uml::virtio::VhostUserDevice;
using uml::video::VirtioVideoDevice;
using uml::testing::append;
using uml::testing::load;
using uml::testing::require;
namespace protocol = uml::gpu::protocol;
using namespace uml::platform;

constexpr std::uint32_t streamId = 7;
constexpr std::size_t eventOffset = 0x300;
constexpr std::size_t packetOffset = 0x700;

/* Decodes nothing: the packet raises the source change, which is all the case
 * needs from a decoder. */
class IdleDecoder final : public VideoDecoder {
public:
    void inspect(std::span<const std::uint8_t>) override { format_ = {64, 64, 8}; }
    void submit(std::span<const std::uint8_t>, std::uint64_t) override { format_ = {64, 64, 8}; }
    void drain() override {}
    void discard() override {}
    std::vector<DecodedVideoFrame> takeFrames() override { return {}; }
    VideoFormat format() const noexcept override { return format_; }
    void requestOutputLayout(VideoPixelLayout) override {}
private:
    VideoFormat format_;
};

/* The video device with a count of the resets it receives and of the runs of
 * each queue. A device's events come from its commands, which the transport
 * runs on its own thread, so the device is given a script of commands to run
 * there when the front end reads its configuration: opening a stream and
 * queueing the first packet raises the source change. */
class RecordingDevice final : public VhostUserDevice {
public:
    RecordingDevice(std::unique_ptr<VirtioVideoDevice> device, std::vector<std::vector<std::uint8_t>> script)
        : device_(std::move(device)), script_(std::move(script)) {}

    [[nodiscard]] const char* name() const noexcept override { return device_->name(); }
    [[nodiscard]] std::uint64_t features() const noexcept override { return device_->features(); }
    [[nodiscard]] std::size_t queueCount() const noexcept override { return device_->queueCount(); }
    [[nodiscard]] std::vector<std::uint8_t> configuration() const override { return device_->configuration(); }
    void writeConfiguration(std::uint32_t offset, std::uint32_t flags, std::span<const std::uint8_t> bytes) override
    {
        device_->writeConfiguration(offset, flags, bytes);
    }
    void setGuestMapper(GuestMapper mapper) override { device_->setGuestMapper(std::move(mapper)); }
    void setSharedMemoryRegions(std::shared_ptr<const uml::virtio::SharedMemoryRegions> regions) override
    {
        device_->setSharedMemoryRegions(std::move(regions));
    }
    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index, std::uint64_t requestId, std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override
    {
        if (index < runs_.size()) runs_[index].fetch_add(1, std::memory_order_release);
        return device_->processQueue(index, requestId, request, responseCapacity);
    }
    [[nodiscard]] bool takeQueueCompletion(uml::virtio::VhostUserQueueCompletion& completion) override
    {
        return device_->takeQueueCompletion(completion);
    }
    void reset() noexcept override
    {
        resets_.fetch_add(1, std::memory_order_release);
        device_->reset();
    }
    [[nodiscard]] int eventDescriptor() const noexcept override { return device_->eventDescriptor(); }
    void drainEventSource() override { device_->drainEventSource(); }
    void configurationRead(std::uint32_t, std::uint32_t) noexcept override
    {
        std::lock_guard lock(mutex_);
        if (std::exchange(scripted_, true)) return;
        try {
            for (const auto& command : script_) {
                const auto result = device_->processQueue(0, ++requestId_, command, 4096);
                if (result.action == uml::virtio::VhostUserQueueAction::complete && !result.response.empty() &&
                    load<virtio_video_cmd_hdr>(result.response).type >= VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION)
                    failure_ = "the device refused a command of the script";
            }
        } catch (const std::exception& error) {
            failure_ = error.what();
        }
    }

    [[nodiscard]] unsigned runs(std::size_t queue) const noexcept { return runs_[queue].load(std::memory_order_acquire); }
    [[nodiscard]] unsigned resets() const noexcept { return resets_.load(std::memory_order_acquire); }
    [[nodiscard]] std::string failure() const
    {
        std::lock_guard lock(mutex_);
        return failure_;
    }

private:
    std::unique_ptr<VirtioVideoDevice> device_;
    std::vector<std::vector<std::uint8_t>> script_;
    std::array<std::atomic<unsigned>, 2> runs_ {};
    std::atomic<unsigned> resets_ {};
    mutable std::mutex mutex_;
    std::uint64_t requestId_ {};
    bool scripted_ {};
    std::string failure_;
};

/* The commands that open a stream and queue one packet, which the device
 * answers with a source change. */
std::vector<std::vector<std::uint8_t>> firstPacket()
{
    virtio_video_stream_create create {};
    create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, streamId};
    create.coded_format = VIRTIO_VIDEO_FORMAT_H264;
    std::vector<std::uint8_t> attach;
    append(attach, virtio_video_resource_attach {VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, streamId, VIRTIO_VIDEO_QUEUE_TYPE_INPUT, 1});
    append(attach, std::uint32_t(1));
    append(attach, std::uint32_t(0));
    append(attach, virtio_video_resource_sg_entry {SplitRing::guestBase + packetOffset, 5, {}});
    virtio_video_resource_queue queue {};
    queue.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
    queue.stream_id = streamId;
    queue.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    queue.resource_id = 1;
    queue.data_sizes[0] = 5;
    queue.timestamp = 1;
    std::vector<std::uint8_t> create_bytes, queue_bytes;
    append(create_bytes, create);
    append(queue_bytes, queue);
    return {std::move(create_bytes), std::move(attach), std::move(queue_bytes)};
}

void eventQueueBufferSurvivesASuspend()
{
    SplitRing ring;
    const std::uint8_t packet[5] = {1, 2, 3, 4, 5};
    std::memcpy(ring.memory().data() + packetOffset, packet, sizeof(packet));
    auto inner = std::make_unique<VirtioVideoDevice>(
        std::vector<VideoDecodeCapability> {{VideoCodec::h264, VideoProfile::h264High, 4096, 2304, 51, 8}},
        [](auto) { return std::make_unique<IdleDecoder>(); });
    auto device = std::make_unique<RecordingDevice>(std::move(inner), firstPacket());
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};

    // The guest posts one buffer of the event queue before anything happens.
    const std::array<protocol::VirtqueueDescriptor, 1> descriptors {{
        {.address = SplitRing::guestBase + eventOffset, .length = sizeof(virtio_video_event),
         .flags = protocol::descriptorWrite, .next = 0}}};
    const std::array<std::uint16_t, 1> heads {0};
    ring.publish(descriptors, heads);

    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck | protocol::protocolFeatureConfig);
        configureMemory(socket, ring.descriptor());
        const auto channels = startQueue(socket, ring, 1);
        writeKick(channels.kick);
        awaitCondition([&] { return recorder->runs(1) >= 1; });
        require(ring.usedIndex() == 0, "the event queue's buffer was used with no event");
        // Setting the memory up reset the device once; the suspend must add none.
        const auto resets = recorder->resets();

        // The suspend: the guest disables the ring and enables it again.
        const protocol::VringState disabled {.index = 1, .value = 0};
        sendRequestAndReadAcknowledgement(socket, protocol::VhostRequest::setVringEnable, bytesOf(disabled));
        const protocol::VringState enabled {.index = 1, .value = 1};
        sendRequestAndReadAcknowledgement(socket, protocol::VhostRequest::setVringEnable, bytesOf(enabled));
        require(recorder->resets() == resets, "the ring disable reset a device that was idle");
        require(ring.usedIndex() == 0, "the event queue's buffer was used with no event");

        // The first packet raises the source change, which the buffer posted before the suspend takes.
        (void)readConfig(socket, 0, sizeof(virtio_video_config));
        awaitCall(channels.call.readEnd.get());
        require(recorder->failure().empty(), recorder->failure().c_str());
        require(ring.usedIndex() == 1 && ring.usedElement(0).id == 0 && ring.usedElement(0).length == sizeof(virtio_video_event),
                "the event did not reach the buffer posted before the suspend");
        const auto event = load<virtio_video_event>(
            std::span<const std::uint8_t>(ring.memory()).subspan(eventOffset, sizeof(virtio_video_event)));
        require(event.event_type == VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED && event.stream_id == streamId,
                "the buffer holds the wrong event");
        require(recorder->resets() == resets, "the device was reset");
    });
}
} // namespace

int main()
{
    try {
        eventQueueBufferSurvivesASuspend();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("video suspend ok");
    return 0;
}

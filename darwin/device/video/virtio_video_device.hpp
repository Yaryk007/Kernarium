// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "vhost_user_device.hpp"
#include "video_decoder.hpp"

#include <memory>

namespace uml::video {

inline constexpr std::uint32_t deviceId = 31;

class VirtioVideoDevice final : public uml::virtio::VhostUserDevice {
public:
    using DecoderFactory = std::function<std::unique_ptr<uml::platform::VideoDecoder>(uml::platform::VideoCodec)>;
    VirtioVideoDevice(std::vector<uml::platform::VideoDecodeCapability> capabilities,
                     DecoderFactory factory);
    ~VirtioVideoDevice() override;
    const char* name() const noexcept override;
    std::uint64_t features() const noexcept override;
    std::size_t queueCount() const noexcept override;
    std::vector<std::uint8_t> configuration() const override;
    void writeConfiguration(std::uint32_t, std::uint32_t, std::span<const std::uint8_t>) override;
    void setGuestMapper(GuestMapper mapper) override;
    void setSharedMemoryRegions(
        std::shared_ptr<const uml::virtio::SharedMemoryRegions> regions) override;
    uml::virtio::VhostUserQueueResult processQueue(std::uint32_t index, std::uint64_t requestId,
        std::span<const std::uint8_t> request, std::size_t responseCapacity) override;
    bool takeQueueCompletion(uml::virtio::VhostUserQueueCompletion&) override;
    void reset() noexcept override;
    /* Readable while an event waits for a buffer of the guest's event queue. */
    int eventDescriptor() const noexcept override;
    void drainEventSource() override;
    /* A codec stream outlives a guest suspend: the buffers it holds stay
     * with the device, and the transport keeps their completions until the
     * ring is enabled again. */
    bool keepsDeferredRequestsWhileDisabled() const noexcept override { return true; }
    bool keepsRequestsOnDisabledRing() const noexcept override { return true; }

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::video

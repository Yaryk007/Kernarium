// SPDX-License-Identifier: GPL-2.0-or-later

/* A hardware device behind the real vhost-user transport: the configuration
 * change the front end is sent for a plug and for an unplug, and the event
 * bitmaps it reads after each. The front end is the GPU tests', so their
 * support header is on the path; the GPU protocol it includes must be the one
 * that header finds, which is why its directory comes before the input
 * library's. */

#include "gpu_test_support.hpp"
#include "virtio_input_device.hpp"

#include <cstdio>
#include <memory>
#include <utility>

namespace {

using namespace uml::gpu::testing;
using uml::gpu::VhostUserServer;
using uml::input::virtio::DeviceKind;
using uml::input::virtio::EventQueue;
using uml::input::virtio::VirtioInputDevice;
namespace gpu = uml::gpu::protocol;
namespace input = uml::input::virtio::protocol;

constexpr std::uint64_t protocolFeatures = gpu::protocolFeatureReplyAck |
    gpu::protocolFeatureSlaveRequest | gpu::protocolFeatureConfig;

/* The configuration the front end reads after it selected the event bitmap of
 * `type`. */
input::InputConfig readEventBits(int socket, std::uint8_t type)
{
    const std::array<std::uint8_t, 2> selection {input::configEventBits, type};
    const gpu::ConfigHeader header {
        .offset = 0,
        .size = selection.size(),
        .flags = gpu::configFlagWritable,
    };
    sendRequestAndReadAcknowledgement(
        socket, gpu::VhostRequest::setConfig, configPayload(header, selection));
    return readConfig(socket, 0, sizeof(input::InputConfig))
        .as<input::InputConfig>();
}

bool hasEvents(int socket)
{
    return readEventBits(socket, input::eventKey).size != 0;
}

void requireNoRequest(int channel)
{
    pollfd quiet {.fd = channel, .events = POLLIN};
    require(::poll(&quiet, 1, 150) == 0);
}

/* One request on the backend channel, answered as a front end does. */
void expectOneChange(int channel)
{
    const auto request =
        readBackendRequest(channel, gpu::VhostSlaveRequest::configChange);
    replyToBackend(channel, request);
}

void aPlugAndAnUnplugEachSendOneConfigurationChange()
{
    auto queue = std::make_shared<EventQueue>(EventQueue::Attachment::pluggable);
    queue->eventBufferPosted();
    auto device = std::make_unique<VirtioInputDevice>(
        DeviceKind::hardwareKeyboard, queue);
    auto* source = device.get();
    VhostUserServer server {std::move(device)};
    withServerConnection(server, [&](int socket) {
        const auto backend = openBackendChannel(socket, protocolFeatures);
        const int channel = backend.descriptors[0].get();
        requireNoRequest(channel);
        require(!hasEvents(socket));

        require(queue->setConnected(true));
        expectOneChange(channel);
        awaitCondition([&] { return !source->configChangePending(); });
        require(hasEvents(socket));
        // Plugging a plugged device sends nothing.
        require(!queue->setConnected(true));
        requireNoRequest(channel);

        require(queue->setConnected(false));
        expectOneChange(channel);
        awaitCondition([&] { return !source->configChangePending(); });
        require(!hasEvents(socket));
        require(!queue->setConnected(false));
        requireNoRequest(channel);

        // Nothing but a plug or an unplug sends one: not the guest's status,
        // and not what the host sends.
        require(queue->setConnected(true));
        expectOneChange(channel);
        awaitCondition([&] { return !source->configChangePending(); });
        const std::array<input::InputEvent, 2> press {
            input::InputEvent {input::eventKey, 30, 1},
            input::InputEvent {input::eventSynchronize,
                               input::synchronizeReport, 0}};
        (void)queue->enqueue(press);
        requireNoRequest(channel);
    });
}

void aPlugBeforeTheDriverProbesSendsItsChangeWithTheFirstBuffer()
{
    // The application plugs before the guest's kernel has probed: the front
    // end reads no events, is sent nothing, and is told when the driver's
    // first event buffer finds the device plugged.
    auto queue = std::make_shared<EventQueue>(EventQueue::Attachment::pluggable);
    auto device = std::make_unique<VirtioInputDevice>(
        DeviceKind::mouse, queue);
    auto* source = device.get();
    VhostUserServer server {std::move(device)};
    withServerConnection(server, [&](int socket) {
        const auto backend = openBackendChannel(socket, protocolFeatures);
        const int channel = backend.descriptors[0].get();
        require(queue->setConnected(true));
        requireNoRequest(channel);
        require(!hasEvents(socket));

        queue->eventBufferPosted();
        expectOneChange(channel);
        awaitCondition([&] { return !source->configChangePending(); });
        require(hasEvents(socket));
        requireNoRequest(channel);
    });
}

} // namespace

int main()
{
    try {
        aPlugAndAnUnplugEachSendOneConfigurationChange();
        aPlugBeforeTheDriverProbesSendsItsChangeWithTheFirstBuffer();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("virtio input transport ok");
    return 0;
}

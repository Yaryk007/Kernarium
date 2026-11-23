// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* What the tests of the host's hardware keyboard, mouse and pointer share: the
 * events they compare, and the three devices with the HardwareInput over them,
 * driven as a guest's driver would. */

#include "hardware_input.hpp"
#include "hardware_reports.hpp"
#include "require.hpp"
#include "wire_bytes.hpp"

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace uml::input::virtio::testing {

using uml::testing::require;

using Event = protocol::InputEvent;
using Events = std::vector<Event>;

constexpr Event event(
    std::uint16_t type,
    std::uint16_t code,
    std::int32_t value) noexcept
{
    return {type, code, value};
}

constexpr Event syncEvent() noexcept
{
    return event(protocol::eventSynchronize, protocol::synchronizeReport, 0);
}

inline bool sameEvents(const Events& actual, std::initializer_list<Event> expected)
{
    if (actual.size() != expected.size())
        return false;
    std::size_t index = 0;
    for (const auto& one : expected) {
        const auto& got = actual[index++];
        if (got.type != one.type || got.code != one.code ||
            got.value != one.value)
            return false;
    }
    return true;
}

inline bool sameEvents(const HardwareReport& report, std::initializer_list<Event> expected)
{
    return sameEvents(report.events, expected);
}


constexpr auto key = protocol::eventKey;

constexpr Event slot0() noexcept
{
    return event(protocol::eventAbsolute, protocol::absoluteMtSlot, 0);
}
constexpr Event positionX(std::int32_t x) noexcept
{
    return event(protocol::eventAbsolute, protocol::absoluteMtPositionX, x);
}
constexpr Event positionY(std::int32_t y) noexcept
{
    return event(protocol::eventAbsolute, protocol::absoluteMtPositionY, y);
}
constexpr Event trackingId(std::int32_t id) noexcept
{
    return event(protocol::eventAbsolute, protocol::absoluteMtTrackingId, id);
}

struct Rig final {
    std::shared_ptr<EventQueue> keyboardQueue {
        std::make_shared<EventQueue>(EventQueue::Attachment::pluggable)};
    std::shared_ptr<EventQueue> mouseQueue {
        std::make_shared<EventQueue>(EventQueue::Attachment::pluggable)};
    std::shared_ptr<EventQueue> pointerQueue {
        std::make_shared<EventQueue>(EventQueue::Attachment::pluggable)};
    VirtioInputDevice keyboardDevice {
        DeviceKind::hardwareKeyboard, keyboardQueue};
    VirtioInputDevice mouseDevice {DeviceKind::mouse, mouseQueue};
    VirtioInputDevice pointerDevice {DeviceKind::pointer, pointerQueue};
    HardwareInput input {keyboardQueue, mouseQueue, pointerQueue};
    std::vector<std::pair<HardwareStatus, bool>> statuses;

    /* `probed`: the guest's drivers have probed and posted their first event
     * buffer, as they have long before an application plugs a device. */
    explicit Rig(bool probed = true)
    {
        input.setStatusCallback([this](HardwareStatus kind, bool on) {
            statuses.emplace_back(kind, on);
        });
        if (probed) {
            probes(keyboardDevice);
            probes(mouseDevice);
            probes(pointerDevice);
        }
    }

    /* The driver's first event buffer: the device is advertised from here. */
    static void probes(VirtioInputDevice& device)
    {
        (void)device.processQueue(protocol::eventQueue, 3, {}, 8);
    }

    static void status(VirtioInputDevice& device, Event sent)
    {
        const auto result = device.processQueue(
            protocol::statusQueue, 1, uml::virtio::toBytes(sent), 0);
        require(result.action == uml::virtio::VhostUserQueueAction::complete);
    }

    static void registers(VirtioInputDevice& device)
    {
        status(device, event(
            protocol::statusNotice, protocol::noticeRegistration, 1));
    }

    static Events received(VirtioInputDevice& device)
    {
        Events all;
        for (;;) {
            const auto result =
                device.processQueue(protocol::eventQueue, 2, {}, 8);
            if (result.action != uml::virtio::VhostUserQueueAction::complete)
                return all;
            all.push_back(uml::testing::loadExact<protocol::InputEvent>(
                result.response));
        }
    }

    void plugEverything()
    {
        input.setKeyboardConnected(true);
        input.setMouseConnected(true);
        registers(keyboardDevice);
        registers(mouseDevice);
        registers(pointerDevice);
    }
};

} // namespace uml::input::virtio::testing

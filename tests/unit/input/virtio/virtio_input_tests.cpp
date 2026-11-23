// SPDX-License-Identifier: GPL-2.0-or-later

#include "hid_keyboard.hpp"
#include "require.hpp"
#include "touchscreen.hpp"
#include "virtio_input_device.hpp"
#include "wire_bytes.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <poll.h>
#include <string>
#include <vector>

namespace {

using uml::input::virtio::DeviceKind;
using uml::input::virtio::EventQueue;
using uml::input::virtio::VirtioInputDevice;
namespace protocol = uml::input::virtio::protocol;

using uml::testing::require;

protocol::InputConfig configuration(VirtioInputDevice& device)
{
    return uml::testing::loadExact<protocol::InputConfig>(
        device.configuration());
}

void select(
    VirtioInputDevice& device,
    std::uint8_t selected,
    std::uint8_t subselected)
{
    device.writeConfiguration(0, 0, std::span {&selected, 1U});
    device.writeConfiguration(1, 0, std::span {&subselected, 1U});
}

bool bit(const protocol::InputConfig& config, std::uint16_t index)
{
    return (config.value.bitmap[index / 8U] & (1U << (index % 8U))) != 0;
}

void touchscreenConfigurationIsDirectWithAFixedRange()
{
    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::touchscreen, events);
    require(device.queueCount() == 2);
    require(device.features() == 0);

    select(device, protocol::configIdName, 0);
    const auto name = configuration(device);
    require(name.size == std::strlen("Kernarium Touchscreen"));
    require(std::memcmp(
                name.value.string.data(),
                "Kernarium Touchscreen",
                name.size) == 0);

    select(device, protocol::configPropertyBits, 0);
    const auto properties = configuration(device);
    require(properties.size == 1);
    require(bit(properties, protocol::propertyDirect));

    select(device, protocol::configEventBits, protocol::eventKey);
    const auto keys = configuration(device);
    require(bit(keys, protocol::buttonTouch));
    require(!bit(keys, 0x110)); // BTN_LEFT
    require(!bit(keys, 0x111)); // BTN_RIGHT
    require(!bit(keys, 0x112)); // BTN_MIDDLE
    select(device, protocol::configEventBits, 0x02); // EV_REL
    require(configuration(device).size == 0);
    select(device, protocol::configEventBits, protocol::eventAbsolute);
    const auto absoluteBits = configuration(device);
    require(bit(absoluteBits, protocol::absoluteX));
    require(bit(absoluteBits, protocol::absoluteY));
    require(bit(absoluteBits, protocol::absoluteMtSlot));
    require(bit(absoluteBits, protocol::absoluteMtTrackingId));
    require(bit(absoluteBits, protocol::absoluteMtPositionX));
    require(bit(absoluteBits, protocol::absoluteMtPositionY));
    require(bit(absoluteBits, protocol::absoluteMtToolType));

    select(device, protocol::configAbsoluteInfo, protocol::absoluteX);
    const auto x = configuration(device);
    require(x.size == sizeof(protocol::InputAbsoluteInfo));
    require(x.value.absolute.minimum == 0);
    require(x.value.absolute.maximum == protocol::touchscreenAbsoluteMaximum);
    select(device, protocol::configAbsoluteInfo, protocol::absoluteY);
    const auto y = configuration(device);
    require(y.value.absolute.minimum == 0);
    require(y.value.absolute.maximum == protocol::touchscreenAbsoluteMaximum);
    for (const auto axis : {protocol::absoluteMtPositionX,
                            protocol::absoluteMtPositionY}) {
        select(device, protocol::configAbsoluteInfo,
               static_cast<std::uint8_t>(axis));
        const auto info = configuration(device);
        require(info.size == sizeof(protocol::InputAbsoluteInfo));
        require(info.value.absolute.minimum == 0);
        require(info.value.absolute.maximum == 32767);
    }
    select(device, protocol::configAbsoluteInfo, protocol::absoluteMtSlot);
    require(configuration(device).value.absolute.maximum == 9);
    select(device, protocol::configAbsoluteInfo, protocol::absoluteMtTrackingId);
    require(configuration(device).value.absolute.maximum == 65535);
    // The tool type spans MT_TOOL_FINGER through MT_TOOL_PALM.
    select(device, protocol::configAbsoluteInfo, protocol::absoluteMtToolType);
    const auto tool = configuration(device);
    require(tool.size == sizeof(protocol::InputAbsoluteInfo));
    require(tool.value.absolute.minimum == 0);
    require(tool.value.absolute.maximum == 2);
}

std::int32_t value(
    const uml::input::virtio::TouchscreenReport& report,
    std::uint16_t type,
    std::uint16_t code)
{
    for (const auto& event : report.span()) {
        if (event.type == type && event.code == code)
            return event.value;
    }
    require(false);
    return -1;
}

void multipleContactsMoveAndLiftIndependently()
{
    uml::input::virtio::Touchscreen screen;
    const auto first = screen.touch(3, 100, 200, true);
    require(first.events[0].type == protocol::eventKey);
    require(first.events[0].code == protocol::buttonTouch);
    require(first.events[first.count - 1].type == protocol::eventSynchronize);
    const auto firstId = value(first, 3, 0x39); // ABS_MT_TRACKING_ID
    const auto second = screen.touch(1, 300, 400, true);
    const auto secondId = value(second, 3, 0x39);
    require(firstId >= 0 && secondId >= 0 && firstId != secondId);
    require(value(second, 3, 0x2f) == 1); // ABS_MT_SLOT
    require(value(second, 3, 0x35) == 300);
    require(value(second, 3, 0x36) == 400);
    require(value(second, 3, 0) == 100); // ABS_X keeps the original finger
    require(value(second, 3, 1) == 200);

    const auto move = screen.touch(3, 150, 250, true);
    require(value(move, 3, 0x2f) == 3);
    require(value(move, 3, 0x35) == 150);
    require(value(move, 3, 0x36) == 250);
    for (const auto& event : move.span())
        require(event.code != 0x39 || event.type != 3);

    const auto lift = screen.touch(3, 150, 250, false);
    require(value(lift, 3, 0x39) == -1);
    require(value(lift, 1, 0x14a) == 1); // BTN_TOUCH stays down
    require(value(lift, 3, 0) == 300); // primary moves to the remaining finger
    require(value(lift, 3, 1) == 400);
    const auto last = screen.touch(1, 300, 400, false);
    require(value(last, 1, 0x14a) == 0);
    require(value(last, 3, 0x39) == -1);
    require(screen.touch(1, 300, 400, false).count == 0);
    require(value(screen.touch(3, 100, 200, true), 3, 0x39) != firstId);
}

void tenContactsAndTrackingIdWrapKeepLiveIdsUnique()
{
    uml::input::virtio::Touchscreen screen;
    std::array<std::int32_t, 10> ids {};
    for (std::uint32_t slot = 0; slot < ids.size(); ++slot) {
        ids[slot] = value(screen.touch(slot, slot, slot, true), 3, 0x39);
        for (std::uint32_t prior = 0; prior < slot; ++prior)
            require(ids[slot] != ids[prior]);
    }
    require(screen.touch(10, 0, 0, true).count == 0);
    require(screen.touch(0, 32768, 0, true).count == 0);
    require(screen.touch(0, 0, 32768, true).count == 0);
    for (std::uint32_t index = 0; index < 65536; ++index) {
        (void)screen.touch(9, 0, 0, false);
        const auto id = value(screen.touch(9, 0, 0, true), 3, 0x39);
        require(id >= 0 && id <= 65535);
        for (std::size_t slot = 0; slot < 9; ++slot)
            require(id != ids[slot]);
    }
    for (std::uint32_t slot = 0; slot < 10; ++slot) {
        const auto lift = screen.touch(slot, 0, 0, false);
        require(value(lift, 1, 0x14a) == (slot == 9 ? 0 : 1));
    }
}

void liftingAtTheViewportEdgeDeliversTheFinalMovementBeforeUp()
{
    uml::input::virtio::Touchscreen screen;
    (void)screen.touch(0, 100, 200, true);
    const auto lift = screen.touch(0, 32767, 200, false);
    unsigned reports = 0;
    bool moved = false;
    bool released = false;
    for (const auto& event : lift.span()) {
        if (event.type == 3 && event.code == 0x35) {
            require(!released);
            require(event.value == 32767);
            moved = true;
        }
        if (event.type == 3 && event.code == 0x39) {
            require(moved && reports == 1);
            require(event.value == -1);
            released = true;
        }
        if (event.type == 0 && event.code == 0)
            ++reports;
    }
    require(moved && released && reports == 2);
    require(lift.events[lift.count - 1].type == protocol::eventSynchronize);
}

using Event = protocol::InputEvent;
using Frame = std::vector<Event>;

constexpr Event event(
    std::uint16_t type,
    std::uint16_t code,
    std::int32_t value) noexcept
{
    return {type, code, value};
}

constexpr Event touchDown(std::int32_t down) noexcept
{
    return event(protocol::eventKey, protocol::buttonTouch, down);
}

constexpr Event slotEvent(std::int32_t slot) noexcept
{
    return event(protocol::eventAbsolute, protocol::absoluteMtSlot, slot);
}

constexpr Event toolEvent(std::int32_t tool) noexcept
{
    return event(protocol::eventAbsolute, protocol::absoluteMtToolType, tool);
}

constexpr Event trackingEvent(std::int32_t id) noexcept
{
    return event(protocol::eventAbsolute, protocol::absoluteMtTrackingId, id);
}

constexpr Event absolute(std::uint16_t axis, std::int32_t value) noexcept
{
    return event(protocol::eventAbsolute, axis, value);
}

constexpr Event endOfReport() noexcept
{
    return event(
        protocol::eventSynchronize, protocol::synchronizeReport, 0);
}

bool equal(const Event& left, const Event& right) noexcept
{
    return left.type == right.type && left.code == right.code &&
        left.value == right.value;
}

// A report's SYN_REPORT-terminated frames, which the guest handles one by
// one.
std::vector<Frame> frames(std::span<const Event> events)
{
    std::vector<Frame> result;
    Frame current;
    for (const auto& one : events) {
        current.push_back(one);
        if (one.type == protocol::eventSynchronize) {
            result.push_back(std::move(current));
            current.clear();
        }
    }
    require(current.empty()); // every report ends in a SYN_REPORT
    return result;
}

std::vector<Frame> frames(const uml::input::virtio::TouchscreenReport& report)
{
    return frames(report.span());
}

bool matches(const Frame& frame, std::initializer_list<Event> expected)
{
    if (frame.size() != expected.size())
        return false;
    std::size_t index = 0;
    for (const auto& one : expected) {
        if (!equal(frame[index++], one))
            return false;
    }
    return true;
}

std::size_t position(const Frame& frame, std::uint16_t code)
{
    for (std::size_t index = 0; index < frame.size(); ++index) {
        if (frame[index].type == protocol::eventAbsolute &&
            frame[index].code == code)
            return index;
    }
    return frame.size();
}

bool has(const Frame& frame, std::uint16_t code)
{
    return position(frame, code) != frame.size();
}

std::int32_t trackingId(const Frame& frame)
{
    const auto index = position(frame, protocol::absoluteMtTrackingId);
    require(index != frame.size());
    return frame[index].value;
}

constexpr auto absoluteX = protocol::absoluteX;
constexpr auto absoluteY = protocol::absoluteY;
constexpr auto absoluteMtX = protocol::absoluteMtPositionX;
constexpr auto absoluteMtY = protocol::absoluteMtPositionY;

void aNewContactIsAFingerBeforeItsTrackingId()
{
    uml::input::virtio::Touchscreen screen;
    const auto down = frames(screen.touch(2, 100, 200, true));
    require(down.size() == 1);
    const auto id = trackingId(down[0]);
    require(id >= 0);
    // The tool type is set first, and in the same report as the contact.
    require(matches(down[0], {
        touchDown(1), slotEvent(2), toolEvent(protocol::toolTypeFinger),
        trackingEvent(id),
        absolute(absoluteMtX, 100), absolute(absoluteMtY, 200),
        absolute(absoluteX, 100), absolute(absoluteY, 200),
        endOfReport(),
    }));
    // A move or a lift states no tool type.
    const auto move = frames(screen.touch(2, 150, 250, true));
    require(move.size() == 1 && !has(move[0], protocol::absoluteMtToolType));
    const auto lift = frames(screen.touch(2, 150, 250, false));
    require(lift.size() == 1 && !has(lift[0], protocol::absoluteMtToolType));
    // A re-entry after a lift is a new contact, and says so the same way.
    const auto again = frames(screen.touch(2, 150, 250, true));
    require(again.size() == 1);
    require(position(again[0], protocol::absoluteMtToolType) <
        position(again[0], protocol::absoluteMtTrackingId));
}

void cancellingAContactIsAPalmReportThenItsLift()
{
    uml::input::virtio::Touchscreen screen;
    (void)screen.touch(0, 100, 200, true);
    const auto cancelled = frames(screen.cancel(0));
    require(cancelled.size() == 2);
    // The slot turns palm while it keeps its tracking ID and BTN_TOUCH stays
    // down, so a reader still sees the contact when it learns it is a palm.
    require(matches(cancelled[0], {
        touchDown(1), slotEvent(0), toolEvent(protocol::toolTypePalm),
        absolute(absoluteX, 100), absolute(absoluteY, 200),
        endOfReport(),
    }));
    // The ordinary lift, where the contact was.
    require(matches(cancelled[1], {
        touchDown(0), slotEvent(0), trackingEvent(-1),
        absolute(absoluteX, 100), absolute(absoluteY, 200),
        endOfReport(),
    }));
    // Nothing is left to lift or cancel.
    require(screen.touch(0, 100, 200, false).count == 0);
    require(screen.cancel(0).count == 0);
}

void cancellingAMovedContactSynthesizesNoMotion()
{
    uml::input::virtio::Touchscreen screen;
    (void)screen.touch(4, 100, 200, true);
    (void)screen.touch(4, 150, 250, true);
    const auto cancelled = frames(screen.cancel(4));
    require(cancelled.size() == 2);
    for (const auto& frame : cancelled) {
        require(!has(frame, absoluteMtX) && !has(frame, absoluteMtY));
    }
    require(matches(cancelled[0], {
        touchDown(1), slotEvent(4), toolEvent(protocol::toolTypePalm),
        absolute(absoluteX, 150), absolute(absoluteY, 250),
        endOfReport(),
    }));
    require(matches(cancelled[1], {
        touchDown(0), slotEvent(4), trackingEvent(-1),
        absolute(absoluteX, 150), absolute(absoluteY, 250),
        endOfReport(),
    }));
}

void cancellingOneOfTwoContactsKeepsTheOtherDown()
{
    uml::input::virtio::Touchscreen screen;
    const auto firstId = trackingId(frames(
        screen.touch(3, 100, 200, true))[0]);
    const auto secondId = trackingId(frames(
        screen.touch(1, 300, 400, true))[0]);
    require(firstId != secondId);

    // The first contact is the single-touch primary: the palm report states
    // its position, and its lift hands ABS_X/Y to the contact that remains.
    const auto first = frames(screen.cancel(3));
    require(first.size() == 2);
    require(matches(first[0], {
        touchDown(1), slotEvent(3), toolEvent(protocol::toolTypePalm),
        absolute(absoluteX, 100), absolute(absoluteY, 200),
        endOfReport(),
    }));
    require(matches(first[1], {
        touchDown(1), slotEvent(3), trackingEvent(-1),
        absolute(absoluteX, 300), absolute(absoluteY, 400),
        endOfReport(),
    }));

    // The survivor moves on and lifts as an ordinary contact.
    const auto move = frames(screen.touch(1, 350, 450, true));
    require(move.size() == 1 && !has(move[0], protocol::absoluteMtTrackingId));
    const auto last = frames(screen.touch(1, 350, 450, false));
    require(matches(last[0], {
        touchDown(0), slotEvent(1), trackingEvent(-1),
        absolute(absoluteX, 350), absolute(absoluteY, 450),
        endOfReport(),
    }));
}

void cancellingASecondaryContactLeavesTheSingleTouchCoordinates()
{
    uml::input::virtio::Touchscreen screen;
    (void)screen.touch(3, 100, 200, true);
    (void)screen.touch(1, 300, 400, true);
    const auto cancelled = frames(screen.cancel(1));
    require(cancelled.size() == 2);
    require(matches(cancelled[0], {
        touchDown(1), slotEvent(1), toolEvent(protocol::toolTypePalm),
        absolute(absoluteX, 100), absolute(absoluteY, 200),
        endOfReport(),
    }));
    require(matches(cancelled[1], {
        touchDown(1), slotEvent(1), trackingEvent(-1),
        absolute(absoluteX, 100), absolute(absoluteY, 200),
        endOfReport(),
    }));
    // The primary is still the first contact, and it lifts last.
    const auto last = frames(screen.touch(3, 100, 200, false));
    require(matches(last[0], {
        touchDown(0), slotEvent(3), trackingEvent(-1),
        absolute(absoluteX, 100), absolute(absoluteY, 200),
        endOfReport(),
    }));
}

void cancellingAnInactiveOrInvalidSlotReportsNothing()
{
    uml::input::virtio::Touchscreen screen;
    require(screen.cancel(0).count == 0);
    require(screen.cancel(protocol::touchscreenContactCount).count == 0);
    require(screen.cancel(0xffffffffU).count == 0);

    (void)screen.touch(5, 10, 20, true);
    require(screen.cancel(4).count == 0);
    require(screen.cancel(protocol::touchscreenContactCount).count == 0);
    // The contact that is down is undisturbed by the refused cancels.
    const auto move = frames(screen.touch(5, 11, 21, true));
    require(move.size() == 1 && !has(move[0], protocol::absoluteMtTrackingId));

    // A released slot and an already cancelled one hold no contact.
    (void)screen.touch(5, 11, 21, false);
    require(screen.cancel(5).count == 0);
    (void)screen.touch(6, 10, 20, true);
    require(screen.cancel(6).count == 12);
    require(screen.cancel(6).count == 0);
}

void aSlotIsAFingerAgainAfterACancel()
{
    uml::input::virtio::Touchscreen screen;
    const auto first = trackingId(frames(screen.touch(0, 100, 200, true))[0]);
    (void)screen.cancel(0);
    const auto again = frames(screen.touch(0, 500, 600, true));
    require(again.size() == 1);
    require(matches(again[0], {
        touchDown(1), slotEvent(0), toolEvent(protocol::toolTypeFinger),
        trackingEvent(trackingId(again[0])),
        absolute(absoluteMtX, 500), absolute(absoluteMtY, 600),
        absolute(absoluteX, 500), absolute(absoluteY, 600),
        endOfReport(),
    }));
    require(trackingId(again[0]) >= 0 && trackingId(again[0]) != first);
    // And it is an ordinary contact: it lifts with one report.
    const auto lift = frames(screen.touch(0, 500, 600, false));
    require(lift.size() == 1);
    require(matches(lift[0], {
        touchDown(0), slotEvent(0), trackingEvent(-1),
        absolute(absoluteX, 500), absolute(absoluteY, 600),
        endOfReport(),
    }));
}

void everyContactSlotCanBeCancelled()
{
    uml::input::virtio::Touchscreen screen;
    for (int round = 0; round < 2; ++round) {
        for (std::uint32_t slot = 0; slot < 10; ++slot)
            (void)screen.touch(slot, slot * 100, slot * 50, true);
        for (std::uint32_t slot = 0; slot < 10; ++slot) {
            const auto cancelled = frames(screen.cancel(slot));
            require(cancelled.size() == 2);
            require(cancelled[0][1].value == static_cast<std::int32_t>(slot));
            require(cancelled[0][2].value == protocol::toolTypePalm);
            require(!has(cancelled[0], protocol::absoluteMtTrackingId));
            require(trackingId(cancelled[1]) == -1);
            // BTN_TOUCH stays down until the last contact is gone.
            require(cancelled[0][0].value == 1);
            require(cancelled[1][0].value == (slot == 9 ? 0 : 1));
            // ABS_X/Y follow the first contact still down.
            if (slot != 9) {
                require(cancelled[1][3].value ==
                    static_cast<std::int32_t>((slot + 1) * 100));
            }
        }
        for (std::uint32_t slot = 0; slot < 10; ++slot)
            require(screen.cancel(slot).count == 0);
    }
}

void aReportFitsItsBuffer()
{
    uml::input::virtio::Touchscreen screen;
    // The longest sequences: a lift at a new point, and a cancel.
    require(screen.touch(0, 100, 200, true).count == 9);
    require(screen.touch(0, 150, 250, false).count == 13);
    (void)screen.touch(0, 100, 200, true);
    require(screen.cancel(0).count == 12);
    (void)screen.touch(0, 100, 200, true);
    (void)screen.touch(1, 100, 200, true);
    require(screen.cancel(0).count == 12);
}

// What a guest reads from the event queue, in order.
std::vector<Event> deliver(
    VirtioInputDevice& device,
    EventQueue& events,
    std::span<const Event> sent)
{
    events.enqueue(sent);
    pollfd readiness {
        .fd = device.eventDescriptor(),
        .events = POLLIN,
        .revents = 0,
    };
    require(::poll(&readiness, 1, 1000) == 1);
    device.drainEventSource();
    std::vector<Event> received;
    for (std::size_t index = 0; index < sent.size(); ++index) {
        const auto result =
            device.processQueue(protocol::eventQueue, 10 + index, {}, 8);
        require(result.action == uml::virtio::VhostUserQueueAction::complete);
        received.push_back(
            uml::testing::loadExact<protocol::InputEvent>(result.response));
    }
    require(device.processQueue(protocol::eventQueue, 99, {}, 8).action ==
        uml::virtio::VhostUserQueueAction::retry);
    return received;
}

void aCancelReachesTheGuestInOrderAsTwoReports()
{
    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::touchscreen, events);
    uml::input::virtio::Touchscreen screen;

    // Down, move, cancel, down again, in the order the host sends them.
    std::vector<Event> sent;
    for (const auto& report : {
             screen.touch(0, 100, 200, true),
             screen.touch(0, 150, 250, true),
             screen.cancel(0),
             screen.touch(0, 300, 400, true),
         }) {
        sent.insert(sent.end(), report.span().begin(), report.span().end());
    }
    const auto received = deliver(device, *events, sent);
    require(received.size() == sent.size());
    for (std::size_t index = 0; index < sent.size(); ++index)
        require(equal(received[index], sent[index]));

    const auto delivered = frames(received);
    require(delivered.size() == 5);
    // The palm frame precedes the lift frame, and the lift is the first frame
    // that ends the tracking ID.
    require(matches(delivered[2], {
        touchDown(1), slotEvent(0), toolEvent(protocol::toolTypePalm),
        absolute(absoluteX, 150), absolute(absoluteY, 250),
        endOfReport(),
    }));
    require(!has(delivered[2], protocol::absoluteMtTrackingId));
    require(trackingId(delivered[3]) == -1);
    require(!has(delivered[3], protocol::absoluteMtToolType));
    // The next contact in the slot is a finger before it has a tracking ID.
    require(position(delivered[4], protocol::absoluteMtToolType) <
        position(delivered[4], protocol::absoluteMtTrackingId));
    require(trackingId(delivered[4]) >= 0);
}

void keyboardAdvertisesEvdevKeysAndLeds()
{
    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::keyboard, events);
    select(device, protocol::configEventBits, protocol::eventKey);
    const auto keys = configuration(device);
    require(bit(keys, 1));
    require(bit(keys, 28));
    require(bit(keys, 57));
    require(bit(keys, protocol::keyboardKeyMaximum));

    select(device, protocol::configEventBits, protocol::eventLed);
    const auto leds = configuration(device);
    require(bit(leds, 0));
    require(bit(leds, 1));
    require(bit(leds, 2));
}

// Android's Generic key layout maps KEY_BACK and KEY_HOMEPAGE to BACK and
// HOME, and the guest's input core delivers only advertised keys.
void keyboardAdvertisesBackAndHomeAndNothingElseAbove127()
{
    require(protocol::keyBack == 158);
    require(protocol::keyHomepage == 172);

    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::keyboard, events);
    select(device, protocol::configEventBits, protocol::eventKey);
    const auto keys = configuration(device);
    require(bit(keys, protocol::keyBack));
    require(bit(keys, protocol::keyHomepage));
    // The map ends at the highest advertised key.
    require(keys.size == protocol::keyHomepage / 8U + 1U);

    constexpr std::uint16_t bitmapBits = 128U * 8U;
    for (std::uint16_t code = 0; code < bitmapBits; ++code) {
        const bool expected = (code >= 1U && code <= 127U) ||
            code == 158U || code == 172U;
        require(bit(keys, code) == expected);
        require(protocol::keyboardAdvertisesKey(code) == expected);
    }
    // A key beyond the bitmap is never advertised either.
    require(!protocol::keyboardAdvertisesKey(bitmapBits));
    require(!protocol::keyboardAdvertisesKey(0xffff));

    // The touchscreen's key map stays BTN_TOUCH alone.
    VirtioInputDevice touchscreen(DeviceKind::touchscreen, events);
    select(touchscreen, protocol::configEventBits, protocol::eventKey);
    const auto touchKeys = configuration(touchscreen);
    require(!bit(touchKeys, protocol::keyBack));
    require(!bit(touchKeys, protocol::keyHomepage));
}

// The camera's lens cover is a switch of the side keys: advertised only when
// the host states it, reported before anything else, and again after every
// reset, so a guest that probes late or restarts reads the state.
void sideKeysReportALensCoverSwitchOnlyWhenTheHostStatesOne()
{
    require(protocol::eventSwitch == 0x05);
    require(protocol::switchCameraLensCover == 0x09);

    auto plain = std::make_shared<EventQueue>();
    VirtioInputDevice withoutSwitch(DeviceKind::keyboard, plain);
    select(withoutSwitch, protocol::configEventBits, protocol::eventSwitch);
    require(configuration(withoutSwitch).size == 0);
    protocol::InputEvent event {};
    require(!plain->take(event));

    for (const bool covered : {true, false}) {
        auto events = std::make_shared<EventQueue>();
        events->setSwitch({protocol::switchCameraLensCover, covered});
        VirtioInputDevice device(DeviceKind::keyboard, events);
        select(device, protocol::configEventBits, protocol::eventSwitch);
        const auto bits = configuration(device);
        require(bit(bits, protocol::switchCameraLensCover));
        require(bits.size == 2);
        // Nothing else grows: the keys and LEDs are what they were.
        select(device, protocol::configEventBits, protocol::eventKey);
        require(bit(configuration(device), protocol::keyHomepage));
        select(device, protocol::configEventBits, 0x02); // EV_REL
        require(configuration(device).size == 0);

        for (int round = 0; round < 3; ++round) {
            require(events->take(event));
            require(event.type == protocol::eventSwitch);
            require(event.code == protocol::switchCameraLensCover);
            require(event.value == (covered ? 1 : 0));
            require(events->take(event));
            require(event.type == protocol::eventSynchronize);
            require(!events->take(event));
            // The guest resets the device; the state is reported again, once.
            events->reset();
        }
    }
}

void aSwitchIsReportedAheadOfTheKeysAndOnlyForAFixedDevice()
{
    auto events = std::make_shared<EventQueue>();
    events->setSwitch({protocol::switchCameraLensCover, true});
    const protocol::InputEvent key[] = {
        {protocol::eventKey, protocol::keyBack, 1},
        {protocol::eventSynchronize, protocol::synchronizeReport, 0},
    };
    require(!events->enqueue(key));
    protocol::InputEvent event {};
    require(events->take(event) && event.type == protocol::eventSwitch);
    require(events->take(event) && event.type == protocol::eventSynchronize);
    require(events->take(event) && event.type == protocol::eventKey && event.code == protocol::keyBack);
    // A second statement replaces the first and does not queue it twice.
    events->setSwitch({protocol::switchCameraLensCover, false});
    require(events->take(event) && event.type == protocol::eventSwitch && event.value == 0);
    require(events->take(event) && event.type == protocol::eventSynchronize);
    require(!events->take(event));

    // A pluggable device has none.
    auto mouse = std::make_shared<EventQueue>(EventQueue::Attachment::pluggable);
    mouse->setSwitch({protocol::switchCameraLensCover, true});
    require(!mouse->switchState());
    VirtioInputDevice device(DeviceKind::mouse, mouse);
    select(device, protocol::configEventBits, protocol::eventSwitch);
    require(configuration(device).size == 0);
}

void keyboardDeliversBackAndHomeAsKeyEvents()
{
    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::keyboard, events);

    // The runtime forwards a key as the key event and a report.
    std::array<protocol::InputEvent, 8> input {};
    std::size_t count = 0;
    const std::array keyCodes {protocol::keyBack, protocol::keyHomepage};
    for (const std::uint16_t code : keyCodes) {
        for (const std::int32_t value : {1, 0}) {
            input[count++] = {
                .type = protocol::eventKey,
                .code = code,
                .value = value,
            };
            input[count++] = {
                .type = protocol::eventSynchronize,
                .code = protocol::synchronizeReport,
                .value = 0,
            };
        }
    }
    events->enqueue(input);
    pollfd readiness {
        .fd = device.eventDescriptor(),
        .events = POLLIN,
        .revents = 0,
    };
    require(::poll(&readiness, 1, 1000) == 1);
    device.drainEventSource();
    for (const auto& sent : input) {
        const auto result =
            device.processQueue(protocol::eventQueue, 3, {}, 8);
        require(result.action == uml::virtio::VhostUserQueueAction::complete);
        const auto returned =
            uml::testing::loadExact<protocol::InputEvent>(result.response);
        require(returned.type == sent.type);
        require(returned.code == sent.code);
        require(returned.value == sent.value);
    }
    require(device.processQueue(protocol::eventQueue, 4, {}, 8).action ==
        uml::virtio::VhostUserQueueAction::retry);
}

void eventQueueDefersAndThenReturnsOneWireEvent()
{
    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::touchscreen, events);
    const auto empty = device.processQueue(protocol::eventQueue, 1, {}, 8);
    require(empty.action == uml::virtio::VhostUserQueueAction::retry);

    const std::array input {
        protocol::InputEvent {
            .type = protocol::eventAbsolute,
            .code = protocol::absoluteX,
            .value = 123,
        },
    };
    events->enqueue(input);
    pollfd readiness {
        .fd = device.eventDescriptor(),
        .events = POLLIN,
        .revents = 0,
    };
    require(::poll(&readiness, 1, 1000) == 1);
    device.drainEventSource();
    const auto result = device.processQueue(protocol::eventQueue, 2, {}, 8);
    require(result.action == uml::virtio::VhostUserQueueAction::complete);
    const auto returned =
        uml::testing::loadExact<protocol::InputEvent>(result.response);
    require(returned.type == protocol::eventAbsolute);
    require(returned.code == protocol::absoluteX);
    require(returned.value == 123);
}


// ---- The hardware keyboard, mouse and pointer: what each advertises, and
// ---- the plugging of the device.

using Attachment = EventQueue::Attachment;
using StatusEvent = uml::input::virtio::StatusEvent;

// A pluggable queue whose driver has probed: it has posted its first event
// buffer, as it has long before an application plugs the device.
std::shared_ptr<EventQueue> pluggableQueue()
{
    auto queue = std::make_shared<EventQueue>(Attachment::pluggable);
    queue->eventBufferPosted();
    return queue;
}

// One whose driver has not.
std::shared_ptr<EventQueue> unprobedQueue()
{
    return std::make_shared<EventQueue>(Attachment::pluggable);
}

// Whether the bitmap holds exactly the bits listed, out of every bit the
// configuration space can state.
bool holdsExactly(
    const protocol::InputConfig& config,
    std::initializer_list<std::uint16_t> bits)
{
    for (std::uint16_t index = 0; index < 128U * 8U; ++index) {
        bool expected = false;
        for (const auto wanted : bits)
            expected |= wanted == index;
        if (bit(config, index) != expected)
            return false;
    }
    return true;
}

std::string text(const protocol::InputConfig& config)
{
    return {config.value.string.data(), config.size};
}

void expectIdentity(
    DeviceKind kind,
    const char* name,
    const char* serial,
    std::uint16_t product)
{
    auto events = pluggableQueue();
    VirtioInputDevice device(kind, events);
    // The identity does not depend on the plug.
    for (const bool plugged : {false, true, false}) {
        (void)events->setConnected(plugged);
        select(device, protocol::configIdName, 0);
        require(text(configuration(device)) == name);
        select(device, protocol::configIdSerial, 0);
        require(text(configuration(device)) == serial);
        select(device, protocol::configIdDeviceIds, 0);
        const auto ids = configuration(device);
        require(ids.size == sizeof(protocol::InputDeviceIds));
        require(ids.value.ids.bus == protocol::busVirtual);
        require(ids.value.ids.vendor == 0x1af4);
        require(ids.value.ids.product == product);
        require(ids.value.ids.version == 1);
    }
    require(device.queueCount() == 2);
    require(device.features() == 0);
}

void everyKindKeepsItsPermanentIdentity()
{
    expectIdentity(
        DeviceKind::hardwareKeyboard, "Kernarium Hardware Keyboard",
        "kernarium-hardware-keyboard-0", 3);
    expectIdentity(
        DeviceKind::mouse, "Kernarium Mouse", "kernarium-mouse-0", 4);
    expectIdentity(
        DeviceKind::pointer, "Kernarium Pointer", "kernarium-pointer-0", 5);
    // The two devices that were there first keep theirs.
    expectIdentity(
        DeviceKind::touchscreen, "Kernarium Touchscreen",
        "kernarium-touch-0", 1);
    expectIdentity(
        DeviceKind::keyboard, "Kernarium Keyboard", "kernarium-keyboard-0", 2);
}

void hardwareKeyboardAdvertisesExactlyTheTablesKeys()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::hardwareKeyboard, events);
    (void)events->setConnected(true);
    select(device, protocol::configEventBits, protocol::eventKey);
    const auto keys = configuration(device);
    const auto& image = uml::input::virtio::hid::keyImage();
    for (std::uint16_t code = 0; code < 128U * 8U; ++code) {
        const bool expected = code < 256U &&
            (image[code / 8U] & (1U << (code % 8U))) != 0;
        require(bit(keys, code) == expected);
        require(uml::input::virtio::hid::producesKey(code) == expected);
    }
    // The ends of the image: Escape, and F24 (193 and 194 are F23 and F24).
    require(bit(keys, 1) && bit(keys, 194) && !bit(keys, 195));
    require(keys.size == 194 / 8U + 1U);
    // KEY_UNKNOWN and KEY_RESERVED are not keys of the keyboard.
    require(!bit(keys, 0) && !bit(keys, 240));

    select(device, protocol::configEventBits, protocol::eventLed);
    require(holdsExactly(configuration(device), {0, 1, 2}));
    // No auto-repeat, no relative or absolute axes, no properties.
    for (const std::uint8_t type :
         std::initializer_list<std::uint8_t> {0x02, 0x03, 0x14, 0x04, 0x05}) {
        select(device, protocol::configEventBits, type);
        require(configuration(device).size == 0);
    }
    select(device, protocol::configPropertyBits, 0);
    require(configuration(device).size == 0);
}

void mouseAdvertisesRelativeAxesWheelsButtonsAndTheCaptureLed()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::mouse, events);
    (void)events->setConnected(true);
    select(device, protocol::configEventBits, 0x02); // EV_REL
    // X, Y, HWHEEL, WHEEL, WHEEL_HI_RES, HWHEEL_HI_RES.
    require(holdsExactly(configuration(device), {0, 1, 6, 8, 0x0b, 0x0c}));
    select(device, protocol::configEventBits, protocol::eventKey);
    // BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA.
    require(holdsExactly(
        configuration(device), {0x110, 0x111, 0x112, 0x113, 0x114}));
    select(device, protocol::configEventBits, protocol::eventLed);
    require(holdsExactly(configuration(device), {8})); // LED_MISC
    for (const std::uint8_t type :
         std::initializer_list<std::uint8_t> {0x03, 0x14, 0x04}) {
        select(device, protocol::configEventBits, type);
        require(configuration(device).size == 0);
    }
    select(device, protocol::configPropertyBits, 0);
    require(configuration(device).size == 0);
    select(device, protocol::configAbsoluteInfo, protocol::absoluteX);
    require(configuration(device).size == 0);
}

void pointerAdvertisesAnAbsoluteMouseAndNoInputProperty()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::pointer, events);
    (void)events->setConnected(true);
    select(device, protocol::configEventBits, protocol::eventKey);
    // BTN_TOOL_MOUSE, the five buttons, BTN_TOUCH.
    require(holdsExactly(
        configuration(device),
        {0x146, 0x14a, 0x110, 0x111, 0x112, 0x113, 0x114}));
    select(device, protocol::configEventBits, protocol::eventAbsolute);
    // MT_SLOT, MT_POSITION_X, MT_POSITION_Y, MT_TRACKING_ID: no tool type,
    // which an absolute mouse in the slot protocol does not take.
    require(holdsExactly(configuration(device), {0x2f, 0x35, 0x36, 0x39}));
    for (const std::uint8_t type :
         std::initializer_list<std::uint8_t> {0x02, 0x11, 0x14, 0x04}) {
        select(device, protocol::configEventBits, type);
        require(configuration(device).size == 0);
    }
    select(device, protocol::configPropertyBits, 0);
    require(configuration(device).size == 0); // not direct: it is a mouse

    for (const auto axis : {protocol::absoluteMtPositionX,
                            protocol::absoluteMtPositionY}) {
        select(device, protocol::configAbsoluteInfo,
               static_cast<std::uint8_t>(axis));
        const auto info = configuration(device);
        require(info.size == sizeof(protocol::InputAbsoluteInfo));
        require(info.value.absolute.minimum == 0);
        require(info.value.absolute.maximum == 32767);
    }
    // Android reads the slot protocol only when the slot axis has a maximum
    // above zero.
    select(device, protocol::configAbsoluteInfo, protocol::absoluteMtSlot);
    require(configuration(device).value.absolute.maximum == 9);
    select(device, protocol::configAbsoluteInfo, protocol::absoluteMtTrackingId);
    require(configuration(device).value.absolute.maximum == 65535);
    select(device, protocol::configAbsoluteInfo, protocol::absoluteMtToolType);
    require(configuration(device).size == 0);
    select(device, protocol::configAbsoluteInfo, protocol::absoluteX);
    require(configuration(device).size == 0);
}

void anUnpluggedDeviceReportsNoEventAtAll()
{
    for (const auto kind : {DeviceKind::hardwareKeyboard, DeviceKind::mouse,
                            DeviceKind::pointer}) {
        auto events = pluggableQueue();
        VirtioInputDevice device(kind, events);
        require(!events->connected());
        for (std::uint16_t type = 0; type < 0x20; ++type) {
            select(device, protocol::configEventBits,
                   static_cast<std::uint8_t>(type));
            require(configuration(device).size == 0);
        }
        select(device, protocol::configPropertyBits, 0);
        require(configuration(device).size == 0);
        for (const std::uint16_t axis :
             {protocol::absoluteX, protocol::absoluteY,
              protocol::absoluteMtSlot, protocol::absoluteMtPositionX,
              protocol::absoluteMtPositionY, protocol::absoluteMtTrackingId}) {
            select(device, protocol::configAbsoluteInfo,
                   static_cast<std::uint8_t>(axis));
            require(configuration(device).size == 0);
        }
        // Plugged, it has events; unplugged again, it has none.
        (void)events->setConnected(true);
        select(device, protocol::configEventBits, protocol::eventKey);
        require(configuration(device).size != 0);
        (void)events->setConnected(false);
        require(configuration(device).size == 0);
    }
}

void theSideKeysAndTheTouchscreenAreAlwaysPluggedIn()
{
    for (const auto kind : {DeviceKind::touchscreen, DeviceKind::keyboard}) {
        auto events = std::make_shared<EventQueue>();
        VirtioInputDevice device(kind, events);
        require(events->connected() && events->registered());
        select(device, protocol::configEventBits, protocol::eventKey);
        const auto before = configuration(device);
        require(before.size != 0);
        require(!events->setConnected(false));
        require(!device.configChangePending());
        require(events->connected());
        select(device, protocol::configEventBits, protocol::eventKey);
        const auto after = configuration(device);
        require(after.size == before.size);
        require(std::memcmp(
            &after.value, &before.value, before.size) == 0);
        // And it needs no registration for its events to go.
        const protocol::InputEvent sent {protocol::eventKey, 116, 1};
        events->enqueue(std::span {&sent, 1U});
        protocol::InputEvent taken {};
        require(events->take(taken) && taken.code == 116);
    }
}

bool readable(const VirtioInputDevice& device)
{
    pollfd readiness {
        .fd = device.eventDescriptor(),
        .events = POLLIN,
        .revents = 0,
    };
    return ::poll(&readiness, 1, 0) == 1;
}

void aPlugAndAnUnplugEachRaiseOneConfigurationChange()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::hardwareKeyboard, events);
    require(!device.configChangePending());
    require(!device.claimConfigChange());

    require(events->setConnected(true));
    require(device.configChangePending());
    // The transport wakes on the device's descriptor and signals the change.
    require(readable(device));
    device.drainEventSource();
    require(!readable(device));
    require(device.claimConfigChange());
    device.clearConfigChange();
    require(!device.configChangePending());

    // Plugging a plugged device changes nothing.
    require(!events->setConnected(true));
    require(!device.configChangePending() && !readable(device));

    require(events->setConnected(false));
    require(device.configChangePending() && readable(device));
    device.drainEventSource();
    require(device.claimConfigChange());
    device.clearConfigChange();
    require(!device.configChangePending());
    require(!events->setConnected(false));
    require(!device.configChangePending());
}

void aChangeThatComesWhileOneIsBeingSentIsNotLost()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::mouse, events);
    require(events->setConnected(true));
    require(device.claimConfigChange());
    // The guest is told, and before it answers the device is unplugged.
    require(events->setConnected(false));
    device.clearConfigChange();
    require(device.configChangePending());
    require(device.claimConfigChange());
    device.clearConfigChange();
    require(!device.configChangePending());

    // A claim that is not acknowledged leaves the change pending, for the
    // transport to send again.
    require(events->setConnected(true));
    require(device.claimConfigChange());
    require(device.configChangePending());
}

std::vector<Event> everythingTheGuestReceives(VirtioInputDevice& device)
{
    std::vector<Event> received;
    for (;;) {
        const auto result =
            device.processQueue(protocol::eventQueue, 5, {}, 8);
        if (result.action != uml::virtio::VhostUserQueueAction::complete)
            return received;
        received.push_back(
            uml::testing::loadExact<protocol::InputEvent>(result.response));
    }
}

void status(VirtioInputDevice& device, Event sent)
{
    const auto result = device.processQueue(
        protocol::statusQueue, 7, uml::virtio::toBytes(sent), 0);
    require(result.action == uml::virtio::VhostUserQueueAction::complete);
    require(result.response.empty());
}

void guestRegisters(VirtioInputDevice& device, bool registered = true)
{
    status(device, event(
        protocol::statusNotice, protocol::noticeRegistration,
        registered ? 1 : 0));
}

void theStatusQueueHandsLedWritesAndRegistrationsToTheSink()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::hardwareKeyboard, events);
    std::vector<StatusEvent> seen;
    events->setStatusSink([&](const StatusEvent& one) { seen.push_back(one); });

    status(device, event(protocol::eventLed, protocol::ledCapsLock, 1));
    status(device, event(protocol::eventLed, protocol::ledNumLock, 0));
    status(device, event(protocol::eventLed, protocol::ledMisc, 1));
    require(seen.size() == 3);
    require(seen[0].kind == StatusEvent::Kind::led && seen[0].led == 1 &&
        seen[0].on);
    require(seen[1].kind == StatusEvent::Kind::led && seen[1].led == 0 &&
        !seen[1].on);
    require(seen[2].led == protocol::ledMisc && seen[2].on);

    // A sound is neither an LED nor a notice, and so is a notice of another
    // code; neither reaches the sink.
    status(device, event(0x12, 2, 1));
    status(device, event(protocol::statusNotice, 1, 1));
    require(seen.size() == 3);

    // A registration the host did not ask for, while unplugged, is ignored;
    // the unregistration that answers an unplug is not, whether or not the
    // device is plugged, and no other value is either.
    guestRegisters(device);
    require(seen.size() == 3 && !events->registered());
    guestRegisters(device, false);
    require(seen.size() == 4 && seen[3].kind == StatusEvent::Kind::registration &&
        !seen[3].on && !events->registered());
    status(device, event(
        protocol::statusNotice, protocol::noticeRegistration, 2));
    status(device, event(
        protocol::statusNotice, protocol::noticeRegistration, -1));
    require(seen.size() == 4);
    seen.resize(3);
    (void)events->setConnected(true);
    // A value that is not 0 or 1 is no registration even when plugged.
    status(device, event(
        protocol::statusNotice, protocol::noticeRegistration, 2));
    require(seen.size() == 3 && !events->registered());
    guestRegisters(device);
    require(seen.size() == 4);
    require(seen[3].kind == StatusEvent::Kind::registration && seen[3].on);
    require(events->registered());
    guestRegisters(device, false);
    require(seen.size() == 5 && !seen[4].on && !events->registered());

    // A buffer that is not one event is refused, as before.
    bool refused = false;
    try {
        (void)device.processQueue(protocol::statusQueue, 8, {}, 0);
    } catch (const std::exception&) {
        refused = true;
    }
    require(refused);

    // Cleared, the sink hears nothing.
    events->setStatusSink({});
    status(device, event(protocol::eventLed, protocol::ledCapsLock, 1));
    require(seen.size() == 5);
}

Event key(std::uint16_t code, int value)
{
    return event(protocol::eventKey, code, value);
}

void aPluggedDeviceHoldsItsFirstEventsUntilTheGuestRegisters()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::hardwareKeyboard, events);
    const std::array first {key(30, 1), endOfReport()};
    const std::array second {key(30, 0), endOfReport()};

    // Unplugged, input is dropped, and plugging does not bring it back.
    events->enqueue(first);
    require(everythingTheGuestReceives(device).empty());
    require(events->setConnected(true));
    require(everythingTheGuestReceives(device).empty());

    // Plugged and not yet registered, it waits.
    events->enqueue(first);
    events->enqueue(second);
    require(everythingTheGuestReceives(device).empty());

    guestRegisters(device);
    const auto received = everythingTheGuestReceives(device);
    require(received.size() == 4);
    require(equal(received[0], first[0]) && equal(received[1], first[1]));
    require(equal(received[2], second[0]) && equal(received[3], second[1]));

    // From then on events go straight through.
    events->enqueue(first);
    const auto direct = everythingTheGuestReceives(device);
    require(direct.size() == 2 && equal(direct[0], first[0]));
}

void aRegistrationWakesTheTransportToDeliverWhatWasHeld()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::hardwareKeyboard, events);
    require(events->setConnected(true));
    device.drainEventSource();
    const std::array down {key(30, 1), endOfReport()};
    events->enqueue(down);
    require(!readable(device));
    guestRegisters(device);
    require(readable(device));
    device.drainEventSource();
    require(everythingTheGuestReceives(device).size() == 2);
}

void aGuestThatNeverRegistersGetsNothingAndCostsABoundedBuffer()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::pointer, events);
    require(events->setConnected(true));

    // A report of four events, many times over the bound, with a button each
    // so that no report replaces the one before it.
    const auto report = [](std::int32_t button) {
        return std::array {
            event(protocol::eventAbsolute, protocol::absoluteMtSlot, 0),
            event(protocol::eventAbsolute, protocol::absoluteMtPositionX, 5),
            event(protocol::eventKey, protocol::buttonLeft, button),
            endOfReport(),
        };
    };
    const unsigned reports = EventQueue::heldEventLimit / 4U * 4U;
    bool lost = false;
    for (unsigned index = 0; index < reports; ++index) {
        const auto one = report(static_cast<std::int32_t>(index % 2U));
        lost = events->enqueue(one);
        // Input is lost exactly when it does not fit, from then on.
        require(lost == (index >= EventQueue::heldEventLimit / 4U));
    }
    require(everythingTheGuestReceives(device).empty());
    require(events->connected() && !events->registered());

    // Registration finally comes: nothing that was held is delivered, and
    // what follows passes.
    guestRegisters(device);
    require(everythingTheGuestReceives(device).empty());
    require(!events->enqueue(report(1)));
    require(everythingTheGuestReceives(device).size() == 4);
    // The next plug starts over.
    require(events->setConnected(false) && events->setConnected(true));
    require(!events->enqueue(report(1)));
    guestRegisters(device);
    require(everythingTheGuestReceives(device).size() == 4);
}

void theHeldBufferTakesExactlyTheBoundAndNoMore()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::hardwareKeyboard, events);
    require(events->setConnected(true));
    const std::array one {key(30, 1)};
    for (std::size_t index = 0; index < EventQueue::heldEventLimit; ++index)
        require(!events->enqueue(one));
    guestRegisters(device);
    require(everythingTheGuestReceives(device).size() ==
        EventQueue::heldEventLimit);

    require(events->setConnected(false) && events->setConnected(true));
    for (std::size_t index = 0; index < EventQueue::heldEventLimit; ++index)
        require(!events->enqueue(one));
    // One more than the bound: it and everything held are lost, and so is
    // what follows, until the registration.
    require(events->enqueue(one));
    require(events->enqueue(one));
    guestRegisters(device);
    require(everythingTheGuestReceives(device).empty());
    require(!events->enqueue(one));
    require(everythingTheGuestReceives(device).size() == 1);
}

void anUnplugDropsWhatWaitedAndAnOldRegistrationDoesNotPlugAgain()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::hardwareKeyboard, events);
    require(events->setConnected(true));
    const std::array down {key(30, 1), endOfReport()};
    require(!events->enqueue(down));
    require(events->setConnected(false));
    // The guest's answer to the plug arrives after the unplug.
    guestRegisters(device);
    require(!events->registered());
    require(everythingTheGuestReceives(device).empty());
    // The next plug waits for its own answer, with nothing left from before:
    // what it delivers is what was sent after it, and not the held press of
    // the plug before.
    require(events->setConnected(true));
    const std::array other {key(31, 1), endOfReport()};
    require(!events->enqueue(other));
    require(everythingTheGuestReceives(device).empty());
    guestRegisters(device);
    const auto received = everythingTheGuestReceives(device);
    require(received.size() == 2 && equal(received[0], other[0]) &&
        equal(received[1], other[1]));
}

void aGuestResetForgetsTheRegistrationAndWhatWasQueued()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::hardwareKeyboard, events);
    require(events->setConnected(true));
    guestRegisters(device);
    const std::array down {key(30, 1), endOfReport()};
    events->enqueue(down);
    device.reset();
    require(everythingTheGuestReceives(device).empty());
    require(events->connected() && !events->registered());
    // The device is still plugged in: the guest registers again, and the
    // events sent meanwhile wait for it.
    events->enqueue(down);
    guestRegisters(device);
    require(everythingTheGuestReceives(device).size() == 2);
}

void anUnplugDeliversTheGuestItsFinalEventsFirst()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::hardwareKeyboard, events);
    require(events->setConnected(true));
    guestRegisters(device);
    const std::array down {key(30, 1), endOfReport()};
    events->enqueue(down);
    const std::array release {key(30, 0), endOfReport()};
    events->disconnect(release);
    require(!events->connected() && device.configChangePending());
    const auto received = everythingTheGuestReceives(device);
    require(received.size() == 4);
    require(equal(received[2], release[0]) && equal(received[3], release[1]));
    // Nothing more is taken once it is gone.
    events->enqueue(down);
    require(everythingTheGuestReceives(device).empty());
    // Disconnecting an unplugged device is nothing.
    require(device.claimConfigChange());
    device.clearConfigChange();
    events->disconnect(release);
    require(!device.configChangePending());
    // A device the guest never registered has nothing to release, and is
    // unplugged all the same.
    require(events->setConnected(true));
    events->disconnect(release);
    require(!events->connected() && device.configChangePending());
    require(everythingTheGuestReceives(device).empty());
}


// ---- A plug before the guest's driver has probed.

// Whether the device reports any event type at all.
bool advertisesAnything(VirtioInputDevice& device)
{
    for (std::uint16_t type = 0; type < 0x20; ++type) {
        select(device, protocol::configEventBits,
               static_cast<std::uint8_t>(type));
        if (configuration(device).size != 0)
            return true;
    }
    return false;
}

// The driver's first event buffer.
void postsAnEventBuffer(VirtioInputDevice& device)
{
    const auto result = device.processQueue(protocol::eventQueue, 11, {}, 8);
    require(result.action != uml::virtio::VhostUserQueueAction::complete);
}

void aPlugBeforeTheDriverProbesIsAdvertisedOnlyAfterItsFirstBuffer()
{
    for (const auto kind : {DeviceKind::hardwareKeyboard, DeviceKind::mouse,
                            DeviceKind::pointer}) {
        auto events = unprobedQueue();
        VirtioInputDevice device(kind, events);
        // The application plugs the device before the guest's kernel runs.
        require(events->setConnected(true));
        require(events->connected() && !events->advertised());
        // The driver has nothing to be told yet, and the probe reads an input
        // device with no events.
        require(!device.configChangePending());
        require(!advertisesAnything(device));
        // Its first buffer finds the device plugged: one change, and from
        // then on the device answers what it has.
        postsAnEventBuffer(device);
        require(events->advertised() && device.configChangePending());
        require(advertisesAnything(device));
        require(device.claimConfigChange());
        device.clearConfigChange();
        require(!device.configChangePending());
        postsAnEventBuffer(device);
        require(!device.configChangePending());

        // A plug that is withdrawn before the driver probes is never told.
        auto other = unprobedQueue();
        VirtioInputDevice second(kind, other);
        require(other->setConnected(true) && other->setConnected(false));
        postsAnEventBuffer(second);
        require(!second.configChangePending() && !other->advertised());
        // A device that is not plugged when the driver probes is told of its
        // plug, once, when it comes.
        require(other->setConnected(true) && second.configChangePending());
        require(advertisesAnything(second));
    }
}

void aKernelWithoutTheTopicNeverPlugsADeviceThatWasPluggedBeforeItsProbe()
{
    auto events = unprobedQueue();
    VirtioInputDevice device(DeviceKind::hardwareKeyboard, events);
    std::vector<StatusEvent> seen;
    events->setStatusSink([&](const StatusEvent& one) { seen.push_back(one); });
    require(events->setConnected(true));
    // The stock driver reads no events, registers an input device without
    // any, posts its buffers, and ignores the configuration change.
    require(!advertisesAnything(device));
    postsAnEventBuffer(device);
    // Nothing the host sends reaches it, and the buffer that holds it is
    // bounded.
    const std::array down {key(30, 1), endOfReport()};
    const std::array up {key(30, 0), endOfReport()};
    bool lost = false;
    for (unsigned round = 0; round < EventQueue::heldEventLimit; ++round) {
        lost |= events->enqueue(down);
        lost |= events->enqueue(up);
    }
    require(lost);
    require(everythingTheGuestReceives(device).empty());
    require(!events->registered() && seen.empty());
    // The host unplugs it when the application asks, as for any device, and
    // sends no more.
    require(events->setConnected(false));
    require(!events->enqueue(down));
}

void aKernelWithTheTopicRegistersAfterAPlugBeforeItsProbe()
{
    auto events = unprobedQueue();
    VirtioInputDevice device(DeviceKind::hardwareKeyboard, events);
    std::vector<StatusEvent> seen;
    events->setStatusSink([&](const StatusEvent& one) { seen.push_back(one); });
    require(events->setConnected(true));
    // The probe reads no events and registers nothing; the first buffer
    // raises the change, which the transport sends.
    require(!advertisesAnything(device));
    const std::array down {key(30, 1), endOfReport()};
    require(!events->enqueue(down));
    postsAnEventBuffer(device);
    require(device.claimConfigChange());
    device.clearConfigChange();
    // The driver reads the configuration again and registers what it finds.
    select(device, protocol::configEventBits, protocol::eventKey);
    require(configuration(device).size != 0);
    guestRegisters(device);
    require(seen.size() == 1 && seen[0].on && events->registered());
    const auto received = everythingTheGuestReceives(device);
    require(received.size() == 2 && equal(received[0], down[0]));
}

void aGuestResetMakesTheDriverProbeAgain()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::hardwareKeyboard, events);
    require(events->setConnected(true));
    require(advertisesAnything(device));
    require(device.claimConfigChange());
    device.clearConfigChange();

    device.reset();
    require(events->connected() && !events->advertised());
    require(!advertisesAnything(device));
    require(!device.configChangePending());
    postsAnEventBuffer(device);
    require(device.configChangePending() && advertisesAnything(device));
}

void theHoldKeepsTheLatestPointerPositionAndNoRelativeMotion()
{
    // The absolute pointer: a contact, motion over and over, then a click and
    // more motion. A hold that appended motion would be full and drop the
    // click.
    {
        auto events = pluggableQueue();
        VirtioInputDevice device(DeviceKind::pointer, events);
        require(events->setConnected(true));
        const auto move = [](std::int32_t x) {
            return std::array {
                event(protocol::eventAbsolute, protocol::absoluteMtSlot, 0),
                event(protocol::eventAbsolute,
                      protocol::absoluteMtPositionX, x),
                event(protocol::eventAbsolute,
                      protocol::absoluteMtPositionY, x + 1),
                endOfReport(),
            };
        };
        const std::array enter {
            event(protocol::eventKey, protocol::buttonToolMouse, 1),
            event(protocol::eventKey, protocol::buttonTouch, 1),
            event(protocol::eventAbsolute, protocol::absoluteMtSlot, 0),
            event(protocol::eventAbsolute, protocol::absoluteMtTrackingId, 0),
            event(protocol::eventAbsolute, protocol::absoluteMtPositionX, 1),
            event(protocol::eventAbsolute, protocol::absoluteMtPositionY, 2),
            endOfReport(),
        };
        const std::array click {
            event(protocol::eventKey, protocol::buttonLeft, 1),
            endOfReport(),
        };
        require(!events->enqueue(enter));
        for (std::int32_t x = 10; x < 1000; ++x)
            require(!events->enqueue(move(x)));
        require(!events->enqueue(click));
        for (std::int32_t x = 2000; x < 3000; ++x)
            require(!events->enqueue(move(x)));
        guestRegisters(device);
        const auto received = everythingTheGuestReceives(device);
        const auto last = move(2999);
        const auto before = move(999);
        std::vector<Event> expected(enter.begin(), enter.end());
        expected.insert(expected.end(), before.begin(), before.end());
        expected.insert(expected.end(), click.begin(), click.end());
        expected.insert(expected.end(), last.begin(), last.end());
        require(received.size() == expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index)
            require(equal(received[index], expected[index]));
    }
    // The relative mouse: motion has no position to keep, so it is not held,
    // and a report that carries a button as well is kept whole.
    {
        auto events = pluggableQueue();
        VirtioInputDevice device(DeviceKind::mouse, events);
        require(events->setConnected(true));
        const std::array motion {
            event(protocol::eventRelative, protocol::relativeX, 3),
            event(protocol::eventRelative, protocol::relativeY, 4),
            endOfReport(),
        };
        const std::array down {
            event(protocol::eventKey, protocol::buttonLeft, 1),
            endOfReport(),
        };
        const std::array both {
            event(protocol::eventRelative, protocol::relativeX, 1),
            event(protocol::eventKey, protocol::buttonLeft, 0),
            endOfReport(),
        };
        for (unsigned index = 0; index < 1000; ++index)
            require(!events->enqueue(motion));
        require(!events->enqueue(down));
        for (unsigned index = 0; index < 1000; ++index)
            require(!events->enqueue(motion));
        require(!events->enqueue(both));
        guestRegisters(device);
        const auto received = everythingTheGuestReceives(device);
        require(received.size() == 5);
        require(equal(received[0], down[0]) && equal(received[1], down[1]));
        require(equal(received[2], both[0]) && equal(received[3], both[1]) &&
            equal(received[4], both[2]));
    }
}

void nothingButAPlugAnUnplugOrAFirstBufferRaisesAConfigurationChange()
{
    auto events = pluggableQueue();
    VirtioInputDevice device(DeviceKind::hardwareKeyboard, events);
    require(events->setConnected(true));
    require(device.claimConfigChange());
    device.clearConfigChange();
    const auto none = [&] { return !device.configChangePending(); };

    const std::array down {key(30, 1), endOfReport()};
    require(!events->enqueue(down));
    require(none());
    status(device, event(protocol::eventLed, protocol::ledCapsLock, 1));
    require(none());
    guestRegisters(device);
    require(none());
    require(!events->enqueue(down));
    require(none());
    (void)everythingTheGuestReceives(device);
    require(none());
    guestRegisters(device, false);
    require(none());
    guestRegisters(device);
    require(none());
    events->clear();
    require(none());
    device.reset();
    require(none());
}

} // namespace

int main()
{
    touchscreenConfigurationIsDirectWithAFixedRange();
    multipleContactsMoveAndLiftIndependently();
    tenContactsAndTrackingIdWrapKeepLiveIdsUnique();
    liftingAtTheViewportEdgeDeliversTheFinalMovementBeforeUp();
    aNewContactIsAFingerBeforeItsTrackingId();
    cancellingAContactIsAPalmReportThenItsLift();
    cancellingAMovedContactSynthesizesNoMotion();
    cancellingOneOfTwoContactsKeepsTheOtherDown();
    cancellingASecondaryContactLeavesTheSingleTouchCoordinates();
    cancellingAnInactiveOrInvalidSlotReportsNothing();
    aSlotIsAFingerAgainAfterACancel();
    everyContactSlotCanBeCancelled();
    aReportFitsItsBuffer();
    aCancelReachesTheGuestInOrderAsTwoReports();
    keyboardAdvertisesEvdevKeysAndLeds();
    keyboardAdvertisesBackAndHomeAndNothingElseAbove127();
    keyboardDeliversBackAndHomeAsKeyEvents();
    sideKeysReportALensCoverSwitchOnlyWhenTheHostStatesOne();
    aSwitchIsReportedAheadOfTheKeysAndOnlyForAFixedDevice();
    eventQueueDefersAndThenReturnsOneWireEvent();
    everyKindKeepsItsPermanentIdentity();
    hardwareKeyboardAdvertisesExactlyTheTablesKeys();
    mouseAdvertisesRelativeAxesWheelsButtonsAndTheCaptureLed();
    pointerAdvertisesAnAbsoluteMouseAndNoInputProperty();
    anUnpluggedDeviceReportsNoEventAtAll();
    theSideKeysAndTheTouchscreenAreAlwaysPluggedIn();
    aPlugAndAnUnplugEachRaiseOneConfigurationChange();
    aChangeThatComesWhileOneIsBeingSentIsNotLost();
    theStatusQueueHandsLedWritesAndRegistrationsToTheSink();
    aPluggedDeviceHoldsItsFirstEventsUntilTheGuestRegisters();
    aRegistrationWakesTheTransportToDeliverWhatWasHeld();
    aGuestThatNeverRegistersGetsNothingAndCostsABoundedBuffer();
    theHeldBufferTakesExactlyTheBoundAndNoMore();
    anUnplugDropsWhatWaitedAndAnOldRegistrationDoesNotPlugAgain();
    aGuestResetForgetsTheRegistrationAndWhatWasQueued();
    anUnplugDeliversTheGuestItsFinalEventsFirst();
    aPlugBeforeTheDriverProbesIsAdvertisedOnlyAfterItsFirstBuffer();
    aKernelWithoutTheTopicNeverPlugsADeviceThatWasPluggedBeforeItsProbe();
    aKernelWithTheTopicRegistersAfterAPlugBeforeItsProbe();
    aGuestResetMakesTheDriverProbeAgain();
    theHoldKeepsTheLatestPointerPositionAndNoRelativeMotion();
    nothingButAPlugAnUnplugOrAFirstBufferRaisesAConfigurationChange();
    return EXIT_SUCCESS;
}

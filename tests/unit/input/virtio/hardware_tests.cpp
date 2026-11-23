// SPDX-License-Identifier: GPL-2.0-or-later

#include "hardware_input_run.hpp"
#include "hardware_test_support.hpp"
#include "hid_keyboard.hpp"
#include "registration_summary.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace uml::input::virtio;
using namespace uml::input::virtio::testing;

// ---- The report builders.

void aKeyGoesDownOnceAndUpOnce()
{
    HardwareKeyboard keyboard;
    const auto down = keyboard.key(30, true);
    require(sameEvents(down, {event(key, 30, 1), syncEvent()}));
    require(down.wakes);
    require(keyboard.holds());
    // A second press of a key that is down is not a repeat.
    require(keyboard.key(30, true).empty());
    const auto up = keyboard.key(30, false);
    require(sameEvents(up, {event(key, 30, 0), syncEvent()}));
    require(!up.wakes);
    require(!keyboard.holds());
    // A release of a key that is not down is nothing.
    require(keyboard.key(30, false).empty());
    require(keyboard.key(48, false).empty());
}

void aKeyTheTableCannotProduceIsDropped()
{
    HardwareKeyboard keyboard;
    for (const std::uint16_t code : std::initializer_list<std::uint16_t> {
             0, 240, 195, 255, 256, 0x110, 0xffff}) {
        require(keyboard.key(code, true).empty());
        require(!keyboard.holds());
    }
}

void releaseAllLetsGoOfEveryKeyInOneReport()
{
    HardwareKeyboard keyboard;
    require(keyboard.releaseAll().empty());
    (void)keyboard.key(125, true); // Meta
    (void)keyboard.key(30, true);
    (void)keyboard.key(194, true);
    (void)keyboard.key(30, true);
    const auto release = keyboard.releaseAll();
    require(sameEvents(release, {
        event(key, 30, 0), event(key, 125, 0), event(key, 194, 0), syncEvent()}));
    require(!release.wakes && !keyboard.holds());
    require(keyboard.releaseAll().empty());
    // What was released can go down again.
    require(keyboard.key(30, true).wakes);

    keyboard.reset();
    require(!keyboard.holds() && keyboard.key(30, false).empty());
}

void aMouseMovesAndScrollsAndPressesButtons()
{
    Mouse mouse;
    require(mouse.update(0, 0, 0, 0, 0).empty());
    const auto moved = mouse.update(5, -3, 0, 0, 0);
    require(sameEvents(moved, {
        event(protocol::eventRelative, protocol::relativeX, 5),
        event(protocol::eventRelative, protocol::relativeY, -3), syncEvent()}));
    require(!moved.wakes);
    // One axis moves alone.
    require(sameEvents(mouse.update(0, 7, 0, 0, 0), {
        event(protocol::eventRelative, protocol::relativeY, 7), syncEvent()}));

    const auto press = mouse.update(0, 0, 1U, 0, 0);
    require(sameEvents(press, {event(key, protocol::buttonLeft, 1), syncEvent()}));
    require(press.wakes && mouse.holds());
    // The held button is not sent again, and a second one is an event.
    require(mouse.update(0, 0, 1U, 0, 0).empty());
    require(sameEvents(mouse.update(1, 0, 0b10011, 0, 0), {
        event(protocol::eventRelative, protocol::relativeX, 1),
        event(key, protocol::buttonRight, 1),
        event(key, protocol::buttonExtra, 1), syncEvent()}));
    // A bit past the fifth button is no button.
    require(mouse.update(0, 0, 0b10011 | 0b100000, 0, 0).empty());
    const auto release = mouse.update(0, 0, 0b10, 0, 0);
    require(sameEvents(release, {
        event(key, protocol::buttonLeft, 0),
        event(key, protocol::buttonExtra, 0), syncEvent()}));
    require(!release.wakes);
    require(sameEvents(mouse.releaseAll(), {
        event(key, protocol::buttonRight, 0), syncEvent()}));
    require(!mouse.holds() && mouse.releaseAll().empty());
}

void theWheelsReportHighResolutionUnitsAndDeriveTheSteps()
{
    constexpr auto high = protocol::relativeWheelHighResolution;
    constexpr auto low = protocol::relativeWheel;
    constexpr auto rel = protocol::eventRelative;
    Mouse mouse;
    // A whole detent: the high-resolution event, then the step.
    const auto detent = mouse.update(0, 0, 0, 120, 0);
    require(sameEvents(detent, {
        event(rel, high, 120), event(rel, low, 1), syncEvent()}));
    require(detent.wakes);
    // Half a detent is no step, and the other half completes it.
    const auto half = mouse.update(0, 0, 0, 60, 0);
    require(sameEvents(half, {event(rel, high, 60), syncEvent()}) && !half.wakes);
    const auto rest = mouse.update(0, 0, 0, 60, 0);
    require(sameEvents(rest, {
        event(rel, high, 60), event(rel, low, 1), syncEvent()}) && rest.wakes);
    // Down is negative, and several detents at once are one step event.
    require(sameEvents(mouse.update(0, 0, 0, -360, 0), {
        event(rel, high, -360), event(rel, low, -3), syncEvent()}));
    // A turn the other way cancels the remainder before it steps.
    require(sameEvents(mouse.update(0, 0, 0, 100, 0), {
        event(rel, high, 100), syncEvent()}));
    require(sameEvents(mouse.update(0, 0, 0, -150, 0), {
        event(rel, high, -150), syncEvent()}));
    require(sameEvents(mouse.update(0, 0, 0, -70, 0), {
        event(rel, high, -70), event(rel, low, -1), syncEvent()}));

    // The horizontal wheel has its own codes and its own remainder.
    Mouse sideways;
    require(sameEvents(sideways.update(0, 0, 0, 60, 90), {
        event(rel, high, 60),
        event(rel, protocol::relativeHorizontalWheelHighResolution, 90),
        syncEvent()}));
    require(sameEvents(sideways.update(0, 0, 0, 0, 30), {
        event(rel, protocol::relativeHorizontalWheelHighResolution, 30),
        event(rel, protocol::relativeHorizontalWheel, 1), syncEvent()}));
    // A report with motion, wheels and a button keeps that order.
    Mouse all;
    require(sameEvents(all.update(2, 3, 1U, 120, 120), {
        event(rel, protocol::relativeX, 2), event(rel, protocol::relativeY, 3),
        event(rel, high, 120), event(rel, low, 1),
        event(rel, protocol::relativeHorizontalWheelHighResolution, 120),
        event(rel, protocol::relativeHorizontalWheel, 1),
        event(key, protocol::buttonLeft, 1), syncEvent()}));
    // A reset forgets the remainder.
    all.reset();
    require(sameEvents(all.update(0, 0, 0, 60, 0), {event(rel, high, 60), syncEvent()}));
    all.reset();
    require(all.update(0, 0, 0, 60, 0).events.size() == 2);
}


void thePointerEntersMovesAndLeaves()
{
    Pointer pointer;
    // Out of range with nothing in range is nothing.
    require(pointer.update(10, 20, 0, false).empty());
    require(pointer.leave().empty() && !pointer.inRange());

    // The tool, then the contact, as Android's own absolute mouse test sends
    // them: a hover.
    const auto enter = pointer.update(100, 200, 0, true);
    require(sameEvents(enter, {
        event(key, protocol::buttonToolMouse, 1),
        event(key, protocol::buttonTouch, 1), slot0(), trackingId(0),
        positionX(100), positionY(200), syncEvent()}));
    require(!enter.wakes && pointer.inRange());

    // A move states the slot and the position, and no tracking id.
    require(sameEvents(pointer.update(101, 200, 0, true), {
        slot0(), positionX(101), positionY(200), syncEvent()}));
    require(pointer.update(101, 200, 0, true).empty());

    // A press with the pointer still wakes and states no position.
    const auto press = pointer.update(101, 200, 0b1, true);
    require(sameEvents(press, {event(key, protocol::buttonLeft, 1), syncEvent()}));
    require(press.wakes);
    // Moving with the button down, then pressing a second.
    require(sameEvents(pointer.update(150, 250, 0b11, true), {
        slot0(), positionX(150), positionY(250),
        event(key, protocol::buttonRight, 1), syncEvent()}));
    require(sameEvents(pointer.update(150, 250, 0b10, true), {
        event(key, protocol::buttonLeft, 0), syncEvent()}));

    // Leaving releases what is held, ends the contact and the tool, and
    // keeps the position out of it.
    const auto leave = pointer.update(150, 250, 0b10, false);
    require(sameEvents(leave, {
        event(key, protocol::buttonRight, 0), slot0(), trackingId(-1),
        event(key, protocol::buttonTouch, 0),
        event(key, protocol::buttonToolMouse, 0), syncEvent()}));
    require(!leave.wakes && !pointer.inRange());
    require(pointer.leave().empty());

    // The next entry is a new contact, which may have a button down already.
    const auto again = pointer.update(5, 6, 0b100, true);
    require(sameEvents(again, {
        event(key, protocol::buttonToolMouse, 1),
        event(key, protocol::buttonTouch, 1), slot0(), trackingId(1),
        positionX(5), positionY(6),
        event(key, protocol::buttonMiddle, 1), syncEvent()}));
    require(again.wakes);
    // A coordinate past the range is held at its edge.
    require(sameEvents(pointer.update(40000, 70000, 0b100, true), {
        slot0(), positionX(32767), positionY(32767), syncEvent()}));
    // A button without a contact is nothing.
    pointer.reset();
    require(!pointer.inRange() && pointer.update(0, 0, 0b1, false).empty());
}

void thePointersTrackingIdsStayInTheRangeItAdvertises()
{
    Pointer pointer;
    std::int32_t last = -1;
    for (int round = 0; round < 70000; ++round) {
        const auto enter = pointer.update(1, 1, 0, true);
        std::int32_t id = -2;
        for (const auto& one : enter.events) {
            if (one.type == protocol::eventAbsolute &&
                one.code == protocol::absoluteMtTrackingId)
                id = one.value;
        }
        require(id >= 0 && id <= protocol::touchscreenTrackingIdMaximum);
        require(id != last);
        last = id;
        (void)pointer.leave();
    }
}

void theRawPositionLandsOnTheSamePixelATouchWould()
{
    using uml::input::virtio::pointerRawPosition;
    // No size, and no coordinate outside the range.
    require(!pointerRawPosition(0, 0, 0, 1080));
    require(!pointerRawPosition(0, 0, 1920, 0));
    require(!pointerRawPosition(32768, 0, 1920, 1080));
    require(!pointerRawPosition(0, 32768, 1920, 1080));
    // The corners, on the long axis and the short one.
    const auto landscape = pointerRawPosition(32767, 32767, 1920, 1080);
    require(landscape && landscape->x == 32767 && landscape->y == 18431);
    const auto portrait = pointerRawPosition(32767, 32767, 1080, 1920);
    require(portrait && portrait->x == 18431 && portrait->y == 32767);
    const auto square = pointerRawPosition(12345, 23456, 1000, 1000);
    require(square && square->x == 12345 && square->y == 23456);
    const auto origin = pointerRawPosition(0, 0, 1920, 1080);
    require(origin && origin->x == 0 && origin->y == 0);

    // Android maps an absolute mouse's raw range onto the display with one
    // scale, max(width, height) / 32768 pixels per raw unit, and a touchscreen
    // 's onto each axis. For the centre of every pixel, the pixel Android puts
    // the cursor on is the pixel a touch there would touch.
    struct Mode {
        std::uint32_t width;
        std::uint32_t height;
    };
    for (const Mode mode : {Mode {1920, 1080}, Mode {1080, 1920},
                            Mode {1280, 720}, Mode {800, 1280},
                            Mode {1000, 1000}, Mode {2560, 1600},
                            Mode {3840, 2160}, Mode {320, 240}}) {
        const std::uint32_t larger = std::max(mode.width, mode.height);
        const auto touch = [](std::uint32_t pixel, std::uint32_t extent) {
            // The touchscreen coordinate of a pixel's centre, which Android
            // maps back to that pixel.
            return static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(2U * pixel + 1U) * 32768U) /
                (2U * extent));
        };
        const auto android = [larger](std::uint32_t raw) {
            return static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(raw) * larger / 32768U);
        };
        for (std::uint32_t py = 0; py < mode.height; ++py) {
            const auto ty = touch(py, mode.height);
            for (std::uint32_t px = 0; px < mode.width; ++px) {
                const auto tx = touch(px, mode.width);
                const auto raw = pointerRawPosition(
                    tx, ty, mode.width, mode.height);
                require(raw.has_value());
                require(raw->x <= 32767 && raw->y <= 32767);
                require(android(raw->x) == px);
                require(android(raw->y) == py);
                // Skip rows to keep the check quick on the big modes.
                if (mode.width > 1000 && px > 64)
                    px += 31;
            }
            if (mode.height > 1000)
                py += 15;
        }
    }
}


void theKeyboardAndTheMouseArePluggedSeparately()
{
    Rig rig;
    require(!rig.input.keyboardConnected() && !rig.input.mouseConnected());
    rig.input.setKeyboardConnected(true);
    require(rig.input.keyboardConnected() && !rig.input.mouseConnected());
    require(rig.keyboardDevice.configChangePending());
    require(!rig.mouseDevice.configChangePending());
    require(!rig.pointerDevice.configChangePending());

    // The mouse and the pointer are one mouse to Android, so they are plugged
    // together, one configuration change each.
    rig.input.setMouseConnected(true);
    require(rig.input.mouseConnected());
    require(rig.mouseDevice.configChangePending());
    require(rig.pointerDevice.configChangePending());
    require(rig.mouseQueue->connected() && rig.pointerQueue->connected());

    // Plugging a plugged device changes nothing.
    for (auto* device : {&rig.keyboardDevice, &rig.mouseDevice,
                         &rig.pointerDevice}) {
        require(device->claimConfigChange());
        device->clearConfigChange();
    }
    rig.input.setKeyboardConnected(true);
    rig.input.setMouseConnected(true);
    require(!rig.keyboardDevice.configChangePending());
    require(!rig.mouseDevice.configChangePending());
    require(!rig.pointerDevice.configChangePending());

    rig.input.setMouseConnected(false);
    require(!rig.input.mouseConnected() && rig.input.keyboardConnected());
    require(rig.mouseDevice.configChangePending());
    require(rig.pointerDevice.configChangePending());
    require(!rig.keyboardDevice.configChangePending());
}

void inputToAnUnpluggedDeviceIsDroppedWithoutBeingRemembered()
{
    Rig rig;
    require(!rig.input.key(30, true));
    require(!rig.input.keyUsage(0x04, true));
    require(!rig.input.pointer(10, 10, 1, true, 1920, 1080));
    require(!rig.input.mouse(1, 1, 1, 0, 0));
    rig.plugEverything();
    require(Rig::received(rig.keyboardDevice).empty());
    require(Rig::received(rig.pointerDevice).empty());
    require(Rig::received(rig.mouseDevice).empty());
    // The press that was dropped is not held: its release is nothing, and
    // the pointer enters afresh.
    require(!rig.input.key(30, false));
    require(Rig::received(rig.keyboardDevice).empty());
    require(!rig.input.pointer(10, 10, 0, true, 1920, 1080));
    const auto pointer = Rig::received(rig.pointerDevice);
    require(pointer.size() == 7 &&
        pointer[0].code == protocol::buttonToolMouse && pointer[0].value == 1);
}

void keysArriveAsLinuxCodesAndTheirReturnWakes()
{
    Rig rig;
    rig.plugEverything();
    // A HID usage becomes the Linux key a USB keyboard would produce: A.
    require(rig.input.keyUsage(0x04, true));
    require(!rig.input.keyUsage(0x04, false));
    // Left GUI, which is Android's Meta, and a usage with no key.
    require(rig.input.keyUsage(0xe3, true));
    require(!rig.input.keyUsage(0x82, true));
    require(!rig.input.keyUsage(0x10004, true));
    require(!rig.input.key(0, true));
    const auto keys = Rig::received(rig.keyboardDevice);
    require(sameEvents(keys, {
        event(key, 30, 1), syncEvent(), event(key, 30, 0), syncEvent(),
        event(key, 125, 1), syncEvent()}));
    // The same key held down, then the table's own code through key().
    require(rig.input.key(48, true));
    require(!rig.input.key(48, true));
    require(Rig::received(rig.keyboardDevice).size() == 2);
}

void thePointerIsMappedOntoTheGuestDisplay()
{
    Rig rig;
    rig.plugEverything();
    // No display mode, no position.
    require(!rig.input.pointer(1000, 1000, 0, true, 0, 0));
    require(Rig::received(rig.pointerDevice).empty());
    require(!rig.input.pointer(32767, 32767, 0, true, 1920, 1080));
    const auto enter = Rig::received(rig.pointerDevice);
    require(sameEvents(enter, {
        event(key, protocol::buttonToolMouse, 1),
        event(key, protocol::buttonTouch, 1), slot0(), trackingId(0),
        positionX(32767), positionY(18431), syncEvent()}));
    // A press wakes; out of range ends the contact even with no display.
    require(rig.input.pointer(0, 0, 1, true, 1920, 1080));
    require(!rig.input.pointer(0, 0, 0, false, 0, 0));
    require(sameEvents(Rig::received(rig.pointerDevice), {
        slot0(), positionX(0), positionY(0),
        event(key, protocol::buttonLeft, 1), syncEvent(),
        event(key, protocol::buttonLeft, 0), slot0(), trackingId(-1),
        event(key, protocol::buttonTouch, 0),
        event(key, protocol::buttonToolMouse, 0), syncEvent()}));
}

void theMouseCarriesMotionButtonsAndWheels()
{
    Rig rig;
    rig.plugEverything();
    require(!rig.input.mouse(3, 4, 0, 0, 0));
    require(rig.input.mouse(0, 0, 1, 0, 0));
    require(rig.input.mouse(0, 0, 1, 120, 0));
    require(!rig.input.mouse(0, 0, 1, 30, 0));
    const auto all = Rig::received(rig.mouseDevice);
    require(sameEvents(all, {
        event(protocol::eventRelative, protocol::relativeX, 3),
        event(protocol::eventRelative, protocol::relativeY, 4), syncEvent(),
        event(key, protocol::buttonLeft, 1), syncEvent(),
        event(protocol::eventRelative, protocol::relativeWheelHighResolution, 120),
        event(protocol::eventRelative, protocol::relativeWheel, 1), syncEvent(),
        event(protocol::eventRelative, protocol::relativeWheelHighResolution, 30),
        syncEvent()}));
}

void anUnplugReleasesEverythingTheHostHolds()
{
    Rig rig;
    rig.plugEverything();
    (void)rig.input.key(30, true);
    (void)rig.input.key(48, true);
    (void)rig.input.mouse(0, 0, 0b101, 0, 0);
    (void)rig.input.pointer(100, 100, 0b11, true, 1000, 1000);
    (void)Rig::received(rig.keyboardDevice);
    (void)Rig::received(rig.mouseDevice);
    (void)Rig::received(rig.pointerDevice);
    for (auto* device : {&rig.keyboardDevice, &rig.mouseDevice,
                         &rig.pointerDevice}) {
        require(device->claimConfigChange());
        device->clearConfigChange();
    }

    rig.input.setKeyboardConnected(false);
    require(rig.keyboardDevice.configChangePending());
    // The keyboard is not the mouse: it stays down.
    require(rig.input.mouseConnected());
    require(sameEvents(Rig::received(rig.keyboardDevice), {
        event(key, 30, 0), event(key, 48, 0), syncEvent()}));

    rig.input.setMouseConnected(false);
    require(sameEvents(Rig::received(rig.mouseDevice), {
        event(key, protocol::buttonLeft, 0),
        event(key, protocol::buttonMiddle, 0), syncEvent()}));
    require(sameEvents(Rig::received(rig.pointerDevice), {
        event(key, protocol::buttonLeft, 0),
        event(key, protocol::buttonRight, 0), slot0(), trackingId(-1),
        event(key, protocol::buttonTouch, 0),
        event(key, protocol::buttonToolMouse, 0), syncEvent()}));
    require(rig.mouseDevice.configChangePending());
    require(rig.pointerDevice.configChangePending());

    // And all of it is gone: nothing is sent, and a new plug starts clean.
    require(!rig.input.key(30, true));
    require(!rig.input.mouse(1, 1, 1, 0, 0));
    rig.input.setKeyboardConnected(true);
    Rig::registers(rig.keyboardDevice);
    require(rig.input.key(30, true));
    require(Rig::received(rig.keyboardDevice).size() == 2);
}

void theGuestsRegistrationsReachTheCallback()
{
    Rig rig;
    rig.plugEverything();
    require((rig.statuses == std::vector<std::pair<HardwareStatus, bool>> {
        {HardwareStatus::keyboardRegistered, true},
        {HardwareStatus::mouseRegistered, true},
        {HardwareStatus::pointerRegistered, true}}));
    rig.statuses.clear();
    Rig::status(rig.pointerDevice, event(
        protocol::statusNotice, protocol::noticeRegistration, 0));
    require((rig.statuses == std::vector<std::pair<HardwareStatus, bool>> {
        {HardwareStatus::pointerRegistered, false}}));
}

void theGuestsLedWritesReachTheCallback()
{
    Rig rig;
    rig.plugEverything();
    rig.statuses.clear();
    Rig::status(rig.keyboardDevice, event(protocol::eventLed, protocol::ledCapsLock, 1));
    Rig::status(rig.keyboardDevice, event(protocol::eventLed, protocol::ledNumLock, 1));
    Rig::status(rig.keyboardDevice, event(protocol::eventLed, protocol::ledScrollLock, 0));
    Rig::status(rig.keyboardDevice, event(protocol::eventLed, protocol::ledMisc, 1));
    Rig::status(rig.mouseDevice, event(protocol::eventLed, protocol::ledMisc, 1));
    Rig::status(rig.mouseDevice, event(protocol::eventLed, protocol::ledMisc, 0));
    Rig::status(rig.mouseDevice, event(protocol::eventLed, protocol::ledCapsLock, 1));
    Rig::status(rig.pointerDevice, event(protocol::eventLed, protocol::ledMisc, 1));
    require(rig.statuses.size() == 5);
    require(rig.statuses[0] == std::pair {HardwareStatus::capsLock, true});
    require(rig.statuses[1] == std::pair {HardwareStatus::numLock, true});
    require(rig.statuses[2] == std::pair {HardwareStatus::scrollLock, false});
    require(rig.statuses[3] == std::pair {HardwareStatus::pointerCapture, true});
    require(rig.statuses[4] == std::pair {HardwareStatus::pointerCapture, false});
    // A cleared callback hears nothing, and the host stays up.
    rig.input.setStatusCallback({});
    Rig::status(rig.mouseDevice, event(protocol::eventLed, protocol::ledMisc, 1));
    require(rig.statuses.size() == 5);
}

void inputTheGuestHasNotRegisteredForIsForgottenAsItIsDropped()
{
    Rig rig;
    rig.input.setMouseConnected(true);
    // The pointer enters and presses, and the guest is slow: more reports than
    // the hold takes are dropped, the enter among them.
    (void)rig.input.pointer(1, 1, 1, true, 1000, 1000);
    for (std::uint32_t step = 0; step < 200; ++step)
        (void)rig.input.pointer(2 + step, 1, step % 2U, true, 1000, 1000);
    Rig::registers(rig.pointerDevice);
    require(Rig::received(rig.pointerDevice).empty());
    // The host knows the guest saw none of it, so the next report is an entry
    // again, not a move that Android would ignore for want of a contact.
    (void)rig.input.pointer(50, 60, 0, true, 1000, 1000);
    const auto next = Rig::received(rig.pointerDevice);
    require(next.size() == 7);
    require(next[0].code == protocol::buttonToolMouse && next[0].value == 1);

    // The keyboard: a key held through the loss is not left down. Its release
    // is nothing, since the guest never saw it go down, and it can go down
    // again.
    rig.input.setKeyboardConnected(true);
    (void)rig.input.key(30, true);
    for (int round = 0; round < 200; ++round) {
        (void)rig.input.key(48, true);
        (void)rig.input.key(48, false);
    }
    Rig::registers(rig.keyboardDevice);
    require(Rig::received(rig.keyboardDevice).empty());
    require(!rig.input.key(30, false));
    require(Rig::received(rig.keyboardDevice).empty());
    require(rig.input.key(30, true));
    require(Rig::received(rig.keyboardDevice).size() == 2);
}

// Whether the guest ends with the key down: the last event for it.
bool keyLeftDown(const Events& stream, std::uint16_t code)
{
    std::int32_t last = 0;
    for (const auto& one : stream) {
        if (one.type == protocol::eventKey && one.code == code)
            last = one.value;
    }
    return last != 0;
}

// The guest registers its keyboard as the host presses and releases a key,
// after the hold overflowed. Whichever comes first, the guest ends with the
// key up: the press was lost and its release is nothing, or both arrive. A
// reset that came after the registration, on the transport's thread, made the
// host forget a press that had already been delivered, and its release was
// never sent. The interleaving is the stress's own, so it runs many rounds.
void aKeyPressedAsTheGuestRegistersIsNeverLeftDown()
{
    Rig rig;
    unsigned stuck = 0;
    for (unsigned round = 0; round < 20000; ++round) {
        rig.input.setKeyboardConnected(true);
        for (int index = 0; index < 200; ++index) {
            (void)rig.input.key(48, true);
            (void)rig.input.key(48, false);
        }
        std::atomic_bool go {false};
        std::thread transport([&] {
            while (!go.load(std::memory_order_acquire)) {
            }
            Rig::registers(rig.keyboardDevice);
        });
        go.store(true, std::memory_order_release);
        (void)rig.input.key(30, true);
        transport.join();
        (void)rig.input.key(30, false);
        if (keyLeftDown(Rig::received(rig.keyboardDevice), 30))
            ++stuck;
        rig.input.setKeyboardConnected(false);
        (void)Rig::received(rig.keyboardDevice);
    }
    require(stuck == 0);
}

// The first click after the guest suspended: the mouse is plugged while the
// guest sleeps, the user moves it for a long while and then clicks. The motion
// does not wake the guest and must not push the click out of the hold.
void aClickAfterALongWaitForTheGuestIsDelivered()
{
    Rig rig;
    rig.input.setMouseConnected(true);
    for (std::uint32_t index = 0; index < 5000; ++index) {
        (void)rig.input.mouse(1, 1, 0, 0, 0);
        require(!rig.input.pointer(index % 1000U, 5, 0, true, 1000, 1000));
    }
    require(rig.input.pointer(10, 10, 1, true, 1000, 1000));
    require(rig.input.mouse(0, 0, 1, 0, 0));
    Rig::registers(rig.pointerDevice);
    Rig::registers(rig.mouseDevice);
    const auto mouse = Rig::received(rig.mouseDevice);
    require(sameEvents(mouse, {event(key, protocol::buttonLeft, 1), syncEvent()}));
    const auto pointer = Rig::received(rig.pointerDevice);
    // The entry, the latest position it moved to, and the press.
    require(pointer.size() == 7 + 4 + 5);
    require(pointer[0].code == protocol::buttonToolMouse);
    require(pointer[pointer.size() - 2].code == protocol::buttonLeft &&
        pointer[pointer.size() - 2].value == 1);
    // And the host still holds the press: letting go reaches the guest.
    require(!rig.input.mouse(0, 0, 0, 0, 0));
    require(sameEvents(Rig::received(rig.mouseDevice), {
        event(key, protocol::buttonLeft, 0), syncEvent()}));
}

// An application plugs the devices before the guest's kernel has probed them,
// as it can at the start of a run or after a restart.
void aPlugBeforeTheGuestProbesIsToldWhenItDoes()
{
    Rig rig(false);
    rig.input.setKeyboardConnected(true);
    rig.input.setMouseConnected(true);
    require(rig.input.keyboardConnected() && rig.input.mouseConnected());
    for (auto* device : {&rig.keyboardDevice, &rig.mouseDevice,
                         &rig.pointerDevice}) {
        require(!device->configChangePending());
        Rig::probes(*device);
        require(device->configChangePending());
    }
    // A kernel with the topic registers after it; what was sent meanwhile is
    // delivered.
    require(rig.input.key(30, true));
    Rig::registers(rig.keyboardDevice);
    require(Rig::received(rig.keyboardDevice).size() == 2);
}

void theUnregistrationThatAnswersAnUnplugReachesTheCallback()
{
    Rig rig;
    rig.plugEverything();
    rig.statuses.clear();
    const auto unregisters = event(
        protocol::statusNotice, protocol::noticeRegistration, 0);
    rig.input.setKeyboardConnected(false);
    Rig::status(rig.keyboardDevice, unregisters);
    require((rig.statuses == std::vector<std::pair<HardwareStatus, bool>> {
        {HardwareStatus::keyboardRegistered, false}}));
    rig.input.setMouseConnected(false);
    Rig::status(rig.mouseDevice, unregisters);
    Rig::status(rig.pointerDevice, unregisters);
    require((rig.statuses == std::vector<std::pair<HardwareStatus, bool>> {
        {HardwareStatus::keyboardRegistered, false},
        {HardwareStatus::mouseRegistered, false},
        {HardwareStatus::pointerRegistered, false}}));
    // The registration that answered the plug before arrives late: it is not
    // news, and the unplugged device stays unregistered.
    rig.statuses.clear();
    Rig::registers(rig.keyboardDevice);
    require(rig.statuses.empty() && !rig.keyboardQueue->registered());
}

// The application hears two registrations: the keyboard, and the pointer once
// the mouse and the absolute pointer both are registered.
void theApplicationHearsTheKeyboardAndThePointerRegister()
{
    using Group = RegistrationSummary::Group;
    using Edge = RegistrationSummary::Edge;
    RegistrationSummary summary;
    // Nothing that is not a registration is a change.
    require(!summary.update(HardwareStatus::capsLock, true));
    require(!summary.update(HardwareStatus::pointerCapture, true));

    require(summary.update(HardwareStatus::keyboardRegistered, true) ==
        Edge {Group::keyboard, true});
    // The same news again is not news.
    require(!summary.update(HardwareStatus::keyboardRegistered, true));
    // One of the pointer's two devices is not the pointer.
    require(!summary.update(HardwareStatus::mouseRegistered, true));
    require(summary.update(HardwareStatus::pointerRegistered, true) ==
        Edge {Group::pointer, true});
    require(!summary.update(HardwareStatus::pointerRegistered, true));
    // Losing either one takes the pointer away, and the other's loss after it
    // changes nothing.
    require(summary.update(HardwareStatus::mouseRegistered, false) ==
        Edge {Group::pointer, false});
    require(!summary.update(HardwareStatus::pointerRegistered, false));
    require(!summary.update(HardwareStatus::mouseRegistered, false));
    require(summary.update(HardwareStatus::keyboardRegistered, false) ==
        Edge {Group::keyboard, false});
}

// A boot's end, whether the guest restarts or the run is over, takes every
// registration away and tells the application so each time, so it never keeps
// one the guest lost.
void aBootsEndTakesTheRegistrationsAway()
{
    using Group = RegistrationSummary::Group;
    using Edge = RegistrationSummary::Edge;
    RegistrationSummary summary;
    (void)summary.update(HardwareStatus::keyboardRegistered, true);
    (void)summary.update(HardwareStatus::mouseRegistered, true);
    (void)summary.update(HardwareStatus::pointerRegistered, true);
    const std::array<Edge, 2> lost {
        Edge {Group::keyboard, false}, Edge {Group::pointer, false}};
    require(summary.reset() == lost);
    // Nothing is remembered: the next boot registers afresh, and a loss
    // reported after the reset is not a change.
    require(summary.reset() == lost);
    require(!summary.update(HardwareStatus::pointerRegistered, false));
    require(summary.update(HardwareStatus::keyboardRegistered, true) ==
        Edge {Group::keyboard, true});
    require(!summary.update(HardwareStatus::pointerRegistered, true));
    require(summary.update(HardwareStatus::mouseRegistered, true) ==
        Edge {Group::pointer, true});
}

// The summary over the real devices: what the three queues announce is what
// the application hears, and a boot's end after it.
void theSummaryFollowsTheDevicesRegistrations()
{
    using Group = RegistrationSummary::Group;
    using Edge = RegistrationSummary::Edge;
    Rig rig;
    RegistrationSummary summary;
    std::vector<Edge> heard;
    rig.input.setStatusCallback([&](HardwareStatus status, bool on) {
        if (const auto edge = summary.update(status, on))
            heard.push_back(*edge);
    });
    rig.plugEverything();
    require((heard == std::vector<Edge> {
        Edge {Group::keyboard, true}, Edge {Group::pointer, true}}));
    heard.clear();
    rig.input.setMouseConnected(false);
    Rig::status(rig.mouseDevice, event(
        protocol::statusNotice, protocol::noticeRegistration, 0));
    require((heard == std::vector<Edge> {Edge {Group::pointer, false}}));
    for (const auto& edge : summary.reset())
        heard.push_back(edge);
    require(heard.size() == 3 && heard[1] == (Edge {Group::keyboard, false}));
}

// One boot's three devices and the input over them, as the runtime builds them:
// unplugged, with the guest's drivers yet to probe.
struct BootRig final {
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
    std::shared_ptr<HardwareInput> input {
        std::make_shared<HardwareInput>(
            keyboardQueue, mouseQueue, pointerQueue)};
};

// What the application asked for outlives the boot it was asked of: a guest
// that restarts itself starts with the devices plugged as it was left.
void aRunKeepsWhatTheApplicationAskedForAcrossBoots()
{
    using Device = HardwareInputRun::Device;
    HardwareInputRun run;
    // Outside a run there is no guest to ask.
    require(!run.request(Device::keyboard, true));
    require(run.current() == nullptr);

    run.begin();
    // Between boots, a request is accepted and kept.
    require(run.request(Device::mouse, true));
    require(run.current() == nullptr);

    BootRig first;
    run.attach(first.input);
    require(run.current() == first.input);
    require(first.input->mouseConnected() && !first.input->keyboardConnected());
    // Nothing the guest has seen changed: its drivers have not probed, and the
    // first buffer of each tells it.
    require(!first.mouseDevice.configChangePending());
    Rig::probes(first.mouseDevice);
    require(first.mouseDevice.configChangePending());

    // While a boot has its input, a request reaches it at once.
    require(run.request(Device::keyboard, true));
    require(first.input->keyboardConnected());
    require(run.request(Device::mouse, false));
    require(!first.input->mouseConnected());
    require(run.request(Device::mouse, true));

    // The guest restarts: the boot's input is gone, a request in between is
    // kept, and the next boot's devices start as the application left them.
    require(run.detach() == first.input);
    require(run.current() == nullptr);
    require(run.request(Device::keyboard, false));
    BootRig second;
    run.attach(second.input);
    require(!second.input->keyboardConnected());
    require(second.input->mouseConnected());
    require(!second.mouseDevice.configChangePending());
    Rig::probes(second.mouseDevice);
    require(second.mouseDevice.configChangePending());
    // The devices the application did not plug stay unplugged and raise nothing.
    Rig::probes(second.keyboardDevice);
    require(!second.keyboardDevice.configChangePending());

    // The run's end forgets the requests, and the next run starts unplugged.
    require(run.detach() == second.input);
    run.end();
    require(!run.request(Device::mouse, false));
    run.begin();
    BootRig third;
    run.attach(third.input);
    require(!third.input->mouseConnected() && !third.input->keyboardConnected());
    (void)run.detach();
    run.end();
}

} // namespace

int main()
{
    aKeyGoesDownOnceAndUpOnce();
    aKeyTheTableCannotProduceIsDropped();
    releaseAllLetsGoOfEveryKeyInOneReport();
    aMouseMovesAndScrollsAndPressesButtons();
    theWheelsReportHighResolutionUnitsAndDeriveTheSteps();
    thePointerEntersMovesAndLeaves();
    thePointersTrackingIdsStayInTheRangeItAdvertises();
    theRawPositionLandsOnTheSamePixelATouchWould();
    theKeyboardAndTheMouseArePluggedSeparately();
    inputToAnUnpluggedDeviceIsDroppedWithoutBeingRemembered();
    keysArriveAsLinuxCodesAndTheirReturnWakes();
    thePointerIsMappedOntoTheGuestDisplay();
    theMouseCarriesMotionButtonsAndWheels();
    anUnplugReleasesEverythingTheHostHolds();
    theGuestsRegistrationsReachTheCallback();
    theGuestsLedWritesReachTheCallback();
    inputTheGuestHasNotRegisteredForIsForgottenAsItIsDropped();
    aKeyPressedAsTheGuestRegistersIsNeverLeftDown();
    aClickAfterALongWaitForTheGuestIsDelivered();
    aPlugBeforeTheGuestProbesIsToldWhenItDoes();
    theUnregistrationThatAnswersAnUnplugReachesTheCallback();
    theApplicationHearsTheKeyboardAndThePointerRegister();
    aBootsEndTakesTheRegistrationsAway();
    theSummaryFollowsTheDevicesRegistrations();
    aRunKeepsWhatTheApplicationAskedForAcrossBoots();
    return EXIT_SUCCESS;
}

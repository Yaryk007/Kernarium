// SPDX-License-Identifier: GPL-2.0-or-later

/* The macOS launcher's --hardware-input: the display's keys and pointer, in
 * the events it sends its touchscreen in the mouse form, become the guest's
 * hardware keyboard, mouse and pointer, plugged by the first key or pointer
 * event and unplugged when the last client goes. */

#include "hardware_input_bridge.hpp"
#include "hardware_test_support.hpp"

#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

namespace {

using namespace uml::input::virtio;
using namespace uml::input::virtio::testing;
using uml::app::HardwareInputBridge;
namespace vnc = uml::input::vnc;
using Raw = vnc::VncInputEvent;

/* The bridge over a rig of the three devices, a guest display of 1920 x 1080
 * and a count of the wakes it asked for. */
struct Fixture final {
    Rig rig;
    HardwareInputBridge bridge;
    std::optional<HardwareInputBridge::ScanoutMode> mode {
        HardwareInputBridge::ScanoutMode {1920, 1080}};
    unsigned wakes {};

    Fixture()
    {
        /* The rig owns its HardwareInput. */
        bridge.attach(std::shared_ptr<HardwareInput>(
            std::shared_ptr<void> {}, &rig.input));
        bridge.set_scanout_mode([this] { return mode; });
        bridge.set_wake([this] { ++wakes; });
    }

    ~Fixture() { bridge.detach(); }
};

Raw raw(std::uint16_t type, std::uint16_t code, std::int32_t value)
{
    return {.type = type, .code = code, .value = value};
}

/* What the display sends for a key. */
std::vector<Raw> keyEvents(std::uint16_t code, bool down)
{
    return {
        raw(vnc::eventKey, code, down ? 1 : 0),
        raw(vnc::eventSync, vnc::syncReport, 0),
    };
}

/* What the display's mouse form sends for a pointer sample: the position, the
 * three buttons (`buttons` has the left in bit 0, the right in bit 1 and the
 * middle in bit 2), and a wheel step if either is nonzero. */
std::vector<Raw> pointerEvents(
    std::int32_t x,
    std::int32_t y,
    unsigned buttons,
    std::int32_t wheel = 0,
    std::int32_t horizontalWheel = 0)
{
    std::vector<Raw> events {
        raw(vnc::eventAbsolute, vnc::absoluteX, x),
        raw(vnc::eventAbsolute, vnc::absoluteY, y),
        raw(vnc::eventKey, vnc::buttonLeft, (buttons & 1U) != 0 ? 1 : 0),
        raw(vnc::eventKey, vnc::buttonMiddle, (buttons & 4U) != 0 ? 1 : 0),
        raw(vnc::eventKey, vnc::buttonRight, (buttons & 2U) != 0 ? 1 : 0),
    };
    if (wheel != 0)
        events.push_back(raw(vnc::eventRelative, vnc::relativeWheel, wheel));
    if (horizontalWheel != 0) {
        events.push_back(raw(
            vnc::eventRelative, vnc::relativeHorizontalWheel,
            horizontalWheel));
    }
    events.push_back(raw(vnc::eventSync, vnc::syncReport, 0));
    return events;
}

void deliver(Fixture& fixture, const std::vector<Raw>& events)
{
    fixture.bridge.deliver(events);
}

void aKeyPlugsOnlyTheKeyboard()
{
    Fixture fixture;
    require(!fixture.rig.input.keyboardConnected());
    deliver(fixture, keyEvents(30, true));
    require(fixture.rig.input.keyboardConnected());
    require(!fixture.rig.input.mouseConnected());
    require(fixture.rig.keyboardDevice.configChangePending());
    require(!fixture.rig.mouseDevice.configChangePending());
    /* The key waits for the guest to register the device. */
    require(Rig::received(fixture.rig.keyboardDevice).empty());
    Rig::registers(fixture.rig.keyboardDevice);
    require(sameEvents(Rig::received(fixture.rig.keyboardDevice), {
        event(key, 30, 1), syncEvent()}));
    require(fixture.wakes == 1);
    /* A release never wakes, and a second key does not plug again. */
    deliver(fixture, keyEvents(30, false));
    require(sameEvents(Rig::received(fixture.rig.keyboardDevice), {
        event(key, 30, 0), syncEvent()}));
    require(fixture.wakes == 1);
}

void aPointerEventPlugsTheMouseAndThePointer()
{
    Fixture fixture;
    deliver(fixture, pointerEvents(32767, 32767, 0));
    require(fixture.rig.input.mouseConnected());
    require(!fixture.rig.input.keyboardConnected());
    require(fixture.rig.mouseDevice.configChangePending());
    require(fixture.rig.pointerDevice.configChangePending());
    Rig::registers(fixture.rig.mouseDevice);
    Rig::registers(fixture.rig.pointerDevice);
    /* The corner of a 1920 x 1080 display, in the pointer's raw range. */
    require(sameEvents(Rig::received(fixture.rig.pointerDevice), {
        event(key, protocol::buttonToolMouse, 1),
        event(key, protocol::buttonTouch, 1), slot0(), trackingId(0),
        positionX(32767), positionY(18431), syncEvent()}));
    require(Rig::received(fixture.rig.mouseDevice).empty());
    /* Motion alone wakes nothing. */
    require(fixture.wakes == 0);
}

void theThreeButtonsKeepTheirMeaning()
{
    Fixture fixture;
    deliver(fixture, pointerEvents(100, 200, 0));
    Rig::registers(fixture.rig.pointerDevice);
    (void)Rig::received(fixture.rig.pointerDevice);

    deliver(fixture, pointerEvents(100, 200, 1U));
    require(sameEvents(Rig::received(fixture.rig.pointerDevice), {
        event(key, protocol::buttonLeft, 1), syncEvent()}));
    require(fixture.wakes == 1);
    deliver(fixture, pointerEvents(100, 200, 1U | 2U | 4U));
    require(sameEvents(Rig::received(fixture.rig.pointerDevice), {
        event(key, protocol::buttonRight, 1),
        event(key, protocol::buttonMiddle, 1), syncEvent()}));
    deliver(fixture, pointerEvents(100, 200, 2U));
    require(sameEvents(Rig::received(fixture.rig.pointerDevice), {
        event(key, protocol::buttonLeft, 0),
        event(key, protocol::buttonMiddle, 0), syncEvent()}));
    /* Every one of those carried a position that had not changed. */
}

void aButtonReleaseAloneKeepsTheLastPosition()
{
    Fixture fixture;
    deliver(fixture, pointerEvents(1000, 2000, 1U));
    Rig::registers(fixture.rig.pointerDevice);
    (void)Rig::received(fixture.rig.pointerDevice);
    /* What the display sends when a mode change ends the buttons: no
     * position. */
    deliver(fixture, {
        raw(vnc::eventKey, vnc::buttonLeft, 0),
        raw(vnc::eventKey, vnc::buttonMiddle, 0),
        raw(vnc::eventKey, vnc::buttonRight, 0),
        raw(vnc::eventSync, vnc::syncReport, 0)});
    require(sameEvents(Rig::received(fixture.rig.pointerDevice), {
        event(key, protocol::buttonLeft, 0), syncEvent()}));
}

void theWheelsRideTheRelativeMouse()
{
    Fixture fixture;
    deliver(fixture, pointerEvents(5, 6, 0, 1, 0));
    Rig::registers(fixture.rig.mouseDevice);
    Rig::registers(fixture.rig.pointerDevice);
    require(fixture.wakes == 1);
    /* The wheel, on the relative mouse: a detent in high-resolution units and
     * the step they make. The position went to the pointer. */
    require(sameEvents(Rig::received(fixture.rig.mouseDevice), {
        event(protocol::eventRelative, protocol::relativeWheelHighResolution,
              120),
        event(protocol::eventRelative, protocol::relativeWheel, 1), syncEvent()}));
    require(!Rig::received(fixture.rig.pointerDevice).empty());
    deliver(fixture, pointerEvents(5, 6, 0, -1, 1));
    require(sameEvents(Rig::received(fixture.rig.mouseDevice), {
        event(protocol::eventRelative, protocol::relativeWheelHighResolution,
              -120),
        event(protocol::eventRelative, protocol::relativeWheel, -1),
        event(protocol::eventRelative,
              protocol::relativeHorizontalWheelHighResolution, 120),
        event(protocol::eventRelative, protocol::relativeHorizontalWheel, 1),
        syncEvent()}));
    require(fixture.wakes == 2);
}

void aPointerBeforeTheGuestHasADisplayIsDropped()
{
    Fixture fixture;
    fixture.mode.reset();
    deliver(fixture, pointerEvents(300, 400, 0));
    /* The mouse is plugged, which a guest sees whether or not it has a
     * display yet, but nothing was sent. */
    require(fixture.rig.input.mouseConnected());
    Rig::registers(fixture.rig.pointerDevice);
    require(Rig::received(fixture.rig.pointerDevice).empty());
    fixture.mode = HardwareInputBridge::ScanoutMode {1080, 1920};
    deliver(fixture, pointerEvents(32767, 32767, 0));
    require(sameEvents(Rig::received(fixture.rig.pointerDevice), {
        event(key, protocol::buttonToolMouse, 1),
        event(key, protocol::buttonTouch, 1), slot0(), trackingId(0),
        positionX(18431), positionY(32767), syncEvent()}));
}

void theLastClientGoingUnplugsWhatWasPlugged()
{
    Fixture fixture;
    /* Nothing is plugged, so there is nothing to unplug. */
    fixture.bridge.clients(0);
    require(!fixture.rig.keyboardDevice.configChangePending());
    require(!fixture.rig.mouseDevice.configChangePending());

    deliver(fixture, keyEvents(30, true));
    deliver(fixture, pointerEvents(10, 10, 1U));
    for (auto* device : {&fixture.rig.keyboardDevice, &fixture.rig.mouseDevice,
                         &fixture.rig.pointerDevice}) {
        Rig::registers(*device);
        (void)Rig::received(*device);
        require(device->claimConfigChange());
        device->clearConfigChange();
    }

    /* A client coming or going while another remains changes nothing. */
    fixture.bridge.clients(1);
    fixture.bridge.clients(2);
    require(fixture.rig.input.keyboardConnected());
    require(fixture.rig.input.mouseConnected());

    fixture.bridge.clients(0);
    require(!fixture.rig.input.keyboardConnected());
    require(!fixture.rig.input.mouseConnected());
    require(fixture.rig.keyboardDevice.configChangePending());
    require(fixture.rig.mouseDevice.configChangePending());
    require(fixture.rig.pointerDevice.configChangePending());
    /* What was held went up first. */
    require(sameEvents(Rig::received(fixture.rig.keyboardDevice), {
        event(key, 30, 0), syncEvent()}));
    require(sameEvents(Rig::received(fixture.rig.pointerDevice), {
        event(key, protocol::buttonLeft, 0), slot0(), trackingId(-1),
        event(key, protocol::buttonTouch, 0),
        event(key, protocol::buttonToolMouse, 0), syncEvent()}));

    /* The next client starts afresh: no button is remembered down, and the
     * first event plugs again. */
    deliver(fixture, pointerEvents(0, 0, 0));
    require(fixture.rig.input.mouseConnected());
    require(!fixture.rig.input.keyboardConnected());
    Rig::registers(fixture.rig.pointerDevice);
    require(sameEvents(Rig::received(fixture.rig.pointerDevice), {
        event(key, protocol::buttonToolMouse, 1),
        event(key, protocol::buttonTouch, 1), slot0(), trackingId(1),
        positionX(0), positionY(0), syncEvent()}));
}

void aTouchIsNotAKeyAndADetachedBridgeIsDeaf()
{
    Fixture fixture;
    deliver(fixture, {
        raw(vnc::eventAbsolute, vnc::absoluteX, 5),
        raw(vnc::eventAbsolute, vnc::absoluteY, 5),
        raw(vnc::eventKey, vnc::buttonTouch, 1),
        raw(vnc::eventSync, vnc::syncReport, 0)});
    /* Only the position counts, so only the mouse is plugged. */
    require(!fixture.rig.input.keyboardConnected());
    require(fixture.rig.input.mouseConnected());

    fixture.bridge.detach();
    deliver(fixture, keyEvents(30, true));
    require(!fixture.rig.input.keyboardConnected());
    /* A bridge that is attached again has plugged nothing. */
    fixture.bridge.attach(std::shared_ptr<HardwareInput>(
        std::shared_ptr<void> {}, &fixture.rig.input));
    deliver(fixture, keyEvents(30, true));
    require(fixture.rig.input.keyboardConnected());
}

} // namespace

int main()
{
    aKeyPlugsOnlyTheKeyboard();
    aPointerEventPlugsTheMouseAndThePointer();
    theThreeButtonsKeepTheirMeaning();
    aButtonReleaseAloneKeepsTheLastPosition();
    theWheelsRideTheRelativeMouse();
    aPointerBeforeTheGuestHasADisplayIsDropped();
    theLastClientGoingUnplugsWhatWasPlugged();
    aTouchIsNotAKeyAndADetachedBridgeIsDeaf();
    return EXIT_SUCCESS;
}

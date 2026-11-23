// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "hardware_reports.hpp"
#include "virtio_input_device.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace uml::input::virtio {

/* What the guest tells the host about the hardware keyboard and mouse. */
enum class HardwareStatus : std::uint8_t {
    /* Android's lock LEDs, which its keyboard lights. */
    numLock,
    capsLock,
    scrollLock,
    /* An Android application captured the pointer, and released it. */
    pointerCapture,
    /* The guest registered an input device for the keyboard, the mouse or the
     * pointer (on), or unregistered it: a plug the guest has answered. A
     * kernel without the driver topic never says it
     * (kernel/common/input). */
    keyboardRegistered,
    mouseRegistered,
    pointerRegistered,
};

/* The host's side of the guest's hardware keyboard, mouse and pointer: one
 * pluggable device each (DeviceKind::hardwareKeyboard, ::mouse, ::pointer),
 * the keys, buttons and contact the host holds, and the plugging. The
 * keyboard is plugged on its own; the mouse and the pointer, which are one
 * mouse to Android, are plugged together.
 *
 * Input for a device that is unplugged is dropped. Before an unplug the host
 * releases every key and button it holds and ends the pointer's contact, in
 * the events the guest still receives. Input that a plugged device drops
 * because the guest has not registered it (EventQueue::enqueue) makes the
 * host forget what it holds of that device, since none of it reached the
 * guest.
 *
 * Every method may be called from any thread. Each input returns whether it
 * wakes a guest that suspended itself (HardwareReport::wakes), which only the
 * runtime can do. */
class HardwareInput final {
public:
    /* Called on the transport thread of the device concerned, and must return
     * promptly. */
    using StatusCallback = std::function<void(HardwareStatus, bool on)>;

    HardwareInput(
        std::shared_ptr<EventQueue> keyboard,
        std::shared_ptr<EventQueue> mouse,
        std::shared_ptr<EventQueue> pointer);
    ~HardwareInput();

    HardwareInput(const HardwareInput&) = delete;
    HardwareInput& operator=(const HardwareInput&) = delete;

    /* Replaces the callback; once this returns no call to the old one is
     * running. A null callback clears it. */
    void setStatusCallback(StatusCallback callback);

    void setKeyboardConnected(bool connected);
    void setMouseConnected(bool connected);
    [[nodiscard]] bool keyboardConnected() const noexcept;
    [[nodiscard]] bool mouseConnected() const noexcept;

    /* A key by its Linux key code, and by its HID usage on the Keyboard/Keypad
     * page, through the table of hid_keyboard.hpp. A usage or code the
     * keyboard cannot produce is dropped. */
    bool key(std::uint16_t code, bool pressed);
    bool keyUsage(std::uint32_t usage, bool pressed);
    /* The pointer at (x, y) of the touchscreen range over a guest display of
     * `width` x `height` pixels (pointerRawPosition), or out of range. A
     * pointer in range over a display of no size is dropped. */
    bool pointer(
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t buttons,
        bool inRange,
        std::uint32_t width,
        std::uint32_t height);
    bool mouse(
        std::int32_t dx,
        std::int32_t dy,
        std::uint32_t buttons,
        std::int32_t wheel,
        std::int32_t horizontalWheel);

private:
    void guestStatus(HardwareStatus kind, const StatusEvent& event);
    void registered(DeviceKind kind, const StatusEvent& event);

    const std::shared_ptr<EventQueue> keyboardQueue_;
    const std::shared_ptr<EventQueue> mouseQueue_;
    const std::shared_ptr<EventQueue> pointerQueue_;
    /* Guards what the host holds, and orders each input against a plug or an
     * unplug. The status sinks do not hold it while they call the callback. */
    std::mutex mutex_;
    HardwareKeyboard keyboard_;
    Mouse mouse_;
    Pointer pointer_;
    std::mutex callbackMutex_;
    StatusCallback callback_;
};

} // namespace uml::input::virtio

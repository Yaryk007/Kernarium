// SPDX-License-Identifier: GPL-2.0-or-later

#include "hardware_input.hpp"

#include "hid_keyboard.hpp"

#include <stdexcept>
#include <utility>

namespace uml::input::virtio {

HardwareInput::HardwareInput(
    std::shared_ptr<EventQueue> keyboard,
    std::shared_ptr<EventQueue> mouse,
    std::shared_ptr<EventQueue> pointer)
    : keyboardQueue_(std::move(keyboard))
    , mouseQueue_(std::move(mouse))
    , pointerQueue_(std::move(pointer))
{
    if (!keyboardQueue_ || !mouseQueue_ || !pointerQueue_)
        throw std::invalid_argument("invalid hardware input options");
    keyboardQueue_->setStatusSink([this](const StatusEvent& event) {
        if (event.kind == StatusEvent::Kind::registration) {
            registered(DeviceKind::hardwareKeyboard, event);
            return;
        }
        switch (event.led) {
        case protocol::ledNumLock:
            guestStatus(HardwareStatus::numLock, event);
            break;
        case protocol::ledCapsLock:
            guestStatus(HardwareStatus::capsLock, event);
            break;
        case protocol::ledScrollLock:
            guestStatus(HardwareStatus::scrollLock, event);
            break;
        default:
            break;
        }
    });
    mouseQueue_->setStatusSink([this](const StatusEvent& event) {
        if (event.kind == StatusEvent::Kind::registration)
            registered(DeviceKind::mouse, event);
        else if (event.led == protocol::ledMisc)
            guestStatus(HardwareStatus::pointerCapture, event);
    });
    pointerQueue_->setStatusSink([this](const StatusEvent& event) {
        if (event.kind == StatusEvent::Kind::registration)
            registered(DeviceKind::pointer, event);
    });
}

HardwareInput::~HardwareInput()
{
    keyboardQueue_->setStatusSink({});
    mouseQueue_->setStatusSink({});
    pointerQueue_->setStatusSink({});
}

void HardwareInput::setStatusCallback(StatusCallback callback)
{
    std::lock_guard lock(callbackMutex_);
    callback_ = std::move(callback);
}

void HardwareInput::guestStatus(HardwareStatus kind, const StatusEvent& event)
{
    std::lock_guard lock(callbackMutex_);
    if (callback_)
        callback_(kind, event.on);
}

void HardwareInput::registered(DeviceKind kind, const StatusEvent& event)
{
    HardwareStatus status;
    switch (kind) {
    case DeviceKind::hardwareKeyboard:
        status = HardwareStatus::keyboardRegistered;
        break;
    case DeviceKind::mouse:
        status = HardwareStatus::mouseRegistered;
        break;
    case DeviceKind::pointer:
        status = HardwareStatus::pointerRegistered;
        break;
    default:
        return;
    }
    guestStatus(status, event);
}

void HardwareInput::setKeyboardConnected(bool connected)
{
    std::lock_guard lock(mutex_);
    if (connected) {
        if (keyboardQueue_->setConnected(true))
            keyboard_.reset();
        return;
    }
    keyboardQueue_->disconnect(keyboard_.releaseAll().span());
    keyboard_.reset();
}

void HardwareInput::setMouseConnected(bool connected)
{
    std::lock_guard lock(mutex_);
    if (connected) {
        if (pointerQueue_->setConnected(true))
            pointer_.reset();
        if (mouseQueue_->setConnected(true))
            mouse_.reset();
        return;
    }
    mouseQueue_->disconnect(mouse_.releaseAll().span());
    pointerQueue_->disconnect(pointer_.leave().span());
    mouse_.reset();
    pointer_.reset();
}

bool HardwareInput::keyboardConnected() const noexcept
{
    return keyboardQueue_->connected();
}

bool HardwareInput::mouseConnected() const noexcept
{
    return mouseQueue_->connected();
}

bool HardwareInput::key(std::uint16_t code, bool pressed)
{
    std::lock_guard lock(mutex_);
    if (!keyboardQueue_->connected())
        return false;
    const auto report = keyboard_.key(code, pressed);
    if (keyboardQueue_->enqueue(report.span()))
        keyboard_.reset();
    return report.wakes;
}

bool HardwareInput::keyUsage(std::uint32_t usage, bool pressed)
{
    const auto code = hid::linuxKey(usage);
    return code != 0 && key(code, pressed);
}

bool HardwareInput::pointer(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t buttons,
    bool inRange,
    std::uint32_t width,
    std::uint32_t height)
{
    std::lock_guard lock(mutex_);
    if (!pointerQueue_->connected())
        return false;
    HardwareReport report;
    if (!inRange) {
        report = pointer_.leave();
    } else {
        const auto position = pointerRawPosition(x, y, width, height);
        if (!position)
            return false;
        report = pointer_.update(position->x, position->y, buttons, true);
    }
    if (pointerQueue_->enqueue(report.span()))
        pointer_.reset();
    return report.wakes;
}

bool HardwareInput::mouse(
    std::int32_t dx,
    std::int32_t dy,
    std::uint32_t buttons,
    std::int32_t wheel,
    std::int32_t horizontalWheel)
{
    std::lock_guard lock(mutex_);
    if (!mouseQueue_->connected())
        return false;
    const auto report = mouse_.update(dx, dy, buttons, wheel, horizontalWheel);
    if (mouseQueue_->enqueue(report.span()))
        mouse_.reset();
    return report.wakes;
}

} // namespace uml::input::virtio

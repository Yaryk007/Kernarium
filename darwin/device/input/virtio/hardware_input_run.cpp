// SPDX-License-Identifier: GPL-2.0-or-later

#include "hardware_input_run.hpp"

#include <utility>

namespace uml::input::virtio {

void HardwareInputRun::begin()
{
    std::lock_guard lock(mutex_);
    running_ = true;
    keyboard_ = false;
    mouse_ = false;
}

void HardwareInputRun::end()
{
    std::lock_guard lock(mutex_);
    running_ = false;
    keyboard_ = false;
    mouse_ = false;
}

void HardwareInputRun::applyLocked(Device device)
{
    if (input_ == nullptr)
        return;
    if (device == Device::keyboard)
        input_->setKeyboardConnected(keyboard_);
    else
        input_->setMouseConnected(mouse_);
}

void HardwareInputRun::attach(std::shared_ptr<HardwareInput> input)
{
    std::lock_guard lock(mutex_);
    input_ = std::move(input);
    /* A device is plugged for a boot only when the application asked, and a
     * boot's devices start unplugged. */
    if (keyboard_)
        applyLocked(Device::keyboard);
    if (mouse_)
        applyLocked(Device::mouse);
}

std::shared_ptr<HardwareInput> HardwareInputRun::detach()
{
    std::lock_guard lock(mutex_);
    return std::exchange(input_, nullptr);
}

bool HardwareInputRun::request(Device device, bool connected)
{
    std::lock_guard lock(mutex_);
    if (!running_)
        return false;
    (device == Device::keyboard ? keyboard_ : mouse_) = connected;
    /* Applied with the lock held, so two requests reach the input in the order
     * they were recorded. */
    applyLocked(device);
    return true;
}

std::shared_ptr<HardwareInput> HardwareInputRun::current() const
{
    std::lock_guard lock(mutex_);
    return input_;
}

} // namespace uml::input::virtio

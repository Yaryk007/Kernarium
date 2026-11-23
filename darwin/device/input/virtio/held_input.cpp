// SPDX-License-Identifier: GPL-2.0-or-later

#include "held_input.hpp"

namespace uml::input::virtio {

void HeldInput::observe(
    std::span<const protocol::InputEvent> events) noexcept
{
    for (const auto& event : events) {
        if (event.type == protocol::eventKey) {
            if (event.code < keyCount)
                keys_.set(event.code, event.value != 0);
        } else if (event.type == protocol::eventAbsolute) {
            if (event.code == protocol::absoluteMtSlot) {
                slot_ = event.value;
            } else if (event.code == protocol::absoluteMtTrackingId &&
                       slot_ >= 0 &&
                       static_cast<std::size_t>(slot_) < slotCount) {
                contacts_.set(static_cast<std::size_t>(slot_),
                              event.value != -1);
            }
        }
    }
}

bool HeldInput::any() const noexcept
{
    return keys_.any() || contacts_.any();
}

std::size_t HeldInput::lastHeldSlot() const noexcept
{
    for (std::size_t slot = slotCount; slot != 0; --slot) {
        if (contacts_.test(slot - 1U))
            return slot - 1U;
    }
    return slotCount;
}

bool HeldInput::restoresSlot() const noexcept
{
    const auto last = lastHeldSlot();
    return last != slotCount && slot_ >= 0 &&
        static_cast<std::size_t>(slot_) != last;
}

std::size_t HeldInput::releaseSize() const noexcept
{
    if (!any())
        return 0;
    return keys_.count() + 2U * contacts_.count() +
        (restoresSlot() ? 1U : 0U) + 1U;
}

void HeldInput::appendRelease(
    std::deque<protocol::InputEvent>& events) noexcept
{
    if (!any())
        return;
    for (std::size_t code = 0; code < keyCount; ++code) {
        if (keys_.test(code)) {
            events.push_back(
                {protocol::eventKey, static_cast<std::uint16_t>(code), 0});
        }
    }
    for (std::size_t slot = 0; slot < slotCount; ++slot) {
        if (!contacts_.test(slot))
            continue;
        events.push_back(
            {protocol::eventAbsolute, protocol::absoluteMtSlot,
             static_cast<std::int32_t>(slot)});
        events.push_back(
            {protocol::eventAbsolute, protocol::absoluteMtTrackingId, -1});
    }
    if (restoresSlot()) {
        events.push_back(
            {protocol::eventAbsolute, protocol::absoluteMtSlot, slot_});
    } else if (const auto last = lastHeldSlot(); last != slotCount) {
        slot_ = static_cast<std::int32_t>(last);
    }
    events.push_back(
        {protocol::eventSynchronize, protocol::synchronizeReport, 0});
    keys_.reset();
    contacts_.reset();
}

void HeldInput::reset() noexcept
{
    keys_.reset();
    contacts_.reset();
    slot_ = 0;
}

} // namespace uml::input::virtio

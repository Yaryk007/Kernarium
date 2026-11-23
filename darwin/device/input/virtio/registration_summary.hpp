// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "hardware_input.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace uml::input::virtio {

/* What the guest has registered of the hardware input, as the application is
 * told: the keyboard, and the pointer, which is the mouse and the absolute
 * pointer together (they are one mouse to Android, plugged together, and the
 * application needs both). The pointer counts as registered while both of its
 * devices are.
 *
 * The statuses of the three devices arrive on three transport threads, so the
 * caller serialises the calls and the notice that follows each, which keeps
 * the order the application sees the order of the changes. */
class RegistrationSummary final {
public:
    enum class Group : std::uint8_t { keyboard, pointer };

    struct Edge {
        Group group;
        bool on;

        friend bool operator==(const Edge&, const Edge&) = default;
    };

    /* Takes one status from HardwareInput's callback and returns the change it
     * makes to a group, if it makes one. A status that is not a registration
     * makes none. */
    [[nodiscard]] std::optional<Edge> update(
        HardwareStatus status, bool on) noexcept
    {
        switch (status) {
        case HardwareStatus::keyboardRegistered: {
            const bool changed = keyboard_ != on;
            keyboard_ = on;
            return changed ? std::optional<Edge> {Edge {Group::keyboard, on}}
                           : std::nullopt;
        }
        case HardwareStatus::mouseRegistered:
        case HardwareStatus::pointerRegistered: {
            const bool before = mouse_ && pointer_;
            (status == HardwareStatus::mouseRegistered ? mouse_ : pointer_) =
                on;
            const bool after = mouse_ && pointer_;
            return before != after
                ? std::optional<Edge> {Edge {Group::pointer, after}}
                : std::nullopt;
        }
        default:
            return std::nullopt;
        }
    }

    /* A boot ended, whether the guest restarts or the run is over: nothing is
     * registered. Returns both groups as off, which the application is told
     * whether or not it was on, so it never keeps a registration the guest
     * lost. */
    [[nodiscard]] std::array<Edge, 2> reset() noexcept
    {
        keyboard_ = false;
        mouse_ = false;
        pointer_ = false;
        return {Edge {Group::keyboard, false}, Edge {Group::pointer, false}};
    }

private:
    bool keyboard_ {};
    bool mouse_ {};
    bool pointer_ {};
};

} // namespace uml::input::virtio

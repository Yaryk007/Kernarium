// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uml::input::virtio {

/* The events of one host input, every one of them ended by a SYN_REPORT, and
 * whether the input is one that wakes a guest that suspended itself: a key or
 * a button going down, or the wheel taking a whole step. Pointer motion does
 * not wake it, so a bumped mouse never resumes a guest. */
struct HardwareReport final {
    std::vector<protocol::InputEvent> events;
    bool wakes {};

    [[nodiscard]] std::span<const protocol::InputEvent> span() const noexcept
    {
        return events;
    }
    [[nodiscard]] bool empty() const noexcept { return events.empty(); }
};

/* The buttons of a mouse and of the pointer, in the bits of one word: the
 * order of UIKit's button numbers, and of Linux's BTN_LEFT, BTN_RIGHT,
 * BTN_MIDDLE, BTN_SIDE and BTN_EXTRA. Android turns the last two into Back
 * and Forward. */
inline constexpr std::uint32_t buttonPrimary = 1U << 0;
inline constexpr std::uint32_t buttonSecondary = 1U << 1;
inline constexpr std::uint32_t buttonMiddle = 1U << 2;
inline constexpr std::uint32_t buttonBack = 1U << 3;
inline constexpr std::uint32_t buttonForward = 1U << 4;
inline constexpr std::uint32_t buttonMask = (1U << 5) - 1U;

/* The keys a host holds down. A key goes down once and up once: Android
 * repeats keys itself, so the keyboard has no auto-repeat. A key the HID
 * table cannot produce, a second press of a key that is down, and the release
 * of one that is not are no events. */
class HardwareKeyboard final {
public:
    [[nodiscard]] HardwareReport key(std::uint16_t code, bool pressed);
    /* One release for each key held, and nothing held after. */
    [[nodiscard]] HardwareReport releaseAll();
    [[nodiscard]] bool holds() const noexcept;
    /* Forgets what is held, for a guest that has seen none of it. */
    void reset() noexcept;

private:
    std::array<std::uint8_t, 32> held_ {};
};

/* A relative mouse. The wheels are in high-resolution units, 120 to a detent
 * with positive values up and to the right, and the low-resolution steps
 * Linux's input core also reports are derived from them, the remainder carried
 * to the next report. */
class Mouse final {
public:
    [[nodiscard]] HardwareReport update(
        std::int32_t dx,
        std::int32_t dy,
        std::uint32_t buttons,
        std::int32_t wheel,
        std::int32_t horizontalWheel);
    /* Releases every button held. */
    [[nodiscard]] HardwareReport releaseAll();
    [[nodiscard]] bool holds() const noexcept { return buttons_ != 0; }
    void reset() noexcept;

private:
    std::uint32_t buttons_ {};
    std::int32_t wheelRemainder_ {};
    std::int32_t horizontalWheelRemainder_ {};
};

/* An absolute mouse: one contact in the first slot while the pointer is over
 * the guest's display, whose position Android's cursor follows, hovering until
 * a button is down. Coordinates are in the pointer's raw range
 * (0 through protocol::pointerAbsoluteMaximum), one scale on both axes: see
 * pointerRawPosition. Leaving the display ends the contact, and Android's
 * cursor stays where it was. */
class Pointer final {
public:
    [[nodiscard]] HardwareReport update(
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t buttons,
        bool inRange);
    /* Releases the buttons held and ends the contact. */
    [[nodiscard]] HardwareReport leave();
    [[nodiscard]] bool inRange() const noexcept { return inRange_; }
    void reset() noexcept;

private:
    bool inRange_ {};
    std::uint32_t x_ {};
    std::uint32_t y_ {};
    std::uint32_t buttons_ {};
    std::int32_t nextTrackingId_ {};
};

struct PointerPosition final {
    std::uint32_t x {};
    std::uint32_t y {};
};

/* The raw position that puts Android's cursor on the guest pixel a touch at
 * (x, y) of the touchscreen range would touch, for a guest display of
 * `width` x `height` pixels. Android maps an absolute mouse's raw range onto
 * the display with one scale for both axes, the larger of the display's
 * pixels per raw unit, where it maps a touchscreen's range onto each axis
 * separately. Nothing for a coordinate outside the range or a display with no
 * size. */
[[nodiscard]] std::optional<PointerPosition> pointerRawPosition(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height) noexcept;

} // namespace uml::input::virtio

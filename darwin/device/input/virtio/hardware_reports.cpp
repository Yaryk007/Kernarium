// SPDX-License-Identifier: GPL-2.0-or-later

#include "hardware_reports.hpp"

#include "hid_keyboard.hpp"

#include <algorithm>
#include <limits>

namespace uml::input::virtio {
namespace {

constexpr std::array<std::uint16_t, 5> buttonCodes {
    protocol::buttonLeft,
    protocol::buttonRight,
    protocol::buttonMiddle,
    protocol::buttonSide,
    protocol::buttonExtra,
};

protocol::InputEvent makeEvent(
    std::uint16_t type,
    std::uint16_t code,
    std::int32_t value) noexcept
{
    return {.type = type, .code = code, .value = value};
}

void endReport(HardwareReport& report)
{
    if (!report.events.empty())
        report.events.push_back(makeEvent(
            protocol::eventSynchronize, protocol::synchronizeReport, 0));
}

/* A button event for each bit that differs between `before` and `after`;
 * true when one of them went down. */
bool buttonEvents(
    HardwareReport& report,
    std::uint32_t before,
    std::uint32_t after)
{
    bool pressed = false;
    for (std::size_t index = 0; index < buttonCodes.size(); ++index) {
        const std::uint32_t bit = 1U << index;
        if (((before ^ after) & bit) == 0)
            continue;
        const bool down = (after & bit) != 0;
        report.events.push_back(
            makeEvent(protocol::eventKey, buttonCodes[index], down ? 1 : 0));
        pressed |= down;
    }
    return pressed;
}

/* One axis of wheel movement: the high-resolution event, then the
 * low-resolution one when a detent is complete. True for a whole step. */
bool wheelEvents(
    HardwareReport& report,
    std::int32_t& remainder,
    std::int32_t highResolution,
    std::uint16_t highResolutionCode,
    std::uint16_t lowResolutionCode)
{
    if (highResolution == 0)
        return false;
    report.events.push_back(
        makeEvent(protocol::eventRelative, highResolutionCode, highResolution));
    const std::int64_t total =
        static_cast<std::int64_t>(remainder) + highResolution;
    const std::int64_t steps = total / protocol::wheelDetent;
    remainder = static_cast<std::int32_t>(total - steps * protocol::wheelDetent);
    if (steps == 0)
        return false;
    report.events.push_back(makeEvent(
        protocol::eventRelative,
        lowResolutionCode,
        static_cast<std::int32_t>(std::clamp<std::int64_t>(
            steps,
            std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()))));
    return true;
}

} // namespace

HardwareReport HardwareKeyboard::key(std::uint16_t code, bool pressed)
{
    HardwareReport report;
    if (!hid::producesKey(code))
        return report;
    auto& byte = held_[code / 8U];
    const auto bit = static_cast<std::uint8_t>(1U << (code % 8U));
    if (((byte & bit) != 0) == pressed)
        return report;
    byte = static_cast<std::uint8_t>(pressed ? byte | bit : byte & ~bit);
    report.events.push_back(
        makeEvent(protocol::eventKey, code, pressed ? 1 : 0));
    report.wakes = pressed;
    endReport(report);
    return report;
}

HardwareReport HardwareKeyboard::releaseAll()
{
    HardwareReport report;
    for (std::uint32_t code = 0; code < held_.size() * 8U; ++code) {
        if ((held_[code / 8U] & (1U << (code % 8U))) != 0) {
            report.events.push_back(makeEvent(
                protocol::eventKey, static_cast<std::uint16_t>(code), 0));
        }
    }
    held_ = {};
    endReport(report);
    return report;
}

bool HardwareKeyboard::holds() const noexcept
{
    return std::any_of(held_.begin(), held_.end(), [](std::uint8_t byte) {
        return byte != 0;
    });
}

void HardwareKeyboard::reset() noexcept
{
    held_ = {};
}

HardwareReport Mouse::update(
    std::int32_t dx,
    std::int32_t dy,
    std::uint32_t buttons,
    std::int32_t wheel,
    std::int32_t horizontalWheel)
{
    HardwareReport report;
    buttons &= buttonMask;
    if (dx != 0)
        report.events.push_back(
            makeEvent(protocol::eventRelative, protocol::relativeX, dx));
    if (dy != 0)
        report.events.push_back(
            makeEvent(protocol::eventRelative, protocol::relativeY, dy));
    report.wakes |= wheelEvents(
        report, wheelRemainder_, wheel,
        protocol::relativeWheelHighResolution, protocol::relativeWheel);
    report.wakes |= wheelEvents(
        report, horizontalWheelRemainder_, horizontalWheel,
        protocol::relativeHorizontalWheelHighResolution,
        protocol::relativeHorizontalWheel);
    report.wakes |= buttonEvents(report, buttons_, buttons);
    buttons_ = buttons;
    endReport(report);
    return report;
}

HardwareReport Mouse::releaseAll()
{
    HardwareReport report;
    (void)buttonEvents(report, buttons_, 0);
    buttons_ = 0;
    endReport(report);
    return report;
}

void Mouse::reset() noexcept
{
    buttons_ = 0;
    wheelRemainder_ = 0;
    horizontalWheelRemainder_ = 0;
}

HardwareReport Pointer::update(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t buttons,
    bool inRange)
{
    if (!inRange)
        return leave();
    HardwareReport report;
    x = std::min(x, protocol::pointerAbsoluteMaximum);
    y = std::min(y, protocol::pointerAbsoluteMaximum);
    buttons &= buttonMask;
    const auto position = [&] {
        report.events.push_back(makeEvent(
            protocol::eventAbsolute, protocol::absoluteMtSlot, 0));
        report.events.push_back(makeEvent(
            protocol::eventAbsolute, protocol::absoluteMtPositionX,
            static_cast<std::int32_t>(x)));
        report.events.push_back(makeEvent(
            protocol::eventAbsolute, protocol::absoluteMtPositionY,
            static_cast<std::int32_t>(y)));
    };
    if (!inRange_) {
        /* The tool in range, then the contact, as Android's own test of an
         * absolute mouse sends them. */
        report.events.push_back(
            makeEvent(protocol::eventKey, protocol::buttonToolMouse, 1));
        report.events.push_back(
            makeEvent(protocol::eventKey, protocol::buttonTouch, 1));
        report.events.push_back(makeEvent(
            protocol::eventAbsolute, protocol::absoluteMtSlot, 0));
        report.events.push_back(makeEvent(
            protocol::eventAbsolute, protocol::absoluteMtTrackingId,
            nextTrackingId_));
        nextTrackingId_ =
            nextTrackingId_ == protocol::touchscreenTrackingIdMaximum ?
                0 : nextTrackingId_ + 1;
        report.events.push_back(makeEvent(
            protocol::eventAbsolute, protocol::absoluteMtPositionX,
            static_cast<std::int32_t>(x)));
        report.events.push_back(makeEvent(
            protocol::eventAbsolute, protocol::absoluteMtPositionY,
            static_cast<std::int32_t>(y)));
        inRange_ = true;
        x_ = x;
        y_ = y;
    } else if (x != x_ || y != y_) {
        position();
        x_ = x;
        y_ = y;
    }
    report.wakes = buttonEvents(report, buttons_, buttons);
    buttons_ = buttons;
    endReport(report);
    return report;
}

HardwareReport Pointer::leave()
{
    HardwareReport report;
    if (!inRange_)
        return report;
    (void)buttonEvents(report, buttons_, 0);
    report.events.push_back(makeEvent(
        protocol::eventAbsolute, protocol::absoluteMtSlot, 0));
    report.events.push_back(makeEvent(
        protocol::eventAbsolute, protocol::absoluteMtTrackingId, -1));
    report.events.push_back(
        makeEvent(protocol::eventKey, protocol::buttonTouch, 0));
    report.events.push_back(
        makeEvent(protocol::eventKey, protocol::buttonToolMouse, 0));
    endReport(report);
    inRange_ = false;
    buttons_ = 0;
    return report;
}

void Pointer::reset() noexcept
{
    inRange_ = false;
    x_ = 0;
    y_ = 0;
    buttons_ = 0;
}

std::optional<PointerPosition> pointerRawPosition(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    if (width == 0 || height == 0 || x > protocol::touchscreenAbsoluteMaximum ||
        y > protocol::touchscreenAbsoluteMaximum)
        return std::nullopt;
    const std::uint64_t larger = std::max(width, height);
    /* Rounded to the nearest raw unit. */
    const auto scaled = [larger](std::uint32_t value, std::uint32_t extent) {
        return static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(value) * extent + larger / 2U) /
            larger);
    };
    return PointerPosition {
        .x = std::min(scaled(x, width), protocol::pointerAbsoluteMaximum),
        .y = std::min(scaled(y, height), protocol::pointerAbsoluteMaximum),
    };
}

} // namespace uml::input::virtio

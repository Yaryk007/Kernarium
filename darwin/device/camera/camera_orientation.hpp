// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "camera.hpp"

#include <cstdint>

namespace uml::camera {

/* How a camera's picture is turned when the host, not the guest, owns the
 * guest's rotation (an iPad window): the guest's display stays upright in the
 * world at rotation 0 while the device body, which carries every built-in
 * sensor, turns under it.
 *
 * `turn` is the clockwise turn of the device body from portrait, as its user
 * sees it, in degrees: 0, 90, 180 or 270. It is 0 where the guest owns the
 * rotation (a phone), which leaves every value as the platform states it.
 *
 * Derivation. A sensor's SENSOR_ORIENTATION is the clockwise turn that makes
 * its buffer upright on the screen in the device's natural orientation, and
 * the guest's screen is always upright in the world. Turn the body clockwise
 * by `turn`:
 *  - a back camera looks the way its user looks, so its scene turns the other
 *    way in the buffer and the buffer needs `turn` more: base + turn;
 *  - a front camera looks the other way, so the scene turns the same way in
 *    the buffer and the buffer needs `turn` less: base - turn;
 *  - an external camera is not on the body and does not turn.
 * Check against an iPhone 15 Pro Max's cameras. A back camera's buffer is
 * stated 90 and is upright with the port on the right (turn 270: 90 + 270 = 0).
 * A front camera's native buffer is upright with the port on the left, and the
 * host turns it half around (CameraInfo::presentationTurn), so it is stated 270
 * and is upright with the port on the right (turn 270: 270 - 270 = 0). */

[[nodiscard]] constexpr std::uint32_t turnedSensorOrientation(
    uml::platform::CameraPosition position, std::uint32_t base, std::uint32_t turn) noexcept
{
    switch (position) {
    case uml::platform::CameraPosition::back:
        return (base + turn) % 360;
    case uml::platform::CameraPosition::front:
        return (base + 360 - turn % 360) % 360;
    case uml::platform::CameraPosition::external:
        break;
    }
    return base;
}

/* How far the host turns a camera's picture clockwise, in degrees, when the
 * device body has gone from `bootTurn`, the turn the guest was told at boot
 * (its SENSOR_ORIENTATION was derived from it), to `turn` since. It is what
 * ROTATE_AND_CROP's automatic setting applies: the difference of the two
 * turned orientations, which the guest cannot know. */
[[nodiscard]] constexpr std::uint32_t pictureTurn(
    uml::platform::CameraPosition position, std::uint32_t bootTurn, std::uint32_t turn) noexcept
{
    return (turnedSensorOrientation(position, 0, turn) + 360 -
            turnedSensorOrientation(position, 0, bootTurn)) % 360;
}

} // namespace uml::camera

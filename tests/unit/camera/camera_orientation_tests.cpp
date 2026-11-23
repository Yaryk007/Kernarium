// SPDX-License-Identifier: GPL-2.0-or-later
// How a camera's SENSOR_ORIENTATION and picture turn follow the turn of the
// device body where the host owns the guest's rotation (an iPad window): the
// table of the four interface orientations for a back camera and for a front
// camera of either mounting (its base is the platform camera's own statement:
// 270, or 90 as an iPhone 15 Pro Max's), the phone case that leaves the
// platform's values, and the turn of the picture after a boot.

#include "camera_orientation.hpp"
#include "require.hpp"

#include <array>
#include <cstdint>

using namespace uml::camera;
using uml::platform::CameraPosition;
using uml::testing::require;

namespace {

/* The device body's clockwise turn from portrait of each interface
 * orientation: UIKit's landscapeLeft has the home button on the left, which is
 * the device turned a quarter clockwise. */
struct Interface {
    const char* name;
    std::uint32_t turn;
    std::uint32_t back;     // a back camera stated 90
    std::uint32_t front270; // a front camera stated 270
    std::uint32_t front90;  // a front camera stated 90
};

constexpr std::array<Interface, 4> interfaces {{
    {"portrait", 0, 90, 270, 90},
    {"landscapeLeft", 90, 180, 180, 0},
    {"portraitUpsideDown", 180, 270, 90, 270},
    {"landscapeRight", 270, 0, 0, 180},
}};

void testTheTableOfOrientations()
{
    // The platform's own values: 90 for a back camera and, for a front camera,
    // its own statement, which is 270 or 90 (an iPhone 15 Pro Max's).
    for (const auto& one : interfaces) {
        require(turnedSensorOrientation(CameraPosition::back, 90, one.turn) == one.back, one.name);
        require(turnedSensorOrientation(CameraPosition::front, 270, one.turn) == one.front270, one.name);
        require(turnedSensorOrientation(CameraPosition::front, 90, one.turn) == one.front90, one.name);
        // An external camera is not on the body.
        require(turnedSensorOrientation(CameraPosition::external, 90, one.turn) == 90, one.name);
    }
    // A native buffer is upright where the turn undoes its base: with the home
    // button on the right (landscapeRight) for a back camera and a front camera
    // stated 270, and with it on the left (landscapeLeft) for a front camera
    // stated 90.
    require(turnedSensorOrientation(CameraPosition::back, 90, 270) == 0 &&
                turnedSensorOrientation(CameraPosition::front, 270, 270) == 0,
            "the native buffer needs no turn in landscapeRight");
    require(turnedSensorOrientation(CameraPosition::front, 90, 90) == 0,
            "a front camera stated 90 needs no turn in landscapeLeft");
}

void testAPhoneKeepsThePlatformsValues()
{
    // A guest that rotates itself states no turn.
    for (const std::uint32_t base : {0u, 90u, 180u, 270u}) {
        for (const auto position : {CameraPosition::back, CameraPosition::front, CameraPosition::external})
            require(turnedSensorOrientation(position, base, 0) == base, "turn 0 leaves the platform's value");
    }
}

void testThePictureTurnsByTheChangeSinceTheBoot()
{
    for (const auto& boot : interfaces) {
        for (const auto& now : interfaces) {
            const auto delta = (now.turn + 360 - boot.turn) % 360;
            // A back camera's picture turns with the body, a front camera's against it.
            require(pictureTurn(CameraPosition::back, boot.turn, now.turn) == delta, boot.name);
            require(pictureTurn(CameraPosition::front, boot.turn, now.turn) == (360 - delta) % 360, boot.name);
            require(pictureTurn(CameraPosition::external, boot.turn, now.turn) == 0, boot.name);
            // The boot's orientation and the turn together are the orientation now.
            require((turnedSensorOrientation(CameraPosition::back, 90, boot.turn) +
                     pictureTurn(CameraPosition::back, boot.turn, now.turn)) % 360 == now.back,
                    "back: boot orientation plus the change is the orientation now");
            require((turnedSensorOrientation(CameraPosition::front, 270, boot.turn) +
                     pictureTurn(CameraPosition::front, boot.turn, now.turn)) % 360 == now.front270,
                    "front stated 270: boot orientation plus the change is the orientation now");
            require((turnedSensorOrientation(CameraPosition::front, 90, boot.turn) +
                     pictureTurn(CameraPosition::front, boot.turn, now.turn)) % 360 == now.front90,
                    "front stated 90: boot orientation plus the change is the orientation now");
        }
    }
    require(pictureTurn(CameraPosition::back, 0, 0) == 0 && pictureTurn(CameraPosition::front, 270, 270) == 0,
            "no change, no turn");
}

} // namespace

int main()
{
    testTheTableOfOrientations();
    testAPhoneKeepsThePlatformsValues();
    testThePictureTurnsByTheChangeSinceTheBoot();
    return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
#include "camera_source.hpp"
#include "require.hpp"


using namespace uml::camera;
using uml::testing::require;

/* The launcher's way to the platform cameras. Nothing here opens a session,
 * asks for access or captures: the host's camera is only discovered, and the
 * permission is only read. */
int main()
{
    const auto phone = makeSyntheticCameraSource(SyntheticProfile::phone);
    const auto tablet = makeSyntheticCameraSource(SyntheticProfile::tablet);
    const auto laptop = makeSyntheticCameraSource(SyntheticProfile::laptop);
    require(phone && tablet && laptop);
    // The profile picks the cameras: a laptop has its front camera alone.
    require(laptop->cameras().size() == 1);
    require(phone->cameras().size() > 1 && tablet->cameras().size() > 1);
    require(phone->cameras().size() != laptop->cameras().size());
    // The phone whose front camera is mounted at 90 is the phone with the host's half turn on its front camera.
    const auto turned = makeSyntheticCameraSource(SyntheticProfile::phoneTurnedFront);
    const auto plainCameras = phone->cameras();
    const auto turnedCameras = turned->cameras();
    require(turnedCameras.size() == plainCameras.size());
    for (std::size_t index = 0; index < plainCameras.size(); ++index) {
        const auto& plain = plainCameras[index];
        const auto& changed = turnedCameras[index];
        const bool front = plain.position == uml::platform::CameraPosition::front;
        require(plain.presentationTurn == 0 && changed.presentationTurn == (front ? 180u : 0u) &&
                changed.sensorOrientation == plain.sensorOrientation);
    }

    // The permission is a read: asking twice gives one answer, and no prompt.
    const bool granted = hostCameraPermissionGranted();
    require(granted == hostCameraPermissionGranted());
    const auto host = makeHostCameraSource();
    require(host != nullptr && !host->hostModel().empty());
    return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
#include "camera_source.hpp"

#include "camera_authorization.hpp"
#include "synthetic_camera/synthetic_camera.hpp"

#include <cstdlib>

namespace uml::camera {

bool hostCameraPermissionGranted()
{
    return uml::platform::makeAppleCameraAuthorization()->permission() ==
           uml::platform::CameraPermission::granted;
}

CameraSourceHandle makeSyntheticCameraSource(SyntheticProfile profile)
{
    uml::platform::SyntheticCameraOptions options;
    switch (profile) {
    case SyntheticProfile::phone:
        options.profile = uml::platform::SyntheticCameraProfile::phone;
        break;
    case SyntheticProfile::tablet:
        options.profile = uml::platform::SyntheticCameraProfile::tablet;
        break;
    case SyntheticProfile::laptop:
        options.profile = uml::platform::SyntheticCameraProfile::laptop;
        break;
    case SyntheticProfile::phoneTurnedFront:
        options.profile = uml::platform::SyntheticCameraProfile::phone;
        options.frontPresentedAt90 = true;
        break;
    }
    if (const char* control = std::getenv(syntheticControlVariable); control != nullptr) options.controlPath = control;
    return uml::platform::makeSyntheticCamera(options);
}

CameraSourceHandle makeHostCameraSource()
{
    return uml::platform::makeAppleCamera();
}

} // namespace uml::camera

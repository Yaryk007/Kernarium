// SPDX-License-Identifier: GPL-2.0-or-later

#include "camera_authorization.hpp"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <memory>
#include <utility>

namespace uml::platform {
namespace {

CameraPermission current() noexcept
{
    @try {
        switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
        case AVAuthorizationStatusAuthorized:
            return CameraPermission::granted;
        case AVAuthorizationStatusNotDetermined:
            return CameraPermission::undetermined;
        default:
            // Denied, or restricted by a policy the user cannot lift here.
            return CameraPermission::denied;
        }
    } @catch (NSException*) {
        return CameraPermission::denied;
    }
}

class AppleCameraAuthorization final : public CameraAuthorization {
public:
    [[nodiscard]] CameraPermission permission() const noexcept override
    {
        return current();
    }

    void request(std::function<void(CameraPermission)> completion) override
    {
        // An answer that is already given is never asked for again, so no
        // prompt can appear for a setting that was turned on before.
        if (const auto answered = current(); answered != CameraPermission::undetermined) {
            completion(answered);
            return;
        }
        const auto shared = std::make_shared<std::function<void(CameraPermission)>>(std::move(completion));
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                 completionHandler:^(BOOL granted) {
                                     (*shared)(granted ? CameraPermission::granted : CameraPermission::denied);
                                 }];
    }
};

} // namespace

std::shared_ptr<CameraAuthorization> makeAppleCameraAuthorization()
{
    return std::make_shared<AppleCameraAuthorization>();
}

} // namespace uml::platform

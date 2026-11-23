// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Foundation/Foundation.h>

#include <array>
#include <cstddef>
#include <optional>

namespace uml::platform::apple {

// The angles a rotation coordinator states for four interface orientations (AVCaptureVideoOrientation's
// numbers, in the order the caller gives them), or none where the object does not take the call.
//
// -[AVCaptureDeviceRotationCoordinator videoRotationAngleRelativeToDeviceOrientation:] is unavailable on macOS and
// visionOS. An iPad app on either has the class and not the method, and sending it raises an Objective-C exception.
// `catch (const std::exception&)` does not catch that: a start with a catch-all above it fails, and a caller with none
// is ended. So the object is asked what it can do, not the host what it is; where it takes the call (an iPhone or an
// iPad) the angles are the ones it states. The selector is looked up by name, so the code builds on an SDK that does
// not declare it, and a test can give it a stand-in object.
inline std::optional<std::array<double, 4>> rotationAnglesOf(id coordinator, const std::array<NSInteger, 4>& orientations)
{
    const SEL selector = NSSelectorFromString(@"videoRotationAngleRelativeToDeviceOrientation:");
    if (coordinator == nil || ![coordinator respondsToSelector:selector]) return std::nullopt;
    const auto angleFor = reinterpret_cast<double (*)(id, SEL, NSInteger)>([coordinator methodForSelector:selector]);
    std::array<double, 4> angles {};
    for (std::size_t index = 0; index < angles.size(); ++index) angles[index] = angleFor(coordinator, selector, orientations[index]);
    return angles;
}

} // namespace uml::platform::apple

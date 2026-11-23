// SPDX-License-Identifier: GPL-2.0-or-later
// A platform call a host lacks, in camera discovery (an iPad app on a Mac or on a Vision Pro meets one: the rotation
// coordinator's angles are unavailable there). An object that does not take the call is not sent it, and one that does
// states its angles in the order asked (where an iPhone or an iPad takes it nothing changes). A discovery that raises
// an Objective-C exception anyway presents no cameras and says so on one line, where `catch (const std::exception&)`
// would not have caught it: a start with a catch-all would have failed and a caller with none would have been ended.
// The platform's raise is stood in by a replacement of the discovery session's device list for the length of the
// call; no camera is opened.
#import <AVFoundation/AVFoundation.h>
#import <objc/runtime.h>

#include "camera.hpp"
#include "require.hpp"
#include "rotation_angles.h"

#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <unistd.h>

using namespace uml::platform;
using uml::testing::require;

// What a coordinator that takes the call states: a number of its own for each orientation asked, so that the order
// the angles come back in is the order of the question.
@interface TakesTheCall : NSObject
- (double)videoRotationAngleRelativeToDeviceOrientation:(NSInteger)orientation;
@end
@implementation TakesTheCall
- (double)videoRotationAngleRelativeToDeviceOrientation:(NSInteger)orientation
{
    return double(orientation) * 90.0 + 0.5;
}
@end

// A coordinator that does not (the class an iPad app on a Mac has): sending it the selector raises, which ends this
// test, so a pass is also a proof that the selector was never sent.
@interface LacksTheCall : NSObject
@end
@implementation LacksTheCall
@end

namespace {

// Everything `body` writes to standard error.
template <class Body>
std::string standardErrorOf(Body&& body)
{
    std::fflush(stderr);
    const int saved = dup(STDERR_FILENO);
    std::FILE* file = std::tmpfile();
    require(saved >= 0 && file != nullptr, "a place to catch standard error");
    dup2(fileno(file), STDERR_FILENO);
    body();
    std::fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    std::rewind(file);
    std::string text;
    char buffer[512];
    for (std::size_t count; (count = std::fread(buffer, 1, sizeof buffer, file)) > 0;) text.append(buffer, count);
    std::fclose(file);
    return text;
}

std::size_t linesOf(const std::string& text)
{
    std::size_t lines = 0;
    for (const char character : text) lines += character == '\n';
    return lines;
}

void aDiscoveryThatRaisesPresentsNoCameras()
{
    // The session a host makes is an instance of a class of its own (a subclass or a private variant), so the list is
    // replaced on the class the host's session has.
    AVCaptureDeviceDiscoverySession* sample =
        [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeBuiltInWideAngleCamera ]
                                                               mediaType:AVMediaTypeVideo
                                                                position:AVCaptureDevicePositionUnspecified];
    Method method = class_getInstanceMethod(object_getClass(sample), @selector(devices));
    require(method != nullptr, "the discovery session lists devices");
    const IMP original = method_getImplementation(method);
    const IMP raising = imp_implementationWithBlock(^NSArray*(id) {
        [NSException raise:@"UMLForcedDiscoveryException" format:@"forced by the test"];
        return nil;
    });
    method_setImplementation(method, raising);
    std::unique_ptr<Camera> camera;
    const std::string raised = standardErrorOf([&] { camera = makeAppleCamera(); });
    method_setImplementation(method, original);
    imp_removeBlock(raising);

    require(camera != nullptr, "a host whose discovery raised still has a camera interface");
    require(linesOf(raised) == 1 &&
                raised.find("Camera: discovery raised UMLForcedDiscoveryException (forced by the test): "
                            "the host presents no cameras\n") == 0,
            "one line says why: " + raised);
    require(camera->cameras().empty() && camera->concurrentSets().empty(), "and it lists no cameras");

    // The replacement is gone: the next discovery is the host's own and raises nothing.
    const std::string quiet = standardErrorOf([&] { camera = makeAppleCamera(); });
    require(camera != nullptr && quiet.find("discovery raised") == std::string::npos, "the host's own discovery is untouched");
}

void aCoordinatorStatesItsAnglesWhereItTakesTheCallAndNowhereElse()
{
    const std::array<NSInteger, 4> asked {1, 2, 3, 4};
    const auto angles = uml::platform::apple::rotationAnglesOf([TakesTheCall new], asked);
    require(angles.has_value(), "a coordinator that takes the call states its angles");
    require((*angles)[0] == 90.5 && (*angles)[1] == 180.5 && (*angles)[2] == 270.5 && (*angles)[3] == 360.5,
            "in the order of the orientations asked");
    const std::array<NSInteger, 4> reversed {4, 3, 2, 1};
    const auto turned = uml::platform::apple::rotationAnglesOf([TakesTheCall new], reversed);
    require(turned.has_value() && (*turned)[0] == 360.5 && (*turned)[3] == 90.5, "whatever the order asked");
    require(!uml::platform::apple::rotationAnglesOf([LacksTheCall new], asked).has_value(),
            "a coordinator that does not take the call states none");
    require(!uml::platform::apple::rotationAnglesOf(nil, asked).has_value(), "no coordinator states none");
}

} // namespace

int main()
{
    aCoordinatorStatesItsAnglesWhereItTakesTheCallAndNowhereElse();
    aDiscoveryThatRaisesPresentsNoCameras();
    return 0;
}

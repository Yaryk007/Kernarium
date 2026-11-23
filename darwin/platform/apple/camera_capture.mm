// SPDX-License-Identifier: GPL-2.0-or-later
//
// The platform Camera over AVFoundation. Discovery reads devices and formats
// and needs no authorization. A session is one AVCaptureMultiCamSession on iOS
// (an AVCaptureSession on macOS, which has no multi-camera session) with one
// input, one video data output and, once a still is asked for, one photo
// output per running camera.

#include "accelerated_nv12.hpp"
#include "camera.hpp"
#include "dng_color.hpp"
#include "frame_bytes.hpp"
#include "key_path_observer.h"
#include "photo_metadata.hpp"
#include "photo_dimensions.hpp"
#include "rotation_angles.h"
#include "still_exposure.hpp"
#include "turned_still.hpp"
#include "video_encoder.hpp"
#include "video_stabilization.hpp"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <TargetConditionals.h>
#include <sys/sysctl.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uml::platform {
namespace {

std::uint64_t hostNow() noexcept
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

std::string stringOf(NSString* value)
{
    return value ? std::string(value.UTF8String) : std::string();
}

#if TARGET_OS_IOS
// What a connection says of its angle and mirroring (camera_presentation.hpp): its own by default, and as it is now.
struct ConnectionFacts {
    std::int16_t defaultAngle {-1};
    std::int16_t angle {-1};
    std::int8_t defaultMirrored {-1};
    std::int8_t mirrored {-1};
    // Whether the connection takes the angle that was asked of it.
    bool angleSupported {};
};

std::int16_t angleNumber(CGFloat angle)
{
    return std::isfinite(angle) ? std::int16_t(std::clamp<long>(std::lround(angle), -32768, 32767)) : std::int16_t(-1);
}

std::int8_t mirroringNumber(AVCaptureConnection* connection)
{
    return connection.supportsVideoMirroring ? std::int8_t(connection.videoMirrored ? 1 : 0) : std::int8_t(-1);
}

// Sets a connection's pictures to the camera's own orientation turned by `turn`, unmirrored, which its stated
// SENSOR_ORIENTATION describes (presentedMountingOf): a device may turn or mirror a camera's pictures by default (some
// iPads turn their front camera's video by 180 degrees). A connection that refuses keeps what it has, which the facts
// say. A video data output physically turns its buffers by the angle, so the angle is set before the output's size.
// Without `setAngle` the angle stays what the connection has, and only the mirroring is turned off.
ConnectionFacts keepSensorOrientation(AVCaptureConnection* connection, std::uint32_t turn, bool setAngle = true)
{
    ConnectionFacts facts;
    if (connection == nil) return facts;
    @try {
        facts.defaultAngle = angleNumber(connection.videoRotationAngle);
        facts.defaultMirrored = mirroringNumber(connection);
        if (connection.supportsVideoMirroring) {
            connection.automaticallyAdjustsVideoMirroring = NO;
            connection.videoMirrored = NO;
        }
        if (setAngle) {
            facts.angleSupported = [connection isVideoRotationAngleSupported:CGFloat(turn)];
            if (connection.videoRotationAngle != CGFloat(turn) && facts.angleSupported) connection.videoRotationAngle = CGFloat(turn);
        } else {
            facts.angleSupported = [connection isVideoRotationAngleSupported:connection.videoRotationAngle];
        }
        facts.angle = angleNumber(connection.videoRotationAngle);
        facts.mirrored = mirroringNumber(connection);
    } @catch (NSException* exception) {
        std::fprintf(stderr, "Camera: a connection kept its own orientation: %s\n",
                     exception.reason != nil ? exception.reason.UTF8String : "");
    }
    return facts;
}

// What a photo output says of its stills' orientation: the compensation (the output turning its processed stills to an
// earlier model's sensor orientation) and the photo connection.
struct PhotoOutputFacts {
    CameraFact compensationSupported {};
    CameraFact compensationEnabled {};
    // Whether the compensation was read before or after the capture session's configuration was committed.
    CameraReading reading {};
    ConnectionFacts connection;
};

PhotoOutputFacts readCompensation(AVCapturePhotoOutput* output, CameraReading reading)
{
    PhotoOutputFacts facts;
    facts.reading = reading;
    @try {
        const bool supported = output.cameraSensorOrientationCompensationSupported;
        facts.compensationSupported = supported ? CameraFact::yes : CameraFact::no;
        facts.compensationEnabled = supported && output.cameraSensorOrientationCompensationEnabled ? CameraFact::yes : CameraFact::no;
    } @catch (NSException*) {
        // The facts stay unknown.
    }
    return facts;
}

// The same for a photo output: where the camera does not imitate an earlier front camera its stills stay in the camera's
// own orientation, without the turn to an earlier model's sensor orientation that the output otherwise applies where it
// supports one. A camera that does imitate one (presentedMountingOf) needs that turn: its compensation is asked on where
// the output says it is supported, which is a reading that is only known to hold once the configuration is committed,
// so a still reads it again then and refuses on that reading alone (startStill). The host's Exif, which would state a
// turn, is not passed on (jpeg_gap.hpp): the photo connection is set to angle 0, unmirrored, where an angle changes the
// Exif alone, and a camera that imitates one keeps its connection's own angle (photoConnectionTurnOf).
PhotoOutputFacts keepStillOrientation(AVCapturePhotoOutput* output, const PresentedMounting& mounting)
{
    @try {
        if (output.cameraSensorOrientationCompensationSupported) {
            if (mounting.emulatesEarlierFront) output.cameraSensorOrientationCompensationEnabled = YES;
            else output.cameraSensorOrientationCompensationEnabled = NO;
        }
    } @catch (NSException* exception) {
        std::fprintf(stderr, "Camera: a photo output kept its orientation compensation: %s\n",
                     exception.reason != nil ? exception.reason.UTF8String : "");
    }
    PhotoOutputFacts facts = readCompensation(output, CameraReading::beforeCommit);
    const std::optional<std::uint32_t> turn = photoConnectionTurnOf(mounting);
    facts.connection = keepSensorOrientation([output connectionWithMediaType:AVMediaTypeVideo], turn.value_or(0), turn.has_value());
    return facts;
}

// The dynamic aspect ratios a device format lists (iOS 26), as the mask of aspectRatioBit.
std::uint8_t aspectRatiosOf(AVCaptureDeviceFormat* format)
{
    std::uint8_t mask = 0;
    for (AVCaptureAspectRatio ratio in format.supportedDynamicAspectRatios) {
        if ([ratio isEqualToString:AVCaptureAspectRatio1x1]) mask |= aspectRatioBit(CameraAspectRatio::r1x1);
        else if ([ratio isEqualToString:AVCaptureAspectRatio16x9]) mask |= aspectRatioBit(CameraAspectRatio::r16x9);
        else if ([ratio isEqualToString:AVCaptureAspectRatio9x16]) mask |= aspectRatioBit(CameraAspectRatio::r9x16);
        else if ([ratio isEqualToString:AVCaptureAspectRatio4x3]) mask |= aspectRatioBit(CameraAspectRatio::r4x3);
        else if ([ratio isEqualToString:AVCaptureAspectRatio3x4]) mask |= aspectRatioBit(CameraAspectRatio::r3x4);
    }
    return mask;
}

AVCaptureAspectRatio avRatioOf(CameraAspectRatio ratio)
{
    switch (ratio) {
    case CameraAspectRatio::r1x1: return AVCaptureAspectRatio1x1;
    case CameraAspectRatio::r16x9: return AVCaptureAspectRatio16x9;
    case CameraAspectRatio::r9x16: return AVCaptureAspectRatio9x16;
    case CameraAspectRatio::r4x3: return AVCaptureAspectRatio4x3;
    case CameraAspectRatio::r3x4: return AVCaptureAspectRatio3x4;
    case CameraAspectRatio::none: break;
    }
    return nil;
}

CameraAspectRatio ratioOfAV(AVCaptureAspectRatio ratio)
{
    for (const auto candidate : {CameraAspectRatio::r1x1, CameraAspectRatio::r16x9, CameraAspectRatio::r9x16, CameraAspectRatio::r4x3,
                                 CameraAspectRatio::r3x4})
        if (ratio != nil && [ratio isEqualToString:avRatioOf(candidate)]) return candidate;
    return CameraAspectRatio::none;
}

CameraDeviceType deviceTypeOf(AVCaptureDeviceType type)
{
    if ([type isEqualToString:AVCaptureDeviceTypeBuiltInWideAngleCamera]) return CameraDeviceType::wide;
    if ([type isEqualToString:AVCaptureDeviceTypeBuiltInUltraWideCamera]) return CameraDeviceType::ultraWide;
    if ([type isEqualToString:AVCaptureDeviceTypeBuiltInTelephotoCamera]) return CameraDeviceType::telephoto;
    if ([type isEqualToString:AVCaptureDeviceTypeBuiltInDualCamera]) return CameraDeviceType::dual;
    if ([type isEqualToString:AVCaptureDeviceTypeBuiltInDualWideCamera]) return CameraDeviceType::dualWide;
    if ([type isEqualToString:AVCaptureDeviceTypeBuiltInTripleCamera]) return CameraDeviceType::triple;
    if ([type isEqualToString:AVCaptureDeviceTypeExternal]) return CameraDeviceType::external;
    if (@available(iOS 27.1, *)) {
        if ([type isEqualToString:AVCaptureDeviceTypeBuiltInOuterUltraWideCamera]) return CameraDeviceType::outerUltraWide;
        if ([type isEqualToString:AVCaptureDeviceTypeBuiltInInnerUltraWideCamera]) return CameraDeviceType::innerUltraWide;
    }
    return CameraDeviceType::other;
}

// The capture connection's mode for a stabilization choice (video_stabilization.hpp).
AVCaptureVideoStabilizationMode stabilizationModeOf(VideoStabilization choice)
{
    switch (choice) {
    case VideoStabilization::lowLatency: return AVCaptureVideoStabilizationModeLowLatency;
    case VideoStabilization::standard: return AVCaptureVideoStabilizationModeStandard;
    case VideoStabilization::off: break;
    }
    return AVCaptureVideoStabilizationModeOff;
}
#endif

[[maybe_unused]] CameraControlState controlState(bool adjusting, bool locked)
{
    if (adjusting) return CameraControlState::searching;
    return locked ? CameraControlState::locked : CameraControlState::converged;
}

#if TARGET_OS_IOS
// The conversion between a lens position and dioptres. The lens position runs
// from 0, the nearest focus, to 1, infinity, without a calibration, so the
// dioptres are an approximation that the nearest focus distance anchors.
float dioptresOf(float lensPosition, float minimumFocusMillimetres)
{
    if (minimumFocusMillimetres <= 0) return 0.0f;
    return (1.0f - std::clamp(lensPosition, 0.0f, 1.0f)) * 1000.0f / minimumFocusMillimetres;
}

float lensPositionOf(float dioptres, float minimumFocusMillimetres)
{
    if (minimumFocusMillimetres <= 0) return 1.0f;
    return std::clamp(1.0f - dioptres * minimumFocusMillimetres / 1000.0f, 0.0f, 1.0f);
}
#endif

struct Entry {
    AVCaptureDevice* device {};
    CameraInfo info;
    // AVFoundation's zoom factor times this is the ratio Android sees.
    float zoomMultiplier {1.0f};
    // How the camera is presented (presentedMountingOf): the orientation stated, the turn of its video connection and
    // whether it imitates an earlier front camera. A Mac's camera states 0 and takes no turn.
    PresentedMounting mounting;
    // What discovery noted of the camera, for the session record (camera_presentation.hpp); the camera's anchor joins it
    // when the session is given one.
    CameraPresentation discovered;
};

[[maybe_unused]] BayerOrder bayerOrderOf(OSType format)
{
    switch (format) {
    case kCVPixelFormatType_14Bayer_RGGB: return BayerOrder::rggb;
    case kCVPixelFormatType_14Bayer_GRBG: return BayerOrder::grbg;
    case kCVPixelFormatType_14Bayer_GBRG: return BayerOrder::gbrg;
    case kCVPixelFormatType_14Bayer_BGGR: return BayerOrder::bggr;
    default: return BayerOrder::none;
    }
}

// The photo sizes a device format lists (supportedMaxPhotoDimensions) that a
// synchronous capture makes. AVFoundation services the 24 MP setting
// (5712x4284), where a format lists it, only as a deferred photo proxy, which
// this backend neither opts into nor receives (AVCapturePhotoOutput.h), so the
// size is not listed and no tier can offer it.
std::vector<CameraStillSize> listedPhotoSizes(AVCaptureDeviceFormat* format)
{
    constexpr CameraStillSize deferredOnly {5712, 4284};
    std::vector<CameraStillSize> listed;
    for (NSValue* value in format.supportedMaxPhotoDimensions) {
        CMVideoDimensions candidate;
        [value getValue:&candidate];
        if (candidate.width <= 0 || candidate.height <= 0) continue;
        const CameraStillSize size {std::uint32_t(candidate.width), std::uint32_t(candidate.height)};
        if (!(size == deferredOnly)) listed.push_back(size);
    }
    return listed;
}

[[nodiscard]] bool formatTakenByMultiCameraSession(AVCaptureDeviceFormat* format)
{
#if TARGET_OS_IOS
    return format != nil && format.multiCamSupported;
#else
    (void)format;
    return true;
#endif
}

// A camera's 4:2:0 formats as data for the choice of its anchor, with the device
// formats they stand for in the same order.
struct FormatSet {
    std::vector<VideoFormatFacts> facts;
    std::vector<AVCaptureDeviceFormat*> formats;
};

// A 4:2:0 device format as data; nothing for any other. `zoomScale` turns the device's zoom factors into
// Android's ratios (zoomRatioScale).
std::optional<VideoFormatFacts> factsOf(AVCaptureDeviceFormat* format, float zoomScale)
{
    const FourCharCode subtype = CMFormatDescriptionGetMediaSubType(format.formatDescription);
    if (subtype != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange && subtype != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
        return std::nullopt;
    const CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
    if (dimensions.width <= 0 || dimensions.height <= 0) return std::nullopt;
    VideoFormatFacts facts;
    facts.width = std::uint32_t(dimensions.width);
    facts.height = std::uint32_t(dimensions.height);
    for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges)
        facts.frameRates.push_back({range.minFrameRate, range.maxFrameRate});
    facts.photoSizes = listedPhotoSizes(format);
    facts.fullRange = subtype == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    facts.multiCamera = formatTakenByMultiCameraSession(format);
    // The zoom the system recommends for the format, where it gives one: the camera's stated maximum
    // (arrangeCameras).
    if (AVZoomRange* recommended = format.systemRecommendedVideoZoomRange)
        facts.recommendedZoomMaximum = float(recommended.maxZoomFactor) * zoomScale;
    // The zoom the format itself delivers: a camera with no recommendation is stated up to it, not to the device's
    // largest over all its formats.
#if TARGET_OS_IOS
    facts.maximumZoom = float(format.videoMaxZoomFactor) * zoomScale;
    facts.aspectRatios = aspectRatiosOf(format);
#endif
    return facts;
}

// The 4:2:0 formats of a device as the guest is offered them (presentedFormatOf): a format that lists dynamic aspect
// ratios is described by the crop the device is pinned at, for a video connection turned by `connectionTurn`, or left
// out when it cannot be described (the line says which, once). The zoom is in the terms of `zoomScale`.
FormatSet formatSetOf(AVCaptureDevice* device, float zoomScale, std::uint32_t connectionTurn)
{
    FormatSet set;
    for (AVCaptureDeviceFormat* format in device.formats) {
        auto facts = factsOf(format, zoomScale);
        if (!facts) continue;
        auto presented = presentedFormatOf(*facts, connectionTurn);
        if (!presented) {
            char line[192];
            std::snprintf(line, sizeof(line), "Camera: a %ux%u format that lists aspect ratios (mask %u) cannot be described for a turn of %u: not offered",
                          facts->width, facts->height, unsigned(facts->aspectRatios), connectionTurn);
            static std::mutex mutex;
            static std::set<std::string> logged;
            std::lock_guard lock(mutex);
            if (logged.insert(line).second) std::fprintf(stderr, "%s\n", line);
            continue;
        }
        set.facts.push_back(std::move(*presented));
        set.formats.push_back(format);
    }
    return set;
}

// What a video output delivers: NV12, at the size the stream needs. The
// anchor's own size arrives unless it decides another (outputSizesFor: the
// anchor's aspect ratio).
NSDictionary* videoSettingsOf(const VideoFormatFacts& facts, std::uint32_t width, std::uint32_t height);

// Sets what a video output delivers. An output left to configure its own buffer dimensions may have been set to
// deliver preview-sized buffers (the platform decides from the session's other outputs, a photo output of the full
// sensor size among them), and a size set then raises ("Width/height may not be changed when
// deliversPreviewSizedOutputBuffers is set to YES"): such an output is made to deliver the size asked for first. One
// that already delivers full-size buffers is left as the platform configured it.
void setVideoOutputSettings(AVCaptureVideoDataOutput* output, NSDictionary* settings)
{
#if TARGET_OS_IOS
    if (output.deliversPreviewSizedOutputBuffers) {
        std::fprintf(stderr, "Camera: a stream's output delivered preview-sized buffers: set to deliver the size asked for\n");
        output.automaticallyConfiguresOutputBufferDimensions = NO;
        output.deliversPreviewSizedOutputBuffers = NO;
    }
#endif
    output.videoSettings = settings;
}

NSDictionary* videoSettingsOf(const VideoFormatFacts& facts, std::uint32_t width, std::uint32_t height)
{
    NSMutableDictionary* settings = [NSMutableDictionary dictionaryWithObject:@(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
                                                                      forKey:(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey];
    if (const auto frame = outputSizesFor(facts, width, height, 0).frame) {
        settings[(__bridge NSString*)kCVPixelBufferWidthKey] = @(frame->width);
        settings[(__bridge NSString*)kCVPixelBufferHeightKey] = @(frame->height);
    }
    return settings;
}

// One line per camera, once for the life of the process: the device facts the
// anchor is chosen from (a format's size, its rate spans, `m` when a
// multi-camera session takes it, `f` when it is full range, `z` and the zoom
// the system recommends for it, and the photo sizes it lists in braces). No
// identifier and no frame data.
void logFormatTables(const std::vector<Entry>& entries)
{
    static std::mutex mutex;
    static std::set<std::string> logged;
    std::lock_guard lock(mutex);
    for (std::size_t camera = 0; camera < entries.size(); ++camera) {
        if (!logged.insert(entries[camera].info.identifier).second) continue;
        std::string line = "Camera: camera " + std::to_string(camera) + " formats:";
        char text[64];
        for (const auto& format : entries[camera].info.captureFormats) {
            line += " " + std::to_string(format.width) + "x" + std::to_string(format.height);
            for (const auto& span : format.frameRates) {
                std::snprintf(text, sizeof(text), " %g-%g", span.minimum, span.maximum);
                line += text;
            }
            if (format.multiCamera) line += " m";
            if (format.fullRange) line += " f";
            if (format.recommendedZoomMaximum > 0) {
                std::snprintf(text, sizeof(text), " z%g", format.recommendedZoomMaximum);
                line += text;
            }
            line += " {";
            for (std::size_t index = 0; index < format.photoSizes.size(); ++index)
                line += (index != 0 ? " " : "") + std::to_string(format.photoSizes[index].width) + "x" +
                    std::to_string(format.photoSizes[index].height);
            line += "};";
        }
        std::fprintf(stderr, "%s\n", line.c_str());
        // How the camera is presented (presentedMountingOf): the rotation coordinator's angles for portrait, portrait upside
        // down, landscape right and landscape left, the orientation stated, and the turn set on the video connection.
        const CameraPresentation& mounted = entries[camera].discovered;
        std::fprintf(stderr, "Camera: camera %zu mounting: angles %d %d %d %d, stated %u, video turn %u, picture turn %u%s\n", camera,
                     int(mounted.coordinatorAngles[0]), int(mounted.coordinatorAngles[1]), int(mounted.coordinatorAngles[2]),
                     int(mounted.coordinatorAngles[3]), unsigned(mounted.stated), unsigned(mounted.turn),
                     unsigned(mounted.presentationTurn), mounted.earlier ? " (presented like an earlier front camera)" : "");
    }
}

std::vector<Entry> discover()
{
    NSMutableArray<AVCaptureDeviceType>* types =
        [NSMutableArray arrayWithObjects:AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal, nil];
#if TARGET_OS_IOS
    [types addObjectsFromArray:@[
        AVCaptureDeviceTypeBuiltInUltraWideCamera, AVCaptureDeviceTypeBuiltInTelephotoCamera,
        AVCaptureDeviceTypeBuiltInDualCamera, AVCaptureDeviceTypeBuiltInDualWideCamera,
        AVCaptureDeviceTypeBuiltInTripleCamera
    ]];
    // The foldable's two ultra wide cameras exist from iOS 27.1; on 27.0 the
    // constants are null, so they are named only where they exist. The
    // application's Camera row asks the same types for a front camera
    // (Andarium's SystemCameraSource.hasFrontCamera).
    if (@available(iOS 27.1, *)) {
        [types addObjectsFromArray:@[
            AVCaptureDeviceTypeBuiltInOuterUltraWideCamera, AVCaptureDeviceTypeBuiltInInnerUltraWideCamera
        ]];
    }
#endif
    AVCaptureDeviceDiscoverySession* session =
        [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                               mediaType:AVMediaTypeVideo
                                                                position:AVCaptureDevicePositionUnspecified];
    // The front devices the platform lists, whether the guest uses them or not, and whether any has a square format or a
    // format that lists dynamic aspect ratios: what the record says of how a front camera is built.
    std::uint8_t frontDevices = 0;
    bool squareFormats = false, dynamicRatios = false;
    for (AVCaptureDevice* device in session.devices) {
        if (device.position != AVCaptureDevicePositionFront) continue;
        frontDevices = std::uint8_t(std::min(255, frontDevices + 1));
        for (AVCaptureDeviceFormat* format in device.formats) {
            const CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
            squareFormats = squareFormats || (dimensions.width > 0 && dimensions.width == dimensions.height);
#if TARGET_OS_IOS
            dynamicRatios = dynamicRatios || format.supportedDynamicAspectRatios.count != 0;
#endif
        }
    }
    // The formats of the front devices that are not offered because they list ratios and cannot be described: what is left
    // of a front device whose formats are all dropped, which is then no camera.
    std::uint16_t frontFormatsDropped = 0;
    std::vector<Entry> entries;
    for (AVCaptureDevice* device in session.devices) {
        Entry entry;
        entry.device = device;
        auto& info = entry.info;
        info.identifier = stringOf(device.uniqueID);
        switch (device.position) {
        case AVCaptureDevicePositionBack: info.position = CameraPosition::back; break;
        case AVCaptureDevicePositionFront: info.position = CameraPosition::front; break;
        default: info.position = CameraPosition::external; break;
        }
        const AVCaptureDeviceType type = device.deviceType;
        info.lens = CameraLensKind::external;
        if ([type isEqualToString:AVCaptureDeviceTypeBuiltInWideAngleCamera]) info.lens = CameraLensKind::wide;
#if TARGET_OS_IOS
        bool ultraWide = [type isEqualToString:AVCaptureDeviceTypeBuiltInUltraWideCamera];
        if (@available(iOS 27.1, *))
            ultraWide = ultraWide || [type isEqualToString:AVCaptureDeviceTypeBuiltInOuterUltraWideCamera] ||
                [type isEqualToString:AVCaptureDeviceTypeBuiltInInnerUltraWideCamera];
        if (ultraWide) info.lens = CameraLensKind::ultraWide;
        else if ([type isEqualToString:AVCaptureDeviceTypeBuiltInTelephotoCamera]) info.lens = CameraLensKind::telephoto;
        else if (device.virtualDevice) info.lens = CameraLensKind::zoom;
        if (info.position == CameraPosition::external) info.lens = CameraLensKind::external;
#endif
        info.autofocus = [device isFocusModeSupported:AVCaptureFocusModeContinuousAutoFocus];
        // A front camera's flash is the screen's, which no guest camera unit models.
        info.flash = device.hasFlash && info.position != CameraPosition::front;
        info.torch = device.hasTorch;
        const bool zoomCamera = info.lens == CameraLensKind::zoom;
        entry.mounting = {info.sensorOrientation, 0, false};
        entry.discovered.position = info.position;
        entry.discovered.frontDevices = frontDevices;
        entry.discovered.squareFormats = squareFormats;
        entry.discovered.dynamicRatios = dynamicRatios;
#if TARGET_OS_IOS
        entry.discovered.deviceType = deviceTypeOf(type);
        // The platform states each camera's mounting, which is what the guest is told (presentedMountingOf); the
        // connections present the pictures in it (keepSensorOrientation). Its fixed angle for each interface
        // orientation is noted for the record. Where the coordinator does not take the call (an iPad app on a Mac or on a
        // Vision Pro: it is unavailable there and raises), the camera keeps the mounting its sensor states, as a Mac's own
        // do.
        AVCaptureDeviceRotationCoordinator* rotation = [[AVCaptureDeviceRotationCoordinator alloc] initWithDevice:device previewLayer:nil];
        if (const auto angles = apple::rotationAnglesOf(rotation, {AVCaptureVideoOrientationPortrait, AVCaptureVideoOrientationPortraitUpsideDown,
                                                                     AVCaptureVideoOrientationLandscapeRight, AVCaptureVideoOrientationLandscapeLeft})) {
            for (std::size_t index = 0; index < angles->size(); ++index) entry.discovered.coordinatorAngles[index] = angleNumber((*angles)[index]);
            entry.mounting = presentedMountingOf(info.position, (*angles)[0]);
            info.sensorOrientation = entry.mounting.stated;
            info.presentationTurn = entry.mounting.presentationTurn;
        }
#endif
        entry.discovered.stated = std::uint16_t(entry.mounting.stated);
        entry.discovered.turn = std::uint16_t(entry.mounting.connectionTurn);
        entry.discovered.presentationTurn = std::uint16_t(entry.mounting.presentationTurn);
        // A camera whose stills are turned half around (turnedStillOf) needs the hardware encoder's capabilities at its
        // first still: discovery starts now, off this thread.
        if (entry.mounting.presentationTurn == 180) prepareVideoEncodeCapabilities();
        entry.discovered.earlier = entry.mounting.emulatesEarlierFront;
#if TARGET_OS_IOS
        // Only a zoom camera's factor is scaled (see zoomRatioScale): every range holds 1.0.
        entry.zoomMultiplier = zoomRatioScale(info.lens, device.displayVideoZoomFactorMultiplier);
#endif

        // Formats: distinct size and frame-rate range pairs of the 4:2:0 formats.
        std::uint32_t pixelWidth = 0, pixelHeight = 0;
        float fieldOfView = 0;
#if TARGET_OS_IOS
        std::uint32_t isoMinimum = 0xffffffffu, isoMaximum = 0;
        double exposureMinimum = 1e9, exposureMaximum = 0;
#endif
        CameraStillSize largestPhoto;
        for (AVCaptureDeviceFormat* format in device.formats) {
            const FourCharCode subtype = CMFormatDescriptionGetMediaSubType(format.formatDescription);
            if (subtype != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange &&
                subtype != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
                continue;
            CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
            if (dimensions.width <= 0 || dimensions.height <= 0) continue;
            bool multiCamera = false;
#if TARGET_OS_IOS
            // A format that lists dynamic aspect ratios is the crop the device is pinned at (presentedFormatOf), with the
            // photos that crop makes and the larger view of its two orientations; one that cannot be described is not
            // listed (formatSetOf does the same).
            std::optional<VideoFormatFacts> pinned;
            if (aspectRatiosOf(format) != 0) {
                const auto listed = factsOf(format, entry.zoomMultiplier);
                pinned = listed ? presentedFormatOf(*listed, entry.mounting.connectionTurn) : std::nullopt;
                if (!pinned) {
                    if (info.position == CameraPosition::front) frontFormatsDropped = std::uint16_t(std::min(65535, frontFormatsDropped + 1));
                    continue;
                }
                dimensions = {std::int32_t(pinned->width), std::int32_t(pinned->height)};
            }
            multiCamera = format.multiCamSupported;
            fieldOfView = std::max(fieldOfView, pinned ? pinnedFieldOfView([format videoFieldOfViewForAspectRatio:AVCaptureAspectRatio3x4 geometricDistortionCorrected:NO],
                                                                         [format videoFieldOfViewForAspectRatio:AVCaptureAspectRatio4x3 geometricDistortionCorrected:NO])
                                                       : format.videoFieldOfView);
            isoMinimum = std::min<std::uint32_t>(isoMinimum, std::uint32_t(std::ceil(format.minISO)));
            isoMaximum = std::max<std::uint32_t>(isoMaximum, std::uint32_t(std::floor(format.maxISO)));
            exposureMinimum = std::min(exposureMinimum, CMTimeGetSeconds(format.minExposureDuration));
            exposureMaximum = std::max(exposureMaximum, CMTimeGetSeconds(format.maxExposureDuration));
#endif
            for (NSValue* value in format.supportedMaxPhotoDimensions) {
                CMVideoDimensions photo;
                [value getValue:&photo];
                if (photo.width <= 0 || photo.height <= 0) continue;
                const CameraStillSize size {std::uint32_t(photo.width), std::uint32_t(photo.height)};
#if TARGET_OS_IOS
                if (pinned && std::find(pinned->photoSizes.begin(), pinned->photoSizes.end(), size) == pinned->photoSizes.end()) continue;
#endif
                if (std::uint64_t(size.width) * size.height > std::uint64_t(largestPhoto.width) * largestPhoto.height)
                    largestPhoto = size;
            }
            if (std::uint64_t(dimensions.width) * dimensions.height > std::uint64_t(pixelWidth) * pixelHeight) {
                pixelWidth = std::uint32_t(dimensions.width);
                pixelHeight = std::uint32_t(dimensions.height);
            }
            for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges) {
                // A rate within 0.15 % of a whole number is that number (the Mac's
                // external camera lists 29.97, 23.976 and 14.99).
                const auto whole = wholeRatesOf({range.minFrameRate, range.maxFrameRate});
                if (!whole) continue;
                CameraVideoFormat candidate;
                candidate.width = std::uint32_t(dimensions.width);
                candidate.height = std::uint32_t(dimensions.height);
                candidate.minimumFrameRate = whole->minimum;
                candidate.maximumFrameRate = whole->maximum;
                candidate.multiCamera = multiCamera;
                if (std::find(info.videoFormats.begin(), info.videoFormats.end(), candidate) == info.videoFormats.end())
                    info.videoFormats.push_back(candidate);
            }
        }
        if (info.videoFormats.empty()) continue;
        // The formats go to the device as data: it picks each camera's anchor and
        // offers what the anchor does (photo_dimensions.hpp).
        {
            const FormatSet set = formatSetOf(device, entry.zoomMultiplier, entry.mounting.connectionTurn);
            info.captureFormats = set.facts;
            // Every size some format makes: what the host can do at all.
            const auto made = stillsMadeByAnyFormat(set.facts, 0);
            info.stillSizes.assign(made.begin(), made.end());
        }
        info.pixelArrayWidth = std::max(pixelWidth, largestPhoto.width);
        info.pixelArrayHeight = std::max(pixelHeight, largestPhoto.height);
        info.horizontalFieldOfView = fieldOfView;

#if TARGET_OS_IOS
        info.isoMinimum = isoMinimum == 0xffffffffu ? 0 : isoMinimum;
        info.isoMaximum = isoMaximum;
        info.exposureMinimumNanoseconds = exposureMinimum >= 1e9 ? 0 : std::uint64_t(exposureMinimum * 1e9);
        info.exposureMaximumNanoseconds = std::uint64_t(exposureMaximum * 1e9);
        info.aperture = device.lensAperture;
        info.minimumFocusDistanceMillimetres = device.minimumFocusDistance > 0 ? float(device.minimumFocusDistance) : 0.0f;
        info.maximumWhiteBalanceGain = device.maxWhiteBalanceGain;
        // A virtual device refuses manual exposure, focus and white balance.
        if (!zoomCamera) {
            info.manualExposure = [device isExposureModeSupported:AVCaptureExposureModeCustom];
            info.manualFocus = info.autofocus && device.lockingFocusWithCustomLensPositionSupported;
            info.manualWhiteBalance = device.lockingWhiteBalanceWithCustomDeviceGainsSupported;
        }
        info.zoomMinimum = float(device.minAvailableVideoZoomFactor) * entry.zoomMultiplier;
        info.zoomMaximum = float(device.maxAvailableVideoZoomFactor) * entry.zoomMultiplier;
        for (NSNumber* factor in device.virtualDeviceSwitchOverVideoZoomFactors)
            info.switchOvers.push_back(factor.floatValue * entry.zoomMultiplier);
        for (AVCaptureDevice* lens in device.constituentDevices) info.constituents.push_back(stringOf(lens.uniqueID));
        // A Bayer still is possible on a single back lens; whether the host
        // delivers one is learned by taking it.
        info.rawStills = !zoomCamera && info.position == CameraPosition::back;
        if (info.rawStills) {
            const std::uint64_t area = std::uint64_t(largestPhoto.width) * largestPhoto.height;
            // 48 MP sensors give 12 MP Bayer.
            const bool binned = area > 24'000'000;
            info.rawStillSize = {binned ? largestPhoto.width / 2 : largestPhoto.width,
                                 binned ? largestPhoto.height / 2 : largestPhoto.height};
        }
#else
        (void)zoomCamera;
#endif
        entries.push_back(std::move(entry));
    }
    for (auto& entry : entries) entry.discovered.frontFormatsDropped = frontFormatsDropped;
    // Back cameras first (the zoom camera, then the lenses), then front, then
    // the rest, each group in a fixed lens order.
    const auto rank = [](const CameraInfo& info) {
        const int position = info.position == CameraPosition::back ? 0 : info.position == CameraPosition::front ? 1 : 2;
        return position * 10 + int(info.lens);
    };
    std::stable_sort(entries.begin(), entries.end(),
                     [&](const Entry& a, const Entry& b) { return rank(a.info) < rank(b.info); });
    logFormatTables(entries);
    return entries;
}

class AppleSession;

// The last note of each camera across the platform sessions of the process, by the device's identifier: a session that
// closes when the cameras are idle and opens again states the mounting again, and the record keeps what the cameras'
// last admissions set (withDiscovery). Its lock is the innermost of the notes' (AppleSession::note).
// It is bounded: the current session's cameras and the most recently used others (heldNotesToDrop).
struct HeldNotes {
    struct Held {
        CameraPresentation note;
        std::uint64_t used {};
    };
    std::mutex mutex;
    std::map<std::string, Held> notes;
    std::uint64_t clock {};
};

HeldNotes& heldNotes()
{
    static HeldNotes held;
    return held;
}

// One camera's output delegate. It forwards to the session through a state
// that outlives it, so a callback that races the session's end finds it gone.
struct SessionState {
    std::mutex mutex;
    AppleSession* session {};
    // Which samples are from before the last repair of the cameras (RepairBoundary): read by the output callbacks
    // as they are entered, set by the recovery, both with `mutex` held for the decision.
    RepairBoundary boundary;
};

} // namespace
} // namespace uml::platform

namespace {
using uml::platform::SessionState;
}

// ---------------------------------------------------------------------------
// Objective-C delegates
// ---------------------------------------------------------------------------

@interface UMLCameraOutputDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
- (instancetype)initWithCamera:(size_t)camera state:(std::shared_ptr<SessionState>)state;
@end

@interface UMLPhotoDelegate : NSObject <AVCapturePhotoCaptureDelegate>
- (instancetype)initWithCamera:(size_t)camera
                         token:(uint64_t)token
                      wantJpeg:(BOOL)wantJpeg
                      jpegSize:(uml::platform::CameraStillSize)jpegSize
                  maximumBytes:(uint64_t)maximumBytes
                       wantRaw:(BOOL)wantRaw
                      turnHalf:(BOOL)turnHalf
                captureIsSrgb:(BOOL)captureIsSrgb
                   jpegQuality:(unsigned)jpegQuality
                       attempt:(unsigned)attempt
                         state:(std::shared_ptr<SessionState>)state;
@end

namespace uml::platform {
namespace {


// The format a camera runs in and what its choice stands on: the device format,
// its facts (equal to what discovery listed) and the rates the guest is offered.
struct Anchor {
    AVCaptureDeviceFormat* format {};
    VideoFormatFacts facts;
    std::vector<WholeRates> rates;
};

// The stream's size and rate (StreamCache) are what the stream was asked for, which the anchor covers, and what the
// backend believes the device holds of them.
struct RunningCamera : StreamCache {
    AVCaptureDeviceInput* input {};
    AVCaptureVideoDataOutput* videoOutput {};
    AVCapturePhotoOutput* photoOutput {};
    // When the session last committed a configuration with a photo maximum that needs the sensor's full-resolution
    // mode (host nanoseconds); zero if there is none (photoPipelinesSettle, stillSettleNanoseconds).
    std::uint64_t photoMaximumCommittedAt {};
    UMLCameraOutputDelegate* delegate {};
    AVCaptureConnection* connection {};
    AVCaptureDeviceFormat* format {};
#if TARGET_OS_IOS
    UMLKeyPathObserver* pressure {};
#endif
    // What the system's pressure leaves of a running stream's rate: made when the
    // stream starts, ended when it stops or its session does. The observer's
    // handler and the work it queues hold it themselves.
    std::shared_ptr<PressureWatch> watch;
    CameraSettings settings;
    std::uint32_t focusSerial {};
    std::uint32_t focusDistanceSerial {};
    // Settings have been applied to the device at least once, so that a format
    // change (which reverts some of them) can put them back.
    bool applied {};
    // What the video connection holds of the stabilization, as this backend wrote it; a new connection holds none.
    VideoStabilizationHeld stabilization;
    // What the guest last asked of it, as the applies of the camera's settings said (also those that left the rest alone).
    StabilizationWish stabilizationWish;
};

// The rate a stream runs at while the system's pressure caps it, and how it is
// programmed on the anchor (programmedRate).
std::optional<ProgrammedRate> programmedFor(const Anchor& anchor, std::uint32_t requested, std::uint32_t pressureCap)
{
    return programmedRate(anchor.facts, anchor.rates, requested, pressureCap);
}

// The cap the system's pressure puts on a camera's stream now.
std::uint32_t pressureCapOf(const RunningCamera& running)
{
    return running.watch ? running.watch->cap() : 0;
}

// What a device holds of its format, frame durations and manual exposure, as the
// device reports them (uml::platform::DeviceTiming): a long manual exposure
// lengthens the maximum beyond what the rate asked for, and a shorter maximum
// shortens the exposure, so a refused change is undone with these, not with a
// duration recomputed from a rate.
CameraTime cameraTimeOf(CMTime time)
{
    return {time.value, time.timescale};
}

CMTime cmTimeOf(CameraTime time)
{
    return CMTimeMake(time.value, time.scale);
}

// A time on the capture's clock, in nanoseconds; nothing for one that is not a number (invalid, indefinite, infinite).
std::optional<std::int64_t> nanosecondsOf(CMTime time)
{
    if (!CMTIME_IS_NUMERIC(time)) return std::nullopt;
    const CMTime scaled = CMTimeConvertScale(time, 1'000'000'000, kCMTimeRoundingMethod_RoundTowardZero);
    return CMTIME_IS_NUMERIC(scaled) ? std::optional<std::int64_t>(scaled.value) : std::nullopt;
}

DeviceTiming timingOf(AVCaptureDevice* device)
{
    DeviceTiming timing;
    const NSUInteger format = [device.formats indexOfObject:device.activeFormat];
    timing.format = format == NSNotFound ? ~0u : std::uint32_t(format);
    timing.minimum = cameraTimeOf(device.activeVideoMinFrameDuration);
    timing.maximum = cameraTimeOf(device.activeVideoMaxFrameDuration);
#if TARGET_OS_IOS
    timing.exposure.custom = device.exposureMode == AVCaptureExposureModeCustom;
    timing.exposure.duration = cameraTimeOf(device.exposureDuration);
    timing.exposure.iso = device.ISO;
#endif
    return timing;
}

// The calls uml::platform::putBackTiming makes on a device.
TimingSteps timingStepsOf(AVCaptureDevice* device)
{
    const auto guarded = [](const std::function<void()>& write) {
        @try {
            write();
            return true;
        } @catch (NSException*) {
            return false;
        }
    };
    TimingSteps steps;
    steps.lock = [device] {
        NSError* error = nil;
        return [device lockForConfiguration:&error] == YES;
    };
    steps.unlock = [device] { [device unlockForConfiguration]; };
    steps.read = [device] { return timingOf(device); };
    steps.setFormat = [device, guarded](std::uint32_t format) {
        return format < device.formats.count && guarded([&] { device.activeFormat = device.formats[format]; });
    };
    // A duration the device did not report (a zero scale: an invalid time) is not written back.
    steps.setMinimum = [device, guarded](CameraTime time) {
        return time.scale <= 0 || guarded([&] { device.activeVideoMinFrameDuration = cmTimeOf(time); });
    };
    steps.setMaximum = [device, guarded](CameraTime time) {
        return time.scale <= 0 || guarded([&] { device.activeVideoMaxFrameDuration = cmTimeOf(time); });
    };
    steps.setCustomExposure = [device, guarded](CameraTime duration, float iso) {
#if TARGET_OS_IOS
        return guarded([&] { [device setExposureModeCustomWithDuration:cmTimeOf(duration) ISO:iso completionHandler:nil]; });
#else
        (void)device;
        (void)guarded;
        (void)duration;
        (void)iso;
        return false;
#endif
    };
    return steps;
}

#if TARGET_OS_IOS
// What activating a device did about its dynamic aspect ratio, for the record: the one pinned, the one the device
// reports afterwards and the output dimensions it reports.
struct RatioFacts {
    bool known {};
    CameraAspectRatio pinned {};
    CameraAspectRatio now {};
    std::uint32_t width {};
    std::uint32_t height {};
};

RatioFacts ratioFactsOf(AVCaptureDevice* device, CameraAspectRatio pinned)
{
    RatioFacts facts;
    facts.known = true;
    facts.pinned = pinned;
    facts.now = ratioOfAV(device.dynamicAspectRatio);
    const CMVideoDimensions dimensions = device.dynamicDimensions;
    facts.width = std::uint32_t(std::max(0, int(dimensions.width)));
    facts.height = std::uint32_t(std::max(0, int(dimensions.height)));
    return facts;
}
#endif

// What a dynamic aspect ratio change says: `ratioPending` as the code when it is asked for, before the device is told;
// else the completion's code, zero or the error's. `changed` is false where the device already held the ratio (nothing was
// asked of it, and no buffer precedes the change); else `syncTime` is the first buffer the change applies to, on the
// device's own clock. It may run on any queue.
struct RatioCompletion {
    std::int32_t code {};
    bool changed {};
    CMTime syncTime {kCMTimeInvalid};
};
using RatioDone = std::function<void(const RatioCompletion&)>;

// Puts a device in its anchor: the format only when it is not the active one
// (setting it again would revert the device's focus and exposure points), the
// dynamic aspect ratio the anchor pins when `pinRatio` (an admission or a repair,
// never a stream that runs: its crop must not change under it) and the device's
// ratio is another, and, for a stream, the frame durations the rate programs.
// False when the device refuses.
bool activate(AVCaptureDevice* device, const Anchor& anchor, const std::optional<ProgrammedRate>& rate, bool pinRatio = false,
              const RatioDone& ratioDone = {})
{
    NSError* error = nil;
    if (![device lockForConfiguration:&error]) return false;
    bool ok = true;
    @try {
        // A format the session does not take raises instead of failing.
        if (device.activeFormat != anchor.format) device.activeFormat = anchor.format;
#if TARGET_OS_IOS
        if (pinRatio && anchor.facts.pinnedRatio != CameraAspectRatio::none) {
            // The change is asked for before the device is told, so that it is under way for whatever reads it, and a completion
            // that comes at once is the change's own; a device that already holds the ratio completes it explicitly.
            RatioDone done = ratioDone;
            if (done) done({ratioPending, true, kCMTimeInvalid});
            if (ratioOfAV(device.dynamicAspectRatio) != anchor.facts.pinnedRatio) {
                [device setDynamicAspectRatio:avRatioOf(anchor.facts.pinnedRatio)
                            completionHandler:^(CMTime syncTime, NSError* completion) {
                    if (done)
                        done({completion == nil ? 0 : std::int32_t(std::clamp<NSInteger>(completion.code, INT32_MIN + 1, INT32_MAX)), true,
                              syncTime});
                }];
            } else if (done) {
                done({0, false, kCMTimeInvalid});
            }
        }
#else
        (void)pinRatio;
        (void)ratioDone;
#endif
        if (rate) {
            CMTime duration = CMTimeMake(1, std::int32_t(rate->rate));
            if (rate->setting.duration != FrameDuration::reciprocal) {
                // The span's own duration, where 1 / the nominal rate would fall
                // a hair outside it.
                AVFrameRateRange* span = anchor.format.videoSupportedFrameRateRanges[rate->setting.span];
                duration = rate->setting.duration == FrameDuration::spanShortest ? span.minFrameDuration : span.maxFrameDuration;
            }
            // The one that moves out of the other's way goes first (maximumFirst): the device may be where a refused
            // change left it, at the durations of a faster stream.
            if (maximumFirst(cameraTimeOf(device.activeVideoMaxFrameDuration), cameraTimeOf(duration))) {
                device.activeVideoMaxFrameDuration = duration;
                device.activeVideoMinFrameDuration = duration;
            } else {
                device.activeVideoMinFrameDuration = duration;
                device.activeVideoMaxFrameDuration = duration;
            }
        }
    } @catch (NSException*) {
        ok = false;
    }
    [device unlockForConfiguration];
    return ok;
}

#if TARGET_OS_IOS
// The photos of a camera whose stills the host turns are encoded with no colour profile, which a reader takes as sRGB, so its
// capture stays in that colour space: selected here where the active format supports it (the session does not choose a
// wide one over it, see AppleSession), and checked on every photo (turned_still.mm), which is refused when it states
// other colours, or none and the capture was not held in sRGB. Whether the device's capture is in sRGB afterwards.
bool keepSrgbColourSpace(AVCaptureDevice* device)
{
    NSError* error = nil;
    if ([device lockForConfiguration:&error]) {
        @try {
            if (device.activeColorSpace != AVCaptureColorSpace_sRGB &&
                [device.activeFormat.supportedColorSpaces containsObject:@(AVCaptureColorSpace_sRGB)])
                device.activeColorSpace = AVCaptureColorSpace_sRGB;
        } @catch (NSException*) {
        }
        [device unlockForConfiguration];
    }
    return device.activeColorSpace == AVCaptureColorSpace_sRGB;
}
#endif

class AppleCamera;

class AppleSession final : public CameraSession {
public:
    AppleSession(const std::vector<Entry>& entries, CameraSessionDelegate& delegate)
        : entries_(entries), delegate_(delegate), state_(std::make_shared<SessionState>())
    {
        state_->session = this;
#if TARGET_OS_IOS
        if (AVCaptureMultiCamSession.multiCamSupported) {
            session_ = [[AVCaptureMultiCamSession alloc] init];
            multiCamera_ = true;
        } else {
            session_ = [[AVCaptureSession alloc] init];
        }
#else
        session_ = [[AVCaptureSession alloc] init];
#endif
        queue_ = dispatch_queue_create("uml.camera.capture", DISPATCH_QUEUE_SERIAL);
        outputQueue_ = dispatch_queue_create("uml.camera.frames", DISPATCH_QUEUE_SERIAL);
        // The map never changes shape afterwards: the session's queue and the
        // thread that calls it reach a camera's state without searching.
        for (std::size_t index = 0; index < entries_.size(); ++index) cameras_[index];
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        auto state = state_;
        const auto post = [state](CameraEvent event) {
            std::lock_guard lock(state->mutex);
            if (state->session) state->session->deliver(event);
        };
        // An interruption that ends, and a restart after an error, are recoveries:
        // the system may have changed a device's format meanwhile, so the session
        // puts its cameras back in their anchors first and only then says it is
        // back (recoverAfterInterruption); the client delivers nothing from a
        // camera until it is.
        const auto recover = [state] {
            std::lock_guard lock(state->mutex);
            if (state->session) state->session->requestRecovery();
        };
#if TARGET_OS_IOS
        // The system interrupts a session that starts while the application is in the background with the same reason
        // as one that was running when the application left, and keeps its start request for the return. Only the
        // second has video to lose: the session reports the departure when frames have reached it since it started
        // or since its last reported departure (deliverFrame sets the flag, the report clears it), and an ordinary
        // interruption otherwise.
        auto ranInForeground = ranInForeground_;
#endif
        interrupted_ = [center addObserverForName:AVCaptureSessionWasInterruptedNotification
                                           object:session_
                                            queue:nil
                                       usingBlock:^(NSNotification* note) {
                                           CameraEventKind kind = CameraEventKind::interrupted;
#if TARGET_OS_IOS
                                           NSNumber* reason = note.userInfo[AVCaptureSessionInterruptionReasonKey];
                                           if (reason.integerValue ==
                                               AVCaptureSessionInterruptionReasonVideoDeviceNotAvailableWithMultipleForegroundApps)
                                               kind = CameraEventKind::interruptedByMultitasking;
                                           else if (reason.integerValue ==
                                                    AVCaptureSessionInterruptionReasonVideoDeviceNotAvailableDueToSystemPressure)
                                               kind = CameraEventKind::interruptedBySystemPressure;
                                           else if (reason.integerValue ==
                                                        AVCaptureSessionInterruptionReasonVideoDeviceNotAvailableInBackground &&
                                                    ranInForeground->exchange(false))
                                               kind = CameraEventKind::interruptedInBackground;
#else
                                           (void)note;
#endif
                                           post({allCameras, kind});
                                       }];
        resumed_ = [center addObserverForName:AVCaptureSessionInterruptionEndedNotification
                                       object:session_
                                        queue:nil
                                   usingBlock:^(NSNotification*) { recover(); }];
        // A restart after a runtime error ends with the session running again,
        // which is no interruption ending: the capture tells its client itself.
        // The blocks hold locals only: a block that read an instance variable
        // would hold this object, which may be gone when the notification comes.
        auto restarting = restarting_;
        AVCaptureSession* observed = session_;
        dispatch_queue_t queue = queue_;
        failed_ = [center addObserverForName:AVCaptureSessionRuntimeErrorNotification
                                      object:session_
                                       queue:nil
                                  usingBlock:^(NSNotification*) {
                                      // A media-services reset is the recoverable case: one
                                      // restart for an episode of errors, which ends when the
                                      // session runs again. A start that fails once more (a
                                      // hardware cost over budget, say) loses the capture; the
                                      // client closes the session and tries later.
                                      if (restarting->exchange(true)) {
                                          post({allCameras, CameraEventKind::failed});
                                          return;
                                      }
                                      post({allCameras, CameraEventKind::interrupted});
                                      dispatch_async(queue, ^{
                                          @try {
                                              if (!observed.running) [observed startRunning];
                                          } @catch (NSException*) {
                                              post({allCameras, CameraEventKind::failed});
                                          }
                                      });
                                  }];
        started_ = [center addObserverForName:AVCaptureSessionDidStartRunningNotification
                                       object:session_
                                        queue:nil
                                   usingBlock:^(NSNotification*) {
                                       if (restarting->exchange(false)) recover();
                                   }];
    }

    ~AppleSession() override { stop(); }

    // Runs `work` on the session's queue and waits for it. A lambda keeps the
    // references it captures as references, which a block would copy.
    void onQueue(const std::function<void()>& work) { dispatch_sync(queue_, ^{ work(); }); }

    // Resolves each camera's anchor against the device's own formats: the facts
    // the device chose from must be the facts the device lists now, or the camera
    // has no anchor and its streams and stills are refused.
    void setAnchors(std::span<const std::optional<std::size_t>> formatOfCamera, const CameraLimits& limits) override
    {
        if (stopped_) return;
        onQueue([&] {
            limits_ = limits;
            anchors_.assign(entries_.size(), nullptr);
            refusals_.clear();
            {
                std::lock_guard lock(notesMutex_);
                notes_.assign(entries_.size(), CameraPresentation {});
            }
            for (std::size_t camera = 0; camera < entries_.size() && camera < formatOfCamera.size(); ++camera) {
                if (!formatOfCamera[camera]) continue;
                const std::size_t index = *formatOfCamera[camera];
                const FormatSet set = formatSetOf(entries_[camera].device, entries_[camera].zoomMultiplier,
                                                  entries_[camera].mounting.connectionTurn);
                const auto& listed = entries_[camera].info.captureFormats;
                // The zoom the system advises and the format's own maximum are no part of the format: a change of them leaves the anchor.
                const auto sameFormat = [&] {
                    if (index >= set.facts.size() || index >= listed.size()) return false;
                    auto now = set.facts[index];
                    now.recommendedZoomMaximum = listed[index].recommendedZoomMaximum;
                    now.maximumZoom = listed[index].maximumZoom;
                    return now == listed[index];
                };
                if (!sameFormat()) {
                    refuse(camera, Refusal::formatChanged, "the anchor is not the format discovery listed");
                    continue;
                }
                auto anchor = std::make_shared<Anchor>();
                anchor->format = set.formats[index];
                // The facts the device offered from: a front camera's photo sizes within its limit, so the photo
                // output's maximum (photoMaximum) and the sizes it makes are the sizes offered.
                anchor->facts = limitedStillFormats(entries_[camera].info.position, set.facts)[index];
                anchor->rates = offeredRatesOf(anchor->facts, videoRateCap);
                anchors_[camera] = std::move(anchor);
            }
            // What the session states of each camera it offers (the record's mounting line).
            for (std::size_t camera = 0; camera < entries_.size(); ++camera) {
                if (anchors_[camera] == nullptr) continue;
                CameraPresentation facts = entries_[camera].discovered;
                const Anchor& anchor = *anchors_[camera];
                facts.anchorWidth = anchor.facts.width;
                facts.anchorHeight = anchor.facts.height;
                facts.anchorRatios = anchor.facts.aspectRatios;
                facts.anchorFieldOfView = fieldOfViewOf(anchor);
                // What an earlier session of the process left of the camera's last use stays beside the new discovery.
                note(camera, [&](CameraPresentation& held) { held = withDiscovery(heldNoteOf(camera), facts); });
            }
        });
    }

    // Called under the state's mutex while the session lives, like
    // deliverStill's release: the block runs before the session can end.
    void requestRecovery() { dispatch_async(queue_, ^{ recoverAfterInterruption(); }); }

    bool startVideo(std::size_t camera, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate) override
    {
        if (stopped_ || camera >= entries_.size() || width == 0 || height == 0 || frameRate == 0) return false;
        bool ok = false;
        onQueue([&] { ok = startVideoOnQueue(camera, width, height, frameRate); });
        return ok;
    }

    void stopVideo(std::size_t camera) override
    {
        if (stopped_ || camera >= entries_.size()) return;
        onQueue([&] {
            auto& running = cameras_[camera];
            if (!running.video) return;
            // A camera that no longer streams leaves the session, and so does the
            // photo output made for it unless a still is still on its way; the
            // camera's indicator and its share of a multi-camera budget go with it.
            removeFromSession(camera, running, true, !stillsInFlight(camera));
            endPressureWatch(running);
            refusals_.reset(camera);
        });
    }

    void apply(std::size_t camera, const CameraSettings& settings) override
    {
        if (stopped_ || camera >= entries_.size()) return;
        onQueue([&] { applyOnQueue(camera, settings, false); });
    }

#if TARGET_OS_IOS
    // The connection and format a stabilization is written to (video_stabilization.hpp).
    class ConnectionStabilization final : public VideoStabilizationPort {
    public:
        ConnectionStabilization(AVCaptureDeviceFormat* format, AVCaptureConnection* connection)
            : format_(format), connection_(connection) {}
        VideoStabilizationSupport support() override
        {
            return {[format_ isVideoStabilizationModeSupported:AVCaptureVideoStabilizationModeStandard],
                    [format_ isVideoStabilizationModeSupported:AVCaptureVideoStabilizationModeLowLatency]};
        }
        void write(VideoStabilization mode) override { connection_.preferredVideoStabilizationMode = stabilizationModeOf(mode); }

    private:
        AVCaptureDeviceFormat* format_;
        AVCaptureConnection* connection_;
    };

    // The video connection's stabilization for what the guest wants (video_stabilization.hpp), written when it differs from
    // what the connection holds. Never the automatic mode: the preview and the recording share this one output, and the
    // system's choice for it is a cinematic mode that holds the viewfinder a second or more behind. A wish the connection
    // already holds changes nothing, so the view does not move when the guest first changes another setting. A format that
    // supports no mode leaves the picture unstabilized, and the record says so, once for the connection.
    std::optional<VideoStabilizationStep> writeStabilization(std::size_t camera, AVCaptureDevice* device, AVCaptureConnection* connection,
                                                             VideoStabilizationHeld& held, bool wanted)
    {
        if (connection == nil) return std::nullopt;
        @try {
            ConnectionStabilization port(device.activeFormat, connection);
            return applyVideoStabilization(port, wanted, held);
        } @catch (NSException* exception) {
            std::fprintf(stderr, "Camera: camera %zu: the video stabilization was refused: %s\n", camera,
                         exception.reason != nil ? exception.reason.UTF8String : "");
        }
        return std::nullopt;
    }

    // What the record and the log say of a connection's stabilization step.
    void recordStabilization(std::size_t camera, const VideoStabilizationStep& step)
    {
        if (step.unsupported)
            std::fprintf(stderr, "Camera: camera %zu: the active format supports no video stabilization; the picture stays unstabilized\n",
                         camera);
        if (step.note) note(camera, [&](CameraPresentation& held) { held.videoStabilization = step.state; });
    }

    void putStabilization(std::size_t camera, AVCaptureDevice* device, RunningCamera& running, bool wanted)
    {
        if (const auto step = writeStabilization(camera, device, running.connection, running.stabilization, wanted))
            recordStabilization(camera, *step);
    }
#endif

    // Puts a camera's settings on its device: all of them again when `force`
    // (a format change reverts some), or what changed since the last time.
    void applyOnQueue(std::size_t camera, const CameraSettings& settings, bool force)
    {
        auto& running = cameras_[camera];
        (void)applyCameraSettings(
            running, running.stabilizationWish, settings.stabilization, force,
            running.settings == settings && running.focusSerial == settings.focusTriggerSerial,
            [&] {
#if TARGET_OS_IOS
                putStabilization(camera, entries_[camera].device, running, settings.stabilization);
#endif
            },
            [&] { writeSettingsOnQueue(camera, settings, force); });
    }

    // The settings other than the stabilization, put on the device.
    void writeSettingsOnQueue(std::size_t camera, const CameraSettings& settings, bool force)
    {
        auto& running = cameras_[camera];
        // What the device holds is not known (a restore was refused): everything is written.
        force = force || running.unrestored;
        const Entry& entry = entries_[camera];
        AVCaptureDevice* device = entry.device;
        NSError* error = nil;
        if (![device lockForConfiguration:&error]) return;
#if TARGET_OS_IOS
        @try {
            const CameraInfo& info = entry.info;
            // A region is in the picture the camera delivers, which its video connection has turned (the square front
            // camera's): the point of interest is in the device's own, so it is mapped back through that turn.
            const auto pointOf = [&](const CameraRegion& region) {
                const CameraRegion own = regionBeforeTurn(region, entry.mounting.connectionTurn);
                return CGPointMake(own.x + own.width / 2, own.y + own.height / 2);
            };
            // Exposure. The point of interest comes first, as the focus's does: the device takes it up when the exposure
            // mode is set, so a tap that moves only the region must not set the mode ahead of the point.
            if (settings.aeRegion && settings.aeRegion->weight > 0 && device.exposurePointOfInterestSupported)
                device.exposurePointOfInterest = pointOf(*settings.aeRegion);
            if (settings.exposureMode == CameraExposureMode::manual && info.manualExposure) {
                const CMTime duration = CMTimeMake(std::int64_t(settings.exposureNanoseconds), 1'000'000'000);
                const float iso = std::clamp<float>(float(settings.iso), device.activeFormat.minISO, device.activeFormat.maxISO);
                [device setExposureModeCustomWithDuration:duration ISO:iso completionHandler:nil];
            } else if (settings.exposureMode == CameraExposureMode::locked) {
                if ([device isExposureModeSupported:AVCaptureExposureModeLocked])
                    device.exposureMode = AVCaptureExposureModeLocked;
            } else if ([device isExposureModeSupported:AVCaptureExposureModeContinuousAutoExposure]) {
                device.exposureMode = AVCaptureExposureModeContinuousAutoExposure;
            }
            if (settings.exposureMode != CameraExposureMode::manual)
                [device setExposureTargetBias:std::clamp(settings.exposureBiasStops, device.minExposureTargetBias,
                                                         device.maxExposureTargetBias)
                            completionHandler:nil];
            // Focus. The point of interest comes first: the device takes it up when the
            // focus mode is set. A new trigger runs one scan in any mode (the device
            // locks where it ends). Otherwise a manual lens goes to the distance only
            // when the guest has written it anew, and holds from then on, as does a
            // locked one (only a continuous focus is locked, so that a later apply
            // does not abort a scan under way); the continuous mode is the rest. A
            // format change reverts the focus, so `force` puts a written distance
            // back.
            if (info.autofocus) {
                if (settings.afRegion && settings.afRegion->weight > 0 && device.focusPointOfInterestSupported)
                    device.focusPointOfInterest = pointOf(*settings.afRegion);
                const bool scan = settings.focusTriggerSerial != running.focusSerial;
                const bool move = settings.focusDistanceSerial != running.focusDistanceSerial ||
                    (force && settings.focusDistanceSerial != 0);
                if (scan && [device isFocusModeSupported:AVCaptureFocusModeAutoFocus]) {
                    device.focusMode = AVCaptureFocusModeAutoFocus;
                } else if (settings.focusMode == CameraFocusMode::manual && info.manualFocus) {
                    if (move) {
                        [device setFocusModeLockedWithLensPosition:lensPositionOf(settings.focusDistanceDioptres,
                                                                                  info.minimumFocusDistanceMillimetres)
                                                 completionHandler:nil];
                    } else if (device.focusMode == AVCaptureFocusModeContinuousAutoFocus &&
                               [device isFocusModeSupported:AVCaptureFocusModeLocked]) {
                        device.focusMode = AVCaptureFocusModeLocked;
                    }
                } else if (settings.focusMode != CameraFocusMode::continuousAuto) {
                    if (device.focusMode == AVCaptureFocusModeContinuousAutoFocus &&
                        [device isFocusModeSupported:AVCaptureFocusModeLocked])
                        device.focusMode = AVCaptureFocusModeLocked;
                } else if ([device isFocusModeSupported:AVCaptureFocusModeContinuousAutoFocus]) {
                    device.focusMode = AVCaptureFocusModeContinuousAutoFocus;
                }
            }
            // White balance.
            if (settings.whiteBalanceMode == CameraWhiteBalanceMode::manual && info.manualWhiteBalance) {
                AVCaptureWhiteBalanceGains gains;
                const float maximum = device.maxWhiteBalanceGain;
                gains.redGain = std::clamp(settings.whiteBalanceGains[0], 1.0f, maximum);
                gains.greenGain = std::clamp(settings.whiteBalanceGains[1], 1.0f, maximum);
                gains.blueGain = std::clamp(settings.whiteBalanceGains[2], 1.0f, maximum);
                [device setWhiteBalanceModeLockedWithDeviceWhiteBalanceGains:gains completionHandler:nil];
            } else if (settings.whiteBalanceMode == CameraWhiteBalanceMode::locked) {
                if ([device isWhiteBalanceModeSupported:AVCaptureWhiteBalanceModeLocked])
                    device.whiteBalanceMode = AVCaptureWhiteBalanceModeLocked;
            } else if ([device isWhiteBalanceModeSupported:AVCaptureWhiteBalanceModeContinuousAutoWhiteBalance]) {
                device.whiteBalanceMode = AVCaptureWhiteBalanceModeContinuousAutoWhiteBalance;
            }
            // Zoom.
            const float factor = std::clamp(settings.zoomRatio / entry.zoomMultiplier, float(device.minAvailableVideoZoomFactor),
                                            float(device.maxAvailableVideoZoomFactor));
            if (std::fabs(device.videoZoomFactor - factor) > 1e-4f) device.videoZoomFactor = factor;
            // Torch.
            if (info.torch) {
                if (settings.torch && device.torchMode != AVCaptureTorchModeOn)
                    [device setTorchModeOnWithLevel:1.0f error:nil];
                else if (!settings.torch && device.torchMode != AVCaptureTorchModeOff)
                    device.torchMode = AVCaptureTorchModeOff;
            }
        } @catch (NSException*) {
            // A value the device refuses in its current state leaves it as it was.
        }
#else
        (void)entry;
#endif
        [device unlockForConfiguration];
        running.settings = settings;
        running.focusSerial = settings.focusTriggerSerial;
        running.focusDistanceSerial = settings.focusDistanceSerial;
        running.applied = true;
    }

    bool takeStill(std::size_t camera, const CameraStillRequest& request) override
    {
        if (stopped_ || camera >= entries_.size() || (request.jpegWidth == 0 && !request.raw)) return false;
        // On the session's queue as a whole: a camera that only takes stills
        // leaves the session when its last still is delivered, and a capture
        // started here must be seen by that before the output could go.
        bool started = false;
        onQueue([&] {
            // The photo settings and the capture itself raise for a request the
            // output does not take; the camera then leaves with nothing in flight.
            @try {
                started = startStill(camera, request);
                // A capture that is refused leaves the camera as it found it.
                if (!started) releaseStill(camera);
            } @catch (NSException* exception) {
                refuse(camera, Refusal::raised, std::string("a still: ") + exception.reason.UTF8String);
                started = false;
                {
                    std::lock_guard lock(photoMutex_);
                    photos_.erase(request.token);
                }
                releaseStill(camera);
            }
        });
        return started;
    }

    // `attempt` is the capture of this request that starts (from one): a photo that comes back at a size other than the
    // one asked for makes the next (uml::platform::stillSizeOutcome). `waited` is what the earlier attempts waited for
    // the settle time, of which the next has what is left.
    bool startStill(std::size_t camera, const CameraStillRequest& request, unsigned attempt = 1, std::uint64_t waited = 0)
    {
        const Entry& entry = entries_[camera];
        auto& running = cameras_[camera];
        if (!prepareStill(camera)) return false;
        // A crop is never changed under a still: the ratio the anchor pins is read. A change whose completion has not arrived
        // is waited for with the settle time, inside the same budget; a completion error or a ratio that differs after the
        // completion refuses the still, with the facts in the record.
        const RatioStillStep ratioStep = ratioStepOf(camera);
        if (ratioStep == RatioStillStep::refuseMoved || ratioStep == RatioStillStep::refuseFailed) return false;
        const bool ratioIsPending = ratioStep == RatioStillStep::wait;
        // A camera that imitates an earlier front camera takes a still only when its photo output turns it, on the
        // reading taken after the configuration was committed.
        if (!stillsAreTurned(camera, running.photoOutput)) return false;
        AVCapturePhotoOutput* output = running.photoOutput;
        AVCapturePhotoSettings* settings = nil;
        // The quality belongs inside the compression properties: iOS raises
        // (NSInvalidArgumentException) for it at the top level, which refused
        // every still there.
        NSDictionary* jpeg = @{
            AVVideoCodecKey: AVVideoCodecTypeJPEG,
            AVVideoCompressionPropertiesKey: @{AVVideoQualityKey: @(request.jpegQuality / 100.0)},
        };
        // A camera whose pictures the host presents half around (CameraInfo::presentationTurn) gives its still as a photo
        // that is turned and encoded here (turnedStillOf), or none: a Bayer still is not turned, and an unturned still is
        // never passed on.
        const bool turnsStill = entry.mounting.presentationTurn == 180;
        if (turnsStill && request.raw) {
            refuse(camera, Refusal::stillNotTurned, "a Bayer still is not turned");
            return false;
        }
        const bool turnsJpeg = turnsStill && request.jpegWidth != 0;
        if (request.raw) {
#if TARGET_OS_IOS
            OSType rawFormat = 0;
            for (NSNumber* format in output.availableRawPhotoPixelFormatTypes)
                if ([AVCapturePhotoOutput isBayerRAWPixelFormat:format.unsignedIntValue]) {
                    rawFormat = format.unsignedIntValue;
                    break;
                }
            if (rawFormat == 0) {
                refuse(camera, Refusal::rawUnavailable, "the photo output lists none");
                return false;
            }
            settings = request.jpegWidth != 0
                ? [AVCapturePhotoSettings photoSettingsWithRawPixelFormatType:rawFormat processedFormat:jpeg]
                : [AVCapturePhotoSettings photoSettingsWithRawPixelFormatType:rawFormat];
#else
            refuse(camera, Refusal::rawUnavailable, "macOS has no Bayer still");
            return false;
#endif
        } else if (turnsJpeg) {
            // The photo's pixels, full range 4:2:0: JFIF's own samples, which the encoder takes as they are.
            NSNumber* format = @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
            if (![output.availablePhotoPixelFormatTypes containsObject:format]) {
                refuse(camera, Refusal::stillNotTurned, "the photo output makes no full-range 4:2:0 photo");
                return false;
            }
            settings = [AVCapturePhotoSettings photoSettingsWithFormat:@{(id)kCVPixelBufferPixelFormatTypeKey: format}];
        } else {
            settings = [AVCapturePhotoSettings photoSettingsWithFormat:jpeg];
        }
        const AVCaptureFlashMode flash = request.flash == CameraFlashMode::on ? AVCaptureFlashModeOn
            : request.flash == CameraFlashMode::automatic ? AVCaptureFlashModeAuto : AVCaptureFlashModeOff;
        if (entry.info.flash && [output.supportedFlashModes containsObject:@(flash)]) settings.flashMode = flash;
#if TARGET_OS_IOS
        if (request.speedPrioritized || request.raw)
            settings.photoQualityPrioritization = AVCapturePhotoQualityPrioritizationSpeed;
#endif
        // The photo has the size the guest configured or the capture is refused:
        // the output makes up to the size set at admission (the anchor's
        // photoMaximum), and the anchor's offer holds only sizes it makes, so a
        // size that fails here is one that was not offered.
        const auto listed = listedPhotoSizes(entry.device.activeFormat);
        const CMVideoDimensions limit = output.maxPhotoDimensions;
        const uml::platform::CameraStillSize outputMaximum {std::uint32_t(std::max(0, int(limit.width))),
                                                            std::uint32_t(std::max(0, int(limit.height)))};
        const uml::platform::CameraStillSize wanted {request.jpegWidth, request.jpegHeight};
        const auto chosen = request.jpegWidth != 0 ? uml::platform::exactPhotoSize(listed, wanted, outputMaximum)
                                                   : uml::platform::coveringPhotoSize(listed, {}, outputMaximum);
        if (request.jpegWidth != 0 && !chosen) {
            refuse(camera, Refusal::stillNotMade,
                   "a JPEG of " + std::to_string(wanted.width) + "x" + std::to_string(wanted.height) + " (output maximum " +
                       std::to_string(outputMaximum.width) + "x" + std::to_string(outputMaximum.height) + ", " +
                       std::to_string(listed.size()) + " sizes listed)");
            return false;
        }
        if (chosen) settings.maxPhotoDimensions = CMVideoDimensions {std::int32_t(chosen->width), std::int32_t(chosen->height)};
        // What the photo's colours are when it states none: the device's sRGB, if it is held.
#if TARGET_OS_IOS
        const BOOL captureIsSrgb = entry.device.activeColorSpace == AVCaptureColorSpace_sRGB;
#else
        const BOOL captureIsSrgb = NO;
#endif
        UMLPhotoDelegate* delegate = [[UMLPhotoDelegate alloc] initWithCamera:camera
                                                                         token:request.token
                                                                      wantJpeg:request.jpegWidth != 0
                                                                      jpegSize:wanted
                                                                  maximumBytes:request.maximumJpegBytes
                                                                       wantRaw:request.raw
                                                                      turnHalf:turnsJpeg
                                                                 captureIsSrgb:captureIsSrgb
                                                                   jpegQuality:request.jpegQuality
                                                                       attempt:attempt
                                                                         state:state_];
        // The photo output holds its delegate weakly, so the session keeps it
        // until the capture finishes, or none of its callbacks would arrive.
        {
            std::lock_guard lock(photoMutex_);
            photos_[request.token] = {camera, delegate, request, attempt, waited};
        }
        // The pipeline applies a maximum that needs the sensor's full-resolution mode some time after the session commits
        // a configuration, and a capture that starts before is served at the binned size: a still starts when that time
        // has passed (the first after the admission, and one after a stream started, resized or stopped, or after a
        // repair), scheduled on the session's queue, which stays free for the other cameras and the stop meanwhile. The waits of
        // a request are bounded (stillWaitBudgetNanoseconds): commits that keep coming fail it instead of postponing it.
        const auto step = uml::platform::scheduledStillStep(false, true, running.photoMaximumCommittedAt, hostNow(), waited, ratioIsPending);
        if (step.step == ScheduledStillStep::fail) {
            // Commits kept postponing it past its budget: the guest is told at once (the caller's failure).
            std::fprintf(stderr, "Camera: camera %zu: the still failed: the photo output's full-resolution maximum or the aspect ratio did not settle within %llu ms\n",
                         camera, static_cast<unsigned long long>(stillWaitBudgetNanoseconds / 1000000));
            std::lock_guard lock(photoMutex_);
            photos_.erase(request.token);
            return false;
        }
        if (step.step == ScheduledStillStep::wait) {
            std::fprintf(stderr, "Camera: camera %zu: the still starts in %llu ms, when the photo output's full-resolution maximum and the aspect ratio have taken effect\n",
                         camera, static_cast<unsigned long long>(step.waitNanoseconds / 1000000));
            {
                std::lock_guard lock(photoMutex_);
                photos_[request.token].waited = step.waitedNanoseconds;
            }
            scheduleStill(camera, request.token, output, settings, delegate, step.waitNanoseconds);
            return true;
        }
        return captureStill(camera, output, settings, delegate);
    }

    bool captureStill(std::size_t camera, AVCapturePhotoOutput* output, AVCapturePhotoSettings* settings, UMLPhotoDelegate* delegate)
    {
        [output capturePhotoWithSettings:settings delegate:delegate];
        refusals_.reset(camera);
        return true;
    }

    // Starts a registered capture `wait` nanoseconds from now on the session's queue. The block holds the state, not the
    // session: a session that ended (stop() clears the state's pointer first and drains the queue after) is not
    // touched, and one that is running is held by that drain until the block is done.
    void scheduleStill(std::size_t camera, std::uint64_t token, AVCapturePhotoOutput* output, AVCapturePhotoSettings* settings,
                       UMLPhotoDelegate* delegate, std::uint64_t wait)
    {
        std::shared_ptr<SessionState> state = state_;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, std::int64_t(wait)), queue_, ^{
            AppleSession* session = nullptr;
            {
                std::lock_guard lock(state->mutex);
                session = static_cast<AppleSession*>(state->session);
            }
            if (session == nullptr) {
                std::fprintf(stderr, "Camera: a scheduled still was dropped: the session ended\n");
                return;
            }
            session->fireStill(camera, token, output, settings, delegate);
        });
    }

    // The time of a scheduled capture came (on the session's queue).
    void fireStill(std::size_t camera, std::uint64_t token, AVCapturePhotoOutput* output, AVCapturePhotoSettings* settings,
                   UMLPhotoDelegate* delegate)
    {
        bool known = false;
        std::uint64_t waited = 0;
        {
            std::lock_guard lock(photoMutex_);
            const auto found = photos_.find(token);
            known = found != photos_.end() && found->second.camera == camera;
            if (known) waited = found->second.waited;
        }
        const auto running = cameras_.find(camera);
        const std::uint64_t committedAt = running == cameras_.end() ? 0 : running->second.photoMaximumCommittedAt;
        // The pinned ratio's change that was under way when the still was accepted has completed or not: read again.
        bool ratioIsPending = false;
        if (known && !stopped_) {
            const RatioStillStep ratioStep = ratioStepOf(camera);
            if (ratioStep == RatioStillStep::refuseMoved || ratioStep == RatioStillStep::refuseFailed) {
                failStill(camera, token);
                return;
            }
            ratioIsPending = ratioStep == RatioStillStep::wait;
        }
        const auto step = uml::platform::scheduledStillStep(stopped_, known && running != cameras_.end(), committedAt, hostNow(), waited, ratioIsPending);
        switch (step.step) {
        case ScheduledStillStep::drop:
            std::fprintf(stderr, "Camera: a scheduled still was dropped: the session ended or the request is gone\n");
            return;
        case ScheduledStillStep::wait: {
            {
                std::lock_guard lock(photoMutex_);
                const auto found = photos_.find(token);
                if (found != photos_.end()) found->second.waited = step.waitedNanoseconds;
            }
            scheduleStill(camera, token, output, settings, delegate, step.waitNanoseconds);
            return;
        }
        case ScheduledStillStep::fail:
            // Commits kept postponing it past its budget: failed once, here, and nothing is scheduled again.
            std::fprintf(stderr, "Camera: camera %zu: the still failed: the photo output's full-resolution maximum or the aspect ratio did not settle within %llu ms\n",
                         camera, static_cast<unsigned long long>(stillWaitBudgetNanoseconds / 1000000));
            failStill(camera, token);
            return;
        case ScheduledStillStep::fire:
            break;
        }
        bool started = false;
        @try {
            started = captureStill(camera, output, settings, delegate);
        } @catch (NSException* exception) {
            refuse(camera, Refusal::raised, std::string("a still: ") + exception.reason.UTF8String);
        }
        // The capture fails at once: the guest is told, never left to wait for it.
        if (!started) failStill(camera, token);
    }

    // The result of a capture that cannot be made, delivered at once (the retry's failure and a scheduled capture's).
    void failStill(std::size_t camera, std::uint64_t token)
    {
        CameraStillResult failed;
        failed.token = token;
        failed.failure.stage = CameraStillStage::notStarted;
        std::lock_guard lock(state_->mutex);
        if (state_->session) deliverStill(camera, std::move(failed));
    }

    void stop() override
    {
        if (stopped_.exchange(true)) return;
        {
            std::lock_guard lock(state_->mutex);
            state_->session = nullptr;
        }
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        for (id token : {interrupted_, resumed_, failed_, started_}) {
            if (token) [center removeObserver:token];
        }
        interrupted_ = resumed_ = failed_ = started_ = nil;
        AVCaptureSession* session = session_;
        // The pressure watches end first, inside the queue and before it is
        // drained for the last time: nothing a notification queued behind this
        // programs a device afterwards.
        onQueue([&, session] {
            for (auto& [camera, running] : cameras_) {
                (void)camera;
                endPressureWatch(running);
            }
            if (session.running) [session stopRunning];
        });
        // Turn a torch off: a session that ends must not leave a light on.
        for (const auto& [camera, running] : cameras_) {
            (void)running;
#if TARGET_OS_IOS
            AVCaptureDevice* device = entries_[camera].device;
            if (device.hasTorch && device.torchMode != AVCaptureTorchModeOff && [device lockForConfiguration:nil]) {
                device.torchMode = AVCaptureTorchModeOff;
                [device unlockForConfiguration];
            }
#endif
        }
        {
            std::lock_guard lock(photoMutex_);
            photos_.clear();
        }
        cameras_.clear();
        session_ = nil;
    }

    // Called by the output delegates, under the state's mutex.
    void deliver(CameraEvent event) { delegate_.cameraEvent(event); }

    void deliverFrame(std::size_t camera, CMSampleBufferRef sample)
    {
        if (camera >= entries_.size()) return;
        // The session runs in the foreground: a departure from now on is its own.
        ranInForeground_->store(true, std::memory_order_relaxed);
        // What the capture made before the cameras were put back in their anchors does not get here
        // (RepairBoundary, applied by the output delegate before it calls this).
        const CMTime presentation = CMSampleBufferGetPresentationTimeStamp(sample);
        const std::uint64_t capturedAt = CMTIME_IS_VALID(presentation) ? std::uint64_t(CMTimeGetSeconds(presentation) * 1e9) : hostNow();
        CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
        if (pixels == nullptr || CVPixelBufferGetPlaneCount(pixels) != 2) return;
        // The camera's pinned ratio is not the one it delivers until the change has taken effect: nothing of it is passed on
        // while it is under way or failed, and nothing made before it took effect afterwards (ratioAdmitsSample); the device's
        // own stand-in frames bridge the gap as they do at a session's start.
        if (!ratioAdmitsSample(camera, nanosecondsOf(presentation), CVPixelBufferGetWidth(pixels), CVPixelBufferGetHeight(pixels))) return;
        if (CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
        CFRetain(sample);
        CameraFrame frame;
        frame.width = std::uint32_t(CVPixelBufferGetWidth(pixels));
        frame.height = std::uint32_t(CVPixelBufferGetHeight(pixels));
        const auto* luma = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 0));
        const auto* chroma = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 1));
        frame.lumaStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 0);
        frame.chromaStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 1);
        frame.luma = {luma, frame.lumaStride * CVPixelBufferGetHeightOfPlane(pixels, 0)};
        frame.chroma = {chroma, frame.chromaStride * CVPixelBufferGetHeightOfPlane(pixels, 1)};
        frame.native = pixels;
        frame.owner = std::shared_ptr<const void>(sample, [pixels](const void* held) {
            CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
            CFRelease(held);
        });
        frame.timestampNanoseconds = capturedAt;
        frame.exposure = exposureOf(camera, sample);
        noteFirstFrame(camera, frame.width, frame.height);
        delegate_.cameraFrame(camera, std::move(frame));
    }

    // A capture is over: its delegate is no longer needed, and the result goes on.
    void deliverStill(std::size_t camera, CameraStillResult result)
    {
        {
            std::lock_guard lock(photoMutex_);
            photos_.erase(result.token);
        }
        delegate_.cameraStill(camera, std::move(result));
        // A camera that only takes stills leaves the session with its last
        // one. This is called under the state's mutex while the session lives,
        // and stop() waits for the queue after clearing that state, so the
        // block runs before the session can end.
        if (camera < entries_.size()) dispatch_async(queue_, ^{ releaseStill(camera); });
    }

    // The capture of a request came back at a size other than the one asked for and has another attempt: it is made
    // again on the session's queue, and the camera stays in the session for it. False when there is none to make
    // (the request is gone, or the session is ending), and the caller fails the capture. Called by the photo delegate,
    // under the state's mutex.
    bool remakeStill(std::size_t camera, std::uint64_t token)
    {
        if (stopped_ || camera >= entries_.size()) return false;
        {
            std::lock_guard lock(photoMutex_);
            const auto found = photos_.find(token);
            if (found == photos_.end() || found->second.camera != camera) return false;
        }
        dispatch_async(queue_, ^{ remakeStillOnQueue(camera, token); });
        return true;
    }

    void remakeStillOnQueue(std::size_t camera, std::uint64_t token)
    {
        Photo photo;
        {
            std::lock_guard lock(photoMutex_);
            const auto found = photos_.find(token);
            if (found == photos_.end()) return;
            photo = found->second;
        }
        bool started = false;
        @try {
            started = !stopped_ && startStill(camera, photo.request, photo.attempt + 1, photo.waited);
        } @catch (NSException* exception) {
            refuse(camera, Refusal::raised, std::string("a still: ") + exception.reason.UTF8String);
        }
        // The capture fails at once: the guest is told, never left to wait for it.
        if (!started) failStill(camera, token);
    }

    // The device's own state now: its 3A states, white balance, zoom, lens and
    // frame duration, and what the preview exposes with. What a frame or a photo
    // exposed with is read from that frame or photo (`exposureOf`, the photo
    // delegate).
    CameraExposure deviceExposureOf(std::size_t camera) const
    {
        const Entry& entry = entries_[camera];
        CameraExposure exposure;
        exposure.activeCamera = std::uint32_t(camera);
        AVCaptureDevice* device = entry.device;
#if TARGET_OS_IOS
        exposure.exposureNanoseconds = std::uint64_t(std::max(0.0, CMTimeGetSeconds(device.exposureDuration)) * 1e9);
        exposure.iso = std::uint32_t(device.ISO);
        const auto gains = device.deviceWhiteBalanceGains;
        exposure.whiteBalanceGains = {gains.redGain, gains.greenGain, gains.blueGain};
        @try {
            const float maximum = device.maxWhiteBalanceGain;
            AVCaptureWhiteBalanceGains clamped {std::clamp(gains.redGain, 1.0f, maximum), std::clamp(gains.greenGain, 1.0f, maximum),
                                                std::clamp(gains.blueGain, 1.0f, maximum)};
            const auto values = [device temperatureAndTintValuesForDeviceWhiteBalanceGains:clamped];
            exposure.colorTemperature = std::uint32_t(std::max(0.0f, values.temperature));
            exposure.tint = values.tint;
        } @catch (NSException*) {
        }
        exposure.lensPositionDioptres = dioptresOf(device.lensPosition, entry.info.minimumFocusDistanceMillimetres);
        exposure.ae = controlState(device.adjustingExposure, device.exposureMode == AVCaptureExposureModeLocked);
        exposure.af = entry.info.autofocus
            ? controlState(device.adjustingFocus, device.focusMode == AVCaptureFocusModeLocked) : CameraControlState::inactive;
        exposure.awb = controlState(device.adjustingWhiteBalance, device.whiteBalanceMode == AVCaptureWhiteBalanceModeLocked);
        exposure.zoomRatio = float(device.videoZoomFactor) * entry.zoomMultiplier;
        exposure.aperture = device.lensAperture;
        exposure.flashFired = false;
        const CMTime duration = device.activeVideoMinFrameDuration;
        exposure.frameDurationNanoseconds = CMTIME_IS_VALID(duration) && duration.value > 0
            ? std::uint64_t(CMTimeGetSeconds(duration) * 1e9) : 0;
#endif
        (void)device;
        return exposure;
    }

    CameraExposure exposureOf(std::size_t camera, CMSampleBufferRef sample) const
    {
        CameraExposure exposure = deviceExposureOf(camera);
        // The sample's own Exif attachment states what the exposure used.
        if (auto* exif = static_cast<CFDictionaryRef>(CMGetAttachment(sample, kCGImagePropertyExifDictionary, nullptr))) {
            double time = 0;
            if (auto* number = static_cast<CFNumberRef>(CFDictionaryGetValue(exif, kCGImagePropertyExifExposureTime));
                number && CFNumberGetValue(number, kCFNumberDoubleType, &time) && time > 0)
                exposure.exposureNanoseconds = std::uint64_t(time * 1e9);
            if (auto* isos = static_cast<CFArrayRef>(CFDictionaryGetValue(exif, kCGImagePropertyExifISOSpeedRatings));
                isos && CFArrayGetCount(isos) > 0) {
                int iso = 0;
                if (CFNumberGetValue(static_cast<CFNumberRef>(CFArrayGetValueAtIndex(isos, 0)), kCFNumberIntType, &iso) && iso > 0)
                    exposure.iso = std::uint32_t(iso);
            }
        }
        return exposure;
    }

    const Entry& entry(std::size_t camera) const { return entries_[camera]; }

    // ------------------------------------------------------------------------
    // What the session says of how it presents its cameras (camera_presentation.hpp)
    // ------------------------------------------------------------------------

    // Edits a camera's note and hands the delegate the result, one note at a time so that a camera's arrive in order. A
    // camera the session was given no anchor for is not the guest's and has none. Called from the session's queue, from
    // the capture's frame callback and from the device's completion handlers; it takes only the notes' own lock, which
    // is below the state's (the callers that hold the state's mutex take it first).
    void note(std::size_t camera, const std::function<void(CameraPresentation&)>& edit)
    {
        std::lock_guard lock(notesMutex_);
        if (camera >= notes_.size()) return;
        CameraPresentation next = notes_[camera];
        edit(next);
        if (next.anchorWidth == 0) return;
        notes_[camera] = next;
        keepNote(camera);
        delegate_.cameraPresentation(camera, next);
    }

    // The camera's note as an earlier session of the process left it (the notes' lock is held).
    CameraPresentation heldNoteOf(std::size_t camera)
    {
        HeldNotes& held = heldNotes();
        std::lock_guard lock(held.mutex);
        const auto found = held.notes.find(entries_[camera].info.identifier);
        return found != held.notes.end() ? found->second.note : CameraPresentation {};
    }

    // Keeps the camera's note for the sessions that follow this one (the notes' lock is held).
    void keepNote(std::size_t camera)
    {
        HeldNotes& held = heldNotes();
        std::lock_guard lock(held.mutex);
        held.notes[entries_[camera].info.identifier] = {notes_[camera], ++held.clock};
        if (held.notes.size() <= entries_.size() + heldNotesRecent) return;
        std::vector<HeldNoteUse> uses;
        for (const auto& [identifier, kept] : held.notes) uses.push_back({identifier, kept.used});
        std::vector<std::string> current;
        for (const auto& entry : entries_) current.push_back(entry.info.identifier);
        for (const auto& identifier : heldNotesToDrop(uses, current, heldNotesRecent)) held.notes.erase(identifier);
    }

    // The first frame of an admission states the size the output delivers, once.
    void noteFirstFrame(std::size_t camera, std::uint32_t width, std::uint32_t height)
    {
        std::lock_guard lock(notesMutex_);
        if (camera >= notes_.size() || !notes_[camera].videoKnown || notes_[camera].videoFirstWidth != 0) return;
        notes_[camera].videoFirstWidth = width;
        notes_[camera].videoFirstHeight = height;
        keepNote(camera);
        delegate_.cameraPresentation(camera, notes_[camera]);
    }

    // The Exif orientation of the photo the host's output gave for a capture, as the last of its camera.
    void noteStillExif(std::size_t camera, std::uint8_t orientation)
    {
        note(camera, [&](CameraPresentation& held) { held.exifOrientation = orientation; });
    }

    // A photo that could not be turned half around and encoded (turnedStillOf): the still failed, and the reason is
    // recorded as a refusal of the camera.
    void noteStillNotTurned(std::size_t camera, TurnedStillFailure failure)
    {
        refuse(camera, Refusal::stillNotTurned, std::string(turnedStillFailureName(failure)));
    }

    // The completion of a dynamic aspect ratio change arrives (the device's own queue), under the state's mutex: `operation`
    // is the change it belongs to, and `completion.code` is `ratioPending` when the change is asked for, before the device
    // is told. The camera's frames are held until the change has completed without an error, and the samples made before
    // the first buffer it applies to never pass: that buffer's timestamp, on the device's clock `from` (the camera's input
    // port's), is converted to the capture's, the one every sample's timestamp is on; where that cannot be done the
    // samples are held until one of the anchor's shape arrives (RatioBoundary). An error ends the camera for the guest
    // (failRatio), as a refused admission does. A change of a camera that has left the session is gone, and its completion
    // does nothing.
    void ratioCompleted(std::size_t camera, std::uint64_t operation, const RatioCompletion& completion, const void* from)
    {
#if TARGET_OS_IOS
        if (camera >= entries_.size()) return;
        const std::int32_t code = completion.code;
        bool failedNow = false;
        {
            RatioBoundary boundary = RatioBoundary::none;
            std::int64_t notBefore = 0;
            if (code == 0 && completion.changed) {
                boundary = RatioBoundary::shape;
                CMClockRef to = session_.synchronizationClock;
                if (from != nullptr && to != nullptr && CMTIME_IS_NUMERIC(completion.syncTime))
                    if (const auto converted = nanosecondsOf(CMSyncConvertTime(completion.syncTime, static_cast<CMClockRef>(const_cast<void*>(from)), to))) {
                        boundary = RatioBoundary::atTime;
                        notBefore = *converted;
                    }
            }
            std::lock_guard lock(notesMutex_);
            if (code == ratioPending) {
                ratioPins_.asked(camera, operation);
            } else {
                const auto outcome = ratioPins_.completed(camera, operation, code, boundary, notBefore);
                if (outcome == RatioPins::Completion::ignored) return;
                failedNow = outcome == RatioPins::Completion::failed;
            }
        }
        // The ratio the camera's anchor pins, which the notes do not hold until the admission that asked for it is noted.
        CameraAspectRatio pinned = CameraAspectRatio::none;
        if (const Anchor* anchor = anchorOf(camera); anchor != nullptr) pinned = anchor->facts.pinnedRatio;
        note(camera, [&](CameraPresentation& held) { noteRatio(held, camera, pinned); });
        if (failedNow)
            failRatio(camera, Refusal::ratioFailed, "the change to " + std::string(aspectRatioName(pinned)) + " ended with error " + std::to_string(code));
#else
        (void)camera;
        (void)operation;
        (void)completion;
        (void)from;
#endif
    }

    // The camera's pinned ratio did not come to be, by an error or by no completion in time: it is refused as an admission
    // whose connection does not hold its turn is, with the reason in the record, and the session is said to have failed,
    // which the device answers by telling the guest its cameras are interrupted and opening another session whose
    // admissions pin the ratio again. Called with the state's mutex held.
    void failRatio(std::size_t camera, Refusal reason, const std::string& detail)
    {
        refuse(camera, reason, detail);
        deliver({allCameras, CameraEventKind::failed});
    }

private:
#if TARGET_OS_IOS
    static CameraFact factOf(bool value) { return value ? CameraFact::yes : CameraFact::no; }

    // The camera's pinned ratio into its note, in the one critical section that holds the notes' lock (note()): the device's
    // ratio as it reads now, and the change's own state as the camera has it now, pending while it is under way or never
    // completed, else its completion's error code. Read and written together, a completion that comes between an admission's
    // reading and its note cannot be overwritten by the older state, and a camera that has no change (it left the session)
    // keeps what its note says.
    void noteRatio(CameraPresentation& held, std::size_t camera, CameraAspectRatio pinned)
    {
        if (pinned != CameraAspectRatio::none) applyRatio(held, ratioFactsOf(entries_[camera].device, pinned));
        if (const auto code = ratioNoteCode(ratioPins_.find(camera))) held.ratioError = *code;
    }

    // The ratio facts into a note; a change whose completion has not come yet is pending.
    static void applyRatio(CameraPresentation& held, const RatioFacts& facts)
    {
        if (!held.ratioKnown) held.ratioError = ratioPending;
        held.ratioKnown = true;
        held.ratioPinned = facts.pinned;
        held.ratioNow = facts.now;
        held.ratioWidth = facts.width;
        held.ratioHeight = facts.height;
    }

    // An anchor's field of view in degrees, rounded: a format that lists ratios states the larger of the two crops it is
    // held at, uncorrected for distortion; -1 where the host has none.
    static std::int16_t fieldOfViewOf(const Anchor& anchor)
    {
        const float degrees = anchor.facts.pinnedRatio != CameraAspectRatio::none
            ? pinnedFieldOfView([anchor.format videoFieldOfViewForAspectRatio:AVCaptureAspectRatio3x4 geometricDistortionCorrected:NO],
                                [anchor.format videoFieldOfViewForAspectRatio:AVCaptureAspectRatio4x3 geometricDistortionCorrected:NO])
            : anchor.format.videoFieldOfView;
        return degrees > 0 ? std::int16_t(std::lround(degrees)) : std::int16_t(-1);
    }

    // What an admission set on the camera: the video connection (its angle and mirroring by default and now, the size the
    // output was asked for), the photo output's compensation and photo connection, the device's ratio.
    void noteAdmission(std::size_t camera, const std::optional<ConnectionFacts>& video, const std::optional<PhotoOutputFacts>& attached,
                       const std::optional<CameraStillSize>& photoMaximum, const std::optional<RatioFacts>& ratio, CameraStillSize asked,
                       AVCapturePhotoOutput* addedPhoto, bool admitted)
    {
        // The photo output's compensation as it reads now that the admission committed the configuration, of the output this
        // admission added (the camera's entry does not hold it yet); the reading taken as the output was attached stays
        // where the output is gone again (admissionCompensationReading).
        const std::optional<PhotoOutputFacts> photo = committedPhotoFacts(attached, addedPhoto, admitted, [](AVCapturePhotoOutput* output) {
            return readCompensation(output, CameraReading::afterCommit);
        });
        AVCaptureDevice* device = entries_[camera].device;
        const CameraFact center = factOf(device.centerStageActive);
        const CameraFact distortion = factOf(device.geometricDistortionCorrectionEnabled);
        const Anchor* anchor = anchorOf(camera);
        note(camera, [&](CameraPresentation& held) {
            if (ratio) noteRatio(held, camera, ratio->pinned);
            if (video) {
                held.videoKnown = true;
                held.videoDefaultAngle = video->defaultAngle;
                held.videoAngle = video->angle;
                held.videoAngleSupported = factOf(video->angleSupported);
                held.videoDefaultMirrored = video->defaultMirrored;
                held.videoMirrored = video->mirrored;
                if (anchor != nullptr) {
                    const auto frame = outputSizesFor(anchor->facts, asked.width, asked.height, 0).frame;
                    held.videoAskedWidth = frame ? frame->width : anchor->facts.width;
                    held.videoAskedHeight = frame ? frame->height : anchor->facts.height;
                }
                held.videoFirstWidth = 0;
                held.videoFirstHeight = 0;
                held.videoStabilization = VideoStabilizationState::unknown;
            }
            if (photo) {
                held.photoKnown = true;
                held.compensationSupported = photo->compensationSupported;
                held.compensationEnabled = photo->compensationEnabled;
                held.compensationReading = photo->reading;
                held.photoDefaultAngle = photo->connection.defaultAngle;
                held.photoAngle = photo->connection.angle;
                held.photoDefaultMirrored = photo->connection.defaultMirrored;
                held.photoMirrored = photo->connection.mirrored;
                if (photoMaximum) {
                    held.photoMaximumWidth = photoMaximum->width;
                    held.photoMaximumHeight = photoMaximum->height;
                }
            }
            if (video || photo) {
                held.centerStageActive = center;
                held.distortionCorrected = distortion;
            }
        });
    }

    // A repair put a camera back in its anchor: the ratio it pinned again, the connections as they are now, and a new first
    // frame to wait for.
    void noteRepair(std::size_t camera, const RunningCamera& running, const std::optional<RatioFacts>& ratio)
    {
        std::optional<std::pair<std::int16_t, std::int8_t>> video;
        if (running.connection != nil)
            @try {
                video = std::make_pair(angleNumber(running.connection.videoRotationAngle), mirroringNumber(running.connection));
            } @catch (NSException*) {
            }
        std::optional<PhotoOutputFacts> photo;
        if (running.photoOutput != nil) {
            photo = readCompensation(running.photoOutput, CameraReading::afterCommit);
            if (AVCaptureConnection* connection = [running.photoOutput connectionWithMediaType:AVMediaTypeVideo]; connection != nil)
                @try {
                    photo->connection.angle = angleNumber(connection.videoRotationAngle);
                    photo->connection.mirrored = mirroringNumber(connection);
                } @catch (NSException*) {
                }
        }
        const CMVideoDimensions maximum = running.photoOutput != nil ? running.photoOutput.maxPhotoDimensions : CMVideoDimensions {};
        note(camera, [&](CameraPresentation& held) {
            if (ratio) noteRatio(held, camera, ratio->pinned);
            if (video && held.videoKnown) {
                held.videoAngle = video->first;
                held.videoMirrored = video->second;
                held.videoFirstWidth = 0;
                held.videoFirstHeight = 0;
            }
            if (photo && held.photoKnown) {
                held.compensationSupported = photo->compensationSupported;
                held.compensationEnabled = photo->compensationEnabled;
                held.compensationReading = photo->reading;
                held.photoAngle = photo->connection.angle;
                held.photoMirrored = photo->connection.mirrored;
                held.photoMaximumWidth = std::uint32_t(std::max(0, int(maximum.width)));
                held.photoMaximumHeight = std::uint32_t(std::max(0, int(maximum.height)));
            }
        });
    }
#else
    static std::int16_t fieldOfViewOf(const Anchor&) { return -1; }
#endif

#if TARGET_OS_IOS
    // What a still does about the ratio its camera's anchor pins (ratioStillStep), on the device's reading now and the
    // state of the change the session asked for. A refusal is logged and recorded here.
    RatioStillStep ratioStepOf(std::size_t camera)
    {
        const Anchor* anchor = anchorOf(camera);
        if (anchor == nullptr || anchor->facts.pinnedRatio == CameraAspectRatio::none) return RatioStillStep::proceed;
        const RatioFacts facts = ratioFactsOf(entries_[camera].device, anchor->facts.pinnedRatio);
        RatioPin pin;
        {
            std::lock_guard lock(notesMutex_);
            if (const auto found = ratioPins_.find(camera)) pin = *found;
        }
        const RatioStillStep step = ratioStillStep(facts.pinned, facts.now, pin.pending && !pin.timedOut, ratioPinErrorForStill(pin));
        if (step == RatioStillStep::refuseMoved || step == RatioStillStep::refuseFailed) {
            note(camera, [&](CameraPresentation& held) {
                applyRatio(held, facts);
                if (step == RatioStillStep::refuseFailed && !pin.timedOut) held.ratioError = pin.error;
            });
            if (step == RatioStillStep::refuseFailed && pin.timedOut)
                refuse(camera, Refusal::ratioTimedOut, "the change to " + std::string(aspectRatioName(facts.pinned)) + " did not complete");
            else if (step == RatioStillStep::refuseFailed)
                refuse(camera, Refusal::ratioFailed, "the change to " + std::string(aspectRatioName(facts.pinned)) + " ended with error " +
                                                         std::to_string(pin.error));
            else
                refuse(camera, Refusal::ratioMoved,
                       "the device holds " + std::string(aspectRatioName(facts.now)) + ", not " + std::string(aspectRatioName(facts.pinned)));
        }
        return step;
    }

    // What a still does about the photo output's orientation compensation of a camera that imitates an earlier front
    // camera (stillCompensationStep), on the reading taken now, after the configuration was committed: supported and off is
    // turned on once, in a configuration of its own. The reading goes into the camera's photo line, and a refusal says
    // which reading refused it. Nothing is asked of any other camera.
    bool stillsAreTurned(std::size_t camera, AVCapturePhotoOutput* output)
    {
        const Entry& entry = entries_[camera];
        if (!entry.mounting.emulatesEarlierFront) return true;
        PhotoOutputFacts facts = readCompensation(output, CameraReading::afterCommit);
        CompensationStep step = stillCompensationStep(true, facts.compensationSupported, facts.compensationEnabled, facts.reading);
        if (step == CompensationStep::enable) {
            [session_ beginConfiguration];
            @try {
                output.cameraSensorOrientationCompensationEnabled = YES;
            } @catch (NSException* exception) {
                std::fprintf(stderr, "Camera: camera %zu: the photo output kept its orientation compensation: %s\n", camera,
                             exception.reason != nil ? exception.reason.UTF8String : "");
            }
            commitConfiguration();
            photoPipelinesSettle();
            facts = readCompensation(output, CameraReading::afterCommit);
            step = stillCompensationStep(true, facts.compensationSupported, facts.compensationEnabled, facts.reading);
            if (step == CompensationStep::enable) step = CompensationStep::refuse;
        }
        note(camera, [&](CameraPresentation& held) {
            held.photoKnown = true;
            held.compensationSupported = facts.compensationSupported;
            held.compensationEnabled = facts.compensationEnabled;
            held.compensationReading = facts.reading;
        });
        if (step != CompensationStep::refuse) return true;
        refuse(camera, Refusal::stillsNotTurned,
               "the photo output does not turn its stills (compensation supported " + std::string(factName(facts.compensationSupported)) +
                   ", on " + std::string(factName(facts.compensationEnabled)) + ", read after the configuration was committed): the camera takes no still");
        return false;
    }
#else
    RatioStillStep ratioStepOf(std::size_t) { return RatioStillStep::proceed; }
    bool stillsAreTurned(std::size_t, AVCapturePhotoOutput*) { return true; }
#endif

#if TARGET_OS_IOS
    // Whether a sample of the camera goes to the guest (ratioSampleDelivered): not while a pinned ratio's change is under way
    // or ended in an error or no completion, and never one made before the first buffer the change applies to (the device
    // delivers buffers of the ratio it had until then, and a callback that was already entered when the completion came
    // must not pass because the completion has). `captureNanoseconds` is the sample's timestamp on the capture's clock; its
    // size is compared with the anchor's aspect ratio where the rule is the shape's.
    bool ratioAdmitsSample(std::size_t camera, std::optional<std::int64_t> captureNanoseconds, std::size_t width, std::size_t height)
    {
        const Anchor* anchor = anchorOf(camera);
        const bool shape = anchor == nullptr || anchor->facts.width == 0 || anchor->facts.height == 0 ||
            std::uint64_t(width) * anchor->facts.height == std::uint64_t(height) * anchor->facts.width;
        std::lock_guard lock(notesMutex_);
        return ratioPins_.sampleDelivered(camera, captureNanoseconds, shape);
    }

    // The session runs with the camera in it (after an admission or a repair): the time the pinned ratio's change may
    // take begins, once for the change that is pending, and is watched on the session's queue.
    void armRatioWatch(std::size_t camera)
    {
        std::uint64_t operation = 0;
        {
            std::lock_guard lock(notesMutex_);
            const auto armed = ratioPins_.armed(camera, hostNow());
            if (!armed) return;
            operation = *armed;
        }
        std::shared_ptr<SessionState> state = state_;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, std::int64_t(ratioCompletionBudgetNanoseconds)), queue_, ^{
            AppleSession* session = nullptr;
            {
                std::lock_guard lock(state->mutex);
                session = static_cast<AppleSession*>(state->session);
            }
            if (session != nullptr) session->ratioWatchFired(camera, operation);
        });
    }

    // The time of a pinned ratio's change ran out (on the session's queue): a change that has not completed ends the
    // camera for the guest (failRatio). The watch fires after the budget, whatever the clocks read.
    void ratioWatchFired(std::size_t camera, std::uint64_t operation)
    {
        if (stopped_) return;
        {
            // An operation that is gone (the camera left the session, or another change replaced it) does nothing.
            std::lock_guard lock(notesMutex_);
            const auto pin = ratioPins_.find(camera);
            if (!pin || pin->operation != operation) return;
            if (!ratioPins_.timedOut(camera, operation, std::max(hostNow(), pin->armedAt + ratioCompletionBudgetNanoseconds))) return;
        }
        std::lock_guard lock(state_->mutex);
        if (state_->session) failRatio(camera, Refusal::ratioTimedOut, "the change did not complete within " + std::to_string(ratioCompletionBudgetNanoseconds / 1000000) + " ms");
    }

    // The camera's last input or output left the session (or an admission that added its input was undone): its change is
    // over, and no watch or completion of it fails the session the other cameras are in. What the notes say of it stays.
    void forgetRatio(std::size_t camera)
    {
        std::lock_guard lock(notesMutex_);
        ratioPins_.invalidate(camera);
    }
#else
    bool ratioAdmitsSample(std::size_t, std::optional<std::int64_t>, std::size_t, std::size_t) { return true; }
    void armRatioWatch(std::size_t) {}
    void forgetRatio(std::size_t) {}
#endif

    // The completion handler of a ratio change the device is asked for (activate): it reaches the session only while
    // the session lives, under the state's mutex like every callback of the platform's.
    // Each call is one operation (a change asked for, or found made), numbered so that a completion belongs to it alone.
    // The completion's timestamp is on the camera's input port's clock, which the callback keeps (the call is made on the
    // session's queue, where the input is, and the completion arrives on the device's).
    RatioDone ratioDone(std::size_t camera)
    {
        std::shared_ptr<SessionState> state = state_;
        const std::uint64_t operation = ++ratioOperation_;
        std::shared_ptr<const void> from;
#if TARGET_OS_IOS
        if (const auto running = cameras_.find(camera); running != cameras_.end() && running->second.input != nil)
            for (AVCaptureInputPort* port in running->second.input.ports)
                if ([port.mediaType isEqualToString:AVMediaTypeVideo] && port.clock != nullptr) {
                    from = std::shared_ptr<const void>(CFRetain(port.clock), [](const void* clock) { CFRelease(clock); });
                    break;
                }
#endif
        return [state, camera, operation, from](const RatioCompletion& completion) {
            std::lock_guard lock(state->mutex);
            if (state->session) static_cast<AppleSession*>(state->session)->ratioCompleted(camera, operation, completion, from.get());
        };
    }

    // Logs a refusal once per camera and reason until the camera's configuration
    // changes (RefusalLog), whoever asks again and however often.
    void refuse(std::size_t camera, Refusal reason, const std::string& detail = {})
    {
        if (!refusals_.first(camera, reason)) return;
        std::fprintf(stderr, "Camera: camera %zu is refused (%s)%s%s\n", camera, std::string(refusalName(reason)).c_str(),
                     detail.empty() ? "" : ": ", detail.c_str());
        // The record counts the refusals that were logged and names the last one's reason.
        note(camera, [&](CameraPresentation& held) {
            held.refusals = std::uint16_t(std::min<unsigned>(held.refusals + 1u, 65535u));
            held.lastRefusal = std::int8_t(reason);
        });
    }

    // Runs one platform call; an exception it raises is a refusal.
    bool guarded(std::size_t camera, const std::function<bool()>& work)
    {
        @try {
            return work();
        } @catch (NSException* exception) {
            refuse(camera, Refusal::raised, exception.reason != nil ? exception.reason.UTF8String : "");
            return false;
        }
    }

    // Puts a device's timing back as a refused change found it (putBackTiming). A
    // device that would not be put back is logged, once, and what the camera's
    // stream and settings are believed to hold is no longer known: the next
    // request for the stream writes the format, the durations and the size again,
    // and the settings with them, or fails (StreamCache).
    void restoreTiming(std::size_t camera, AVCaptureDevice* device, const std::optional<DeviceTiming>& held)
    {
        if (!held) return;
        const bool ok = guarded(camera, [&] { return putBackTiming(*held, timingStepsOf(device)); });
        if (ok) return;
        cameras_[camera].restoreRefused();
        refuse(camera, Refusal::notRestored);
    }

    const Anchor* anchorOf(std::size_t camera) const
    {
        return camera < anchors_.size() ? anchors_[camera].get() : nullptr;
    }

    // The cameras with an input in the session: those that stream and those
    // that only take stills, a still in flight included.
    std::vector<std::size_t> peersOf(std::size_t camera) const
    {
        std::vector<std::size_t> result;
        for (const auto& [index, running] : cameras_)
            if (index != camera && running.input != nil) result.push_back(index);
        return result;
    }

    // Brings a camera into the session as its anchor: the input, the format and
    // the rate, and the outputs it gains (a stream's video output and a still's
    // photo output). One transaction for every entry (uml::platform::admit): a
    // refusal removes what this call added and nothing else, so a camera that
    // was in the session stays as it was, in its anchor.
    bool admitCamera(std::size_t camera, bool video, bool photo, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate)
    {
        const Entry& entry = entries_[camera];
        auto& running = cameras_[camera];
        const Anchor* anchor = anchorOf(camera);
        if (anchor == nullptr) {
            refuse(camera, Refusal::noAnchor);
            return false;
        }
        // A multi-camera session takes the formats it lists for every input,
        // whatever each is for: this anchor and every other input's must be one.
        const auto peers = multiCamera_ ? peersOf(camera) : std::vector<std::size_t> {};
        auto peerFormats = std::make_unique<bool[]>(peers.size() + 1);
        for (std::size_t index = 0; index < peers.size(); ++index) {
            const Anchor* other = anchorOf(peers[index]);
            peerFormats[index] = other != nullptr && other->facts.multiCamera;
        }
        if (!mayJoin(!peers.empty(), anchor->facts.multiCamera, std::span<const bool>(peerFormats.get(), peers.size()))) {
            refuse(camera, Refusal::notJoinable);
            return false;
        }
        std::optional<ProgrammedRate> rate;
        if (video) {
            rate = programmedFor(*anchor, frameRate, pressureCapOf(running));
            if (!rate) {
                refuse(camera, Refusal::raised, "the anchor takes no rate near " + std::to_string(frameRate));
                return false;
            }
        }
        AVCaptureDeviceFormat* const activeBefore = entry.device.activeFormat;
        std::optional<DeviceTiming> held;
        AVCaptureVideoDataOutput* newVideo = nil;
        UMLCameraOutputDelegate* newDelegate = nil;
        AVCaptureConnection* newConnection = nil;
        AVCapturePhotoOutput* newPhoto = nil;
        AdmitPlan plan;
        plan.addInput = running.input == nil;
        plan.activate = plan.addInput || video;
        plan.addVideo = video && running.videoOutput == nil;
        plan.addPhoto = photo && running.photoOutput == nil;
#if TARGET_OS_IOS
        // What this admission set on the device and its connections, for the record.
        std::optional<ConnectionFacts> videoConnection;
        std::optional<PhotoOutputFacts> photoFacts;
        std::optional<CameraStillSize> photoMaximumSet;
        std::optional<RatioFacts> ratioFacts;
        // The stabilization the new video connection was programmed with before the configuration committed.
        VideoStabilizationHeld admittedStabilization;
        std::optional<VideoStabilizationStep> admittedStabilizationStep;
#endif
        AdmitSteps steps;
        steps.begin = [&] { [session_ beginConfiguration]; };
        steps.commit = [&] {
            commitConfiguration();
            photoPipelinesSettle();
        };
        steps.addInput = [&] { return guarded(camera, [&] { return addInput(entry, running); }); };
        steps.snapshotTiming = [&] { held = timingOf(entry.device); };
        steps.restoreTiming = [&] { restoreTiming(camera, entry.device, held); };
        steps.activate = [&] {
            return guarded(camera, [&] {
                const bool activated = activate(entry.device, *anchor, rate, true, ratioDone(camera));
#if TARGET_OS_IOS
                if (activated && entry.mounting.presentationTurn == 180) {
                    // The session's own choice ends here, before the selection: while it is on, a device reads back the wide
                    // colour space the session picks at once, whatever it is set to (commitConfiguration keeps it off while
                    // the camera is in the session).
                    session_.automaticallyConfiguresCaptureDeviceForWideColor = NO;
                    if (!keepSrgbColourSpace(entry.device))
                        std::fprintf(stderr, "Camera: camera %zu: the capture is not held in sRGB (colour space %ld): a photo that states no colours is refused\n",
                                     camera, long(entry.device.activeColorSpace));
                }
                if (anchor->facts.pinnedRatio != CameraAspectRatio::none) ratioFacts = ratioFactsOf(entry.device, anchor->facts.pinnedRatio);
#endif
                return activated;
            });
        };
        steps.addVideo = [&] {
            return guarded(camera, [&] {
                AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
                output.videoSettings = @{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:
                                             @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)};
                output.alwaysDiscardsLateVideoFrames = YES;
                UMLCameraOutputDelegate* delegate = [[UMLCameraOutputDelegate alloc] initWithCamera:camera state:state_];
                [output setSampleBufferDelegate:delegate queue:outputQueue_];
                newDelegate = delegate;
                if (multiCamera_) {
#if TARGET_OS_IOS
                    AVCaptureInputPort* port = [running.input portsWithMediaType:AVMediaTypeVideo
                                                                 sourceDeviceType:entry.device.deviceType
                                                             sourceDevicePosition:entry.device.position].firstObject;
                    if (port == nil || ![session_ canAddOutput:output]) return false;
                    [(AVCaptureMultiCamSession*)session_ addOutputWithNoConnections:output];
                    newVideo = output;
                    AVCaptureConnection* connection = [[AVCaptureConnection alloc] initWithInputPorts:@[port] output:output];
                    if (![session_ canAddConnection:connection]) return false;
                    [session_ addConnection:connection];
                    newConnection = connection;
#else
                    return false;
#endif
                } else {
                    if (![session_ canAddOutput:output]) return false;
                    [session_ addOutput:output];
                    newVideo = output;
                    newConnection = [output connectionWithMediaType:AVMediaTypeVideo];
                }
#if TARGET_OS_IOS
                // The connection's angle comes first and the output's size after it: the output delivers buffers turned by
                // the angle, and the size must match them (AVCaptureVideoDataOutput.h). A camera that imitates an earlier
                // front camera is refused when its connection will not hold the turn it needs, never left to deliver
                // frames of another shape or upright the other way.
                const ConnectionFacts connection = keepSensorOrientation(newConnection, entry.mounting.connectionTurn);
                videoConnection = connection;
                if (entry.mounting.emulatesEarlierFront && (!connection.angleSupported || connection.angle != std::int16_t(entry.mounting.connectionTurn))) {
                    refuse(camera, Refusal::turnNotHeld,
                           "the video connection does not hold a turn of " + std::to_string(entry.mounting.connectionTurn) + " (it has " +
                               std::to_string(connection.angle) + ", by default " + std::to_string(connection.defaultAngle) + ")");
                    return false;
                }
#endif
                // The stream's size, scaled by the platform from the anchor's at
                // the anchor's aspect ratio: set once the output has its source.
                setVideoOutputSettings(output, videoSettingsOf(anchor->facts, width, height));
                return true;
            });
        };
#if TARGET_OS_IOS
        // The stabilization the guest asks for, on the connection before the configuration commits: a session that runs
        // already (a lens added to it) starts the new output at the commit, and a session that starts after it delivers from
        // its first frame, so no frame of this connection has another crop than the rest.
        steps.programVideo = [&] {
            admittedStabilizationStep = writeStabilization(camera, entry.device, newConnection, admittedStabilization,
                                                           running.stabilizationWish.atAdmission());
        };
#endif
        steps.addPhoto = [&] {
            return guarded(camera, [&] {
                AVCapturePhotoOutput* output = [[AVCapturePhotoOutput alloc] init];
#if TARGET_OS_IOS
                output.maxPhotoQualityPrioritization = AVCapturePhotoQualityPrioritizationQuality;
#endif
#if TARGET_OS_IOS
                // The orientation of the output's stills; a camera that imitates an earlier front camera needs the output
                // to turn them (its compensation, supported and on). What the output says of it is only known to hold
                // once the configuration is committed, so nothing is refused here: a still reads it then (startStill).
                const auto keepOrientation = [&](AVCapturePhotoOutput* attached) -> bool {
                    photoFacts = keepStillOrientation(attached, entry.mounting);
                    return true;
                };
#endif
                const auto attach = [&]() -> bool {
                    if (multiCamera_) {
#if TARGET_OS_IOS
                        AVCaptureInputPort* port = [running.input portsWithMediaType:AVMediaTypeVideo
                                                                     sourceDeviceType:entry.device.deviceType
                                                                 sourceDevicePosition:entry.device.position].firstObject;
                        if (port == nil || ![session_ canAddOutput:output]) return false;
                        [(AVCaptureMultiCamSession*)session_ addOutputWithNoConnections:output];
                        newPhoto = output;
                        AVCaptureConnection* connection = [[AVCaptureConnection alloc] initWithInputPorts:@[port] output:output];
                        if (![session_ canAddConnection:connection]) return false;
                        [session_ addConnection:connection];
                        return keepOrientation(output);
#else
                        return false;
#endif
                    }
                    if (![session_ canAddOutput:output]) return false;
                    [session_ addOutput:output];
                    newPhoto = output;
#if TARGET_OS_IOS
                    return keepOrientation(output);
#else
                    return true;
#endif
                };
                // The output makes photos up to the anchor's photoMaximum, which
                // AVFoundation requires to be one of the active format's sizes
                // of the output's source: set after the output is attached.
                const auto setMaximum = [&](CameraStillSize size) -> bool {
                    @try {
                        output.maxPhotoDimensions = CMVideoDimensions {std::int32_t(size.width), std::int32_t(size.height)};
#if TARGET_OS_IOS
                        photoMaximumSet = size;
#endif
                        return true;
                    } @catch (NSException* exception) {
                        // The first refusal's detail is the one logged; the admission's own
                        // refusal of the output after it is the same reason and stays quiet.
                        refuse(camera, Refusal::outputRefused,
                               "the photo output refused photos up to " + std::to_string(size.width) + "x" + std::to_string(size.height) +
                                   ": " + (exception.reason != nil ? exception.reason.UTF8String : ""));
                        return false;
                    }
                };
                const auto detach = [&]() {
                    @try {
                        [session_ removeOutput:output];
                    } @catch (NSException*) {
                    }
                    newPhoto = nil;
                };
                return preparePhotoOutput(photoMaximum(anchor->facts, limits_.maximumStillPixels), {attach, setMaximum, detach}) ==
                    PhotoOutputPreparation::ready;
            });
        };
        steps.overBudget = [&] { return overBudget(); };
        steps.removeVideo = [&] {
            if (newVideo == nil) return;
            @try {
                [session_ removeOutput:newVideo];
            } @catch (NSException*) {
            }
            newVideo = nil;
            newDelegate = nil;
            newConnection = nil;
        };
        steps.removePhoto = [&] {
            if (newPhoto == nil) return;
            @try {
                [session_ removeOutput:newPhoto];
            } @catch (NSException*) {
            }
            newPhoto = nil;
        };
        steps.removeInput = [&] {
            if (running.input == nil) return;
            @try {
                [session_ removeInput:running.input];
            } @catch (NSException*) {
            }
            running.input = nil;
            running.format = nil;
        };
        const Admission result = admit(plan, steps);
#if TARGET_OS_IOS
        noteAdmission(camera, videoConnection, photoFacts, photoMaximumSet, ratioFacts, video ? CameraStillSize {width, height} : CameraStillSize {},
                      newPhoto, result == Admission::admitted);
#endif
        if (result != Admission::admitted) {
            // An admission that added the camera's input and was undone leaves no change under way; a camera that was in the
            // session before keeps the change its anchor's admission asked for.
            if (running.input == nil) forgetRatio(camera);
            switch (result) {
            case Admission::inputRefused: refuse(camera, Refusal::inputRefused); break;
            case Admission::formatRefused: refuse(camera, Refusal::raised, "the device refused its anchor"); break;
            case Admission::videoRefused:
            case Admission::photoRefused: refuse(camera, Refusal::outputRefused); break;
            case Admission::overBudget: refuse(camera, Refusal::overBudget); break;
            case Admission::admitted: break;
            }
            return false;
        }
        if (newVideo != nil) {
            running.videoOutput = newVideo;
            running.delegate = newDelegate;
            running.connection = newConnection;
#if TARGET_OS_IOS
            running.stabilization = admittedStabilization;
            if (admittedStabilizationStep) recordStabilization(camera, *admittedStabilizationStep);
#else
            running.stabilization = {};
#endif
        }
        if (newPhoto != nil) {
            running.photoOutput = newPhoto;
            photoPipelinesSettle();
        }
        running.format = anchor->format;
        // The timing written again is known; what was applied before is written again after it.
        const bool settingsStale = plan.activate && running.timingWritten();
        if (video) {
            running.video = true;
            running.width = width;
            running.height = height;
            running.frameRate = frameRate;
            beginPressureWatch(entry, running, anchors_[camera], frameRate);
        }
        refusals_.reset(camera);
        // A format change reverts some settings of the device: put them back.
        if ((activeBefore != anchor->format || settingsStale) && running.applied) applyOnQueue(camera, running.settings, true);
        logJoin(camera);
        startSession();
        armRatioWatch(camera);
        return true;
    }

    // A running stream changes size or rate (uml::platform::resizeStream): the
    // durations first, then the size; a refusal programs the old durations again.
    bool resizeCamera(std::size_t camera, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate)
    {
        const Entry& entry = entries_[camera];
        auto& running = cameras_[camera];
        const Anchor* anchor = anchorOf(camera);
        if (anchor == nullptr) {
            refuse(camera, Refusal::noAnchor);
            return false;
        }
        const bool rateChanges = running.rateChanges(frameRate);
        const bool sizeChanges = running.sizeChanges(width, height);
        std::optional<DeviceTiming> held;
        ResizeSteps steps;
        steps.begin = [&] { [session_ beginConfiguration]; };
        steps.commit = [&] {
            commitConfiguration();
            photoPipelinesSettle();
        };
        steps.snapshotTiming = [&] { held = timingOf(entry.device); };
        if (rateChanges)
            steps.setDurations = [&] {
                return guarded(camera, [&] {
                    const auto rate = programmedFor(*anchor, frameRate, pressureCapOf(running));
                    return rate && activate(entry.device, *anchor, rate);
                });
            };
        if (sizeChanges)
            steps.setVideoSize = [&] {
                return guarded(camera, [&] {
                    setVideoOutputSettings(running.videoOutput, videoSettingsOf(anchor->facts, width, height));
                    return true;
                });
            };
        steps.restoreTiming = [&] { restoreTiming(camera, entry.device, held); };
        const Resizing result = resizeStream(steps);
        if (result != Resizing::resized) {
            refuse(camera, result == Resizing::durationsRefused ? Refusal::raised : Refusal::outputRefused,
                   result == Resizing::durationsRefused ? "the device refused the rate" : "the output refused the size");
            return false;
        }
        running.width = width;
        running.height = height;
        running.frameRate = frameRate;
        if (running.watch) running.watch->request(frameRate);
        refusals_.reset(camera);
        // Durations that were not known are written again, so are the settings the shorter ones may have cut.
        if (running.timingWritten() && running.applied) applyOnQueue(camera, running.settings, true);
        return true;
    }

    bool startVideoOnQueue(std::size_t camera, std::uint32_t width, std::uint32_t height, std::uint32_t frameRate)
    {
        switch (cameras_[camera].pathFor(width, height, frameRate)) {
        case StreamCache::Path::cached: return true;
        case StreamCache::Path::resize: return resizeCamera(camera, width, height, frameRate);
        case StreamCache::Path::admit: return admitCamera(camera, true, false, width, height, frameRate);
        }
        return false;
    }

    // After an interruption or a restart (uml::platform::recoverAnchors): an
    // input whose device is no longer in its anchor is put back, with the sizes
    // of its outputs, which follow the format (a stream's frame and a photo's
    // maximum). Only when every camera is in its anchor is the session said to be
    // back; a repair that is refused (the device would not lock, a setter raised)
    // says it failed, which the client answers by closing the session and opening
    // another, whose cameras start in their anchors. This runs on the session's
    // queue and the announcement is the last thing it does.
    void recoverAfterInterruption()
    {
        if (stopped_) return;
        std::vector<std::size_t> inputs;
        for (const auto& [camera, running] : cameras_)
            if (running.input != nil) inputs.push_back(camera);
        RecoverySteps steps;
        steps.outOfAnchor = [&](std::size_t camera) {
            const Anchor* anchor = anchorOf(camera);
            return anchor != nullptr && entries_[camera].device.activeFormat != anchor->format;
        };
        bool repaired = false;
        steps.repair = [&](std::size_t camera) {
            const bool ok = repairAnchor(camera);
            repaired = repaired || ok;
            return ok;
        };
        steps.announce = [&](Recovery outcome) {
            if (outcome == Recovery::failed) std::fprintf(stderr, "Camera: the cameras could not be put back in their anchors\n");
            // What the capture made before this moment is from before the repair: its clock now, and the boundary
            // drawn under the lock that delivery holds, before the session is said to be back.
            const bool drawBoundary = outcome == Recovery::resumed && repaired;
            const std::optional<std::int64_t> captureClock = drawBoundary ? captureClockNow() : std::nullopt;
            std::lock_guard lock(state_->mutex);
            if (drawBoundary) state_->boundary.repaired(captureClock);
            if (state_->session) deliver({allCameras, outcome == Recovery::resumed ? CameraEventKind::resumed : CameraEventKind::failed});
        };
        // A failed recovery is final for the session: a second notification (the end
        // of the interruption and the restart both arrive) repairs and announces
        // nothing more, whatever state the first left the cameras in.
        (void)recovery_.recover(inputs, steps);
    }

    // The capture's own clock now: the timeline of its samples' timestamps (the session's synchronization clock),
    // which is not assumed to be the host's.
    std::optional<std::int64_t> captureClockNow() const
    {
        CMClockRef clock = session_.synchronizationClock;
        return clock != nullptr ? nanosecondsOf(CMClockGetTime(clock)) : std::nullopt;
    }

    // Puts a camera whose device was moved out of its anchor back: the format
    // and the durations of its stream, the sizes of its outputs, its settings.
    bool repairAnchor(std::size_t camera)
    {
        auto& running = cameras_[camera];
        const Anchor* anchor = anchorOf(camera);
        const Entry& entry = entries_[camera];
        std::fprintf(stderr, "Camera: camera %zu's device was moved out of its anchor: put back\n", camera);
        [session_ beginConfiguration];
#if TARGET_OS_IOS
        std::optional<RatioFacts> ratioFacts;
#endif
        const bool ok = guarded(camera, [&] {
            if (!activate(entry.device, *anchor, running.video ? programmedFor(*anchor, running.frameRate, pressureCapOf(running)) : std::nullopt,
                          true, ratioDone(camera)))
                return false;
#if TARGET_OS_IOS
            if (anchor->facts.pinnedRatio != CameraAspectRatio::none) ratioFacts = ratioFactsOf(entry.device, anchor->facts.pinnedRatio);
#endif
#if TARGET_OS_IOS
            // A camera that imitates an earlier front camera is held at its turn before the output's size is set again
            // (admitCamera); a connection that does not hold it fails the repair.
            if (entry.mounting.emulatesEarlierFront && running.connection != nil) {
                const ConnectionFacts connection = keepSensorOrientation(running.connection, entry.mounting.connectionTurn);
                if (!connection.angleSupported || connection.angle != std::int16_t(entry.mounting.connectionTurn)) {
                    refuse(camera, Refusal::turnNotHeld,
                           "the video connection does not hold a turn of " + std::to_string(entry.mounting.connectionTurn) + " (it has " +
                               std::to_string(connection.angle) + ")");
                    return false;
                }
            }
#endif
            if (running.videoOutput != nil)
                setVideoOutputSettings(running.videoOutput, videoSettingsOf(anchor->facts, running.width, running.height));
            if (running.photoOutput != nil)
                if (const auto maximum = photoMaximum(anchor->facts, limits_.maximumStillPixels))
                    running.photoOutput.maxPhotoDimensions = CMVideoDimensions {std::int32_t(maximum->width), std::int32_t(maximum->height)};
            return true;
        });
        commitConfiguration();
        photoPipelinesSettle();
#if TARGET_OS_IOS
        noteRepair(camera, running, ratioFacts);
#endif
        // A stream's durations are written with its anchor; a still-only camera's are written when it streams.
        if (ok && running.video) running.timingWritten();
        if (ok && running.applied) applyOnQueue(camera, running.settings, true);
        // The ratio pinned again is under way until its completion: the time it may take begins now.
        armRatioWatch(camera);
        return ok;
    }

    // Commits the session's configuration. A session sets its devices to wide colour (Display P3, preferred for photos) at a
    // commit, and a device's own colour space selection does not hold against it. The stills of a camera whose stills the
    // host turns are encoded with no colour profile, so its capture is held in sRGB (keepSrgbColourSpace): the session's
    // choice is the host's while such a camera's input is in the session, set before the commit that applies it, and is the
    // system's for every other session (back-only and external-only ones included), as it always was.
    void commitConfiguration()
    {
#if TARGET_OS_IOS
        std::vector<std::uint32_t> turns;
        for (const auto& [index, running] : cameras_)
            if (running.input != nil) turns.push_back(entries_[index].mounting.presentationTurn);
        session_.automaticallyConfiguresCaptureDeviceForWideColor = !sessionHoldsColourSpaces(turns);
#endif
        [session_ commitConfiguration];
    }

    void startSession()
    {
        @try {
            if (!session_.running) [session_ startRunning];
        } @catch (NSException*) {
            // The session's runtime error says how it went.
        }
    }

    // Whether the multi-camera session's hardware is over its budget (it would
    // not start): a join that does that is undone.
    bool overBudget() const
    {
#if TARGET_OS_IOS
        return multiCamera_ && ((AVCaptureMultiCamSession*)session_).hardwareCost > 1.0f;
#else
        return false;
#endif
    }

    // The first time a camera joins others in the session, its hardware cost.
    void logJoin(std::size_t camera)
    {
#if TARGET_OS_IOS
        if (!multiCamera_ || peersOf(camera).empty() || !joinsLogged_.insert(camera).second) return;
        std::fprintf(stderr, "Camera: camera %zu joined the multi-camera session: hardware cost %.2f of 1.00\n", camera,
                     double(((AVCaptureMultiCamSession*)session_).hardwareCost));
#else
        (void)camera;
#endif
    }

    // Adds a camera's input to the session; nothing is left behind on failure.
    bool addInput(const Entry& entry, RunningCamera& running)
    {
        NSError* error = nil;
        AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:entry.device error:&error];
        if (input == nil || ![session_ canAddInput:input]) return false;
#if TARGET_OS_IOS
        if (multiCamera_) {
            [(AVCaptureMultiCamSession*)session_ addInputWithNoConnections:input];
            running.input = input;
            return true;
        }
#endif
        [session_ addInput:input];
        running.input = input;
        return true;
    }

    // Takes a camera's outputs, and its input once it has none left, out of
    // the session. The camera's ratio change goes with its last input (forgetRatio): while an output or a still in flight
    // needs the camera, its change stays.
    void removeFromSession(std::size_t camera, RunningCamera& running, bool video, bool photo)
    {
        [session_ beginConfiguration];
        @try {
            if (video && running.videoOutput) [session_ removeOutput:running.videoOutput];
            if (photo && running.photoOutput) [session_ removeOutput:running.photoOutput];
        } @catch (NSException*) {
        }
        if (video) {
            running.videoOutput = nil;
            running.delegate = nil;
            running.connection = nil;
            running.stabilization = {};
            running.video = false;
        }
        if (photo) running.photoOutput = nil;
        if (cameraLeavesSession(running.input != nil, running.videoOutput != nil, running.photoOutput != nil)) {
            @try {
                [session_ removeInput:running.input];
            } @catch (NSException*) {
            }
            running.input = nil;
            running.format = nil;
            forgetRatio(camera);
        }
        commitConfiguration();
        photoPipelinesSettle();
    }

    // The session committed a configuration (an admission, a stream's resize, a repair of an anchor, a removal), and
    // the pipeline of every photo output whose maximum needs the full-resolution mode is reconfigured again: it takes
    // its time from now. The one bookkeeping of every commit (photoMaximumCommittedAt).
    void photoPipelinesSettle()
    {
        const std::uint64_t now = hostNow();
        for (auto& [camera, running] : cameras_) {
            const Anchor* anchor = anchorOf(camera);
            if (running.photoOutput == nil || anchor == nullptr) continue;
            running.photoMaximumCommittedAt =
                uml::platform::photoMaximumCommittedAt(photoMaximum(anchor->facts, limits_.maximumStillPixels), now);
        }
    }

    // Called on the session's queue after a still is delivered: a camera that
    // streams keeps its photo output, one that only takes stills leaves.
    void releaseStill(std::size_t camera)
    {
        if (stopped_ || camera >= entries_.size()) return;
        auto& running = cameras_[camera];
        if (running.video || running.photoOutput == nil || stillsInFlight(camera)) return;
        removeFromSession(camera, running, false, true);
    }

    // The system's pressure on a camera lowers the rate it runs at, as Apple
    // advises (heat that goes on rising ends in the session's interruption);
    // the capture's client sees fewer frames, and the interruption itself
    // arrives as an event of its own. The handler and the work it queues hold
    // the watch, the device and the anchor themselves (copied into the blocks
    // from locals: a block that named a parameter or a member by reference would
    // reach what the session may have freed by the time it runs), and the work
    // does nothing once the watch has ended.
    void beginPressureWatch(const Entry& entry, RunningCamera& running, std::shared_ptr<const Anchor> anchor, std::uint32_t frameRate)
    {
        if (running.watch) return;
        auto watch = std::make_shared<PressureWatch>(anchor->facts, anchor->rates);
        watch->request(frameRate);
        running.watch = watch;
#if TARGET_OS_IOS
        AVCaptureDevice* device = entry.device;
        dispatch_queue_t queue = queue_;
        // The device's system pressure (heat, mostly) as it changes.
        running.pressure = [[UMLKeyPathObserver alloc] initWithObject:device
                                                              keyPath:@"systemPressureState"
                                                              handler:^(id value) {
            AVCaptureSystemPressureState* state = value;
            if (state == nil) return;
            AVCaptureSystemPressureLevel level = state.level;
            std::uint32_t cap = 0;
            if ([level isEqualToString:AVCaptureSystemPressureLevelSerious]) cap = 20;
            else if ([level isEqualToString:AVCaptureSystemPressureLevelCritical] ||
                     [level isEqualToString:AVCaptureSystemPressureLevelShutdown])
                cap = 15;
            if (!watch->pressure(cap)) return;
            // The frame durations alone: the device is in its anchor already.
            dispatch_async(queue, ^{
                if (const auto rate = watch->programmed()) (void)activate(device, *anchor, rate);
            });
        }];
#else
        (void)entry;
#endif
    }

    // Nothing queued by the observer programs the device afterwards.
    static void endPressureWatch(RunningCamera& running)
    {
        if (running.watch) running.watch->end();
        running.watch.reset();
#if TARGET_OS_IOS
        [running.pressure invalidate];
        running.pressure = nil;
#endif
    }

    // A still needs the anchor, the input and a photo output: the camera joins
    // the session as a video camera does.
    bool prepareStill(std::size_t camera)
    {
        if (cameras_[camera].photoOutput != nil) return true;
        return admitCamera(camera, false, true, 0, 0, 0);
    }

    // The captures on their way: their delegates are held here.
    struct Photo {
        std::size_t camera {};
        UMLPhotoDelegate* delegate {};
        // What was asked, for the capture that follows one whose photo is not the size asked for.
        CameraStillRequest request;
        unsigned attempt {1};
        // What the request has waited for the photo output's settle time, over its attempts (stillWaitBudgetNanoseconds).
        std::uint64_t waited {};
    };
    bool stillsInFlight(std::size_t camera)
    {
        std::lock_guard lock(photoMutex_);
        return std::any_of(photos_.begin(), photos_.end(), [camera](const auto& entry) { return entry.second.camera == camera; });
    }

    std::vector<Entry> entries_;
    CameraSessionDelegate& delegate_;
    std::shared_ptr<SessionState> state_;
    std::mutex photoMutex_;
    std::map<std::uint64_t, Photo> photos_;
    AVCaptureSession* session_ {};
    dispatch_queue_t queue_;
    dispatch_queue_t outputQueue_;
    id interrupted_ {};
    id resumed_ {};
    id failed_ {};
    id started_ {};
    std::shared_ptr<std::atomic_bool> restarting_ {std::make_shared<std::atomic_bool>(false)};
    /* Frames have reached the session since it started or since the application's departure was last reported; the
     * interruption observer holds a copy, as it may run after the session is gone. */
    std::shared_ptr<std::atomic_bool> ranInForeground_ {std::make_shared<std::atomic_bool>(false)};
    bool multiCamera_ {};
    // What the anchors were chosen for: the still cap bounds a photo output.
    CameraLimits limits_;
    // Each camera's anchor, by index of cameras(); null for a camera the device
    // does not offer.
    std::vector<std::shared_ptr<const Anchor>> anchors_;
    RefusalLog refusals_;
    // How each camera is presented, by index of cameras() (note()); the notes' own lock is taken after the state's.
    std::mutex notesMutex_;
    std::vector<CameraPresentation> notes_;
    // The state of each camera's pinned aspect ratio change, by index of cameras() (guarded by notesMutex_): the
    // operation, whether it is under way, its completion's error code, and whether its time ran out (RatioPin).
    RatioPins ratioPins_;
    // The number of the last ratio change asked for (on the session's queue).
    std::uint64_t ratioOperation_ {};
    std::set<std::size_t> joinsLogged_;
    std::atomic_bool stopped_ {};
    // The recoveries of this session: a failure stays until the session is torn
    // down.
    RecoveryLatch recovery_;
    std::map<std::size_t, RunningCamera> cameras_;
};

class AppleCamera final : public Camera {
public:
    // Discovery calls the platform, which can raise where a host lacks what the build assumes (an iPad app on a Mac
    // lacks the rotation coordinator's angles). `catch (const std::exception&)` does not catch an Objective-C
    // exception: a start with a catch-all fails on it and a caller with none is ended by it, so the exception stops
    // here: the host presents no cameras, with one line saying why.
    AppleCamera()
    {
        @try {
            entries_ = discover();
#if TARGET_OS_IOS
            // The sets AVFoundation lists, by position in cameras().
            NSMutableArray<AVCaptureDeviceType>* types = [NSMutableArray array];
            for (const auto& entry : entries_) [types addObject:entry.device.deviceType];
            AVCaptureDeviceDiscoverySession* session =
                [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                                       mediaType:AVMediaTypeVideo
                                                                        position:AVCaptureDevicePositionUnspecified];
            for (NSSet<AVCaptureDevice*>* set in session.supportedMultiCamDeviceSets) {
                std::vector<std::size_t> members;
                for (AVCaptureDevice* device in set)
                    for (std::size_t index = 0; index < entries_.size(); ++index)
                        if ([entries_[index].device.uniqueID isEqualToString:device.uniqueID]) members.push_back(index);
                std::sort(members.begin(), members.end());
                if (members.size() > 1) sets_.push_back(std::move(members));
            }
#endif
        } @catch (NSException* exception) {
            entries_.clear();
            sets_.clear();
            std::fprintf(stderr, "Camera: discovery raised %s (%s): the host presents no cameras\n",
                         exception.name != nil ? exception.name.UTF8String : "an exception",
                         exception.reason != nil ? exception.reason.UTF8String : "no reason");
        }
    }

    std::vector<CameraInfo> cameras() const override
    {
        std::vector<CameraInfo> infos;
        for (const auto& entry : entries_) infos.push_back(entry.info);
        return infos;
    }

    std::vector<std::vector<std::size_t>> concurrentSets() const override { return sets_; }

    std::string hostModel() const override
    {
        char model[128] = {};
        std::size_t size = sizeof(model);
#if TARGET_OS_IOS
        const char* name = "hw.machine";
#else
        const char* name = "hw.model";
#endif
        if (sysctlbyname(name, model, &size, nullptr, 0) != 0) return "unknown";
        return model;
    }

    std::uint64_t now() const noexcept override { return hostNow(); }

    std::unique_ptr<Nv12Transformer> makeFrameTransformer() const override { return makeAcceleratedNv12Transformer(); }

    std::uint32_t stillWorkingBytesPerPixel() const noexcept override { return systemCameraStillWorkingBytesPerPixel; }
    std::uint32_t turnedStillWorkingBytesPerPixel() const noexcept override { return systemCameraTurnedStillWorkingBytesPerPixel; }

    std::unique_ptr<CameraSession> openSession(CameraSessionDelegate& delegate) override
    {
        if (entries_.empty()) return nullptr;
        // The system's answer is read, never asked for: a session is the one
        // act that starts the capture indicator, and only a granted app may.
        if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo] != AVAuthorizationStatusAuthorized)
            return nullptr;
        return std::make_unique<AppleSession>(entries_, delegate);
    }

private:
    std::vector<Entry> entries_;
    std::vector<std::vector<std::size_t>> sets_;
};

} // namespace

std::unique_ptr<Camera> makeAppleCamera()
{
    return std::make_unique<AppleCamera>();
}

} // namespace uml::platform

// ---------------------------------------------------------------------------

@implementation UMLCameraOutputDelegate {
    size_t _camera;
    std::shared_ptr<SessionState> _state;
}

- (instancetype)initWithCamera:(size_t)camera state:(std::shared_ptr<SessionState>)state
{
    if ((self = [super init])) {
        _camera = camera;
        _state = std::move(state);
    }
    return self;
}

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection
{
    (void)output;
    (void)connection;
    // The sample's capture time on the capture's clock, and the boundary's generation (read inside deliverSample),
    // are both taken before this callback waits for the lock the recovery's announcement holds.
    const auto capturedAt = uml::platform::nanosecondsOf(CMSampleBufferGetPresentationTimeStamp(sampleBuffer));
    uml::platform::deliverSample(_state->boundary, _state->mutex, capturedAt, [&] {
        if (_state->session) static_cast<uml::platform::AppleSession*>(_state->session)->deliverFrame(_camera, sampleBuffer);
    });
}

@end

namespace {

// What a still that did not end in a picture says about itself (camera_still_failure.hpp). The first failure of a
// capture stands: a later callback of the same capture names the same fault.
void noteStillFailure(uml::platform::CameraStillFailure& failure, uml::platform::CameraStillStage stage)
{
    if (failure.stage == uml::platform::CameraStillStage::none) failure.stage = stage;
}

// The framework's error as a domain word and a code: nothing else of it (no message, no description) is kept.
void noteFrameworkError(uml::platform::CameraStillFailure& failure, NSError* error)
{
    if (error == nil || failure.stage != uml::platform::CameraStillStage::none) return;
    failure.stage = uml::platform::CameraStillStage::frameworkError;
    failure.errorDomain = uml::platform::errorDomainOf(error.domain != nil ? error.domain.UTF8String : "");
    failure.errorCode = std::int32_t(std::clamp<NSInteger>(error.code, INT32_MIN, INT32_MAX));
}

// Whether the photo output supports turning its stills to an earlier model's sensor orientation, and whether it did for
// this capture; unknown where the host has no such output (macOS).
void noteCompensation(uml::platform::CameraStillFailure& failure, AVCapturePhotoOutput* output)
{
#if TARGET_OS_IOS
    if (output == nil) return;
    @try {
        const bool supported = output.cameraSensorOrientationCompensationSupported;
        failure.compensationSupported = supported ? uml::platform::CameraFact::yes : uml::platform::CameraFact::no;
        failure.compensationEnabled = supported && output.cameraSensorOrientationCompensationEnabled
            ? uml::platform::CameraFact::yes : uml::platform::CameraFact::no;
        failure.compensationReading = uml::platform::CameraReading::afterCommit;
    } @catch (NSException*) {
        // The facts stay unknown.
    }
#else
    (void)failure;
    (void)output;
#endif
}

} // namespace

@implementation UMLPhotoDelegate {
    size_t _camera;
    uint64_t _token;
    BOOL _wantJpeg;
    // The size of the JPEG the guest configured.
    uml::platform::CameraStillSize _jpegSize;
    // The largest file the host takes; zero for no bound.
    uint64_t _maximumBytes;
    BOOL _wantRaw;
    // The photo is uncompressed and turned half around into the JPEG (turnedStillOf), at the guest's quality; why it was
    // not, if it was not.
    BOOL _turnHalf;
    // The capture's colour space was the device's sRGB when the photo was asked for.
    BOOL _captureIsSrgb;
    unsigned _jpegQuality;
    uml::platform::TurnedStillFailure _turnFailure;
    // Which capture of the request this is (from one); a photo of another size makes the next.
    unsigned _attempt;
    std::shared_ptr<SessionState> _state;
    uml::platform::CameraStillResult _result;
    // What the photos' metadata and the Bayer still's DNG state of the
    // exposure. The processed photo's readings are the exposure's when there
    // is one; the Bayer still's otherwise.
    uml::platform::PhotoReadings _jpegReadings;
    uml::platform::PhotoReadings _rawReadings;
    // The Exif orientation (1 to 8) of the processed photo the output gave; zero for none.
    uint8_t _exifOrientation;
    uml::platform::DngExposureData _dng;
}

- (instancetype)initWithCamera:(size_t)camera
                         token:(uint64_t)token
                      wantJpeg:(BOOL)wantJpeg
                      jpegSize:(uml::platform::CameraStillSize)jpegSize
                  maximumBytes:(uint64_t)maximumBytes
                       wantRaw:(BOOL)wantRaw
                      turnHalf:(BOOL)turnHalf
                captureIsSrgb:(BOOL)captureIsSrgb
                   jpegQuality:(unsigned)jpegQuality
                       attempt:(unsigned)attempt
                         state:(std::shared_ptr<SessionState>)state
{
    if ((self = [super init])) {
        _turnHalf = turnHalf;
        _captureIsSrgb = captureIsSrgb;
        _jpegQuality = jpegQuality;
        _turnFailure = uml::platform::TurnedStillFailure::none;
        _attempt = attempt;
        _camera = camera;
        _token = token;
        _wantJpeg = wantJpeg;
        _jpegSize = jpegSize;
        _maximumBytes = maximumBytes;
        _wantRaw = wantRaw;
        _state = std::move(state);
        _result.token = token;
    }
    return self;
}

- (void)captureOutput:(AVCapturePhotoOutput*)output
    didFinishProcessingPhoto:(AVCapturePhoto*)photo
                       error:(NSError*)error
{
    noteCompensation(_result.failure, output);
    // A part that fails leaves its field empty and the other part alone: a Bayer
    // still that cannot be made does not take the person's JPEG with it.
    if (error != nil || photo == nil) {
        // The framework's error is kept as a domain word and a code for the session record; the capture goes on
        // without this part. A photo that is not there at all is missing data.
        noteFrameworkError(_result.failure, error);
        noteStillFailure(_result.failure, uml::platform::CameraStillStage::missingData);
        return;
    }
    if (_turnHalf) {
        [self takeTurnedPhoto:photo];
        return;
    }
    NSData* data = photo.fileDataRepresentation;
    // The photo's own Exif, read from its file: AVCapturePhoto.metadata is not
    // on the Mac.
    const auto readings = data == nil ? uml::platform::PhotoReadings {}
        : uml::platform::photoReadingsOfImage(
              std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data.bytes), data.length));
#if TARGET_OS_IOS
    const bool rawPhoto = photo.isRawPhoto;
#else
    const bool rawPhoto = false;
#endif
    // How the output tagged its processed photo: the host drops the tag, but it shows whether the output turned the photo
    // for the orientation the camera states (the session record).
    if (!rawPhoto && readings.orientation && *readings.orientation >= 1 && *readings.orientation <= 8)
        _exifOrientation = uint8_t(*readings.orientation);
#if TARGET_OS_IOS
    if (rawPhoto) {
        _rawReadings = readings;
        CVPixelBufferRef pixels = photo.pixelBuffer;
        if (pixels == nullptr || data == nil) return;
        const auto order = uml::platform::bayerOrderOf(CVPixelBufferGetPixelFormatType(pixels));
        const auto width = CVPixelBufferGetWidth(pixels);
        const auto height = CVPixelBufferGetHeight(pixels);
        if (order == uml::platform::BayerOrder::none || CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
            return;
        const auto* base = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(pixels));
        const auto stride = CVPixelBufferGetBytesPerRow(pixels);
        _result.raw.resize(width * height);
        for (std::size_t row = 0; row < height; ++row)
            std::memcpy(_result.raw.data() + row * width, base + row * stride, width * sizeof(std::uint16_t));
        CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
        // The DNG that comes with the Bayer pixels states what the sensor is.
        const auto bytes = std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(data.bytes), data.length);
        _dng = uml::platform::parseDngExposureData(bytes);
        if (auto color = uml::platform::parseDngColorData(bytes)) {
            _result.color = *color;
            if (_result.color.bayer == uml::platform::BayerOrder::none) _result.color.bayer = order;
            if (_result.color.rawWidth == 0) {
                _result.color.rawWidth = std::uint32_t(width);
                _result.color.rawHeight = std::uint32_t(height);
            }
        } else {
            _result.color.bayer = order;
            _result.color.rawWidth = std::uint32_t(width);
            _result.color.rawHeight = std::uint32_t(height);
        }
    } else
#endif
    if (data != nil) {
        // A file larger than the host takes for the size the guest configured (its JPEG buffer) is not
        // copied: the capture fails with those bytes still the system's alone (the host's cost of a
        // capture counts this file).
        const std::span<const std::uint8_t> file(static_cast<const std::uint8_t*>(data.bytes), data.length);
        if (!uml::platform::takeJpegFile(file, _maximumBytes, _wantJpeg, _result.jpeg)) {
            if (_wantJpeg) {
                std::fprintf(stderr, "Camera: the photo's file is %lu bytes, more than the %llu a %ux%u photo may take\n",
                             static_cast<unsigned long>(data.length), static_cast<unsigned long long>(_maximumBytes), _jpegSize.width,
                             _jpegSize.height);
                _result.failure.stage = uml::platform::CameraStillStage::fileSize;
            }
            return;
        }
        _jpegReadings = readings;
    }
}

// The camera's pictures are presented half around, so the photo was asked for uncompressed: its pixels are turned and
// encoded here, and its readings are those of its metadata (an uncompressed photo has no file to read them from).
- (void)takeTurnedPhoto:(AVCapturePhoto*)photo
{
#if TARGET_OS_IOS
    const auto readings = uml::platform::photoReadingsOf((__bridge CFDictionaryRef)photo.metadata);
#else
    const uml::platform::PhotoReadings readings;
#endif
    if (readings.orientation && *readings.orientation >= 1 && *readings.orientation <= 8) _exifOrientation = uint8_t(*readings.orientation);
    // The same bound as a photo's own file (takeJpegFile), kept by the encoder: a file larger than the guest's buffer is
    // not copied out of it, and none of the host's copies of it is made.
    auto turned = uml::platform::turnedStillOf(photo.pixelBuffer, _jpegQuality, _maximumBytes, _captureIsSrgb);
    if (turned.failure != uml::platform::TurnedStillFailure::none) {
        _turnFailure = turned.failure;
        std::fprintf(stderr, "Camera: camera %zu: the photo could not be turned half around: %s\n", _camera,
                     std::string(uml::platform::turnedStillFailureName(turned.failure)).c_str());
        noteStillFailure(_result.failure, uml::platform::CameraStillStage::missingData);
        return;
    }
    _result.jpeg = std::move(turned.jpeg);
    _jpegReadings = readings;
}

- (void)captureOutput:(AVCapturePhotoOutput*)output
    didFinishCaptureForResolvedSettings:(AVCaptureResolvedPhotoSettings*)resolvedSettings
                                  error:(NSError*)error
{
    noteCompensation(_result.failure, output);
    // The capture's own error: the photo callback's, when it had one, is the one kept.
    noteFrameworkError(_result.failure, error);
    // The session drops its hold on this object when it delivers, which can be
    // the last one.
    __strong UMLPhotoDelegate* keepAlive = self;
    (void)keepAlive;
    if (_wantJpeg && resolvedSettings != nil) {
        const CMVideoDimensions resolved = resolvedSettings.photoDimensions;
        _result.failure.resolvedWidth = std::uint32_t(std::max(0, int(resolved.width)));
        _result.failure.resolvedHeight = std::uint32_t(std::max(0, int(resolved.height)));
    }
    // The photo is the size the guest configured or it is not passed on: the
    // output makes at most the size it was asked for, and the platform says
    // what it resolved. (The device compares the encoded picture as well.)
    if (_wantJpeg && resolvedSettings != nil && !_result.jpeg.empty()) {
        const CMVideoDimensions resolved = resolvedSettings.photoDimensions;
        const uml::platform::CameraStillSize resolvedSize {std::uint32_t(std::max(0, int(resolved.width))),
                                                           std::uint32_t(std::max(0, int(resolved.height)))};
        const auto outcome = uml::platform::stillSizeOutcome(resolvedSize, _jpegSize, _attempt);
        if (outcome != uml::platform::StillSizeOutcome::deliver) {
            std::fprintf(stderr, "Camera: the photo resolved to %ux%u, not the %ux%u that was asked for (capture %u of %u)\n",
                         resolvedSize.width, resolvedSize.height, _jpegSize.width, _jpegSize.height, _attempt,
                         uml::platform::stillAttemptLimit);
            _result.jpeg.clear();
            _result.failure.stage = uml::platform::CameraStillStage::resolvedSize;
            if (outcome == uml::platform::StillSizeOutcome::retry) {
                // The photo of the first capture is dropped and the request is made again; the result is the
                // next capture's.
                std::lock_guard lock(_state->mutex);
                if (_state->session && static_cast<uml::platform::AppleSession*>(_state->session)->remakeStill(_camera, _token)) return;
            }
        }
    }
    // A JPEG that was asked for and did not arrive, for a reason no callback gave, is missing data.
    if (_wantJpeg && _result.jpeg.empty()) noteStillFailure(_result.failure, uml::platform::CameraStillStage::missingData);
    // Whatever arrived stands; the capture failed only when nothing asked for did.
    _result.ok = (_wantJpeg && !_result.jpeg.empty()) || (_wantRaw && !_result.raw.empty());
    // Whether the flash fired is the metadata's word, and the platform's decision
    // for the capture when the metadata has none.
    std::optional<bool> flashResolved;
#if TARGET_OS_IOS
    if (resolvedSettings != nil) flashResolved = bool(resolvedSettings.isFlashEnabled);
#else
    (void)resolvedSettings;
#endif
    std::lock_guard lock(_state->mutex);
    if (!_state->session) return;
    auto* session = static_cast<uml::platform::AppleSession*>(_state->session);
    _result.failure.hostExifOrientation = _exifOrientation;
    if (_wantJpeg && _exifOrientation != 0) session->noteStillExif(_camera, _exifOrientation);
    if (_turnFailure != uml::platform::TurnedStillFailure::none) session->noteStillNotTurned(_camera, _turnFailure);
    const bool haveJpegReadings = _jpegReadings.exposureSeconds || _jpegReadings.iso || _jpegReadings.fNumber ||
        _jpegReadings.focalLengthMillimetres || _jpegReadings.flash;
    // The photo's own readings come from its Exif. The 3A states, white balance,
    // zoom and lens are read from the session now, at delivery: they are what it
    // reports then, not what they were at the exposure, and on macOS the session
    // reports none of them, so they keep their defaults.
    _result.exposure = uml::platform::stillExposureOf(
        session->deviceExposureOf(_camera), haveJpegReadings ? _jpegReadings : _rawReadings, flashResolved, _dng);
    session->deliverStill(_camera, std::move(_result));
}

@end

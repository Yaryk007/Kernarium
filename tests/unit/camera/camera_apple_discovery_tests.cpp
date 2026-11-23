// SPDX-License-Identifier: GPL-2.0-or-later
#include "calibration_store.hpp"
#include "camera.hpp"
#include "camera_authorization.hpp"
#include "camera_description.hpp"
#include "frame_bytes.hpp"
#include "require.hpp"

#include <set>
#include <string>

using namespace uml::platform;
using uml::testing::require;

namespace {

/* The zoom factors AVFoundation states on a multi-lens phone, run through the rule that discovery applies: a lens
 * whose display multiplier is not 1 (a telephoto's 5, an ultra wide's 0.5) is not scaled by it, and a zoom camera is. */
void zoomRangesHoldOne()
{
    struct Device {
        CameraLensKind lens;
        double displayMultiplier;
        double minimumFactor, maximumFactor;
    };
    const Device devices[] = {
        {CameraLensKind::zoom, 0.5, 1.0, 123.75},       // the triple camera: factor 1 is its ultra wide lens
        {CameraLensKind::zoom, 1.0, 1.0, 123.75},       // a dual camera
        {CameraLensKind::wide, 1.0, 1.0, 123.75},
        {CameraLensKind::ultraWide, 0.5, 1.0, 123.75},
        {CameraLensKind::telephoto, 5.0, 1.0, 123.75},
        {CameraLensKind::telephoto, 4.0, 1.0, 123.75},
        {CameraLensKind::external, 1.0, 1.0, 4.0},
    };
    for (const auto& device : devices) {
        const float scale = zoomRatioScale(device.lens, device.displayMultiplier);
        const float minimum = float(device.minimumFactor) * scale;
        const float maximum = float(device.maximumFactor) * scale;
        require(minimum <= 1.0f && maximum >= 1.0f, "every camera's zoom range holds 1.0");
        // A single lens states its own factors: 1.0 is its own full field of view.
        if (device.lens != CameraLensKind::zoom)
            require(scale == 1.0f && minimum == float(device.minimumFactor), "a single lens is not scaled by the display multiplier");
    }
    // A zoom camera is scaled, which puts its ultra wide lens at 0.5; a multiplier the host does not state is 1.
    require(zoomRatioScale(CameraLensKind::zoom, 0.5) == 0.5f, "the zoom camera's display multiplier");
    require(zoomRatioScale(CameraLensKind::zoom, 0.0) == 1.0f && zoomRatioScale(CameraLensKind::zoom, -1.0) == 1.0f,
            "an unstated multiplier is 1");
    for (const auto lens : {CameraLensKind::wide, CameraLensKind::ultraWide, CameraLensKind::telephoto, CameraLensKind::external})
        require(zoomRatioScale(lens, 0.5) == 1.0f && zoomRatioScale(lens, 5.0) == 1.0f && zoomRatioScale(lens, 0.0) == 1.0f,
                "no lens that is not a zoom camera has a multiplier");
}

} // namespace

/* Discovery only. The test reads what AVFoundation lists and never opens a
 * session, never asks for access and never captures, so it runs on a host
 * with any cameras, or with none, and cannot light a capture indicator. */
int main()
{
    zoomRangesHoldOne();
    const auto permissionBefore = makeAppleCameraAuthorization()->permission();
    const auto camera = makeAppleCamera();
    require(camera != nullptr);
    const auto cameras = camera->cameras();
    require(!camera->hostModel().empty());
    // The system's camera states the working storage its photo pipeline holds, which the reserve counts.
    require(camera->stillWorkingBytesPerPixel() == systemCameraStillWorkingBytesPerPixel, "the system camera's working storage");
    require(camera->turnedStillWorkingBytesPerPixel() == systemCameraTurnedStillWorkingBytesPerPixel,
            "and the encoder's, for a still it turns half around");
    std::set<std::string> identifiers;
    for (const auto& info : cameras) {
        require(!info.identifier.empty() && identifiers.insert(info.identifier).second);
        require(!info.videoFormats.empty());
        for (const auto& format : info.videoFormats) {
            require(format.width > 0 && format.height > 0);
            require(format.minimumFrameRate >= 1 && format.maximumFrameRate >= format.minimumFrameRate);
        }
        require(info.zoomMinimum > 0 && info.zoomMaximum >= info.zoomMinimum);
        // Android requires every camera's zoom range to hold 1.0: a single lens's 1.0 is its own full field of view.
        require(info.zoomMinimum <= 1.0f && info.zoomMaximum >= 1.0f, "a discovered camera's zoom range holds 1.0");
        if (info.lens != CameraLensKind::zoom) require(info.zoomMinimum == 1.0f, "a single lens's widest view is its ratio 1.0");
        // The system's zoom advice for a format is a ratio that holds 1.0, or nothing.
        for (const auto& format : info.captureFormats) {
            require(format.recommendedZoomMaximum == 0.0f || format.recommendedZoomMaximum >= 1.0f,
                    "a format's recommended zoom is none, or at least 1.0");
            require(format.maximumZoom == 0.0f || format.maximumZoom >= 1.0f, "a format's own maximum zoom is none, or at least 1.0");
        }
        require(info.pixelArrayWidth > 0 && info.pixelArrayHeight > 0);
        require(info.sensorOrientation % 90 == 0 && info.sensorOrientation < 360);
        // A zoom camera takes no manual control, and names lenses the host lists.
        if (info.lens == CameraLensKind::zoom) {
            require(!info.manualExposure && !info.manualFocus && !info.manualWhiteBalance && !info.rawStills);
            require(!info.constituents.empty());
        }
        // A manual control needs its range.
        if (info.manualExposure) require(info.isoMaximum >= info.isoMinimum && info.exposureMaximumNanoseconds > 0);
        if (info.rawStills) require(info.rawStillSize.width > 0);
    }
    // The order is stable, and so are the sets, which index into it.
    const auto again = camera->cameras();
    require(again.size() == cameras.size());
    for (std::size_t index = 0; index < cameras.size(); ++index) require(again[index].identifier == cameras[index].identifier);
    for (const auto& set : camera->concurrentSets()) {
        require(set.size() > 1);
        for (const auto member : set) require(member < cameras.size());
    }
    // What the guest is offered of the host's cameras: no zoom camera, whatever the host lists, and every camera a single
    // lens whose range starts at its own full field of view and ends at the zoom the system recommends for the format
    // it runs in, where it gives one. The primary back camera is the wide lens when the host has one.
    const auto published = uml::camera::arrangeCameras(cameras, camera->concurrentSets(), camera->hostModel(),
                                                       uml::camera::CalibrationStore(), {});
    bool hostHasBackWide = false;
    for (const auto& info : cameras) hostHasBackWide |= info.position == CameraPosition::back && info.lens == CameraLensKind::wide;
    for (const auto& described : published) {
        require(described.info.lens != CameraLensKind::zoom && described.wire.lens != VIRTIO_VIDEO_CAMERA_LENS_ZOOM,
                "the host publishes no zoom camera");
        require(described.wire.zoom_min_q16 == 65536 && described.wire.zoom_max_q16 >= 65536,
                "a published camera's zoom range is [1, maximum]");
        if (described.anchor) {
            const float advised = described.info.captureFormats[*described.anchor].recommendedZoomMaximum;
            if (advised > 0) require(described.info.zoomMaximum == advised, "a camera zooms up to the system's recommendation");
            else require(described.info.zoomMaximum <= cameras[described.platformIndex].zoomMaximum,
                         "a camera with no recommendation zooms no further than the device does");
        }
    }
    if (hostHasBackWide && !published.empty() && published.front().info.position == CameraPosition::back)
        require(published.front().info.lens == CameraLensKind::wide, "the primary back camera is the wide lens");
    // Discovery changed nothing about the permission.
    require(makeAppleCameraAuthorization()->permission() == permissionBefore);
    return 0;
}

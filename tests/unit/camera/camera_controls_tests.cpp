// SPDX-License-Identifier: GPL-2.0-or-later
// The V4L2 controls a camera publishes and how a client's values become the
// platform camera's settings: which controls each kind of camera has, what a
// value may be, how the aliases of one setting stay in step, and what the
// platform is asked to do.

#include "camera_controls.hpp"
#include "require.hpp"
#include "synthetic_camera.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>

using namespace uml::camera;
using namespace uml::platform;
using uml::testing::require;

namespace {

constexpr std::int64_t q16 = 65536;

CameraInfo infoOf(SyntheticCameraProfile profile, std::size_t index)
{
    SyntheticCameraOptions options;
    options.profile = profile;
    return makeSyntheticCamera(options)->cameras().at(index);
}

bool has(const CameraControls& controls, std::uint32_t id) { return controls.find(id) != nullptr; }

std::vector<std::uint8_t> region(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height, std::uint32_t weight)
{
    const std::uint32_t words[5] = {x, y, width, height, weight};
    std::vector<std::uint8_t> bytes(sizeof(words));
    std::memcpy(bytes.data(), words, sizeof(words));
    return bytes;
}

void set(const CameraControls& controls, ControlValues& values, std::uint32_t id, std::int64_t value,
         std::source_location where = std::source_location::current())
{
    require(controls.check(id, value, 0) == ControlCheck::ok, "a value the control should take was refused", where);
    controls.store(values, id, value);
}

void testWhatEachCameraHas()
{
    // A host's zoom camera (a virtual device the device does not publish) takes no manual control and zooms below 1.0.
    auto zoomInfo = infoOf(SyntheticCameraProfile::phone, 0);
    zoomInfo.lens = CameraLensKind::zoom;
    zoomInfo.manualExposure = zoomInfo.manualFocus = zoomInfo.manualWhiteBalance = false;
    zoomInfo.zoomMinimum = 0.5f;
    zoomInfo.zoomMaximum = 15.0f;
    const CameraControls zoom(zoomInfo, 90, false);
    const auto wideInfo = infoOf(SyntheticCameraProfile::phone, 0);
    const CameraControls wide(wideInfo, 90, false);
    const auto frontInfo = infoOf(SyntheticCameraProfile::phone, 3);
    const CameraControls front(frontInfo, 270, true);

    // The zoom camera takes automatic exposure, focus and white balance only.
    require(has(zoom, UML_V4L2_CID_EXPOSURE_AUTO) && has(zoom, UML_V4L2_CID_FOCUS_AUTO) && has(zoom, UML_V4L2_CID_AUTO_WHITE_BALANCE),
            "the zoom camera has the automatic controls");
    require(!has(zoom, UML_V4L2_CID_EXPOSURE_ABSOLUTE) && !has(zoom, VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS) &&
                !has(zoom, VIRTIO_VIDEO_CAMERA_CID_ISO) && !has(zoom, UML_V4L2_CID_FOCUS_ABSOLUTE) &&
                !has(zoom, VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_RED) && !has(zoom, UML_V4L2_CID_WHITE_BALANCE_TEMPERATURE),
            "the zoom camera has no manual control");
    require(zoom.check(UML_V4L2_CID_EXPOSURE_AUTO, UML_V4L2_EXPOSURE_MANUAL, 0) == ControlCheck::invalid,
            "manual exposure is not a menu entry of the zoom camera");
    require(zoom.check(UML_V4L2_CID_EXPOSURE_AUTO, UML_V4L2_EXPOSURE_AUTO, 0) == ControlCheck::ok, "automatic is");
    require(has(zoom, VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16) && has(zoom, UML_V4L2_CID_ZOOM_ABSOLUTE), "the zoom camera zooms");
    require(zoom.find(VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16)->minimum == std::uint64_t(q16 / 2) &&
                zoom.find(VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16)->maximum == std::uint64_t(15 * q16),
            "its ratio runs from 0.5 to 15");

    // A lens has them by hand.
    for (const auto id : {UML_V4L2_CID_EXPOSURE_ABSOLUTE, VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, UML_V4L2_CID_ISO_SENSITIVITY,
                          VIRTIO_VIDEO_CAMERA_CID_ISO, UML_V4L2_CID_FOCUS_ABSOLUTE, UML_V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE,
                          UML_V4L2_CID_WHITE_BALANCE_TEMPERATURE, VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_RED,
                          VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_GREEN, VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_BLUE,
                          UML_V4L2_CID_FLASH_LED_MODE, VIRTIO_VIDEO_CAMERA_CID_STILL_FLASH, VIRTIO_VIDEO_CAMERA_CID_AF_REGION})
        require(has(wide, id), "the wide lens lacks a control");
    require(wide.check(UML_V4L2_CID_EXPOSURE_AUTO, UML_V4L2_EXPOSURE_MANUAL, 0) == ControlCheck::ok, "manual exposure is open");
    require(wide.check(UML_V4L2_CID_EXPOSURE_AUTO, 2, 0) == ControlCheck::invalid, "shutter priority is not");

    // The front camera has no focus and no flash; it states its facing and is blocked.
    require(!has(front, UML_V4L2_CID_FOCUS_AUTO) && !has(front, UML_V4L2_CID_AUTO_FOCUS_START) &&
                !has(front, UML_V4L2_CID_FOCUS_ABSOLUTE) && !has(front, VIRTIO_VIDEO_CAMERA_CID_AF_REGION),
            "the front camera does not focus");
    require(!has(front, UML_V4L2_CID_FLASH_LED_MODE) && !has(front, VIRTIO_VIDEO_CAMERA_CID_STILL_FLASH), "or flash");
    const auto defaults = front.defaults();
    require(defaults.scalars.at(UML_V4L2_CID_CAMERA_ORIENTATION) == UML_V4L2_CAMERA_ORIENTATION_FRONT, "the facing");
    require(defaults.scalars.at(UML_V4L2_CID_CAMERA_SENSOR_ROTATION) == 270, "the sensor rotation is the application's");
    require(defaults.scalars.at(UML_V4L2_CID_PRIVACY) == 1, "a blocked session states the privacy control");
    require(wide.defaults().scalars.at(UML_V4L2_CID_PRIVACY) == 0, "a live one does not");
    require(front.check(UML_V4L2_CID_PRIVACY, 0, 0) == ControlCheck::ok, "check ignores read-only (the device refuses it)");

    std::set<std::uint32_t> ids;
    for (const auto& descriptor : wide.descriptors()) require(ids.insert(descriptor.id).second, "a control is published twice");
    require(wide.find(UML_V4L2_CID_MPEG_VIDEO_BITRATE) == nullptr, "a control the camera lacks");
    require(wide.check(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 0, 0) == ControlCheck::unsupported, "is unsupported");
}

void testRanges()
{
    const auto info = infoOf(SyntheticCameraProfile::phone, 0);
    const CameraControls controls(info, 90, false);
    require(controls.check(UML_V4L2_CID_AUTO_EXPOSURE_BIAS, 3000, 0) == ControlCheck::ok &&
                controls.check(UML_V4L2_CID_AUTO_EXPOSURE_BIAS, 3001, 0) == ControlCheck::invalid &&
                controls.check(UML_V4L2_CID_AUTO_EXPOSURE_BIAS, -3001, 0) == ControlCheck::invalid,
            "the exposure bias runs to three stops either way");
    require(controls.check(UML_V4L2_CID_ISO_SENSITIVITY, 24, 0) == ControlCheck::invalid &&
                controls.check(UML_V4L2_CID_ISO_SENSITIVITY, 25, 0) == ControlCheck::ok &&
                controls.check(UML_V4L2_CID_ISO_SENSITIVITY, 3200, 0) == ControlCheck::ok &&
                controls.check(UML_V4L2_CID_ISO_SENSITIVITY, 3201, 0) == ControlCheck::invalid,
            "the ISO range is the camera's");
    require(controls.check(VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, 19'999, 0) == ControlCheck::invalid &&
                controls.check(VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, 20'000, 0) == ControlCheck::ok &&
                controls.check(VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, 1'000'000'001, 0) == ControlCheck::invalid,
            "the exposure range is the camera's");
    require(controls.check(UML_V4L2_CID_3A_LOCK, 7, 0) == ControlCheck::ok && controls.check(UML_V4L2_CID_3A_LOCK, 8, 0) == ControlCheck::invalid,
            "the lock is a bitmask of three");
    require(controls.check(UML_V4L2_CID_AUTO_WHITE_BALANCE, 2, 0) == ControlCheck::invalid, "a boolean is 0 or 1");
    require(controls.check(UML_V4L2_CID_FLASH_LED_MODE, 1, 0) == ControlCheck::invalid &&
                controls.check(UML_V4L2_CID_FLASH_LED_MODE, 2, 0) == ControlCheck::ok,
            "the flash is for stills; the LED mode is a torch");
    require(controls.check(VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP, 4, 0) == ControlCheck::ok &&
                controls.check(VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP, 5, 0) == ControlCheck::invalid,
            "rotation is none, three turns or automatic");
    require(controls.check(VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_RED, q16 - 1, 0) == ControlCheck::invalid &&
                controls.check(VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_RED, 4 * q16, 0) == ControlCheck::ok &&
                controls.check(VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_RED, 4 * q16 + 1, 0) == ControlCheck::invalid,
            "the gains run from unity to the camera's maximum");
    require(controls.check(VIRTIO_VIDEO_CAMERA_CID_AE_REGION, 0, regionPayloadSize) == ControlCheck::ok &&
                controls.check(VIRTIO_VIDEO_CAMERA_CID_AE_REGION, 0, regionPayloadSize - 4) == ControlCheck::invalid &&
                controls.check(VIRTIO_VIDEO_CAMERA_CID_AE_REGION, 0, 0) == ControlCheck::invalid,
            "a region is five words");
    // The focus distance runs to the nearest the lens focuses at: 120 mm.
    const auto nearest = controls.find(UML_V4L2_CID_FOCUS_ABSOLUTE)->maximum;
    require(nearest == 8333, "120 mm is 8.333 dioptres");
}

void testAliases()
{
    const auto info = infoOf(SyntheticCameraProfile::phone, 0);
    const CameraControls controls(info, 90, false);
    auto values = controls.defaults();

    set(controls, values, UML_V4L2_CID_EXPOSURE_ABSOLUTE, 1000);  // 100 ms
    require(values.scalars.at(VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS) == 100'000'000, "V4L2 exposure sets the nanoseconds");
    set(controls, values, VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, 8'333'333);
    require(values.scalars.at(UML_V4L2_CID_EXPOSURE_ABSOLUTE) == 83, "nanoseconds set the V4L2 exposure, rounded");
    set(controls, values, UML_V4L2_CID_ISO_SENSITIVITY, 400);
    require(values.scalars.at(VIRTIO_VIDEO_CAMERA_CID_ISO) == 400, "the ISO pair");
    set(controls, values, VIRTIO_VIDEO_CAMERA_CID_ISO, 800);
    require(values.scalars.at(UML_V4L2_CID_ISO_SENSITIVITY) == 800, "in both directions");
    set(controls, values, UML_V4L2_CID_ZOOM_ABSOLUTE, 250);
    require(values.scalars.at(VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16) == 5 * q16 / 2, "V4L2 zoom sets the ratio");
    set(controls, values, VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, 3 * q16);
    require(values.scalars.at(UML_V4L2_CID_ZOOM_ABSOLUTE) == 300, "and back");

    // White balance by switch, preset, temperature and gains.
    set(controls, values, UML_V4L2_CID_AUTO_WHITE_BALANCE, 0);
    require(values.scalars.at(UML_V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE) == UML_V4L2_WHITE_BALANCE_MANUAL,
            "leaving automatic white balance makes the preset manual");
    set(controls, values, UML_V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE, UML_V4L2_WHITE_BALANCE_DAYLIGHT);
    require(values.scalars.at(UML_V4L2_CID_AUTO_WHITE_BALANCE) == 0 &&
                values.scalars.at(UML_V4L2_CID_WHITE_BALANCE_TEMPERATURE) == 5500,
            "a preset states its temperature");
    require(values.scalars.at(VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_GREEN) == q16 &&
                values.scalars.at(VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_RED) > q16 &&
                values.scalars.at(VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_BLUE) > q16,
            "and its gains, green being the reference");
    set(controls, values, UML_V4L2_CID_WHITE_BALANCE_TEMPERATURE, 2850);
    require(values.scalars.at(UML_V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE) == UML_V4L2_WHITE_BALANCE_MANUAL &&
                values.scalars.at(UML_V4L2_CID_AUTO_WHITE_BALANCE) == 0,
            "a temperature is manual");
    require(std::llround(2.6 * q16) == values.scalars.at(VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_BLUE), "with the incandescent blue gain");
    set(controls, values, UML_V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE, UML_V4L2_WHITE_BALANCE_AUTO);
    require(values.scalars.at(UML_V4L2_CID_AUTO_WHITE_BALANCE) == 1, "the automatic preset is automatic white balance");

    // The focus button counts its presses while FOCUS_AUTO is off; with it on a
    // driver ignores the request. A written distance is counted too.
    for (int press = 0; press < 2; ++press) controls.store(values, UML_V4L2_CID_AUTO_FOCUS_START, 1);
    require(values.scalars.at(UML_V4L2_CID_AUTO_FOCUS_START) == 0, "presses with automatic focus on are ignored");
    set(controls, values, UML_V4L2_CID_FOCUS_AUTO, 0);
    for (int press = 0; press < 3; ++press) controls.store(values, UML_V4L2_CID_AUTO_FOCUS_START, 1);
    require(values.scalars.at(UML_V4L2_CID_AUTO_FOCUS_START) == 3, "presses are counted");
    require(controls.read(values, UML_V4L2_CID_AUTO_FOCUS_START, nullptr) == 0, "a button reads zero");
    auto settings = controls.settings(values);
    require(settings.focusTriggerSerial == 3, "the platform sees the serial");
    require(values.focusDistanceWrites == 0 && settings.focusDistanceSerial == 0, "no distance was written");
    set(controls, values, UML_V4L2_CID_FOCUS_ABSOLUTE, 1000);
    set(controls, values, UML_V4L2_CID_FOCUS_ABSOLUTE, 1000);
    require(values.focusDistanceWrites == 2 && controls.settings(values).focusDistanceSerial == 2,
            "each write of the distance counts, even of the same value");
    require(controls.settings(values).focusTriggerSerial == 3, "and a write is not a press");
    set(controls, values, UML_V4L2_CID_FOCUS_AUTO, 1);

    // A compound control keeps what the guest sent.
    const auto bytes = region(1000, 2000, 3000, 4000, 5);
    controls.store(values, VIRTIO_VIDEO_CAMERA_CID_AE_REGION, 0, bytes);
    require(values.payloads.at(VIRTIO_VIDEO_CAMERA_CID_AE_REGION) == bytes, "the region payload");
}

void testReadsAndSettings()
{
    const auto info = infoOf(SyntheticCameraProfile::phone, 0);
    const CameraControls controls(info, 90, false);
    auto values = controls.defaults();
    auto settings = controls.settings(values);
    require(settings == CameraSettings {} || (settings.exposureMode == CameraExposureMode::continuousAuto &&
                                               settings.focusMode == CameraFocusMode::continuousAuto &&
                                               settings.whiteBalanceMode == CameraWhiteBalanceMode::continuousAuto &&
                                               settings.zoomRatio == 1.0f && !settings.torch && !settings.aeRegion),
            "the defaults are everything automatic");

    // Manual exposure carries the time and ISO.
    set(controls, values, UML_V4L2_CID_EXPOSURE_AUTO, UML_V4L2_EXPOSURE_MANUAL);
    set(controls, values, VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, 4'000'000);
    set(controls, values, VIRTIO_VIDEO_CAMERA_CID_ISO, 640);
    settings = controls.settings(values);
    require(settings.exposureMode == CameraExposureMode::manual && settings.exposureNanoseconds == 4'000'000 && settings.iso == 640,
            "manual exposure");
    set(controls, values, UML_V4L2_CID_EXPOSURE_AUTO, UML_V4L2_EXPOSURE_AUTO);
    set(controls, values, UML_V4L2_CID_3A_LOCK, UML_V4L2_LOCK_EXPOSURE | UML_V4L2_LOCK_FOCUS | UML_V4L2_LOCK_WHITE_BALANCE);
    settings = controls.settings(values);
    require(settings.exposureMode == CameraExposureMode::locked && settings.focusMode == CameraFocusMode::locked &&
                settings.whiteBalanceMode == CameraWhiteBalanceMode::locked,
            "the 3A lock locks all three");
    set(controls, values, UML_V4L2_CID_3A_LOCK, 0);

    // Focus.
    set(controls, values, UML_V4L2_CID_FOCUS_AUTO, 0);
    set(controls, values, UML_V4L2_CID_FOCUS_ABSOLUTE, 2500);
    settings = controls.settings(values);
    require(settings.focusMode == CameraFocusMode::manual && std::fabs(settings.focusDistanceDioptres - 2.5f) < 1e-4f,
            "manual focus in thousandths of a dioptre");
    set(controls, values, UML_V4L2_CID_FOCUS_AUTO, 1);
    set(controls, values, UML_V4L2_CID_AUTO_EXPOSURE_BIAS, -1500);
    require(std::fabs(controls.settings(values).exposureBiasStops + 1.5f) < 1e-4f, "bias in thousandths of a stop");

    // White balance gains.
    set(controls, values, UML_V4L2_CID_AUTO_WHITE_BALANCE, 0);
    set(controls, values, VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_RED, 3 * q16 / 2);
    set(controls, values, VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_BLUE, 2 * q16);
    settings = controls.settings(values);
    require(settings.whiteBalanceMode == CameraWhiteBalanceMode::manual && std::fabs(settings.whiteBalanceGains[0] - 1.5f) < 1e-4f &&
                std::fabs(settings.whiteBalanceGains[2] - 2.0f) < 1e-4f,
            "manual white balance carries the gains");

    // Zoom is clamped to what the camera does.
    set(controls, values, VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, 6 * q16);
    require(std::fabs(controls.settings(values).zoomRatio - 6.0f) < 1e-4f, "zoom");

    // Torch, regions, stabilization.
    set(controls, values, UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_FLASH_LED_MODE_TORCH);
    require(controls.settings(values).torch, "torch");
    controls.store(values, VIRTIO_VIDEO_CAMERA_CID_AE_REGION, 0, region(16384, 32768, 16384, 8192, 3));
    controls.store(values, VIRTIO_VIDEO_CAMERA_CID_AF_REGION, 0, region(0, 0, 65536, 65536, 1));
    settings = controls.settings(values);
    require(settings.aeRegion && std::fabs(settings.aeRegion->x - 0.25f) < 1e-4f && std::fabs(settings.aeRegion->y - 0.5f) < 1e-4f &&
                std::fabs(settings.aeRegion->width - 0.25f) < 1e-4f && std::fabs(settings.aeRegion->height - 0.125f) < 1e-4f &&
                settings.aeRegion->weight == 3,
            "the metering region in fractions of the frame");
    require(settings.afRegion && settings.afRegion->weight == 1, "the focus region");
    // A camera whose pictures the device turns half around gets the guest's regions mapped back into the platform's
    // picture: the same region on the opposite side of the frame, the weight kept, the whole frame the whole frame.
    {
        auto turnedInfo = infoOf(SyntheticCameraProfile::phone, 3);
        turnedInfo.presentationTurn = 180;
        turnedInfo.autofocus = true;
        const CameraControls turned(turnedInfo, 270, false);
        auto turnedValues = turned.defaults();
        turned.store(turnedValues, VIRTIO_VIDEO_CAMERA_CID_AE_REGION, 0, region(16384, 32768, 16384, 8192, 3));
        turned.store(turnedValues, VIRTIO_VIDEO_CAMERA_CID_AF_REGION, 0, region(0, 0, 65536, 65536, 1));
        const auto mapped = turned.settings(turnedValues);
        require(mapped.aeRegion && std::fabs(mapped.aeRegion->x - 0.5f) < 1e-4f && std::fabs(mapped.aeRegion->y - 0.375f) < 1e-4f &&
                    std::fabs(mapped.aeRegion->width - 0.25f) < 1e-4f && std::fabs(mapped.aeRegion->height - 0.125f) < 1e-4f &&
                    mapped.aeRegion->weight == 3,
                "the metering region is where the half turn put it");
        require(mapped.afRegion && std::fabs(mapped.afRegion->x) < 1e-4f && std::fabs(mapped.afRegion->y) < 1e-4f &&
                    std::fabs(mapped.afRegion->width - 1.0f) < 1e-4f && mapped.afRegion->weight == 1,
                "the whole frame is the whole frame");
        // Any other turn the platform states is none (the device turns half around or not at all).
        turnedInfo.presentationTurn = 90;
        const CameraControls odd(turnedInfo, 270, false);
        auto oddValues = odd.defaults();
        odd.store(oddValues, VIRTIO_VIDEO_CAMERA_CID_AE_REGION, 0, region(16384, 32768, 16384, 8192, 3));
        require(odd.settings(oddValues).aeRegion && std::fabs(odd.settings(oddValues).aeRegion->x - 0.25f) < 1e-4f, "no other turn maps a region");
    }
    controls.store(values, VIRTIO_VIDEO_CAMERA_CID_AE_REGION, 0, region(0, 0, 0, 0, 0));
    require(!controls.settings(values).aeRegion, "a region of weight zero clears it");
    set(controls, values, UML_V4L2_CID_IMAGE_STABILIZATION, 0);
    require(!controls.settings(values).stabilization, "stabilization off");

    // What stills and the pipeline read.
    set(controls, values, VIRTIO_VIDEO_CAMERA_CID_STILL_FLASH, 2);
    require(controls.stillFlash(values) == CameraFlashMode::on, "flash on for stills");
    set(controls, values, VIRTIO_VIDEO_CAMERA_CID_STILL_FLASH, 1);
    require(controls.stillFlash(values) == CameraFlashMode::automatic, "automatic");
    set(controls, values, UML_V4L2_CID_JPEG_COMPRESSION_QUALITY, 70);
    require(controls.jpegQuality(values) == 70, "JPEG quality");
    require(!controls.testPattern(values), "no test pattern");
    set(controls, values, UML_V4L2_CID_TEST_PATTERN, 1);
    require(controls.testPattern(values), "the test pattern");
    require(controls.rotateAndCrop(values) == 0, "no rotation");
    set(controls, values, VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP, 3);
    require(controls.rotateAndCrop(values) == 3, "a turn of 270 degrees");

    // The volatile focus status follows the latest exposure.
    CameraExposure exposure;
    require(controls.read(values, UML_V4L2_CID_AUTO_FOCUS_STATUS, nullptr) == UML_V4L2_AUTO_FOCUS_STATUS_IDLE, "no exposure, idle");
    exposure.af = CameraControlState::searching;
    require(controls.read(values, UML_V4L2_CID_AUTO_FOCUS_STATUS, &exposure) == UML_V4L2_AUTO_FOCUS_STATUS_BUSY, "searching, busy");
    exposure.af = CameraControlState::converged;
    require(controls.read(values, UML_V4L2_CID_AUTO_FOCUS_STATUS, &exposure) == UML_V4L2_AUTO_FOCUS_STATUS_REACHED, "converged, reached");
    exposure.af = CameraControlState::failed;
    require(controls.read(values, UML_V4L2_CID_AUTO_FOCUS_STATUS, &exposure) == UML_V4L2_AUTO_FOCUS_STATUS_FAILED, "failed, failed");
}

void testTemperatureGains()
{
    const auto cold = gainsForTemperature(1000), warm = gainsForTemperature(2850);
    require(cold == warm, "below the first preset it is the first preset");
    const auto hot = gainsForTemperature(20000), shade = gainsForTemperature(7500);
    require(hot == shade, "and above the last, the last");
    const auto middle = gainsForTemperature(4750);  // halfway between fluorescent (4000) and daylight (5500) at 0.5
    require(middle[1] == 1.0f && middle[0] > gainsForTemperature(4000)[0] && middle[0] < gainsForTemperature(5500)[0],
            "between presets the red gain interpolates");
    require(gainsForTemperature(3000)[2] > gainsForTemperature(6000)[2], "a warm light needs more blue gain");
}

} // namespace

int main()
{
    testWhatEachCameraHas();
    testRanges();
    testAliases();
    testReadsAndSettings();
    testTemperatureGains();
    return 0;
}

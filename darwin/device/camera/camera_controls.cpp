// SPDX-License-Identifier: GPL-2.0-or-later
#include "camera_controls.hpp"

#include "photo_dimensions.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace uml::camera {
namespace {
using uml::platform::CameraControlState;
using uml::platform::CameraExposure;
using uml::platform::CameraExposureMode;
using uml::platform::CameraFlashMode;
using uml::platform::CameraFocusMode;
using uml::platform::CameraInfo;
using uml::platform::CameraPosition;
using uml::platform::CameraRegion;
using uml::platform::CameraSettings;
using uml::platform::CameraWhiteBalanceMode;
using uml::platform::regionBeforeTurn;

constexpr std::int64_t q16One = 65536;
constexpr std::int64_t exposureUnitNanoseconds = 100'000;  // V4L2 states exposure in 100 microseconds
constexpr std::int64_t defaultExposureNanoseconds = 8'333'333;
constexpr std::int64_t defaultIso = 100;

struct Preset {
    std::uint32_t menu;
    std::int64_t kelvin;
    std::array<float, 3> gains;
};
// Ordered by temperature.
constexpr std::array<Preset, 5> presets {{
    {UML_V4L2_WHITE_BALANCE_INCANDESCENT, 2850, {1.0f, 1.0f, 2.6f}},
    {UML_V4L2_WHITE_BALANCE_FLUORESCENT, 4000, {1.4f, 1.0f, 1.9f}},
    {UML_V4L2_WHITE_BALANCE_DAYLIGHT, 5500, {1.9f, 1.0f, 1.55f}},
    {UML_V4L2_WHITE_BALANCE_CLOUDY, 6500, {2.0f, 1.0f, 1.45f}},
    {UML_V4L2_WHITE_BALANCE_SHADE, 7500, {2.1f, 1.0f, 1.35f}},
}};

std::uint64_t u(std::int64_t value) noexcept { return static_cast<std::uint64_t>(value); }
std::int64_t signedValue(std::uint64_t value) noexcept { return static_cast<std::int64_t>(value); }

virtio_video_v4l2_control_desc descriptor(std::uint32_t id, std::uint32_t type, std::int64_t minimum, std::int64_t maximum,
                                          std::int64_t step, std::int64_t defaultValue, std::uint32_t flags = 0,
                                          std::uint64_t skip = 0, std::uint32_t elemSize = 0)
{
    virtio_video_v4l2_control_desc d {};
    d.id = id;
    d.type = type;
    d.minimum = u(minimum);
    d.maximum = u(maximum);
    d.step = u(step);
    d.default_value = u(defaultValue);
    d.menu_skip_mask = skip;
    d.flags = flags | VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC;
    d.elem_size = elemSize;
    return d;
}

float clampFloat(float value, float low, float high) noexcept { return std::min(std::max(value, low), high); }

CameraRegion regionOf(const std::vector<std::uint8_t>& payload)
{
    CameraRegion region;
    if (payload.size() != regionPayloadSize) return region;
    std::uint32_t words[5];
    std::memcpy(words, payload.data(), sizeof(words));
    region = {float(words[0]) / 65536.0f, float(words[1]) / 65536.0f, float(words[2]) / 65536.0f,
              float(words[3]) / 65536.0f, words[4]};
    return region;
}

} // namespace

std::array<float, 3> gainsForTemperature(std::int64_t kelvin) noexcept
{
    if (kelvin <= presets.front().kelvin) return presets.front().gains;
    for (std::size_t index = 1; index < presets.size(); ++index) {
        if (kelvin > presets[index].kelvin) continue;
        const auto& low = presets[index - 1];
        const auto& high = presets[index];
        const float t = float(kelvin - low.kelvin) / float(high.kelvin - low.kelvin);
        return {low.gains[0] + (high.gains[0] - low.gains[0]) * t, 1.0f, low.gains[2] + (high.gains[2] - low.gains[2]) * t};
    }
    return presets.back().gains;
}

CameraControls::CameraControls(const CameraInfo& info, std::uint32_t sensorOrientation, bool blocked)
    : info_(info)
{
    // Exposure.
    const bool manualExposure = info.manualExposure && info.exposureMaximumNanoseconds > 0;
    descriptors_.push_back(descriptor(UML_V4L2_CID_EXPOSURE_AUTO, UML_V4L2_CTRL_TYPE_MENU, 0, 3, 1, UML_V4L2_EXPOSURE_AUTO, 0,
                                      manualExposure ? 0b1100 : 0b1110));
    descriptors_.push_back(descriptor(UML_V4L2_CID_AUTO_EXPOSURE_BIAS, UML_V4L2_CTRL_TYPE_INTEGER, -3000, 3000, 1, 0));
    descriptors_.push_back(descriptor(UML_V4L2_CID_3A_LOCK, UML_V4L2_CTRL_TYPE_BITMASK, 0, 7, 0, 0));
    if (manualExposure) {
        const auto minNs = std::int64_t(std::max<std::uint64_t>(info.exposureMinimumNanoseconds, 1));
        const auto maxNs = std::int64_t(info.exposureMaximumNanoseconds);
        const auto defaultNs = std::clamp<std::int64_t>(defaultExposureNanoseconds, minNs, maxNs);
        descriptors_.push_back(descriptor(UML_V4L2_CID_EXPOSURE_ABSOLUTE, UML_V4L2_CTRL_TYPE_INTEGER,
                                          std::max<std::int64_t>(1, (minNs + exposureUnitNanoseconds - 1) / exposureUnitNanoseconds),
                                          std::max<std::int64_t>(1, maxNs / exposureUnitNanoseconds), 1,
                                          std::max<std::int64_t>(1, defaultNs / exposureUnitNanoseconds)));
        descriptors_.push_back(descriptor(VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, UML_V4L2_CTRL_TYPE_INTEGER64, minNs, maxNs, 1,
                                          defaultNs));
        descriptors_.push_back(descriptor(UML_V4L2_CID_ISO_SENSITIVITY_AUTO, UML_V4L2_CTRL_TYPE_MENU, 0, 1, 1,
                                          UML_V4L2_ISO_SENSITIVITY_AUTO));
        const auto isoMin = std::int64_t(std::max<std::uint32_t>(info.isoMinimum, 1));
        const auto isoMax = std::int64_t(std::max(info.isoMaximum, info.isoMinimum));
        const auto isoDefault = std::clamp<std::int64_t>(defaultIso, isoMin, isoMax);
        descriptors_.push_back(descriptor(UML_V4L2_CID_ISO_SENSITIVITY, UML_V4L2_CTRL_TYPE_INTEGER, isoMin, isoMax, 1, isoDefault));
        descriptors_.push_back(descriptor(VIRTIO_VIDEO_CAMERA_CID_ISO, UML_V4L2_CTRL_TYPE_INTEGER, isoMin, isoMax, 1, isoDefault));
    }
    // Focus.
    if (info.autofocus) {
        descriptors_.push_back(descriptor(UML_V4L2_CID_FOCUS_AUTO, UML_V4L2_CTRL_TYPE_BOOLEAN, 0, 1, 1, 1));
        descriptors_.push_back(descriptor(UML_V4L2_CID_AUTO_FOCUS_START, UML_V4L2_CTRL_TYPE_BUTTON, 0, 0, 0, 0));
        descriptors_.push_back(descriptor(UML_V4L2_CID_AUTO_FOCUS_STOP, UML_V4L2_CTRL_TYPE_BUTTON, 0, 0, 0, 0));
        descriptors_.push_back(descriptor(UML_V4L2_CID_AUTO_FOCUS_STATUS, UML_V4L2_CTRL_TYPE_BITMASK, 0, 7, 0, 0,
                                          VIRTIO_VIDEO_V4L2_CTRL_F_READ_ONLY | VIRTIO_VIDEO_V4L2_CTRL_F_VOLATILE));
        if (info.manualFocus) {
            // The focus distance in thousandths of a dioptre, 0 being infinity.
            const std::int64_t nearest = info.minimumFocusDistanceMillimetres > 0
                ? std::int64_t(std::lround(1'000'000.0 / info.minimumFocusDistanceMillimetres)) : 0;
            descriptors_.push_back(descriptor(UML_V4L2_CID_FOCUS_ABSOLUTE, UML_V4L2_CTRL_TYPE_INTEGER, 0, nearest, 1, 0));
        }
    }
    // White balance.
    descriptors_.push_back(descriptor(UML_V4L2_CID_AUTO_WHITE_BALANCE, UML_V4L2_CTRL_TYPE_BOOLEAN, 0, 1, 1, 1));
    if (info.manualWhiteBalance) {
        descriptors_.push_back(descriptor(UML_V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE, UML_V4L2_CTRL_TYPE_MENU, 0, 9, 1,
                                          UML_V4L2_WHITE_BALANCE_AUTO, 0, 0b0010110000));
        descriptors_.push_back(descriptor(UML_V4L2_CID_WHITE_BALANCE_TEMPERATURE, UML_V4L2_CTRL_TYPE_INTEGER, 2000, 10000, 1, 5500));
        const auto maxGain = std::int64_t(std::lround(std::max(info.maximumWhiteBalanceGain, 1.0f) * float(q16One)));
        for (const auto id : {VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_RED, VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_GREEN,
                              VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_BLUE})
            descriptors_.push_back(descriptor(id, UML_V4L2_CTRL_TYPE_INTEGER, q16One, maxGain, 1, q16One));
    }
    // Zoom.
    if (info.zoomMaximum > info.zoomMinimum) {
        descriptors_.push_back(descriptor(UML_V4L2_CID_ZOOM_ABSOLUTE, UML_V4L2_CTRL_TYPE_INTEGER,
                                          std::int64_t(std::floor(info.zoomMinimum * 100.0f)),
                                          std::int64_t(std::floor(info.zoomMaximum * 100.0f)), 1, 100));
        descriptors_.push_back(descriptor(VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, UML_V4L2_CTRL_TYPE_INTEGER,
                                          std::int64_t(std::floor(info.zoomMinimum * float(q16One))),
                                          std::int64_t(std::floor(info.zoomMaximum * float(q16One))), 1, q16One));
    }
    // Flash and torch.
    if (info.torch)
        descriptors_.push_back(descriptor(UML_V4L2_CID_FLASH_LED_MODE, UML_V4L2_CTRL_TYPE_MENU, 0, 2, 1, UML_V4L2_FLASH_LED_MODE_NONE,
                                          0, 0b010));
    if (info.flash)
        descriptors_.push_back(descriptor(VIRTIO_VIDEO_CAMERA_CID_STILL_FLASH, UML_V4L2_CTRL_TYPE_MENU, 0, 2, 1, 0));
    descriptors_.push_back(descriptor(UML_V4L2_CID_IMAGE_STABILIZATION, UML_V4L2_CTRL_TYPE_BOOLEAN, 0, 1, 1, 1));
    descriptors_.push_back(descriptor(VIRTIO_VIDEO_CAMERA_CID_AE_REGION, UML_V4L2_CTRL_TYPE_U32, 0, 0, 0, 0, 0, 0, regionPayloadSize));
    if (info.autofocus)
        descriptors_.push_back(descriptor(VIRTIO_VIDEO_CAMERA_CID_AF_REGION, UML_V4L2_CTRL_TYPE_U32, 0, 0, 0, 0, 0, 0, regionPayloadSize));
    descriptors_.push_back(descriptor(VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP, UML_V4L2_CTRL_TYPE_MENU, 0, 4, 1, 0));
    descriptors_.push_back(descriptor(UML_V4L2_CID_TEST_PATTERN, UML_V4L2_CTRL_TYPE_MENU, 0, 1, 1, 0));
    if (!info.stillSizes.empty())
        descriptors_.push_back(descriptor(UML_V4L2_CID_JPEG_COMPRESSION_QUALITY, UML_V4L2_CTRL_TYPE_INTEGER, 1, 100, 1, 95));
    // Read-only facts.
    const std::int64_t orientation = info.position == CameraPosition::front ? UML_V4L2_CAMERA_ORIENTATION_FRONT
        : info.position == CameraPosition::back ? UML_V4L2_CAMERA_ORIENTATION_BACK : UML_V4L2_CAMERA_ORIENTATION_EXTERNAL;
    descriptors_.push_back(descriptor(UML_V4L2_CID_CAMERA_ORIENTATION, UML_V4L2_CTRL_TYPE_MENU, 0, 2, 1, orientation,
                                      VIRTIO_VIDEO_V4L2_CTRL_F_READ_ONLY));
    descriptors_.push_back(descriptor(UML_V4L2_CID_CAMERA_SENSOR_ROTATION, UML_V4L2_CTRL_TYPE_INTEGER, 0, 270, 90,
                                      sensorOrientation, VIRTIO_VIDEO_V4L2_CTRL_F_READ_ONLY));
    descriptors_.push_back(descriptor(UML_V4L2_CID_PRIVACY, UML_V4L2_CTRL_TYPE_BOOLEAN, 0, 1, 1, blocked ? 1 : 0,
                                      VIRTIO_VIDEO_V4L2_CTRL_F_READ_ONLY));
}

const virtio_video_v4l2_control_desc* CameraControls::find(std::uint32_t id) const noexcept
{
    for (const auto& d : descriptors_)
        if (d.id == id) return &d;
    return nullptr;
}

ControlValues CameraControls::defaults() const
{
    ControlValues values;
    for (const auto& d : descriptors_) {
        if (d.elem_size != 0) values.payloads[d.id] = std::vector<std::uint8_t>(d.elem_size, 0);
        else values.scalars[d.id] = signedValue(d.default_value);
    }
    // The white balance a client starts from: auto, the daylight gains.
    if (find(VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_RED)) {
        const auto gains = gainsForTemperature(5500);
        values.scalars[VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_RED] = std::int64_t(std::lround(gains[0] * float(q16One)));
        values.scalars[VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_BLUE] = std::int64_t(std::lround(gains[2] * float(q16One)));
    }
    return values;
}

ControlCheck CameraControls::check(std::uint32_t id, std::int64_t value, std::size_t payloadSize) const noexcept
{
    const auto* d = find(id);
    if (!d) return ControlCheck::unsupported;
    const auto minimum = signedValue(d->minimum);
    const auto maximum = signedValue(d->maximum);
    const auto step = signedValue(d->step);
    switch (d->type) {
    case UML_V4L2_CTRL_TYPE_INTEGER:
    case UML_V4L2_CTRL_TYPE_INTEGER64:
        return value >= minimum && value <= maximum && (step <= 0 || (value - minimum) % step == 0) ? ControlCheck::ok
                                                                                                   : ControlCheck::invalid;
    case UML_V4L2_CTRL_TYPE_BOOLEAN:
        return value == 0 || value == 1 ? ControlCheck::ok : ControlCheck::invalid;
    case UML_V4L2_CTRL_TYPE_MENU:
        return value >= minimum && value <= maximum && value < 64 && !((d->menu_skip_mask >> value) & 1)
            ? ControlCheck::ok : ControlCheck::invalid;
    case UML_V4L2_CTRL_TYPE_BITMASK:
        return value >= 0 && (value & ~maximum) == 0 ? ControlCheck::ok : ControlCheck::invalid;
    case UML_V4L2_CTRL_TYPE_BUTTON:
        return ControlCheck::ok;
    default:
        return d->elem_size != 0 && payloadSize == d->elem_size ? ControlCheck::ok : ControlCheck::invalid;
    }
}

void CameraControls::store(ControlValues& values, std::uint32_t id, std::int64_t value, std::vector<std::uint8_t> payload) const
{
    const auto* d = find(id);
    if (!d) return;
    if (d->elem_size != 0) {
        values.payloads[id] = std::move(payload);
        return;
    }
    auto& scalars = values.scalars;
    const auto setIfPublished = [&](std::uint32_t other, std::int64_t v) {
        if (find(other)) scalars[other] = v;
    };
    const auto setGains = [&](const std::array<float, 3>& gains) {
        setIfPublished(VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_RED, std::int64_t(std::lround(gains[0] * float(q16One))));
        setIfPublished(VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_GREEN, std::int64_t(std::lround(gains[1] * float(q16One))));
        setIfPublished(VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_BLUE, std::int64_t(std::lround(gains[2] * float(q16One))));
    };
    if (id == UML_V4L2_CID_AUTO_FOCUS_START) {
        // A press asks for one scan, and only with FOCUS_AUTO off: with it on, V4L2
        // has the driver ignore the request.
        const auto automatic = scalars.find(UML_V4L2_CID_FOCUS_AUTO);
        if (automatic != scalars.end() && automatic->second == 0) ++scalars[id];
        return;
    }
    scalars[id] = value;
    switch (id) {
    case UML_V4L2_CID_FOCUS_ABSOLUTE: ++values.focusDistanceWrites; break;
    case UML_V4L2_CID_EXPOSURE_ABSOLUTE:
        setIfPublished(VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, std::clamp<std::int64_t>(
            value * exposureUnitNanoseconds, signedValue(find(VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS)->minimum),
            signedValue(find(VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS)->maximum)));
        break;
    case VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS:
        setIfPublished(UML_V4L2_CID_EXPOSURE_ABSOLUTE, std::clamp<std::int64_t>(
            (value + exposureUnitNanoseconds / 2) / exposureUnitNanoseconds,
            signedValue(find(UML_V4L2_CID_EXPOSURE_ABSOLUTE)->minimum), signedValue(find(UML_V4L2_CID_EXPOSURE_ABSOLUTE)->maximum)));
        break;
    case UML_V4L2_CID_ISO_SENSITIVITY: setIfPublished(VIRTIO_VIDEO_CAMERA_CID_ISO, value); break;
    case VIRTIO_VIDEO_CAMERA_CID_ISO: setIfPublished(UML_V4L2_CID_ISO_SENSITIVITY, value); break;
    case UML_V4L2_CID_ZOOM_ABSOLUTE:
        setIfPublished(VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, std::clamp<std::int64_t>(
            value * q16One / 100, signedValue(find(VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16)->minimum),
            signedValue(find(VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16)->maximum)));
        break;
    case VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16:
        setIfPublished(UML_V4L2_CID_ZOOM_ABSOLUTE, std::clamp<std::int64_t>(
            (value * 100 + q16One / 2) / q16One, signedValue(find(UML_V4L2_CID_ZOOM_ABSOLUTE)->minimum),
            signedValue(find(UML_V4L2_CID_ZOOM_ABSOLUTE)->maximum)));
        break;
    case UML_V4L2_CID_AUTO_WHITE_BALANCE:
        if (find(UML_V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE)) {
            const auto preset = scalars[UML_V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE];
            if (value == 1) scalars[UML_V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE] = UML_V4L2_WHITE_BALANCE_AUTO;
            else if (preset == UML_V4L2_WHITE_BALANCE_AUTO) scalars[UML_V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE] = UML_V4L2_WHITE_BALANCE_MANUAL;
        }
        break;
    case UML_V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE:
        scalars[UML_V4L2_CID_AUTO_WHITE_BALANCE] = value == UML_V4L2_WHITE_BALANCE_AUTO ? 1 : 0;
        for (const auto& preset : presets)
            if (preset.menu == value) {
                setGains(preset.gains);
                setIfPublished(UML_V4L2_CID_WHITE_BALANCE_TEMPERATURE, preset.kelvin);
            }
        break;
    case UML_V4L2_CID_WHITE_BALANCE_TEMPERATURE:
        scalars[UML_V4L2_CID_AUTO_WHITE_BALANCE] = 0;
        setIfPublished(UML_V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE, UML_V4L2_WHITE_BALANCE_MANUAL);
        setGains(gainsForTemperature(value));
        break;
    default: break;
    }
}

std::int64_t CameraControls::read(const ControlValues& values, std::uint32_t id, const CameraExposure* latest) const
{
    if (id == UML_V4L2_CID_AUTO_FOCUS_STATUS) {
        if (!latest) return UML_V4L2_AUTO_FOCUS_STATUS_IDLE;
        switch (latest->af) {
        case CameraControlState::searching: return UML_V4L2_AUTO_FOCUS_STATUS_BUSY;
        case CameraControlState::converged:
        case CameraControlState::locked: return UML_V4L2_AUTO_FOCUS_STATUS_REACHED;
        case CameraControlState::failed: return UML_V4L2_AUTO_FOCUS_STATUS_FAILED;
        default: return UML_V4L2_AUTO_FOCUS_STATUS_IDLE;
        }
    }
    if (id == UML_V4L2_CID_AUTO_FOCUS_START) return 0;
    const auto found = values.scalars.find(id);
    return found == values.scalars.end() ? 0 : found->second;
}

CameraSettings CameraControls::settings(const ControlValues& values) const
{
    const auto scalar = [&](std::uint32_t id, std::int64_t fallback) {
        const auto found = values.scalars.find(id);
        return found == values.scalars.end() ? fallback : found->second;
    };
    const auto lock = scalar(UML_V4L2_CID_3A_LOCK, 0);
    // The guest's regions are in the picture it is given, which the device turned by the camera's half turn: they are
    // mapped back into the platform's picture, which is what its points of interest are in.
    const std::uint32_t turn = info_.presentationTurn == 180 ? 180u : 0u;
    CameraSettings settings;
    // Exposure.
    if (scalar(UML_V4L2_CID_EXPOSURE_AUTO, 0) == UML_V4L2_EXPOSURE_MANUAL && info_.manualExposure) {
        settings.exposureMode = CameraExposureMode::manual;
        settings.exposureNanoseconds = std::uint64_t(scalar(VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, defaultExposureNanoseconds));
        settings.iso = std::uint32_t(scalar(VIRTIO_VIDEO_CAMERA_CID_ISO, defaultIso));
    } else if (lock & UML_V4L2_LOCK_EXPOSURE) {
        settings.exposureMode = CameraExposureMode::locked;
    }
    settings.exposureBiasStops = float(scalar(UML_V4L2_CID_AUTO_EXPOSURE_BIAS, 0)) / 1000.0f;
    // Focus. With FOCUS_AUTO off the lens holds; a press of the focus button runs
    // one scan, and a written FOCUS_ABSOLUTE moves it (the serials tell a new
    // press or write from an old one).
    if (info_.autofocus) {
        if (scalar(UML_V4L2_CID_FOCUS_AUTO, 1) == 0)
            settings.focusMode = info_.manualFocus ? CameraFocusMode::manual : CameraFocusMode::locked;
        else if (lock & UML_V4L2_LOCK_FOCUS)
            settings.focusMode = CameraFocusMode::locked;
        settings.focusDistanceDioptres = float(scalar(UML_V4L2_CID_FOCUS_ABSOLUTE, 0)) / 1000.0f;
        settings.focusTriggerSerial = std::uint32_t(scalar(UML_V4L2_CID_AUTO_FOCUS_START, 0));
        settings.focusDistanceSerial = values.focusDistanceWrites;
        const auto af = regionOf(values.payloads.contains(VIRTIO_VIDEO_CAMERA_CID_AF_REGION)
                                     ? values.payloads.at(VIRTIO_VIDEO_CAMERA_CID_AF_REGION) : std::vector<std::uint8_t> {});
        if (af.weight > 0) settings.afRegion = regionBeforeTurn(af, turn);
    }
    // White balance.
    if (scalar(UML_V4L2_CID_AUTO_WHITE_BALANCE, 1) == 0) {
        if (info_.manualWhiteBalance) {
            settings.whiteBalanceMode = CameraWhiteBalanceMode::manual;
            settings.whiteBalanceGains = {float(scalar(VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_RED, q16One)) / float(q16One),
                                          float(scalar(VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_GREEN, q16One)) / float(q16One),
                                          float(scalar(VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_BLUE, q16One)) / float(q16One)};
            const auto limit = std::max(info_.maximumWhiteBalanceGain, 1.0f);
            for (auto& gain : settings.whiteBalanceGains) gain = clampFloat(gain, 1.0f, limit);
        } else {
            settings.whiteBalanceMode = CameraWhiteBalanceMode::locked;
        }
    } else if (lock & UML_V4L2_LOCK_WHITE_BALANCE) {
        settings.whiteBalanceMode = CameraWhiteBalanceMode::locked;
    }
    // Zoom, torch, regions, stabilization.
    settings.zoomRatio = clampFloat(float(scalar(VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, q16One)) / float(q16One),
                                    info_.zoomMinimum, std::max(info_.zoomMaximum, info_.zoomMinimum));
    settings.torch = scalar(UML_V4L2_CID_FLASH_LED_MODE, 0) == UML_V4L2_FLASH_LED_MODE_TORCH;
    const auto ae = regionOf(values.payloads.contains(VIRTIO_VIDEO_CAMERA_CID_AE_REGION)
                                 ? values.payloads.at(VIRTIO_VIDEO_CAMERA_CID_AE_REGION) : std::vector<std::uint8_t> {});
    if (ae.weight > 0) settings.aeRegion = regionBeforeTurn(ae, turn);
    settings.stabilization = scalar(UML_V4L2_CID_IMAGE_STABILIZATION, 1) != 0;
    return settings;
}

CameraFlashMode CameraControls::stillFlash(const ControlValues& values) const
{
    const auto found = values.scalars.find(VIRTIO_VIDEO_CAMERA_CID_STILL_FLASH);
    if (found == values.scalars.end() || !info_.flash) return CameraFlashMode::off;
    return found->second == 2 ? CameraFlashMode::on : found->second == 1 ? CameraFlashMode::automatic : CameraFlashMode::off;
}

std::uint32_t CameraControls::jpegQuality(const ControlValues& values) const
{
    const auto found = values.scalars.find(UML_V4L2_CID_JPEG_COMPRESSION_QUALITY);
    return found == values.scalars.end() ? 95u : std::uint32_t(std::clamp<std::int64_t>(found->second, 1, 100));
}

bool CameraControls::testPattern(const ControlValues& values) const
{
    const auto found = values.scalars.find(UML_V4L2_CID_TEST_PATTERN);
    return found != values.scalars.end() && found->second != 0;
}

std::uint32_t CameraControls::rotateAndCrop(const ControlValues& values) const
{
    const auto found = values.scalars.find(VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP);
    return found == values.scalars.end() ? 0u : std::uint32_t(std::clamp<std::int64_t>(found->second, 0, 4));
}

} // namespace uml::camera

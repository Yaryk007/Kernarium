// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "camera.hpp"
#include "virtio_video.h"
#include "virtio_video_v4l2_ids.h"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace uml::camera {

/* The V4L2 controls a camera publishes, and how a client's values become the
 * platform camera's settings. It is a pure function of the camera's description
 * so that a test checks it without a device. The control table in
 * darwin/device/camera/README.md summarizes the contract. */

/* The value of every control a camera has set or defaults. A compound control
 * keeps the bytes the guest sent. The values belong to the camera, so every
 * stream of it reads and writes the same ones. */
struct ControlValues {
    std::map<std::uint32_t, std::int64_t> scalars;
    std::map<std::uint32_t, std::vector<std::uint8_t>> payloads;
    /* How many times the guest has written FOCUS_ABSOLUTE: the lens moves to
     * the written distance only after a write, and holds otherwise. */
    std::uint32_t focusDistanceWrites {};
};

enum class ControlCheck : std::uint8_t {
    ok,
    unsupported,  // not a control this camera publishes
    invalid,      // outside the published range, step or menu
};

/* The AE and AF regions are five u32: x, y, width, height in Q16 fractions of
 * the frame, and a weight. */
inline constexpr std::uint32_t regionPayloadSize = 20;

class CameraControls final {
public:
    /* `blocked` states the privacy control; `sensorOrientation` the read-only
     * rotation, which is the application's to state. */
    CameraControls(const uml::platform::CameraInfo& info, std::uint32_t sensorOrientation, bool blocked);

    [[nodiscard]] const std::vector<virtio_video_v4l2_control_desc>& descriptors() const noexcept
    {
        return descriptors_;
    }
    [[nodiscard]] const virtio_video_v4l2_control_desc* find(std::uint32_t id) const noexcept;
    /* Every published control at its default. */
    [[nodiscard]] ControlValues defaults() const;
    /* Whether `scalar` (or, for a compound control, a payload of `payloadSize`
     * bytes) is a value `id` accepts. */
    [[nodiscard]] ControlCheck check(std::uint32_t id, std::int64_t scalar, std::size_t payloadSize) const noexcept;
    /* Sets a control and keeps its aliases in step: the exposure time in 100
     * microseconds and in nanoseconds, the two ISO controls, the two zoom
     * controls, and white balance by switch, preset, temperature and gains. The
     * focus button counts a press while FOCUS_AUTO is off, as V4L2 says a driver
     * ignores it otherwise, and a write of FOCUS_ABSOLUTE is counted. Call it
     * only for a value `check` accepted. */
    void store(ControlValues& values, std::uint32_t id, std::int64_t scalar,
               std::vector<std::uint8_t> payload = {}) const;
    /* The value a get returns: the stored one, or for a volatile control what
     * the latest exposure says (`latest` may be null). */
    [[nodiscard]] std::int64_t read(const ControlValues& values, std::uint32_t id,
                                    const uml::platform::CameraExposure* latest) const;

    /* What the platform camera is asked to do. */
    [[nodiscard]] uml::platform::CameraSettings settings(const ControlValues& values) const;
    [[nodiscard]] uml::platform::CameraFlashMode stillFlash(const ControlValues& values) const;
    [[nodiscard]] std::uint32_t jpegQuality(const ControlValues& values) const;
    /* The test pattern is on: the camera's frames are black. */
    [[nodiscard]] bool testPattern(const ControlValues& values) const;
    /* VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP: 0 none, 1 to 3 a turn of 90, 180
     * or 270 degrees, 4 automatic. */
    [[nodiscard]] std::uint32_t rotateAndCrop(const ControlValues& values) const;

private:
    uml::platform::CameraInfo info_;
    std::vector<virtio_video_v4l2_control_desc> descriptors_;
};

/* The white balance gains (red, green, blue, relative to green) of a colour
 * temperature in kelvin, by interpolating between the presets. */
[[nodiscard]] std::array<float, 3> gainsForTemperature(std::int64_t kelvin) noexcept;

} // namespace uml::camera

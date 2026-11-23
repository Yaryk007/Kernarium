// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The colour calibration and optics a DNG file carries, read from its TIFF
// structure. A Bayer still is the only place the host reveals a sensor's colour
// matrices, so the camera learns them from the DNG that capture also produces.

#include "camera.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace uml::platform {

// Reads ColorMatrix1/2, ForwardMatrix1/2, CalibrationIlluminant1/2, BlackLevel
// and WhiteLevel from the first image directory (and the directories it
// links), and the lens focal length from the Exif directory. Returns nothing
// when `dng` is not a TIFF, or lacks a colour matrix; a missing optional tag
// leaves its field zero.
[[nodiscard]] std::optional<CameraColorData> parseDngColorData(std::span<const std::uint8_t> dng);

// What the same DNG says of its own exposure, which the colour data above does
// not: the white balance the platform measured (AsShotNeutral) and the noise
// model of the exposure (NoiseProfile). Each is absent when the file has no
// such tag or it does not hold usable numbers (a value that is not finite and
// positive); the caller then reports nothing rather than a nominal figure.
struct DngExposureData {
    // The camera's response to the scene's neutral, per colour channel: red,
    // green, blue.
    std::optional<std::array<float, 3>> neutralPoint;
    // (S, O) per colour channel in CFA order: red, green on red rows, green on
    // blue rows, blue. A profile of one or three pairs is spread to four.
    std::optional<std::array<float, 8>> noiseProfile;
};

[[nodiscard]] DngExposureData parseDngExposureData(std::span<const std::uint8_t> dng);

} // namespace uml::platform

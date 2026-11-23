// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// What a still's exposure was, as the platform's photo states it. A photo's own
// metadata knows the exposure time, the ISO sensitivity, the f-number, the
// focal length and whether the flash fired. What the session reports when the
// photo is delivered (the 3A states, white balance, zoom, which lens) is read
// at delivery, not at the exposure, and a platform that reports none of it
// leaves the defaults. A lens has one aperture and one focal length, so when the
// photo names none the lens's own is a true reading and is used. A reading the
// photo did not give, or gave as a number that cannot be a measurement, is zero
// in the result, which the camera device reads as "the photo gave none": no
// exposure takes zero seconds and no sensor has zero sensitivity.

#include "camera.hpp"
#include "dng_color.hpp"

#include <cstdint>
#include <optional>

namespace uml::platform {

// The numbers of a photo's metadata, as it states them. A key the metadata
// does not have is absent; a key it has is passed on as the number it is, zero
// and not-a-number included, for `stillExposureOf` to judge.
struct PhotoReadings {
    std::optional<double> exposureSeconds;
    std::optional<double> iso;
    std::optional<double> fNumber;
    std::optional<double> focalLengthMillimetres;
    // The Exif Flash field: bit 0 says the flash fired.
    std::optional<std::int64_t> flash;
    // The image's orientation tag (1 to 8, TIFF's numbering), where it has one.
    std::optional<std::int64_t> orientation;
};

// The exposure of a still. `device` is the session's state as it reports it when
// the photo is delivered: its 3A states, white balance, zoom, lens position and
// the lens itself are the still's as of the delivery, and so is the lens's fixed
// aperture where the photo gives no f-number; its exposure time and ISO are the
// live preview's and are not used.
// `flashResolved` is the platform's decision for the capture (the resolved
// photo settings), which decides when the metadata does not say. `dng` is what
// a Bayer still's DNG states of the exposure's neutral point and noise; absent
// for a JPEG.
[[nodiscard]] CameraExposure stillExposureOf(const CameraExposure& device,
                                             const PhotoReadings& readings,
                                             std::optional<bool> flashResolved,
                                             const DngExposureData& dng);

} // namespace uml::platform

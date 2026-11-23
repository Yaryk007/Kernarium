// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "still_exposure.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <cstdint>
#include <span>

namespace uml::platform {

// The exposure readings of a CGImageProperties dictionary, the shape of an
// image file's properties: the {Exif} dictionary's ExposureTime, ISOSpeedRatings (the first
// of them), FNumber, FocalLength and Flash, and the image's Orientation. A key that is missing, or holds
// something that is not a number, is absent; a number is passed on as it is.
// A null dictionary has none.
[[nodiscard]] PhotoReadings photoReadingsOf(CFDictionaryRef metadata);

// The same, of an encoded image (the JPEG or the DNG that a photo's file
// representation is): its Exif as ImageIO reads it. Bytes that are not an
// image have none. This is the photo's metadata on macOS as on iOS, where
// AVCapturePhoto.metadata does not exist on the Mac.
[[nodiscard]] PhotoReadings photoReadingsOfImage(std::span<const std::uint8_t> encoded);

} // namespace uml::platform

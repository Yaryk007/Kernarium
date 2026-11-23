// SPDX-License-Identifier: GPL-2.0-or-later
#include "photo_metadata.hpp"

#include <ImageIO/ImageIO.h>

namespace uml::platform {
namespace {

[[nodiscard]] std::optional<double> realOf(CFTypeRef value)
{
    if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) return std::nullopt;
    double number = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &number)) return std::nullopt;
    return number;
}

[[nodiscard]] std::optional<std::int64_t> integerOf(CFTypeRef value)
{
    if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) return std::nullopt;
    std::int64_t number = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &number)) return std::nullopt;
    return number;
}

} // namespace

PhotoReadings photoReadingsOf(CFDictionaryRef metadata)
{
    PhotoReadings readings;
    if (metadata == nullptr) return readings;
    // The orientation is a property of the image, not of its Exif dictionary.
    readings.orientation = integerOf(CFDictionaryGetValue(metadata, kCGImagePropertyOrientation));
    const auto* exifValue = CFDictionaryGetValue(metadata, kCGImagePropertyExifDictionary);
    if (exifValue == nullptr || CFGetTypeID(exifValue) != CFDictionaryGetTypeID()) return readings;
    const auto exif = static_cast<CFDictionaryRef>(exifValue);
    readings.exposureSeconds = realOf(CFDictionaryGetValue(exif, kCGImagePropertyExifExposureTime));
    readings.fNumber = realOf(CFDictionaryGetValue(exif, kCGImagePropertyExifFNumber));
    readings.focalLengthMillimetres = realOf(CFDictionaryGetValue(exif, kCGImagePropertyExifFocalLength));
    readings.flash = integerOf(CFDictionaryGetValue(exif, kCGImagePropertyExifFlash));
    // The sensitivity is an array of ratings; the first is the exposure's.
    const auto* isos = CFDictionaryGetValue(exif, kCGImagePropertyExifISOSpeedRatings);
    if (isos != nullptr && CFGetTypeID(isos) == CFArrayGetTypeID() && CFArrayGetCount(static_cast<CFArrayRef>(isos)) > 0)
        readings.iso = realOf(CFArrayGetValueAtIndex(static_cast<CFArrayRef>(isos), 0));
    return readings;
}

PhotoReadings photoReadingsOfImage(std::span<const std::uint8_t> encoded)
{
    if (encoded.empty()) return {};
    const CFDataRef data = CFDataCreateWithBytesNoCopy(nullptr, encoded.data(), CFIndex(encoded.size()), kCFAllocatorNull);
    if (data == nullptr) return {};
    PhotoReadings readings;
    if (const CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr)) {
        if (const CFDictionaryRef properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr)) {
            readings = photoReadingsOf(properties);
            CFRelease(properties);
        }
        CFRelease(source);
    }
    CFRelease(data);
    return readings;
}

} // namespace uml::platform

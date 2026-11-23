// SPDX-License-Identifier: GPL-2.0-or-later
#include "jpeg_gap.hpp"

#include <cstring>

namespace uml::camera {
namespace {

bool standalone(std::uint8_t marker) noexcept
{
    return marker == 0x00 || marker == 0x01 || marker == 0xd8 || marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7);
}

/* The end of the image whose first scan header starts at `at` (the 0xff of its
 * marker): the offset after the end of image that closes it. A scan's
 * entropy-coded data has 0xff only as 0xff00 or a restart marker; a
 * progressive image has more scans and tables before its end of image.
 * Nothing after it belongs to this image (a gain map or depth image follows
 * it in a multi-picture file). Empty for data that does not end. */
std::optional<std::size_t> imageEnd(std::span<const std::uint8_t> jpeg, std::size_t at)
{
    for (;;) {
        if (at + 2 > jpeg.size() || jpeg[at] != 0xff) return std::nullopt;
        std::size_t code = at + 1;
        while (code < jpeg.size() && jpeg[code] == 0xff) ++code;
        if (code >= jpeg.size()) return std::nullopt;
        const std::uint8_t marker = jpeg[code];
        if (marker == 0xd9) return code + 1;
        if (standalone(marker) || code + 3 > jpeg.size()) return std::nullopt;
        const std::size_t length = std::size_t(jpeg[code + 1]) << 8 | jpeg[code + 2];
        if (length < 2 || code + 1 + length > jpeg.size()) return std::nullopt;
        std::size_t next = code + 1 + length;
        if (marker == 0xda) {
            // Entropy-coded data runs to the next marker.
            for (;;) {
                const void* found = next < jpeg.size() ? std::memchr(jpeg.data() + next, 0xff, jpeg.size() - next) : nullptr;
                if (found == nullptr) return std::nullopt;
                next = std::size_t(static_cast<const std::uint8_t*>(found) - jpeg.data());
                if (next + 1 >= jpeg.size()) return std::nullopt;
                const std::uint8_t follow = jpeg[next + 1];
                if (follow == 0x00 || (follow >= 0xd0 && follow <= 0xd7)) next += 2;
                else if (follow == 0xff) ++next;
                else break;
            }
        }
        at = next;
    }
}

/* The multi-picture index (APP2 "MPF") describes the pictures that follow the
 * image, which the device does not pass on. */
bool multiPictureIndex(std::span<const std::uint8_t> jpeg, std::uint8_t marker, std::size_t code, std::size_t length) noexcept
{
    return marker == 0xe2 && length >= 6 && std::memcmp(jpeg.data() + code + 3, "MPF\0", 4) == 0;
}

/* A frame header (SOF0 to SOF15 except the table, JPEG-extension and
 * arithmetic-conditioning codes 0xc4, 0xc8 and 0xcc). */
bool frameHeader(std::uint8_t marker) noexcept
{
    return marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
}

} // namespace

std::optional<JpegWithGap> reserveExifGap(std::span<const std::uint8_t> jpeg, std::uint32_t gapLength)
{
    if (gapLength < 6 || gapLength > 65533 || jpeg.size() < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) return std::nullopt;
    JpegWithGap result;
    result.head = {0xff, 0xd8};
    result.head.push_back(0xff);
    result.head.push_back(0xe1);
    const std::uint32_t segmentLength = gapLength + 2;
    result.head.push_back(std::uint8_t(segmentLength >> 8));
    result.head.push_back(std::uint8_t(segmentLength));
    result.gapOffset = std::uint32_t(result.head.size());
    result.gapLength = gapLength;
    result.head.resize(result.head.size() + gapLength, 0);
    std::memcpy(result.head.data() + result.gapOffset, "Exif\0\0", 6);

    std::size_t at = 2;
    for (;;) {
        // A marker is 0xff, any number of 0xff fill bytes, then its code.
        if (at + 2 > jpeg.size() || jpeg[at] != 0xff) return std::nullopt;
        std::size_t code = at + 1;
        while (code < jpeg.size() && jpeg[code] == 0xff) ++code;
        if (code >= jpeg.size()) return std::nullopt;
        const std::uint8_t marker = jpeg[code];
        if (marker == 0xda) {
            // The scan to the end of image, and no further.
            const auto end = imageEnd(jpeg, code - 1);
            if (!end) return std::nullopt;
            result.tail = jpeg.subspan(code - 1, *end - (code - 1));
            return result;
        }
        // Markers that stand alone have no place before a scan.
        if (standalone(marker)) return std::nullopt;
        if (code + 3 > jpeg.size()) return std::nullopt;
        const std::size_t length = std::size_t(jpeg[code + 1]) << 8 | jpeg[code + 2];
        if (length < 2 || code + 1 + length > jpeg.size()) return std::nullopt;
        if (frameHeader(marker) && length >= 8 && result.frameWidth == 0 && result.frameHeight == 0) {
            // Precision, then the height and the width, two bytes each.
            result.frameHeight = std::uint16_t(jpeg[code + 4] << 8 | jpeg[code + 5]);
            result.frameWidth = std::uint16_t(jpeg[code + 6] << 8 | jpeg[code + 7]);
        }
        if (marker != 0xe1 && !multiPictureIndex(jpeg, marker, code, length))
            result.head.insert(result.head.end(), jpeg.begin() + std::ptrdiff_t(code - 1),
                               jpeg.begin() + std::ptrdiff_t(code + 1 + length));
        at = code + 1 + length;
    }
}

} // namespace uml::camera

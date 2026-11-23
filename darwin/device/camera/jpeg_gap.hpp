// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uml::camera {

/* The payload length of the Exif segment the device reserves in a JPEG still.
 * The segment is an APP1 whose payload (the bytes after the two length
 * bytes, "Exif\0\0" first and zeros after it) the guest rewrites in place;
 * its length field stays as written. */
inline constexpr std::uint32_t exifGapLength = 65000;

/* A JPEG as the device writes it into a guest buffer: `head` (the start of
 * image, the reserved Exif segment and every other segment before the scan)
 * and `tail` (the scan or scans and the end of image, exactly as the platform
 * made them), so the entropy-coded data is copied once. */
struct JpegWithGap {
    std::vector<std::uint8_t> head;
    std::span<const std::uint8_t> tail;
    std::uint32_t gapOffset {};  // of the payload, from the start of the file
    std::uint32_t gapLength {};
    // The picture's size as its frame header (SOF) states it, in the sensor's
    // orientation; zero when the header is missing or states no height.
    std::uint16_t frameWidth {};
    std::uint16_t frameHeight {};

    [[nodiscard]] std::size_t size() const noexcept { return head.size() + tail.size(); }
};

/* Reserves the Exif gap in `jpeg` and drops its own Exif and XMP (APP1)
 * segments and its multi-picture index (APP2 MPF); the other segments (JFIF,
 * quantization and Huffman tables, the frame header) keep their order. The
 * file ends at the end of image of the first picture: a gain map, a depth
 * image or any other picture after it is not passed on. Returns nothing for
 * data that is not a JPEG with a scan and an end of image. The result's
 * `tail` refers to `jpeg` and lies inside it, and its frame size is the
 * first frame header's. */
[[nodiscard]] std::optional<JpegWithGap> reserveExifGap(std::span<const std::uint8_t> jpeg,
                                                        std::uint32_t gapLength = exifGapLength);

} // namespace uml::camera

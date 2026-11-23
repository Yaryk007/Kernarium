// SPDX-License-Identifier: Apache-2.0
//
// The two edits the HAL makes to a JPEG that the host encoded, and the one Exif
// value that is the HAL's own rule. The host leaves an Exif segment (an APP1
// whose length field already says its payload is abi::exif_gap_length bytes) in
// the picture, and the HAL writes the Exif data and the thumbnail into it in
// place, so the picture's entropy-coded data is not moved again. Android's JPEG
// buffer then ends with a camera3_jpeg_blob_t that states the size of the
// picture.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "camera_abi.hpp"

namespace andarium::camera {

// camera3_jpeg_blob_t: the last bytes of a BLOB buffer that holds a JPEG.
struct JpegBlobHeader {
    std::uint16_t blob_id;
    std::uint32_t jpeg_size;
};
static_assert(sizeof(JpegBlobHeader) == 8);
constexpr std::uint16_t jpeg_blob_id = 0x00FF;

// The focal length and the physical sensor size that a camera's description
// states, in millimetres: a pair, which gives the camera's field of view (the
// host derives both from the field of view it reports, so the pair holds
// whatever pitch it assumed for the pixels). A description that states none of
// the three states no pair, and every member is zero then: the static metadata
// keeps a template's default size for such a camera because the platform
// requires a size, and that default is no stated one.
struct StatedOptics {
    float focal_length_mm = 0.0f;
    float sensor_width_mm = 0.0f;
    float sensor_height_mm = 0.0f;
};

[[nodiscard]] inline StatedOptics stated_optics(const abi::Desc& desc) noexcept
{
    if (desc.focal_length_um == 0 || desc.sensor_width_um == 0 || desc.sensor_height_um == 0) return {};
    return {float(desc.focal_length_um) / 1000.0f, float(desc.sensor_width_um) / 1000.0f,
            float(desc.sensor_height_um) / 1000.0f};
}

// The Exif tag FocalLengthIn35mmFormat: the focal length that a camera with a
// 35 mm frame (43.27 mm across the diagonal) needs for the same field of view,
// in whole millimetres. It follows from the camera's stated focal length and
// physical sensor size, all in millimetres, and not from the focal length of
// one picture (a calibration replaces that with the lens's own, which does not
// go with a size derived from the field of view). Without a pair (the camera
// states none) or with a result that no sensible lens has (the tag is 16 bits),
// there is no such value and the tag is left out: a sensor of no size is not a
// sensor of a hundredth of a millimetre, as a divisor clamped to that would
// make it.
[[nodiscard]] inline std::optional<std::uint16_t> focal_length_in_35mm_film(float focal_length_mm,
                                                                              float sensor_width_mm,
                                                                              float sensor_height_mm)
{
    constexpr float film_diagonal_mm = 43.27f;
    if (!(focal_length_mm > 0.0f) || !(sensor_width_mm > 0.0f) || !(sensor_height_mm > 0.0f)) return std::nullopt;
    if (!std::isfinite(focal_length_mm) || !std::isfinite(sensor_width_mm) || !std::isfinite(sensor_height_mm))
        return std::nullopt;
    const float diagonal = std::hypot(sensor_width_mm, sensor_height_mm);
    const float value = std::round(focal_length_mm * film_diagonal_mm / diagonal);
    if (!(value >= 1.0f) || value > 65535.0f) return std::nullopt;
    return static_cast<std::uint16_t>(value);
}

// Writes the Exif data (what libexif saves: "Exif\0\0" and the TIFF structure)
// at the start of the gap and zeroes the rest of it. Returns false and changes
// nothing when the gap does not lie inside the buffer, is not the length the
// device reserves or is too small for the data.
inline bool place_exif(std::span<std::uint8_t> jpeg, std::uint32_t gap_offset, std::uint32_t gap_length,
                       std::span<const std::uint8_t> exif)
{
    if (gap_length != abi::exif_gap_length || gap_offset < 4) return false;
    if (gap_offset > jpeg.size() || gap_length > jpeg.size() - gap_offset) return false;
    if (exif.size() > gap_length) return false;
    // The two bytes before the payload are the segment's length, and the two
    // before those the APP1 marker; a gap anywhere else is not the device's.
    if (jpeg[gap_offset - 4] != 0xFF || jpeg[gap_offset - 3] != 0xE1) return false;
    if (!exif.empty()) std::memcpy(jpeg.data() + gap_offset, exif.data(), exif.size());
    std::memset(jpeg.data() + gap_offset + exif.size(), 0, gap_length - exif.size());
    return true;
}

// Writes the blob header at the end of a BLOB buffer. The picture must leave
// room for it.
inline bool write_blob_header(std::span<std::uint8_t> buffer, std::uint32_t jpeg_size)
{
    if (buffer.size() < sizeof(JpegBlobHeader) || jpeg_size > buffer.size() - sizeof(JpegBlobHeader)) return false;
    JpegBlobHeader header{};
    header.blob_id = jpeg_blob_id;
    header.jpeg_size = jpeg_size;
    std::memcpy(buffer.data() + buffer.size() - sizeof(header), &header, sizeof(header));
    return true;
}

} // namespace andarium::camera

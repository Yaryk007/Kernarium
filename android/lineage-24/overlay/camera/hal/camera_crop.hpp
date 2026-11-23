// SPDX-License-Identifier: Apache-2.0
//
// The crop region that a result of the camera HAL states. For a HAL that lists
// the zoom ratio the camera service splits an application's zoom into
// CONTROL_ZOOM_RATIO and a residual SCALER_CROP_REGION (what the ratio cannot
// express, whole pixels of the active array) and joins them again in the result:
// it unscales the result's crop region by the result's ratio. A result that
// states the whole array instead of the request's region loses the residual, and
// the application's region is a pixel or three off once the ratio is high (about
// 57x on a 4032 x 3024 array, CTS testDigitalZoom).
//
// The space matters. The HAL's session layer (google_camera_hal, ZoomRatioMapper)
// divides the request's region by the ratio, about the array's centre, before the
// camera's request state sees it, and multiplies the result's region by the ratio
// afterwards: the helper below works in the camera's space, between the two, and
// the ratio is not one of its inputs. In that space the zoom the camera service
// reports back, the ratio times the array over the application's region, is the
// array over the region the helper returns, so the smallest region is the array
// divided by the advertised maximum digital zoom, whatever the ratio. A camera
// that lists no zoom ratio gets the request as the application made it, and the
// same rule holds.
//
// The camera is CENTER_ONLY: it applies a region centred in the active array, so
// the result states that region, not the rectangle that was asked for. Its size is
// the requested size, clamped to the array and to that smallest size. The origin
// of the request is not used; a request with no region, or an empty one, gets the
// array. The host scales a frame to the stream's size and applies the zoom ratio;
// it takes no residual region, so for a region whose aspect differs from the
// stream's the result states the region and not what the host does to the picture.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace andarium::camera {

// The layout of android.scaler.cropRegion: left, top, width, height.
struct CropRegion {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

// The smallest extent of a region over `extent` pixels: the extent over the maximum
// digital zoom, rounded up so that the zoom stays within the maximum, and within
// [1, extent]. A maximum that is not above 1 allows no crop.
[[nodiscard]] constexpr std::int32_t minimum_extent(std::int32_t extent, double max_digital_zoom) noexcept
{
    if (extent <= 1 || !(max_digital_zoom > 1.0)) return extent;
    const double smallest = static_cast<double>(extent) / max_digital_zoom;
    if (!(smallest < static_cast<double>(extent))) return extent;
    auto whole = static_cast<std::int64_t>(smallest);
    if (static_cast<double>(whole) < smallest) ++whole;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(whole, 1, extent));
}

// `requested` is the request's SCALER_CROP_REGION entry as the camera sees it (null or
// short when the request has none), `array` the active array of the sensor mode in use
// and `max_digital_zoom` the camera's advertised maximum. The region is centred in the
// array; an odd remainder goes to the right and the bottom.
[[nodiscard]] constexpr CropRegion result_crop_region(const std::int32_t* requested, std::size_t count,
                                                      const CropRegion& array, double max_digital_zoom) noexcept
{
    if (requested == nullptr || count < 4 || requested[2] <= 0 || requested[3] <= 0 || array.width <= 0 ||
        array.height <= 0) {
        return array;
    }
    const std::int32_t width = std::max(std::min(requested[2], array.width), minimum_extent(array.width, max_digital_zoom));
    const std::int32_t height =
        std::max(std::min(requested[3], array.height), minimum_extent(array.height, max_digital_zoom));
    return {array.left + (array.width - width) / 2, array.top + (array.height - height) / 2, width, height};
}

} // namespace andarium::camera

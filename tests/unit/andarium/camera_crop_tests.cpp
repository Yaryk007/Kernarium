// SPDX-License-Identifier: GPL-2.0-or-later
//
// The crop region a result of the camera HAL states
// (android/lineage-24/overlay/camera/hal/camera_crop.hpp). The camera service
// splits an application's zoom into the ratio and a residual crop region and
// joins them in the result, so the result must hand back the residual the request
// carried; the camera is CENTER_ONLY, so that residual is the centred region of
// the requested size, clamped to the array and to the smallest size the maximum
// digital zoom allows.
//
// The HAL's session layer (google_camera_hal ZoomRatioMapper) stands between the
// service and the camera: it divides the request's region by the ratio before the
// camera sees it and multiplies the result's region by the ratio afterwards. The
// helper works in the camera's space, so its rows are of two kinds: the helper
// itself, and the whole round trip, request to result, through a model of the
// mapper's arithmetic (below). The round trip holds what the application gets
// back: the review's asymmetric request, the CTS testDigitalZoom residual on a
// 4032 x 3024 front camera, letterbox and pillarbox crops at ratios 1, 2, 0.5 and
// the maximum, the smallest size, and an off-centre request.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "camera_crop.hpp"
#include "require.hpp"

using namespace andarium::camera;
using uml::testing::require;

namespace {

constexpr CropRegion array{0, 0, 4032, 3024};
// A camera that advertises 8x; the CTS front camera of the round trip rows advertises 100x.
constexpr double eight = 8.0;
constexpr double hundred = 100.0;

bool is(const CropRegion& region, std::int32_t left, std::int32_t top, std::int32_t width, std::int32_t height)
{
    return region.left == left && region.top == top && region.width == width && region.height == height;
}

// The helper alone, in the camera's space.
CropRegion ask(const std::int32_t (&region)[4], double maximum = eight, const CropRegion& within = array)
{
    return result_crop_region(region, 4, within, maximum);
}

// A model of the arithmetic of the HAL's zoom ratio mapper (google_camera_hal, common/hal/utils: ConvertZoomRatio on a request,
// RevertZoomRatio on a result, ClampBoundary), in the single precision it has: a region is scaled about the array's centre by the
// ratio, rounded to whole pixels, and cut to the array (a request only for a ratio of 1 or more). The mapper applies it to the
// crop region of the request before the camera's request state sees it, and to the region of the result before the service does.
struct Rect {
    std::int32_t left, top, width, height;
};

void clamp_boundary(const CropRegion& within, std::int32_t& x, std::int32_t& y, std::int32_t& width, std::int32_t& height)
{
    x = std::max(0, std::min(x, within.width - 1));
    y = std::max(0, std::min(y, within.height - 1));
    width = std::max(1, std::min(width, within.width - x));
    height = std::max(1, std::min(height, within.height - y));
}

Rect convert_zoom_ratio(float ratio, const CropRegion& within, Rect region)
{
    const float w = static_cast<float>(within.width), h = static_cast<float>(within.height);
    Rect out{};
    out.left = static_cast<std::int32_t>(std::round(static_cast<float>(region.left) / ratio + 0.5f * w * (1.0f - 1.0f / ratio)));
    out.top = static_cast<std::int32_t>(std::round(static_cast<float>(region.top) / ratio + 0.5f * h * (1.0f - 1.0f / ratio)));
    out.width = static_cast<std::int32_t>(std::round(static_cast<float>(region.width) / ratio));
    out.height = static_cast<std::int32_t>(std::round(static_cast<float>(region.height) / ratio));
    if (ratio >= 1.0f) clamp_boundary(within, out.left, out.top, out.width, out.height);
    return out;
}

Rect revert_zoom_ratio(float ratio, const CropRegion& within, Rect region)
{
    const float w = static_cast<float>(within.width), h = static_cast<float>(within.height);
    Rect out{};
    out.left = static_cast<std::int32_t>(std::round(static_cast<float>(region.left) * ratio - 0.5f * w * (ratio - 1.0f)));
    out.top = static_cast<std::int32_t>(std::round(static_cast<float>(region.top) * ratio - 0.5f * h * (ratio - 1.0f)));
    out.width = static_cast<std::int32_t>(std::round(static_cast<float>(region.width) * ratio));
    out.height = static_cast<std::int32_t>(std::round(static_cast<float>(region.height) * ratio));
    clamp_boundary(within, out.left, out.top, out.width, out.height);
    return out;
}

// What the application gets back for a crop region and a zoom ratio: the mapper divides, the camera states its region, the mapper
// multiplies. `maximum` is the camera's advertised maximum digital zoom.
Rect through_the_camera(Rect asked, float ratio, double maximum, const CropRegion& within = array)
{
    const Rect seen = convert_zoom_ratio(ratio, within, asked);
    const std::int32_t entry[4] = {seen.left, seen.top, seen.width, seen.height};
    const CropRegion applied = result_crop_region(entry, 4, within, maximum);
    return revert_zoom_ratio(ratio, within, {applied.left, applied.top, applied.width, applied.height});
}

bool is(const Rect& region, std::int32_t left, std::int32_t top, std::int32_t width, std::int32_t height)
{
    return region.left == left && region.top == top && region.width == width && region.height == height;
}

// The zoom the camera service reports for a region and a ratio: the ratio times the array over the region.
double reported_zoom(const Rect& region, float ratio, const CropRegion& within = array)
{
    return static_cast<double>(ratio) * within.width / region.width;
}

void check_an_asymmetric_request_is_recentred()
{
    // The HAL review's request: zoom ratio 1, the top half of a 4032 x 3024 array. A CENTER_ONLY camera cannot apply it.
    const std::int32_t top_half[4] = {0, 0, 4032, 1512};
    require(is(ask(top_half), 0, 756, 4032, 1512), "the same size, centred: top 756");
    const std::int32_t left_half[4] = {0, 0, 2016, 3024};
    require(is(ask(left_half), 1008, 0, 2016, 3024), "and the left half is the middle half");
    const std::int32_t inside[4] = {1000, 800, 2000, 1500};
    require(is(ask(inside), 1016, 762, 2000, 1500), "a region inside the array is centred as well");
    const std::int32_t odd[4] = {0, 0, 3955, 2870};
    require(is(ask(odd), 38, 77, 3955, 2870), "an odd remainder goes to the right and the bottom");
    const std::int32_t whole[4] = {0, 0, 4032, 3024};
    require(is(ask(whole), 0, 0, 4032, 3024), "a request for the whole array is the whole array");
}

void check_what_the_request_does_not_state()
{
    require(is(result_crop_region(nullptr, 0, array, eight), 0, 0, 4032, 3024), "no entry: the array");
    const std::int32_t entry[4] = {10, 10, 1000, 1000};
    require(is(result_crop_region(entry, 0, array, eight), 0, 0, 4032, 3024), "an entry with no values: the array");
    require(is(result_crop_region(entry, 3, array, eight), 0, 0, 4032, 3024), "a short entry: the array");
    require(is(result_crop_region(nullptr, 4, array, eight), 0, 0, 4032, 3024), "values that are not there: the array");
}

void check_an_unusable_size_falls_back_to_the_array()
{
    const std::int32_t empty[4] = {10, 10, 0, 100};
    require(is(ask(empty), 0, 0, 4032, 3024), "no width: the array");
    const std::int32_t flat[4] = {10, 10, 100, 0};
    require(is(ask(flat), 0, 0, 4032, 3024), "no height: the array");
    const std::int32_t negative[4] = {10, 10, -5, 100};
    require(is(ask(negative), 0, 0, 4032, 3024), "a negative width: the array");
    constexpr CropRegion nothing{0, 0, 0, 0};
    const std::int32_t some[4] = {0, 0, 100, 100};
    require(is(result_crop_region(some, 4, nothing, eight), 0, 0, 0, 0), "an empty array stays empty");
}

void check_a_region_partly_or_wholly_outside_is_centred_at_its_size()
{
    const std::int32_t partly[4] = {-10, -20, 1000, 800};
    require(is(ask(partly), 1516, 1112, 1000, 800), "a region that starts before the array keeps its size");
    const std::int32_t past_the_edge[4] = {4000, 3000, 1000, 800};
    require(is(ask(past_the_edge), 1516, 1112, 1000, 800), "and one that ends past it");
    const std::int32_t right[4] = {4032, 0, 1000, 800};
    require(is(ask(right), 1516, 1112, 1000, 800), "and one that lies right of it, as the origin is not used");
    const std::int32_t larger[4] = {-5, -5, 5000, 4000};
    require(is(ask(larger), 0, 0, 4032, 3024), "a region around the array is cut to the array");
    const std::int32_t one_side[4] = {0, 0, 5000, 1000};
    require(is(ask(one_side), 0, 1012, 4032, 1000), "and a side that is too long is cut alone");
}

void check_the_smallest_size_follows_the_maximum_zoom()
{
    const std::int32_t tiny[4] = {0, 0, 1, 1};
    // 4032 / 8 = 504 and 3024 / 8 = 378.
    require(is(ask(tiny), 1764, 1323, 504, 378), "the minimum size: the array over the maximum zoom");
    const std::int32_t just_below[4] = {0, 0, 503, 377};
    require(is(ask(just_below), 1764, 1323, 504, 378), "a size just below it is the minimum");
    const std::int32_t at[4] = {0, 0, 504, 378};
    require(is(ask(at), 1764, 1323, 504, 378), "and one at it stays");
    const std::int32_t above[4] = {0, 0, 505, 379};
    require(is(ask(above), 1763, 1322, 505, 379), "one above it stays");
    // The camera's space has no ratio in it: the session layer has divided the region by the ratio already.
    require(is(ask(tiny, 4.0), 1512, 1134, 1008, 756), "a maximum of 4: 1008 x 756");
    require(is(ask(tiny, hundred), 1995, 1496, 41, 31), "a maximum of 100: 41 x 31, rounded up");
    // A camera without digital zoom (maximum 1, or none stated) can crop nothing.
    require(is(ask(tiny, 1.0), 0, 0, 4032, 3024), "a maximum zoom of 1: the array");
    require(is(ask(tiny, 0.0), 0, 0, 4032, 3024), "none stated: the array");
    // The minimum is rounded up, so the zoom the service computes never exceeds the maximum.
    constexpr CropRegion odd{0, 0, 1001, 751};
    const std::int32_t smallest[4] = {0, 0, 1, 1};
    const CropRegion region = ask(smallest, 3.0, odd);
    require(region.width == 334 && region.height == 251, "1001 / 3 and 751 / 3 round up");
    require(1001.0 / region.width <= 3.0 && 751.0 / region.height <= 3.0, "so the zoom stays within the maximum");
}

void check_the_array_may_have_an_origin_and_the_sums_may_overflow()
{
    constexpr CropRegion moved{100, 50, 1000, 800};
    const std::int32_t inside[4] = {200, 150, 300, 200};
    require(is(ask(inside, eight, moved), 450, 350, 300, 200), "centred in an array that has an origin");
    const std::int32_t before[4] = {0, 0, 3000, 2000};
    require(is(ask(before, eight, moved), 100, 50, 1000, 800), "and cut to it");
    constexpr std::int32_t big = std::numeric_limits<std::int32_t>::max();
    const std::int32_t overflow[4] = {big - 1, big - 1, big, big};
    require(is(ask(overflow), 0, 0, 4032, 3024), "a size beyond the array is the array, nothing wraps");
    const std::int32_t huge[4] = {0, 0, big, big};
    require(is(ask(huge), 0, 0, 4032, 3024), "a huge region is the array");
    constexpr CropRegion wide{big - 10, big - 10, 4, 4};
    const std::int32_t small[4] = {0, 0, 2, 2};
    require(is(result_crop_region(small, 4, wide, eight), big - 9, big - 9, 2, 2), "an array at the end of the range is centred without overflow");
}

void check_the_round_trip_at_a_ratio_of_one()
{
    // A camera without zoom ratio support is this case: the camera sees the request as the application made it.
    require(is(through_the_camera({0, 0, 4032, 1512}, 1.0f, eight), 0, 756, 4032, 1512), "ratio 1, the review's asymmetric request: top 756");
    require(is(through_the_camera({0, 0, 2016, 3024}, 1.0f, eight), 1008, 0, 2016, 3024), "ratio 1, a pillarbox: the middle half");
    require(is(through_the_camera({0, 756, 4032, 1512}, 1.0f, eight), 0, 756, 4032, 1512), "ratio 1, a centred letterbox stays");
    require(is(through_the_camera({1000, 800, 2000, 1500}, 1.0f, eight), 1016, 762, 2000, 1500), "ratio 1, an off-centre region is centred");
    require(is(through_the_camera({0, 0, 4032, 3024}, 1.0f, eight), 0, 0, 4032, 3024), "ratio 1, the whole array");
    require(is(through_the_camera({0, 0, 1, 1}, 1.0f, eight), 1764, 1323, 504, 378), "ratio 1, the smallest size");
}

void check_the_round_trip_at_a_ratio_above_one()
{
    // The application's region at a ratio of 2 and a maximum of 4: the session layer halves the region, so the camera sees 2016 x 756
    // for a 4032 x 1512 letterbox. The minimum in the camera's space is 1008 x 756; a minimum scaled by the ratio (2016 x 1512) raised
    // that height and the result came back as the whole array: the letterbox was lost.
    require(is(through_the_camera({0, 756, 4032, 1512}, 2.0f, 4.0), 0, 756, 4032, 1512), "ratio 2, a letterbox: it stays");
    require(is(through_the_camera({1008, 0, 2016, 3024}, 2.0f, 4.0), 1008, 0, 2016, 3024), "ratio 2, a pillarbox: it stays");
    require(is(through_the_camera({0, 0, 2016, 1512}, 2.0f, 4.0), 1008, 756, 2016, 1512), "ratio 2, an off-centre request is centred");
    require(is(through_the_camera({0, 0, 4032, 3024}, 2.0f, 4.0), 0, 0, 4032, 3024), "ratio 2, the whole array");
    // The smallest size the service may report: the ratio times the array over the region, at most the maximum.
    const Rect smallest = through_the_camera({0, 0, 1, 1}, 2.0f, 4.0);
    require(is(smallest, 1008, 756, 2016, 1512), "ratio 2, the smallest size: half the array, which is the zoom of 4");
    require(reported_zoom(smallest, 2.0f) <= 4.0 + 1e-9, "and the service reports at most the maximum zoom");
    require(is(through_the_camera({0, 756, 4032, 1512}, 4.0f, 8.0), 0, 756, 4032, 1512), "ratio 4, maximum 8, a letterbox: it stays");
    // A ratio between: the region is smaller than the smallest the maximum allows only below the line.
    const Rect between = through_the_camera({0, 0, 1000, 800}, 3.0f, 8.0);
    require(reported_zoom(between, 3.0f) <= 8.0 + 1e-9, "ratio 3, a small region: at most the maximum zoom");
    require(between.width == 1512 && between.height == 1134, "ratio 3, a small region: the smallest size, 3 x 504 by 3 x 378");
}

void check_the_round_trip_at_the_maximum_ratio()
{
    // At the maximum ratio the application has no crop left to make: whatever it asks, the result is the array.
    require(is(through_the_camera({0, 0, 1, 1}, 4.0f, 4.0), 0, 0, 4032, 3024), "ratio = maximum, the smallest request: the array");
    require(is(through_the_camera({0, 756, 4032, 1512}, 4.0f, 4.0), 0, 0, 4032, 3024), "ratio = maximum, a letterbox: the array");
    require(is(through_the_camera({0, 0, 4032, 3024}, 4.0f, 4.0), 0, 0, 4032, 3024), "ratio = maximum, the array: the array");
    require(is(through_the_camera({0, 0, 4032, 3024}, 8.0f, 8.0), 0, 0, 4032, 3024), "maximum 8: the array as well");
}

void check_the_round_trip_at_a_ratio_below_one()
{
    // A camera that zooms out lists a ratio below 1. The session layer does not cut a request to the array then, and the camera, which
    // cannot show more than its array, states the array; the application gets the region that is the zoom the camera really has.
    const Rect whole = through_the_camera({0, 0, 4032, 3024}, 0.5f, eight);
    require(is(whole, 1008, 756, 2016, 1512), "ratio 0.5, the whole array: half of it, the zoom of 1");
    require(reported_zoom(whole, 0.5f) == 1.0, "and the service reports a zoom of 1");
    // The smallest size: 0.5 x 504 by 0.5 x 378 in the application's space is 252 x 189, a zoom of 0.5 x 4032 / 252 = 8.
    const Rect smallest = through_the_camera({0, 0, 1, 1}, 0.5f, eight);
    require(is(smallest, 1890, 1418, 252, 189), "ratio 0.5, the smallest size: 252 x 189");
    require(reported_zoom(smallest, 0.5f) <= eight + 1e-9, "and the service reports at most the maximum zoom");
}

void check_the_round_trip_of_the_cts_residual_at_a_high_ratio()
{
    // CTS testDigitalZoom, step 12, camera 1 (4032 x 3024, maximum 100): the residual the camera service sent with the ratio 77.5385.
    // The session layer's rounding to whole pixels in each direction costs up to half the ratio in a dimension; the region is centred.
    constexpr float ratio = 77.5385f;
    const Rect step12 = through_the_camera({77, 115, 3955, 2870}, ratio, hundred);
    require(std::abs(step12.width - 3955) <= 40 && std::abs(step12.height - 2870) <= 40, "step 12: the size within the mapper's rounding");
    require(std::abs((step12.left * 2 + step12.width) - 4032) <= 80 && std::abs((step12.top * 2 + step12.height) - 3024) <= 80,
            "step 12: centred");
    require(reported_zoom(step12, ratio) <= hundred + 1.0, "step 12: the service reports about the zoom it asked for");
    // The letterbox at a high ratio: the size of the request is kept (to the rounding), not raised to a scaled minimum.
    const Rect letterbox = through_the_camera({0, 756, 4032, 1512}, 10.0f, hundred);
    require(std::abs(letterbox.width - 4032) <= 10 && std::abs(letterbox.height - 1512) <= 10,
            "ratio 10, maximum 100, a letterbox: its size, not the whole array");
}

} // namespace

int main()
{
    check_an_asymmetric_request_is_recentred();
    check_what_the_request_does_not_state();
    check_an_unusable_size_falls_back_to_the_array();
    check_a_region_partly_or_wholly_outside_is_centred_at_its_size();
    check_the_smallest_size_follows_the_maximum_zoom();
    check_the_array_may_have_an_origin_and_the_sums_may_overflow();
    check_the_round_trip_at_a_ratio_of_one();
    check_the_round_trip_at_a_ratio_above_one();
    check_the_round_trip_at_the_maximum_ratio();
    check_the_round_trip_at_a_ratio_below_one();
    check_the_round_trip_of_the_cts_residual_at_a_high_ratio();
    return 0;
}

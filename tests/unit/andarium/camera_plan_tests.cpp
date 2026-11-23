// SPDX-License-Identifier: GPL-2.0-or-later
//
// The camera HAL's rules for what a camera is (android/lineage-24/overlay/camera/hal):
// the IDs Android sees for the host's cameras, the capabilities and the stream
// tables its description and formats earn.
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <vector>

#include "camera_concurrent_vectors.hpp"
#include "camera_ids.hpp"
#include "camera_plan.hpp"
#include "require.hpp"

using namespace andarium::camera;
using uml::testing::require;

namespace {

std::vector<CameraKey> triple_host()
{
    return {{0, abi::facing::back, abi::lens::zoom},
            {1, abi::facing::front, abi::lens::wide},
            {2, abi::facing::back, abi::lens::wide},
            {3, abi::facing::back, abi::lens::ultra_wide},
            {4, abi::facing::back, abi::lens::telephoto}};
}

std::uint32_t id_of(const std::vector<IdAssignment>& ids, std::uint32_t index)
{
    for (const auto& entry : ids)
        if (entry.index == index) return entry.id;
    return 99;
}

// A host of three lenses that publishes no zoom camera (the host's wire order is the primary back lens, the front
// camera, then the other lenses): the wide lens is "0", the front camera "1", the ultra wide "3" and the telephoto "4",
// and there is no "2". Only the wide lens streams with the front camera, so every other pair conflicts.
void check_three_lens_host()
{
    const std::vector<CameraKey> keys = {{0, abi::facing::back, abi::lens::wide},
                                         {1, abi::facing::front, abi::lens::wide},
                                         {2, abi::facing::back, abi::lens::ultra_wide},
                                         {3, abi::facing::back, abi::lens::telephoto}};
    const auto ids = assign_ids(keys);
    require(ids.size() == 4, "four cameras");
    require(ids[0].id == 0 && ids[0].index == 0 && ids[1].id == 1 && ids[1].index == 1 && ids[2].id == 3 && ids[2].index == 2 &&
                ids[3].id == 4 && ids[3].index == 3,
            "wide 0, front 1, ultra wide 3, telephoto 4: no 2");
    for (const auto& assignment : ids) require(assignment.id != wide_camera_id, "the wide lens is the primary camera, not a second one");

    // Two lenses: the ultra wide keeps its ID, and a host whose only back lens is the ultra wide numbers it "0".
    const std::vector<CameraKey> two = {keys[0], keys[1], keys[2]};
    const auto pair = assign_ids(two);
    require(pair.size() == 3 && pair[0].id == 0 && pair[1].id == 1 && pair[2].id == 3 && pair[2].index == 2, "wide, front, ultra wide");
    const std::vector<CameraKey> ultra_only = {{0, abi::facing::back, abi::lens::ultra_wide}, keys[1]};
    const auto alone = assign_ids(ultra_only);
    require(alone.size() == 2 && alone[0].id == 0 && alone[0].index == 0 && alone[1].id == 1, "the primary back lens is 0 whichever it is");

    // The host's masks (by its own numbers): the wide lens and the front camera name one another.
    const std::uint32_t wide = 1u << 0, front = 1u << 1, ultra = 1u << 2, tele = 1u << 3;
    const std::vector<Concurrency> cameras = {{0, 0, front, ultra | tele},
                                              {1, 1, wide, ultra | tele},
                                              {3, 2, 0, wide | front | tele},
                                              {4, 3, 0, wide | front | ultra}};
    const auto sets = concurrent_sets(cameras);
    require(sets.size() == 1 && sets[0] == std::vector<std::uint32_t>({0, 1}), "one set: the wide lens and the front camera");
    require(concurrent_feature(cameras, [](std::uint32_t) { return true; }), "the layout's concurrent feature holds");
    require(!concurrent_feature(cameras, [](std::uint32_t id) { return id != 0; }),
            "and does not when the wide lens misses the guaranteed sizes");
    require(conflicting_ids(cameras, 0) == std::vector<std::uint32_t>({3, 4}), "the wide lens conflicts with the other lenses");
    require(conflicting_ids(cameras, 1) == std::vector<std::uint32_t>({3, 4}), "so does the front camera");
    require(conflicting_ids(cameras, 3) == std::vector<std::uint32_t>({0, 1, 4}), "the ultra wide conflicts with every other camera");
    require(conflicting_ids(cameras, 4) == std::vector<std::uint32_t>({0, 1, 3}), "as does the telephoto");
    require(conflicting_ids(cameras, 2).empty(), "no camera has the ID 2");
}

void check_ids()
{
    auto keys = triple_host();
    auto ids = assign_ids(keys);
    require(ids.size() == 5, "a triple host lists five cameras");
    require(id_of(ids, 0) == 0 && id_of(ids, 1) == 1 && id_of(ids, 2) == 2 && id_of(ids, 3) == 3 && id_of(ids, 4) == 4,
            "zoom 0, front 1, wide 2, ultra wide 3, telephoto 4");
    for (std::size_t i = 1; i < ids.size(); ++i) require(ids[i - 1].id < ids[i].id, "ascending");

    // A dual wide device (no telephoto): there is no "4".
    keys.pop_back();
    ids = assign_ids(keys);
    require(ids.size() == 4 && id_of(ids, 3) == 3, "no telephoto, no ID 4");

    // A single-lens device: the one lens is "0", the front camera "1", and there is no "2".
    const std::vector<CameraKey> mini = {{0, abi::facing::back, abi::lens::wide}, {1, abi::facing::front, abi::lens::wide}};
    ids = assign_ids(mini);
    require(ids.size() == 2 && id_of(ids, 0) == 0 && id_of(ids, 1) == 1, "single lens");

    // The host orders its lenses by the lens kind it has, so a missing wide shifts the indices; the IDs follow the kind.
    const std::vector<CameraKey> odd = {{0, abi::facing::back, abi::lens::zoom},
                                        {1, abi::facing::front, abi::lens::wide},
                                        {2, abi::facing::back, abi::lens::telephoto}};
    ids = assign_ids(odd);
    require(id_of(ids, 2) == 4, "a telephoto keeps ID 4 whatever its index");

    // A Mac: a front camera only. Its ID is "0".
    ids = assign_ids(std::vector<CameraKey>{{0, abi::facing::front, abi::lens::wide}});
    require(ids.size() == 1 && ids[0].id == 0 && ids[0].index == 0, "the front camera alone is ID 0");

    require(assign_ids(std::vector<CameraKey>{}).empty(), "no camera, no ID");
    const std::vector<CameraKey> external = {{0, abi::facing::external, abi::lens::external}};
    require(assign_ids(external).empty(), "an external camera is not listed");
    const std::vector<CameraKey> twice = {{0, abi::facing::back, abi::lens::wide},
                                          {1, abi::facing::back, abi::lens::ultra_wide},
                                          {2, abi::facing::back, abi::lens::ultra_wide}};
    require(assign_ids(twice).size() == 2, "a second camera of one lens is left out");
}

abi::Desc lens_desc()
{
    abi::Desc desc{};
    desc.index = 2;
    desc.facing = abi::facing::back;
    desc.lens = abi::lens::wide;
    desc.flags = abi::feature::autofocus | abi::feature::flash | abi::feature::manual_exposure | abi::feature::raw;
    desc.pixel_array_width = 4032;
    desc.pixel_array_height = 3024;
    desc.focal_length_um = 5100;
    desc.aperture_x100 = 180;
    desc.min_focus_distance_mm = 100;
    desc.iso_min = 32;
    desc.iso_max = 3200;
    desc.exposure_min_ns = 25'000;
    desc.exposure_max_ns = 1'000'000'000;
    desc.bayer_order = abi::bayer::rggb;
    desc.raw_width = 4032;
    desc.raw_height = 3024;
    return desc;
}

Formats phone_formats()
{
    Formats formats;
    formats.video = {{3840, 2160, 1, 30}, {3840, 2160, 60, 60}, {1920, 1080, 1, 60}, {1280, 720, 1, 120}, {640, 480, 1, 30}};
    formats.jpeg = {{4032, 3024}, {1920, 1080}, {640, 480}};
    formats.raw = {4032, 3024};
    return formats;
}

void check_capabilities()
{
    auto desc = lens_desc();
    auto formats = phone_formats();
    auto caps = capabilities_of(desc, formats);
    require(caps.manual_sensor && caps.read_sensor_settings, "a lens with manual exposure has MANUAL_SENSOR");
    require(caps.raw, "a calibrated lens has RAW");
    require(array_size(desc, caps) == Size({4032, 3024}), "the array is the Bayer size");

    desc.flags &= ~abi::feature::raw;
    require(!capabilities_of(desc, formats).raw, "an uncalibrated lens has no RAW");
    desc.flags |= abi::feature::raw;
    desc.bayer_order = abi::bayer::none;
    require(!capabilities_of(desc, formats).raw, "RAW needs a Bayer order");
    desc.bayer_order = abi::bayer::rggb;
    formats.raw = {};
    require(!capabilities_of(desc, formats).raw, "RAW needs a Bayer size");

    // The zoom camera is automatic only, whatever the host says.
    desc = lens_desc();
    desc.lens = abi::lens::zoom;
    caps = capabilities_of(desc, phone_formats());
    require(!caps.manual_sensor && !caps.read_sensor_settings && !caps.raw, "the zoom camera has no manual capabilities");

    // The small tier stops at 1080p.
    formats = phone_formats();
    formats.video = {{1920, 1080, 1, 60}, {640, 480, 1, 30}};
    require(pipeline_depth(formats) == 4, "the small tier has four buffers");
    require(pipeline_depth(phone_formats()) == 6, "the large tier has six");
}

void check_tables()
{
    const auto desc = lens_desc();
    const auto formats = phone_formats();
    const auto caps = capabilities_of(desc, formats);
    const auto tables = stream_tables(formats, caps);
    require(tables.configurations.size() % 4 == 0, "four numbers a configuration");
    const auto entries = tables.configurations.size() / 4;
    require(tables.min_frame_durations.size() == entries * 4 && tables.stall_durations.size() == entries * 4,
            "a duration for each configuration");
    // 6 YUV sizes (the 4 video sizes, 1920x1440 and 320x240) twice, 7 JPEG sizes
    // (the host's 3, 1920x1440, 1280x720, 320x240 and the recording size 3840x2160),
    // one RAW.
    require(entries == 6 * 2 + 7 + 1, "the number of entries");

    auto find = [&](std::int32_t format, std::int32_t width, std::int32_t height) -> int {
        for (std::size_t i = 0; i < entries; ++i)
            if (tables.configurations[4 * i] == format && tables.configurations[4 * i + 1] == width &&
                tables.configurations[4 * i + 2] == height)
                return int(i);
        return -1;
    };
    const int yuv4k = find(pixel_format::yuv_420_888, 3840, 2160);
    require(yuv4k >= 0 && tables.configurations[4 * std::size_t(yuv4k) + 3] == 0, "an output at 4K");
    require(tables.min_frame_durations[4 * std::size_t(yuv4k) + 3] == 1'000'000'000 / 60, "4K runs at the fastest rate it is offered at");
    const int yuv720 = find(pixel_format::implementation_defined, 1280, 720);
    require(yuv720 >= 0 && tables.min_frame_durations[4 * std::size_t(yuv720) + 3] == 1'000'000'000 / 120, "720p at 120");
    const int blob = find(pixel_format::blob, 4032, 3024);
    require(blob >= 0 && tables.stall_durations[4 * std::size_t(blob) + 3] > 300'000'000, "a 12 MP JPEG stalls for a while");
    require(find(pixel_format::blob, 640, 480) >= 0, "a small JPEG");
    for (const auto& size : standard_jpeg_sizes)
        require(find(pixel_format::blob, std::int32_t(size.width), std::int32_t(size.height)) >= 0, "every size CTS requires of a JPEG");
    const int cut = find(pixel_format::yuv_420_888, 1920, 1440);
    require(cut >= 0 && tables.min_frame_durations[4 * std::size_t(cut) + 3] == 1'000'000'000 / 60,
            "a derived size runs at the rate of the mode it is cut from");
    const int record_blob = find(pixel_format::blob, 3840, 2160);
    const int blob_1440 = find(pixel_format::blob, 1920, 1440);
    require(record_blob >= 0 && blob_1440 >= 0 &&
                tables.stall_durations[4 * std::size_t(record_blob) + 3] == jpeg_stall_ns({3840, 2160}) &&
                tables.stall_durations[4 * std::size_t(record_blob) + 3] > tables.stall_durations[4 * std::size_t(blob_1440) + 3],
            "a JPEG at the recording size stalls by its own pixels, more than the smaller derived one");
    // A JPEG keeps the still path's rate (a 30 frames per second floor), host or derived, when the modes run that fast.
    for (std::size_t i = 0; i < entries; ++i)
        if (tables.configurations[4 * i] == pixel_format::blob)
            require(tables.min_frame_durations[4 * i + 3] == 33'333'333, "a JPEG runs at the still path's rate");
    const int cut_blob = find(pixel_format::blob, 320, 240);
    require(cut_blob >= 0 && tables.stall_durations[4 * std::size_t(cut_blob) + 3] >= 100'000'000, "a derived JPEG stalls too");
    const int raw = find(pixel_format::raw16, 4032, 3024);
    require(raw >= 0 && tables.stall_durations[4 * std::size_t(raw) + 3] > 0, "RAW16 at the Bayer size has a stall");
    for (std::size_t i = 0; i < entries; ++i) {
        require(tables.min_frame_durations[4 * i] == tables.configurations[4 * i], "the format repeats");
        require(tables.min_frame_durations[4 * i + 1] == tables.configurations[4 * i + 1], "the width repeats");
    }
    require(find(pixel_format::raw16, 4032, 3024) >= 0, "RAW present");
    require(find(pixel_format::yuv_420_888, 4032, 3024) < 0, "no YUV at a still size the video does not offer");

    Capabilities none;
    const auto without = stream_tables(formats, none);
    require(without.configurations.size() == tables.configurations.size() - 4, "no RAW entry without RAW");
}

// What the host publishes for a camera it has no Bayer still for (the device's description with no RAW feature, no
// Bayer order, no RAW size, and a node that lists no RAW format) gives no RAW anywhere in the plan: no RAW capability,
// no RAW16 stream configuration, and the sensor array is the description's pixel array, not a Bayer size. The metadata
// the HAL builds from these is gated on the same capability (the V4L2 characteristics, `raw`).
void check_no_bayer_raw_host()
{
    auto desc = lens_desc();
    desc.flags &= ~abi::feature::raw;
    desc.bayer_order = abi::bayer::none;
    desc.raw_width = desc.raw_height = 0;
    auto formats = phone_formats();
    formats.raw = {};
    const auto caps = capabilities_of(desc, formats);
    require(caps.manual_sensor && !caps.raw, "the camera keeps its manual sensor and has no RAW capability");
    require(array_size(desc, caps) == Size({4032, 3024}) && desc.raw_width == 0, "the array is the pixel array");
    const auto tables = stream_tables(formats, caps);
    for (std::size_t i = 0; i + 3 < tables.configurations.size(); i += 4)
        require(tables.configurations[i] != pixel_format::raw16, "no RAW16 stream configuration");
    // Any one of the three missing parts is enough on its own, whatever the others say.
    auto only_flag = lens_desc();
    only_flag.flags &= ~abi::feature::raw;
    require(!capabilities_of(only_flag, phone_formats()).raw, "no RAW flag");
    auto only_order = lens_desc();
    only_order.bayer_order = abi::bayer::none;
    require(!capabilities_of(only_order, phone_formats()).raw, "no Bayer order");
    require(!capabilities_of(lens_desc(), formats).raw, "no RAW format on the node");
}

void check_numbers()
{
    const auto ranges = fps_ranges(phone_formats());
    require(ranges.size() % 2 == 0, "pairs");
    auto has = [&](std::int32_t low, std::int32_t high) {
        for (std::size_t i = 0; i + 1 < ranges.size(); i += 2)
            if (ranges[i] == low && ranges[i + 1] == high) return true;
        return false;
    };
    require(has(30, 30) && has(15, 30) && has(60, 60) && has(120, 120), "ranges for 30, 60 and 120");
    require(!has(240, 240), "none for 240 when the host has none");
    Formats slow;
    slow.video = {{640, 480, 1, 30}};
    require(fps_ranges(slow).size() == 6, "a 30 fps host has three ranges");

    const auto sizes = thumbnail_sizes();
    require(sizes.size() >= 4 && sizes[0] == 0 && sizes[1] == 0, "(0, 0) first");
    for (std::size_t i = 2; i + 3 < sizes.size(); i += 2)
        require(std::int64_t(sizes[i]) * sizes[i + 1] <= std::int64_t(sizes[i + 2]) * sizes[i + 3], "ascending area");

    require(jpeg_max_size(phone_formats()) == 4032 * 3024 * 3 / 2 + 65000 + 8, "the largest JPEG");
    const auto desc = lens_desc();
    require(focal_length_mm(desc) == 5.1f && aperture(desc) == 1.8f, "optics");
    require(minimum_focus_dioptres(desc) == 10.0f, "100 mm is ten dioptres");
    auto fixed = desc;
    fixed.min_focus_distance_mm = 0;
    require(minimum_focus_dioptres(fixed) == 0.0f, "fixed focus has no focus distance");
    require(sensitivity_min(desc) == 32 && sensitivity_max(desc) == 3200, "sensitivity");
    auto coarse = desc;
    coarse.iso_min = 200;
    coarse.iso_max = 64;
    require(sensitivity_min(coarse) == 100 && sensitivity_max(coarse) == 100, "the range includes the default ISO");
    require(from_q16(to_q16(0.5)) == 0.5f && from_q16(to_q16(-1.25)) == -1.25f, "Q16 round trip");
    require(max_frame_duration_ns(desc) == 1'000'000'000, "the longest frame is a second");
}

bool has_range(const std::vector<std::int32_t>& ranges, std::int32_t low, std::int32_t high)
{
    for (std::size_t i = 0; i + 1 < ranges.size(); i += 2)
        if (ranges[i] == low && ranges[i + 1] == high) return true;
    return false;
}

// CTS wants (r, r) and (15, r) for r, the rate of the largest YUV size, and a fixed
// range for every rate the host offers, whatever the rate.
void check_fps_ranges()
{
    for (const std::uint32_t rate : {24u, 25u, 30u, 60u, 120u}) {
        Formats formats;
        formats.video = {{1920, 1080, 1, rate}, {640, 480, 1, 15}};
        const auto ranges = fps_ranges(formats);
        require(ranges.size() % 2 == 0, "pairs");
        require(has_range(ranges, std::int32_t(rate), std::int32_t(rate)), "a fixed range at the rate of the largest size");
        require(has_range(ranges, 15, std::int32_t(rate)), "and a variable one that starts at 15");
        require(has_range(ranges, 15, 15), "a fixed range for the 15 fps mode");
        for (std::size_t i = 0; i + 1 < ranges.size(); i += 2) require(ranges[i] <= ranges[i + 1], "low is not above high");
    }
    // The rate of the largest size, not the fastest rate of a small one.
    Formats mixed;
    mixed.video = {{3840, 2160, 1, 24}, {1280, 720, 1, 120}};
    const auto ranges = fps_ranges(mixed);
    require(has_range(ranges, 24, 24) && has_range(ranges, 15, 24) && has_range(ranges, 120, 120),
            "the largest size decides r, every offered rate has a fixed range");
    require(!has_range(ranges, 15, 120) && !has_range(ranges, 30, 30), "and no range the host cannot run");
    // The rate comes out of the minimum frame duration: 1e9 / 33333333 is 30.0000003.
    Formats ntsc;
    ntsc.video = {{1920, 1080, 1, 30}};
    require(has_range(fps_ranges(ntsc), 30, 30) && has_range(fps_ranges(ntsc), 15, 30), "30 fps");
    for (std::size_t i = 1; i < ranges.size() / 2; ++i)
        require(ranges[2 * (i - 1)] < ranges[2 * i] || (ranges[2 * (i - 1)] == ranges[2 * i] && ranges[2 * (i - 1) + 1] < ranges[2 * i + 1]),
                "ascending");
}

// How an output size that the host does not list is made.
void check_derivation()
{
    // The same aspect first, even from a larger mode; the smallest of them.
    const std::vector<Size> natives = {{1280, 720}, {1500, 900}, {640, 480}};
    auto cut = cut_from(natives, {1000, 600});
    require(cut && cut->source == Size({1500, 900}) && !cut->host, "5:3 comes from the 5:3 mode, though 1280x720 is smaller");
    require(cut->window == Window({0, 0, 1500, 900}), "with no crop");
    cut = cut_from(natives, {320, 240});
    require(cut && cut->source == Size({640, 480}) && cut->window == Window({0, 0, 640, 480}), "320x240 is 640x480 scaled");
    // No mode of the aspect covers it: the smallest that does, cut to the aspect and centred.
    cut = cut_from({{1920, 1080}, {3840, 2160}}, {1008, 756});
    require(cut && cut->source == Size({1920, 1080}) && cut->window == Window({240, 0, 1440, 1080}), "a 4:3 window of 1080p");
    cut = cut_from({{3840, 2160}}, {1920, 1440});
    require(cut && cut->window == Window({480, 0, 2880, 2160}), "1920x1440 from 4K");
    cut = cut_from({{800, 1200}}, {400, 300});
    require(cut && cut->window == Window({0, 300, 800, 600}), "a portrait mode is cut at the top and the bottom");
    // The window covers the target and is even, whatever the sizes.
    for (const Size& native : natives)
        for (const Size& target : {Size{1280, 720}, Size{640, 480}, Size{320, 240}, Size{1920, 1440}, Size{100, 100}}) {
            const auto found = cut_from({native}, target);
            if (!found) {
                require(native.width < target.width || native.height < target.height, "only a mode that does not cover is refused");
                continue;
            }
            const auto& window = found->window;
            require(window.width >= target.width && window.height >= target.height, "the window covers the target");
            require(window.x + window.width <= native.width && window.y + window.height <= native.height, "inside the frame");
            require(((window.x | window.y | window.width | window.height) & 1u) == 0, "even, for NV12's chroma");
        }
    require(!cut_from(natives, {1920, 1080}), "no mode covers it");
    require(!cut_from(natives, {321, 240}) && !cut_from(natives, {320, 241}), "an odd size cannot be NV12");
    require(!cut_from(natives, {0, 0}) && !cut_from({}, {320, 240}), "nothing to cut");
}

// The window of a frame that has the shape's aspect ratio, which a thumbnail is cropped to before it is scaled.
void check_centred_window()
{
    require(centred_window({640, 480}, {320, 240}) == Window({0, 0, 640, 480}), "a frame of the aspect is kept whole");
    require(centred_window({1920, 1080}, {4, 3}) == Window({240, 0, 1440, 1080}), "a wide frame loses its sides");
    require(centred_window({640, 480}, {16, 9}) == Window({0, 60, 640, 360}), "a tall frame loses top and bottom");
    require(centred_window({640, 480}, {240, 320}) == Window({140, 0, 360, 480}), "a portrait thumbnail of a landscape frame");
    require(centred_window({640, 482}, {4, 3}) == Window({0, 0, 640, 480}), "an odd margin leaves the offset even");
    require(centred_window({640, 480}, {0, 0}) == Window({0, 0, 640, 480}), "no shape keeps the frame");
    require(centred_window({640, 480}, {2, 1000}).width == 0, "a shape too thin for the frame leaves an empty window");
    for (const Size& frame : {Size{640, 480}, Size{1920, 1080}, Size{3840, 2160}, Size{480, 640}})
        for (const Size& shape : {Size{320, 240}, Size{160, 90}, Size{240, 320}, Size{128, 128}, Size{20, 100}, Size{96, 96}}) {
            const auto window = centred_window(frame, shape);
            require(window.x + window.width <= frame.width && window.y + window.height <= frame.height, "inside the frame");
            require(((window.x | window.y | window.width | window.height) & 1u) == 0, "even");
            // The shape's ratio up to the evening of the window's sides.
            const auto across = std::uint64_t(window.width) * shape.height, down = std::uint64_t(window.height) * shape.width;
            require(across <= down + 2ull * shape.height && down <= across + 2ull * shape.width, "the aspect of the shape");
            require(window.width == frame.width || window.height == frame.height, "the largest window of that aspect");
        }
}

// The camera API and CTS want certain sizes of every camera, and LIMITED cameras
// need their lists nested: the three rules, on each synthetic camera's formats.
void check_cts_subsets()
{
    struct Case {
        const char* name;
        Formats formats;
    };
    const Case cases[] = {{"a phone lens", vectors::phone_lens_formats()},
                          {"a pad's front camera", vectors::pad_front_formats()},
                          {"a laptop", vectors::laptop_formats()},
                          {"the phone's wide lens with a 48 MP array",
                           vectors::formats_of({{640, 480, 1, 60}, {1280, 720, 1, 120}, {1920, 1080, 1, 60}, {3840, 2160, 1, 60}},
                                               {{8064, 6048}, {4032, 3024}, {2016, 1512}, {1008, 756}})}};
    for (const auto& item : cases) {
        const auto sizes = derive_sizes(item.formats);
        const auto has = [](const std::vector<SizeSource>& list, const Size& size) {
            for (const auto& entry : list)
                if (entry.size == size) return true;
            return false;
        };
        // Rule 1: the JPEG sizes that are required are there.
        for (const auto& size : standard_jpeg_sizes) require(has(sizes.jpeg, size), item.name);
        // Rule 2: every JPEG size below the largest video size is a YUV size.
        std::uint64_t largest_video = 0;
        for (const auto& mode : item.formats.video) largest_video = std::max(largest_video, std::uint64_t(mode.width) * mode.height);
        for (const auto& entry : sizes.jpeg)
            if (entry.size.pixels() <= largest_video) require(has(sizes.yuv, entry.size), item.name);
        // Rule 3: every YUV size is a PRIVATE size.
        const auto tables = stream_tables(item.formats, Capabilities{});
        const auto in_tables = [&](std::int32_t format, const Size& size) {
            for (std::size_t i = 0; i + 3 < tables.configurations.size(); i += 4)
                if (tables.configurations[i] == format && tables.configurations[i + 1] == std::int32_t(size.width) &&
                    tables.configurations[i + 2] == std::int32_t(size.height))
                    return true;
            return false;
        };
        for (const auto& entry : sizes.yuv) {
            require(in_tables(pixel_format::yuv_420_888, entry.size), item.name);
            require(in_tables(pixel_format::implementation_defined, entry.size), item.name);
        }
        for (const auto& entry : sizes.jpeg) require(in_tables(pixel_format::blob, entry.size), item.name);
        // The host's own sizes are served by the host, the rest are cut from native modes.
        for (const auto& entry : sizes.yuv) {
            const bool native = std::any_of(item.formats.video.begin(), item.formats.video.end(), [&](const VideoMode& mode) {
                return mode.width == entry.size.width && mode.height == entry.size.height;
            });
            require(entry.host == native, item.name);
            if (!entry.host)
                require(std::any_of(item.formats.video.begin(), item.formats.video.end(), [&](const VideoMode& mode) {
                    return mode.width == entry.source.width && mode.height == entry.source.height;
                }), item.name);
        }
        for (const auto& entry : sizes.jpeg) {
            const bool native = std::find(item.formats.jpeg.begin(), item.formats.jpeg.end(), entry.size) != item.formats.jpeg.end();
            require(entry.host == native, item.name);
        }
    }

    // What the derivation makes of a phone lens.
    const auto phone = derive_sizes(vectors::phone_lens_formats());
    const auto find = [](const std::vector<SizeSource>& list, const Size& size) -> const SizeSource* {
        for (const auto& entry : list)
            if (entry.size == size) return &entry;
        return nullptr;
    };
    const auto* yuv_1008 = find(phone.yuv, {1008, 756});
    require(yuv_1008 && yuv_1008->source == Size({1920, 1080}) && yuv_1008->window == Window({240, 0, 1440, 1080}), "1008x756 from 1080p");
    const auto* yuv_2016 = find(phone.yuv, {2016, 1512});
    require(yuv_2016 && yuv_2016->source == Size({3840, 2160}), "2016x1512 from 4K");
    require(!find(phone.yuv, {4032, 3024}), "no YUV beyond the video modes");
    const auto* jpeg_720 = find(phone.jpeg, {1280, 720});
    require(jpeg_720 && !jpeg_720->host && jpeg_720->source == Size({1280, 720}), "the 720p JPEG is encoded by the HAL from the native frame");
    require(find(phone.jpeg, {4032, 3024})->host, "a native photo size keeps the host's JPEG");

    // A pad's front camera has video up to 1080p: it cannot make 1920x1440 (said once, here).
    const auto pad = derive_sizes(vectors::pad_front_formats());
    require(!find(pad.jpeg, {1920, 1440}) && !find(pad.yuv, {1920, 1440}), "no mode covers 1920x1440 on a pad's front camera");
    require(find(pad.jpeg, {1280, 720}) && find(pad.jpeg, {1008, 756}) && find(pad.yuv, {1008, 756}), "the rest is there");
    require(!meets_concurrent_streams(pad) && meets_concurrent_streams(phone), "so a pad's front camera is not a concurrent one");
    // A laptop's camera never reaches 1920x1440.
    const auto laptop = derive_sizes(vectors::laptop_formats());
    require(meets_concurrent_streams(laptop) && !find(laptop.jpeg, {1920, 1440}), "the 1080p laptop needs only 720p");

    // A camera without any JPEG size of its own still lists the required ones.
    Formats video_only;
    video_only.video = {{1920, 1080, 1, 30}};
    const auto derived = derive_sizes(video_only);
    require(derived.jpeg.size() == 4 && !derived.jpeg[0].host, "JPEG sizes from video alone");
    require(jpeg_max_size(video_only) == 1920 * 1080 * 3 / 2 + 65000 + 8, "the largest derived JPEG sets the maximum");
}

// The camera API's mandatory "preview, record and JPEG video snapshot" combination wants a JPEG at the recording size, which
// is the size of the camera's largest video mode (the media profiles of a layout stop where the host's video stops: 1080p
// at the small tier, 2160p at the large one). The size is made inside the budget the host already has: cut from the video
// mode of that very size (no capture format of its own, no larger buffer than a stream of that mode takes), never larger
// than the camera's largest JPEG, and it appears on no camera whose video stops at 1080p.
void check_record_jpeg()
{
    const auto find = [](const std::vector<SizeSource>& list, const Size& size) -> const SizeSource* {
        for (const auto& entry : list)
            if (entry.size == size) return &entry;
        return nullptr;
    };
    const auto largest_of = [](const std::vector<SizeSource>& list) {
        std::uint64_t pixels = 0;
        for (const auto& entry : list) pixels = std::max(pixels, entry.size.pixels());
        return pixels;
    };

    // The large tier: video up to 4K, stills up to 12 MP.
    const Size uhd{3840, 2160};
    for (const auto& formats : {vectors::phone_lens_formats(), phone_formats()}) {
        const auto sizes = derive_sizes(formats);
        const auto* record = find(sizes.jpeg, uhd);
        require(record != nullptr, "a camera that records 2160p has a JPEG at 2160p");
        require(!record->host && record->source == uhd && record->window == Window({0, 0, uhd.width, uhd.height}),
                "it is the whole frame of the 2160p video mode, scaled by nothing");
        // The camera's largest picture is still the host's, and the buffer bound follows it, not the recording size.
        std::uint64_t host_most = 0;
        for (const auto& size : formats.jpeg) host_most = std::max(host_most, size.pixels());
        require(largest_of(sizes.jpeg) == host_most && jpeg_max_size(formats) == std::int32_t(host_most * 3 / 2 + 65000 + 8),
                "the largest picture stays the host's, and so does the largest JPEG buffer");
        require(find(sizes.yuv, uhd) && find(sizes.yuv, uhd)->host, "and the video size it comes from is a YUV size of the host");
    }
    // A host JPEG of that size stays the host's.
    const auto host_uhd = derive_sizes(vectors::formats_of({{1920, 1080, 1, 30}, {3840, 2160, 1, 30}}, {{4032, 3024}, {3840, 2160}}));
    require(find(host_uhd.jpeg, uhd) && find(host_uhd.jpeg, uhd)->host, "a size the host serves is not made here");

    // 1080p video: nothing beyond what the lists have always held. Every JPEG size is the host's, one of the guaranteed
    // sizes, or 1080p, which is the recording size there.
    for (const auto& formats : {vectors::pad_front_formats(), vectors::laptop_formats(),
                                vectors::formats_of({{640, 480, 1, 30}, {1920, 1080, 1, 60}}, {{4032, 3024}, {2016, 1512}})}) {
        const auto sizes = derive_sizes(formats);
        for (const auto& entry : sizes.jpeg) {
            const bool host = std::find(formats.jpeg.begin(), formats.jpeg.end(), entry.size) != formats.jpeg.end();
            const bool standard = std::find(std::begin(standard_jpeg_sizes), std::end(standard_jpeg_sizes), entry.size) !=
                std::end(standard_jpeg_sizes);
            require(host || standard || entry.size == size_1440p, "no JPEG size appears that a 1080p camera had no use for");
        }
        require(!find(sizes.jpeg, uhd) && !find(sizes.yuv, uhd), "no 2160p on a camera that stops at 1080p");
    }

    // The choice does not depend on the order the host lists its modes in, also with a mode of as many pixels.
    const auto wide_first = vectors::formats_of({{1920, 1080, 1, 30}, {4320, 1920, 1, 30}, {3840, 2160, 1, 30}}, {{4032, 3024}});
    const auto wide_last = vectors::formats_of({{1920, 1080, 1, 30}, {3840, 2160, 1, 30}, {4320, 1920, 1, 30}}, {{4032, 3024}});
    for (const auto* formats : {&wide_first, &wide_last}) {
        const auto sizes = derive_sizes(*formats);
        require(find(sizes.jpeg, uhd) && !find(sizes.jpeg, uhd)->host, "the 2160p JPEG does not depend on the order of equal-area modes");
    }
    // Never larger than the camera's largest JPEG: video of 2160p over stills of 3 MP adds no 2160p picture, and the edge is
    // exact: stills of as many pixels as the video frame (a different shape) allow it, one row fewer do not.
    const auto small_stills = derive_sizes(vectors::formats_of({{1920, 1080, 1, 30}, {3840, 2160, 1, 30}}, {{2016, 1512}}));
    require(!find(small_stills.jpeg, uhd), "a recording size above the camera's stills is not a picture size");
    const auto equal_stills = derive_sizes(vectors::formats_of({{1920, 1080, 1, 30}, {3840, 2160, 1, 30}}, {{3456, 2400}}));
    require(uhd.pixels() == 3456u * 2400u && find(equal_stills.jpeg, uhd) && !find(equal_stills.jpeg, uhd)->host,
            "stills of the recording size's pixel count allow it");
    const auto short_stills = derive_sizes(vectors::formats_of({{1920, 1080, 1, 30}, {3840, 2160, 1, 30}}, {{3840, 2158}}));
    require(!find(short_stills.jpeg, uhd), "stills one row short of the recording size do not");
    // The recording size of the large tier only: any other largest video mode (a square, a portrait or a 1440p one) adds
    // no picture size, which would be a visible change and, for a square or portrait one, a size with no thumbnail of its
    // aspect. Every size such a camera lists is the host's, a guaranteed one or 1080p.
    for (const auto& formats : {vectors::formats_of({{640, 480, 1, 30}, {1024, 1024, 1, 30}}, {{1600, 1200}}),
                                vectors::formats_of({{640, 480, 1, 30}, {1080, 1920, 1, 30}}, {{1600, 1200}}),
                                vectors::formats_of({{1280, 720, 1, 30}, {2560, 1440, 1, 30}}, {{4032, 3024}})}) {
        const Size largest_mode = largest(video_sizes(formats));
        const auto sizes = derive_sizes(formats);
        require(!find(sizes.jpeg, largest_mode), "an unusual largest video mode is not a picture size");
        for (const auto& entry : sizes.jpeg) {
            const bool host = std::find(formats.jpeg.begin(), formats.jpeg.end(), entry.size) != formats.jpeg.end();
            const bool standard = std::find(std::begin(standard_jpeg_sizes), std::end(standard_jpeg_sizes), entry.size) !=
                std::end(standard_jpeg_sizes);
            require(host || standard || entry.size == size_1440p, "no JPEG size appears that the camera had no use for");
        }
    }

    // A video mode that tops out below 30 frames per second sets the minimum frame duration of the JPEGs cut from it (the
    // 3840x2160 JPEG and the 1920x1440 one come from the slow 4K mode, 1920x1080 from the 30 fps one); the host's own
    // photo size keeps the still path's.
    const auto slow = vectors::formats_of({{1920, 1080, 1, 30}, {3840, 2160, 1, 15}}, {{4032, 3024}});
    const auto slow_tables = stream_tables(slow, Capabilities {});
    const auto blob_duration = [&](std::int32_t width, std::int32_t height) -> std::int64_t {
        for (std::size_t i = 0; i < slow_tables.configurations.size() / 4; ++i)
            if (slow_tables.configurations[4 * i] == pixel_format::blob && slow_tables.configurations[4 * i + 1] == width &&
                slow_tables.configurations[4 * i + 2] == height)
                return slow_tables.min_frame_durations[4 * i + 3];
        return -1;
    };
    require(blob_duration(3840, 2160) == 66'666'666, "a JPEG cut from a 15 fps 4K mode runs at 15");
    require(blob_duration(1920, 1440) == 66'666'666, "so does every derived size that only that mode covers");
    require(blob_duration(1920, 1080) == 33'333'333 && blob_duration(4032, 3024) == 33'333'333, "the rest keep the still path's rate");

    // A camera without a video mode has no recording size to add.
    const auto none = derive_sizes(vectors::formats_of({}, {{4032, 3024}}));
    require(none.jpeg.size() == 1 && none.jpeg[0].host && none.jpeg[0].size == Size({4032, 3024}),
            "a camera without video lists its own picture size only (no empty recording size)");
}

// The `concurrent` feature has one definition; the vectors are the application lane's too.
void check_concurrent_feature()
{
    for (const auto& vector : vectors::concurrent_vectors())
        require(vectors::evaluate(vector) == vector.concurrent, vector.name.c_str());

    // The sets the provider reports are the host's, reduced to the cameras that meet the sizes.
    const std::vector<Concurrency> all = {{0, 0, 0b110, 0}, {1, 1, 0b101, 0}, {2, 2, 0b011, 0}};
    const auto reported = reported_concurrent_sets(all, [](std::uint32_t id) { return id != 2; });
    require(reported.size() == 1 && reported[0] == std::vector<std::uint32_t>({0, 1}), "the failing camera leaves the set");
    require(reported_concurrent_sets(all, [](std::uint32_t id) { return id == 0; }).empty(), "a set of one is no set");
    const auto everyone = reported_concurrent_sets(all, [](std::uint32_t) { return true; });
    require(everyone.size() == 1 && everyone[0] == std::vector<std::uint32_t>({0, 1, 2}), "all of them");
}

// The host's concurrency: which cameras stream together, and which exclude one another.
void check_concurrency()
{
    const std::uint32_t front = 1u << 1;
    // The host index is the ID here, and the zoom camera runs with the front one only.
    const std::vector<Concurrency> pair = {{0, 0, front, 0}, {1, 1, 1u << 0, 0}, {2, 2, 0, 0}};
    auto sets = concurrent_sets(pair);
    require(sets.size() == 1 && sets[0] == std::vector<std::uint32_t>({0, 1}), "the zoom and the front camera stream together");

    // One side naming the other is not enough.
    const std::vector<Concurrency> lopsided = {{0, 0, front, 0}, {1, 1, 0, 0}};
    require(concurrent_sets(lopsided).empty(), "both masks must agree");

    // Three cameras that all name one another are one set, not three pairs.
    const std::vector<Concurrency> triple = {{0, 0, 0b110, 0}, {1, 1, 0b101, 0}, {2, 2, 0b011, 0}};
    sets = concurrent_sets(triple);
    require(sets.size() == 1 && sets[0] == std::vector<std::uint32_t>({0, 1, 2}), "a maximal set");

    // A chain a-b, b-c is two sets.
    const std::vector<Concurrency> chain = {{0, 0, 0b010, 0}, {1, 1, 0b101, 0}, {2, 2, 0b010, 0}};
    sets = concurrent_sets(chain);
    require(sets.size() == 2 && sets[0] == std::vector<std::uint32_t>({0, 1}) && sets[1] == std::vector<std::uint32_t>({1, 2}),
            "two pairs");

    // The ID differs from the host's number.
    const std::vector<Concurrency> renumbered = {{0, 4, 1u << 5, 0}, {1, 5, 1u << 4, 0}};
    sets = concurrent_sets(renumbered);
    require(sets.size() == 1 && sets[0] == std::vector<std::uint32_t>({0, 1}), "masks are in the host's numbers");

    require(concurrent_sets(std::vector<Concurrency>{}).empty(), "no cameras, no sets");

    const std::vector<Concurrency> conflicts = {{0, 0, 0, 1u << 3}, {1, 1, 0, 0}, {3, 3, 0, 0}, {4, 4, 1u << 0, 0}};
    auto ids = conflicting_ids(conflicts, 0);
    require(ids == std::vector<std::uint32_t>({3}), "the conflict mask names camera 3");
    ids = conflicting_ids(conflicts, 3);
    require(ids == std::vector<std::uint32_t>({0}), "a conflict is symmetric");
    require(conflicting_ids(conflicts, 1).empty(), "camera 1 conflicts with none");
    require(conflicting_ids(conflicts, 9).empty(), "an unknown camera has no conflicts");
}

// What StaticMetadataTest.validateCapability reads of the key lists: a camera that does not list a capability must lack at
// least one key the capability requires (else "meets all requirements" is a failure), and a region key is there only for a
// kind of region the camera takes.
struct KeyLists {
    std::set<std::int32_t> request;
    std::set<std::int32_t> result;
};

constexpr std::int32_t ae_lock = 0x10002;
constexpr std::int32_t lens_aperture = 0x80000;
constexpr std::int32_t lens_filter_density = 0x80001;
constexpr std::int32_t lens_focus_distance = 0x80003;
constexpr std::int32_t lens_ois_mode = 0x80004;
constexpr std::int32_t sensor_exposure_time = 0xe0000;
constexpr std::int32_t sensor_sensitivity = 0xe0002;

// The keys of the template that a capability reads, as the template (emu_camera_v4l2.json) lists them for every camera.
KeyLists template_lists()
{
    const std::set<std::int32_t> keys = {tag::control_ae_regions, tag::control_af_regions, tag::sensor_frame_duration,
                                         tag::black_level_lock,   ae_lock,                 sensor_exposure_time,
                                         sensor_sensitivity,      lens_aperture,           lens_filter_density,
                                         lens_focus_distance,     lens_ois_mode,           tag::tonemap_mode,
                                         tag::tonemap_curve_red,  tag::tonemap_curve_green, tag::tonemap_curve_blue};
    return {keys, keys};
}

// The characteristics the template lists that a camera's key lists depend on.
std::set<std::int32_t> template_characteristics()
{
    return {tag::tonemap_available_tone_map_modes};
}

// What the HAL leaves of them for this camera: the plan names the ones no V4L2 camera states.
std::set<std::int32_t> stated_characteristics()
{
    auto stated = template_characteristics();
    for (const auto key : unstated_characteristics()) stated.erase(key);
    return stated;
}

KeyLists listed_keys(const abi::Desc& desc, const Capabilities& caps)
{
    auto lists = template_lists();
    for (const auto key : unlisted_keys(desc, caps)) {
        lists.request.erase(key);
        lists.result.erase(key);
    }
    return lists;
}

bool has_all(const std::set<std::int32_t>& keys, const std::vector<std::int32_t>& wanted)
{
    return std::all_of(wanted.begin(), wanted.end(), [&](std::int32_t key) { return keys.count(key) != 0; });
}

// The request keys of MANUAL_SENSOR and the result keys of READ_SENSOR_SETTINGS, with the lens keys of a camera that can focus.
bool meets_manual_sensor(const KeyLists& lists, bool focuser)
{
    std::vector<std::int32_t> keys = {ae_lock, tag::sensor_frame_duration, sensor_exposure_time, sensor_sensitivity,
                                      tag::black_level_lock};
    if (focuser) keys.insert(keys.end(), {lens_aperture, lens_focus_distance, lens_filter_density, lens_ois_mode});
    return has_all(lists.request, keys);
}

bool meets_read_sensor_settings(const KeyLists& lists, bool focuser)
{
    std::vector<std::int32_t> keys = {tag::sensor_frame_duration, sensor_exposure_time, sensor_sensitivity};
    if (focuser) keys.insert(keys.end(), {lens_aperture, lens_focus_distance, lens_filter_density});
    return has_all(lists.result, keys);
}

// The CTS first pass on a phone: the zoom camera is automatic, the front camera has manual exposure and no autofocus, the
// back lenses have both. Each of them lists what it declares, and none lists what it does not.
void check_key_lists()
{
    auto lens = lens_desc();
    auto zoom = lens;
    zoom.lens = abi::lens::zoom;
    auto front = lens;
    front.facing = abi::facing::front;
    front.lens = abi::lens::wide;
    front.flags &= ~abi::feature::autofocus;
    front.min_focus_distance_mm = 0;
    const auto formats = phone_formats();

    require(max_regions_of(lens) == std::array<std::int32_t, 3>({1, 0, 1}), "a camera with autofocus has one AF region");
    require(max_regions_of(front) == std::array<std::int32_t, 3>({1, 0, 0}), "a camera without autofocus has none");

    for (const auto& desc : {lens, zoom, front}) {
        const auto caps = capabilities_of(desc, formats);
        const auto lists = listed_keys(desc, caps);
        for (const bool focuser : {false, true}) {
            require(meets_manual_sensor(lists, focuser) == caps.manual_sensor,
                    "a camera has the keys of MANUAL_SENSOR when it lists it, and only then");
            require(meets_read_sensor_settings(lists, focuser) == caps.read_sensor_settings,
                    "a camera has the keys of READ_SENSOR_SETTINGS when it lists it, and only then");
        }
        const auto regions = max_regions_of(desc);
        require((lists.request.count(tag::control_af_regions) != 0) == (regions[2] > 0), "AF regions are requestable with one");
        require((lists.result.count(tag::control_af_regions) != 0) == (regions[2] > 0), "and reported with one");
        require(lists.request.count(tag::control_ae_regions) != 0 && lists.result.count(tag::control_ae_regions) != 0,
                "AE regions stay");
        if (caps.manual_sensor)
            require(lists.request.count(tag::black_level_lock) != 0 && lists.result.count(tag::black_level_lock) != 0 &&
                        lists.result.count(tag::sensor_frame_duration) != 0,
                    "a manual camera lists the black level lock and the frame duration both ways");
        else
            require(lists.request.count(tag::black_level_lock) == 0 && lists.result.count(tag::black_level_lock) == 0 &&
                        lists.request.count(tag::sensor_frame_duration) == 0 && lists.result.count(tag::sensor_frame_duration) == 0,
                    "an automatic camera lists neither");
        // CaptureRequestTest.testToneMapControl runs for a camera that lists the tone map mode and then reads
        // android.tonemap.maxCurvePoints, a number of curve points that no host camera has (the picture is its own),
        // and the HAL publishes none; so no camera lists the mode or a curve.
        for (const auto key : {tag::tonemap_mode, tag::tonemap_curve_red, tag::tonemap_curve_green, tag::tonemap_curve_blue})
            require(lists.request.count(key) == 0 && lists.result.count(key) == 0, "no tone map key is listed");
        // CameraDeviceTest (the still template) wants the tone map mode request key and the available modes listed together
        // and vice versa, and the HAL's templates carry a tone map mode only for a camera that states modes: so a camera that
        // lists no mode states no modes. (The pairing is of the camera, not of one template: the key lists are the same in
        // every template.)
        require((lists.request.count(tag::tonemap_mode) != 0) == (stated_characteristics().count(tag::tonemap_available_tone_map_modes) != 0),
                "the tone map mode request key and the available tone map modes are listed together, or neither is");
        // The exposure keys are not taken out: an automatic camera reports what it ran (CTS waives them).
        require(lists.result.count(sensor_exposure_time) != 0 && lists.result.count(sensor_sensitivity) != 0,
                "the exposure time and sensitivity stay");
    }
    require(!capabilities_of(zoom, formats).manual_sensor && capabilities_of(front, formats).manual_sensor,
            "the zoom camera is the automatic one, the front camera is manual");
}

} // namespace

// What state_raw_metadata writes, as the HAL's sink for a camera's static metadata would take it.
struct RecordingSink {
    std::map<std::int32_t, std::vector<std::uint8_t>> bytes;
    std::vector<std::int32_t> characteristics;
    std::vector<std::int32_t> results;

    void put_bytes(std::int32_t key, std::span<const std::uint8_t> values) { bytes[key].assign(values.begin(), values.end()); }
    void list_characteristic(std::int32_t key) { characteristics.push_back(key); }
    void list_result(std::int32_t key) { results.push_back(key); }
};

// The metadata that a camera with RAW states and lists (the HAL runs the same function): the lens shading map and the
// hot pixel map with both modes, whether the shading is applied, and the keys of what a RAW camera returns.
void check_raw_metadata()
{
    RecordingSink sink;
    state_raw_metadata(sink);

    const std::vector<std::uint8_t> off_and_on = {map_mode::off, map_mode::on};
    require(sink.bytes.size() == 3, "a RAW camera states three entries");
    require(sink.bytes.at(tag::statistics_info_available_lens_shading_map_modes) == off_and_on,
            "its lens shading map modes are OFF and ON (the device info rejects a RAW camera without ON)");
    require(sink.bytes.at(tag::statistics_info_available_hot_pixel_map_modes) == off_and_on,
            "its hot pixel map modes are OFF and ON (Android requires ON of a RAW camera)");
    require(sink.bytes.at(tag::sensor_info_lens_shading_applied) == std::vector<std::uint8_t>{lens_shading_applied},
            "it states whether its lens shading is applied");
    require(lens_shading_applied == lens_shading_applied_false,
            "the host applies no shading correction to the Bayer data it copies, and its shading map is all ones");

    // Everything stated is a characteristic of the camera, and the hot pixel map is a result it can return.
    for (const auto& [key, values] : sink.bytes)
        require(std::count(sink.characteristics.begin(), sink.characteristics.end(), key) == 1,
                "every entry stated is listed once among the characteristics");
    require(sink.characteristics.size() == sink.bytes.size(), "and nothing else is");
    require(sink.results == std::vector<std::int32_t>{tag::statistics_hot_pixel_map},
            "the hot pixel map is the one result key a RAW camera adds (the template lists the lens shading map already)");

    // The tags are distinct and are not keys that the plan takes out of a camera's lists (unlisted_keys): a RAW camera
    // lists the map modes and the maps of the lens shading and the hot pixels, and no camera has them unlisted.
    const std::set<std::int32_t> raw_keys = {tag::sensor_info_lens_shading_applied,
                                             tag::statistics_hot_pixel_map,
                                             tag::statistics_hot_pixel_map_mode,
                                             tag::statistics_info_available_hot_pixel_map_modes,
                                             tag::statistics_info_available_lens_shading_map_modes,
                                             tag::statistics_lens_shading_map,
                                             tag::statistics_lens_shading_map_mode};
    require(raw_keys.size() == 7, "the seven keys are distinct");
    auto raw = lens_desc();
    auto automatic = raw;
    automatic.lens = abi::lens::zoom;
    auto plain = raw;
    plain.flags &= ~abi::feature::raw;
    plain.bayer_order = abi::bayer::none;
    auto front = raw;
    front.facing = abi::facing::front;
    front.flags &= ~abi::feature::autofocus;
    for (const auto& desc : {raw, automatic, plain, front}) {
        const auto unlisted = unlisted_keys(desc, capabilities_of(desc, phone_formats()));
        for (const auto key : unlisted)
            require(raw_keys.count(key) == 0, "the plan unlists none of the lens shading and hot pixel map keys");
    }
}

int main()
{
    check_ids();
    check_three_lens_host();
    check_concurrency();
    check_capabilities();
    check_tables();
    check_no_bayer_raw_host();
    check_numbers();
    check_fps_ranges();
    check_derivation();
    check_centred_window();
    check_cts_subsets();
    check_record_jpeg();
    check_concurrent_feature();
    check_key_lists();
    check_raw_metadata();
    return 0;
}

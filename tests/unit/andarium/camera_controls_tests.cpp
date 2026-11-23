// SPDX-License-Identifier: GPL-2.0-or-later
//
// How the camera HAL turns a request into the camera's controls and an
// exposure's metadata into the request's results, and how it edits a JPEG that
// the host encoded (android/lineage-24/overlay/camera/hal).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

#include "camera_controls.hpp"
#include "camera_exif.hpp"
#include "require.hpp"

using namespace andarium::camera;
using uml::testing::require;
namespace av = android_value;

namespace {

Limits lens_limits()
{
    Limits limits;
    limits.flags = abi::feature::autofocus | abi::feature::flash | abi::feature::torch | abi::feature::zoom |
        abi::feature::manual_exposure | abi::feature::manual_focus | abi::feature::manual_white_balance;
    limits.array_width = 4000;
    limits.array_height = 3000;
    limits.max_wb_gain_q16 = to_q16(4.0);
    limits.min_focus_distance_mm = 100;
    limits.zoom_min_q16 = to_q16(1.0);
    limits.zoom_max_q16 = to_q16(10.0);
    limits.exposure_min_ns = 25'000;
    limits.exposure_max_ns = 1'000'000'000;
    limits.iso_min = 32;
    limits.iso_max = 3200;
    limits.manual_sensor = true;
    return limits;
}

const ControlWrite* find(const std::vector<ControlWrite>& writes, std::uint32_t id)
{
    for (const auto& write : writes)
        if (write.id == id) return &write;
    return nullptr;
}

// The camera accepted every write.
std::vector<ControlWrite> accepted(ControlPlanner& planner, std::vector<ControlWrite> writes)
{
    for (const auto& write : writes) planner.commit(write);
    return writes;
}

void check_automatic()
{
    ControlPlanner planner(lens_limits());
    Request request;
    auto writes = planner.plan(request);
    require(find(writes, abi::cid::exposure_auto)->value == abi::v4l2_value::exposure_auto &&
                abi::v4l2_value::exposure_auto == 0,
            "AE on is V4L2's automatic exposure, which the host offers");
    require(find(writes, abi::cid::iso_sensitivity_auto)->value == abi::v4l2_value::iso_automatic, "ISO automatic");
    require(find(writes, abi::cid::focus_auto)->value == 1, "continuous picture is continuous AF");
    require(find(writes, abi::cid::auto_white_balance)->value == 1, "AWB auto");
    require(find(writes, abi::cid::exposure_ns) == nullptr, "no manual exposure in AE");
    require(find(writes, abi::cid::lock_3a)->value == 0, "no locks");
    require(find(writes, abi::cid::zoom_q16)->value == std::int64_t(to_q16(1.0)), "zoom 1.0");
    require(find(writes, abi::cid::flash_led_mode)->value == abi::v4l2_value::flash_led_off, "torch off");
    require(find(writes, abi::cid::still_flash)->value == 0, "still flash off");
    require(find(writes, abi::cid::test_pattern)->value == 0, "no test pattern");

    // A write the camera has not accepted is planned again; one it accepted is not.
    require(planner.plan(request).size() == writes.size(), "nothing is remembered before it is committed");
    accepted(planner, writes);
    require(planner.plan(request).empty(), "an unchanged request writes nothing");
    // A refused value does not retire the control: a later request writes it again.
    planner.reset();
    writes = planner.plan(request);
    for (const auto& write : writes)
        if (write.id != abi::cid::exposure_auto) planner.commit(write);
    require(planner.plan(request).size() == 1 && planner.plan(request)[0].id == abi::cid::exposure_auto,
            "only the refused write is planned again");
    // A reset forgets.
    planner.reset();
    require(!planner.plan(request).empty(), "after a reset everything is written");
}

void check_manual()
{
    ControlPlanner planner(lens_limits());
    Request request;
    request.ae_mode = av::ae_off;
    request.exposure_ns = 8'333'333;
    request.sensitivity = 400;
    request.af_mode = av::af_off;
    request.focus_distance = 5.0f;
    request.awb_mode = av::awb_off;
    request.wb_gains = {2.0f, 1.0f, 1.0f, 1.5f};
    auto writes = planner.plan(request);
    require(find(writes, abi::cid::exposure_auto)->value == abi::v4l2_value::exposure_manual, "manual exposure");
    require(find(writes, abi::cid::iso_sensitivity_auto)->value == abi::v4l2_value::iso_manual, "manual ISO");
    require(find(writes, abi::cid::exposure_ns)->value == 8'333'333, "the exposure in nanoseconds");
    require(find(writes, abi::cid::iso)->value == 400, "the ISO");
    require(find(writes, abi::cid::auto_exposure_bias) == nullptr, "no bias in manual exposure");
    require(find(writes, abi::cid::focus_auto)->value == 0, "AF off");
    require(find(writes, abi::cid::focus_absolute)->value == 5000, "five dioptres in thousandths");
    require(find(writes, abi::cid::auto_white_balance)->value == 0, "manual white balance");
    require(find(writes, abi::cid::wb_gain_red)->value == std::int64_t(to_q16(2.0)), "red gain");
    require(find(writes, abi::cid::wb_gain_blue)->value == std::int64_t(to_q16(1.5)), "blue gain");

    // Out-of-range values are clamped to the camera's.
    request.exposure_ns = 10;
    request.sensitivity = 100000;
    request.focus_distance = 50.0f;
    request.wb_gains = {9.0f, 1.0f, 1.0f, 0.1f};
    writes = planner.plan(request);
    require(find(writes, abi::cid::exposure_ns)->value == 25'000, "exposure clamped up");
    require(find(writes, abi::cid::iso)->value == 3200, "ISO clamped down");
    require(find(writes, abi::cid::focus_absolute)->value == 10000, "focus clamped to the minimum distance");
    require(find(writes, abi::cid::wb_gain_red)->value == std::int64_t(to_q16(4.0)), "gain clamped to the maximum");
    require(find(writes, abi::cid::wb_gain_blue)->value == std::int64_t(to_q16(1.0)), "gain at least one");

    // A camera without manual sensor (the zoom camera) stays automatic.
    auto zoom = lens_limits();
    zoom.manual_sensor = false;
    ControlPlanner automatic(zoom);
    request.ae_mode = av::ae_off;
    writes = automatic.plan(request);
    require(find(writes, abi::cid::exposure_auto)->value == abi::v4l2_value::exposure_auto, "no manual exposure");
    require(find(writes, abi::cid::exposure_ns) == nullptr, "no exposure write");

    // The host's minimum is one: a description that says zero clamps to one, and a
    // maximum below the minimum does not invert the range.
    auto loose = lens_limits();
    loose.iso_min = 0;
    loose.exposure_min_ns = 0;
    ControlPlanner clamps(loose);
    require(clamps.clamp_iso(0) == 1 && clamps.clamp_exposure(0) == 1, "the minimum is one");
    loose.iso_min = 200;
    loose.iso_max = 100;
    require(ControlPlanner(loose).clamp_iso(5000) == 200, "an inverted range holds the minimum");
}

void check_focus_and_locks()
{
    ControlPlanner planner(lens_limits());
    Request request;
    request.af_mode = av::af_auto;
    request.af_trigger = av::af_trigger_start;
    auto writes = planner.plan(request);
    require(find(writes, abi::cid::focus_auto)->value == 0, "single AF is not continuous");
    require(find(writes, abi::cid::auto_focus_start) != nullptr, "the trigger starts a scan");

    require(planner.af_engaged(), "the trigger engages the scan");
    request.af_trigger = av::af_trigger_idle;
    writes = planner.plan(request);
    require(find(writes, abi::cid::auto_focus_start) == nullptr, "an idle trigger starts nothing");
    require(planner.af_engaged(), "the scan stays engaged");

    request.af_trigger = av::af_trigger_cancel;
    writes = planner.plan(request);
    require(find(writes, abi::cid::auto_focus_stop) != nullptr, "cancel stops it");
    require(!planner.af_engaged(), "and ends it");

    // A new mode starts over, and a manual lens is told where to be though a scan
    // may have moved it since the request last said so.
    request.af_mode = av::af_off;
    request.af_trigger = av::af_trigger_idle;
    request.focus_distance = 2.0f;
    accepted(planner, planner.plan(request));
    request.af_mode = av::af_auto;
    accepted(planner, planner.plan(request));
    request.af_trigger = av::af_trigger_start;
    accepted(planner, planner.plan(request));
    require(planner.af_engaged(), "engaged again");
    request.af_mode = av::af_off;
    request.af_trigger = av::af_trigger_idle;
    writes = planner.plan(request);
    require(!planner.af_engaged(), "a change of mode ends the scan");
    require(find(writes, abi::cid::focus_absolute) != nullptr && find(writes, abi::cid::focus_absolute)->value == 2000,
            "the manual lens is moved again after a scan");
    require(find(writes, abi::cid::auto_focus_start) == nullptr, "and nothing else is pressed");

    // The press and the cancel are the last write of the request: the scan runs after
    // the region, the locks, the white balance and the zoom that the same request states.
    for (const auto trigger : {av::af_trigger_start, av::af_trigger_cancel}) {
        ControlPlanner ordered(lens_limits());
        Request full;
        full.af_mode = av::af_auto;
        full.af_trigger = trigger;
        full.af_region = {1000, 750, 3000, 2250, 500};
        full.ae_region = {500, 500, 1500, 1500, 300};
        full.ae_lock = true;
        full.awb_lock = true;
        full.awb_mode = av::awb_daylight;
        full.zoom_ratio = 2.0f;
        full.flash_mode = av::flash_torch;
        full.video_stabilization = true;
        writes = ordered.plan(full);
        const auto scan = trigger == av::af_trigger_start ? abi::cid::auto_focus_start : abi::cid::auto_focus_stop;
        require(writes.size() > 8 && writes.back().id == scan, "the press or the cancel is written last");
        for (const auto id : {abi::cid::af_region, abi::cid::lock_3a, abi::cid::auto_n_preset_white_balance,
                              abi::cid::ae_region, abi::cid::zoom_q16, abi::cid::flash_led_mode,
                              abi::cid::image_stabilization})
            require(find(writes, id) != nullptr && find(writes, id) != &writes.back(),
                    "every other setting of the request is written before it");
        std::size_t scans = 0;
        for (const auto& write : writes) scans += write.id == scan ? 1 : 0;
        require(scans == 1, "and written once");
    }

    // In continuous AF a trigger locks focus until it is cancelled.
    ControlPlanner continuous(lens_limits());
    Request pic;
    pic.af_trigger = av::af_trigger_start;
    writes = accepted(continuous, continuous.plan(pic));
    require(find(writes, abi::cid::lock_3a)->value == abi::v4l2_value::lock_focus, "focus locked by the trigger");
    require(!continuous.af_engaged(), "the scanning flag is for the scanning modes");
    pic.af_trigger = av::af_trigger_idle;
    writes = accepted(continuous, continuous.plan(pic));
    require(find(writes, abi::cid::lock_3a) == nullptr, "the lock holds");
    pic.af_trigger = av::af_trigger_cancel;
    writes = continuous.plan(pic);
    require(find(writes, abi::cid::lock_3a)->value == 0, "cancel releases it");

    Request locked;
    locked.ae_lock = true;
    locked.awb_lock = true;
    ControlPlanner locks(lens_limits());
    require(find(locks.plan(locked), abi::cid::lock_3a)->value ==
                (abi::v4l2_value::lock_exposure | abi::v4l2_value::lock_white_balance),
            "exposure and white balance locks");

    // A fixed-focus camera writes no focus controls.
    auto fixed = lens_limits();
    fixed.flags &= ~abi::feature::autofocus;
    ControlPlanner nofocus(fixed);
    writes = nofocus.plan(Request{});
    require(find(writes, abi::cid::focus_auto) == nullptr && find(writes, abi::cid::af_region) == nullptr, "fixed focus");
}

void check_other_controls()
{
    ControlPlanner planner(lens_limits());
    Request request;
    request.ae_compensation_steps = 3;
    request.awb_mode = av::awb_daylight;
    request.zoom_ratio = 25.0f;
    request.flash_mode = av::flash_torch;
    request.ae_mode = av::ae_on_auto_flash;
    request.test_pattern = av::pattern_solid_color;
    request.rotate_and_crop = av::rotate_auto;
    request.jpeg_quality = 120;
    request.video_stabilization = true;
    auto writes = planner.plan(request);
    require(find(writes, abi::cid::auto_exposure_bias)->value == 1000, "three thirds of a stop is a thousand");
    require(find(writes, abi::cid::auto_white_balance)->value == 0 && find(writes, abi::cid::auto_n_preset_white_balance)->value == 6,
            "the daylight preset");
    require(find(writes, abi::cid::zoom_q16)->value == std::int64_t(to_q16(10.0)), "zoom clamped to the camera's");
    require(find(writes, abi::cid::flash_led_mode)->value == abi::v4l2_value::flash_led_torch, "torch");
    require(find(writes, abi::cid::still_flash)->value == 1, "automatic flash for the still");
    require(find(writes, abi::cid::test_pattern)->value == 1, "a test pattern is black");
    require(find(writes, abi::cid::rotate_and_crop)->value == abi::v4l2_value::rotate_automatic, "automatic rotation");
    require(find(writes, abi::cid::image_stabilization)->value == 1, "stabilization");
    require(find(writes, abi::cid::jpeg_compression_quality)->value == 100, "quality clamped");

    request.ae_mode = av::ae_on_always_flash;
    request.flash_mode = av::flash_off;
    planner.reset();
    require(find(planner.plan(request), abi::cid::still_flash)->value == 2, "always flash");
    request.ae_mode = av::ae_on;
    request.flash_mode = av::flash_single;
    planner.reset();
    require(find(planner.plan(request), abi::cid::still_flash)->value == 2, "a single flash");

    // The presets the host offers (2, 3, 6, 8, 9); it skips the warm fluorescent and
    // the twilight ones.
    require(awb_preset(av::awb_incandescent) == 2 && awb_preset(av::awb_fluorescent) == 3 && awb_preset(av::awb_daylight) == 6 &&
                awb_preset(av::awb_cloudy_daylight) == 8 && awb_preset(av::awb_shade) == 9,
            "presets");
    require(!awb_preset(av::awb_warm_fluorescent) && !awb_preset(av::awb_twilight), "no preset the host skips");
    require(!awb_preset(av::awb_auto) && !awb_preset(av::awb_off), "auto and off are not presets");
    require(rotate_value(av::rotate_none) == 0 && rotate_value(av::rotate_90) == 1 && rotate_value(av::rotate_180) == 2 &&
                rotate_value(av::rotate_270) == 3,
            "rotation values");

    // A camera without manual white balance has no presets: the preset is automatic.
    auto zoom_camera = lens_limits();
    zoom_camera.flags &= ~abi::feature::manual_white_balance;
    ControlPlanner zoom_planner(zoom_camera);
    writes = zoom_planner.plan(request);
    require(find(writes, abi::cid::auto_white_balance)->value == 1 && find(writes, abi::cid::auto_n_preset_white_balance) == nullptr,
            "no presets without manual white balance");

    // A camera without a torch or flash writes no flash controls.
    auto plain = lens_limits();
    plain.flags &= ~(abi::feature::flash | abi::feature::torch | abi::feature::zoom);
    ControlPlanner without(plain);
    writes = without.plan(request);
    require(find(writes, abi::cid::flash_led_mode) == nullptr && find(writes, abi::cid::still_flash) == nullptr &&
                find(writes, abi::cid::zoom_q16) == nullptr,
            "no flash, no zoom");
}

void check_regions()
{
    ControlPlanner planner(lens_limits());
    Request request;
    request.ae_region = {1000, 750, 3000, 2250, 500};
    auto writes = planner.plan(request);
    const auto* region = find(writes, abi::cid::ae_region);
    require(region != nullptr && region->is_region, "an AE region");
    require(region->region[0] == to_q16(0.25) && region->region[1] == to_q16(0.25), "origin");
    require(region->region[2] == to_q16(0.5) && region->region[3] == to_q16(0.5), "size");
    require(region->region[4] == 500, "weight");
    require(find(planner.plan(request), abi::cid::ae_region) != nullptr, "a region the camera has not accepted is written again");
    accepted(planner, writes);
    require(find(planner.plan(request), abi::cid::ae_region) == nullptr, "unchanged");

    request.ae_region.weight = 0;
    region = find(planner.plan(request), abi::cid::ae_region);
    require(region != nullptr && region->region == std::array<std::uint32_t, abi::region_words>{}, "weight 0 clears");

    // Regions outside the array are clipped, and an empty one clears.
    const auto words = region_words({-100, -100, 8000, 6000, 1500}, lens_limits());
    require(words[0] == 0 && words[2] == to_q16(1.0) && words[3] == to_q16(1.0) && words[4] == 1000, "clipped");
    require(region_words({10, 10, 10, 50, 100}, lens_limits())[4] == 0, "an empty region is cleared");
}

void check_results()
{
    abi::Meta meta{};
    meta.exposure_ns = 10'000'000;
    meta.frame_duration_ns = 33'333'333;
    meta.iso = 125;
    meta.ae_state = av::ae_state_converged;
    meta.af_state = av::af_state_focused_locked;
    meta.awb_state = av::awb_state_converged;
    meta.lens_position_q16 = to_q16(2.5);
    meta.aperture_x100 = 180;
    meta.focal_length_um = 5100;
    meta.zoom_ratio_q16 = to_q16(1.5);
    meta.wb_gain_q16[0] = to_q16(1.8);
    meta.wb_gain_q16[1] = to_q16(1.0);
    meta.wb_gain_q16[2] = to_q16(1.4);
    meta.flash_state = 0;

    Request request;
    auto result = result_of(meta, request, true, false);
    require(result.ae_state == av::ae_state_converged && result.af_state == av::af_state_focused_locked &&
                result.awb_state == av::awb_state_converged,
            "the host's states");
    require(result.exposure_ns == 10'000'000 && result.sensitivity == 125 && result.frame_duration_ns == 33'333'333, "what applied");
    require(result.focus_distance == 2.5f && result.aperture == 1.8f && result.focal_length_mm == 5.1f, "optics");
    require(result.zoom_ratio == 1.5f, "zoom");
    require(result.wb_gains[0] > 1.79f && result.wb_gains[1] == result.wb_gains[2] && result.wb_gains[3] > 1.39f, "gains");
    require(result.flash_state == av::flash_state_ready, "a flash that has no state is ready");

    request.ae_precapture_trigger = av::ae_trigger_start;
    require(result_of(meta, request, true, false).ae_state == av::ae_state_precapture, "a precapture trigger reports PRECAPTURE");
    request.ae_precapture_trigger = av::ae_trigger_idle;
    request.ae_mode = av::ae_off;
    request.af_mode = av::af_off;
    request.awb_mode = av::awb_off;
    result = result_of(meta, request, false, false);
    require(result.ae_state == av::ae_state_inactive && result.af_state == av::af_state_inactive &&
                result.awb_state == av::awb_state_inactive,
            "manual modes are inactive");
    require(result.flash_state == av::flash_state_unavailable, "no flash");
    meta.zoom_ratio_q16 = 0;
    require(result_of(meta, request, false, false).zoom_ratio == 1.0f, "a record without zoom says 1.0");

    // AF in the scanning modes: inactive until a trigger, ACTIVE_SCAN for the request
    // that triggers, the host's state while the scan is engaged, inactive again
    // after a cancel (not engaged); the continuous modes are the host's state.
    request = Request{};
    request.af_mode = av::af_auto;
    meta.af_state = av::af_state_focused_locked; // the host locks at once with FOCUS_AUTO off
    require(result_of(meta, request, true, false).af_state == av::af_state_inactive, "inactive before the first trigger");
    request.af_trigger = av::af_trigger_start;
    require(result_of(meta, request, true, true).af_state == av::af_state_active_scan, "the triggering request is scanning");
    request.af_trigger = av::af_trigger_idle;
    require(result_of(meta, request, true, true).af_state == av::af_state_focused_locked, "then the host's state");
    meta.af_state = av::af_state_not_focused_locked;
    require(result_of(meta, request, true, true).af_state == av::af_state_not_focused_locked, "including a failed scan");
    request.af_trigger = av::af_trigger_cancel;
    require(result_of(meta, request, true, false).af_state == av::af_state_inactive, "inactive after a cancel");
    request.af_mode = av::af_macro;
    request.af_trigger = av::af_trigger_idle;
    require(result_of(meta, request, true, false).af_state == av::af_state_inactive, "macro scans like auto");
    request.af_mode = av::af_continuous_picture;
    meta.af_state = av::af_state_passive_focused;
    require(result_of(meta, request, true, false).af_state == av::af_state_passive_focused, "continuous focus is the host's state");
}

void check_exif()
{
    std::vector<std::uint8_t> jpeg(80'000, 0xAA);
    const std::uint32_t offset = 20;
    jpeg[offset - 4] = 0xFF;
    jpeg[offset - 3] = 0xE1;
    std::fill(jpeg.begin() + offset, jpeg.begin() + offset + abi::exif_gap_length, 0);
    const std::array<std::uint8_t, 8> exif = {'E', 'x', 'i', 'f', 0, 0, 'I', 'I'};
    require(place_exif(jpeg, offset, abi::exif_gap_length, exif), "the Exif data fits");
    require(std::memcmp(jpeg.data() + offset, exif.data(), exif.size()) == 0, "it is at the start of the gap");
    require(jpeg[offset + exif.size()] == 0 && jpeg[offset + abi::exif_gap_length - 1] == 0, "the rest is zero");
    require(jpeg[offset + abi::exif_gap_length] == 0xAA && jpeg[offset - 5] == 0xAA, "nothing outside the gap changes");

    // A shorter one over an older, longer one clears the remainder.
    std::fill(jpeg.begin() + offset, jpeg.begin() + offset + abi::exif_gap_length, 0x55);
    require(place_exif(jpeg, offset, abi::exif_gap_length, exif), "again");
    require(jpeg[offset + exif.size() + 100] == 0, "the old bytes are gone");

    std::vector<std::uint8_t> big(abi::exif_gap_length + 1, 1);
    auto copy = jpeg;
    require(!place_exif(copy, offset, abi::exif_gap_length, big) && copy == jpeg, "too much Exif data changes nothing");
    require(!place_exif(copy, offset, abi::exif_gap_length - 1, exif), "a gap of another length is not the device's");
    require(!place_exif(copy, 79'000, abi::exif_gap_length, exif), "a gap that overruns the buffer");
    require(!place_exif(copy, 2, abi::exif_gap_length, exif), "no room for the segment header");
    copy[offset - 3] = 0xE0;
    require(!place_exif(copy, offset, abi::exif_gap_length, exif), "the segment must be an APP1");
    require(place_exif(jpeg, offset, abi::exif_gap_length, {}), "an empty Exif clears the gap");

    std::vector<std::uint8_t> buffer(1000, 0);
    require(write_blob_header(buffer, 900), "the header fits");
    JpegBlobHeader header{};
    std::memcpy(&header, buffer.data() + buffer.size() - sizeof(header), sizeof(header));
    require(header.blob_id == 0x00FF && header.jpeg_size == 900, "the blob header");
    require(!write_blob_header(buffer, 993), "a picture that leaves no room for the header");
    require(write_blob_header(buffer, 992), "the largest picture");
    std::vector<std::uint8_t> tiny(4, 0);
    require(!write_blob_header(tiny, 0), "a buffer smaller than the header");
}

// The focal length in 35 mm film follows from the camera's stated focal length
// and physical sensor size: the pair that the field of view gives. The sizes in
// the tests are the ones the host states (darwin/device/camera/camera_description.cpp):
// the pixel array at a nominal pitch, and the focal length that gives the
// horizontal field of view the host reports for a sensor that wide. The value is
// then the field of view's own equivalent whatever the pitch; a camera that
// states no pair has no value, and the tag is left out (a 6.765 mm lens made
// 29272 from a size of zero).
abi::Desc host_description(std::uint32_t width_px, std::uint32_t height_px, double horizontal_fov_degrees,
                           double pitch_um = 1.2)
{
    abi::Desc desc{};
    desc.pixel_array_width = width_px;
    desc.pixel_array_height = height_px;
    const double width_um = width_px * pitch_um;
    const double height_um = height_px * pitch_um;
    desc.sensor_width_um = std::uint32_t(std::lround(width_um));
    desc.sensor_height_um = std::uint32_t(std::lround(height_um));
    desc.focal_length_um =
        std::uint32_t(std::lround((width_um / 2.0) / std::tan(horizontal_fov_degrees * std::numbers::pi / 360.0)));
    return desc;
}

// The 35 mm equivalent of a field of view, from the film's diagonal and the aspect ratio alone.
double equivalent_of_field_of_view(std::uint32_t width_px, std::uint32_t height_px, double horizontal_fov_degrees)
{
    const double half_width = std::tan(horizontal_fov_degrees * std::numbers::pi / 360.0);
    return 43.27 / (2.0 * half_width * std::hypot(1.0, double(height_px) / double(width_px)));
}

std::optional<std::uint16_t> tag_of(const abi::Desc& desc)
{
    const auto optics = stated_optics(desc);
    return focal_length_in_35mm_film(optics.focal_length_mm, optics.sensor_width_mm, optics.sensor_height_mm);
}

void check_focal_length_35mm()
{
    struct Camera {
        const char* name;
        std::uint32_t width, height;
        double fov;
    };
    // A wide back camera, a front one, an ultra wide and a telephoto, at the sizes of their pixel arrays.
    const Camera cameras[] = {{"main", 4032, 3024, 63.4}, {"front", 3088, 2316, 70.0}, {"ultra wide", 4032, 3024, 106.0},
                              {"telephoto", 4032, 3024, 25.0}, {"wide 16:9", 3840, 2160, 63.4}};
    for (const Camera& camera : cameras) {
        const double expected = equivalent_of_field_of_view(camera.width, camera.height, camera.fov);
        for (const double pitch : {0.8, 1.2, 2.3}) {
            const auto tag = tag_of(host_description(camera.width, camera.height, camera.fov, pitch));
            require(tag.has_value(), camera.name);
            require(std::abs(double(*tag) - expected) <= 1.0, "the tag is the field of view's own 35 mm equivalent");
        }
    }
    require(tag_of(host_description(4032, 3024, 63.4)) == std::optional<std::uint16_t>(28), "the main camera is a 28 mm one");
    require(tag_of(host_description(3088, 2316, 70.0)) == std::optional<std::uint16_t>(25), "the front camera is a 25 mm one");
    require(tag_of(host_description(4032, 3024, 106.0)) == std::optional<std::uint16_t>(13), "the ultra wide is a 13 mm one");

    // A description that states no pair states none: every member is zero, and there is no tag
    // (the lens's own focal length, 6.765 mm, on a size of zero used to make 29272).
    for (int missing = 0; missing < 3; ++missing) {
        abi::Desc desc = host_description(4032, 3024, 63.4);
        (missing == 0 ? desc.focal_length_um : missing == 1 ? desc.sensor_width_um : desc.sensor_height_um) = 0;
        const auto optics = stated_optics(desc);
        require(optics.focal_length_mm == 0.0f && optics.sensor_width_mm == 0.0f && optics.sensor_height_mm == 0.0f,
                "a pair with a member missing is no pair");
        require(!tag_of(desc), "no pair: no tag");
    }
    abi::Desc unstated = host_description(4032, 3024, 63.4);
    unstated.sensor_width_um = unstated.sensor_height_um = 0;
    unstated.focal_length_um = 6765;
    require(!tag_of(unstated), "a lens focal length with no sensor size: no tag (not 29272)");

    // The arithmetic itself: the 35 mm frame is the film, half the frame doubles the focal length.
    require(focal_length_in_35mm_film(50.0f, 36.0f, 24.0f) == std::optional<std::uint16_t>(50), "a 35 mm frame is the film itself");
    require(focal_length_in_35mm_film(25.0f, 18.0f, 12.0f) == std::optional<std::uint16_t>(50), "half the frame doubles the focal length");
    require(focal_length_in_35mm_film(1000.0f, 36.0f, 24.0f) == std::optional<std::uint16_t>(1000), "a long lens on film");

    // What the pair rules out.
    require(!focal_length_in_35mm_film(6.765f, 0.0f, 0.0f), "no sensor size: no tag");
    require(!focal_length_in_35mm_film(6.765f, 4.8f, 0.0f), "one side missing: no tag");
    require(!focal_length_in_35mm_film(6.765f, 0.0f, 3.6f), "the other side missing: no tag");
    require(!focal_length_in_35mm_film(6.765f, -4.8f, 3.6f), "a negative size: no tag");
    require(!focal_length_in_35mm_film(0.0f, 4.8f, 3.6f), "no focal length: no tag");
    require(!focal_length_in_35mm_film(-1.0f, 4.8f, 3.6f), "a negative focal length: no tag");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    require(!focal_length_in_35mm_film(nan, 4.8f, 3.6f) && !focal_length_in_35mm_film(6.765f, nan, 3.6f), "not a number: no tag");
    require(!focal_length_in_35mm_film(inf, 4.8f, 3.6f) && !focal_length_in_35mm_film(6.765f, 4.8f, inf), "infinite: no tag");
    // The tag is 16 bits: a result beyond it, or one that rounds to nothing, is no focal length either.
    require(!focal_length_in_35mm_film(100.0f, 0.001f, 0.001f), "a result beyond 65535: no tag");
    require(!focal_length_in_35mm_film(0.001f, 36.0f, 24.0f), "a result that rounds to zero: no tag");
}

} // namespace

int main()
{
    check_automatic();
    check_manual();
    check_focus_and_locks();
    check_other_controls();
    check_regions();
    check_results();
    check_exif();
    check_focal_length_35mm();
    return 0;
}

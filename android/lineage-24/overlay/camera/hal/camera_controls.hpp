// SPDX-License-Identifier: Apache-2.0
//
// How a capture request's settings become the camera's V4L2 controls, and how
// the metadata of an exposure becomes the request's results. The planner keeps
// what it last wrote so that a request writes only what changed (the camera's
// controls apply as soon as the host can apply them, and the metadata record
// says what applied). Android's enumerations are written here as numbers; the
// HAL's glue checks each against the platform's header at compile time.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "camera_abi.hpp"
#include "camera_plan.hpp"

namespace andarium::camera {

namespace android_value {
// ANDROID_CONTROL_AE_MODE_*
constexpr std::uint8_t ae_off = 0, ae_on = 1, ae_on_auto_flash = 2, ae_on_always_flash = 3;
// ANDROID_CONTROL_AF_MODE_*
constexpr std::uint8_t af_off = 0, af_auto = 1, af_macro = 2, af_continuous_video = 3, af_continuous_picture = 4;
// ANDROID_CONTROL_AF_TRIGGER_*
constexpr std::uint8_t af_trigger_idle = 0, af_trigger_start = 1, af_trigger_cancel = 2;
// ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_*
constexpr std::uint8_t ae_trigger_idle = 0, ae_trigger_start = 1, ae_trigger_cancel = 2;
// ANDROID_CONTROL_AWB_MODE_*
constexpr std::uint8_t awb_off = 0, awb_auto = 1, awb_incandescent = 2, awb_fluorescent = 3, awb_warm_fluorescent = 4,
                       awb_daylight = 5, awb_cloudy_daylight = 6, awb_twilight = 7, awb_shade = 8;
// ANDROID_FLASH_MODE_*
constexpr std::uint8_t flash_off = 0, flash_single = 1, flash_torch = 2;
// ANDROID_SENSOR_TEST_PATTERN_MODE_*
constexpr std::uint8_t pattern_off = 0, pattern_solid_color = 1, pattern_black = 5;
// ANDROID_SCALER_ROTATE_AND_CROP_*
constexpr std::uint8_t rotate_none = 0, rotate_90 = 1, rotate_180 = 2, rotate_270 = 3, rotate_auto = 4;
// ANDROID_CONTROL_AE_STATE_*
constexpr std::uint32_t ae_state_inactive = 0, ae_state_searching = 1, ae_state_converged = 2, ae_state_locked = 3,
                        ae_state_flash_required = 4, ae_state_precapture = 5;
// ANDROID_CONTROL_AF_STATE_*
constexpr std::uint32_t af_state_inactive = 0, af_state_passive_scan = 1, af_state_passive_focused = 2,
                        af_state_active_scan = 3, af_state_focused_locked = 4, af_state_not_focused_locked = 5,
                        af_state_passive_unfocused = 6;
// ANDROID_CONTROL_AWB_STATE_*
constexpr std::uint32_t awb_state_inactive = 0, awb_state_searching = 1, awb_state_converged = 2, awb_state_locked = 3;
// ANDROID_FLASH_STATE_*
constexpr std::uint32_t flash_state_unavailable = 0, flash_state_ready = 2;
} // namespace android_value

// A region in the pixels of the active array and a weight, as Android states it.
struct Region {
    std::int32_t x_min = 0, y_min = 0, x_max = 0, y_max = 0, weight = 0;
    friend bool operator==(const Region&, const Region&) = default;
};

// The settings of one request that reach the camera.
struct Request {
    std::uint8_t ae_mode = android_value::ae_on;
    bool ae_lock = false;
    std::int32_t ae_compensation_steps = 0; // in units of the EV step
    std::int64_t exposure_ns = 0;
    std::int32_t sensitivity = 0;
    std::uint8_t ae_precapture_trigger = android_value::ae_trigger_idle;
    std::uint8_t af_mode = android_value::af_continuous_picture;
    std::uint8_t af_trigger = android_value::af_trigger_idle;
    float focus_distance = 0.0f; // dioptres
    std::uint8_t awb_mode = android_value::awb_auto;
    bool awb_lock = false;
    std::array<float, 4> wb_gains = {1.0f, 1.0f, 1.0f, 1.0f}; // red, green even, green odd, blue
    float zoom_ratio = 1.0f;
    std::uint8_t flash_mode = android_value::flash_off;
    std::uint8_t test_pattern = android_value::pattern_off;
    std::uint8_t rotate_and_crop = android_value::rotate_none;
    std::uint8_t jpeg_quality = 95;
    bool video_stabilization = false;
    Region ae_region;
    Region af_region;
};

// What the camera accepts (from its description).
struct Limits {
    std::uint32_t flags = 0;
    std::uint32_t array_width = 0;
    std::uint32_t array_height = 0;
    std::uint32_t max_wb_gain_q16 = 0;
    std::uint32_t min_focus_distance_mm = 0;
    std::uint32_t zoom_min_q16 = 0;
    std::uint32_t zoom_max_q16 = 0;
    std::uint64_t exposure_min_ns = 0;
    std::uint64_t exposure_max_ns = 0;
    std::uint32_t iso_min = 0;
    std::uint32_t iso_max = 0;
    bool manual_sensor = false;
    bool has_jpeg = true;

    static Limits of(const abi::Desc& desc, const Capabilities& caps, const Size& array) noexcept
    {
        Limits limits;
        limits.flags = desc.flags;
        limits.array_width = array.width;
        limits.array_height = array.height;
        limits.max_wb_gain_q16 = desc.max_wb_gain_q16;
        limits.min_focus_distance_mm = desc.min_focus_distance_mm;
        limits.zoom_min_q16 = desc.zoom_min_q16;
        limits.zoom_max_q16 = desc.zoom_max_q16;
        limits.exposure_min_ns = desc.exposure_min_ns;
        limits.exposure_max_ns = desc.exposure_max_ns;
        limits.iso_min = desc.iso_min;
        limits.iso_max = desc.iso_max;
        limits.manual_sensor = caps.manual_sensor;
        return limits;
    }
};

// One control to write: a value, or the five words of a region.
struct ControlWrite {
    std::uint32_t id = 0;
    std::int64_t value = 0;
    std::array<std::uint32_t, abi::region_words> region{};
    bool is_region = false;
    friend bool operator==(const ControlWrite&, const ControlWrite&) = default;
};

// The V4L2 white-balance preset (V4L2_WHITE_BALANCE_*) of an Android AWB mode, or
// nothing for a mode that is not a preset the host offers (it has no warm
// fluorescent and no twilight).
inline std::optional<std::int64_t> awb_preset(std::uint8_t mode) noexcept
{
    switch (mode) {
    case android_value::awb_incandescent: return abi::v4l2_value::white_balance_incandescent;
    case android_value::awb_fluorescent: return abi::v4l2_value::white_balance_fluorescent;
    case android_value::awb_daylight: return abi::v4l2_value::white_balance_daylight;
    case android_value::awb_cloudy_daylight: return abi::v4l2_value::white_balance_cloudy;
    case android_value::awb_shade: return abi::v4l2_value::white_balance_shade;
    default: return std::nullopt;
    }
}

// The device's rotate-and-crop menu value of an Android mode.
inline std::int64_t rotate_value(std::uint8_t mode) noexcept
{
    switch (mode) {
    case android_value::rotate_90: return 1;
    case android_value::rotate_180: return 2;
    case android_value::rotate_270: return 3;
    case android_value::rotate_auto: return abi::v4l2_value::rotate_automatic;
    default: return 0;
    }
}

// An Android region (pixels of the active array) as the device's five words.
inline std::array<std::uint32_t, abi::region_words> region_words(const Region& region, const Limits& limits) noexcept
{
    std::array<std::uint32_t, abi::region_words> words{};
    if (region.weight <= 0 || limits.array_width == 0 || limits.array_height == 0) return words;
    const double width = limits.array_width, height = limits.array_height;
    const double x0 = std::clamp(double(region.x_min) / width, 0.0, 1.0);
    const double y0 = std::clamp(double(region.y_min) / height, 0.0, 1.0);
    const double x1 = std::clamp(double(region.x_max) / width, 0.0, 1.0);
    const double y1 = std::clamp(double(region.y_max) / height, 0.0, 1.0);
    if (x1 <= x0 || y1 <= y0) return words;
    words[0] = to_q16(x0);
    words[1] = to_q16(y0);
    words[2] = to_q16(x1 - x0);
    words[3] = to_q16(y1 - y0);
    words[4] = std::uint32_t(std::min(region.weight, 1000));
    return words;
}

// Plans the writes of each request.
class ControlPlanner {
public:
    explicit ControlPlanner(const Limits& limits, float ev_step = 1.0f / 3.0f) : limits_(limits), ev_step_(ev_step) {}

    // The writes this request needs, given what was written before it (what the
    // caller has committed). A trigger writes at every request that carries it.
    // The planner remembers nothing about a write until commit(): a write the
    // camera refused or never received is planned again.
    std::vector<ControlWrite> plan(const Request& request)
    {
        std::vector<ControlWrite> writes;
        const bool manual_exposure = request.ae_mode == android_value::ae_off && limits_.manual_sensor;

        value(writes, abi::cid::exposure_auto,
              manual_exposure ? abi::v4l2_value::exposure_manual : abi::v4l2_value::exposure_auto);
        value(writes, abi::cid::iso_sensitivity_auto,
              manual_exposure ? abi::v4l2_value::iso_manual : abi::v4l2_value::iso_automatic);
        if (manual_exposure) {
            value(writes, abi::cid::exposure_ns, clamp_exposure(request.exposure_ns));
            value(writes, abi::cid::iso, clamp_iso(request.sensitivity));
        } else {
            value(writes, abi::cid::auto_exposure_bias, bias_of(request.ae_compensation_steps));
        }

        const bool has_af = (limits_.flags & abi::feature::autofocus) != 0;
        const bool continuous = request.af_mode == android_value::af_continuous_picture ||
            request.af_mode == android_value::af_continuous_video;
        const bool scanning_mode = request.af_mode == android_value::af_auto || request.af_mode == android_value::af_macro;
        if (has_af && (!last_af_mode_ || *last_af_mode_ != request.af_mode)) {
            // A new mode starts over: the lock and the scan of the old one end, and a
            // manual lens goes where the request says, though a scan may have moved it.
            focus_locked_ = false;
            af_engaged_ = false;
            if (!continuous && !scanning_mode) forget(abi::cid::focus_absolute);
            last_af_mode_ = request.af_mode;
        }

        std::int64_t locks = 0;
        if (request.ae_lock) locks |= abi::v4l2_value::lock_exposure;
        if (request.awb_lock) locks |= abi::v4l2_value::lock_white_balance;
        if (focus_locked_) locks |= abi::v4l2_value::lock_focus;

        // Focus. With FOCUS_AUTO off the host's lens holds, a press of the focus
        // button runs one scan and a written distance moves it. The press and the
        // cancel are the request's last write, so that the scan runs with the
        // region, locks, white balance and zoom the request states, and no later
        // write of this request disturbs it.
        std::optional<ControlWrite> scan;
        if (has_af) {
            value(writes, abi::cid::focus_auto, continuous ? 1 : 0);
            if (!continuous && !scanning_mode && (limits_.flags & abi::feature::manual_focus))
                value(writes, abi::cid::focus_absolute, focus_of(request.focus_distance));
            if (request.af_trigger == android_value::af_trigger_start) {
                if (continuous) {
                    focus_locked_ = true;
                    locks |= abi::v4l2_value::lock_focus;
                } else if (scanning_mode) {
                    scan = ControlWrite{abi::cid::auto_focus_start, 1, {}, false};
                    af_engaged_ = true;
                }
            } else if (request.af_trigger == android_value::af_trigger_cancel) {
                focus_locked_ = false;
                locks &= ~abi::v4l2_value::lock_focus;
                if (scanning_mode) {
                    scan = ControlWrite{abi::cid::auto_focus_stop, 1, {}, false};
                    af_engaged_ = false;
                }
            }
            region(writes, abi::cid::af_region, request.af_region, last_af_words_);
        }
        value(writes, abi::cid::lock_3a, locks);

        // White balance.
        const bool manual_white_balance =
            request.awb_mode == android_value::awb_off && (limits_.flags & abi::feature::manual_white_balance);
        if (manual_white_balance) {
            value(writes, abi::cid::auto_white_balance, 0);
            value(writes, abi::cid::wb_gain_red, gain_of(request.wb_gains[0]));
            value(writes, abi::cid::wb_gain_green, gain_of(0.5f * (request.wb_gains[1] + request.wb_gains[2])));
            value(writes, abi::cid::wb_gain_blue, gain_of(request.wb_gains[3]));
        } else if (const auto preset = awb_preset(request.awb_mode);
                   preset && (limits_.flags & abi::feature::manual_white_balance)) {
            value(writes, abi::cid::auto_white_balance, 0);
            value(writes, abi::cid::auto_n_preset_white_balance, *preset);
        } else {
            value(writes, abi::cid::auto_white_balance, 1);
        }

        region(writes, abi::cid::ae_region, request.ae_region, last_ae_words_);

        if (limits_.flags & abi::feature::zoom) value(writes, abi::cid::zoom_q16, zoom_of(request.zoom_ratio));

        // Flash: a torch lights the lens at once, a single flash is for the still.
        if (limits_.flags & abi::feature::torch)
            value(writes, abi::cid::flash_led_mode,
                  request.flash_mode == android_value::flash_torch ? abi::v4l2_value::flash_led_torch
                                                                   : abi::v4l2_value::flash_led_off);
        if (limits_.flags & abi::feature::flash) {
            std::int64_t still = 0;
            if (request.flash_mode == android_value::flash_single || request.ae_mode == android_value::ae_on_always_flash)
                still = 2;
            else if (request.ae_mode == android_value::ae_on_auto_flash)
                still = 1;
            value(writes, abi::cid::still_flash, still);
        }

        value(writes, abi::cid::test_pattern,
              request.test_pattern == android_value::pattern_off ? 0 : 1);
        value(writes, abi::cid::rotate_and_crop, rotate_value(request.rotate_and_crop));
        value(writes, abi::cid::image_stabilization, request.video_stabilization ? 1 : 0);
        if (limits_.has_jpeg)
            value(writes, abi::cid::jpeg_compression_quality, std::clamp<std::int64_t>(request.jpeg_quality, 1, 100));
        if (scan) writes.push_back(*scan);
        return writes;
    }

    // Records that the camera accepted a write, so that the next plan leaves it
    // out until the request changes it.
    void commit(const ControlWrite& write)
    {
        if (write.is_region) {
            if (write.id == abi::cid::ae_region) last_ae_words_ = write.region;
            else if (write.id == abi::cid::af_region) last_af_words_ = write.region;
            return;
        }
        for (auto& entry : last_)
            if (entry.first == write.id) {
                entry.second = write.value;
                return;
            }
        last_.emplace_back(write.id, write.value);
    }

    // Forgets what was written (the camera was closed or its stream restarted).
    void reset()
    {
        last_.clear();
        last_ae_words_ = last_af_words_ = {};
        focus_locked_ = false;
        af_engaged_ = false;
        last_af_mode_.reset();
    }

    // A scan the request asked for (AF mode auto or macro, a trigger) is under way
    // or done, and no cancel or mode change has ended it.
    [[nodiscard]] bool af_engaged() const noexcept { return af_engaged_; }

    // The host takes exposure times and sensitivities from 1 up (its ranges, not
    // below the description's minimum), and the HAL's range may be wider.
    [[nodiscard]] std::int64_t clamp_exposure(std::int64_t ns) const noexcept
    {
        const std::int64_t low = std::max<std::int64_t>(std::int64_t(limits_.exposure_min_ns), 1);
        if (limits_.exposure_max_ns == 0) return std::max(ns, low);
        return std::clamp<std::int64_t>(ns, low, std::max(low, std::int64_t(limits_.exposure_max_ns)));
    }
    [[nodiscard]] std::int64_t clamp_iso(std::int32_t iso) const noexcept
    {
        const std::int64_t low = std::max<std::int64_t>(std::int64_t(limits_.iso_min), 1);
        return std::clamp<std::int64_t>(iso, low, std::max(low, std::int64_t(limits_.iso_max)));
    }

private:
    // Android states exposure compensation in steps of ev_step stops; the host
    // takes thousandths of a stop.
    [[nodiscard]] std::int64_t bias_of(std::int32_t steps) const noexcept
    {
        return std::clamp<std::int64_t>(std::llround(double(steps) * double(ev_step_) * 1000.0), -3000, 3000);
    }
    // The host's focus position is in thousandths of a dioptre; 0 is infinity.
    [[nodiscard]] std::int64_t focus_of(float dioptres) const noexcept
    {
        if (limits_.min_focus_distance_mm == 0) return 0;
        const double maximum = 1'000'000.0 / double(limits_.min_focus_distance_mm);
        return std::llround(std::clamp(double(dioptres) * 1000.0, 0.0, maximum));
    }
    [[nodiscard]] std::int64_t gain_of(float gain) const noexcept
    {
        const double maximum = limits_.max_wb_gain_q16 != 0 ? double(limits_.max_wb_gain_q16) / 65536.0 : 8.0;
        return std::int64_t(to_q16(std::clamp(double(gain), 1.0, maximum)));
    }
    [[nodiscard]] std::int64_t zoom_of(float ratio) const noexcept
    {
        const double low = limits_.zoom_min_q16 ? double(limits_.zoom_min_q16) / 65536.0 : 1.0;
        const double high = limits_.zoom_max_q16 ? double(limits_.zoom_max_q16) / 65536.0 : 1.0;
        return std::int64_t(to_q16(std::clamp(double(ratio), low, std::max(low, high))));
    }

    void value(std::vector<ControlWrite>& writes, std::uint32_t id, std::int64_t v) const
    {
        for (const auto& entry : last_)
            if (entry.first == id && entry.second == v) return;
        writes.push_back({id, v, {}, false});
    }
    void region(std::vector<ControlWrite>& writes, std::uint32_t id, const Region& wanted,
                const std::array<std::uint32_t, abi::region_words>& last) const
    {
        const auto words = region_words(wanted, limits_);
        if (words == last) return;
        ControlWrite write;
        write.id = id;
        write.is_region = true;
        write.region = words;
        writes.push_back(write);
    }
    void forget(std::uint32_t id)
    {
        last_.erase(std::remove_if(last_.begin(), last_.end(), [id](const auto& entry) { return entry.first == id; }),
                    last_.end());
    }

    Limits limits_;
    float ev_step_;
    std::vector<std::pair<std::uint32_t, std::int64_t>> last_; // what the camera accepted
    std::array<std::uint32_t, abi::region_words> last_ae_words_{}, last_af_words_{};
    std::optional<std::uint8_t> last_af_mode_;
    bool focus_locked_ = false;
    bool af_engaged_ = false;
};

// What the result of a request says, from the exposure's metadata record.
struct ResultValues {
    std::uint32_t ae_state = 0;
    std::uint32_t af_state = 0;
    std::uint32_t awb_state = 0;
    std::int64_t exposure_ns = 0;
    std::int64_t frame_duration_ns = 0;
    std::int32_t sensitivity = 0;
    float focus_distance = 0.0f;
    float aperture = 0.0f;
    float focal_length_mm = 0.0f;
    float zoom_ratio = 1.0f;
    std::array<float, 4> wb_gains = {1.0f, 1.0f, 1.0f, 1.0f};
    std::uint32_t flash_state = 0;
};

// The request's AE precapture trigger reports PRECAPTURE for the exposure it
// arrives with, and the host's own state after it (iOS meters while it takes a
// photo, so there is no separate precapture sequence to wait for). AF reports
// inactive when focus is off; in the scanning modes (auto, macro) it is inactive
// until a trigger and after a cancel or a change of mode (`af_engaged`), ACTIVE_SCAN
// for the request that triggers, and the host's state after that; in the continuous
// modes it is the host's state.
inline ResultValues result_of(const abi::Meta& meta, const Request& request, bool flash_available,
                              bool af_engaged) noexcept
{
    ResultValues result;
    result.ae_state = request.ae_precapture_trigger == android_value::ae_trigger_start
        ? android_value::ae_state_precapture
        : meta.ae_state;
    if (request.ae_mode == android_value::ae_off) result.ae_state = android_value::ae_state_inactive;
    result.af_state = meta.af_state;
    if (request.af_mode == android_value::af_off) {
        result.af_state = android_value::af_state_inactive;
    } else if (request.af_mode == android_value::af_auto || request.af_mode == android_value::af_macro) {
        if (request.af_trigger == android_value::af_trigger_start)
            result.af_state = android_value::af_state_active_scan;
        else if (!af_engaged)
            result.af_state = android_value::af_state_inactive;
    }
    result.awb_state = request.awb_mode == android_value::awb_off ? android_value::awb_state_inactive : meta.awb_state;
    result.exposure_ns = std::int64_t(meta.exposure_ns);
    result.frame_duration_ns = std::int64_t(meta.frame_duration_ns);
    result.sensitivity = std::int32_t(meta.iso);
    result.focus_distance = from_q16(meta.lens_position_q16);
    result.aperture = float(meta.aperture_x100) / 100.0f;
    result.focal_length_mm = float(meta.focal_length_um) / 1000.0f;
    result.zoom_ratio = meta.zoom_ratio_q16 ? from_q16(meta.zoom_ratio_q16) : 1.0f;
    const float red = from_q16(meta.wb_gain_q16[0]), green = from_q16(meta.wb_gain_q16[1]), blue = from_q16(meta.wb_gain_q16[2]);
    result.wb_gains = {red, green, green, blue};
    result.flash_state = flash_available ? (meta.flash_state != 0 ? meta.flash_state : android_value::flash_state_ready)
                                         : android_value::flash_state_unavailable;
    return result;
}

} // namespace andarium::camera

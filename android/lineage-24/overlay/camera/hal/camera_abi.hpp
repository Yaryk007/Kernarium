// SPDX-License-Identifier: Apache-2.0
//
// The camera device's records as the guest's HAL reads them: the static
// description of a camera (one record per camera) and the metadata of an
// exposure (one record per exposure, from the metadata node). They are
// declared in kernel/common/video/virtio_video.h, which is the contract; this
// copy has no Linux types so that the HAL and the host-side tests can include
// it, and tests/unit/andarium/camera_abi_tests.cpp compares every size, offset
// and value with the header.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace andarium::camera::abi {

static_assert(std::endian::native == std::endian::little, "the records are little endian");

constexpr std::uint32_t max_cameras = 8;
constexpr std::uint32_t max_switch_overs = 4;

namespace facing {
constexpr std::uint32_t back = 1;
constexpr std::uint32_t front = 2;
constexpr std::uint32_t external = 3;
} // namespace facing

namespace lens {
constexpr std::uint32_t zoom = 1;
constexpr std::uint32_t wide = 2;
constexpr std::uint32_t ultra_wide = 3;
constexpr std::uint32_t telephoto = 4;
constexpr std::uint32_t external = 5;
} // namespace lens

namespace feature {
constexpr std::uint32_t autofocus = 1u << 0;
constexpr std::uint32_t flash = 1u << 1;
constexpr std::uint32_t torch = 1u << 2;
constexpr std::uint32_t zoom = 1u << 3;
constexpr std::uint32_t manual_exposure = 1u << 4;
constexpr std::uint32_t manual_focus = 1u << 5;
constexpr std::uint32_t manual_white_balance = 1u << 6;
constexpr std::uint32_t raw = 1u << 7;
} // namespace feature

namespace bayer {
constexpr std::uint32_t none = 0;
constexpr std::uint32_t rggb = 1;
constexpr std::uint32_t grbg = 2;
constexpr std::uint32_t gbrg = 3;
constexpr std::uint32_t bggr = 4;
} // namespace bayer

namespace frame_flag {
constexpr std::uint32_t black = 1u << 0;
constexpr std::uint32_t interrupted = 1u << 1;
constexpr std::uint32_t blocked = 1u << 2;
constexpr std::uint32_t resuming = 1u << 3;
constexpr std::uint32_t still = 1u << 4;
} // namespace frame_flag

// A camera's static description: virtio_video_camera_desc.
struct Desc {
    std::uint32_t index;
    std::uint32_t facing;
    std::uint32_t lens;
    std::uint32_t flags;
    std::uint32_t conflict_mask;
    std::uint32_t concurrent_mask;
    std::uint32_t constituent_mask;
    std::uint32_t sensor_orientation;
    std::uint32_t pixel_array_width;
    std::uint32_t pixel_array_height;
    std::uint32_t sensor_width_um;
    std::uint32_t sensor_height_um;
    std::uint32_t focal_length_um;
    std::uint32_t aperture_x100;
    std::uint32_t min_focus_distance_mm;
    std::uint32_t iso_min;
    std::uint32_t iso_max;
    std::uint8_t reserved0[4];
    std::uint64_t exposure_min_ns;
    std::uint64_t exposure_max_ns;
    std::uint32_t zoom_min_q16;
    std::uint32_t zoom_max_q16;
    std::uint32_t num_switch_overs;
    std::uint32_t switch_over_q16[max_switch_overs];
    std::uint32_t max_wb_gain_q16;
    std::uint32_t intrinsics_q16[4];
    std::uint32_t bayer_order;
    std::uint32_t black_level;
    std::uint32_t white_level;
    std::uint32_t raw_width;
    std::uint32_t raw_height;
    std::uint32_t reference_illuminant1;
    std::uint32_t reference_illuminant2;
    std::int32_t color_matrix1_q16[9];
    std::int32_t color_matrix2_q16[9];
    std::int32_t forward_matrix1_q16[9];
    std::int32_t forward_matrix2_q16[9];
    std::uint8_t reserved[20];
};

// What a camera actually did for one exposure: virtio_video_camera_meta.
struct Meta {
    std::uint64_t sequence;
    std::uint64_t exposure_ns;
    std::uint64_t frame_duration_ns;
    std::uint32_t iso;
    std::uint32_t flags;
    std::uint32_t ae_state;
    std::uint32_t af_state;
    std::uint32_t awb_state;
    std::uint32_t lens_position_q16;
    std::uint32_t wb_gain_q16[3];
    std::uint32_t color_temperature_k;
    std::int32_t tint_q16;
    std::uint32_t zoom_ratio_q16;
    std::uint32_t active_camera;
    std::uint32_t aperture_x100;
    std::uint32_t focal_length_um;
    std::uint32_t flash_state;
    std::uint32_t neutral_point_q16[3];
    std::uint32_t noise_profile_f32[8];
    std::uint32_t jpeg_gap_offset;
    std::uint32_t jpeg_gap_length;
    std::uint8_t reserved[28];
};

static_assert(sizeof(Desc) == 328);
static_assert(offsetof(Desc, exposure_min_ns) == 72);
static_assert(offsetof(Desc, switch_over_q16) == 100);
static_assert(offsetof(Desc, bayer_order) == 136);
static_assert(offsetof(Desc, color_matrix1_q16) == 164);
static_assert(offsetof(Desc, reserved) == 308);
static_assert(sizeof(Meta) == 168);
static_assert(offsetof(Meta, wb_gain_q16) == 48);
static_assert(offsetof(Meta, noise_profile_f32) == 100);
static_assert(offsetof(Meta, jpeg_gap_offset) == 132);

// The controls the HAL writes. The standard ones are V4L2's own numbers
// (virtio_video_v4l2_ids.h states which the device offers); the private ones
// sit in the driver-private part of the user class (VIRTIO_VIDEO_CAMERA_CID_*).
namespace cid {
constexpr std::uint32_t auto_white_balance = 0x0098090c;
constexpr std::uint32_t white_balance_temperature = 0x0098091a;
constexpr std::uint32_t exposure_auto = 0x009a0901;
constexpr std::uint32_t exposure_absolute = 0x009a0902;
constexpr std::uint32_t focus_absolute = 0x009a090a;
constexpr std::uint32_t focus_auto = 0x009a090c;
constexpr std::uint32_t zoom_absolute = 0x009a090d;
constexpr std::uint32_t privacy = 0x009a0910;
constexpr std::uint32_t auto_exposure_bias = 0x009a0913;
constexpr std::uint32_t auto_n_preset_white_balance = 0x009a0914;
constexpr std::uint32_t image_stabilization = 0x009a0916;
constexpr std::uint32_t iso_sensitivity = 0x009a0917;
constexpr std::uint32_t iso_sensitivity_auto = 0x009a0918;
constexpr std::uint32_t lock_3a = 0x009a091b;
constexpr std::uint32_t auto_focus_start = 0x009a091c;
constexpr std::uint32_t auto_focus_stop = 0x009a091d;
constexpr std::uint32_t auto_focus_status = 0x009a091e;
constexpr std::uint32_t camera_orientation = 0x009a0922;
constexpr std::uint32_t camera_sensor_rotation = 0x009a0923;
constexpr std::uint32_t flash_led_mode = 0x009c0901;
constexpr std::uint32_t test_pattern = 0x009f0903;
constexpr std::uint32_t jpeg_compression_quality = 0x009d0903;

constexpr std::uint32_t private_base = 0x00981c00;
constexpr std::uint32_t exposure_ns = private_base + 0;
constexpr std::uint32_t iso = private_base + 1;
constexpr std::uint32_t wb_gain_red = private_base + 2;
constexpr std::uint32_t wb_gain_green = private_base + 3;
constexpr std::uint32_t wb_gain_blue = private_base + 4;
constexpr std::uint32_t zoom_q16 = private_base + 5;
constexpr std::uint32_t ae_region = private_base + 6;
constexpr std::uint32_t af_region = private_base + 7;
constexpr std::uint32_t still_flash = private_base + 8;
constexpr std::uint32_t rotate_and_crop = private_base + 9;
// The camera's description: a read-only compound control (U8[328]) that the
// guest driver answers, not the device (V4l2Utils.h reads it).
constexpr std::uint32_t description = private_base + 16;
} // namespace cid

// V4L2 menu values and bits the HAL writes to those controls.
namespace v4l2_value {
// EXPOSURE_AUTO is a menu of which the host offers the automatic (0) and manual (1)
// items only.
constexpr std::int64_t exposure_auto = 0;
constexpr std::int64_t exposure_manual = 1;
constexpr std::int64_t iso_manual = 0;    // V4L2_ISO_SENSITIVITY_MANUAL
constexpr std::int64_t iso_automatic = 1; // V4L2_ISO_SENSITIVITY_AUTO
constexpr std::int64_t lock_exposure = 1;
constexpr std::int64_t lock_white_balance = 2;
constexpr std::int64_t lock_focus = 4;
constexpr std::int64_t flash_led_off = 0;
constexpr std::int64_t flash_led_torch = 2;
constexpr std::int64_t focus_status_busy = 1;
constexpr std::int64_t focus_status_reached = 2;
constexpr std::int64_t focus_status_failed = 4;
constexpr std::int64_t rotate_automatic = 4;
// AUTO_N_PRESET_WHITE_BALANCE: the presets the host offers (it skips the others).
constexpr std::int64_t white_balance_incandescent = 2;
constexpr std::int64_t white_balance_fluorescent = 3;
constexpr std::int64_t white_balance_daylight = 6;
constexpr std::int64_t white_balance_cloudy = 8;
constexpr std::int64_t white_balance_shade = 9;
} // namespace v4l2_value

// The flags of the camera's buffers on the wire. The driver maps them for the HAL:
// V4L2_BUF_FLAG_LAST on the last buffer of a group of stills (not GROUP_MORE) and
// V4L2_BUF_FLAG_ERROR for a failed capture (DEQUEUE_FLAG_ERR); the other dequeue
// flags ride in the metadata as frame_flag.
namespace buffer_flag {
constexpr std::uint32_t group_more = 1u << 0;
constexpr std::uint32_t dequeue_error = 1u << 0;
constexpr std::uint32_t dequeue_black = 1u << 8;
constexpr std::uint32_t dequeue_interrupted = 1u << 9;
constexpr std::uint32_t dequeue_blocked = 1u << 10;
constexpr std::uint32_t dequeue_resuming = 1u << 11;
} // namespace buffer_flag

// The region controls carry five u32: x, y, width, height (Q16 fractions of the
// frame) and a weight from 0 to 1000; weight 0 clears the region.
constexpr std::size_t region_words = 5;

// Where the reserved Exif segment's payload of a JPEG still lies, and the
// number of bytes the device reserves for it.
constexpr std::uint32_t exif_gap_length = 65000;

// The driver registers the camera device's nodes as /dev/video10 and above; the
// codec nodes stay in video0 to video9 (the codec HAL scans only those). Discovery
// opens a node only from video10 up, because opening a codec node starts a session
// on the host.
constexpr std::uint32_t first_camera_node = 10;

// Whether an entry of /dev ("video" and a number) may be a camera node.
constexpr bool may_be_camera_node(std::string_view name) noexcept
{
    constexpr std::string_view prefix = "video";
    if (name.substr(0, prefix.size()) != prefix) return false;
    name.remove_prefix(prefix.size());
    if (name.empty() || name.size() > 9) return false;
    std::uint32_t number = 0;
    for (const char c : name) {
        if (c < '0' || c > '9') return false;
        number = number * 10 + std::uint32_t(c - '0');
    }
    return number >= first_camera_node;
}

} // namespace andarium::camera::abi

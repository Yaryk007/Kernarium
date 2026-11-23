// SPDX-License-Identifier: GPL-2.0-or-later
//
// The camera HAL's copy of the camera device's records
// (android/lineage-24/overlay/camera/hal/camera_abi.hpp) against the wire header
// that is the contract: every size, offset and value.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "camera_abi.hpp"
#include "camera_controls.hpp"
#include "jpeg_gap.hpp"
#include "require.hpp"
#include "virtio_video.h"
#include "virtio_video_v4l2_ids.h"

using uml::testing::require;
namespace abi = andarium::camera::abi;

#define SAME_OFFSET(hal, wire, member) \
    require(offsetof(abi::hal, member) == offsetof(wire, member), "offset of " #member)

namespace {

void check_desc()
{
    require(sizeof(abi::Desc) == sizeof(virtio_video_camera_desc), "the description's size");
    SAME_OFFSET(Desc, virtio_video_camera_desc, index);
    SAME_OFFSET(Desc, virtio_video_camera_desc, facing);
    SAME_OFFSET(Desc, virtio_video_camera_desc, lens);
    SAME_OFFSET(Desc, virtio_video_camera_desc, flags);
    SAME_OFFSET(Desc, virtio_video_camera_desc, conflict_mask);
    SAME_OFFSET(Desc, virtio_video_camera_desc, concurrent_mask);
    SAME_OFFSET(Desc, virtio_video_camera_desc, constituent_mask);
    SAME_OFFSET(Desc, virtio_video_camera_desc, sensor_orientation);
    SAME_OFFSET(Desc, virtio_video_camera_desc, pixel_array_width);
    SAME_OFFSET(Desc, virtio_video_camera_desc, pixel_array_height);
    SAME_OFFSET(Desc, virtio_video_camera_desc, sensor_width_um);
    SAME_OFFSET(Desc, virtio_video_camera_desc, sensor_height_um);
    SAME_OFFSET(Desc, virtio_video_camera_desc, focal_length_um);
    SAME_OFFSET(Desc, virtio_video_camera_desc, aperture_x100);
    SAME_OFFSET(Desc, virtio_video_camera_desc, min_focus_distance_mm);
    SAME_OFFSET(Desc, virtio_video_camera_desc, iso_min);
    SAME_OFFSET(Desc, virtio_video_camera_desc, iso_max);
    SAME_OFFSET(Desc, virtio_video_camera_desc, exposure_min_ns);
    SAME_OFFSET(Desc, virtio_video_camera_desc, exposure_max_ns);
    SAME_OFFSET(Desc, virtio_video_camera_desc, zoom_min_q16);
    SAME_OFFSET(Desc, virtio_video_camera_desc, zoom_max_q16);
    SAME_OFFSET(Desc, virtio_video_camera_desc, num_switch_overs);
    SAME_OFFSET(Desc, virtio_video_camera_desc, switch_over_q16);
    SAME_OFFSET(Desc, virtio_video_camera_desc, max_wb_gain_q16);
    SAME_OFFSET(Desc, virtio_video_camera_desc, intrinsics_q16);
    SAME_OFFSET(Desc, virtio_video_camera_desc, bayer_order);
    SAME_OFFSET(Desc, virtio_video_camera_desc, black_level);
    SAME_OFFSET(Desc, virtio_video_camera_desc, white_level);
    SAME_OFFSET(Desc, virtio_video_camera_desc, raw_width);
    SAME_OFFSET(Desc, virtio_video_camera_desc, raw_height);
    SAME_OFFSET(Desc, virtio_video_camera_desc, reference_illuminant1);
    SAME_OFFSET(Desc, virtio_video_camera_desc, reference_illuminant2);
    SAME_OFFSET(Desc, virtio_video_camera_desc, color_matrix1_q16);
    SAME_OFFSET(Desc, virtio_video_camera_desc, color_matrix2_q16);
    SAME_OFFSET(Desc, virtio_video_camera_desc, forward_matrix1_q16);
    SAME_OFFSET(Desc, virtio_video_camera_desc, forward_matrix2_q16);
    SAME_OFFSET(Desc, virtio_video_camera_desc, reserved);
    require(sizeof(abi::Desc::switch_over_q16) / sizeof(std::uint32_t) == VIRTIO_VIDEO_CAMERA_MAX_SWITCH_OVERS,
            "the switch-over count");
}

void check_meta()
{
    require(sizeof(abi::Meta) == sizeof(virtio_video_camera_meta), "the metadata's size");
    SAME_OFFSET(Meta, virtio_video_camera_meta, sequence);
    SAME_OFFSET(Meta, virtio_video_camera_meta, exposure_ns);
    SAME_OFFSET(Meta, virtio_video_camera_meta, frame_duration_ns);
    SAME_OFFSET(Meta, virtio_video_camera_meta, iso);
    SAME_OFFSET(Meta, virtio_video_camera_meta, flags);
    SAME_OFFSET(Meta, virtio_video_camera_meta, ae_state);
    SAME_OFFSET(Meta, virtio_video_camera_meta, af_state);
    SAME_OFFSET(Meta, virtio_video_camera_meta, awb_state);
    SAME_OFFSET(Meta, virtio_video_camera_meta, lens_position_q16);
    SAME_OFFSET(Meta, virtio_video_camera_meta, wb_gain_q16);
    SAME_OFFSET(Meta, virtio_video_camera_meta, color_temperature_k);
    SAME_OFFSET(Meta, virtio_video_camera_meta, tint_q16);
    SAME_OFFSET(Meta, virtio_video_camera_meta, zoom_ratio_q16);
    SAME_OFFSET(Meta, virtio_video_camera_meta, active_camera);
    SAME_OFFSET(Meta, virtio_video_camera_meta, aperture_x100);
    SAME_OFFSET(Meta, virtio_video_camera_meta, focal_length_um);
    SAME_OFFSET(Meta, virtio_video_camera_meta, flash_state);
    SAME_OFFSET(Meta, virtio_video_camera_meta, neutral_point_q16);
    SAME_OFFSET(Meta, virtio_video_camera_meta, noise_profile_f32);
    SAME_OFFSET(Meta, virtio_video_camera_meta, jpeg_gap_offset);
    SAME_OFFSET(Meta, virtio_video_camera_meta, jpeg_gap_length);
    SAME_OFFSET(Meta, virtio_video_camera_meta, reserved);
}

void check_values()
{
    require(abi::max_cameras == VIRTIO_VIDEO_CAMERA_MAX_CAMERAS, "camera count");
    require(abi::max_switch_overs == VIRTIO_VIDEO_CAMERA_MAX_SWITCH_OVERS, "switch-over count");
    require(abi::facing::back == VIRTIO_VIDEO_CAMERA_FACING_BACK, "back");
    require(abi::facing::front == VIRTIO_VIDEO_CAMERA_FACING_FRONT, "front");
    require(abi::facing::external == VIRTIO_VIDEO_CAMERA_FACING_EXTERNAL, "external");
    require(abi::lens::zoom == VIRTIO_VIDEO_CAMERA_LENS_ZOOM, "zoom");
    require(abi::lens::wide == VIRTIO_VIDEO_CAMERA_LENS_WIDE, "wide");
    require(abi::lens::ultra_wide == VIRTIO_VIDEO_CAMERA_LENS_ULTRA_WIDE, "ultra wide");
    require(abi::lens::telephoto == VIRTIO_VIDEO_CAMERA_LENS_TELEPHOTO, "telephoto");
    require(abi::lens::external == VIRTIO_VIDEO_CAMERA_LENS_EXTERNAL, "external lens");
    require(abi::feature::autofocus == VIRTIO_VIDEO_CAMERA_F_AUTOFOCUS, "autofocus");
    require(abi::feature::flash == VIRTIO_VIDEO_CAMERA_F_FLASH, "flash");
    require(abi::feature::torch == VIRTIO_VIDEO_CAMERA_F_TORCH, "torch");
    require(abi::feature::zoom == VIRTIO_VIDEO_CAMERA_F_ZOOM, "zoom feature");
    require(abi::feature::manual_exposure == VIRTIO_VIDEO_CAMERA_F_MANUAL_EXPOSURE, "manual exposure");
    require(abi::feature::manual_focus == VIRTIO_VIDEO_CAMERA_F_MANUAL_FOCUS, "manual focus");
    require(abi::feature::manual_white_balance == VIRTIO_VIDEO_CAMERA_F_MANUAL_WHITE_BALANCE, "manual white balance");
    require(abi::feature::raw == VIRTIO_VIDEO_CAMERA_F_RAW, "raw");
    require(abi::bayer::none == VIRTIO_VIDEO_CAMERA_BAYER_NONE, "no bayer");
    require(abi::bayer::rggb == VIRTIO_VIDEO_CAMERA_BAYER_RGGB, "rggb");
    require(abi::bayer::grbg == VIRTIO_VIDEO_CAMERA_BAYER_GRBG, "grbg");
    require(abi::bayer::gbrg == VIRTIO_VIDEO_CAMERA_BAYER_GBRG, "gbrg");
    require(abi::bayer::bggr == VIRTIO_VIDEO_CAMERA_BAYER_BGGR, "bggr");
    require(abi::frame_flag::black == VIRTIO_VIDEO_CAMERA_FRAME_F_BLACK, "black frame");
    require(abi::frame_flag::interrupted == VIRTIO_VIDEO_CAMERA_FRAME_F_INTERRUPTED, "interrupted frame");
    require(abi::frame_flag::blocked == VIRTIO_VIDEO_CAMERA_FRAME_F_BLOCKED, "blocked frame");
    require(abi::frame_flag::resuming == VIRTIO_VIDEO_CAMERA_FRAME_F_RESUMING, "resuming frame");
    require(abi::frame_flag::still == VIRTIO_VIDEO_CAMERA_FRAME_F_STILL, "still frame");
}

void check_controls()
{
    require(abi::cid::auto_white_balance == UML_V4L2_CID_AUTO_WHITE_BALANCE, "AUTO_WHITE_BALANCE");
    require(abi::cid::white_balance_temperature == UML_V4L2_CID_WHITE_BALANCE_TEMPERATURE, "WHITE_BALANCE_TEMPERATURE");
    require(abi::cid::exposure_auto == UML_V4L2_CID_EXPOSURE_AUTO, "EXPOSURE_AUTO");
    require(abi::cid::exposure_absolute == UML_V4L2_CID_EXPOSURE_ABSOLUTE, "EXPOSURE_ABSOLUTE");
    require(abi::cid::focus_absolute == UML_V4L2_CID_FOCUS_ABSOLUTE, "FOCUS_ABSOLUTE");
    require(abi::cid::focus_auto == UML_V4L2_CID_FOCUS_AUTO, "FOCUS_AUTO");
    require(abi::cid::zoom_absolute == UML_V4L2_CID_ZOOM_ABSOLUTE, "ZOOM_ABSOLUTE");
    require(abi::cid::privacy == UML_V4L2_CID_PRIVACY, "PRIVACY");
    require(abi::cid::auto_exposure_bias == UML_V4L2_CID_AUTO_EXPOSURE_BIAS, "AUTO_EXPOSURE_BIAS");
    require(abi::cid::auto_n_preset_white_balance == UML_V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE, "AUTO_N_PRESET_WHITE_BALANCE");
    require(abi::cid::image_stabilization == UML_V4L2_CID_IMAGE_STABILIZATION, "IMAGE_STABILIZATION");
    require(abi::cid::iso_sensitivity == UML_V4L2_CID_ISO_SENSITIVITY, "ISO_SENSITIVITY");
    require(abi::cid::iso_sensitivity_auto == UML_V4L2_CID_ISO_SENSITIVITY_AUTO, "ISO_SENSITIVITY_AUTO");
    require(abi::cid::lock_3a == UML_V4L2_CID_3A_LOCK, "3A_LOCK");
    require(abi::cid::auto_focus_start == UML_V4L2_CID_AUTO_FOCUS_START, "AUTO_FOCUS_START");
    require(abi::cid::auto_focus_stop == UML_V4L2_CID_AUTO_FOCUS_STOP, "AUTO_FOCUS_STOP");
    require(abi::cid::auto_focus_status == UML_V4L2_CID_AUTO_FOCUS_STATUS, "AUTO_FOCUS_STATUS");
    require(abi::cid::camera_orientation == UML_V4L2_CID_CAMERA_ORIENTATION, "CAMERA_ORIENTATION");
    require(abi::cid::camera_sensor_rotation == UML_V4L2_CID_CAMERA_SENSOR_ROTATION, "CAMERA_SENSOR_ROTATION");
    require(abi::cid::flash_led_mode == UML_V4L2_CID_FLASH_LED_MODE, "FLASH_LED_MODE");
    require(abi::cid::test_pattern == UML_V4L2_CID_TEST_PATTERN, "TEST_PATTERN");
    require(abi::cid::jpeg_compression_quality == UML_V4L2_CID_JPEG_COMPRESSION_QUALITY, "JPEG_COMPRESSION_QUALITY");

    require(abi::cid::private_base == VIRTIO_VIDEO_CAMERA_CID_BASE, "private base");
    require(abi::cid::exposure_ns == VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS, "EXPOSURE_NS");
    require(abi::cid::iso == VIRTIO_VIDEO_CAMERA_CID_ISO, "ISO");
    require(abi::cid::wb_gain_red == VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_RED, "WB_GAIN_RED");
    require(abi::cid::wb_gain_green == VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_GREEN, "WB_GAIN_GREEN");
    require(abi::cid::wb_gain_blue == VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_BLUE, "WB_GAIN_BLUE");
    require(abi::cid::zoom_q16 == VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16, "ZOOM_Q16");
    require(abi::cid::ae_region == VIRTIO_VIDEO_CAMERA_CID_AE_REGION, "AE_REGION");
    require(abi::cid::af_region == VIRTIO_VIDEO_CAMERA_CID_AF_REGION, "AF_REGION");
    require(abi::cid::still_flash == VIRTIO_VIDEO_CAMERA_CID_STILL_FLASH, "STILL_FLASH");
    require(abi::cid::rotate_and_crop == VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP, "ROTATE_AND_CROP");
    require(abi::cid::description == VIRTIO_VIDEO_CAMERA_CID_DESCRIPTION, "DESCRIPTION");
    require(abi::v4l2_value::rotate_automatic == VIRTIO_VIDEO_CAMERA_ROTATE_AUTOMATIC, "automatic rotation");
}

// The menu items and bits the HAL writes, and the numbers it reads back.
void check_menu_values()
{
    require(abi::v4l2_value::exposure_auto == UML_V4L2_EXPOSURE_AUTO && abi::v4l2_value::exposure_auto == 0, "EXPOSURE_AUTO automatic");
    require(abi::v4l2_value::exposure_manual == UML_V4L2_EXPOSURE_MANUAL, "EXPOSURE_AUTO manual");
    require(abi::v4l2_value::iso_manual == UML_V4L2_ISO_SENSITIVITY_MANUAL, "ISO manual");
    require(abi::v4l2_value::iso_automatic == UML_V4L2_ISO_SENSITIVITY_AUTO, "ISO automatic");
    require(abi::v4l2_value::lock_exposure == UML_V4L2_LOCK_EXPOSURE, "lock exposure");
    require(abi::v4l2_value::lock_white_balance == UML_V4L2_LOCK_WHITE_BALANCE, "lock white balance");
    require(abi::v4l2_value::lock_focus == UML_V4L2_LOCK_FOCUS, "lock focus");
    require(abi::v4l2_value::flash_led_off == UML_V4L2_FLASH_LED_MODE_NONE, "LED off");
    require(abi::v4l2_value::flash_led_torch == UML_V4L2_FLASH_LED_MODE_TORCH, "LED torch");
    require(abi::v4l2_value::focus_status_busy == UML_V4L2_AUTO_FOCUS_STATUS_BUSY, "AF busy");
    require(abi::v4l2_value::focus_status_reached == UML_V4L2_AUTO_FOCUS_STATUS_REACHED, "AF reached");
    require(abi::v4l2_value::focus_status_failed == UML_V4L2_AUTO_FOCUS_STATUS_FAILED, "AF failed");
    require(abi::v4l2_value::white_balance_incandescent == UML_V4L2_WHITE_BALANCE_INCANDESCENT, "incandescent");
    require(abi::v4l2_value::white_balance_fluorescent == UML_V4L2_WHITE_BALANCE_FLUORESCENT, "fluorescent");
    require(abi::v4l2_value::white_balance_daylight == UML_V4L2_WHITE_BALANCE_DAYLIGHT, "daylight");
    require(abi::v4l2_value::white_balance_cloudy == UML_V4L2_WHITE_BALANCE_CLOUDY, "cloudy");
    require(abi::v4l2_value::white_balance_shade == UML_V4L2_WHITE_BALANCE_SHADE, "shade");

    // The presets the planner writes are the ones the host does not skip (its menu
    // skips 4, 5 and 7), and a planned preset is a menu item V4L2 knows.
    using andarium::camera::awb_preset;
    namespace av = andarium::camera::android_value;
    for (const auto mode : {av::awb_incandescent, av::awb_fluorescent, av::awb_daylight, av::awb_cloudy_daylight, av::awb_shade}) {
        const auto preset = awb_preset(mode);
        require(preset && *preset != 4 && *preset != 5 && *preset != 7, "a preset the host offers");
    }
}

// The buffers' flags on the wire, the region records and the Android enumerations
// that the wire names by reference.
void check_wire_details()
{
    require(abi::buffer_flag::group_more == VIRTIO_VIDEO_CAMERA_QUEUE_F_GROUP_MORE, "GROUP_MORE");
    require(abi::buffer_flag::dequeue_error == VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, "the error dequeue flag");
    require(abi::buffer_flag::dequeue_black == VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLACK, "black");
    require(abi::buffer_flag::dequeue_interrupted == VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED, "interrupted");
    require(abi::buffer_flag::dequeue_blocked == VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLOCKED, "blocked");
    require(abi::buffer_flag::dequeue_resuming == VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_RESUMING, "resuming");

    // A region is five u32 (x, y, width, height, weight): the header says so, and
    // the host's records are 20 bytes (regionPayloadSize in the host's controls).
    require(abi::region_words == 5 && abi::region_words * sizeof(std::uint32_t) == 20, "a region record");
    require(abi::exif_gap_length == uml::camera::exifGapLength, "the Exif gap the host reserves");

    // The camera nodes are video10 and above; the codec nodes below them are never opened.
    require(abi::first_camera_node == 10, "the first camera node");
    for (const char* node : {"video10", "video11", "video99", "video100", "video4294967"})
        require(abi::may_be_camera_node(node), "a camera node");
    for (const char* node : {"video0", "video1", "video9", "video", "video-1", "video1x", "videoX", "video4linux", "vbi10",
                             "media10", "", "xvideo10", "video10000000000"})
        require(!abi::may_be_camera_node(node), "not a camera node");

    // The 3A and flash state numbers are Android's own (CONTROL_AE_STATE_*,
    // CONTROL_AF_STATE_*, CONTROL_AWB_STATE_*, FLASH_STATE_*), as the header says.
    namespace av = andarium::camera::android_value;
    require(av::ae_state_inactive == 0 && av::ae_state_searching == 1 && av::ae_state_converged == 2 && av::ae_state_locked == 3 &&
                av::ae_state_flash_required == 4 && av::ae_state_precapture == 5,
            "AE states");
    require(av::af_state_inactive == 0 && av::af_state_passive_scan == 1 && av::af_state_passive_focused == 2 &&
                av::af_state_active_scan == 3 && av::af_state_focused_locked == 4 && av::af_state_not_focused_locked == 5 &&
                av::af_state_passive_unfocused == 6,
            "AF states");
    require(av::awb_state_inactive == 0 && av::awb_state_searching == 1 && av::awb_state_converged == 2 && av::awb_state_locked == 3,
            "AWB states");
    require(av::flash_state_unavailable == 0 && av::flash_state_ready == 2, "flash states");
}

} // namespace

int main()
{
    check_desc();
    check_meta();
    check_values();
    check_controls();
    check_menu_values();
    check_wire_details();
    return 0;
}

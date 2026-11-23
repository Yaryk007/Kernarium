// SPDX-License-Identifier: GPL-2.0-or-later
#include "virtio_video.h"
#include "virtio_video_v4l2_ids.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>

/* The camera records are laid out for a kernel that compiles the same header:
 * each keeps its size and every 64-bit member its natural alignment, and every
 * gap is a named member, so no compiler padding is left to disagree about. */
static_assert(sizeof(virtio_video_camera_query) == 16);
static_assert(sizeof(virtio_video_camera_desc) == 328);
static_assert(sizeof(virtio_video_camera_desc) % 8 == 0);
static_assert(offsetof(virtio_video_camera_desc, reserved0) == 68);
static_assert(offsetof(virtio_video_camera_desc, exposure_min_ns) == 72);
static_assert(offsetof(virtio_video_camera_desc, reserved) + sizeof(virtio_video_camera_desc::reserved) ==
              sizeof(virtio_video_camera_desc));
static_assert(offsetof(virtio_video_camera_desc, switch_over_q16) == 100);
static_assert(offsetof(virtio_video_camera_desc, color_matrix1_q16) == 164);
static_assert(sizeof(virtio_video_camera_query_resp) == 32);
static_assert(sizeof(virtio_video_camera_query_formats) == 16);
static_assert(sizeof(virtio_video_camera_query_controls) == 16);
static_assert(sizeof(virtio_video_camera_stream_create) == 88);
static_assert(sizeof(virtio_video_camera_queue_resp) == 64);
static_assert(offsetof(virtio_video_camera_queue_resp, sequence) == 56);
static_assert(sizeof(virtio_video_camera_meta) == 168);
static_assert(sizeof(virtio_video_camera_meta) % 8 == 0);
static_assert(offsetof(virtio_video_camera_meta, reserved) + sizeof(virtio_video_camera_meta::reserved) ==
              sizeof(virtio_video_camera_meta));
static_assert(offsetof(virtio_video_camera_meta, wb_gain_q16) == 48);
static_assert(VIRTIO_VIDEO_DEVICE_CAMERA == 0x0102);
static_assert(UML_VIRTIO_VIDEO_CAMERA_DEVICE_ID != 65535);
static_assert(UML_VIRTIO_VIDEO_CAMERA_DEVICE_ID != 65534);

namespace {
int failures = 0;
void check(bool passed, const char* what)
{
    if (passed) return;
    std::fprintf(stderr, "camera-protocol: %s\n", what);
    ++failures;
}
}

int main()
{
    /* The camera's commands and formats sit clear of the codecs'. */
    check(VIRTIO_VIDEO_CMD_CAMERA_QUERY > VIRTIO_VIDEO_CMD_GET_STREAM_INFO, "camera commands follow the codecs'");
    check(VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE < VIRTIO_VIDEO_RESP_OK_NODATA, "camera commands stay below the responses");
    check(VIRTIO_VIDEO_FORMAT_CAMERA_MIN > VIRTIO_VIDEO_FORMAT_CODED_MAX, "camera formats follow the coded ones");
    check(VIRTIO_VIDEO_FORMAT_CAMERA_JPEG == VIRTIO_VIDEO_FORMAT_CAMERA_MIN, "the JPEG still starts the camera formats");
    check(VIRTIO_VIDEO_FORMAT_CAMERA_META == VIRTIO_VIDEO_FORMAT_CAMERA_MAX, "the metadata format ends the camera formats");
    /* A camera completion flag never collides with a codec's. */
    const unsigned codec = VIRTIO_VIDEO_DEQUEUE_FLAG_ERR | VIRTIO_VIDEO_DEQUEUE_FLAG_EOS |
        VIRTIO_VIDEO_DEQUEUE_FLAG_KEY_FRAME | VIRTIO_VIDEO_DEQUEUE_FLAG_PFRAME | VIRTIO_VIDEO_DEQUEUE_FLAG_BFRAME |
        VIRTIO_VIDEO_DEQUEUE_FLAG_CODEC_CONFIG;
    const unsigned camera = VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLACK | VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED |
        VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLOCKED | VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_RESUMING;
    check((codec & camera) == 0, "camera completion flags are distinct from the codecs'");
    /* The private controls form one block clear of the standard ones. */
    check(VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP == VIRTIO_VIDEO_CAMERA_CID_BASE + 9, "private control block");
    check(UML_V4L2_CID_EXPOSURE_ABSOLUTE < VIRTIO_VIDEO_CAMERA_CID_BASE ||
              UML_V4L2_CID_EXPOSURE_ABSOLUTE > VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP,
          "private controls clear of the camera class");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

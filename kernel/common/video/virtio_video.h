/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Virtio Video Device
 *
 * This header is BSD licensed so anyone can use the definitions
 * to implement compatible drivers/servers:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of IBM nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL IBM OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Copyright (C) 2020 OpenSynergy GmbH.
 */

#ifndef _UAPI_LINUX_VIRTIO_VIDEO_H
#define _UAPI_LINUX_VIRTIO_VIDEO_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t __u8;
typedef uint32_t __le32;
typedef uint64_t __le64;
#endif

#define UML_VIRTIO_VIDEO_VERSION 1

enum virtio_video_device_type {
	VIRTIO_VIDEO_DEVICE_ENCODER = 0x0100,
	VIRTIO_VIDEO_DEVICE_DECODER,
	VIRTIO_VIDEO_DEVICE_CAMERA,
};

/*
 * Feature bits
 */
enum {
	/* Guest pages can be used for video buffers. */
	VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES = 0,
	/* The host can process buffers even if they are non-contiguous memory
	   such as scatter-gather lists. */
	VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG = 1,
	/* Support of vendor virtqueues */
	VIRTIO_VIDEO_F_VENDOR = 2,
	/* Scatter entries may name an offset in a virtio shared-memory
	   region instead of a guest-physical address. */
	VIRTIO_VIDEO_F_RESOURCE_SHM_REGION = 3,
	/* The device speaks the V4L2 control and stream-information commands
	   (VIRTIO_VIDEO_CMD_*_V4L2_CONTROLS, VIRTIO_VIDEO_CMD_*_STREAM_INFO).
	   An encoder (device ID 30) and a camera (device ID 65533) always offer
	   it, and a driver binds either only when it is negotiated. A decoder
	   never offers it. */
	VIRTIO_VIDEO_F_V4L2_CONTROLS = 4
};


#define VIRTIO_VIDEO_MAX_PLANES 8

/*
 * Image formats
 */

enum virtio_video_format {
	/* Raw formats */
	VIRTIO_VIDEO_FORMAT_RAW_MIN = 1,
	VIRTIO_VIDEO_FORMAT_ARGB8888 = VIRTIO_VIDEO_FORMAT_RAW_MIN,
	VIRTIO_VIDEO_FORMAT_BGRA8888,
	VIRTIO_VIDEO_FORMAT_RGBA8888,
	VIRTIO_VIDEO_FORMAT_NV12, /* 12  Y/CbCr 4:2:0  */
	VIRTIO_VIDEO_FORMAT_YUV420, /* 12  YUV 4:2:0     */
	VIRTIO_VIDEO_FORMAT_YVU420, /* 12  YVU 4:2:0     */
	VIRTIO_VIDEO_FORMAT_YUV422, /* 16 YUV 4:2:2 */
	VIRTIO_VIDEO_FORMAT_P010,
	VIRTIO_VIDEO_FORMAT_NV16, /* 16  Y/CbCr 4:2:2, encoder input */
	VIRTIO_VIDEO_FORMAT_NV24, /* 24  Y/CbCr 4:4:4, encoder input */
	VIRTIO_VIDEO_FORMAT_RAW_MAX = VIRTIO_VIDEO_FORMAT_NV24,

	/* Coded formats */
	VIRTIO_VIDEO_FORMAT_CODED_MIN = 0x1000,
	VIRTIO_VIDEO_FORMAT_MPEG2 =
		VIRTIO_VIDEO_FORMAT_CODED_MIN, /* MPEG-2 Part 2 */
	VIRTIO_VIDEO_FORMAT_MPEG4, /* MPEG-4 Part 2 */
	VIRTIO_VIDEO_FORMAT_H264, /* H.264 */
	VIRTIO_VIDEO_FORMAT_HEVC, /* HEVC aka H.265*/
	VIRTIO_VIDEO_FORMAT_VP8, /* VP8 */
	VIRTIO_VIDEO_FORMAT_VP9, /* VP9 */
	VIRTIO_VIDEO_FORMAT_MJPEG,
	VIRTIO_VIDEO_FORMAT_CODED_MAX = VIRTIO_VIDEO_FORMAT_MJPEG,

	/* Camera device only. Video streams use VIRTIO_VIDEO_FORMAT_NV12. */
	VIRTIO_VIDEO_FORMAT_CAMERA_MIN = 0x2000,
	/* A complete JFIF still. */
	VIRTIO_VIDEO_FORMAT_CAMERA_JPEG = VIRTIO_VIDEO_FORMAT_CAMERA_MIN,
	/* Bayer stills: 16-bit little-endian samples, right-aligned, up to the
	   camera's white level, rows at the stride the device reports. */
	VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_RGGB,
	VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_GRBG,
	VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_GBRG,
	VIRTIO_VIDEO_FORMAT_CAMERA_RAW16_BGGR,
	/* The metadata stream: one struct virtio_video_camera_meta per buffer. */
	VIRTIO_VIDEO_FORMAT_CAMERA_META,
	VIRTIO_VIDEO_FORMAT_CAMERA_MAX = VIRTIO_VIDEO_FORMAT_CAMERA_META,
};

enum virtio_video_profile {
	/* H.264 */
	VIRTIO_VIDEO_PROFILE_H264_MIN = 0x100,
	VIRTIO_VIDEO_PROFILE_H264_BASELINE = VIRTIO_VIDEO_PROFILE_H264_MIN,
	VIRTIO_VIDEO_PROFILE_H264_MAIN,
	VIRTIO_VIDEO_PROFILE_H264_EXTENDED,
	VIRTIO_VIDEO_PROFILE_H264_HIGH,
	VIRTIO_VIDEO_PROFILE_H264_HIGH10PROFILE,
	VIRTIO_VIDEO_PROFILE_H264_HIGH422PROFILE,
	VIRTIO_VIDEO_PROFILE_H264_HIGH444PREDICTIVEPROFILE,
	VIRTIO_VIDEO_PROFILE_H264_SCALABLEBASELINE,
	VIRTIO_VIDEO_PROFILE_H264_SCALABLEHIGH,
	VIRTIO_VIDEO_PROFILE_H264_STEREOHIGH,
	VIRTIO_VIDEO_PROFILE_H264_MULTIVIEWHIGH,
	VIRTIO_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE,
	VIRTIO_VIDEO_PROFILE_H264_CONSTRAINED_HIGH,
	VIRTIO_VIDEO_PROFILE_H264_MAX = VIRTIO_VIDEO_PROFILE_H264_CONSTRAINED_HIGH,

	/* HEVC */
	VIRTIO_VIDEO_PROFILE_HEVC_MIN = 0x200,
	VIRTIO_VIDEO_PROFILE_HEVC_MAIN = VIRTIO_VIDEO_PROFILE_HEVC_MIN,
	VIRTIO_VIDEO_PROFILE_HEVC_MAIN10,
	VIRTIO_VIDEO_PROFILE_HEVC_MAIN_STILL_PICTURE,
	VIRTIO_VIDEO_PROFILE_HEVC_MAX =
		VIRTIO_VIDEO_PROFILE_HEVC_MAIN_STILL_PICTURE,

	/* VP8 */
	VIRTIO_VIDEO_PROFILE_VP8_MIN = 0x300,
	VIRTIO_VIDEO_PROFILE_VP8_PROFILE0 = VIRTIO_VIDEO_PROFILE_VP8_MIN,
	VIRTIO_VIDEO_PROFILE_VP8_PROFILE1,
	VIRTIO_VIDEO_PROFILE_VP8_PROFILE2,
	VIRTIO_VIDEO_PROFILE_VP8_PROFILE3,
	VIRTIO_VIDEO_PROFILE_VP8_MAX = VIRTIO_VIDEO_PROFILE_VP8_PROFILE3,

	/* VP9 */
	VIRTIO_VIDEO_PROFILE_VP9_MIN = 0x400,
	VIRTIO_VIDEO_PROFILE_VP9_PROFILE0 = VIRTIO_VIDEO_PROFILE_VP9_MIN,
	VIRTIO_VIDEO_PROFILE_VP9_PROFILE1,
	VIRTIO_VIDEO_PROFILE_VP9_PROFILE2,
	VIRTIO_VIDEO_PROFILE_VP9_PROFILE3,
	VIRTIO_VIDEO_PROFILE_VP9_MAX = VIRTIO_VIDEO_PROFILE_VP9_PROFILE3,
};

enum virtio_video_level {
	/* H.264 */
	VIRTIO_VIDEO_LEVEL_H264_MIN = 0x100,
	VIRTIO_VIDEO_LEVEL_H264_1_0 = VIRTIO_VIDEO_LEVEL_H264_MIN,
	VIRTIO_VIDEO_LEVEL_H264_1_1,
	VIRTIO_VIDEO_LEVEL_H264_1_2,
	VIRTIO_VIDEO_LEVEL_H264_1_3,
	VIRTIO_VIDEO_LEVEL_H264_2_0,
	VIRTIO_VIDEO_LEVEL_H264_2_1,
	VIRTIO_VIDEO_LEVEL_H264_2_2,
	VIRTIO_VIDEO_LEVEL_H264_3_0,
	VIRTIO_VIDEO_LEVEL_H264_3_1,
	VIRTIO_VIDEO_LEVEL_H264_3_2,
	VIRTIO_VIDEO_LEVEL_H264_4_0,
	VIRTIO_VIDEO_LEVEL_H264_4_1,
	VIRTIO_VIDEO_LEVEL_H264_4_2,
	VIRTIO_VIDEO_LEVEL_H264_5_0,
	VIRTIO_VIDEO_LEVEL_H264_5_1,
	VIRTIO_VIDEO_LEVEL_H264_5_2,
	VIRTIO_VIDEO_LEVEL_H264_6_0,
	VIRTIO_VIDEO_LEVEL_H264_6_1,
	VIRTIO_VIDEO_LEVEL_H264_6_2,
	VIRTIO_VIDEO_LEVEL_H264_MAX = VIRTIO_VIDEO_LEVEL_H264_6_2,
    VIRTIO_VIDEO_LEVEL_HEVC_MIN = 0x200,
    VIRTIO_VIDEO_LEVEL_HEVC_1 = VIRTIO_VIDEO_LEVEL_HEVC_MIN,
    VIRTIO_VIDEO_LEVEL_HEVC_2,
    VIRTIO_VIDEO_LEVEL_HEVC_2_1,
    VIRTIO_VIDEO_LEVEL_HEVC_3,
    VIRTIO_VIDEO_LEVEL_HEVC_3_1,
    VIRTIO_VIDEO_LEVEL_HEVC_4,
    VIRTIO_VIDEO_LEVEL_HEVC_4_1,
    VIRTIO_VIDEO_LEVEL_HEVC_5,
    VIRTIO_VIDEO_LEVEL_HEVC_5_1,
    VIRTIO_VIDEO_LEVEL_HEVC_5_2,
    VIRTIO_VIDEO_LEVEL_HEVC_6,
    VIRTIO_VIDEO_LEVEL_HEVC_6_1,
    VIRTIO_VIDEO_LEVEL_HEVC_6_2,
    VIRTIO_VIDEO_LEVEL_HEVC_MAX = VIRTIO_VIDEO_LEVEL_HEVC_6_2,
};

/*
 * Config
 */

struct virtio_video_config {
	__le32 version;
	__le32 max_caps_length;
	__le32 max_resp_length;
};

/*
 * Commands
 */

enum virtio_video_cmd_type {
	/* Command */
	VIRTIO_VIDEO_CMD_QUERY_CAPABILITY = 0x0100,
	VIRTIO_VIDEO_CMD_STREAM_CREATE,
	VIRTIO_VIDEO_CMD_STREAM_DESTROY,
	VIRTIO_VIDEO_CMD_STREAM_DRAIN,
	VIRTIO_VIDEO_CMD_RESOURCE_ATTACH,
	VIRTIO_VIDEO_CMD_RESOURCE_QUEUE,
	VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES,
	VIRTIO_VIDEO_CMD_QUEUE_CLEAR,
	VIRTIO_VIDEO_CMD_GET_PARAMS,
	VIRTIO_VIDEO_CMD_SET_PARAMS,
	VIRTIO_VIDEO_CMD_QUERY_CONTROL,
	VIRTIO_VIDEO_CMD_GET_CONTROL,
	VIRTIO_VIDEO_CMD_SET_CONTROL,
	/* Encoder and camera; requires VIRTIO_VIDEO_F_V4L2_CONTROLS */
	VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS,
	VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS,
	VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS,
	VIRTIO_VIDEO_CMD_SET_STREAM_INFO,
	VIRTIO_VIDEO_CMD_GET_STREAM_INFO,
	/* Camera only; device scope except STREAM_CREATE */
	VIRTIO_VIDEO_CMD_CAMERA_QUERY,
	VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS,
	VIRTIO_VIDEO_CMD_CAMERA_QUERY_CONTROLS,
	VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE,

	/* Response */
	VIRTIO_VIDEO_RESP_OK_NODATA = 0x0200,
	VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY,
	VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE,
	VIRTIO_VIDEO_RESP_OK_GET_PARAMS,
	VIRTIO_VIDEO_RESP_OK_QUERY_CONTROL,
	VIRTIO_VIDEO_RESP_OK_GET_CONTROL,
	VIRTIO_VIDEO_RESP_OK_QUERY_V4L2_CONTROLS,
	VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS,
	VIRTIO_VIDEO_RESP_OK_STREAM_INFO,
	VIRTIO_VIDEO_RESP_OK_CAMERA_QUERY,

	VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION = 0x0300,
	VIRTIO_VIDEO_RESP_ERR_OUT_OF_MEMORY,
	VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
	VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID,
	VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
	VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL,
	/* A control that may not change while the stream is encoding was set
	   while it was; the driver normally refuses this itself (-EBUSY). */
	VIRTIO_VIDEO_RESP_ERR_BUSY,
};

struct virtio_video_cmd_hdr {
	__le32 type; /* One of enum virtio_video_cmd_type */
	__le32 stream_id;
};

/* VIRTIO_VIDEO_CMD_QUERY_CAPABILITY */
enum virtio_video_queue_type {
	VIRTIO_VIDEO_QUEUE_TYPE_INPUT = 0x100,
	VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT,
};

struct virtio_video_query_capability {
	struct virtio_video_cmd_hdr hdr;
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__u8 padding[4];
};

enum virtio_video_planes_layout_flag {
	VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER = 1 << 0,
	VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE = 1 << 1,
};

struct virtio_video_format_range {
	__le32 min;
	__le32 max;
	__le32 step;
	__u8 padding[4];
};

struct virtio_video_format_frame {
	struct virtio_video_format_range width;
	struct virtio_video_format_range height;
	__le32 num_rates;
	__u8 padding[4];
	/* Followed by struct virtio_video_format_range frame_rates[] */
};

struct virtio_video_format_desc {
	__le64 mask;
	__le32 format; /* One of VIRTIO_VIDEO_FORMAT_* types */
	__le32 planes_layout; /* Bitmask with VIRTIO_VIDEO_PLANES_LAYOUT_* */
	__le32 plane_align;
	__le32 num_frames;
	/* Followed by struct virtio_video_format_frame frames[] */
};

struct virtio_video_query_capability_resp {
	struct virtio_video_cmd_hdr hdr;
	__le32 num_descs;
	__u8 padding[4];
	/* Followed by struct virtio_video_format_desc descs[] */
};

/* VIRTIO_VIDEO_CMD_STREAM_CREATE */
enum virtio_video_mem_type {
	VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES,
};

struct virtio_video_stream_create {
	struct virtio_video_cmd_hdr hdr;
	__le32 in_mem_type; /* One of VIRTIO_VIDEO_MEM_TYPE_* types */
	__le32 out_mem_type; /* One of VIRTIO_VIDEO_MEM_TYPE_* types */
	__le32 coded_format; /* One of VIRTIO_VIDEO_FORMAT_* types */
	__u8 padding[4];
	__u8 tag[64];
};

/* VIRTIO_VIDEO_CMD_STREAM_DESTROY */
struct virtio_video_stream_destroy {
	struct virtio_video_cmd_hdr hdr;
};

/* VIRTIO_VIDEO_CMD_STREAM_DRAIN */
struct virtio_video_stream_drain {
	struct virtio_video_cmd_hdr hdr;
};

/* VIRTIO_VIDEO_CMD_RESOURCE_ATTACH */
struct virtio_video_resource_object {
	__u8 uuid [16];
};

/*
 * A scatter entry normally names guest-physical memory. An entry whose
 * VIRTIO_VIDEO_RESOURCE_SG_FLAG_SHM_REGION flag is set instead names an offset
 * in the virtio shared-memory region its flags identify, which the host
 * resolves to the memory the region's owning device published there. Such an
 * entry is only valid when VIRTIO_VIDEO_F_RESOURCE_SHM_REGION is negotiated.
 */
#define VIRTIO_VIDEO_RESOURCE_SG_FLAG_SHM_REGION (1u << 0)
#define VIRTIO_VIDEO_RESOURCE_SG_SHM_ID_SHIFT 8
#define VIRTIO_VIDEO_RESOURCE_SG_SHM_ID_MASK (0xffu << 8)
#define VIRTIO_VIDEO_RESOURCE_SG_FLAG_MASK \
	(VIRTIO_VIDEO_RESOURCE_SG_FLAG_SHM_REGION | \
	 VIRTIO_VIDEO_RESOURCE_SG_SHM_ID_MASK)

struct virtio_video_resource_sg_entry {
	__le64 addr;
	__le32 length;
	__le32 flags; /* Bitmask with VIRTIO_VIDEO_RESOURCE_SG_FLAG_* */
};

struct virtio_video_resource_sg_list {
	__le32 num_entries;
	__u8 padding[4];
	struct virtio_video_resource_sg_entry entries[];
};
#define VIRTIO_VIDEO_RESOURCE_SG_SIZE(n) \
	offsetof(struct virtio_video_resource_sg_list, entries[n])

union virtio_video_resource {
	struct virtio_video_resource_sg_list sg_list;
	struct virtio_video_resource_object object;
};

struct virtio_video_resource_attach {
	__le32 cmd_type;
	__le32 stream_id;
	__le32 queue_type; /* VIRTIO_VIDEO_QUEUE_TYPE_* */
	__le32 resource_id;
	/* Followed by struct virtio_video_resource resources[] */
};

/* VIRTIO_VIDEO_CMD_RESOURCE_QUEUE */
struct virtio_video_resource_queue {
	__le32 cmd_type;
	__le32 stream_id;
	__le32 queue_type; /* VIRTIO_VIDEO_QUEUE_TYPE_* */
	__le32 resource_id;
	__le32 flags;      /* Bitmask with VIRTIO_VIDEO_ENQUEUE_FLAG_ * */
	__u8 padding[4];
	__le64 timestamp;
	__le32 data_sizes[VIRTIO_VIDEO_MAX_PLANES];
};

enum virtio_video_dequeue_flag {
	VIRTIO_VIDEO_DEQUEUE_FLAG_ERR = 1 << 0,
	VIRTIO_VIDEO_DEQUEUE_FLAG_EOS = 1 << 1,

	/* Encoder only */
	VIRTIO_VIDEO_DEQUEUE_FLAG_KEY_FRAME = 1 << 2,
	VIRTIO_VIDEO_DEQUEUE_FLAG_PFRAME = 1 << 3,
	VIRTIO_VIDEO_DEQUEUE_FLAG_BFRAME = 1 << 4,
	/* The buffer holds only parameter sets (VPS/SPS/PPS), produced first
	   under V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE. */
	VIRTIO_VIDEO_DEQUEUE_FLAG_CODEC_CONFIG = 1 << 5,
};

/*
 * On an encoder (device ID 30):
 *  - VIRTIO_VIDEO_QUEUE_TYPE_INPUT carries raw frames (V4L2 OUTPUT) and
 *    VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT carries coded buffers (V4L2 CAPTURE).
 *  - A coded buffer's response carries the timestamp of the raw frame it
 *    encodes, data_sizes[0] = bytes written, and KEY_FRAME, PFRAME or
 *    BFRAME. H.264 and HEVC payloads are Annex B byte streams.
 *  - Coded buffers complete in decode order. The input buffer of a frame
 *    completes as soon as the host has copied it, before it is encoded.
 *  - STREAM_DRAIN completes once every queued frame is encoded; the next
 *    coded buffer then completes empty with EOS.
 *  - virtio_video_params.frame_rate is ignored; the frame interval is
 *    VIRTIO_VIDEO_CMD_SET_STREAM_INFO's.
 */

struct virtio_video_resource_queue_resp {
	struct virtio_video_cmd_hdr hdr;
	__le32 flags;
	__le64 timestamp;
	__le32 data_sizes[VIRTIO_VIDEO_MAX_PLANES];
};

/* VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES */
struct virtio_video_queue_detach_resources {
	__le32 cmd_type;
	__le32 stream_id;
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__u8 padding[4];
};

/* VIRTIO_VIDEO_CMD_QUEUE_CLEAR */
struct virtio_video_queue_clear {
	struct virtio_video_cmd_hdr hdr;
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__u8 padding[4];
};

/* VIRTIO_VIDEO_CMD_GET_PARAMS */
struct virtio_video_plane_format {
	__le32 plane_size;
	__le32 stride;
};

struct virtio_video_crop {
	__le32 left;
	__le32 top;
	__le32 width;
	__le32 height;
};

struct virtio_video_params {
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__le32 format; /* One of VIRTIO_VIDEO_FORMAT_* types */
	__le32 frame_width;
	__le32 frame_height;
	__le32 min_buffers;
	__le32 max_buffers;
	struct virtio_video_crop crop;
	__le32 frame_rate;
	__le32 num_planes;
	struct virtio_video_plane_format plane_formats[VIRTIO_VIDEO_MAX_PLANES];
};

struct virtio_video_get_params {
	struct virtio_video_cmd_hdr hdr;
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__u8 padding[4];
};

struct virtio_video_get_params_resp {
	struct virtio_video_cmd_hdr hdr;
	struct virtio_video_params params;
};

/* VIRTIO_VIDEO_CMD_SET_PARAMS */
struct virtio_video_set_params {
	struct virtio_video_cmd_hdr hdr;
	struct virtio_video_params params;
};

/* VIRTIO_VIDEO_CMD_QUERY_CONTROL */
enum virtio_video_control_type {
	VIRTIO_VIDEO_CONTROL_BITRATE = 1,
	VIRTIO_VIDEO_CONTROL_PROFILE,
	VIRTIO_VIDEO_CONTROL_LEVEL,
};

struct virtio_video_query_control_profile {
	__le32 format; /* One of VIRTIO_VIDEO_FORMAT_* */
	__u8 padding[4];
};

struct virtio_video_query_control_level {
	__le32 format; /* One of VIRTIO_VIDEO_FORMAT_* */
	__u8 padding[4];
};

struct virtio_video_query_control {
	struct virtio_video_cmd_hdr hdr;
	__le32 control; /* One of VIRTIO_VIDEO_CONTROL_* types */
	__u8 padding[4];
	/*
	 * Followed by a value of struct virtio_video_query_control_*
	 * in accordance with the value of control.
	 */
};

struct virtio_video_query_control_resp_profile {
	__le32 num;
	__u8 padding[4];
	/* Followed by an array le32 profiles[] */
};

struct virtio_video_query_control_resp_level {
	__le32 num;
	__u8 padding[4];
	/* Followed by an array le32 level[] */
};

struct virtio_video_query_control_resp {
	struct virtio_video_cmd_hdr hdr;
	/* Followed by one of struct virtio_video_query_control_resp_* */
};

/* VIRTIO_VIDEO_CMD_GET_CONTROL */
struct virtio_video_get_control {
	struct virtio_video_cmd_hdr hdr;
	__le32 control; /* One of VIRTIO_VIDEO_CONTROL_* types */
	__u8 padding[4];
};

struct virtio_video_control_val_bitrate {
	__le32 bitrate;
	__u8 padding[4];
};

struct virtio_video_control_val_profile {
	__le32 profile;
	__u8 padding[4];
};

struct virtio_video_control_val_level {
	__le32 level;
	__u8 padding[4];
};

struct virtio_video_get_control_resp {
	struct virtio_video_cmd_hdr hdr;
	/* Followed by one of struct virtio_video_control_val_* */
};

/* VIRTIO_VIDEO_CMD_SET_CONTROL */
struct virtio_video_set_control {
	struct virtio_video_cmd_hdr hdr;
	__le32 control; /* One of VIRTIO_VIDEO_CONTROL_* types */
	__u8 padding[4];
	/* Followed by one of struct virtio_video_control_val_* */
};

struct virtio_video_set_control_resp {
	struct virtio_video_cmd_hdr hdr;
};

/*
 * V4L2 controls (encoder and camera, VIRTIO_VIDEO_F_V4L2_CONTROLS)
 *
 * Controls cross the wire as V4L2 control IDs and V4L2 menu indices, which
 * are stable UAPI. The host lists every control it supports with its range,
 * and the driver registers exactly that list and forwards values unchanged,
 * so a capability the host learns needs no driver change. The driver skips a
 * descriptor whose ID it does not know.
 */

/* VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS; hdr.stream_id is 0 (device scope). */
struct virtio_video_query_v4l2_controls {
	struct virtio_video_cmd_hdr hdr;
	__le32 start_index; /* first descriptor to return */
	__u8 padding[4];
};

enum virtio_video_v4l2_control_flag {
	/* May change while the stream is encoding (otherwise the driver
	   grabs it while streaming). */
	VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC = 1 << 0,
	VIRTIO_VIDEO_V4L2_CTRL_F_READ_ONLY = 1 << 1,
	/* Read from the host on every get. */
	VIRTIO_VIDEO_V4L2_CTRL_F_VOLATILE = 1 << 2,
};

struct virtio_video_v4l2_control_desc {
	__le32 id; /* V4L2_CID_* */
	__le32 type; /* V4L2_CTRL_TYPE_* */
	__le64 minimum; /* two's-complement s64 */
	__le64 maximum;
	__le64 step;
	__le64 default_value;
	/* Menu types: bit n set means menu index n is unsupported, as the
	   skip mask of v4l2_ctrl_new_std_menu(). Zero otherwise. */
	__le64 menu_skip_mask;
	__le32 flags; /* Bitmask with VIRTIO_VIDEO_V4L2_CTRL_F_* */
	__le32 elem_size; /* Compound types: payload bytes. Zero otherwise. */
	/* Bit (format - VIRTIO_VIDEO_FORMAT_CODED_MIN) set for each coded
	   format the control applies to; informational. */
	__le64 coded_formats;
};

struct virtio_video_query_v4l2_controls_resp {
	struct virtio_video_cmd_hdr hdr;
	__le32 num_descs; /* descriptors in this response */
	__le32 next_index; /* start_index of the next query; 0 when complete */
	/* Followed by struct virtio_video_v4l2_control_desc descs[num_descs] */
};

/*
 * VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS / VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS
 *
 * A set applies every value or none, as VIDIOC_S_EXT_CTRLS does. A compound
 * value (elem_size != 0) is carried in the payload area that follows the
 * value array: size is its length and value its byte offset in that area,
 * a multiple of 8. A get request names the controls with size and value
 * zero; the response returns them in the same layout.
 */
struct virtio_video_v4l2_control_value {
	__le32 id; /* V4L2_CID_* */
	__le32 size; /* 0 for a scalar */
	__le64 value; /* scalar value, or payload offset */
};

struct virtio_video_v4l2_controls {
	struct virtio_video_cmd_hdr hdr;
	__le32 num_controls;
	__u8 padding[4];
	/* Followed by struct virtio_video_v4l2_control_value controls[]
	   and then the payload area */
};

struct virtio_video_v4l2_controls_resp {
	struct virtio_video_cmd_hdr hdr;
	/* num_controls when every control succeeded, else the index of the
	   first one that failed */
	__le32 error_index;
	__le32 num_controls;
	/* GET: followed by the values and payload area, as in the request */
};

/*
 * VIRTIO_VIDEO_CMD_SET_STREAM_INFO / VIRTIO_VIDEO_CMD_GET_STREAM_INFO
 *
 * The fractional frame interval (VIDIOC_S_PARM) and the raw frames'
 * colorimetry (the colorspace fields of VIDIOC_S_FMT on V4L2 OUTPUT), in
 * V4L2 enum values. The coded side reports the colorimetry it writes.
 */
struct virtio_video_stream_info {
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__le32 frame_interval_numerator;
	__le32 frame_interval_denominator;
	__le32 colorspace; /* enum v4l2_colorspace */
	__le32 xfer_func; /* enum v4l2_xfer_func */
	__le32 ycbcr_enc; /* enum v4l2_ycbcr_encoding */
	__le32 quantization; /* enum v4l2_quantization */
	__u8 padding[4];
};

struct virtio_video_set_stream_info {
	struct virtio_video_cmd_hdr hdr;
	struct virtio_video_stream_info info;
};

struct virtio_video_get_stream_info {
	struct virtio_video_cmd_hdr hdr;
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__u8 padding[4];
};

struct virtio_video_get_stream_info_resp {
	struct virtio_video_cmd_hdr hdr;
	struct virtio_video_stream_info info;
};


/*
 * Camera (device type VIRTIO_VIDEO_DEVICE_CAMERA)
 *
 * One device carries every camera of the host. The host publishes them with
 * VIRTIO_VIDEO_CMD_CAMERA_QUERY and the driver makes each one a capture node
 * (its video and still streams) and a metadata node (its meta streams). The
 * camera device offers VIRTIO_VIDEO_F_V4L2_CONTROLS and always
 * VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES and VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG;
 * it offers VIRTIO_VIDEO_F_RESOURCE_SHM_REGION when the host has a
 * shared-memory region to name. The queues are the codecs': 0 carries
 * commands, 1 carries events.
 *
 * Fixed-point values are Q16 (value * 65536), unsigned unless marked s32.
 * Lengths of the optics are in micrometres, frame and exposure times in
 * nanoseconds. Every record is laid out with natural alignment.
 */

/* The device ID. It is an experimental ID of the Virtio specification's local
 * range, taken after the battery (65535) and the GNSS receiver (65534). */
#define UML_VIRTIO_VIDEO_CAMERA_DEVICE_ID 65533

#define VIRTIO_VIDEO_CAMERA_MAX_CAMERAS 8
#define VIRTIO_VIDEO_CAMERA_MAX_SWITCH_OVERS 4

enum virtio_video_camera_facing {
	VIRTIO_VIDEO_CAMERA_FACING_BACK = 1,
	VIRTIO_VIDEO_CAMERA_FACING_FRONT,
	VIRTIO_VIDEO_CAMERA_FACING_EXTERNAL,
};

enum virtio_video_camera_lens {
	/* The back zoom camera: the host's virtual device, which switches between
	   lenses by zoom and is automatic only (no manual flags). */
	VIRTIO_VIDEO_CAMERA_LENS_ZOOM = 1,
	VIRTIO_VIDEO_CAMERA_LENS_WIDE,
	VIRTIO_VIDEO_CAMERA_LENS_ULTRA_WIDE,
	VIRTIO_VIDEO_CAMERA_LENS_TELEPHOTO,
	VIRTIO_VIDEO_CAMERA_LENS_EXTERNAL,
};

enum virtio_video_camera_flag {
	VIRTIO_VIDEO_CAMERA_F_AUTOFOCUS = 1 << 0,
	VIRTIO_VIDEO_CAMERA_F_FLASH = 1 << 1,
	VIRTIO_VIDEO_CAMERA_F_TORCH = 1 << 2,
	VIRTIO_VIDEO_CAMERA_F_ZOOM = 1 << 3,
	VIRTIO_VIDEO_CAMERA_F_MANUAL_EXPOSURE = 1 << 4,
	VIRTIO_VIDEO_CAMERA_F_MANUAL_FOCUS = 1 << 5,
	VIRTIO_VIDEO_CAMERA_F_MANUAL_WHITE_BALANCE = 1 << 6,
	/* Bayer stills can be taken and the colour data below is calibrated. */
	VIRTIO_VIDEO_CAMERA_F_RAW = 1 << 7,
};

enum virtio_video_camera_bayer {
	VIRTIO_VIDEO_CAMERA_BAYER_NONE = 0,
	VIRTIO_VIDEO_CAMERA_BAYER_RGGB,
	VIRTIO_VIDEO_CAMERA_BAYER_GRBG,
	VIRTIO_VIDEO_CAMERA_BAYER_GBRG,
	VIRTIO_VIDEO_CAMERA_BAYER_BGGR,
};

enum virtio_video_camera_session_flag {
	/* The session was started blocked: every frame is black and no capture
	   session ever opens. It cannot change until the guest ends. */
	VIRTIO_VIDEO_CAMERA_SESSION_F_BLOCKED = 1 << 0,
};

/* VIRTIO_VIDEO_CMD_CAMERA_QUERY; hdr.stream_id is 0. */
struct virtio_video_camera_query {
	struct virtio_video_cmd_hdr hdr;
	__le32 start_index; /* first camera to return */
	__u8 padding[4];
};

/*
 * One camera. The ranges are the camera's own; what the guest may ask for
 * inside them is the controls' (VIRTIO_VIDEO_CMD_CAMERA_QUERY_CONTROLS) and the
 * formats' (VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS). The colour data is zero,
 * and VIRTIO_VIDEO_CAMERA_F_RAW clear, until the host has learned it from a
 * Bayer still.
 */
struct virtio_video_camera_desc {
	__le32 index; /* what every other command calls the camera */
	__le32 facing; /* VIRTIO_VIDEO_CAMERA_FACING_* */
	__le32 lens; /* VIRTIO_VIDEO_CAMERA_LENS_* */
	__le32 flags; /* Bitmask with VIRTIO_VIDEO_CAMERA_F_* */
	/* Bit n: camera n cannot stream together with this one. */
	__le32 conflict_mask;
	/* Bit n: camera n can stream together with this one. */
	__le32 concurrent_mask;
	/* The zoom camera: bit n is set for each lens it switches between. */
	__le32 constituent_mask;
	/* Clockwise degrees (0, 90, 180, 270) from the sensor's top to the
	   upright side of a frame, as Android's SENSOR_ORIENTATION. */
	__le32 sensor_orientation;
	__le32 pixel_array_width; /* the full field of view, in pixels */
	__le32 pixel_array_height;
	__le32 sensor_width_um; /* approximate until calibrated */
	__le32 sensor_height_um;
	__le32 focal_length_um;
	__le32 aperture_x100; /* f-number * 100 */
	__le32 min_focus_distance_mm; /* 0: fixed focus */
	__le32 iso_min;
	__le32 iso_max;
	__u8 reserved0[4];
	__le64 exposure_min_ns;
	__le64 exposure_max_ns;
	/* The ratio Android sees: 0.5 is the ultra wide lens of a zoom camera. */
	__le32 zoom_min_q16;
	__le32 zoom_max_q16;
	__le32 num_switch_overs;
	__le32 switch_over_q16[VIRTIO_VIDEO_CAMERA_MAX_SWITCH_OVERS];
	__le32 max_wb_gain_q16;
	/* fx, fy, cx, cy in pixels of the pixel array; zero until known. */
	__le32 intrinsics_q16[4];
	__le32 bayer_order; /* VIRTIO_VIDEO_CAMERA_BAYER_*; NONE without RAW */
	__le32 black_level;
	__le32 white_level;
	__le32 raw_width;
	__le32 raw_height;
	/* DNG light-source codes of the two colour calibrations. */
	__le32 reference_illuminant1;
	__le32 reference_illuminant2;
	/* DNG matrices, row-major, s32 Q16. */
	__le32 color_matrix1_q16[9];
	__le32 color_matrix2_q16[9];
	__le32 forward_matrix1_q16[9];
	__le32 forward_matrix2_q16[9];
	__u8 reserved[20];
};

struct virtio_video_camera_query_resp {
	struct virtio_video_cmd_hdr hdr;
	__le32 num_cameras; /* in all */
	__le32 num_descs; /* in this response */
	__le32 next_index; /* start_index of the next query; 0 when complete */
	__le32 session_flags; /* Bitmask with VIRTIO_VIDEO_CAMERA_SESSION_F_* */
	__le32 desc_size; /* sizeof(struct virtio_video_camera_desc) */
	__u8 padding[4];
	/* Followed by struct virtio_video_camera_desc descs[num_descs] */
};

enum virtio_video_camera_stream_kind {
	/* NV12 frames at a frame rate, from the running capture. */
	VIRTIO_VIDEO_CAMERA_STREAM_VIDEO = 1,
	/* One JPEG or Bayer still per buffer queued while the stream runs. */
	VIRTIO_VIDEO_CAMERA_STREAM_STILL,
	/* One struct virtio_video_camera_meta per buffer. */
	VIRTIO_VIDEO_CAMERA_STREAM_META,
};

/* VIRTIO_VIDEO_CMD_CAMERA_QUERY_FORMATS. The answer is a
   VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY, as for the codecs: NV12 sizes with
   frame-rate ranges for a video stream, JPEG sizes and the Bayer size for a
   still stream. */
struct virtio_video_camera_query_formats {
	struct virtio_video_cmd_hdr hdr;
	__le32 camera_index;
	__le32 kind; /* VIRTIO_VIDEO_CAMERA_STREAM_VIDEO or _STILL */
};

/*
 * Camera controls are V4L2 camera, flash and white-balance controls wherever
 * V4L2 has one (virtio_video_v4l2_ids.h names them), with these in addition.
 * They sit in the driver-private part of the user class, clear of every
 * V4L2_CID_USER_*_BASE of the maintained kernels. A value the device does not
 * list for a camera is absent for it.
 */
#define VIRTIO_VIDEO_CAMERA_CID_BASE 0x00981c00u
/* s64: the exposure time in nanoseconds (V4L2's own unit is 100 us). */
#define VIRTIO_VIDEO_CAMERA_CID_EXPOSURE_NS (VIRTIO_VIDEO_CAMERA_CID_BASE + 0)
/* The ISO sensitivity at the device's own resolution. */
#define VIRTIO_VIDEO_CAMERA_CID_ISO (VIRTIO_VIDEO_CAMERA_CID_BASE + 1)
/* White-balance gains, Q16, when white balance is manual. */
#define VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_RED (VIRTIO_VIDEO_CAMERA_CID_BASE + 2)
#define VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_GREEN (VIRTIO_VIDEO_CAMERA_CID_BASE + 3)
#define VIRTIO_VIDEO_CAMERA_CID_WB_GAIN_BLUE (VIRTIO_VIDEO_CAMERA_CID_BASE + 4)
/* The zoom ratio Android sees, Q16 (the zoom camera goes below 1.0). */
#define VIRTIO_VIDEO_CAMERA_CID_ZOOM_Q16 (VIRTIO_VIDEO_CAMERA_CID_BASE + 5)
/* Metering and focus regions: five u32 (x, y, width, height of the region as
   fractions of the frame in Q16, and a weight 0..1000); weight 0 clears it. */
#define VIRTIO_VIDEO_CAMERA_CID_AE_REGION (VIRTIO_VIDEO_CAMERA_CID_BASE + 6)
#define VIRTIO_VIDEO_CAMERA_CID_AF_REGION (VIRTIO_VIDEO_CAMERA_CID_BASE + 7)
/* Menu: the flash of a still (0 off, 1 automatic, 2 on). */
#define VIRTIO_VIDEO_CAMERA_CID_STILL_FLASH (VIRTIO_VIDEO_CAMERA_CID_BASE + 8)
/* Menu: 0 none, 1 to 3 rotate the frames by 90, 180 or 270 degrees clockwise,
   4 automatic (the device rotates by the host's own correction, which the
   host application states). */
#define VIRTIO_VIDEO_CAMERA_CID_ROTATE_AND_CROP (VIRTIO_VIDEO_CAMERA_CID_BASE + 9)
#define VIRTIO_VIDEO_CAMERA_ROTATE_AUTOMATIC 4
/* A read-only compound control (U8[328]) that the guest driver answers, not the
   device: the struct virtio_video_camera_desc of the camera behind the node, as
   VIRTIO_VIDEO_CMD_CAMERA_QUERY gave it. The device never lists it. */
#define VIRTIO_VIDEO_CAMERA_CID_DESCRIPTION (VIRTIO_VIDEO_CAMERA_CID_BASE + 16)

/* VIRTIO_VIDEO_CMD_CAMERA_QUERY_CONTROLS: the controls of one camera, in the
   answer of VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS. The values belong to the
   camera, so a set or get through any of its streams reaches them all. */
struct virtio_video_camera_query_controls {
	struct virtio_video_cmd_hdr hdr;
	__le32 camera_index;
	__le32 start_index;
};

/* VIRTIO_VIDEO_CMD_CAMERA_STREAM_CREATE; hdr.stream_id is the new stream. */
struct virtio_video_camera_stream_create {
	struct virtio_video_cmd_hdr hdr;
	__le32 camera_index;
	__le32 kind; /* VIRTIO_VIDEO_CAMERA_STREAM_* */
	__le32 flags;
	__u8 padding[4];
	__u8 tag[64];
};

/*
 * Camera streams use the codec commands. A stream's queue is
 * VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT, the only one, and the guest queues empty
 * buffers on it. GET_PARAMS and SET_PARAMS state its format, size and the
 * plane layout the device decides (an NV12 stream is one buffer, luma then
 * chroma rows at one stride); SET_STREAM_INFO states the frame interval;
 * the device reports BT.709 full-range YCbCr for NV12. A still stream's
 * format is VIRTIO_VIDEO_FORMAT_CAMERA_JPEG or one of the RAW16 formats and
 * its buffer size is the most the device writes; a meta stream's format is
 * VIRTIO_VIDEO_FORMAT_CAMERA_META.
 *
 * A video stream completes one buffer per frame, in capture order. A still
 * stream completes a buffer once per capture it was asked to take. A guest
 * raises the capture by queueing the buffer; the controls in force when the
 * device receives it apply. Buffers queued with
 * VIRTIO_VIDEO_CAMERA_QUEUE_F_GROUP_MORE and the same non-zero timestamp (a
 * cookie of the guest's) on the still streams of one camera are filled from one
 * capture, which starts when a buffer of that cookie arrives without the flag
 * (a JPEG with its Bayer still). A group whose last buffer has not arrived two
 * seconds after its first completes with VIRTIO_VIDEO_DEQUEUE_FLAG_ERR; once
 * the capture has started it has ten seconds to deliver, and its buffers
 * complete with the same flag when it does not. A buffer also completes with
 * VIRTIO_VIDEO_DEQUEUE_FLAG_ERR when the capture is refused, with
 * VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLOCKED | _BLACK in a blocked session and
 * _INTERRUPTED while the capture is interrupted or its session cannot open;
 * a Bayer still that fails alone leaves the group's JPEG intact.
 */
#define VIRTIO_VIDEO_CAMERA_QUEUE_F_GROUP_MORE (1u << 0)

enum virtio_video_camera_dequeue_flag {
	/* The frame is black: nothing was captured. */
	VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLACK = 1 << 8,
	/* The capture is interrupted (another application, a call, multitasking,
	   system pressure). */
	VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_INTERRUPTED = 1 << 9,
	/* The session is blocked (VIRTIO_VIDEO_CAMERA_SESSION_F_BLOCKED). */
	VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_BLOCKED = 1 << 10,
	/* The capture is starting: the frame is a stand-in from the creation of
	   the stream, from the stream's restart and from the end of an
	   interruption until the host's capture delivers its first frame. */
	VIRTIO_VIDEO_DEQUEUE_FLAG_CAMERA_RESUMING = 1 << 11,
};

/* The completion of a camera buffer: the codecs' response and the number of
   the exposure it came from. `timestamp` is the frame's age in nanoseconds
   when the device completes the buffer (the driver subtracts it from its own
   clock), or the guest's cookie for a still. */
struct virtio_video_camera_queue_resp {
	struct virtio_video_resource_queue_resp base;
	/* The sequence of the struct virtio_video_camera_meta that describes
	   the exposure, in capture order from 1 for each camera. */
	__le64 sequence;
};

/* The 3A state values are Android's CONTROL_AE_STATE, CONTROL_AF_STATE and
   CONTROL_AWB_STATE. */
enum virtio_video_camera_frame_flag {
	VIRTIO_VIDEO_CAMERA_FRAME_F_BLACK = 1 << 0,
	VIRTIO_VIDEO_CAMERA_FRAME_F_INTERRUPTED = 1 << 1,
	VIRTIO_VIDEO_CAMERA_FRAME_F_BLOCKED = 1 << 2,
	VIRTIO_VIDEO_CAMERA_FRAME_F_RESUMING = 1 << 3,
	VIRTIO_VIDEO_CAMERA_FRAME_F_STILL = 1 << 4,
};

/* What the host's camera actually did for one exposure. The device writes one
   per exposure on every meta stream of the camera, and the same sequence
   rides every frame and still the exposure produced. */
struct virtio_video_camera_meta {
	__le64 sequence;
	__le64 exposure_ns;
	__le64 frame_duration_ns;
	__le32 iso;
	__le32 flags; /* Bitmask with VIRTIO_VIDEO_CAMERA_FRAME_F_* */
	__le32 ae_state;
	__le32 af_state;
	__le32 awb_state;
	/* The focus distance in dioptres; zero for a fixed-focus camera. */
	__le32 lens_position_q16;
	__le32 wb_gain_q16[3]; /* red, green, blue */
	__le32 color_temperature_k;
	__le32 tint_q16; /* s32 */
	__le32 zoom_ratio_q16;
	/* The camera index of the lens that produced the exposure. */
	__le32 active_camera;
	__le32 aperture_x100;
	__le32 focal_length_um;
	__le32 flash_state; /* Android's FLASH_STATE */
	/* Stills: the white balance and the noise model of the exposure
	   (IEEE-754 binary32 pairs (S, O) per colour channel). */
	__le32 neutral_point_q16[3];
	__le32 noise_profile_f32[8];
	/* A JPEG still: where the reserved Exif segment's payload lies in the
	   buffer, for the guest to fill in place. Zero when there is none. */
	__le32 jpeg_gap_offset;
	__le32 jpeg_gap_length;
	__u8 reserved[28];
};

/*
 * Events
 */

enum virtio_video_event_type {
	/* For all devices */
	VIRTIO_VIDEO_EVENT_ERROR = 0x0100,

	/* For decoder only */
	VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED = 0x0200,

	/* For camera only; stream_id is the camera index. */
	VIRTIO_VIDEO_EVENT_CAMERA_AVAILABLE = 0x0300,
	VIRTIO_VIDEO_EVENT_CAMERA_UNAVAILABLE,
	VIRTIO_VIDEO_EVENT_CAMERA_INTERRUPTED,
	VIRTIO_VIDEO_EVENT_CAMERA_RESUMED,
};

struct virtio_video_event {
	__le32 event_type; /* One of VIRTIO_VIDEO_EVENT_* types */
	__le32 stream_id;
};

#endif /* _UAPI_LINUX_VIRTIO_VIDEO_H */

/* SPDX-License-Identifier: BSD-3-Clause */
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
 * Copyright (C) 2019 OpenSynergy GmbH.
 */
/*
 * Virtio Video Device
 */

#ifndef _UAPI_LINUX_VIRTIO_VIDEO_H
#define _UAPI_LINUX_VIRTIO_VIDEO_H

#include <linux/types.h>
#include <linux/videodev2.h>
#include <linux/virtio_config.h>

enum virtio_video_device_type {
	VIRTIO_VIDEO_DEVICE_ENCODER = 0x0100,
	VIRTIO_VIDEO_DEVICE_DECODER,
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
	VIRTIO_VIDEO_F_VENDOR = 2
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
	VIRTIO_VIDEO_FORMAT_RAW_MAX = VIRTIO_VIDEO_FORMAT_YUV422,

	/* Coded formats */
	VIRTIO_VIDEO_FORMAT_CODED_MIN = 0x1000,
	VIRTIO_VIDEO_FORMAT_MPEG2 =
		VIRTIO_VIDEO_FORMAT_CODED_MIN, /* MPEG-2 Part 2 */
	VIRTIO_VIDEO_FORMAT_MPEG4, /* MPEG-4 Part 2 */
	VIRTIO_VIDEO_FORMAT_H264, /* H.264 */
	VIRTIO_VIDEO_FORMAT_HEVC, /* HEVC aka H.265*/
	VIRTIO_VIDEO_FORMAT_VP8, /* VP8 */
	VIRTIO_VIDEO_FORMAT_VP9, /* VP9 */
	VIRTIO_VIDEO_FORMAT_CODED_MAX = VIRTIO_VIDEO_FORMAT_VP9,
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
 * Commandq definitions
 */

enum virtio_video_cmd_type {
	/* Device */
	VIRTIO_VIDEO_CMD_DEVICE_QUERY_CAPS = 0x0100,

	/* Stream */
	VIRTIO_VIDEO_CMD_STREAM_OPEN = 0x200,
	VIRTIO_VIDEO_CMD_STREAM_CLOSE,
	VIRTIO_VIDEO_CMD_STREAM_SET_PARAMS,
	VIRTIO_VIDEO_CMD_STREAM_GET_PARAMS,
	VIRTIO_VIDEO_CMD_STREAM_UNBLOCK,
	VIRTIO_VIDEO_CMD_STREAM_DRAIN,
	VIRTIO_VIDEO_CMD_STREAM_QUEUE_RESET,
	VIRTIO_VIDEO_CMD_STREAM_RESOURCE_QUEUE,

	/* Deprecated since spec drafts v4-v9
	 * TODO: merge into QUERY_CAPS, SET/GET_PARAMS to update the
	 * implementation against the latest spec draft
	 */

	/* Device */
	VIRTIO_VIDEO_CMD_QUERY_CONTROL = 0x300,

	/* Stream */
	VIRTIO_VIDEO_CMD_RESOURCE_ATTACH = 0x400,
	VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES,
	VIRTIO_VIDEO_CMD_GET_CONTROL,
	VIRTIO_VIDEO_CMD_SET_CONTROL,
};

enum virtio_video_result_type {
	VIRTIO_VIDEO_RESULT_OK = 0,
	VIRTIO_VIDEO_RESULT_ERROR,
};

#define VIRTIO_VIDEO_QUEUE_TYPE_MAIN 0
#define VIRTIO_VIDEO_QUEUE_TYPE_INPUT 1
#define VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT 2

struct virtio_video_cmd_hdr {
	__le32 type; /* One of enum virtio_video_cmd_type */
	__le32 stream_id;
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* */
	__le32 async_response_cookie;
};

/*
 * Eventq definitions
 */

enum virtio_video_event_type {
	VIRTIO_VIDEO_ASYNC_RESP_STREAM_OPEN = 0x200,
	VIRTIO_VIDEO_ASYNC_RESP_STREAM_CLOSE,
	VIRTIO_VIDEO_ASYNC_RESP_STREAM_SET_PARAMS,
	VIRTIO_VIDEO_ASYNC_RESP_STREAM_GET_PARAMS,
	VIRTIO_VIDEO_ASYNC_RESP_STREAM_UNBLOCK,
	VIRTIO_VIDEO_ASYNC_RESP_STREAM_DRAIN,
	VIRTIO_VIDEO_ASYNC_RESP_STREAM_QUEUE_RESET,
	VIRTIO_VIDEO_ASYNC_RESP_STREAM_RESOURCE_QUEUE,

	/* Deprecated since spec drafts v4-v9
	 * TODO: merge into QUERY_CAPS, SET/GET_PARAMS to update the
	 * implementation against the latest spec draft
	 */
	VIRTIO_VIDEO_ASYNC_RESP_RESOURCE_ATTACH = 0x400,
	VIRTIO_VIDEO_ASYNC_RESP_QUEUE_DETACH_RESOURCES,
	VIRTIO_VIDEO_ASYNC_RESP_GET_CONTROL,
	VIRTIO_VIDEO_ASYNC_RESP_SET_CONTROL,

	/* Deprecated since spec draft v8
	 * TODO: merge into CLOSE, DRAIN to update the implementation against the
	 * latest spec draft
	 */
	VIRTIO_VIDEO_EVENT_ERROR = 0x500,
};

#define VIRTIO_VIDEO_EVENT_FLAG_ERROR (1 << 0)
#define VIRTIO_VIDEO_EVENT_FLAG_STANDALONE (1 << 1)
#define VIRTIO_VIDEO_EVENT_FLAG_CANCELED (1 << 2)

struct virtio_video_event_header {
	__le32 event_type; /* One of VIRTIO_VIDEO_EVENT_* types */
	__le32 stream_id;
	__le32 async_response_cookie;
	__le32 event_flags; /* Bitmask of VIRTIO_VIDEO_EVENT_FLAG_* */
};

/*
 * TLV format
 */

#define VIRTIO_VIDEO_TLV_CODED_SET 1
#define VIRTIO_VIDEO_TLV_RAW_SET 2
#define VIRTIO_VIDEO_TLV_LINK 3
#define VIRTIO_VIDEO_TLV_CODED_FORMAT 4
#define VIRTIO_VIDEO_TLV_RAW_FORMAT 5
#define VIRTIO_VIDEO_TLV_CODED_RESOURCES 6
#define VIRTIO_VIDEO_TLV_RAW_RESOURCES 7
#define VIRTIO_VIDEO_TLV_RESOURCE_GUEST_PAGES 8
#define VIRTIO_VIDEO_TLV_RESOURCE_VIRTIO_OBJECT 9
#define VIRTIO_VIDEO_TLV_CROP 10
#define VIRTIO_VIDEO_TLV_V4L2_CONTROLS 11

struct virtio_video_tlv {
	__le32 type;
	__le32 length;
	/* Followed by __u8 value[length]; */
};

struct virtio_video_range {
	__le32 min;
	__le32 max;
	__le32 step;
	__u8 padding[4];
};

struct virtio_video_tlv_v4l2_int_caps {
	struct virtio_video_range range;
};

struct virtio_video_tlv_v4l2_int_val {
	__le32 value;
};

#define MASK(x) (1 << (x))

struct virtio_video_tlv_v4l2_enum_caps {
	__le32 bitmask; /* Bitmask of MASK(<enum value>) */
};

struct virtio_video_tlv_v4l2_enum_val {
	__u8 value; /* <enum value> */
	__u8 padding[3];
};

/* Commands */

/* VIRTIO_VIDEO_CMD_DEVICE_QUERY_CAPS */
struct virtio_video_device_query_caps {
	__le32 type; /* One of enum virtio_video_cmd_type */
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_{INPUT|OUTPUT} types */
};

enum virtio_video_planes_layout_flag {
	VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER = 1 << 0,
	VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE = 1 << 1,
};

struct virtio_video_format_frame {
	struct virtio_video_range width;
	struct virtio_video_range height;
	__le32 num_rates;
	__u8 padding[4];
	/* Followed by struct virtio_video_range frame_rates[] */
};

struct virtio_video_format_desc {
	__le64 mask;
	__le32 format; /* One of VIRTIO_VIDEO_FORMAT_* types */
	__le32 planes_layout; /* Bitmask with VIRTIO_VIDEO_PLANES_LAYOUT_* */
	__le32 plane_align;
	__le32 num_frames;
	/* Followed by struct virtio_video_format_frame frames[] */
};

struct virtio_video_device_query_caps_resp {
	__le32 result; /* VIRTIO_VIDEO_RESULT_* */
	__le32 num_descs;
	/* Followed by struct virtio_video_format_desc descs[] */
};

/* VIRTIO_VIDEO_CMD_STREAM_OPEN */
enum virtio_video_mem_type {
	VIRTIO_VIDEO_MEM_TYPE_GUEST_PAGES,
};

struct virtio_video_stream_open {
	struct virtio_video_cmd_hdr hdr;
	/* hdr.queue_type must be VIRTIO_VIDEO_QUEUE_TYPE_MAIN */
	__le32 in_mem_type; /* One of VIRTIO_VIDEO_MEM_TYPE_* types */
	__le32 out_mem_type; /* One of VIRTIO_VIDEO_MEM_TYPE_* types */
	__le32 coded_format; /* One of VIRTIO_VIDEO_FORMAT_* types */
	__u8 padding[4];
	__u8 tag[64];
};

/* VIRTIO_VIDEO_CMD_STREAM_CLOSE */
struct virtio_video_stream_close {
	struct virtio_video_cmd_hdr hdr;
	/* hdr.queue_type must be VIRTIO_VIDEO_QUEUE_TYPE_MAIN */
};

/* VIRTIO_VIDEO_CMD_STREAM_DRAIN */
struct virtio_video_stream_drain {
	struct virtio_video_cmd_hdr hdr;
	/* hdr.queue_type must be VIRTIO_VIDEO_QUEUE_TYPE_MAIN for now */
};

/* VIRTIO_VIDEO_CMD_RESOURCE_ATTACH */
struct virtio_video_resource_object {
	__u8 uuid[16];
};

struct virtio_video_resource_sg_entry {
	__le64 addr;
	__le32 length;
	__u8 padding[4];
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
	struct virtio_video_cmd_hdr hdr;
	/* hdr.queue_type must be VIRTIO_VIDEO_QUEUE_TYPE_MAIN for now */
	__le32 queue_type; /* VIRTIO_VIDEO_QUEUE_TYPE_{INPUT|OUTPUT} */
	__le32 resource_id;
	/* Followed by struct virtio_video_resource resources[] */
};

/* VIRTIO_VIDEO_CMD_RESOURCE_QUEUE */

/* Buffer flags */
/* Encoder only */
#define VIRTIO_VIDEO_QUEUE_FLAG_KEY_FRAME (1 << 0)
#define VIRTIO_VIDEO_QUEUE_FLAG_P_FRAME (1 << 1)
#define VIRTIO_VIDEO_QUEUE_FLAG_B_FRAME (1 << 2)
/* Replaced in the latest drafts, keep for now */
#define VIRTIO_VIDEO_QUEUE_FLAG_ERR (1 << 16)
#define VIRTIO_VIDEO_QUEUE_FLAG_EOS (1 << 17)

struct virtio_video_resource_queue {
	struct virtio_video_cmd_hdr hdr;
	__le32 resource_id;
	__le32 flags; /* Bitmask of VIRTIO_VIDEO_QUEUE_FLAG_* */
	__le64 timestamp;
	__le32 offsets[VIRTIO_VIDEO_MAX_PLANES];
	__le32 data_sizes[VIRTIO_VIDEO_MAX_PLANES];
};

struct virtio_video_resource_queue_async_resp {
	struct virtio_video_event_header hdr;
	__le32 flags; /* Bitmask of VIRTIO_VIDEO_QUEUE_FLAG_* */
	__u8 padding[4];
	__le64 timestamp;
	__le32 offsets[VIRTIO_VIDEO_MAX_PLANES];
	__le32 data_sizes[VIRTIO_VIDEO_MAX_PLANES];
};

/* VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES */
struct virtio_video_queue_detach_resources {
	struct virtio_video_cmd_hdr hdr;
	/* hdr.queue_type must be VIRTIO_VIDEO_QUEUE_TYPE_MAIN for now */
	__le32 queue_type; /* VIRTIO_VIDEO_QUEUE_TYPE_{INPUT|OUTPUT} */
	__u8 padding[4];
};

/* VIRTIO_VIDEO_CMD_QUEUE_RESET */
struct virtio_video_queue_reset {
	struct virtio_video_cmd_hdr hdr;
	/* hdr.queue_type must be VIRTIO_VIDEO_QUEUE_TYPE_MAIN */
	__le32 reset_queue_type; /* VIRTIO_VIDEO_QUEUE_TYPE_{INPUT|OUTPUT} */
	__u8 padding[4];
};

/* VIRTIO_VIDEO_CMD_STREAM_GET_PARAMS */
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

struct virtio_video_colorimetry {
	__le32 primaries; /* enum v4l2_colorspace */
	__le32 transfer; /* enum v4l2_xfer_func */
	__le32 matrix; /* enum v4l2_ycbcr_encoding */
	__le32 range; /* enum v4l2_quantization */
};

struct virtio_video_params {
	__le32 queue_type; /* One of VIRTIO_VIDEO_QUEUE_TYPE_* types */
	__le32 format; /* One of VIRTIO_VIDEO_FORMAT_* types */
	__le32 frame_width;
	__le32 frame_height;
	__le32 min_buffers;
	__le32 max_buffers;
	struct virtio_video_crop crop;
	struct virtio_video_colorimetry colorimetry;
	__le32 frame_rate;
	__le32 num_planes;
	struct virtio_video_plane_format plane_formats[VIRTIO_VIDEO_MAX_PLANES];
};

struct virtio_video_stream_get_params {
	struct virtio_video_cmd_hdr hdr;
	/* hdr.queue_type must be VIRTIO_VIDEO_QUEUE_TYPE_MAIN for now */
	__le32 queue_type; /* VIRTIO_VIDEO_QUEUE_TYPE_{INPUT|OUTPUT} */
	__u8 padding[4];
};

struct virtio_video_stream_get_params_async_resp {
	struct virtio_video_event_header hdr;
	struct virtio_video_params params;
};

/* VIRTIO_VIDEO_CMD_STREAM_SET_PARAMS */
struct virtio_video_stream_set_params {
	struct virtio_video_cmd_hdr hdr;
	/* hdr.queue_type must be VIRTIO_VIDEO_QUEUE_TYPE_MAIN for now */
	struct virtio_video_params params;
};

/* VIRTIO_VIDEO_CMD_QUERY_CONTROL */
struct virtio_video_query_control_profile {
	__le32 format; /* One of VIRTIO_VIDEO_FORMAT_* */
	__u8 padding[4];
};

struct virtio_video_query_control_level {
	__le32 format; /* One of VIRTIO_VIDEO_FORMAT_* */
	__u8 padding[4];
};

struct virtio_video_query_control {
	__le32 type; /* One of enum virtio_video_cmd_type */
	__le32 control; /* One of V4L2_CID_* types */
	/* Followed by a value of struct virtio_video_query_control_*
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
	__le32 result; /* VIRTIO_VIDEO_RESULT_* */
	__u8 padding[4];
	/* Followed by one of struct virtio_video_query_control_resp_* */
};

/* VIRTIO_VIDEO_CMD_GET_CONTROL */
struct virtio_video_get_control {
	struct virtio_video_cmd_hdr hdr;
	/* hdr.queue_type must be VIRTIO_VIDEO_QUEUE_TYPE_MAIN for now */
	__le32 control; /* One of V4L2_CID_* types */
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

struct virtio_video_control_val_dec_display_delay_enable {
	__le32 delay_enable;
	__u8 padding[4];
};

struct virtio_video_control_val_dec_display_delay {
	__le32 delay;
	__u8 padding[4];
};

struct virtio_video_get_control_async_resp {
	struct virtio_video_event_header hdr;
	/* Followed by one of struct virtio_video_control_val_* */
};

/* VIRTIO_VIDEO_CMD_SET_CONTROL */
struct virtio_video_set_control {
	struct virtio_video_cmd_hdr hdr;
	/* hdr.queue_type must be VIRTIO_VIDEO_QUEUE_TYPE_MAIN for now */
	/* Followed by a TLV with the control */
};

#endif /* _UAPI_LINUX_VIRTIO_VIDEO_H */

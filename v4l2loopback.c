/* -*- c-file-style: "linux" -*- */
/*
 * v4l2loopback.c  --  video4linux2 loopback driver
 *
 * Copyright (C) 2005-2009 Vasily Levin (vasaka@gmail.com)
 * Copyright (C) 2010-2023 IOhannes m zmoelnig (zmoelnig@iem.at)
 * Copyright (C) 2011 Stefan Diewald (stefan.diewald@mytum.de)
 * Copyright (C) 2012 Anton Novikov (random.plant@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/time.h>
#include <linux/module.h>
#include <linux/videodev2.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/capability.h>
#include <linux/eventpoll.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-common.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>

#include <linux/miscdevice.h>
#include "v4l2loopback.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0)
#error This module is not supported on kernels before 4.0.0.
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
#define strscpy strlcpy
#endif

#if defined(timer_setup) && defined(from_timer)
#define HAVE_TIMER_SETUP
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
#define VFL_TYPE_VIDEO VFL_TYPE_GRABBER
#endif

#define V4L2LOOPBACK_VERSION_CODE                                              \
	KERNEL_VERSION(V4L2LOOPBACK_VERSION_MAJOR, V4L2LOOPBACK_VERSION_MINOR, \
		       V4L2LOOPBACK_VERSION_BUGFIX)

MODULE_DESCRIPTION("V4L2 loopback video device");
MODULE_AUTHOR("Vasily Levin, "
	      "IOhannes m zmoelnig <zmoelnig@iem.at>,"
	      "Stefan Diewald,"
	      "Anton Novikov"
	      "et al.");
#ifdef SNAPSHOT_VERSION
MODULE_VERSION(__stringify(SNAPSHOT_VERSION));
#else
MODULE_VERSION("" __stringify(V4L2LOOPBACK_VERSION_MAJOR) "." __stringify(
	V4L2LOOPBACK_VERSION_MINOR) "." __stringify(V4L2LOOPBACK_VERSION_BUGFIX));
#endif
MODULE_LICENSE("GPL");

/*
 * helpers
 */
#define dprintk(fmt, args...)                                          \
	do {                                                           \
		if (debug > 0) {                                       \
			printk(KERN_INFO "v4l2-loopback[" __stringify( \
				       __LINE__) "], pid(%d):  " fmt,  \
			       task_pid_nr(current), ##args);          \
		}                                                      \
	} while (0)

#define MARK()                                                             \
	do {                                                               \
		if (debug > 1) {                                           \
			printk(KERN_INFO "%s:%d[%s], pid(%d)\n", __FILE__, \
			       __LINE__, __func__, task_pid_nr(current));  \
		}                                                          \
	} while (0)

#define dprintkrw(fmt, args...)                                        \
	do {                                                           \
		if (debug > 2) {                                       \
			printk(KERN_INFO "v4l2-loopback[" __stringify( \
				       __LINE__) "], pid(%d): " fmt,   \
			       task_pid_nr(current), ##args);          \
		}                                                      \
	} while (0)

static inline void v4l2l_get_timestamp(struct v4l2_buffer *b)
{
	struct timespec64 ts;
	ktime_get_ts64(&ts);

	b->timestamp.tv_sec = ts.tv_sec;
	b->timestamp.tv_usec = (ts.tv_nsec / NSEC_PER_USEC);
	b->flags |= V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
}

#if BITS_PER_LONG == 32
#include <asm/div64.h> /* do_div() for 64bit division */
static inline int v4l2l_mod64(const s64 A, const u32 B)
{
	u64 a = (u64)A;
	u32 b = B;

	if (A > 0)
		return do_div(a, b);
	a = -A;
	return -do_div(a, b);
}
#else
static inline int v4l2l_mod64(const s64 A, const u32 B)
{
	return A % B;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
typedef unsigned __poll_t;
#endif

/* module constants
 *  can be overridden during he build process using something like
 *	make KCPPFLAGS="-DMAX_DEVICES=100"
 */

/* maximum number of v4l2loopback devices that can be created */
#ifndef MAX_DEVICES
#define MAX_DEVICES 8
#endif

/* whether the default is to announce capabilities exclusively or not */
#ifndef V4L2LOOPBACK_DEFAULT_EXCLUSIVECAPS
#define V4L2LOOPBACK_DEFAULT_EXCLUSIVECAPS 0
#endif

/* when a producer is considered to have gone stale */
#ifndef MAX_TIMEOUT
#define MAX_TIMEOUT (100 * 1000) /* in msecs */
#endif

/* max buffers that can be mapped, actually they
 * are all mapped to max_buffers buffers */
#ifndef MAX_BUFFERS
#define MAX_BUFFERS 32
#endif

/* module parameters */
static int debug = 3;
module_param(debug, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "debugging level (higher values == more verbose)");

// buffer数量需要将最大值由 2 改成 4，否则 HAL层 查询Camera配置时，由于请求的buffer数量小于4而报查询失败  在 ExternalCameraDeviceSession.cpp 里的 if (req_buffers.count < v4lBufferCount)
#define V4L2LOOPBACK_DEFAULT_MAX_BUFFERS 4
static int max_buffers = V4L2LOOPBACK_DEFAULT_MAX_BUFFERS;
module_param(max_buffers, int, S_IRUGO);
MODULE_PARM_DESC(max_buffers,
		 "how many buffers should be allocated [DEFAULT: " __stringify(
			 V4L2LOOPBACK_DEFAULT_MAX_BUFFERS) "]");

/* how many times a device can be opened
 * the per-module default value can be overridden on a per-device basis using
 * the /sys/devices interface
 *
 * note that max_openers should be at least 2 in order to get a working system:
 *   one opener for the producer and one opener for the consumer
 *   however, we leave that to the user
 */
#define V4L2LOOPBACK_DEFAULT_MAX_OPENERS 10
static int max_openers = V4L2LOOPBACK_DEFAULT_MAX_OPENERS;
module_param(max_openers, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(
	max_openers,
	"how many users can open the loopback device [DEFAULT: " __stringify(
		V4L2LOOPBACK_DEFAULT_MAX_OPENERS) "]");

// insmod 表示虚拟Camera的数量
static int devices = -1;
// 通过module_param_array宏 来接受通过指令赋值，eg: insmod v4l2loopback.ko devices=4
module_param(devices, int, 0);
MODULE_PARM_DESC(devices, "how many devices should be created");

// 定义一个大小为MAX_DEVICES的int数组 video_nr，元素初始值都设置为-1，表示要新增的虚拟Camera 的设备号
static int video_nr[MAX_DEVICES] = { [0 ...(MAX_DEVICES - 1)] = -1 };
// video_nr 使用 module_param_array宏 接受通过指令赋值，eg：insmod v4l2loopback.ko video_nr=2,3,5
module_param_array(video_nr, int, NULL, 0444);
MODULE_PARM_DESC(video_nr,
		 "video device numbers (-1=auto, 0=/dev/video0, etc.)");

static char *card_label[MAX_DEVICES];
module_param_array(card_label, charp, NULL, 0000);
MODULE_PARM_DESC(card_label, "card labels for each device");

static bool exclusive_caps[MAX_DEVICES] = {
	[0 ...(MAX_DEVICES - 1)] = V4L2LOOPBACK_DEFAULT_EXCLUSIVECAPS
};
module_param_array(exclusive_caps, bool, NULL, 0444);
/* FIXXME: wording */
MODULE_PARM_DESC(
	exclusive_caps,
	"whether to announce OUTPUT/CAPTURE capabilities exclusively or not  [DEFAULT: " __stringify(
		V4L2LOOPBACK_DEFAULT_EXCLUSIVECAPS) "]");

/* format specifications */
#define V4L2LOOPBACK_SIZE_MIN_WIDTH 2
#define V4L2LOOPBACK_SIZE_MIN_HEIGHT 1
#define V4L2LOOPBACK_SIZE_DEFAULT_MAX_WIDTH 8192
#define V4L2LOOPBACK_SIZE_DEFAULT_MAX_HEIGHT 8192

#define V4L2LOOPBACK_SIZE_DEFAULT_WIDTH 640
#define V4L2LOOPBACK_SIZE_DEFAULT_HEIGHT 480

static int max_width = V4L2LOOPBACK_SIZE_DEFAULT_MAX_WIDTH;
module_param(max_width, int, S_IRUGO);
MODULE_PARM_DESC(max_width,
		 "maximum allowed frame width [DEFAULT: " __stringify(
			 V4L2LOOPBACK_SIZE_DEFAULT_MAX_WIDTH) "]");
static int max_height = V4L2LOOPBACK_SIZE_DEFAULT_MAX_HEIGHT;
module_param(max_height, int, S_IRUGO);
MODULE_PARM_DESC(max_height,
		 "maximum allowed frame height [DEFAULT: " __stringify(
			 V4L2LOOPBACK_SIZE_DEFAULT_MAX_HEIGHT) "]");

static DEFINE_IDR(v4l2loopback_index_idr);
static DEFINE_MUTEX(v4l2loopback_ctl_mutex);

/* frame intervals */
#define V4L2LOOPBACK_FPS_MIN 0
#define V4L2LOOPBACK_FPS_MAX 1000

/* control IDs */
#define V4L2LOOPBACK_CID_BASE (V4L2_CID_USER_BASE | 0xf000)
#define CID_KEEP_FORMAT (V4L2LOOPBACK_CID_BASE + 0)
#define CID_SUSTAIN_FRAMERATE (V4L2LOOPBACK_CID_BASE + 1)
#define CID_TIMEOUT (V4L2LOOPBACK_CID_BASE + 2)
#define CID_TIMEOUT_IMAGE_IO (V4L2LOOPBACK_CID_BASE + 3)

static int v4l2loopback_s_ctrl(struct v4l2_ctrl *ctrl);
static const struct v4l2_ctrl_ops v4l2loopback_ctrl_ops = {
	.s_ctrl = v4l2loopback_s_ctrl,
};
static const struct v4l2_ctrl_config v4l2loopback_ctrl_keepformat = {
	// clang-format off
	.ops	= &v4l2loopback_ctrl_ops,
	.id	= CID_KEEP_FORMAT,
	.name	= "keep_format",
	.type	= V4L2_CTRL_TYPE_BOOLEAN,
	.min	= 0,
	.max	= 1,
	.step	= 1,
	.def	= 1,
	// clang-format on
};
static const struct v4l2_ctrl_config v4l2loopback_ctrl_sustainframerate = {
	// clang-format off
	.ops	= &v4l2loopback_ctrl_ops,
	.id	= CID_SUSTAIN_FRAMERATE,
	.name	= "sustain_framerate",
	.type	= V4L2_CTRL_TYPE_BOOLEAN,
	.min	= 0,
	.max	= 1,
	.step	= 1,
	.def	= 0,
	// clang-format on
};
static const struct v4l2_ctrl_config v4l2loopback_ctrl_timeout = {
	// clang-format off
	.ops	= &v4l2loopback_ctrl_ops,
	.id	= CID_TIMEOUT,
	.name	= "timeout",
	.type	= V4L2_CTRL_TYPE_INTEGER,
	.min	= 0,
	.max	= MAX_TIMEOUT,
	.step	= 1,
	.def	= 0,
	// clang-format on
};
static const struct v4l2_ctrl_config v4l2loopback_ctrl_timeoutimageio = {
	// clang-format off
	.ops	= &v4l2loopback_ctrl_ops,
	.id	= CID_TIMEOUT_IMAGE_IO,
	.name	= "timeout_image_io",
	.type	= V4L2_CTRL_TYPE_BUTTON,
	.min	= 0,
	.max	= 1,
	.step	= 1,
	.def	= 0,
	// clang-format on
};

/* module structures */
struct v4l2loopback_private {
	int device_nr;
};

/* TODO(vasaka) use typenames which are common to kernel, but first find out if
 * it is needed */
/* struct keeping state and settings of loopback device */

struct v4l2l_buffer {
	struct v4l2_buffer buffer;
	struct list_head list_head;
	int use_count;
};

struct v4l2_loopback_device {
	struct v4l2_device v4l2_dev;
	struct v4l2_ctrl_handler ctrl_handler;
	struct video_device *vdev;
	/* pixel and stream format */
	struct v4l2_pix_format pix_format;
	bool pix_format_has_valid_sizeimage;
	struct v4l2_captureparm capture_param;
	unsigned long frame_jiffies;

	/* ctrls */
	int keep_format; /* CID_KEEP_FORMAT; stay ready_for_capture even when all
			    openers close() the device */
	int sustain_framerate; /* CID_SUSTAIN_FRAMERATE; duplicate frames to maintain
				  (close to) nominal framerate */

	/* buffers stuff */
	u8 *image; /* pointer to actual buffers data */
	unsigned long int imagesize; /* size of buffers data */
	int buffers_number; /* should not be big, 4 is a good choice */
	struct v4l2l_buffer buffers[MAX_BUFFERS]; /* inner driver buffers */
	int used_buffers; /* number of the actually used buffers */
	int max_openers; /* how many times can this device be opened */

	s64 write_position; /* number of last written frame + 1 */
	struct list_head outbufs_list; /* buffers in output DQBUF order */
	int bufpos2index
		[MAX_BUFFERS]; /* mapping of (read/write_position % used_buffers)
                        * to inner buffer index */
	long buffer_size;

	/* sustain_framerate stuff */
	struct timer_list sustain_timer;
	unsigned int reread_count;

	/* timeout stuff */
	unsigned long timeout_jiffies; /* CID_TIMEOUT; 0 means disabled */
	// 用于控制v4l2设备的 I/O 超时行为。当超时发生时，将会把 timeout_image 写入到设备中。
	int timeout_image_io; /* CID_TIMEOUT_IMAGE_IO; next opener will
			       * read/write to timeout_image */
	// 一个指向超时图像数据的指针。当超时发生时，将会把这个图像数据写入到设备中。
	u8 *timeout_image; /* copy of it will be captured when timeout passes */
	struct v4l2l_buffer timeout_image_buffer;
	struct timer_list timeout_timer;
	int timeout_happened;

	/* sync stuff */
	atomic_t open_count;

	// 用于跟踪打开设备并协商格式的写入者数量。它表示设备是否准备好进行捕获操作。
	// 当有写入者打开设备并成功协商格式时，ready_for_capture的值会相应地增加, 通过检查ready_for_capture的值，可以确定设备是否已经准备好进行捕获操作。
	int ready_for_capture; /* set to the number of writers that opened the
                                * device and negotiated format. */
	int ready_for_output; /* set to true when no writer is currently attached
			       * this differs slightly from !ready_for_capture,
			       * e.g. when using fallback images */
	int active_readers; /* increase if any reader starts streaming */
	int announce_all_caps; /* set to false, if device caps (OUTPUT/CAPTURE)
                                * should only be announced if the resp. "ready"
                                * flag is set; default=TRUE */

	int min_width, max_width;
	int min_height, max_height;

	char card_label[32];

	wait_queue_head_t read_event;
	spinlock_t lock, list_lock;
};

/* types of opener shows what opener wants to do with loopback */
enum opener_type {
	// clang-format off
	UNNEGOTIATED	= 0,
	READER		= 1,
	WRITER		= 2,
	// clang-format on
};

/* struct keeping state and type of opener */
struct v4l2_loopback_opener {
	enum opener_type type;
	s64 read_position; /* number of last processed frame + 1 or
			    * write_position - 1 if reader went out of sync */
	unsigned int reread_count;
	struct v4l2_buffer *buffers;
	int buffers_number; /* should not be big, 4 is a good choice */
	int timeout_image_io;

	struct v4l2_fh fh;
};

#define fh_to_opener(ptr) container_of((ptr), struct v4l2_loopback_opener, fh)

/* this is heavily inspired by the bttv driver found in the linux kernel */
struct v4l2l_format {
	char *name;
	int fourcc; /* video4linux 2 */
	int depth; /* bit/pixel */
	int flags;
};
/* set the v4l2l_format.flags to PLANAR for non-packed formats */
#define FORMAT_FLAGS_PLANAR 0x01
#define FORMAT_FLAGS_COMPRESSED 0x02

#include "v4l2loopback_formats.h"

#ifndef V4L2_TYPE_IS_CAPTURE
#define V4L2_TYPE_IS_CAPTURE(type)                \
	((type) == V4L2_BUF_TYPE_VIDEO_CAPTURE || \
	 (type) == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
#endif /* V4L2_TYPE_IS_CAPTURE */
#ifndef V4L2_TYPE_IS_OUTPUT
#define V4L2_TYPE_IS_OUTPUT(type)                \
	((type) == V4L2_BUF_TYPE_VIDEO_OUTPUT || \
	 (type) == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
#endif /* V4L2_TYPE_IS_OUTPUT */

/* whether the format can be changed */
/* the format is fixated if we
   - have writers (ready_for_capture>0)
   - and/or have readers (active_readers>0)
*/
/**
 * 用于判断一个 V4L2 loopback 设备是否固定了其格式
 * 判断三个条件，其中任何一个为真，都认为设备的格式是固定的
 * 		1、ready_for_capture > 0	有写入者，设备的格式就不能改变。
 * 		2、active_readers > 0		有读取者，设备的格式就不能改变。
 * 		3、keep_format				设置格式一致保持，设备的格式就不能改变。
*/
#define V4L2LOOPBACK_IS_FIXED_FMT(device)                               \
	(device->ready_for_capture > 0 || device->active_readers > 0 || \
	 device->keep_format)

static const unsigned int FORMATS = ARRAY_SIZE(formats);

static char *fourcc2str(unsigned int fourcc, char buf[4])
{
	buf[0] = (fourcc >> 0) & 0xFF;
	buf[1] = (fourcc >> 8) & 0xFF;
	buf[2] = (fourcc >> 16) & 0xFF;
	buf[3] = (fourcc >> 24) & 0xFF;

	return buf;
}

static const struct v4l2l_format *format_by_fourcc(int fourcc)
{
	unsigned int i;

	for (i = 0; i < FORMATS; i++) {
		if (formats[i].fourcc == fourcc)
			return formats + i;
	}

	dprintk("unsupported format '%c%c%c%c'\n", (fourcc >> 0) & 0xFF,
		(fourcc >> 8) & 0xFF, (fourcc >> 16) & 0xFF,
		(fourcc >> 24) & 0xFF);
	return NULL;
}

static void pix_format_set_size(struct v4l2_pix_format *f,
				const struct v4l2l_format *fmt,
				unsigned int width, unsigned int height)
{
	f->width = width;
	f->height = height;

	if (fmt->flags & FORMAT_FLAGS_PLANAR) {
		f->bytesperline = width; /* Y plane */
		f->sizeimage = (width * height * fmt->depth) >> 3;
	} else if (fmt->flags & FORMAT_FLAGS_COMPRESSED) {
		/* doesn't make sense for compressed formats */
		f->bytesperline = 0;
		f->sizeimage = (width * height * fmt->depth) >> 3;
	} else {
		f->bytesperline = (width * fmt->depth) >> 3;
		f->sizeimage = height * f->bytesperline;
	}
}

static int v4l2l_fill_format(struct v4l2_format *fmt, int capture,
			     const u32 minwidth, const u32 maxwidth,
			     const u32 minheight, const u32 maxheight)
{
	u32 width = fmt->fmt.pix.width, height = fmt->fmt.pix.height;
	u32 pixelformat = fmt->fmt.pix.pixelformat;
	struct v4l2_format fmt0 = *fmt;
	u32 bytesperline = 0, sizeimage = 0;
	if (!width)
		width = V4L2LOOPBACK_SIZE_DEFAULT_WIDTH;
	if (!height)
		height = V4L2LOOPBACK_SIZE_DEFAULT_HEIGHT;
	if (width < minwidth)
		width = minwidth;
	if (width > maxwidth)
		width = maxwidth;
	if (height < minheight)
		height = minheight;
	if (height > maxheight)
		height = maxheight;

	/* sets: width,height,pixelformat,bytesperline,sizeimage */
	if (!(V4L2_TYPE_IS_MULTIPLANAR(fmt0.type))) {
		fmt0.fmt.pix.bytesperline = 0;
		fmt0.fmt.pix.sizeimage = 0;
	}

	if (0) {
		;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
	} else if (!v4l2_fill_pixfmt(&fmt0.fmt.pix, pixelformat, width,
				     height)) {
		;
	} else if (!v4l2_fill_pixfmt_mp(&fmt0.fmt.pix_mp, pixelformat, width,
					height)) {
		;
#endif
	} else {
		const struct v4l2l_format *format =
			format_by_fourcc(pixelformat);
		if (!format)
			return -EINVAL;
		pix_format_set_size(&fmt0.fmt.pix, format, width, height);
		fmt0.fmt.pix.pixelformat = format->fourcc;
	}

	if (V4L2_TYPE_IS_MULTIPLANAR(fmt0.type)) {
		*fmt = fmt0;

		if ((fmt->fmt.pix_mp.colorspace == V4L2_COLORSPACE_DEFAULT) ||
		    (fmt->fmt.pix_mp.colorspace > V4L2_COLORSPACE_DCI_P3))
			fmt->fmt.pix_mp.colorspace = V4L2_COLORSPACE_SRGB;
		if (V4L2_FIELD_ANY == fmt->fmt.pix_mp.field)
			fmt->fmt.pix_mp.field = V4L2_FIELD_NONE;
		if (capture)
			fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		else
			fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	} else {
		bytesperline = fmt->fmt.pix.bytesperline;
		sizeimage = fmt->fmt.pix.sizeimage;

		*fmt = fmt0;

		if (!fmt->fmt.pix.bytesperline)
			fmt->fmt.pix.bytesperline = bytesperline;
		if (!fmt->fmt.pix.sizeimage)
			fmt->fmt.pix.sizeimage = sizeimage;

		if ((fmt->fmt.pix.colorspace == V4L2_COLORSPACE_DEFAULT) ||
		    (fmt->fmt.pix.colorspace > V4L2_COLORSPACE_DCI_P3))
			fmt->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
		if (V4L2_FIELD_ANY == fmt->fmt.pix.field)
			fmt->fmt.pix.field = V4L2_FIELD_NONE;
		if (capture)
			fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		else
			fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	}

	return 0;
}

/* Checks if v4l2l_fill_format() has set a valid, fixed sizeimage val. */
static bool v4l2l_pix_format_has_valid_sizeimage(struct v4l2_format *fmt)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
	const struct v4l2_format_info *info;

	info = v4l2_format_info(fmt->fmt.pix.pixelformat);
	if (info && info->mem_planes == 1)
		return true;
#endif

	return false;
}

static int pix_format_eq(const struct v4l2_pix_format *ref,
			 const struct v4l2_pix_format *tgt, int strict)
{
	/* check if the two formats are equivalent.
	 * ANY fields are handled gracefully
	 */
#define _pix_format_eq0(x)    \
	if (ref->x != tgt->x) \
	result = 0
#define _pix_format_eq1(x, def)                              \
	do {                                                 \
		if ((def != tgt->x) && (ref->x != tgt->x)) { \
			printk(KERN_INFO #x " failed");      \
			result = 0;                          \
		}                                            \
	} while (0)
	int result = 1;
	_pix_format_eq0(width);
	_pix_format_eq0(height);
	_pix_format_eq0(pixelformat);
	if (!strict)
		return result;
	_pix_format_eq1(field, V4L2_FIELD_ANY);
	_pix_format_eq0(bytesperline);
	_pix_format_eq0(sizeimage);
	_pix_format_eq1(colorspace, V4L2_COLORSPACE_DEFAULT);
	return result;
}

static struct v4l2_loopback_device *v4l2loopback_getdevice(struct file *f);
static int inner_try_setfmt(struct file *file, struct v4l2_format *fmt)
{
	int capture = V4L2_TYPE_IS_CAPTURE(fmt->type);
	struct v4l2_loopback_device *dev;
	int needschange = 0;
	char buf[5];
	buf[4] = 0;

	dev = v4l2loopback_getdevice(file);

	needschange = !(pix_format_eq(&dev->pix_format, &fmt->fmt.pix, 0));
	if (V4L2LOOPBACK_IS_FIXED_FMT(dev)) {
		fmt->fmt.pix = dev->pix_format;
		if (needschange) {
			if (dev->active_readers > 0 && capture) {
				/* cannot call fmt_cap while there are readers */
				return -EBUSY;
			}
			if (dev->ready_for_capture > 0 && !capture) {
				/* cannot call fmt_out while there are writers */
				return -EBUSY;
			}
		}
	}
	if (v4l2l_fill_format(fmt, capture, dev->min_width, dev->max_width,
			      dev->min_height, dev->max_height) != 0) {
		return -EINVAL;
	}

	if (1) {
		char buf[5];
		buf[4] = 0;
		dprintk("capFOURCC=%s\n",
			fourcc2str(dev->pix_format.pixelformat, buf));
	}
	return 0;
}

static int set_timeperframe(struct v4l2_loopback_device *dev,
			    struct v4l2_fract *tpf)
{
	if ((tpf->denominator < 1) || (tpf->numerator < 1)) {
		return -EINVAL;
	}
	dev->capture_param.timeperframe = *tpf;
	dev->frame_jiffies = max(1UL, msecs_to_jiffies(1000) * tpf->numerator /
					      tpf->denominator);
	return 0;
}

static struct v4l2_loopback_device *v4l2loopback_cd2dev(struct device *cd);

/* device attributes */
/* available via sysfs: /sys/devices/virtual/video4linux/video* */

static ssize_t attr_show_format(struct device *cd,
				struct device_attribute *attr, char *buf)
{
	/* gets the current format as "FOURCC:WxH@f/s", e.g. "YUYV:320x240@1000/30" */

    // 通过通用设备指针，获取 v4l2_loopback_device 指针
	struct v4l2_loopback_device *dev = v4l2loopback_cd2dev(cd);
    dprintk("v4l2loopback attr_show_format name:%s \n", dev->vdev->name);
	const struct v4l2_fract *tpf;
	char buf4cc[5], buf_fps[32];

	if (!dev || !V4L2LOOPBACK_IS_FIXED_FMT(dev))
		return 0;

    // 获取帧率信息
	tpf = &dev->capture_param.timeperframe;

	fourcc2str(dev->pix_format.pixelformat, buf4cc);
	buf4cc[4] = 0;
	if (tpf->numerator == 1)
		snprintf(buf_fps, sizeof(buf_fps), "%d", tpf->denominator);
	else
		snprintf(buf_fps, sizeof(buf_fps), "%d/%d", tpf->denominator,
			 tpf->numerator);
    int num = sprintf(buf, "%4s:%dx%d@%s\n", buf4cc, dev->pix_format.width,
		       dev->pix_format.height, buf_fps);
    // 将buf打印出来
    dprintk("v4l2loopback attr_show_format buf:%s \n", buf);

	return num;
}

static ssize_t attr_store_format(struct device *cd,
				 struct device_attribute *attr, const char *buf,
				 size_t len)
{
	struct v4l2_loopback_device *dev = v4l2loopback_cd2dev(cd);
    dprintk("v4l2loopback attr_store_format name:%s \n", dev->vdev->name);
    int fps_num = 0, fps_den = 1;

	if (!dev)
		return -ENODEV;

	/* only fps changing is supported */
	if (sscanf(buf, "@%d/%d", &fps_num, &fps_den) > 0) {
		struct v4l2_fract f = { .numerator = fps_den,
					.denominator = fps_num };
		int err = 0;
		if ((err = set_timeperframe(dev, &f)) < 0)
			return err;
		return len;
	}
	return -EINVAL;
}

// attr_show_format 当用户空间程序尝试读取 /sys/devices/virtual/video4linux/video**/format 下的数据，此函数会被调用
//                      例如 cat /sys/devices/virtual/video4linux/video1/format  , cat到的数据其实就是 attr_show_format 函数的返回值
// attr_store_format 设置 format 这个属性时被调用
static DEVICE_ATTR(format, S_IRUGO | S_IWUSR, attr_show_format,
		   attr_store_format);

static ssize_t attr_show_buffers(struct device *cd,
				 struct device_attribute *attr, char *buf)
{
	struct v4l2_loopback_device *dev = v4l2loopback_cd2dev(cd);

	if (!dev)
		return -ENODEV;

	return sprintf(buf, "%d\n", dev->used_buffers);
}

static DEVICE_ATTR(buffers, S_IRUGO, attr_show_buffers, NULL);

static ssize_t attr_show_maxopeners(struct device *cd,
				    struct device_attribute *attr, char *buf)
{
	struct v4l2_loopback_device *dev = v4l2loopback_cd2dev(cd);

	if (!dev)
		return -ENODEV;

	return sprintf(buf, "%d\n", dev->max_openers);
}

static ssize_t attr_store_maxopeners(struct device *cd,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	struct v4l2_loopback_device *dev = NULL;
	unsigned long curr = 0;

	if (kstrtoul(buf, 0, &curr))
		return -EINVAL;

	dev = v4l2loopback_cd2dev(cd);
	if (!dev)
		return -ENODEV;

	if (dev->max_openers == curr)
		return len;

	if (curr > __INT_MAX__ || dev->open_count.counter > curr) {
		/* request to limit to less openers as are currently attached to us */
		return -EINVAL;
	}

	dev->max_openers = (int)curr;

	return len;
}

static DEVICE_ATTR(max_openers, S_IRUGO | S_IWUSR, attr_show_maxopeners,
		   attr_store_maxopeners);

static ssize_t attr_show_state(struct device *cd, struct device_attribute *attr,
			       char *buf)
{
	struct v4l2_loopback_device *dev = v4l2loopback_cd2dev(cd);

	if (!dev)
		return -ENODEV;

	if (dev->ready_for_capture)
		return sprintf(buf, "capture\n");
	if (dev->ready_for_output)
		return sprintf(buf, "output\n");

	return -EAGAIN;
}

static DEVICE_ATTR(state, S_IRUGO, attr_show_state, NULL);

static void v4l2loopback_remove_sysfs(struct video_device *vdev)
{
#define V4L2_SYSFS_DESTROY(x) device_remove_file(&vdev->dev, &dev_attr_##x)

	if (vdev) {
		V4L2_SYSFS_DESTROY(format);
		V4L2_SYSFS_DESTROY(buffers);
		V4L2_SYSFS_DESTROY(max_openers);
		V4L2_SYSFS_DESTROY(state);
		/* ... */
	}
}


/**
 * Linux 系统的 /sys 文件系统中为一个 V4L2 (Video4Linux2) 设备创建一些特定的 sysfs 属性
 * @param vdev
 */
static void v4l2loopback_create_sysfs(struct video_device *vdev)
{
	int res = 0;

#define V4L2_SYSFS_CREATE(x)                                 \
	res = device_create_file(&vdev->dev, &dev_attr_##x); \
	if (res < 0)                                         \
	break
	if (!vdev)
		return;
	do {
		V4L2_SYSFS_CREATE(format);
		V4L2_SYSFS_CREATE(buffers);
		V4L2_SYSFS_CREATE(max_openers);
		V4L2_SYSFS_CREATE(state);
		/* ... */
	} while (0);

	if (res >= 0)
		return;
	dev_err(&vdev->dev, "%s error: %d\n", __func__, res);
}

/* Event APIs */

#define V4L2LOOPBACK_EVENT_BASE (V4L2_EVENT_PRIVATE_START)
#define V4L2LOOPBACK_EVENT_OFFSET 0x08E00000
#define V4L2_EVENT_PRI_CLIENT_USAGE \
	(V4L2LOOPBACK_EVENT_BASE + V4L2LOOPBACK_EVENT_OFFSET + 1)

struct v4l2_event_client_usage {
	__u32 count;
};

/* global module data */
/* find a device based on it's device-number (e.g. '3' for /dev/video3) */
struct v4l2loopback_lookup_cb_data {
	int device_nr;
	struct v4l2_loopback_device *device;
};
static int v4l2loopback_lookup_cb(int id, void *ptr, void *data)
{
	struct v4l2_loopback_device *device = ptr;
	struct v4l2loopback_lookup_cb_data *cbdata = data;
	if (cbdata && device && device->vdev) {
		if (device->vdev->num == cbdata->device_nr) {
			cbdata->device = device;
			cbdata->device_nr = id;
			return 1;
		}
	}
	return 0;
}
static int v4l2loopback_lookup(int device_nr,
			       struct v4l2_loopback_device **device)
{
	struct v4l2loopback_lookup_cb_data data = {
		.device_nr = device_nr,
		.device = NULL,
	};
	int err = idr_for_each(&v4l2loopback_index_idr, &v4l2loopback_lookup_cb,
			       &data);
	if (1 == err) {
		if (device)
			*device = data.device;
		return data.device_nr;
	}
	return -ENODEV;
}
static struct v4l2_loopback_device *v4l2loopback_cd2dev(struct device *cd)
{
	struct video_device *loopdev = to_video_device(cd);
	struct v4l2loopback_private *ptr =
		(struct v4l2loopback_private *)video_get_drvdata(loopdev);
	int nr = ptr->device_nr;

	return idr_find(&v4l2loopback_index_idr, nr);
}

static struct v4l2_loopback_device *v4l2loopback_getdevice(struct file *f)
{
	struct v4l2loopback_private *ptr = video_drvdata(f);
	int nr = ptr->device_nr;

	return idr_find(&v4l2loopback_index_idr, nr);
}

/* forward declarations */
static void client_usage_queue_event(struct video_device *vdev);
static void init_buffers(struct v4l2_loopback_device *dev);
static int allocate_buffers(struct v4l2_loopback_device *dev);
static void free_buffers(struct v4l2_loopback_device *dev);
static void try_free_buffers(struct v4l2_loopback_device *dev);
static int allocate_timeout_image(struct v4l2_loopback_device *dev);
static void check_timers(struct v4l2_loopback_device *dev);
static const struct v4l2_file_operations v4l2_loopback_fops;
static const struct v4l2_ioctl_ops v4l2_loopback_ioctl_ops;

/* Queue helpers */
/* next functions sets buffer flags and adjusts counters accordingly */
static inline void set_done(struct v4l2l_buffer *buffer)
{
	buffer->buffer.flags &= ~V4L2_BUF_FLAG_QUEUED;
	buffer->buffer.flags |= V4L2_BUF_FLAG_DONE;
}

static inline void set_queued(struct v4l2l_buffer *buffer)
{
	buffer->buffer.flags &= ~V4L2_BUF_FLAG_DONE;
	buffer->buffer.flags |= V4L2_BUF_FLAG_QUEUED;
}

static inline void unset_flags(struct v4l2l_buffer *buffer)
{
	buffer->buffer.flags &= ~V4L2_BUF_FLAG_QUEUED;
	buffer->buffer.flags &= ~V4L2_BUF_FLAG_DONE;
}

/* V4L2 ioctl caps and params calls */
/* returns device capabilities
 * called on VIDIOC_QUERYCAP
 */
static int vidioc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct v4l2_loopback_device *dev = v4l2loopback_getdevice(file);
	int device_nr =
		((struct v4l2loopback_private *)video_get_drvdata(dev->vdev))
			->device_nr;
	__u32 capabilities = V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;

	strscpy(cap->driver, "v4l2 loopback", sizeof(cap->driver));
	snprintf(cap->card, sizeof(cap->card), "%s", dev->card_label);
	snprintf(cap->bus_info, sizeof(cap->bus_info),
		 "platform:v4l2loopback-%03d", device_nr);

	if (dev->announce_all_caps) {
		capabilities |= V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_OUTPUT;
	} else {
		if (dev->ready_for_capture) {
			capabilities |= V4L2_CAP_VIDEO_CAPTURE;
		}
		if (dev->ready_for_output) {
			capabilities |= V4L2_CAP_VIDEO_OUTPUT;
		}
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
	dev->vdev->device_caps =
#endif /* >=linux-4.7.0 */
		cap->device_caps = cap->capabilities = capabilities;

	cap->capabilities |= V4L2_CAP_DEVICE_CAPS;

	memset(cap->reserved, 0, sizeof(cap->reserved));
	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *fh,
				  struct v4l2_frmsizeenum *argp)
{
	struct v4l2_loopback_device *dev;

	/* there can be only one... */
	if (argp->index)
		return -EINVAL;

	dev = v4l2loopback_getdevice(file);
	if (V4L2LOOPBACK_IS_FIXED_FMT(dev)) {
		/* format has already been negotiated
		 * cannot change during runtime
		 */
		if (argp->pixel_format != dev->pix_format.pixelformat)
			return -EINVAL;

		argp->type = V4L2_FRMSIZE_TYPE_DISCRETE;

		argp->discrete.width = dev->pix_format.width;
		argp->discrete.height = dev->pix_format.height;
	} else {
		/* if the format has not been negotiated yet, we accept anything
		 */
		if (NULL == format_by_fourcc(argp->pixel_format))
			return -EINVAL;

		if (dev->min_width == dev->max_width &&
		    dev->min_height == dev->max_height) {
			argp->type = V4L2_FRMSIZE_TYPE_DISCRETE;

			argp->discrete.width = dev->min_width;
			argp->discrete.height = dev->min_height;
		} else {
			argp->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;

			argp->stepwise.min_width = dev->min_width;
			argp->stepwise.min_height = dev->min_height;

			argp->stepwise.max_width = dev->max_width;
			argp->stepwise.max_height = dev->max_height;

			argp->stepwise.step_width = 1;
			argp->stepwise.step_height = 1;
		}
	}
	return 0;
}

/* returns frameinterval (fps) for the set resolution
 * called on VIDIOC_ENUM_FRAMEINTERVALS
 */
static int vidioc_enum_frameintervals(struct file *file, void *fh,
				      struct v4l2_frmivalenum *argp)
{
	struct v4l2_loopback_device *dev = v4l2loopback_getdevice(file);

	/* there can be only one... */
	if (argp->index)
		return -EINVAL;

	if (V4L2LOOPBACK_IS_FIXED_FMT(dev)) {
		if (argp->width != dev->pix_format.width ||
		    argp->height != dev->pix_format.height ||
		    argp->pixel_format != dev->pix_format.pixelformat)
			return -EINVAL;

		argp->type = V4L2_FRMIVAL_TYPE_DISCRETE;
		argp->discrete = dev->capture_param.timeperframe;
	} else {
		if (argp->width < dev->min_width ||
		    argp->width > dev->max_width ||
		    argp->height < dev->min_height ||
		    argp->height > dev->max_height ||
		    NULL == format_by_fourcc(argp->pixel_format))
			return -EINVAL;

		argp->type = V4L2_FRMIVAL_TYPE_CONTINUOUS;
		argp->stepwise.min.numerator = 1;
		argp->stepwise.min.denominator = V4L2LOOPBACK_FPS_MAX;
		argp->stepwise.max.numerator = 1;
		argp->stepwise.max.denominator = V4L2LOOPBACK_FPS_MIN;
		argp->stepwise.step.numerator = 1;
		argp->stepwise.step.denominator = 1;
	}

	return 0;
}

/* ------------------ CAPTURE ----------------------- */

/* returns device formats
 * called on VIDIOC_ENUM_FMT, with v4l2_buf_type set to V4L2_BUF_TYPE_VIDEO_CAPTURE
 */
/**
 * 枚举（Enum）视频格式（Format）的函数，专用于视频捕获（Capture）操作。
 * 在 应用层发送ioctl指令 VIDIOC_ENUM_FMT 时，并且v4l2_buf_type设置为V4L2_BUF_TYPE_VIDEO_CAPTURE时，此函数会被调用。
 * 功能：根据设备是否支持固定格式来返回相应的格式描述和像素格式。
 * 		如果设备支持固定格式，那么只返回一种格式；
 * 		如果设备不支持固定格式，则返回参数无效的错误
*/
static int vidioc_enum_fmt_cap(struct file *file, void *fh,
			       struct v4l2_fmtdesc *f)
{
	struct v4l2_loopback_device *dev;
	const struct v4l2l_format *fmt;
	MARK();

	// 1. 获取 v4l2_loopback_device 指针
	dev = v4l2loopback_getdevice(file);

	if (f->index)
		return -EINVAL;
	// 2. 检查设备是否支持固定的格式 ， 如果支持固定格式，那么只返回一种格式

    // 打印 dev->keep_format
    dprintk("v4l2loopback vidioc_enum_fmt_cap dev->keep_format:%d \n", dev->keep_format);

	if (V4L2LOOPBACK_IS_FIXED_FMT(dev)) {
        MARK();
		/* format has been fixed, so only one single format is supported */
		const __u32 format = dev->pix_format.pixelformat;

		if ((fmt = format_by_fourcc(format))) {
			snprintf(f->description, sizeof(f->description), "%s",
				 fmt->name);
		} else {
			snprintf(f->description, sizeof(f->description),
				 "[%c%c%c%c]", (format >> 0) & 0xFF,
				 (format >> 8) & 0xFF, (format >> 16) & 0xFF,
				 (format >> 24) & 0xFF);
		}

		f->pixelformat = dev->pix_format.pixelformat;
	} else {
		return -EINVAL;
	}
	f->flags = 0;
	MARK();
	return 0;
}

/* returns current video format
 * called on VIDIOC_G_FMT, with v4l2_buf_type set to V4L2_BUF_TYPE_VIDEO_CAPTURE
 */
static int vidioc_g_fmt_cap(struct file *file, void *priv,
			    struct v4l2_format *fmt)
{
	struct v4l2_loopback_device *dev;
	MARK();

	dev = v4l2loopback_getdevice(file);
	if (!dev->ready_for_capture && !dev->ready_for_output)
		return -EINVAL;

	fmt->fmt.pix = dev->pix_format;
	MARK();
	return 0;
}

/* checks if it is OK to change to format fmt;
 * actual check is done by inner_try_setfmt
 * just checking that pixelformat is OK and set other parameters, app should
 * obey this decision
 * called on VIDIOC_TRY_FMT, with v4l2_buf_type set to V4L2_BUF_TYPE_VIDEO_CAPTURE
 */
static int vidioc_try_fmt_cap(struct file *file, void *priv,
			      struct v4l2_format *fmt)
{
	int ret = 0;
	if (!V4L2_TYPE_IS_CAPTURE(fmt->type))
		return -EINVAL;
	ret = inner_try_setfmt(file, fmt);
	if (-EBUSY == ret)
		return 0;
	return ret;
}

/* sets new output format, if possible
 * actually format is set  by input and we even do not check it, just return
 * current one, but it is possible to set subregions of input TODO(vasaka)
 * called on VIDIOC_S_FMT, with v4l2_buf_type set to V4L2_BUF_TYPE_VIDEO_CAPTURE
 */
/**
 *  设置（Set）视频格式（Format）的函数，专用于视频捕获（Capture）操作。
 * @param file  代表与设备文件相关联的内核文件结构的指针。
 * @param priv  指向与当前打开的文件实例关联的私有数据
 * @param fmt   包含要设置的视频格式信息的结构体。
 * @return
 */
static int vidioc_s_fmt_cap(struct file *file, void *priv,
			    struct v4l2_format *fmt)
{
	int ret;
	struct v4l2_loopback_device *dev = v4l2loopback_getdevice(file);
    // 检查格式类型是否为视频捕获类型。如果不是，返回 -EINVAL 错误。
	if (!V4L2_TYPE_IS_CAPTURE(fmt->type))
		return -EINVAL;
    // 尝试设置视频格式。函数 'inner_try_setfmt' 检查格式的可行性。
	ret = inner_try_setfmt(file, fmt);
	if (!ret) {
        // 更新设备的像素格式
		dev->pix_format = fmt->fmt.pix;
	}
	return ret;
}

/* ------------------ OUTPUT ----------------------- */

/* returns device formats;
 * LATER: allow all formats
 * called on VIDIOC_ENUM_FMT, with v4l2_buf_type set to V4L2_BUF_TYPE_VIDEO_OUTPUT
 */
static int vidioc_enum_fmt_out(struct file *file, void *fh,
			       struct v4l2_fmtdesc *f)
{
	struct v4l2_loopback_device *dev;
	const struct v4l2l_format *fmt;

	dev = v4l2loopback_getdevice(file);

	if (V4L2LOOPBACK_IS_FIXED_FMT(dev)) {
		/* format has been fixed, so only one single format is supported */
		const __u32 format = dev->pix_format.pixelformat;

		if (f->index)
			return -EINVAL;

		if ((fmt = format_by_fourcc(format))) {
			snprintf(f->description, sizeof(f->description), "%s",
				 fmt->name);
		} else {
			snprintf(f->description, sizeof(f->description),
				 "[%c%c%c%c]", (format >> 0) & 0xFF,
				 (format >> 8) & 0xFF, (format >> 16) & 0xFF,
				 (format >> 24) & 0xFF);
		}

		f->pixelformat = dev->pix_format.pixelformat;
	} else {
		/* fill in a dummy format */
		/* coverity[unsigned_compare] */
		if (f->index < 0 || f->index >= FORMATS)
			return -EINVAL;

		fmt = &formats[f->index];

		f->pixelformat = fmt->fourcc;
		snprintf(f->description, sizeof(f->description), "%s",
			 fmt->name);
	}
	f->flags = 0;

	return 0;
}

/* returns current video format format fmt */
/* NOTE: this is called from the producer
 * so if format has not been negotiated yet,
 * it should return ALL of available formats,
 * called on VIDIOC_G_FMT, with v4l2_buf_type set to V4L2_BUF_TYPE_VIDEO_OUTPUT
 */
static int vidioc_g_fmt_out(struct file *file, void *priv,
			    struct v4l2_format *fmt)
{
	struct v4l2_loopback_device *dev;
	MARK();

	dev = v4l2loopback_getdevice(file);

	/*
	 * LATER: this should return the currently valid format
	 * gstreamer doesn't like it, if this returns -EINVAL, as it
	 * then concludes that there is _no_ valid format
	 * CHECK whether this assumption is wrong,
	 * or whether we have to always provide a valid format
	 */

	fmt->fmt.pix = dev->pix_format;
	return 0;
}

/* checks if it is OK to change to format fmt;
 * if format is negotiated do not change it
 * called on VIDIOC_TRY_FMT with v4l2_buf_type set to V4L2_BUF_TYPE_VIDEO_OUTPUT
 */
static int vidioc_try_fmt_out(struct file *file, void *priv,
			      struct v4l2_format *fmt)
{
	int ret = 0;
	if (!V4L2_TYPE_IS_OUTPUT(fmt->type))
		return -EINVAL;
	ret = inner_try_setfmt(file, fmt);
	if (-EBUSY == ret)
		return 0;
	return ret;
}

/* sets new output format, if possible;
 * allocate data here because we do not know if it will be streaming or
 * read/write IO
 * called on VIDIOC_S_FMT with v4l2_buf_type set to V4L2_BUF_TYPE_VIDEO_OUTPUT
 */
/**
 * 设置视频输出格式
 * @param file  代表与设备文件相关联的内核文件结构的指针。
 * @param priv  指向与当前打开的文件实例关联的私有数据。
 * @param fmt   包含要设置的视频格式信息的结构体。
 * @return
 */
static int vidioc_s_fmt_out(struct file *file, void *priv,
			    struct v4l2_format *fmt)
{
	struct v4l2_loopback_device *dev;
	int ret;
	char buf[5];
	buf[4] = 0;
    // 检查传入的格式是否是视频输出类型。如果不是，函数返回 -EINVAL（无效参数）。
	if (!V4L2_TYPE_IS_OUTPUT(fmt->type))
		return -EINVAL;
    // 获取设备实例
	dev = v4l2loopback_getdevice(file);
    // 尝试设置视频格式。这个内部函数可能会检查格式的合理性和兼容性。
	ret = inner_try_setfmt(file, fmt);
	if (!ret) {
        // 更新设备的像素格式 dev->pix_format 为新的格式。
		dev->pix_format = fmt->fmt.pix;
        // 设置像素格式的 sizeimage 字段是否有效。
		dev->pix_format_has_valid_sizeimage =
			v4l2l_pix_format_has_valid_sizeimage(fmt);
		dprintk("s_fmt_out(%d) %d...%d\n", ret, dev->ready_for_capture,
			dev->pix_format.sizeimage);
		dprintk("outFOURCC=%s\n",
			fourcc2str(dev->pix_format.pixelformat, buf));

        // 如果设备还未准备好捕获，函数将计算缓冲区大小，并可能调用 allocate_buffers 来分配必要的缓冲区。
		if (!dev->ready_for_capture) {
			dev->buffer_size =
				PAGE_ALIGN(dev->pix_format.sizeimage);
			// JMZ: TODO get rid of the next line
			fmt->fmt.pix.sizeimage = dev->buffer_size;
			ret = allocate_buffers(dev);
		}
	}
	return ret;
}

// #define V4L2L_OVERLAY
#ifdef V4L2L_OVERLAY
/* ------------------ OVERLAY ----------------------- */
/* currently unsupported */
/* GSTreamer's v4l2sink is buggy, as it requires the overlay to work
 * while it should only require it, if overlay is requested
 * once the gstreamer element is fixed, remove the overlay dummies
 */
#warning OVERLAY dummies
static int vidioc_g_fmt_overlay(struct file *file, void *priv,
				struct v4l2_format *fmt)
{
	return 0;
}

static int vidioc_s_fmt_overlay(struct file *file, void *priv,
				struct v4l2_format *fmt)
{
	return 0;
}
#endif /* V4L2L_OVERLAY */

/* ------------------ PARAMs ----------------------- */

/* get some data flow parameters, only capability, fps and readbuffers has
 * effect on this driver
 * called on VIDIOC_G_PARM
 */
static int vidioc_g_parm(struct file *file, void *priv,
			 struct v4l2_streamparm *parm)
{
	/* do not care about type of opener, hope these enums would always be
	 * compatible */
	struct v4l2_loopback_device *dev;
	MARK();

	dev = v4l2loopback_getdevice(file);
	parm->parm.capture = dev->capture_param;
	return 0;
}

/* get some data flow parameters, only capability, fps and readbuffers has
 * effect on this driver
 * called on VIDIOC_S_PARM
 */
static int vidioc_s_parm(struct file *file, void *priv,
			 struct v4l2_streamparm *parm)
{
	struct v4l2_loopback_device *dev;
	int err = 0;
	MARK();

	dev = v4l2loopback_getdevice(file);
	dprintk("vidioc_s_parm called frate=%d/%d\n",
		parm->parm.capture.timeperframe.numerator,
		parm->parm.capture.timeperframe.denominator);

	switch (parm->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		if ((err = set_timeperframe(
			     dev, &parm->parm.capture.timeperframe)) < 0)
			return err;
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		if ((err = set_timeperframe(
			     dev, &parm->parm.capture.timeperframe)) < 0)
			return err;
		break;
	default:
		return -1;
	}

	parm->parm.capture = dev->capture_param;
	return 0;
}

#ifdef V4L2LOOPBACK_WITH_STD
/* sets a tv standard, actually we do not need to handle this any special way
 * added to support effecttv
 * called on VIDIOC_S_STD
 */
static int vidioc_s_std(struct file *file, void *fh, v4l2_std_id *_std)
{
	v4l2_std_id req_std = 0, supported_std = 0;
	const v4l2_std_id all_std = V4L2_STD_ALL, no_std = 0;

	if (_std) {
		req_std = *_std;
		*_std = all_std;
	}

	/* we support everything in V4L2_STD_ALL, but not more... */
	supported_std = (all_std & req_std);
	if (no_std == supported_std)
		return -EINVAL;

	return 0;
}

/* gets a fake video standard
 * called on VIDIOC_G_STD
 */
static int vidioc_g_std(struct file *file, void *fh, v4l2_std_id *norm)
{
	if (norm)
		*norm = V4L2_STD_ALL;
	return 0;
}
/* gets a fake video standard
 * called on VIDIOC_QUERYSTD
 */
static int vidioc_querystd(struct file *file, void *fh, v4l2_std_id *norm)
{
	if (norm)
		*norm = V4L2_STD_ALL;
	return 0;
}
#endif /* V4L2LOOPBACK_WITH_STD */

static int v4l2loopback_set_ctrl(struct v4l2_loopback_device *dev, u32 id,
				 s64 val)
{
	switch (id) {
	case CID_KEEP_FORMAT:
		if (val < 0 || val > 1)
			return -EINVAL;
		dev->keep_format = val;
        dprintk("v4l2loopback v4l2loopback_set_ctrl dev->keep_format:%d \n", dev->keep_format);
		try_free_buffers(
			dev); /* will only free buffers if !keep_format */
		break;
	case CID_SUSTAIN_FRAMERATE:
		if (val < 0 || val > 1)
			return -EINVAL;
		spin_lock_bh(&dev->lock);
		dev->sustain_framerate = val;
		check_timers(dev);
		spin_unlock_bh(&dev->lock);
		break;
	case CID_TIMEOUT:
		if (val < 0 || val > MAX_TIMEOUT)
			return -EINVAL;
		spin_lock_bh(&dev->lock);
		dev->timeout_jiffies = msecs_to_jiffies(val);
		check_timers(dev);
		spin_unlock_bh(&dev->lock);
		allocate_timeout_image(dev);
		break;
	case CID_TIMEOUT_IMAGE_IO:
		dev->timeout_image_io = 1;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int v4l2loopback_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_loopback_device *dev = container_of(
		ctrl->handler, struct v4l2_loopback_device, ctrl_handler);
	return v4l2loopback_set_ctrl(dev, ctrl->id, ctrl->val);
}

/* returns set of device outputs, in our case there is only one
 * called on VIDIOC_ENUMOUTPUT
 */
static int vidioc_enum_output(struct file *file, void *fh,
			      struct v4l2_output *outp)
{
	__u32 index = outp->index;
	struct v4l2_loopback_device *dev = v4l2loopback_getdevice(file);
	MARK();

	if (!dev->announce_all_caps && !dev->ready_for_output)
		return -ENOTTY;

	if (0 != index)
		return -EINVAL;

	/* clear all data (including the reserved fields) */
	memset(outp, 0, sizeof(*outp));

	outp->index = index;
	strscpy(outp->name, "loopback in", sizeof(outp->name));
	outp->type = V4L2_OUTPUT_TYPE_ANALOG;
	outp->audioset = 0;
	outp->modulator = 0;
#ifdef V4L2LOOPBACK_WITH_STD
	outp->std = V4L2_STD_ALL;
#ifdef V4L2_OUT_CAP_STD
	outp->capabilities |= V4L2_OUT_CAP_STD;
#endif /*  V4L2_OUT_CAP_STD */
#endif /* V4L2LOOPBACK_WITH_STD */

	return 0;
}

/* which output is currently active,
 * called on VIDIOC_G_OUTPUT
 */
static int vidioc_g_output(struct file *file, void *fh, unsigned int *i)
{
	struct v4l2_loopback_device *dev = v4l2loopback_getdevice(file);
	if (!dev->announce_all_caps && !dev->ready_for_output)
		return -ENOTTY;
	if (i)
		*i = 0;
	return 0;
}

/* set output, can make sense if we have more than one video src,
 * called on VIDIOC_S_OUTPUT
 */
static int vidioc_s_output(struct file *file, void *fh, unsigned int i)
{
	struct v4l2_loopback_device *dev = v4l2loopback_getdevice(file);
	if (!dev->announce_all_caps && !dev->ready_for_output)
		return -ENOTTY;

	if (i)
		return -EINVAL;

	return 0;
}

/* returns set of device inputs, in our case there is only one,
 * but later I may add more
 * called on VIDIOC_ENUMINPUT
 */
static int vidioc_enum_input(struct file *file, void *fh,
			     struct v4l2_input *inp)
{
	struct v4l2_loopback_device *dev;
	__u32 index = inp->index;
	MARK();

	if (0 != index)
		return -EINVAL;

	/* clear all data (including the reserved fields) */
	memset(inp, 0, sizeof(*inp));

	inp->index = index;
	strscpy(inp->name, "loopback", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	inp->audioset = 0;
	inp->tuner = 0;
	inp->status = 0;

#ifdef V4L2LOOPBACK_WITH_STD
	inp->std = V4L2_STD_ALL;
#ifdef V4L2_IN_CAP_STD
	inp->capabilities |= V4L2_IN_CAP_STD;
#endif
#endif /* V4L2LOOPBACK_WITH_STD */

	dev = v4l2loopback_getdevice(file);
	if (!dev->ready_for_capture) {
		inp->status |= V4L2_IN_ST_NO_SIGNAL;
	}

	return 0;
}

/* which input is currently active,
 * called on VIDIOC_G_INPUT
 */
static int vidioc_g_input(struct file *file, void *fh, unsigned int *i)
{
	struct v4l2_loopback_device *dev = v4l2loopback_getdevice(file);
	if (!dev->announce_all_caps && !dev->ready_for_capture)
		return -ENOTTY;
	if (i)
		*i = 0;
	return 0;
}

/* set input, can make sense if we have more than one video src,
 * called on VIDIOC_S_INPUT
 */
static int vidioc_s_input(struct file *file, void *fh, unsigned int i)
{
	struct v4l2_loopback_device *dev = v4l2loopback_getdevice(file);
	if (!dev->announce_all_caps && !dev->ready_for_capture)
		return -ENOTTY;
	if (i == 0)
		return 0;
	return -EINVAL;
}

/* --------------- V4L2 ioctl buffer related calls ----------------- */

/* negotiate buffer type
 * only mmap streaming supported
 * called on VIDIOC_REQBUFS
 */
/**
 * 处理应用程序的缓冲区请求，分配一定数量的缓冲区
*/
static int vidioc_reqbufs(struct file *file, void *fh,
			  struct v4l2_requestbuffers *b)
{
	struct v4l2_loopback_device *dev;
	struct v4l2_loopback_opener *opener;
	int i;
	MARK();

	// 获取设备和打开者的信息
	dev = v4l2loopback_getdevice(file);
	opener = fh_to_opener(fh);

	dprintk("reqbufs: %d\t%d=%d\n", b->memory, b->count,
		dev->buffers_number);

	// 如果设置了超时图像IO，只支持MMAP内存类型，缓冲区数量设置为2
	if (opener->timeout_image_io) {
		dev->timeout_image_io = 0;
		if (b->memory != V4L2_MEMORY_MMAP)
			return -EINVAL;
		b->count = 2;
		return 0;
	}

	// 如果设备不准备好输出，返回EBUSY
	if (V4L2_TYPE_IS_OUTPUT(b->type) && (!dev->ready_for_output)) {
		return -EBUSY;
	}

	// 初始化缓冲区
	init_buffers(dev);
	switch (b->memory) {
	case V4L2_MEMORY_MMAP:
		/* do nothing here, buffers are always allocated */
		// 如果请求的缓冲区数量小于1或设备的缓冲区数量小于1，直接返回
		if (b->count < 1 || dev->buffers_number < 1)
			return 0;

		// 如果请求的缓冲区数量大于设备的缓冲区数量，将请求的缓冲区数量设置为设备的缓冲区数量
		if (b->count > dev->buffers_number)
			b->count = dev->buffers_number;

		/* make sure that outbufs_list contains buffers from 0 to used_buffers-1
		 * actually, it will have been already populated via v4l2_loopback_init()
		 * at this point */
		// 如果outbufs_list为空，将设备的缓冲区添加到outbufs_list
		if (list_empty(&dev->outbufs_list)) {
			for (i = 0; i < dev->used_buffers; ++i)
				list_add_tail(&dev->buffers[i].list_head,
					      &dev->outbufs_list);
		}

		/* also, if dev->used_buffers is going to be decreased, we should remove
		 * out-of-range buffers from outbufs_list, and fix bufpos2index mapping */
		// 如果请求的缓冲区数量小于设备使用的缓冲区数量，需要从outbufs_list中删除超出范围的缓冲区，并修复bufpos2index映射
		if (b->count < dev->used_buffers) {
			struct v4l2l_buffer *pos, *n;

			list_for_each_entry_safe(pos, n, &dev->outbufs_list,
						 list_head) {
				if (pos->buffer.index >= b->count)
					list_del(&pos->list_head);
			}

			/* after we update dev->used_buffers, buffers in outbufs_list will
			 * correspond to dev->write_position + [0;b->count-1] range */
            // 更新bufpos2index映射
            i = v4l2l_mod64(dev->write_position, b->count);
			list_for_each_entry(pos, &dev->outbufs_list,
					    list_head) {
				dev->bufpos2index[i % b->count] =
					pos->buffer.index;
				++i;
			}
		}

		// 更新打开者和设备的缓冲区数量
		opener->buffers_number = b->count;
		if (opener->buffers_number < dev->used_buffers)
			dev->used_buffers = opener->buffers_number;
		return 0;
	default:
		return -EINVAL;
	}
}

/* returns buffer asked for;
 * give app as many buffers as it wants, if it less than MAX,
 * but map them in our inner buffers
 * called on VIDIOC_QUERYBUF
 */
/**
 * 查询缓冲区的状态
 * 通过 "VIDIOC_QUERYBUF" ioctl命令，应用程序可以查询缓冲区的状态。
 * 包括检查缓冲区的类型和索引，返回请求的缓冲区的信息，以及设置返回的缓冲区的标志。
*/
static int vidioc_querybuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	enum v4l2_buf_type type;
	int index;
	struct v4l2_loopback_device *dev;
	struct v4l2_loopback_opener *opener;

	MARK();

	// 获取缓冲区的类型和索引
	type = b->type;
	index = b->index;
	// 获取设备和打开者的信息
	dev = v4l2loopback_getdevice(file);
	opener = fh_to_opener(fh);

	// 检查缓冲区的类型是否正确
	if ((b->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) &&
	    (b->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)) {
		return -EINVAL;
	}
	// 检查缓冲区的索引是否超过了最大值
	if (b->index > max_buffers)
		return -EINVAL;

	// 如果打开者使用了超时图像IO，那么返回超时图像缓冲区的信息
    // 否则返回请求的缓冲区的信息
	if (opener->timeout_image_io)
		*b = dev->timeout_image_buffer.buffer;
	else
		*b = dev->buffers[b->index % dev->used_buffers].buffer;

	// 设置返回的缓冲区的类型和索引
	b->type = type;
	b->index = index;
	dprintkrw("buffer type: %d (of %d with size=%ld)\n", b->memory,
		  dev->buffers_number, dev->buffer_size);

	/*  Hopefully fix 'DQBUF return bad index if queue bigger then 2 for capture'
            https://github.com/umlaeute/v4l2loopback/issues/60 */

	// 清除V4L2_BUF_FLAG_DONE标志，表示这个缓冲区还没有完成
	b->flags &= ~V4L2_BUF_FLAG_DONE;
	// 设置返回的缓冲区的标志为V4L2_BUF_FLAG_QUEUED，表示这个缓冲区已经被放入队列
	b->flags |= V4L2_BUF_FLAG_QUEUED;

	return 0;
}

static void buffer_written(struct v4l2_loopback_device *dev,
			   struct v4l2l_buffer *buf)
{
	del_timer_sync(&dev->sustain_timer);
	del_timer_sync(&dev->timeout_timer);

	spin_lock_bh(&dev->list_lock);
	list_move_tail(&buf->list_head, &dev->outbufs_list);
	spin_unlock_bh(&dev->list_lock);

	spin_lock_bh(&dev->lock);
	dev->bufpos2index[v4l2l_mod64(dev->write_position, dev->used_buffers)] =
		buf->buffer.index;
	++dev->write_position;
	dev->reread_count = 0;

	check_timers(dev);
	spin_unlock_bh(&dev->lock);
}

/* put buffer to queue
 * called on VIDIOC_QBUF
 */
/**
 * 处理应用程序的缓冲区入队请求
 * 通过 "VIDIOC_QBUF" ioctl命令，应用程序可以将缓冲区入队
 * 包括检查缓冲区的索引，获取请求的缓冲区的信息，以及根据缓冲区的类型进行不同的处理。
*/
static int vidioc_qbuf(struct file *file, void *fh, struct v4l2_buffer *buf)
{
	struct v4l2_loopback_device *dev;
	struct v4l2_loopback_opener *opener;
	struct v4l2l_buffer *b;
	int index;

    MARK();

	// 获取设备和打开者的信息
	dev = v4l2loopback_getdevice(file);
	opener = fh_to_opener(fh);

	// 检查缓冲区的索引是否超过了最大值
	if (buf->index > max_buffers)
		return -EINVAL;
	// 如果打开者使用了超时图像IO，那么直接返回
	if (opener->timeout_image_io)
		return 0;

	// 获取请求的缓冲区的信息
	index = buf->index % dev->used_buffers;
	b = &dev->buffers[index];

	// 根据缓冲区的类型进行不同的处理
	switch (buf->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		dprintkrw(
			"qbuf(CAPTURE)#%d: buffer#%d @ %p type=%d bytesused=%d length=%d flags=%x field=%d timestamp=%lld.%06ld sequence=%d\n",
			index, buf->index, buf, buf->type, buf->bytesused,
			buf->length, buf->flags, buf->field,
			(long long)buf->timestamp.tv_sec,
			(long int)buf->timestamp.tv_usec, buf->sequence);
		// 如果是捕获类型的缓冲区，那么将其设置为已入队
		set_queued(b);
		return 0;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		dprintkrw(
			"qbuf(OUTPUT)#%d: buffer#%d @ %p type=%d bytesused=%d length=%d flags=%x field=%d timestamp=%lld.%06ld sequence=%d\n",
			index, buf->index, buf, buf->type, buf->bytesused,
			buf->length, buf->flags, buf->field,
			(long long)buf->timestamp.tv_sec,
			(long int)buf->timestamp.tv_usec, buf->sequence);
		// 如果是输出类型的缓冲区，那么进行一些处理，包括设置时间戳，检查和设置使用的字节数，将其设置为已完成，并唤醒所有等待读事件的进程
		if ((!(b->buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_COPY)) &&
		    (buf->timestamp.tv_sec == 0 && buf->timestamp.tv_usec == 0))
			v4l2l_get_timestamp(&b->buffer);
		else {
			b->buffer.timestamp = buf->timestamp;
			b->buffer.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
		}
		if (dev->pix_format_has_valid_sizeimage) {
			if (buf->bytesused >= dev->pix_format.sizeimage) {
				b->buffer.bytesused = dev->pix_format.sizeimage;
			} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
				dev_warn_ratelimited(
					&dev->vdev->dev,
#else
				dprintkrw(
#endif
					"warning queued output buffer bytesused too small %d < %d\n",
					buf->bytesused,
					dev->pix_format.sizeimage);
				b->buffer.bytesused = buf->bytesused;
			}
		} else {
			b->buffer.bytesused = buf->bytesused;
		}

		set_done(b);
		buffer_written(dev, b);

		/*  Hopefully fix 'DQBUF return bad index if queue bigger then 2 for capture'
                    https://github.com/umlaeute/v4l2loopback/issues/60 */
		buf->flags &= ~V4L2_BUF_FLAG_DONE;
		buf->flags |= V4L2_BUF_FLAG_QUEUED;

		wake_up_all(&dev->read_event);
		return 0;
	default:
		return -EINVAL;
	}
}

static int can_read(struct v4l2_loopback_device *dev,
		    struct v4l2_loopback_opener *opener)
{
	int ret;

	spin_lock_bh(&dev->lock);
	check_timers(dev);
	ret = dev->write_position > opener->read_position ||
	      dev->reread_count > opener->reread_count || dev->timeout_happened;
	spin_unlock_bh(&dev->lock);
	return ret;
}

static int get_capture_buffer(struct file *file)
{
	struct v4l2_loopback_device *dev = v4l2loopback_getdevice(file);
	struct v4l2_loopback_opener *opener = fh_to_opener(file->private_data);
	int pos, ret;
	int timeout_happened;

	if ((file->f_flags & O_NONBLOCK) &&
	    (dev->write_position <= opener->read_position &&
	     dev->reread_count <= opener->reread_count &&
	     !dev->timeout_happened))
		return -EAGAIN;
	wait_event_interruptible(dev->read_event, can_read(dev, opener));

	spin_lock_bh(&dev->lock);
	if (dev->write_position == opener->read_position) {
		if (dev->reread_count > opener->reread_count + 2)
			opener->reread_count = dev->reread_count - 1;
		++opener->reread_count;
		pos = v4l2l_mod64(opener->read_position + dev->used_buffers - 1,
				  dev->used_buffers);
	} else {
		opener->reread_count = 0;
		if (dev->write_position >
		    opener->read_position + dev->used_buffers)
			opener->read_position = dev->write_position - 1;
		pos = v4l2l_mod64(opener->read_position, dev->used_buffers);
		++opener->read_position;
	}
	timeout_happened = dev->timeout_happened;
	dev->timeout_happened = 0;
	spin_unlock_bh(&dev->lock);

	ret = dev->bufpos2index[pos];
	if (timeout_happened) {
		if (ret < 0) {
			dprintk("trying to return not mapped buf[%d]\n", ret);
			return -EFAULT;
		}
		/* although allocated on-demand, timeout_image is freed only
		 * in free_buffers(), so we don't need to worry about it being
		 * deallocated suddenly */
		memcpy(dev->image + dev->buffers[ret].buffer.m.offset,
		       dev->timeout_image, dev->buffer_size);
	}
	return ret;
}

/* put buffer to dequeue
 * called on VIDIOC_DQBUF
 */
/**
 * 从设备中取出一个缓冲区
 * 当 VIDIOC_DQBUF 命令被调用时，该函数会被触发
*/
static int vidioc_dqbuf(struct file *file, void *fh, struct v4l2_buffer *buf)
{
	struct v4l2_loopback_device *dev;
	struct v4l2_loopback_opener *opener;
	int index;
	struct v4l2l_buffer *b;

    MARK();

	// 获取设备和打开者的信息
	dev = v4l2loopback_getdevice(file);
	opener = fh_to_opener(fh);
    // 如果有超时buffer，把超时buffer给出去
	if (opener->timeout_image_io) {
		*buf = dev->timeout_image_buffer.buffer;
		return 0;
	}

	switch (buf->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		// 获取缓冲区的 index
		index = get_capture_buffer(file);
		if (index < 0)
			return index;

		// 打印读取位置和index
		dprintkrw("capture DQBUF pos: %lld index: %d\n",
			  (long long)(opener->read_position - 1), index);
		// 如果缓冲区没有被映射，返回错误, 这里逻辑上层 ExternalCameraDeviceSession 的逻辑冲突，注释掉
//		if (!(dev->buffers[index].buffer.flags &
//		      V4L2_BUF_FLAG_MAPPED)) {
//			dprintk("trying to return not mapped buf[%d]\n", index);
//			return -EINVAL;
//		}
		// 清除缓冲区的标志
		unset_flags(&dev->buffers[index]);
		// 返回缓冲区
		*buf = dev->buffers[index].buffer;
		dprintkrw(
			"dqbuf(CAPTURE)#%d: buffer#%d @ %p type=%d bytesused=%d length=%d flags=%x field=%d timestamp=%lld.%06ld sequence=%d\n",
			index, buf->index, buf, buf->type, buf->bytesused,
			buf->length, buf->flags, buf->field,
			(long long)buf->timestamp.tv_sec,
			(long int)buf->timestamp.tv_usec, buf->sequence);
		return 0;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		spin_lock_bh(&dev->list_lock);

		b = list_entry(dev->outbufs_list.prev, struct v4l2l_buffer,
			       list_head);
		list_move_tail(&b->list_head, &dev->outbufs_list);

		spin_unlock_bh(&dev->list_lock);
		dprintkrw("output DQBUF index: %d\n", b->buffer.index);
		// 清除缓冲区的标志
		unset_flags(b);
		// 返回缓冲区
		*buf = b->buffer;
		buf->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		dprintkrw(
			"dqbuf(OUTPUT)#%d: buffer#%d @ %p type=%d bytesused=%d length=%d flags=%x field=%d timestamp=%lld.%06ld sequence=%d\n",
			index, buf->index, buf, buf->type, buf->bytesused,
			buf->length, buf->flags, buf->field,
			(long long)buf->timestamp.tv_sec,
			(long int)buf->timestamp.tv_usec, buf->sequence);
		return 0;
	default:
		return -EINVAL;
	}
}

/* ------------- STREAMING ------------------- */

/* start streaming
 * called on VIDIOC_STREAMON
 */
/**
 * 开始视频流
 * 当 VIDIOC_STREAMON 命令被调用时，该函数会被触发。
 * @param file  设备文件的文件结构体指针
 * @param fh    表示打开的设备文件的文件句柄
 * @param type  表示视频流的类型
*/
static int vidioc_streamon(struct file *file, void *fh, enum v4l2_buf_type type)
{
	struct v4l2_loopback_device *dev;
	struct v4l2_loopback_opener *opener;
	MARK();

	dev = v4l2loopback_getdevice(file);
	opener = fh_to_opener(fh);

	// 根据缓冲区类型进行不同的处理
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
	 	// 如果设备还未准备好捕获视频，则尝试分配缓冲区
		if (!dev->ready_for_capture) {
			int ret = allocate_buffers(dev);
            dprintk("v4l2loopback vidioc_streamon allocate_buffers ret:%d \n", ret);
			if (ret < 0)
				return ret; // 如果分配缓冲区失败，则返回错误码
		}
		opener->type = WRITER;	// 设置打开的设备文件的类型为写入者
		dev->ready_for_output = 0;	// 设置设备未准备好输出视频
		dev->ready_for_capture++;	// 增加设备的捕获准备计数
		return 0;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		// 如果设备还未准备好捕获视频，则返回错误码
		if (!dev->ready_for_capture) {
            // 打印日志
            dprintk("v4l2loopback vidioc_streamon dev->ready_for_capture:%d \n", dev->ready_for_capture);
            return -EIO;
        }
		// 如果设备已经有活动的读取者，则返回错误码
		if (dev->active_readers > 0) {
            // 打印日志
            dprintk("v4l2loopback vidioc_streamon dev->active_readers:%d \n", dev->active_readers);
            return -EBUSY;
        }
        // 打印出 dev->active_readers
//        printk("v4l2loopback vidioc_streamon dev->active_readers:%d \n", dev->active_readers);
		opener->type = READER;	// 设置打开的设备文件的类型为读取者
		dev->active_readers++;	// 增加设备的活动读取者计数
		client_usage_queue_event(dev->vdev);
		return 0;
	default:
		return -EINVAL;
	}
	return -EINVAL;
}

/* stop streaming
 * called on VIDIOC_STREAMOFF
 */
static int vidioc_streamoff(struct file *file, void *fh,
			    enum v4l2_buf_type type)
{
	struct v4l2_loopback_device *dev;
	struct v4l2_loopback_opener *opener;

	MARK();
	dprintk("%d\n", type);

	dev = v4l2loopback_getdevice(file);
	opener = fh_to_opener(fh);
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		if (dev->ready_for_capture > 0)
			dev->ready_for_capture--;
		return 0;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		if (opener->type == READER) {
			opener->type = 0;
			dev->active_readers--;
			client_usage_queue_event(dev->vdev);
		}
		return 0;
	default:
		return -EINVAL;
	}
	return -EINVAL;
}

#ifdef CONFIG_VIDEO_V4L1_COMPAT
static int vidiocgmbuf(struct file *file, void *fh, struct video_mbuf *p)
{
	struct v4l2_loopback_device *dev;
	MARK();

	dev = v4l2loopback_getdevice(file);
	p->frames = dev->buffers_number;
	p->offsets[0] = 0;
	p->offsets[1] = 0;
	p->size = dev->buffer_size;
	return 0;
}
#endif

static void client_usage_queue_event(struct video_device *vdev)
{
	struct v4l2_event ev;
	struct v4l2_loopback_device *dev;

	dev = container_of(vdev->v4l2_dev, struct v4l2_loopback_device,
			   v4l2_dev);

	memset(&ev, 0, sizeof(ev));
	ev.type = V4L2_EVENT_PRI_CLIENT_USAGE;
	((struct v4l2_event_client_usage *)&ev.u)->count = dev->active_readers;

	v4l2_event_queue(vdev, &ev);
}

static int client_usage_ops_add(struct v4l2_subscribed_event *sev,
				unsigned elems)
{
	if (!(sev->flags & V4L2_EVENT_SUB_FL_SEND_INITIAL))
		return 0;

	client_usage_queue_event(sev->fh->vdev);
	return 0;
}

static void client_usage_ops_replace(struct v4l2_event *old,
				     const struct v4l2_event *new)
{
	*((struct v4l2_event_client_usage *)&old->u) =
		*((struct v4l2_event_client_usage *)&new->u);
}

static void client_usage_ops_merge(const struct v4l2_event *old,
				   struct v4l2_event *new)
{
	*((struct v4l2_event_client_usage *)&new->u) =
		*((struct v4l2_event_client_usage *)&old->u);
}

const struct v4l2_subscribed_event_ops client_usage_ops = {
	.add = client_usage_ops_add,
	.replace = client_usage_ops_replace,
	.merge = client_usage_ops_merge,
};

static int vidioc_subscribe_event(struct v4l2_fh *fh,
				  const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subscribe_event(fh, sub);
	case V4L2_EVENT_PRI_CLIENT_USAGE:
		return v4l2_event_subscribe(fh, sub, 0, &client_usage_ops);
	}

	return -EINVAL;
}

/**
 * @brief 打开虚拟内存区域
 *
 * 此函数用于打开给定的虚拟内存区域，并对相关的缓冲区进行操作。
 *
 * @param vma 虚拟内存区域结构体指针
 */
static void vm_open(struct vm_area_struct *vma)
{
	struct v4l2l_buffer *buf;
	MARK();

	buf = vma->vm_private_data;
	buf->use_count++;

	buf->buffer.flags |= V4L2_BUF_FLAG_MAPPED;
}

static void vm_close(struct vm_area_struct *vma)
{
	struct v4l2l_buffer *buf;
	MARK();

	buf = vma->vm_private_data;
	buf->use_count--;

	if (buf->use_count <= 0)
		buf->buffer.flags &= ~V4L2_BUF_FLAG_MAPPED;
}

static struct vm_operations_struct vm_ops = {
	.open = vm_open,
	.close = vm_close,
};

/**
 * @brief 	应用程序的内存映射请求 （由用户空间程序执行'mmap'系统调用开始的）
 * 			包括检查映射的大小和偏移，分配和查找缓冲区，以及将设备内存映射到用户空间。
 * @param file 		设备文件的文件结构体指针
 * @param vma 		虚拟内存区域结构体指针
 * @return int 		返回值
 */
static int v4l2_loopback_mmap(struct file *file, struct vm_area_struct *vma)
{
	u8 *addr;
	unsigned long start;
	unsigned long size;
	struct v4l2_loopback_device *dev;
	struct v4l2_loopback_opener *opener;
	struct v4l2l_buffer *buffer = NULL;
	MARK();

	// 获取映射的起始地址和大小
	start = (unsigned long)vma->vm_start;
	size = (unsigned long)(vma->vm_end - vma->vm_start);

	// 获取设备和打开者的信息
	dev = v4l2loopback_getdevice(file);
	opener = fh_to_opener(file->private_data);

	// 检查映射的大小是否超过了缓冲区的大小
	if (size > dev->buffer_size) {
		dprintk("userspace tries to mmap too much, fail\n");
		return -EINVAL;
	}

	if (opener->timeout_image_io) {
		/* we are going to map the timeout_image_buffer */
		// 检查映射的偏移是否超过了缓冲区的范围
		if ((vma->vm_pgoff << PAGE_SHIFT) !=
		    dev->buffer_size * MAX_BUFFERS) {
			dprintk("invalid mmap offset for timeout_image_io mode\n");
			return -EINVAL;
		}
	} else if ((vma->vm_pgoff << PAGE_SHIFT) >
		   dev->buffer_size * (dev->buffers_number - 1)) {
		dprintk("userspace tries to mmap too far, fail\n");
		return -EINVAL;
	}

	/* FIXXXXXME: allocation should not happen here! */
	// 如果设备的图像缓冲区还没有分配，那么在这里分配
	if (NULL == dev->image)
		if (allocate_buffers(dev) < 0)
			return -EINVAL;

	if (opener->timeout_image_io) {
		buffer = &dev->timeout_image_buffer;
		addr = dev->timeout_image;
	} else {
		// 根据映射的偏移找到对应的缓冲区和地址
		int i;
		for (i = 0; i < dev->buffers_number; ++i) {
			buffer = &dev->buffers[i];
			if ((buffer->buffer.m.offset >> PAGE_SHIFT) ==
			    vma->vm_pgoff)
				break;
		}

		if (i >= dev->buffers_number)
			return -EINVAL;

		addr = dev->image + (vma->vm_pgoff << PAGE_SHIFT);
	}

	// 将设备内存映射到用户空间
	while (size > 0) {
		struct page *page;

		page = vmalloc_to_page(addr);

		if (vm_insert_page(vma, start, page) < 0)
			return -EAGAIN;

		start += PAGE_SIZE;
		addr += PAGE_SIZE;
		size -= PAGE_SIZE;
	}

	// 设置vma的操作函数和私有数据
	vma->vm_ops = &vm_ops;
	vma->vm_private_data = buffer;

	// 打开虚拟内存区域
	vm_open(vma);

	MARK();
	return 0;
}

static unsigned int v4l2_loopback_poll(struct file *file,
				       struct poll_table_struct *pts)
{
	struct v4l2_loopback_opener *opener;
	struct v4l2_loopback_device *dev;
	__poll_t req_events = poll_requested_events(pts);
	int ret_mask = 0;
	MARK();

	opener = fh_to_opener(file->private_data);
	dev = v4l2loopback_getdevice(file);

	if (req_events & POLLPRI) {
		if (!v4l2_event_pending(&opener->fh))
			poll_wait(file, &opener->fh.wait, pts);
		if (v4l2_event_pending(&opener->fh)) {
			ret_mask |= POLLPRI;
			if (!(req_events & DEFAULT_POLLMASK))
				return ret_mask;
		}
	}

	switch (opener->type) {
	case WRITER:
		ret_mask |= POLLOUT | POLLWRNORM;
		break;
	case READER:
		if (!can_read(dev, opener)) {
			if (ret_mask)
				return ret_mask;
			poll_wait(file, &dev->read_event, pts);
		}
		if (can_read(dev, opener))
			ret_mask |= POLLIN | POLLRDNORM;
		if (v4l2_event_pending(&opener->fh))
			ret_mask |= POLLPRI;
		break;
	default:
		break;
	}

	MARK();
	return ret_mask;
}

/* do not want to limit device opens, it can be as many readers as user want,
 * writers are limited by means of setting writer field */
/**
 *  打开设备的函数
 * 这个函数会在用户程序尝试打开设备时被 V4L2 框架调用。
 * 例如，当用户程序调用 open("/dev/videoN", O_RDWR) 时，
 * 		如果 /dev/videoN 是一个 V4L2 loopback 设备，
 * 		那么 V4L2 框架就会调用这个函数。
 * @param file	文件指针
*/
static int v4l2_loopback_open(struct file *file)
{
	// 1. 获取到要打开的设备 v4l2_loopback_device dev
	struct v4l2_loopback_device *dev;
	struct v4l2_loopback_opener *opener;
	MARK();
	dev = v4l2loopback_getdevice(file);

	// 2. 检查设备的 open_count 是否已经达到了 max_openers 的限制。如果达到了，那么函数返回 -EBUSY 错误代码，表示设备忙。
	if (dev->open_count.counter >= dev->max_openers) {
        // 打印出  dev->open_count.counter 和 dev->max_openers
        dprintk("v4l2loopback v4l2_loopback_open dev->open_count.counter:%d dev->max_openers:%d \n", dev->open_count.counter, dev->max_openers);
        return -EBUSY;
    }
	/* kfree on close */
	// 3. 为设备分配一个 v4l2_loopback_opener 结构体，包含了打开者的信息，比如超时时间等。
	opener = kzalloc(sizeof(*opener), GFP_KERNEL);
	if (opener == NULL) {
        dprintk("v4l2loopback v4l2_loopback_open kzalloc failed \n");
        return -ENOMEM;
    }

	// 4. 设备打开计数器加一
	atomic_inc(&dev->open_count);
    // 打印 dev->open_count.counter
    dprintk("v4l2loopback v4l2_loopback_open atomic_inc dev->open_count.counter:%d \n", dev->open_count.counter);

	// 5. 如果设备启用了 timeout_image_io，那么函数会尝试分配一个超时图像
	// hjl 这个要重点关注，可能是默认图的位置
	opener->timeout_image_io = dev->timeout_image_io;
	if (opener->timeout_image_io) {
		// 尝试分配一个超时图像
		int r = allocate_timeout_image(dev);

		// 如果分配失败，减少设备的 open_count，释放刚刚分配的 v4l2_loopback_opener 结构体
		if (r < 0) {
			dprintk("timeout image allocation failed\n");

			atomic_dec(&dev->open_count);
            dprintk("v4l2loopback v4l2_loopback_open atomic_dec 000 dev->open_count.counter:%d \n", dev->open_count.counter);


            kfree(opener);
			return r;
		}
	}
	// v4l2_fh 结构体是用于表示一个打开的设备实例的。
	// 每当一个用户程序打开一个设备，就会创建一个新的 v4l2_fh 实例。
	// 6. v4l2_fh_init 用来初始化一个 v4l2_fh 实例，
	v4l2_fh_init(&opener->fh, video_devdata(file));
	file->private_data = &opener->fh;

	// 7. v4l2_fh_add 用来将这个 v4l2_fh 实例添加到设备的打开者列表中，这样设备就可以知道有哪些程序打开了它。
	v4l2_fh_add(&opener->fh);
	dprintk("opened dev:%p with image:%p\n", dev, dev ? dev->image : NULL);
	MARK();
	return 0;
}

/**
 * @brief 	关闭设备的函数
 * 			这个函数会在用户程序尝试关闭设备时被 V4L2 框架调用。
 * @param file
 * @return int
 */
static int v4l2_loopback_close(struct file *file)
{
	// 1. 获取到要关闭的设备 v4l2_loopback_device dev 和 v4l2_loopback_opener opener
	struct v4l2_loopback_opener *opener;
	struct v4l2_loopback_device *dev;
	int is_writer = 0, is_reader = 0;
	MARK();

	opener = fh_to_opener(file->private_data);
	dev = v4l2loopback_getdevice(file);

	// 2. 标记打开者是读者还是写者
	if (WRITER == opener->type)
		is_writer = 1;
	if (READER == opener->type)
		is_reader = 1;
	// 3. 从设备的打开者列表中删除这个打开者
	atomic_dec(&dev->open_count);
    dprintk("v4l2loopback v4l2_loopback_open atomic_dec dev->open_count.counter:%d \n", dev->open_count.counter);


    // 4. 如果设备的 open_count 计数器为 0，那么说明没有程序打开这个设备了，那么就可以释放设备的内存了（删除两个定时器）
	if (dev->open_count.counter == 0) {
		// dev->sustain_timer是一个持续定时器，它用于保持设备处于打开状态的时间。当设备的打开计数器（dev->open_count.counter）为0时，表示设备没有被打开，这时就需要停止持续定时器的计时。
		del_timer_sync(&dev->sustain_timer);
		//dev->timeout_timer是一个超时定时器，它用于在设备打开一段时间后自动关闭设备。当设备的打开计数器为0时，表示设备没有被打开，这时就需要停止超时定时器的计时。
		del_timer_sync(&dev->timeout_timer);
	}
	//	5.  尝试释放设备的缓冲区
	try_free_buffers(dev);

	// 6. 从设备的打开者列表中删除 opener->fh，并退出 opener->fh
	v4l2_fh_del(&opener->fh);
	v4l2_fh_exit(&opener->fh);

	// 释放 opener
	kfree(opener);

	// 如果打开者是为了写入数据，则设备准备好输出
	if (is_writer)
		dev->ready_for_output = 1;

	// 如果打开者是为了读取数据，设备的活动读者数量-1, 并且会在设备的客户端使用队列中触发一个事件。
	if (is_reader) {
		dev->active_readers--;
		client_usage_queue_event(dev->vdev);
	}
	MARK();
	return 0;
}

static ssize_t v4l2_loopback_read(struct file *file, char __user *buf,
				  size_t count, loff_t *ppos)
{
	int read_index;
	struct v4l2_loopback_device *dev;
	struct v4l2_buffer *b;
	MARK();

	dev = v4l2loopback_getdevice(file);

	read_index = get_capture_buffer(file);
	if (read_index < 0)
		return read_index;
	if (count > dev->buffer_size)
		count = dev->buffer_size;
	b = &dev->buffers[read_index].buffer;
	if (count > b->bytesused)
		count = b->bytesused;
	if (copy_to_user((void *)buf, (void *)(dev->image + b->m.offset),
			 count)) {
		printk(KERN_ERR
		       "v4l2-loopback: failed copy_to_user() in read buf\n");
		return -EFAULT;
	}
	dprintkrw("leave v4l2_loopback_read()\n");
	return count;
}

static ssize_t v4l2_loopback_write(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct v4l2_loopback_opener *opener;
	struct v4l2_loopback_device *dev;
	int write_index;
	struct v4l2_buffer *b;
	int err = 0;

	MARK();

	dev = v4l2loopback_getdevice(file);
	opener = fh_to_opener(file->private_data);

	if (UNNEGOTIATED == opener->type) {
		spin_lock(&dev->lock);

		if (dev->ready_for_output) {
			err = vidioc_streamon(file, file->private_data,
					      V4L2_BUF_TYPE_VIDEO_OUTPUT);
		}

		spin_unlock(&dev->lock);

		if (err < 0)
			return err;
	}

	if (WRITER != opener->type)
		return -EINVAL;

	if (!dev->ready_for_capture) {
		int ret = allocate_buffers(dev);
		if (ret < 0)
			return ret;
		dev->ready_for_capture = 1;
	}
	dprintkrw("v4l2_loopback_write() trying to write %zu bytes\n", count);
	if (count > dev->buffer_size)
		count = dev->buffer_size;

	write_index = v4l2l_mod64(dev->write_position, dev->used_buffers);
	b = &dev->buffers[write_index].buffer;

	if (copy_from_user((void *)(dev->image + b->m.offset), (void *)buf,
			   count)) {
		printk(KERN_ERR
		       "v4l2-loopback: failed copy_from_user() in write buf, could not write %zu\n",
		       count);
		return -EFAULT;
	}
	v4l2l_get_timestamp(b);
	b->bytesused = count;
	b->sequence = dev->write_position;
	buffer_written(dev, &dev->buffers[write_index]);
	wake_up_all(&dev->read_event);
	dprintkrw("leave v4l2_loopback_write() count:%d\n", count);
	return count;
}

/* init functions */
/* frees buffers, if already allocated */
static void free_buffers(struct v4l2_loopback_device *dev)
{
	MARK();
	dprintk("freeing image@%p for dev:%p\n", dev ? dev->image : NULL, dev);
	if (!dev)
		return;
	if (dev->image) {
		vfree(dev->image);
		dev->image = NULL;
	}
	if (dev->timeout_image) {
		vfree(dev->timeout_image);
		dev->timeout_image = NULL;
	}
	dev->imagesize = 0;
}
/* frees buffers, if they are no longer needed */
static void try_free_buffers(struct v4l2_loopback_device *dev)
{
	MARK();
	if (0 == dev->open_count.counter && !dev->keep_format) {
		free_buffers(dev);
		dev->ready_for_capture = 0;
		dev->buffer_size = 0;
		dev->write_position = 0;
	}
}
/* allocates buffers, if buffer_size is set */
static int allocate_buffers(struct v4l2_loopback_device *dev)
{
	int err;

	MARK();
	/* vfree on close file operation in case no open handles left */

	if (dev->buffer_size < 1 || dev->buffers_number < 1)
		return -EINVAL;

	if ((__LONG_MAX__ / dev->buffer_size) < dev->buffers_number)
		return -ENOSPC;

	if (dev->image) {
		dprintk("allocating buffers again: %ld %ld\n",
			dev->buffer_size * dev->buffers_number, dev->imagesize);
		/* FIXME: prevent double allocation more intelligently! */
		if (dev->buffer_size * dev->buffers_number == dev->imagesize)
			return 0;

		/* check whether the total number of readers/writers is <=1 */
		if ((dev->ready_for_capture + dev->active_readers) <= 1)
			free_buffers(dev);
		else
			return -EINVAL;
	}

	dev->imagesize = (unsigned long)dev->buffer_size *
			 (unsigned long)dev->buffers_number;

	dprintk("allocating %ld = %ldx%d\n", dev->imagesize, dev->buffer_size,
		dev->buffers_number);
	err = -ENOMEM;

	if (dev->timeout_jiffies > 0) {
		err = allocate_timeout_image(dev);
		if (err < 0)
			goto error;
	}

	dev->image = vmalloc(dev->imagesize);
	if (dev->image == NULL)
		goto error;

	dprintk("vmallocated %ld bytes\n", dev->imagesize);
	MARK();

	init_buffers(dev);
	return 0;

error:
	free_buffers(dev);
	return err;
}

/* init inner buffers, they are capture mode and flags are set as
 * for capture mod buffers */
static void init_buffers(struct v4l2_loopback_device *dev)
{
	int i;
	int buffer_size;
	int bytesused;
	MARK();

	buffer_size = dev->buffer_size;
	bytesused = dev->pix_format.sizeimage;
	for (i = 0; i < dev->buffers_number; ++i) {
		struct v4l2_buffer *b = &dev->buffers[i].buffer;
		b->index = i;
		b->bytesused = bytesused;
		b->length = buffer_size;
		b->field = V4L2_FIELD_NONE;
		b->flags = 0;
		b->m.offset = i * buffer_size;
		b->memory = V4L2_MEMORY_MMAP;
		b->sequence = 0;
		b->timestamp.tv_sec = 0;
		b->timestamp.tv_usec = 0;
		b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		v4l2l_get_timestamp(b);
	}
	dev->timeout_image_buffer = dev->buffers[0];
	dev->timeout_image_buffer.buffer.m.offset = MAX_BUFFERS * buffer_size;
	MARK();
}

static int allocate_timeout_image(struct v4l2_loopback_device *dev)
{
	MARK();
	if (dev->buffer_size <= 0) {
		dev->timeout_image_io = 0;
		return -EINVAL;
	}

	if (dev->timeout_image == NULL) {
		dev->timeout_image = vzalloc(dev->buffer_size);
		if (dev->timeout_image == NULL) {
			dev->timeout_image_io = 0;
			return -ENOMEM;
		}
	}
	return 0;
}

/* fills and register video device */
static void init_vdev(struct video_device *vdev, int nr)
{
	MARK();

#ifdef V4L2LOOPBACK_WITH_STD
	vdev->tvnorms = V4L2_STD_ALL;
#endif /* V4L2LOOPBACK_WITH_STD */

	vdev->vfl_type = VFL_TYPE_VIDEO;
	vdev->fops = &v4l2_loopback_fops;
	vdev->ioctl_ops = &v4l2_loopback_ioctl_ops;
	vdev->release = &video_device_release;
	vdev->minor = -1;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
	vdev->device_caps = V4L2_CAP_DEVICE_CAPS | V4L2_CAP_VIDEO_CAPTURE |
			    V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_READWRITE |
			    V4L2_CAP_STREAMING;
#endif

	if (debug > 1)
		vdev->dev_debug = V4L2_DEV_DEBUG_IOCTL |
				  V4L2_DEV_DEBUG_IOCTL_ARG;

	vdev->vfl_dir = VFL_DIR_M2M;

	MARK();
}

/* init default capture parameters, only fps may be changed in future */
static void init_capture_param(struct v4l2_captureparm *capture_param)
{
	MARK();
	capture_param->capability = 0;
	capture_param->capturemode = 0;
	capture_param->extendedmode = 0;
	capture_param->readbuffers = max_buffers;
	capture_param->timeperframe.numerator = 1;
	capture_param->timeperframe.denominator = 30;
}

static void check_timers(struct v4l2_loopback_device *dev)
{
	if (!dev->ready_for_capture)
		return;

	if (dev->timeout_jiffies > 0 && !timer_pending(&dev->timeout_timer))
		mod_timer(&dev->timeout_timer, jiffies + dev->timeout_jiffies);
	if (dev->sustain_framerate && !timer_pending(&dev->sustain_timer))
		mod_timer(&dev->sustain_timer,
			  jiffies + dev->frame_jiffies * 3 / 2);
}
#ifdef HAVE_TIMER_SETUP
static void sustain_timer_clb(struct timer_list *t)
{
	struct v4l2_loopback_device *dev = from_timer(dev, t, sustain_timer);
#else
static void sustain_timer_clb(unsigned long nr)
{
	struct v4l2_loopback_device *dev =
		idr_find(&v4l2loopback_index_idr, nr);
#endif
	spin_lock(&dev->lock);
	if (dev->sustain_framerate) {
		dev->reread_count++;
		dprintkrw("reread: %lld %d\n", (long long)dev->write_position,
			  dev->reread_count);
		if (dev->reread_count == 1)
			mod_timer(&dev->sustain_timer,
				  jiffies + max(1UL, dev->frame_jiffies / 2));
		else
			mod_timer(&dev->sustain_timer,
				  jiffies + dev->frame_jiffies);
		wake_up_all(&dev->read_event);
	}
	spin_unlock(&dev->lock);
}
#ifdef HAVE_TIMER_SETUP
static void timeout_timer_clb(struct timer_list *t)
{
	struct v4l2_loopback_device *dev = from_timer(dev, t, timeout_timer);
#else
static void timeout_timer_clb(unsigned long nr)
{
	struct v4l2_loopback_device *dev =
		idr_find(&v4l2loopback_index_idr, nr);
#endif
	spin_lock(&dev->lock);
	if (dev->timeout_jiffies > 0) {
		dev->timeout_happened = 1;
		mod_timer(&dev->timeout_timer, jiffies + dev->timeout_jiffies);
		wake_up_all(&dev->read_event);
	}
	spin_unlock(&dev->lock);
}

/* init loopback main structure */
#define DEFAULT_FROM_CONF(confmember, default_condition, default_value)        \
	((conf) ?                                                              \
		 ((conf->confmember default_condition) ? (default_value) :     \
							 (conf->confmember)) : \
		 default_value)


/**
 * 添加一个虚拟视频设备
 * @param conf
 * @param ret_nr
 * @return
 */
static int v4l2_loopback_add(struct v4l2_loopback_config *conf, int *ret_nr)
{
    // 存储新创建设备信息的指针
	struct v4l2_loopback_device *dev;
    // 视频控制处理器
	struct v4l2_ctrl_handler *hdl;
    // 存储设备号？
	struct v4l2loopback_private *vdev_priv = NULL;
    // 初始化为内存不足
	int err = -ENOMEM;
    // 默认宽高 640*480
	u32 _width = V4L2LOOPBACK_SIZE_DEFAULT_WIDTH;
	u32 _height = V4L2LOOPBACK_SIZE_DEFAULT_HEIGHT;

    // 这里通过宏判断，入参 conf 里的 min_width 和 宏 V4L2LOOPBACK_SIZE_MIN_WIDTH，取二者较大者赋值给 _min_width
	u32 _min_width = DEFAULT_FROM_CONF(min_width,
					   < V4L2LOOPBACK_SIZE_MIN_WIDTH,
					   V4L2LOOPBACK_SIZE_MIN_WIDTH);
	u32 _min_height = DEFAULT_FROM_CONF(min_height,
					    < V4L2LOOPBACK_SIZE_MIN_HEIGHT,
					    V4L2LOOPBACK_SIZE_MIN_HEIGHT);
    // 通过宏判断，入参 conf 里的 max_width 和 上面的 _min_width，取二者较大值给 _max_width
	u32 _max_width = DEFAULT_FROM_CONF(max_width, < _min_width, max_width);
	u32 _max_height =
		DEFAULT_FROM_CONF(max_height, < _min_height, max_height);
    // 根据配置，宣告所有能力
	bool _announce_all_caps = (conf && conf->announce_all_caps >= 0) ?
					  (conf->announce_all_caps) :
					  V4L2LOOPBACK_DEFAULT_EXCLUSIVECAPS;
    // 设置缓冲区数量最大值，默认2 , DEFAULT_FROM_CONF 这个宏根据第二个参数值，true：采用第三个参数，false: 采用第一个参数
	int _max_buffers = DEFAULT_FROM_CONF(max_buffers, <= 0, max_buffers);
    // 设置最大打开这数量，默认10
	int _max_openers = DEFAULT_FROM_CONF(max_openers, <= 0, max_openers);

	int nr = -1;

	_announce_all_caps = (!!_announce_all_caps);

    // 这里 output_nr 和 capture_nr 都取值自 conf->output_nr，加载模块时跟参 video_nr 后如果有值，则conf->output_nr就是这个值，否则是-1
	if (conf) {
		const int output_nr = conf->output_nr;
#ifdef SPLIT_DEVICES
		const int capture_nr = conf->capture_nr;
#else
		const int capture_nr = output_nr;
#endif
		if (capture_nr >= 0 && output_nr == capture_nr) {
			nr = output_nr;
		} else if (capture_nr < 0 && output_nr < 0) {
			nr = -1;
		} else if (capture_nr < 0) {
			nr = output_nr;
		} else if (output_nr < 0) {
			nr = capture_nr;
		} else {
			printk(KERN_ERR
			       "split OUTPUT and CAPTURE devices not yet supported.");
			printk(KERN_INFO
			       "both devices must have the same number (%d != %d).",
			       output_nr, capture_nr);
			return -EINVAL;
		}
	}

    // 检查是否已有设备使用指定'nr'(设备号)，如果是，则返回 EEXIST 错误，表示设备已存在。
	if (idr_find(&v4l2loopback_index_idr, nr)) {
        dprintk("creating v4l2loopback-device fail  haved nr #%d\n", nr);
        return -EEXIST;
    }

	dprintk("creating v4l2loopback-device #%d\n", nr);
    // 为 dev 分配内存
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dprintk("creating v4l2loopback-device idr_alloc #%d\n", nr);

	/* allocate id, if @id >= 0, we're requesting that specific id */
    // 根据 nr 的值，尝试为设备分配一个唯一的标识符（ID）。如果 nr >= 0，则尝试分配特定的ID；否则，自动分配一个ID。
	if (nr >= 0) {
		err = idr_alloc(&v4l2loopback_index_idr, dev, nr, nr + 1,
				GFP_KERNEL);
		if (err == -ENOSPC)
			err = -EEXIST;
	} else {
		err = idr_alloc(&v4l2loopback_index_idr, dev, 0, 0, GFP_KERNEL);
	}
	if (err < 0)
		goto out_free_dev;
    // 将分配好的ID赋值给 nr
    dprintk("allocation v4l2loopback-device id %d\n", nr);
    nr = err;
	err = -ENOMEM;

	if (conf && conf->card_label[0]) {
		snprintf(dev->card_label, sizeof(dev->card_label), "%s",
			 conf->card_label);
	} else {
		snprintf(dev->card_label, sizeof(dev->card_label),
			 "Dummy video device (0x%04X)", nr);
	}
	snprintf(dev->v4l2_dev.name, sizeof(dev->v4l2_dev.name),
		 "v4l2loopback-%03d", nr);

    // 注册 v4l2_loopback 设备到 V4L2 框架中。NULL 表示没有父设备。
	err = v4l2_device_register(NULL, &dev->v4l2_dev);
	if (err)
		goto out_free_idr;
	MARK();

    // 为视频设备分配一个 video_device 结构体。
	dev->vdev = video_device_alloc();
	if (dev->vdev == NULL) {
		err = -ENOMEM;
		goto out_unregister;
	}
    // 为视频设备分配私有数据结构体。
	vdev_priv = kzalloc(sizeof(struct v4l2loopback_private), GFP_KERNEL);
	if (vdev_priv == NULL) {
		err = -ENOMEM;
		goto out_unregister;
	}
    // 设置视频设备的驱动数据为私有数据结构体。
	video_set_drvdata(dev->vdev, vdev_priv);
	if (video_get_drvdata(dev->vdev) == NULL) {
		err = -ENOMEM;
		goto out_unregister;
	}

	MARK();

    // 设置视频设备的名称
	snprintf(dev->vdev->name, sizeof(dev->vdev->name), "%s",
		 dev->card_label);
    // 在私有数据结构体中设置设备编号。
	vdev_priv->device_nr = nr;
    // 初始化视频设备
	init_vdev(dev->vdev, nr);
	dev->vdev->v4l2_dev = &dev->v4l2_dev;
    // 捕获参数
	init_capture_param(&dev->capture_param);
    // 设置帧率
	err = set_timeperframe(dev, &dev->capture_param.timeperframe);
	if (err)
		goto out_unregister;

    // 不保存之前的流的格式？
	dev->keep_format = 0;
    // 不保持恒定帧率
	dev->sustain_framerate = 0;
    // 设置设备是否宣布其支持的所有功能。这取决于配置变量 _announce_all_caps。
	dev->announce_all_caps = _announce_all_caps;
	dev->min_width = _min_width;
	dev->min_height = _min_height;
	dev->max_width = _max_width;
	dev->max_height = _max_height;
    // 设置可以同时打开设备的最大用户数量
	dev->max_openers = _max_openers;
    // 初始化设备的缓冲区数量
	dev->buffers_number = dev->used_buffers = _max_buffers;
    // 初始化设备的写入位置
	dev->write_position = 0;

	MARK();
    // 始化两个自旋锁 dev->lock 和 dev->list_lock。这些锁用于在多线程环境中保护对共享资源的访问
	spin_lock_init(&dev->lock);
	spin_lock_init(&dev->list_lock);
    // 始化输出缓冲区列表 dev->outbufs_list 并为每个已使用的缓冲区添加一个列表项。
	INIT_LIST_HEAD(&dev->outbufs_list);
	if (list_empty(&dev->outbufs_list)) {
		int i;

		for (i = 0; i < dev->used_buffers; ++i)
			list_add_tail(&dev->buffers[i].list_head,
				      &dev->outbufs_list);
	}
    // 清零数组 dev->bufpos2index，用于映射缓冲区位置到其索引
	memset(dev->bufpos2index, 0, sizeof(dev->bufpos2index));
    // 设置 dev->open_count 为 0，这是一个原子变量，用于追踪打开设备的次数
	atomic_set(&dev->open_count, 0);
    // 设置 dev->ready_for_capture 为 0 和 dev->ready_for_output 为 1，用于标记设备的状态。
	dev->ready_for_capture = 1;
	dev->ready_for_output = 1;

	dev->buffer_size = 0;
	dev->image = NULL;
	dev->imagesize = 0;
    // 初始化相关的定时器，如 sustain_timer 和 timeout_timer，用于处理持续帧率和超时逻辑。
#ifdef HAVE_TIMER_SETUP
	timer_setup(&dev->sustain_timer, sustain_timer_clb, 0);
	timer_setup(&dev->timeout_timer, timeout_timer_clb, 0);
#else
	setup_timer(&dev->sustain_timer, sustain_timer_clb, nr);
	setup_timer(&dev->timeout_timer, timeout_timer_clb, nr);
#endif
	dev->reread_count = 0;
	dev->timeout_jiffies = 0;
	dev->timeout_image = NULL;
	dev->timeout_happened = 0;

    // 初始化 dev->ctrl_handler 控制处理程序并注册自定义控制
	hdl = &dev->ctrl_handler;
	err = v4l2_ctrl_handler_init(hdl, 4);
	if (err)
		goto out_unregister;
	v4l2_ctrl_new_custom(hdl, &v4l2loopback_ctrl_keepformat, NULL);
	v4l2_ctrl_new_custom(hdl, &v4l2loopback_ctrl_sustainframerate, NULL);
	v4l2_ctrl_new_custom(hdl, &v4l2loopback_ctrl_timeout, NULL);
	v4l2_ctrl_new_custom(hdl, &v4l2loopback_ctrl_timeoutimageio, NULL);
	if (hdl->error) {
		err = hdl->error;
		goto out_free_handler;
	}
	dev->v4l2_dev.ctrl_handler = hdl;

	err = v4l2_ctrl_handler_setup(hdl);
	if (err)
		goto out_free_handler;

	/* FIXME set buffers to 0 */

	/* Set initial format */
	if (_width < _min_width)
		_width = _min_width;
	if (_width > _max_width)
		_width = _max_width;
	if (_height < _min_height)
		_height = _min_height;
	if (_height > _max_height)
		_height = _max_height;

	dev->pix_format.width = _width;
	dev->pix_format.height = _height;
	dev->pix_format.pixelformat = formats[0].fourcc;
	dev->pix_format.colorspace =
		V4L2_COLORSPACE_DEFAULT; /* do we need to set this ? */
	dev->pix_format.field = V4L2_FIELD_NONE;

	// 打印出当前 v4l2_loopback_device 的设备号
	dprintk("v4l2loopback video_register_device nr #%d\n", nr);

    // 手动设置buffer格式
	dev->pix_format.pixelformat = v4l2_fourcc('Y', 'U', '1', '2');
	dev->pix_format.width = 1920;
	dev->pix_format.height = 1080;
	dev->pix_format.sizeimage = dev->pix_format.width * dev->pix_format.height * 3 / 2;

	// 比对 printk("v4l2loopback video_register_device nr #%d\n", nr); 的写法，打印出 dev->pix_format.width dev->pix_format.height dev->pix_format.pixelformat 这三个变量的值
	dprintk("v4l2loopback width = %d\n", dev->pix_format.width);
	dprintk("v4l2loopback height = %d\n", dev->pix_format.height);
	dprintk("v4l2loopback pixelformat = %c%c%c%c\n",
		(dev->pix_format.pixelformat >> 0) & 0xFF,
		(dev->pix_format.pixelformat >> 8) & 0xFF,
		(dev->pix_format.pixelformat >> 16) & 0xFF,
		(dev->pix_format.pixelformat >> 24) & 0xFF);
    dprintk("v4l2loopback sizeimage = %d\n", dev->pix_format.sizeimage);


	dev->buffer_size = PAGE_ALIGN(dev->pix_format.sizeimage);
	dprintk("buffer_size = %ld (=%d)\n", dev->buffer_size,
		dev->pix_format.sizeimage);

	if (dev->buffer_size && ((err = allocate_buffers(dev)) < 0))
		goto out_free_handler;

	init_waitqueue_head(&dev->read_event);

	/* register the device -> it creates /dev/video* */
	if (video_register_device(dev->vdev, VFL_TYPE_VIDEO, nr) < 0) {
		printk(KERN_ERR
		       "v4l2loopback: failed video_register_device()\n");
		err = -EFAULT;
		goto out_free_device;
	}
	v4l2loopback_create_sysfs(dev->vdev);

	MARK();
	if (ret_nr)
		*ret_nr = dev->vdev->num;
	return 0;

out_free_device:
	video_device_release(dev->vdev);
out_free_handler:
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
out_unregister:
	video_set_drvdata(dev->vdev, NULL);
	if (vdev_priv != NULL)
		kfree(vdev_priv);
	v4l2_device_unregister(&dev->v4l2_dev);
out_free_idr:
	idr_remove(&v4l2loopback_index_idr, nr);
out_free_dev:
	kfree(dev);
	return err;
}

/**
 * @brief 	删除一个虚拟视频设备
 *
 * @param dev
 */
static void v4l2_loopback_remove(struct v4l2_loopback_device *dev)
{
	free_buffers(dev);
	v4l2loopback_remove_sysfs(dev->vdev);
	kfree(video_get_drvdata(dev->vdev));
	video_unregister_device(dev->vdev);
	v4l2_device_unregister(&dev->v4l2_dev);
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
	kfree(dev);
}

static long v4l2loopback_control_ioctl(struct file *file, unsigned int cmd,
				       unsigned long parm)
{
	struct v4l2_loopback_device *dev;
	struct v4l2_loopback_config conf;
	struct v4l2_loopback_config *confptr = &conf;
	int device_nr, capture_nr, output_nr;
	int ret;

	ret = mutex_lock_killable(&v4l2loopback_ctl_mutex);
	if (ret)
		return ret;

	ret = -EINVAL;
	switch (cmd) {
	default:
		ret = -ENOSYS;
		break;
		/* add a v4l2loopback device (pair), based on the user-provided specs */
	case V4L2LOOPBACK_CTL_ADD:
		if (parm) {
			if ((ret = copy_from_user(&conf, (void *)parm,
						  sizeof(conf))) < 0)
				break;
		} else
			confptr = NULL;
		ret = v4l2_loopback_add(confptr, &device_nr);
		if (ret >= 0)
			ret = device_nr;
		break;
		/* remove a v4l2loopback device (both capture and output) */
	case V4L2LOOPBACK_CTL_REMOVE:
		ret = v4l2loopback_lookup((int)parm, &dev);
		if (ret >= 0 && dev) {
			int nr = ret;
			ret = -EBUSY;
			if (dev->open_count.counter > 0)
				break;
			idr_remove(&v4l2loopback_index_idr, nr);
			v4l2_loopback_remove(dev);
			ret = 0;
		};
		break;
		/* get information for a loopback device.
                 * this is mostly about limits (which cannot be queried directly with  VIDIOC_G_FMT and friends
                 */
	case V4L2LOOPBACK_CTL_QUERY:
		if (!parm)
			break;
		if ((ret = copy_from_user(&conf, (void *)parm, sizeof(conf))) <
		    0)
			break;
		capture_nr = output_nr = conf.output_nr;
#ifdef SPLIT_DEVICES
		capture_nr = conf.capture_nr;
#endif
		device_nr = (output_nr < 0) ? capture_nr : output_nr;
		MARK();
		/* get the device from either capture_nr or output_nr (whatever is valid) */
		if ((ret = v4l2loopback_lookup(device_nr, &dev)) < 0)
			break;
		MARK();
		/* if we got the device from output_nr and there is a valid capture_nr,
                 * make sure that both refer to the same device (or bail out)
                 */
		if ((device_nr != capture_nr) && (capture_nr >= 0) &&
		    ((ret = v4l2loopback_lookup(capture_nr, 0)) < 0))
			break;
		MARK();
		/* if otoh, we got the device from capture_nr and there is a valid output_nr,
                 * make sure that both refer to the same device (or bail out)
                 */
		if ((device_nr != output_nr) && (output_nr >= 0) &&
		    ((ret = v4l2loopback_lookup(output_nr, 0)) < 0))
			break;
		MARK();

		/* v4l2_loopback_config identified a single device, so fetch the data */
		snprintf(conf.card_label, sizeof(conf.card_label), "%s",
			 dev->card_label);
		MARK();
		conf.output_nr = dev->vdev->num;
#ifdef SPLIT_DEVICES
		conf.capture_nr = dev->vdev->num;
#endif
		conf.min_width = dev->min_width;
		conf.min_height = dev->min_height;
		conf.max_width = dev->max_width;
		conf.max_height = dev->max_height;
		conf.announce_all_caps = dev->announce_all_caps;
		conf.max_buffers = dev->buffers_number;
		conf.max_openers = dev->max_openers;
		conf.debug = debug;
		MARK();
		if (copy_to_user((void *)parm, &conf, sizeof(conf))) {
			ret = -EFAULT;
			break;
		}
		MARK();
		ret = 0;
		;
		break;
	}

	MARK();
	mutex_unlock(&v4l2loopback_ctl_mutex);
	MARK();
	return ret;
}

/* LINUX KERNEL */

static const struct file_operations v4l2loopback_ctl_fops = {
	// clang-format off
	.owner		= THIS_MODULE,
	.open		= nonseekable_open,
	.unlocked_ioctl	= v4l2loopback_control_ioctl,
	.compat_ioctl	= v4l2loopback_control_ioctl,
	.llseek		= noop_llseek,
	// clang-format on
};

static struct miscdevice v4l2loopback_misc = {
	// clang-format off
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "v4l2loopback",
	.fops		= &v4l2loopback_ctl_fops,
	// clang-format on
};

// 处理视频设备的各种文件操作, 这些操作在设备文件('/dev/video*')上执行
static const struct v4l2_file_operations v4l2_loopback_fops = {
	// clang-format off
	.owner		= THIS_MODULE,
	.open		= v4l2_loopback_open,
	.release	= v4l2_loopback_close,
	.read		= v4l2_loopback_read,
	.write		= v4l2_loopback_write,
	.poll		= v4l2_loopback_poll,
	.mmap		= v4l2_loopback_mmap,
	.unlocked_ioctl	= video_ioctl2,
	// clang-format on
};

static const struct v4l2_ioctl_ops v4l2_loopback_ioctl_ops = {
	// clang-format off
	.vidioc_querycap		= &vidioc_querycap,
	.vidioc_enum_framesizes		= &vidioc_enum_framesizes,
	.vidioc_enum_frameintervals	= &vidioc_enum_frameintervals,

	.vidioc_enum_output		= &vidioc_enum_output,
	.vidioc_g_output		= &vidioc_g_output,
	.vidioc_s_output		= &vidioc_s_output,

	.vidioc_enum_input		= &vidioc_enum_input,
	.vidioc_g_input			= &vidioc_g_input,
	.vidioc_s_input			= &vidioc_s_input,

	.vidioc_enum_fmt_vid_cap	= &vidioc_enum_fmt_cap,
	.vidioc_g_fmt_vid_cap		= &vidioc_g_fmt_cap,
	.vidioc_s_fmt_vid_cap		= &vidioc_s_fmt_cap,
	.vidioc_try_fmt_vid_cap		= &vidioc_try_fmt_cap,

	.vidioc_enum_fmt_vid_out	= &vidioc_enum_fmt_out,
	.vidioc_s_fmt_vid_out		= &vidioc_s_fmt_out,
	.vidioc_g_fmt_vid_out		= &vidioc_g_fmt_out,
	.vidioc_try_fmt_vid_out		= &vidioc_try_fmt_out,

#ifdef V4L2L_OVERLAY
	.vidioc_s_fmt_vid_overlay	= &vidioc_s_fmt_overlay,
	.vidioc_g_fmt_vid_overlay	= &vidioc_g_fmt_overlay,
#endif

#ifdef V4L2LOOPBACK_WITH_STD
	.vidioc_s_std			= &vidioc_s_std,
	.vidioc_g_std			= &vidioc_g_std,
	.vidioc_querystd		= &vidioc_querystd,
#endif /* V4L2LOOPBACK_WITH_STD */

	.vidioc_g_parm			= &vidioc_g_parm,
	.vidioc_s_parm			= &vidioc_s_parm,

	.vidioc_reqbufs			= &vidioc_reqbufs,
	.vidioc_querybuf		= &vidioc_querybuf,
	.vidioc_qbuf			= &vidioc_qbuf,
	.vidioc_dqbuf			= &vidioc_dqbuf,

	.vidioc_streamon		= &vidioc_streamon,
	.vidioc_streamoff		= &vidioc_streamoff,

#ifdef CONFIG_VIDEO_V4L1_COMPAT
	.vidiocgmbuf			= &vidiocgmbuf,
#endif

	.vidioc_subscribe_event		= &vidioc_subscribe_event,
	.vidioc_unsubscribe_event	= &v4l2_event_unsubscribe,
	// clang-format on
};

static int free_device_cb(int id, void *ptr, void *data)
{
	struct v4l2_loopback_device *dev = ptr;
	v4l2_loopback_remove(dev);
	return 0;
}
static void free_devices(void)
{
	idr_for_each(&v4l2loopback_index_idr, &free_device_cb, NULL);
	idr_destroy(&v4l2loopback_index_idr);
}

static int __init v4l2loopback_init_module(void)
{
	const u32 min_width = V4L2LOOPBACK_SIZE_MIN_WIDTH;
	const u32 min_height = V4L2LOOPBACK_SIZE_MIN_HEIGHT;
	int err;
	int i;
	MARK();

	err = misc_register(&v4l2loopback_misc);
	if (err < 0)
		return err;

	if (devices < 0) {
		devices = 1;

		/* try guessing the devices from the "video_nr" parameter */
		for (i = MAX_DEVICES - 1; i >= 0; i--) {
			if (video_nr[i] >= 0) {
				devices = i + 1;
				break;
			}
		}
	}

	if (devices > MAX_DEVICES) {
		devices = MAX_DEVICES;
		printk(KERN_INFO
		       "v4l2loopback: number of initial devices is limited to: %d\n",
		       MAX_DEVICES);
	}

	if (max_buffers > MAX_BUFFERS) {
		max_buffers = MAX_BUFFERS;
		printk(KERN_INFO
		       "v4l2loopback: number of buffers is limited to: %d\n",
		       MAX_BUFFERS);
	}

	if (max_openers < 0) {
		printk(KERN_INFO
		       "v4l2loopback: allowing %d openers rather than %d\n",
		       2, max_openers);
		max_openers = 2;
	}

	if (max_width < min_width) {
		max_width = V4L2LOOPBACK_SIZE_DEFAULT_MAX_WIDTH;
		printk(KERN_INFO "v4l2loopback: using max_width %d\n",
		       max_width);
	}
	if (max_height < min_height) {
		max_height = V4L2LOOPBACK_SIZE_DEFAULT_MAX_HEIGHT;
		printk(KERN_INFO "v4l2loopback: using max_height %d\n",
		       max_height);
	}

	for (i = 0; i < devices; i++) {
		dprintk(KERN_INFO "v4l2loopback: output_nr %d\n", video_nr[i]);
		struct v4l2_loopback_config cfg = {
			// clang-format off
			.output_nr		= video_nr[i],
#ifdef SPLIT_DEVICES
			.capture_nr		= video_nr[i],
#endif
			.min_width		= min_width,
			.min_height		= min_height,
			.max_width		= max_width,
			.max_height		= max_height,
			.announce_all_caps	= (!exclusive_caps[i]),
			.max_buffers		= max_buffers,
			.max_openers		= max_openers,
			.debug			= debug,
			// clang-format on
		};
		cfg.card_label[0] = 0;
		if (card_label[i])
			snprintf(cfg.card_label, sizeof(cfg.card_label), "%s",
				 card_label[i]);
		err = v4l2_loopback_add(&cfg, 0);
		if (err) {
			free_devices();
			goto error;
		}
	}

	dprintk("module installed\n");

	printk(KERN_INFO "v4l2loopback driver version %d.%d.%d%s loaded\n",
	       // clang-format off
	       (V4L2LOOPBACK_VERSION_CODE >> 16) & 0xff,
	       (V4L2LOOPBACK_VERSION_CODE >>  8) & 0xff,
	       (V4L2LOOPBACK_VERSION_CODE      ) & 0xff,
#ifdef SNAPSHOT_VERSION
	       " (" __stringify(SNAPSHOT_VERSION) ")"
#else
	       ""
#endif
	       );
	// clang-format on

	return 0;
error:
	misc_deregister(&v4l2loopback_misc);
	return err;
}

static void v4l2loopback_cleanup_module(void)
{
	MARK();
	/* unregister the device -> it deletes /dev/video* */
	free_devices();
	/* and get rid of /dev/v4l2loopback */
	misc_deregister(&v4l2loopback_misc);
	dprintk("module removed\n");
}

MODULE_ALIAS_MISCDEV(MISC_DYNAMIC_MINOR);

module_init(v4l2loopback_init_module);
module_exit(v4l2loopback_cleanup_module);

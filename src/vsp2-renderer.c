/*
 * Copyright © 2014-2016 Renesas Electronics Corp.
 *
 * Based on pixman-renderer by:
 * Copyright © 2012 Intel Corporation
 * Copyright © 2013 Vasily Khoruzhick <anarsoul@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *	Takanari Hayama <taki@igel.co.jp>
 */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include "v4l2-renderer.h"
#include "v4l2-renderer-device.h"

#if defined(V4L2_GL_FALLBACK_ENABLED) || defined(VSP2_SCALER_ENABLED)
#include <unistd.h>
#include <xf86drm.h>
#include <libkms/libkms.h>
#endif

#include <drm_fourcc.h>

#if 0
#define DBG(...) weston_log(__VA_ARGS__)
#define DBGC(...) weston_log_continue(__VA_ARGS__)
#else
#define DBG(...) do {} while (0)
#define DBGC(...) do {} while (0)
#endif

struct vsp_surface_state {
	struct v4l2_surface_state base;

	struct v4l2_format fmt;
	enum v4l2_mbus_pixelcode mbus_code;
};

struct vsp_renderer_output {
	struct v4l2_renderer_output base;
	struct vsp_surface_state surface_state;
};

#define VSP_INPUT_MAX		5
#define VSP_INPUT_DEFAULT	4
#define VSP_SCALER_MAX	1
#define VSP_SCALER_MIN_PIXELS	4	// UDS can't take pixels smaller than this

struct __vsp2_media_entity {
	const char *name;
	int fd;
	struct media_entity_desc entity;
};

struct vsp2_media_entity {
	struct __vsp2_media_entity devnode;
	struct __vsp2_media_entity subdev;
	struct media_link_desc link;
};

#define MEDIA_ENTITY(idx, dev_name, subdev_name, src_idx, sink_idx)	\
	[(idx)] = {				\
		.devnode = {			\
			.name = (dev_name),	\
			.fd = -1		\
		},				\
		.subdev = {			\
			.name = (subdev_name),	\
			.fd = -1		\
		},				\
		.link = {			\
			.source = {		\
				.index = (src_idx),	\
				.flags = MEDIA_PAD_FL_SOURCE	\
			},			\
			.sink = {		\
				.index = (sink_idx),		\
				.flags = MEDIA_PAD_FL_SINK	\
			}			\
		}				\
	}

enum {
	VSPB_RPF0, VSPB_RPF1, VSPB_RPF2, VSPB_RPF3, VSPB_RPF4,
	VSPB_BRU,
	VSPB_WPF0,
	VSPB_ENTITY_MAX
};

static struct vsp2_media_entity vspb_entities[] = {
	MEDIA_ENTITY(VSPB_RPF0, "rpf.0 input", "rpf.0", 1, 0),	// rpf.0:1 -> bru:0
	MEDIA_ENTITY(VSPB_RPF1, "rpf.1 input", "rpf.1", 1, 1),	// rpf.1:1 -> bru:1
	MEDIA_ENTITY(VSPB_RPF2, "rpf.2 input", "rpf.2", 1, 2),	// rpf.2:1 -> bru:2
	MEDIA_ENTITY(VSPB_RPF3, "rpf.3 input", "rpf.3", 1, 3),	// rpf.3:1 -> bru:3
	MEDIA_ENTITY(VSPB_RPF4, "rpf.4 input", "rpf.4", 1, 4),	// rpf.4:1 -> bru:4

	MEDIA_ENTITY(VSPB_BRU, NULL, "bru", 5, 0),			// bru:5 -> wpf.0:0

	MEDIA_ENTITY(VSPB_WPF0, "wpf.0 output", "wpf.0", -1, -1)	// immutable
};

#ifdef VSP2_SCALER_ENABLED
enum {
	VSPI_RPF0,
	VSPI_UDS0,
	VSPI_WPF0,
	VSPI_ENTITY_MAX
};

static struct vsp2_media_entity vspi_entities[] = {
	MEDIA_ENTITY(VSPI_RPF0, "rpf.0 input", "rpf.0", 1, 0),	// rpf.0:1 -> uds.0:0

	MEDIA_ENTITY(VSPI_UDS0, NULL, "uds.0", 1, 0),			// uds.0:1 -> wpf.0:0

	MEDIA_ENTITY(VSPI_WPF0, "wpf.0 output", "wpf.0", -1, -1)	// immutable
};

struct vsp_scaler_device {
	int media_fd;

	int width;
	int height;
	struct vsp_surface_state state;

	struct vsp2_media_entity *rpf;
	struct vsp2_media_entity *uds;
	struct vsp2_media_entity *wpf;
};
#endif

typedef enum {
	VSP_STATE_IDLE,
	VSP_STATE_START,
	VSP_STATE_COMPOSING,
} vsp_state_t;

struct vsp_input {
	struct vsp2_media_entity *rpf;
	struct vsp_surface_state *input_surface_states;
	struct v4l2_rect src;
	struct v4l2_rect dst;
	int opaque;
};

struct vsp_device {
	struct v4l2_renderer_device base;

	vsp_state_t state;

	struct vsp_surface_state *output_surface_state;

	int input_count;
	int input_max;
	struct vsp_input inputs[VSP_INPUT_MAX];

	struct vsp2_media_entity *bru;
	struct vsp2_media_entity *wpf;
	struct v4l2_format current_wpf_fmt;

#ifdef VSP2_SCALER_ENABLED
	int scaler_enable;
	int scaler_count;
	int scaler_max;
	struct vsp_scaler_device *scaler;
#endif

#ifdef V4L2_GL_FALLBACK_ENABLED
	int max_views_to_compose;
#endif
};

#define ARRAY_SIZE(a)		(sizeof((a))/sizeof((a[0])))

static uint32_t vsp2_support_formats[] = {
	DRM_FORMAT_XRGB8888, /* V4L2_PIX_FMT_XBGR32 */
	DRM_FORMAT_ARGB8888, /* V4L2_PIX_FMT_ABGR32 */
	DRM_FORMAT_BGRX8888, /* V4L2_PIX_FMT_XRGB32 */
	DRM_FORMAT_BGRA8888, /* V4L2_PIX_FMT_ARGB32 */
	DRM_FORMAT_RGB888, /* V4L2_PIX_FMT_RGB24 */
	DRM_FORMAT_BGR888, /* V4L2_PIX_FMT_BGR24 */
	DRM_FORMAT_RGB565, /* V4L2_PIX_FMT_RGB565 */
	DRM_FORMAT_RGB332, /* 4L2_PIX_FMT_RGB332 */
	DRM_FORMAT_YUYV, /* V4L2_PIX_FMT_YUYV */
	DRM_FORMAT_YVYU, /* V4L2_PIX_FMT_YVYU */
	DRM_FORMAT_UYVY, /* V4L2_PIX_FMT_UYVY */
	DRM_FORMAT_VYUY, /* V4L2_PIX_FMT_VYUY */
	DRM_FORMAT_NV12, /* V4L2_PIX_FMT_NV12M */
	DRM_FORMAT_NV16, /* V4L2_PIX_FMT_NV16M */
	DRM_FORMAT_NV21, /* V4L2_PIX_FMT_NV21M */
	DRM_FORMAT_NV61, /* V4L2_PIX_FMT_NV61M */
	DRM_FORMAT_YUV420, /* V4L2_PIX_FMT_YUV420M */
	DRM_FORMAT_YVU420, /* V4L2_PIX_FMT_YVU420M */
	DRM_FORMAT_YUV422, /* V4L2_PIX_FMT_YUV422M */
	DRM_FORMAT_YVU422, /* V4L2_PIX_FMT_YVU422M */
	DRM_FORMAT_YUV444, /* V4L2_PIX_FMT_YUV444M */
	DRM_FORMAT_YVU444, /* V4L2_PIX_FMT_YVU444M */

	/* for backward compatibility */
	DRM_FORMAT_XBGR8888, /* V4L2_PIX_FMT_XRGB32 */
	DRM_FORMAT_ABGR8888, /* V4L2_PIX_FMT_ARGB32 */
};

static int
video_is_capture(__u32 cap)
{
	return ((cap & V4L2_CAP_VIDEO_CAPTURE) || (cap & V4L2_CAP_VIDEO_CAPTURE_MPLANE));
}

static int
video_is_mplane(__u32 cap)
{
	return ((cap & V4L2_CAP_VIDEO_CAPTURE_MPLANE) || (cap & V4L2_CAP_VIDEO_OUTPUT_MPLANE));
}

static int
video_is_streaming(__u32 cap)
{
	return (cap & V4L2_CAP_STREAMING);
}

static void
vsp2_check_capability(int fd, const char *devname)
{
	struct v4l2_capability cap;
	int ret;

	memset(&cap, 0, sizeof cap);
	ret = ioctl(fd, VIDIOC_QUERYCAP, &cap);
	if (ret < 0) {
		weston_log("VIDIOC_QUERY_CAP on %s failed.\n", devname);
		return;
	}

	weston_log("Device `%s'(%d) is a video %s (%s mplane and %s streaming support)\n",
		   devname, fd,
		   (video_is_capture(cap.device_caps) ? "capture" : "output"),
		   (video_is_mplane(cap.device_caps) ? "w/" : "w/o"),
		   (video_is_streaming(cap.device_caps) ? "w/" : "w/o"));
}

#ifdef VSP2_SCALER_ENABLED
static int
vsp2_scan_device_and_reset_links(int fd, struct vsp2_media_entity *entities, int count);

static struct vsp_scaler_device*
vsp2_scaler_init(struct weston_config_section *section)
{
	struct vsp_scaler_device *scaler;
	char *device;

	/* if vspi-device is not set, then we don't enable UDS */
	weston_config_section_get_string(section, "vspi-device", &device, NULL);

	if (!device) {
		DBG("no vspi-device specified.\n");
		return NULL;
	}

	if ((scaler = calloc(1, sizeof *scaler)) == NULL)
		return NULL;

	weston_log("Using %s as a VSPI.\n", device);
	scaler->media_fd = open(device, O_RDWR);
	if (scaler->media_fd < 0) {
		weston_log("Can't open the device %s.\n", device);
		free(device);
		goto error;
	}
	free(device);

	/* scan VSPI device */
	if (vsp2_scan_device_and_reset_links(scaler->media_fd,
					     vspi_entities, VSPI_ENTITY_MAX) < 0) {
		weston_log("Device scan and reset failed.\n");
		goto error;
	}

	scaler->rpf = &vspi_entities[VSPI_RPF0];
	scaler->uds = &vspi_entities[VSPI_UDS0];
	scaler->wpf = &vspi_entities[VSPI_WPF0];

	/* Initialize input */
	weston_log("Setting up scaler input.\n");
	scaler->rpf->link.source.entity = scaler->rpf->subdev.entity.id;
	scaler->rpf->link.sink.entity = scaler->uds->subdev.entity.id;
	scaler->rpf->link.flags = MEDIA_LNK_FL_ENABLED;

	if (ioctl(scaler->media_fd, MEDIA_IOC_SETUP_LINK, &scaler->rpf->link) < 0) {
		weston_log("setting a link between uds and wpf failed.\n");
		goto error;
	}

	vsp2_check_capability(scaler->rpf->devnode.fd, scaler->rpf->devnode.entity.name);

	/* Initialize scaler */
	weston_log("Setting up a scaler.\n");
	scaler->uds->link.source.entity = scaler->uds->subdev.entity.id;
	scaler->uds->link.sink.entity = scaler->wpf->subdev.entity.id;
	scaler->uds->link.flags = MEDIA_LNK_FL_ENABLED;

	if (ioctl(scaler->media_fd, MEDIA_IOC_SETUP_LINK, &scaler->uds->link) < 0) {
		weston_log("setting a link between uds and wpf failed.\n");
		goto error;
	}

	/* output side... nothing to do */
	vsp2_check_capability(scaler->wpf->devnode.fd, scaler->wpf->devnode.entity.name);

	return scaler;

error:
	if (scaler)
		free(scaler);
	weston_log("VSPI device init failed...\n");
	return NULL;
}
#endif

static int
vsp2_scan_device_and_reset_links(int fd, struct vsp2_media_entity *entities, int count) {
	struct media_entity_desc entity = { .id = 0 };
	struct media_links_enum links_enum = { .pads = NULL };
	struct media_link_desc *links = NULL;
	int max_links = 0, n, ret = 0;

	while (1) {
		entity.id |= MEDIA_ENT_ID_FLAG_NEXT;
		if ((ret = ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &entity)) < 0) {
			if (errno == EINVAL)
				ret = 0;
			break;
		}

		// make sure that we have enough space for links
		if (max_links < entity.links) {
			if (!(links = realloc(links, sizeof(struct media_link_desc) * entity.links)))
				break;
			max_links = entity.links;
		}

		links_enum.entity = entity.id;
		links_enum.links = links;

		if ((ret = ioctl(fd, MEDIA_IOC_ENUM_LINKS, &links_enum)) < 0)
			break;

		for (n = 0; n < entity.links; n++) {
			if (links[n].flags & MEDIA_LNK_FL_IMMUTABLE)
				continue;
			links[n].flags &= ~MEDIA_LNK_FL_ENABLED;
			if (ioctl(fd, MEDIA_IOC_SETUP_LINK, &links[n]) < 0)
				weston_log("reset link on entity=%d link=%d failed. ignore error.\n",
					   entity.id, n);
		}

		// check if we need this entity
		struct __vsp2_media_entity *node = NULL;

		for (n = 0; n < count && node == NULL; n++) {
			if ((entities[n].devnode.name != NULL) &&
			    (entities[n].devnode.fd == -1) &&
			    (strstr(entity.name, entities[n].devnode.name)) &&
			    (entity.type == MEDIA_ENT_T_DEVNODE_V4L))
				node = &entities[n].devnode;
			else if ((entities[n].subdev.name != NULL) &&
				 (entities[n].subdev.fd == -1) &&
			         (strstr(entity.name, entities[n].subdev.name)) &&
				 (entity.type == MEDIA_ENT_T_V4L2_SUBDEV))
				node = &entities[n].subdev;
		}

		if (node) {
			char path[32];
			memcpy(&node->entity, &entity, sizeof(struct media_entity_desc));
			snprintf(path, sizeof(path), "/dev/char/%d:%d", entity.v4l.major, entity.v4l.minor);
			node->fd = open(path, O_RDWR);
			weston_log("'%s' found (fd=%d @ '%s').\n", node->name, node->fd, path);

			if (node->fd < 0) {
				ret = -1;
				break;
			}
		}
	}

	/* check if we got all we need */
	for (n = 0; n < count; n++) {
		if ((entities[n].devnode.name) && (entities[n].devnode.fd == -1))
			weston_log("'%s' NOT FOUND!\n", entities[n].devnode.name);

		if ((entities[n].subdev.name) && (entities[n].subdev.fd == -1))
			weston_log("'%s' NOT FOUND!\n", entities[n].subdev.name);
	}

	if (links)
		free(links);

	return ret;
}

static struct v4l2_renderer_device*
vsp2_init(int media_fd, struct media_device_info *info, struct weston_config *config)
{
	struct vsp_device *vsp = NULL;
	char *device_name;
	struct weston_config_section *section;
	int i;

	/* Get device name */
	if ((device_name = strchr(info->bus_info, ':')))
		device_name += 1;
	else
		device_name = info->bus_info;

	/*
	 * XXX: The model name that V4L2 media controller passes should be fixed in the future,
	 * so that we can distinguish the capability of the VSP device. R-Car Gen3 has VSPB,
	 * VSPI, and VSPD as 'VSP2', but they all have different capabilities. Right now,
	 * the model name is always 'VSP2'.
	 */
	if (strncmp(info->model, "VSP", 3)) {
		weston_log("The device is not VSP.");
		goto error;
	}

	weston_log("Using the device %s\n", device_name);

	vsp = calloc(1, sizeof(struct vsp_device));
	if (!vsp)
		goto error;
	vsp->base.media_fd = media_fd;
	vsp->base.device_name = device_name;
	vsp->state = VSP_STATE_IDLE;

	/* check configuration */
	section = weston_config_get_section(config,
					    "vsp-renderer", NULL, NULL);
	weston_config_section_get_int(section, "max_inputs", &vsp->input_max, VSP_INPUT_DEFAULT);
#ifdef V4L2_GL_FALLBACK_ENABLED
	weston_config_section_get_int(section, "max_views_to_compose", &vsp->max_views_to_compose, -1);
#endif

	if (vsp->input_max < 2)
		vsp->input_max = 2;
	if (vsp->input_max > VSP_INPUT_MAX)
		vsp->input_max = VSP_INPUT_MAX;

	if (vsp2_scan_device_and_reset_links(media_fd, vspb_entities, VSPB_ENTITY_MAX) < 0) {
		weston_log("Device scan and reset failed.\n");
		goto error;
	}

	vsp->bru = &vspb_entities[VSPB_BRU];
	vsp->wpf = &vspb_entities[VSPB_WPF0];

	/* Initialize inputs */
	weston_log("Setting up inputs. Use %d inputs.\n", vsp->input_max);
	for (i = 0; i < vsp->input_max; i++) {
		struct vsp2_media_entity *rpf = &vspb_entities[VSPB_RPF0 + i];

		/* create the link desc */
		rpf->link.source.entity = rpf->subdev.entity.id;
		rpf->link.sink.entity = vsp->bru->subdev.entity.id;

		/* set up */
		vsp->inputs[i].rpf = rpf;

		/* check capability */
		vsp2_check_capability(rpf->devnode.fd, rpf->devnode.entity.name);
	}

	/* Initialize composer */
	weston_log("Setting up a composer.\n");

	/* set an input format for BRU to be ARGB (default) */
	for (i = 0; i < vsp->input_max; i++) {
		struct v4l2_subdev_format subdev_format = {
			.pad = i,
			.which = V4L2_SUBDEV_FORMAT_ACTIVE,
			.format = {
				.width = 256,		// a random number
				.height = 256,		// a random number
				.code = V4L2_MBUS_FMT_ARGB8888_1X32
			}
		};

		if (ioctl(vsp->bru->subdev.fd, VIDIOC_SUBDEV_S_FMT, &subdev_format) < 0) {
			weston_log("setting default failed.\n");
			goto error;
		}

		if (subdev_format.format.code != V4L2_MBUS_FMT_ARGB8888_1X32) {
			weston_log("couldn't set to ARGB.\n");
			goto error;
		}
	}

	/* set a link betweeen bru:5 and wpf.0:0 */
	vsp->bru->link.source.entity = vsp->bru->subdev.entity.id;
	vsp->bru->link.sink.entity = vsp->wpf->subdev.entity.id;
	vsp->bru->link.flags = MEDIA_LNK_FL_ENABLED;

	struct media_link_desc *link = &vsp->bru->link;

	if (ioctl(vsp->base.media_fd, MEDIA_IOC_SETUP_LINK, link) < 0) {
		weston_log("setting a link between bru and wpf failed.\n");
		goto error;
	}

	/* Initialize output */
	weston_log("Setting up an output.\n");

	/* output is always enabled - immutable */
	vsp2_check_capability(vsp->wpf->devnode.fd, vsp->wpf->devnode.entity.name);

#ifdef VSP2_SCALER_ENABLED
	vsp->scaler_max = VSP_SCALER_MAX;

	weston_config_section_get_bool(section, "vsp-scaler", &vsp->scaler_enable, 0);

	DBG("vsp-scaler = '%s'\n", vsp->scaler_enable ? "true" : "false");

	if (vsp->scaler_enable) {
		if (!(vsp->scaler = vsp2_scaler_init(section)))
			vsp->scaler_enable = 0;
	}
#endif

	return (struct v4l2_renderer_device*)vsp;

error:
	if (vsp)
		free(vsp);
	weston_log("VSP device init failed...\n");

	return NULL;
}

static struct v4l2_surface_state*
vsp2_create_surface(struct v4l2_renderer_device *dev)
{
	return (struct v4l2_surface_state*)calloc(1, sizeof(struct vsp_surface_state));
}

static int
vsp2_attach_buffer(struct v4l2_surface_state *surface_state)
{
	struct vsp_surface_state *vs = (struct vsp_surface_state*)surface_state;
	enum v4l2_mbus_pixelcode code;
	int i;

	if (vs->base.width > 8190 || vs->base.height > 8190)
		return -1;

	switch(vs->base.pixel_format) {
	case V4L2_PIX_FMT_XRGB32:
	case V4L2_PIX_FMT_ARGB32:
	case V4L2_PIX_FMT_XBGR32:
	case V4L2_PIX_FMT_ABGR32:
	case V4L2_PIX_FMT_RGB24:
	case V4L2_PIX_FMT_BGR24:
	case V4L2_PIX_FMT_RGB565:
	case V4L2_PIX_FMT_RGB332:
		code = V4L2_MBUS_FMT_ARGB8888_1X32;
		break;

	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_YVYU:
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_VYUY:
	case V4L2_PIX_FMT_NV12M:
	case V4L2_PIX_FMT_NV21M:
	case V4L2_PIX_FMT_NV16M:
	case V4L2_PIX_FMT_NV61M:
	case V4L2_PIX_FMT_YUV420M:
	case V4L2_PIX_FMT_YVU420M:
	case V4L2_PIX_FMT_YUV422M:
	case V4L2_PIX_FMT_YVU422M:
	case V4L2_PIX_FMT_YUV444M:
	case V4L2_PIX_FMT_YVU444M:
		code = V4L2_MBUS_FMT_AYUV8_1X32;
		break;

	default:
		return -1;
	}

	// create v4l2_fmt to use later
	vs->mbus_code = code;
	vs->fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	vs->fmt.fmt.pix_mp.width = vs->base.width;
	vs->fmt.fmt.pix_mp.height = vs->base.height;
	vs->fmt.fmt.pix_mp.pixelformat = vs->base.pixel_format;
	vs->fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
	vs->fmt.fmt.pix_mp.num_planes = vs->base.num_planes;

	for (i = 0; i < vs->base.num_planes; i++)
		vs->fmt.fmt.pix_mp.plane_fmt[i].bytesperline = vs->base.planes[i].stride;

	return 0;
}

static int
vsp2_set_format(int fd, struct v4l2_format *fmt, int opaque)
{
	int ret;
	unsigned int original_pixelformat = fmt->fmt.pix_mp.pixelformat;

#if 0
	// For debugging purpose only

	struct v4l2_format current_fmt;

	memset(&current_fmt, 0, sizeof(struct v4l2_format));
	current_fmt.type = fmt->type;

	if (ioctl(fd, VIDIOC_G_FMT, &current_fmt) == -1) {
		weston_log("VIDIOC_G_FMT failed to %d (%s).\n", fd, strerror(errno));
	}

	DBG("Current video format: %d, %08x(%c%c%c%c) %ux%u (stride %u) field %08u buffer size %u\n",
	    current_fmt.type,
	    current_fmt.fmt.pix_mp.pixelformat,
	    (current_fmt.fmt.pix_mp.pixelformat >> 24) & 0xff,
	    (current_fmt.fmt.pix_mp.pixelformat >> 16) & 0xff,
	    (current_fmt.fmt.pix_mp.pixelformat >>  8) & 0xff,
	    current_fmt.fmt.pix_mp.pixelformat & 0xff,
	    current_fmt.fmt.pix_mp.width, current_fmt.fmt.pix_mp.height, current_fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
	    current_fmt.fmt.pix_mp.field,
	    current_fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
#endif

	switch (original_pixelformat) {
	case V4L2_PIX_FMT_ABGR32:
		if (opaque)
			fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_XBGR32;
		else
			/* ABGR32 surfaces are premultiplied. */
			fmt->fmt.pix_mp.flags = V4L2_PIX_FMT_FLAG_PREMUL_ALPHA;
	}

	ret = ioctl(fd, VIDIOC_S_FMT, fmt);

	DBG("New video format: %d, %08x(%c%c%c%c) %ux%u (stride %u) field %08u buffer size %u\n",
	    fmt->type,
	    fmt->fmt.pix_mp.pixelformat,
	    (fmt->fmt.pix_mp.pixelformat >> 24) & 0xff,
	    (fmt->fmt.pix_mp.pixelformat >> 16) & 0xff,
	    (fmt->fmt.pix_mp.pixelformat >>  8) & 0xff,
	    fmt->fmt.pix_mp.pixelformat & 0xff,
	    fmt->fmt.pix_mp.width, fmt->fmt.pix_mp.height, fmt->fmt.pix_mp.plane_fmt[0].bytesperline,
	    fmt->fmt.pix_mp.field,
	    fmt->fmt.pix_mp.plane_fmt[0].sizeimage);

	fmt->fmt.pix_mp.pixelformat = original_pixelformat;

	if (ret == -1) {
		weston_log("VIDIOC_S_FMT failed to %d (%s).\n", fd, strerror(errno));
		return -1;
	}

	return 0;
}

static int
vsp2_set_output(struct vsp_device *vsp, struct vsp_renderer_output *out)
{
	struct v4l2_subdev_format subdev_format = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.format = {
			.width = out->base.width,
			.height = out->base.height,
			.code = V4L2_MBUS_FMT_ARGB8888_1X32	// TODO: does this have to be flexible?
		}
	};

	DBG("Setting output size to %dx%d\n", out->base.width, out->base.height);

	/* set up bru:5 */
	subdev_format.pad = 5;
	if (ioctl(vsp->bru->subdev.fd, VIDIOC_SUBDEV_S_FMT, &subdev_format) < 0)
		return -1;

	/* set up wpf.0:0 and wpf.0:1 */
	subdev_format.pad = 0;
	if (ioctl(vsp->wpf->subdev.fd, VIDIOC_SUBDEV_S_FMT, &subdev_format) < 0)
		return -1;

	subdev_format.pad = 1;
	if (ioctl(vsp->wpf->subdev.fd, VIDIOC_SUBDEV_S_FMT, &subdev_format) < 0)
		return -1;

	return 0;
}

#ifdef VSP2_SCALER_ENABLED
static int
vsp2_scaler_create_buffer(struct vsp_scaler_device *scaler, int fd,
			  struct kms_driver *kms, int width, int height)
{
	unsigned attr[] = {
		KMS_BO_TYPE, KMS_BO_TYPE_SCANOUT_X8R8G8B8,
		KMS_WIDTH, 0,
		KMS_HEIGHT, 0,
		KMS_TERMINATE_PROP_LIST
	};
	unsigned int handle, stride;
	struct vsp_surface_state *vs = &scaler->state;

	if (scaler->width >= width && scaler->height >= height)
		return 0;

	if (scaler->width < width)
		scaler->width = width;
	if (scaler->height < height)
		scaler->height = height;

	if (vs->base.planes[0].dmafd > 0) {
		close(vs->base.planes[0].dmafd);
		vs->base.planes[0].dmafd = 0;
		kms_bo_destroy(&vs->base.bo);
	}

	attr[3] = (scaler->width + 0x1f) & ~0x1f;
	attr[5] = scaler->height;

	if (kms_bo_create(kms, attr, &vs->base.bo))
		goto error;
	if (kms_bo_get_prop(vs->base.bo, KMS_PITCH, &stride))
		goto error;
	vs->base.bo_stride = stride;
	if (kms_bo_get_prop(vs->base.bo, KMS_HANDLE, &handle))
		goto error;
	if (drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC,
			       &vs->base.planes[0].dmafd))
		goto error;

	vs->base.bpp = 4;

	return 0;

error:
	if (vs->base.planes[0].dmafd > 0)
		close(vs->base.planes[0].dmafd);
	kms_bo_destroy(&vs->base.bo);
	return -1;
}
#endif

static struct v4l2_renderer_output*
vsp2_create_output(struct v4l2_renderer_device *dev, int width, int height)
{
	struct vsp_renderer_output *outdev;
	struct v4l2_format *fmt;

	outdev = calloc(1, sizeof(struct vsp_renderer_output));
	if (!outdev)
		return NULL;

	/* set output surface state */
	outdev->base.width = width;
	outdev->base.height = height;
	outdev->surface_state.mbus_code = V4L2_MBUS_FMT_ARGB8888_1X32;
	outdev->surface_state.base.width = width;
	outdev->surface_state.base.height = height;
	outdev->surface_state.base.num_planes = 1;
	outdev->surface_state.base.src_rect.width = width;
	outdev->surface_state.base.src_rect.height = height;
	outdev->surface_state.base.dst_rect.width = width;
	outdev->surface_state.base.dst_rect.height = height;
	outdev->surface_state.base.alpha = 1;

	/* we use this later to let output to be input for composition */
	fmt = &outdev->surface_state.fmt;
	fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt->fmt.pix_mp.width = width;
	fmt->fmt.pix_mp.height = height;
	fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_ABGR32;
	fmt->fmt.pix_mp.num_planes = 1;

#ifdef VSP2_SCALER_ENABLED
	struct vsp_device *vsp = (struct vsp_device*)dev;

	if (vsp->scaler_enable) {
		int ret = vsp2_scaler_create_buffer(vsp->scaler,
						    vsp->base.drm_fd,
						    vsp->base.kms,
						    width, height);
		if (ret) {
			weston_log("Can't create buffer for scaling. Disabling VSP scaler.\n");
			vsp->scaler_enable = 0;
		}
	}
#endif

	return (struct v4l2_renderer_output*)outdev;
}

static inline int
vsp2_dequeue_capture_buffer(int fd)
{
	struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
	struct v4l2_buffer buf = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_DMABUF,
		.index = 0,
		.m.planes = planes,
		.length = 1
	};

	if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
		weston_log("VIDIOC_DQBUF failed on %d (%s).\n", fd, strerror(errno));
		return -1;
	}

	return 0;
}

static inline int
_vsp2_queue_buffer(int fd, enum v4l2_buf_type type, struct vsp_surface_state *vs)
{
	struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
	struct v4l2_buffer buf = {
		.type = type,
		.memory = V4L2_MEMORY_DMABUF,
		.index = 0,
		.m.planes = planes,
		.length = vs->base.num_planes
	};
	int i;

	for (i = 0; i < vs->base.num_planes; i++) {
		buf.m.planes[i].m.fd = vs->base.planes[i].dmafd;
		buf.m.planes[i].length = vs->base.planes[i].length;
		buf.m.planes[i].bytesused = vs->base.planes[i].bytesused;
	}

	if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
		weston_log("VIDIOC_QBUF failed for dmafd=%d(%d planes) on %d (%s).\n",
			   vs->base.planes[i].dmafd, vs->base.num_planes, fd, strerror(errno));
		return -1;
	}

	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		for (i = 0; i < vs->base.num_planes; i++) {
			vs->base.planes[i].length = buf.m.planes[i].length;
			/* XXX:
			   Set length value to bytesused because bytesused is
			   returned 0 from kernel driver. Need to set returned
			   bytesused value. */
			vs->base.planes[i].bytesused = buf.m.planes[i].length;
		}
	}
	return 0;
}

#define vsp2_queue_capture_buffer(fd, vs) \
	_vsp2_queue_buffer((fd), V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, (vs))

#define vsp2_queue_output_buffer(fd, vs) \
	_vsp2_queue_buffer((fd), V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, (vs))

static inline int
_vsp2_request_buffer(int fd, enum v4l2_buf_type type, int count)
{
	struct v4l2_requestbuffers reqbuf = {
		.type = type,
		.memory = V4L2_MEMORY_DMABUF,
		.count = count
	};

	if (ioctl(fd, VIDIOC_REQBUFS, &reqbuf) == -1) {
		weston_log("clearing VIDIOC_REQBUFS failed (%s).\n", strerror(errno));
		return -1;
	}

	return 0;
}

#define vsp2_request_capture_buffer(fd, cnt) \
	_vsp2_request_buffer((fd), V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, (cnt))

#define vsp2_request_output_buffer(fd, cnt) \
	_vsp2_request_buffer((fd), V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, (cnt))

static void
vsp2_comp_begin(struct v4l2_renderer_device *dev, struct v4l2_renderer_output *out)
{
	struct vsp_device *vsp = (struct vsp_device*)dev;
	struct vsp_renderer_output *output = (struct vsp_renderer_output*)out;
	struct v4l2_format *fmt = &output->surface_state.fmt;

	DBG("start vsp composition.\n");

	vsp->state = VSP_STATE_START;

	if (!memcmp(&vsp->current_wpf_fmt, fmt, sizeof(struct v4l2_format))) {
		DBG(">>> No need to set up the output.\n");
		goto skip;
	}

	vsp2_set_output(vsp, output);

	// dump the old setting
	vsp2_request_capture_buffer(vsp->wpf->devnode.fd, 0);

	// set format for composition output via wpf.0
	fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	vsp2_set_format(vsp->wpf->devnode.fd, fmt, 0);

	// set back the type to be used by bru as an input
	fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	// we require only one buffer for wpf.0
	vsp2_request_capture_buffer(vsp->wpf->devnode.fd, 1);

	// keep the current setting
	vsp->current_wpf_fmt = *fmt;

skip:
	vsp->output_surface_state = &output->surface_state;
	DBG("output set to dmabuf=%d\n", vsp->output_surface_state->base.planes[0].dmafd);
}

static int
vsp2_comp_setup_inputs(struct vsp_device *vsp, struct vsp_input *input, bool enable)
{
	struct vsp_surface_state *vs = input->input_surface_states;
	struct v4l2_rect *src = &input->src;
	struct v4l2_rect *dst = &input->dst;
	struct vsp2_media_entity *rpf = input->rpf;
	struct vsp2_media_entity *bru = vsp->bru;
	struct media_link_desc *link = &rpf->link;
	struct v4l2_subdev_format subdev_fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE
	};
	struct v4l2_subdev_selection subdev_sel = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE
	};
	struct v4l2_control v4l2_ctrl_alpha = {
		.id = V4L2_CID_ALPHA_COMPONENT
	};

	// enable link associated with this pad
	if (enable)
		link->flags |= MEDIA_LNK_FL_ENABLED;
	else
		link->flags &= ~MEDIA_LNK_FL_ENABLED;

	if (ioctl(vsp->base.media_fd, MEDIA_IOC_SETUP_LINK, link) < 0) {
		weston_log("enabling media link setup failed.\n");
		return -1;
	}

	if (!enable)
		return 0;

	// dump the old setting
	if (vsp2_request_output_buffer(rpf->devnode.fd, 0) < 0)
		return -1;

	// set input format
	if (vsp2_set_format(rpf->devnode.fd, &vs->fmt, input->opaque))
		return -1;

	// set size and formart for rpf.n:0, the input format
	subdev_fmt.pad = 0;
	subdev_fmt.format.width = vs->fmt.fmt.pix_mp.width;
	subdev_fmt.format.height = vs->fmt.fmt.pix_mp.height;
	subdev_fmt.format.code = vs->mbus_code;

	if (ioctl(rpf->subdev.fd, VIDIOC_SUBDEV_S_FMT, &subdev_fmt) < 0) {
		weston_log("set input format via subdev failed.\n");
		return -1;
	}

	// set an alpha
	v4l2_ctrl_alpha.value = (__s32)(vs->base.alpha * 0xff);
	if (ioctl(rpf->subdev.fd, VIDIOC_S_CTRL, &v4l2_ctrl_alpha) < 0) {
		weston_log("setting alpha (=%f) failed.", vs->base.alpha);
		return -1;
	}

	// set a crop paramters for the input
	subdev_sel.pad = 0;
	subdev_sel.target = V4L2_SEL_TGT_CROP;
	subdev_sel.r = *src;
	if (ioctl(rpf->subdev.fd, VIDIOC_SUBDEV_S_SELECTION, &subdev_sel) < 0) {
		weston_log("set crop parameter failed: %dx%d@(%d,%d).\n",
			   src->width, src->height, src->left, src->top);
		return -1;
	}
	*src = subdev_sel.r;

	// this is rpf.n:1, the output towards BRU. this shall be consistent among all inputs.
	subdev_fmt.pad = 1;
	subdev_fmt.format.width = src->width;
	subdev_fmt.format.height = src->height;
	subdev_fmt.format.code = V4L2_MBUS_FMT_ARGB8888_1X32;
	if (ioctl(rpf->subdev.fd, VIDIOC_SUBDEV_S_FMT, &subdev_fmt) < 0) {
		weston_log("set output format via subdev failed.\n");
		return -1;
	}

	// so does the BRU input. get the pad index from the link desc.
	// the reset are the same.
	subdev_fmt.pad = link->sink.index;
	if (ioctl(bru->subdev.fd, VIDIOC_SUBDEV_S_FMT, &subdev_fmt) < 0) {
		weston_log("set composition format via subdev failed.\n");
		return -1;
	}

	// set a composition paramters
	subdev_sel.pad = link->sink.index;
	subdev_sel.target = V4L2_SEL_TGT_COMPOSE;
	subdev_sel.r = *dst;
	if (ioctl(bru->subdev.fd, VIDIOC_SUBDEV_S_SELECTION, &subdev_sel) < 0) {
		weston_log("set compose parameter failed: %dx%d@(%d,%d).\n",
			   dst->width, dst->height, dst->left, dst->top);
		return -1;
	}

	// request a buffer
	if (vsp2_request_output_buffer(rpf->devnode.fd, 1) < 0)
		return -1;

	// queue buffer
	if (vsp2_queue_output_buffer(rpf->devnode.fd, vs) < 0)
		return -1;

	return 0;
}

static int
vsp2_comp_flush(struct vsp_device *vsp)
{
	int i, fd;
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	DBG("flush vsp composition.\n");
#ifdef VSP2_SCALER_ENABLED
	vsp->scaler_count = 0;
#endif

	// enable links and queue buffer
	for (i = 0; i < vsp->input_count; i++)
		vsp2_comp_setup_inputs(vsp, &vsp->inputs[i], true);

	// disable unused inputs
	for (i = vsp->input_count; i < vsp->input_max; i++)
		vsp2_comp_setup_inputs(vsp, &vsp->inputs[i], false);

	// get an output pad
	fd = vsp->wpf->devnode.fd;

	// queue buffer
	if (vsp2_queue_capture_buffer(fd, vsp->output_surface_state) < 0)
		goto error;

	// stream on
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	for (i = 0; i < vsp->input_count; i++) {
		if (ioctl(vsp->inputs[i].rpf->devnode.fd, VIDIOC_STREAMON, &type) == -1) {
			weston_log("VIDIOC_STREAMON failed for input %d. (%s)\n", i, strerror(errno));
		}
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(fd, VIDIOC_STREAMON, &type) == -1) {
		weston_log("VIDIOC_STREAMON failed for output (%s).\n", strerror(errno));
		goto error;
	}

	// dequeue buffer
	if (vsp2_dequeue_capture_buffer(fd) < 0)
		goto error;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(fd, VIDIOC_STREAMOFF, &type) == -1) {
		weston_log("%s: VIDIOC_STREAMOFF failed on %d (%s).\n", __func__, fd, strerror(errno));
		goto error;
	}

	// stream off
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	for (i = 0; i < vsp->input_count; i++) {
		if (ioctl(vsp->inputs[i].rpf->devnode.fd, VIDIOC_STREAMOFF, &type) == -1) {
			weston_log("VIDIOC_STREAMOFF failed for input %d.\n", i);
		}
	}

	vsp->input_count = 0;
	return 0;

error:
	vsp->input_count = 0;
	return -1;
}

static void
vsp2_comp_finish(struct v4l2_renderer_device *dev)
{
	struct vsp_device *vsp = (struct vsp_device*)dev;

	if (vsp->input_count > 0)
		vsp2_comp_flush(vsp);

	vsp->state = VSP_STATE_IDLE;
	DBG("complete vsp composition.\n");
	vsp->output_surface_state = NULL;
}

#define IS_IDENTICAL_RECT(a, b) ((a)->width == (b)->width && (a)->height == (b)->height && \
				 (a)->left  == (b)->left  && (a)->top    == (b)->top)

#ifdef VSP2_SCALER_ENABLED
static int
vsp2_do_scaling(struct vsp_scaler_device *scaler, struct vsp_input *input,
		struct v4l2_rect *src, struct v4l2_rect *dst)
{
	struct vsp_surface_state *scaler_vs = &scaler->state;
	struct v4l2_format *fmt = &scaler_vs->fmt;
	int type;

	struct vsp_surface_state *vs = input->input_surface_states;
	struct v4l2_subdev_format subdev_fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE
	};
	struct v4l2_subdev_selection subdev_sel = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE
	};

	// set size and format for rpf.0:0; the input format
	subdev_fmt.pad = 0;
	subdev_fmt.format.width = vs->base.width;
	subdev_fmt.format.height = vs->base.height;
	subdev_fmt.format.code = vs->mbus_code;

	if (ioctl(scaler->rpf->subdev.fd, VIDIOC_SUBDEV_S_FMT, &subdev_fmt) < 0) {
		weston_log("set rpf.0 input format via subdev failed.\n");
		return -1;
	}

	// set a crop paramters for the input
	subdev_sel.pad = 0;
	subdev_sel.target = V4L2_SEL_TGT_CROP;
	subdev_sel.r = *src;
	if (ioctl(scaler->rpf->subdev.fd, VIDIOC_SUBDEV_S_SELECTION, &subdev_sel) < 0) {
		weston_log("set rcop parameter failed: %dx%d@(%d,%d).\n",
			   src->width, src->height, src->left, src->top);
		return -1;
	}
	*src = subdev_sel.r;

	// set rpf.0:1; the output towards UDS.
	subdev_fmt.pad = 1;
	subdev_fmt.format.width = src->width;
	subdev_fmt.format.height = src->height;
	subdev_fmt.format.code = V4L2_MBUS_FMT_ARGB8888_1X32;

	if (ioctl(scaler->rpf->subdev.fd, VIDIOC_SUBDEV_S_FMT, &subdev_fmt) < 0) {
		weston_log("set rpf.0 output format via subdev failed.\n");
		return -1;
	}

	// set uds.0:0 to be the same as rpf.0:1
	subdev_fmt.pad = 0;
	if (ioctl(scaler->uds->subdev.fd, VIDIOC_SUBDEV_S_FMT, &subdev_fmt) < 0) {
		weston_log("set input format of UDS via subdev failed.\n");
		return -1;
	}

	// uds.0:1 output size; the color format should be the same as the input
	subdev_fmt.pad = 1;
	subdev_fmt.format.width = dst->width;
	subdev_fmt.format.height = dst->height;
	if (ioctl(scaler->uds->subdev.fd, VIDIOC_SUBDEV_S_FMT, &subdev_fmt) < 0) {
		weston_log("set output format of UDS via subdev failed.\n");
		return -1;
	}

	// wpf.0:0 input; same as uds.0:1
	subdev_fmt.pad = 0;
	if (ioctl(scaler->wpf->subdev.fd, VIDIOC_SUBDEV_S_FMT, &subdev_fmt) < 0) {
		weston_log("set input format of WPF via subdev failed.\n");
		return -1;
	}

	/* queue buffer for input */
	if (vsp2_request_output_buffer(scaler->rpf->devnode.fd, 0) < 0)
		return -1;

	if (vsp2_set_format(scaler->rpf->devnode.fd, &vs->fmt, input->opaque))
		return -1;

	if (vsp2_request_output_buffer(scaler->rpf->devnode.fd, 1) < 0)
		return -1;

	if (vsp2_queue_output_buffer(scaler->rpf->devnode.fd, vs) < 0)
		return -1;

	/* queue buffer for output */
	vsp2_request_capture_buffer(scaler->wpf->devnode.fd, 0);

	fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt->fmt.pix_mp.width = dst->width;
	fmt->fmt.pix_mp.height = dst->height;
	fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_ABGR32;
	fmt->fmt.pix_mp.num_planes = 1;

	vsp2_set_format(scaler->wpf->devnode.fd, fmt, 0);

	fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	vsp2_request_capture_buffer(scaler->wpf->devnode.fd, 1);

	scaler_vs->base.num_planes = 1;
	if (vsp2_queue_capture_buffer(scaler->wpf->devnode.fd, scaler_vs) < 0)
		return -1;

	/* execute scaling */
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (ioctl(scaler->rpf->devnode.fd, VIDIOC_STREAMON, &type) == -1) {
		weston_log("VIDIOC_STREAMON failed for scaler input. (%s)\n",
			   strerror(errno));
		return -1;
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(scaler->wpf->devnode.fd, VIDIOC_STREAMON, &type) == -1) {
		weston_log("VIDIOC_STREAMON failed for scaler output. (%s)\n",
			   strerror(errno));
		return -1;
	}

	if (vsp2_dequeue_capture_buffer(scaler->wpf->devnode.fd) < 0)
		return -1;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(scaler->wpf->devnode.fd, VIDIOC_STREAMOFF, &type) == -1) {
		weston_log("VIDIOC_STREAMOFF failed for scaler output. (%s)\n",
			   strerror(errno));
	}

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (ioctl(scaler->rpf->devnode.fd, VIDIOC_STREAMOFF, &type) == -1) {
		weston_log("VIDIOC_STREAMOFF failed for scaler input. (%s)\n",
			   strerror(errno));
	}

	scaler_vs->base.width = dst->width;
	scaler_vs->base.height = dst->height;
	scaler_vs->mbus_code = V4L2_MBUS_FMT_ARGB8888_1X32;
	scaler_vs->base.alpha = vs->base.alpha;
	fmt->type = vs->fmt.type;

	input->input_surface_states = scaler_vs;
	input->src.left = input->src.top = 0;
	input->src.width = dst->width;
	input->src.height = dst->height;

	return 0;
}
#endif

static int
vsp2_do_draw_view(struct vsp_device *vsp, struct vsp_surface_state *vs, struct v4l2_rect *src, struct v4l2_rect *dst,
		 int opaque)
{
	if (src->width < 2 || src->height < 2) {
		DBG("ignoring the size of zeros < (%dx%d)\n", src->width, src->height);
		return 0;
	}

	if (src->width > 8190 || src->height > 8190) {
		weston_log("ignoring the size exceeding the limit (8190x8190) < (%dx%d)\n", src->width, src->height);
		return 0;
	}

	if (src->left + (int)src->width <= 0 || src->top + (int)src->height <= 0) {
		DBG("ignoring the source right/bottom less than equal zero. src:(%d, %d) %dx%d\n", src->left, src->top, src->width, src->height);
		return 0;
	}

#ifdef VSP2_SCALER_ENABLED
	int should_use_scaler = 0;

	if (vsp->scaler_enable &&
	    (dst->width != src->width || dst->height != src->height)) {
		if (src->width < VSP_SCALER_MIN_PIXELS || src->height < VSP_SCALER_MIN_PIXELS) {
			weston_log("ignoring the size the scaler can't handle (input size=%dx%d).\n",
				   src->width, src->height);
			return 0;
		}
		should_use_scaler = 1;
	}
#endif

	if (src->left < 0) {
		src->width += src->left;
		dst->left -= src->left;
		src->left = 0;
	}

	if (src->top < 0) {
		src->height += src->top;
		dst->top -= src->top;
		src->top = 0;
	}

	DBG("set input %d (dmafd=%d): %dx%d@(%d,%d)->%dx%d@(%d,%d). alpha=%f\n",
	    vsp->input_count,
	    vs->base.planes[0].dmafd,
	    src->width, src->height, src->left, src->top,
	    dst->width, dst->height, dst->left, dst->top,
	    vs->base.alpha);

	switch(vsp->state) {
	case VSP_STATE_START:
		DBG("VSP_STATE_START -> COMPSOING\n");
		vsp->state = VSP_STATE_COMPOSING;
		break;

	case VSP_STATE_COMPOSING:
		if (vsp->input_count == 0) {
			DBG("VSP_STATE_COMPOSING -> START (compose with output)\n");
			vsp->state = VSP_STATE_START;
			if (vsp2_do_draw_view(vsp, vsp->output_surface_state,
					     &vsp->output_surface_state->base.src_rect,
					     &vsp->output_surface_state->base.dst_rect, 0) < 0)
				return -1;
		}
		break;

	default:
		weston_log("unknown state... %d\n", vsp->state);
		return -1;
	}

	struct vsp_input *input = &vsp->inputs[vsp->input_count];

	// get an available input pad
	input->input_surface_states = vs;
	input->src = *src;
	input->dst = *dst;
	input->opaque = opaque;

#ifdef VSP2_SCALER_ENABLED
	/* check if we need to use a scaler */
	if (should_use_scaler) {
		DBG("We need to use a scaler. (%dx%d)->(%dx%d)\n",
		    src->width, src->height, dst->width, dst->height);

		// if all scaler buffers have already been used, we must compose now.
                if (vsp->scaler_count == vsp->scaler_max) {
			vsp2_comp_flush(vsp);
			return vsp2_do_draw_view(vsp, vs, src, dst, opaque);
		}

		vsp2_do_scaling(vsp->scaler, input, src, dst);
		vsp->scaler_count++;
	}
#endif

	// check if we should flush now
	vsp->input_count++;
	if (vsp->input_count == vsp->input_max)
		vsp2_comp_flush(vsp);

	return 0;
}

static int
vsp2_comp_draw_view(struct v4l2_renderer_device *dev, struct v4l2_surface_state *surface_state)
{
	struct vsp_device *vsp = (struct vsp_device*)dev;
	struct vsp_surface_state *vs = (struct vsp_surface_state*)surface_state;

	DBG("start rendering a view.\n");
	if (!IS_IDENTICAL_RECT(&surface_state->dst_rect, &surface_state->opaque_dst_rect)) {
		DBG("rendering non-opaque region.\n");
		if (vsp2_do_draw_view(vsp, vs, &surface_state->src_rect, &surface_state->dst_rect, 0) < 0)
			return -1;
	}

	DBG("rendering opaque region if available.\n");
	if (vsp2_do_draw_view(vsp, vs, &surface_state->opaque_src_rect, &surface_state->opaque_dst_rect, 1) < 0)
		return -1;

	return 0;
}

static void
vsp2_set_output_buffer(struct v4l2_renderer_output *out, struct v4l2_bo_state *bo)
{
	struct vsp_renderer_output *output = (struct vsp_renderer_output*)out;
	DBG("set output dmafd to %d\n", bo->dmafd);
	output->surface_state.base.planes[0].dmafd = bo->dmafd;
	output->surface_state.fmt.fmt.pix_mp.plane_fmt[0].bytesperline = bo->stride;
}

#ifdef V4L2_GL_FALLBACK_ENABLED
static int
vsp2_can_compose(struct v4l2_renderer_device *dev, struct v4l2_view *view_list, int count)
{
	struct vsp_device *vsp = (struct vsp_device*)dev;
	int i;

	if (vsp->max_views_to_compose > 0 && vsp->max_views_to_compose < count)
		return 0;

	for (i = 0; i < count; i++) {
		struct weston_view *ev = view_list[i].view;
		float *d = ev->transform.matrix.d;
		struct weston_surface *surf = ev->surface;
		float *vd = surf->buffer_to_surface_matrix.d;

		if ((ev->transform.matrix.type | surf->buffer_to_surface_matrix.type) & WESTON_MATRIX_TRANSFORM_ROTATE)
			return 0;
#ifdef VSP2_SCALER_ENABLED
		if (vsp->scaler_enable && d[0] > 0 && d[5] > 0 && vd[0] > 0 && vd[5] > 0)
			continue;
#endif
		if (ev->output->zoom.active ||
		    d[0] != 1.0 || d[5] != 1.0 || d[10] != 1.0 ||
		    vd[0] != 1.0 || vd[5] != 1.0 || vd[10] != 1.0)
			return 0;
	}

	return 1;
}
#endif

static uint32_t
vsp2_get_capabilities(void)
{
	return 0;
}

static bool
vsp2_check_format(uint32_t color_format)
{
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(vsp2_support_formats); i++) {
		if (color_format == vsp2_support_formats[i])
			return true;
	}

	return false;
}

WL_EXPORT struct v4l2_device_interface v4l2_device_interface = {
	.init = vsp2_init,

	.create_output = vsp2_create_output,
	.set_output_buffer = vsp2_set_output_buffer,

	.create_surface = vsp2_create_surface,
	.attach_buffer = vsp2_attach_buffer,

	.begin_compose = vsp2_comp_begin,
	.finish_compose = vsp2_comp_finish,
	.draw_view = vsp2_comp_draw_view,

#ifdef V4L2_GL_FALLBACK_ENABLED
	.can_compose = vsp2_can_compose,
#endif

	.get_capabilities = vsp2_get_capabilities,
	.check_format = vsp2_check_format,
};

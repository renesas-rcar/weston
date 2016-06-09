/*
 * Copyright © 2014 Renesas Electronics Corp.
 *
 * Based on pixman-renderer by:
 * Copyright © 2012 Intel Corporation
 * Copyright © 2013 Vasily Khoruzhick <anarsoul@gmail.com>
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Authors:
 *	Takanari Hayama <taki@igel.co.jp>
 */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include "v4l2-renderer.h"
#include "v4l2-renderer-device.h"

#include <libmediactl-v4l2/mediactl.h>
#include <libmediactl-v4l2/mediactl-priv.h>
#include <libmediactl-v4l2/v4l2subdev.h>
#include <libmediactl-v4l2/tools.h>

#ifdef V4L2_GL_FALLBACK
#include <unistd.h>
#include <xf86drm.h>
#include <libkms/libkms.h>
#endif

#include <linux/input.h>

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

#define VSP_INPUT_MAX	4
#define VSP_SCALER_MAX	1
#define VSP_SCALER_MIN_PIXELS	4	// UDS can't take pixels smaller than this

const char *vsp_input_links[] = {
	"'%s rpf.0':1 -> '%s bru':0",
	"'%s rpf.1':1 -> '%s bru':1",
	"'%s rpf.2':1 -> '%s bru':2",
	"'%s rpf.3':1 -> '%s bru':3"
};

const char *vsp_output_links[] = {
	"'%s bru':5 -> '%s wpf.0':0",
	"'%s wpf.0':1 -> '%s wpf.0 output':0"
};

const char *vsp_inputs[] = {
	"%s rpf.0 input",
	"%s rpf.1 input",
	"%s rpf.2 input",
	"%s rpf.3 input"
};

const char *vsp_output = {
	"%s wpf.0 output"
};

const char *vsp_input_infmt[] = {
	"'%s rpf.0':0",
	"'%s rpf.1':0",
	"'%s rpf.2':0",
	"'%s rpf.3':0"
};

const char *vsp_input_outfmt[] = {
	"'%s rpf.0':1",
	"'%s rpf.1':1",
	"'%s rpf.2':1",
	"'%s rpf.3':1"
};

const char *vsp_input_composer[] = {
	"'%s bru':0",
	"'%s bru':1",
	"'%s bru':2",
	"'%s bru':3"
};

const char *vsp_input_subdev[] = {
	"%s rpf.0",
	"%s rpf.1",
	"%s rpf.2",
	"%s rpf.3"
};

const char *vsp_output_fmt[] = {
	"'%s bru':5",
	"'%s wpf.0':0",
	"'%s wpf.0':1"
};

struct vsp_media_pad {
	struct media_pad	*infmt_pad;
	struct media_pad	*outfmt_pad;
	struct media_pad	*compose_pad;
	struct media_entity	*input_entity;

	struct media_link	*link;

	int			fd;
};

struct vsp_output {
	struct media_pad	*pads[ARRAY_SIZE(vsp_output_fmt)];
};

typedef enum {
	VSP_STATE_IDLE,
	VSP_STATE_START,
	VSP_STATE_COMPOSING,
} vsp_state_t;

struct vsp_input {
	struct vsp_media_pad input_pads;
	struct vsp_surface_state *input_surface_states;
	struct v4l2_rect src;
	struct v4l2_rect dst;
	int opaque;
};

#ifdef V4L2_GL_FALLBACK
typedef enum {
    SCALER_TYPE_OFF = 0,
    SCALER_TYPE_VSP,
    SCALER_TYPE_GL,
} scaler_t;
static scaler_t scaler_type = SCALER_TYPE_OFF;	/* default: scaling off */

/* scaler */
const char *vsp_scaler_input_link = {
	"'%s rpf.0':1 -> '%s uds.0':0"
};

const char *vsp_scaler_output_link = {
	"'%s uds.0':1 -> '%s wpf.0':0"
};

const char *vsp_scaler_input = {
	"%s rpf.0 input"
};

const char *vsp_scaler_output = {
	"%s wpf.0 output"
};

const char *vsp_scaler_input_infmt = {
	"'%s rpf.0':0"
};

const char *vsp_scaler_input_outfmt = {
	"'%s rpf.0':1"
};

const char *vsp_scaler_uds_infmt = {
	"'%s uds.0':0"
};

const char *vsp_scaler_uds_outfmt = {
	"'%s uds.0':1"
};

const char *vsp_scaler_output_fmt = {
	"'%s wpf.0':0",
};

struct vsp2_scaler_media_pad {
	struct media_pad *infmt;
	struct media_pad *outfmt;
	struct media_link *link;

	int fd;
};

struct vsp_scaler_device {
	struct vsp2_scaler_media_pad input;
	struct vsp2_scaler_media_pad uds;
	struct media_pad *output_fmt;

	struct vsp_surface_state state;
	int width;
	int height;
	int dmafd;
};
#endif


struct vsp_device {
	struct v4l2_renderer_device base;

	vsp_state_t state;

	struct vsp_media_pad output_pad;
	struct vsp_surface_state *output_surface_state;

	int input_count;
	int input_max;
	struct vsp_input inputs[VSP_INPUT_MAX];

	struct vsp_output output;

#ifdef V4L2_GL_FALLBACK
	int scaler_count;
	int scaler_max;
	struct vsp_scaler_device *scaler;
#endif
};

#ifdef V4L2_GL_FALLBACK
static int max_views_to_compose = -1;
#endif

static void
video_debug_mediactl(void)
{
	FILE *p = popen("media-ctl -d /dev/media4 -p", "r");
	char buf[BUFSIZ * 16];

	if (!p)
		return;

	weston_log("====== output of media-ctl ======\n");
	while(!feof(p)) {
		fread(buf, sizeof(buf), 1, p);
		weston_log_continue(buf);
	}
	weston_log_continue("\n================================\n");

	pclose(p);
}

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
vsp2_check_capabiility(int fd, const char *devname)
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

#ifdef V4L2_GL_FALLBACK
static struct vsp_scaler_device*
vsp2_scaler_init(struct weston_config_section *section)
{
	char *vspi_device;
	struct media_device *media;
	struct media_link *link;
	struct media_entity *entity;
	const struct media_device_info *info;
	const char *device_name, *devname;
	char buf[128], *p, *endp;
	struct vsp_scaler_device *vspi = NULL;

	weston_config_section_get_string(section, "vspi-device", &vspi_device,
					 "/dev/media6");

	media = media_device_new(vspi_device);
	if (!media) {
		weston_log("Can't create %s.\n", vspi_device);
		goto error;
	}

	if (media_device_enumerate(media)) {
		weston_log("Can't enumerate %s.\n", vspi_device);
		goto error;
	}

	info = media_get_info(media);
	weston_log("Media device information\n"
		   "------------------------\n"
		   "driver              %s\n"
		   "model               %s\n"
		   "serial              %s\n"
		   "bus info            %s\n"
		   "hw revision         0x%x\n"
		   "driver version      %u.%u.%u\n",
		   info->driver, info->model, info->serial,
		   info->bus_info, info->hw_revision,
		   (info->driver_version >> 16) & 0xff,
		   (info->driver_version >>  8) & 0xff,
		   (info->driver_version)       & 0xff);

	if ((p = strchr(info->bus_info, ':')))
		device_name = p + 1;
	else
		device_name = info->bus_info;

	/*
	 * XXX: This needs to be fixed. Renesas VSP driver has differenet
	 * naming rule from upstream. This should be fixed to make the code
	 * works for upstream drivers as well.
	 *
	 * TODO: We also need to refactor the code, so that we don't have
	 * duplication of codes. Especially, these media controller related
	 * code.
	 */
	if (strncmp(device_name, "soc:vspi", 8)) {
		weston_log("The device is not VSPI.\n");
		goto error;
	}

	vspi = calloc(1, sizeof(*vspi));
	if (!vspi) {
		weston_log("Can't alloc memory.\n");
		goto error;
	}

	/* Reset link */
	if (media_reset_links(media)) {
		weston_log("Reset media controller links failed.\n");
		goto error;
	}

	/* RPF => UDS */
	weston_log("Setting up input.\n");
	/* set up link */
	snprintf(buf, sizeof(buf), vsp_scaler_input_link, device_name,
		 device_name);
	weston_log("setting up link: '%s'\n", buf);
	link = media_parse_link(media, buf, &endp);
	if (media_setup_link(media, link->source, link->sink, 1)) {
		weston_log("link set up failed.\n");
		goto error;
	}
	vspi->input.link = link;

	/* RPF input */
	snprintf(buf, sizeof(buf), vsp_scaler_input_infmt, device_name);
	weston_log("get an input pad: '%s'\n", buf);
	if (!(vspi->input.infmt = media_parse_pad(media, buf, NULL))) {
		weston_log("parse pad failed.\n");
		goto error;
	}

	/* RPF output */
	snprintf(buf, sizeof(buf), vsp_scaler_input_outfmt, device_name);
	weston_log("get an input sink: '%s'\n", buf);
	if (!(vspi->input.outfmt = media_parse_pad(media, buf, NULL))) {
		weston_log("parse pad failed.\n");
		goto error;
	}

	/* UDS input */
	snprintf(buf, sizeof(buf), vsp_scaler_uds_infmt, device_name);
	weston_log("get a scaler pad: '%s'\n", buf);
	if (!(vspi->uds.infmt = media_parse_pad(media, buf, NULL))) {
		weston_log("parse pad failed.\n");
		goto error;
	}

	/* get a file descriptor for the input */
	snprintf(buf, sizeof(buf), vsp_scaler_input, device_name);
	entity = media_get_entity_by_name(media, buf, strlen(buf));
	if (!entity) {
		weston_log("error... '%s' not found.\n", buf);
		goto error;
	}

	if (v4l2_subdev_open(entity)) {
		weston_log("subdev '%s' open failed.\n", buf);
		goto error;
	}

	vspi->input.fd = entity->fd;
	vsp2_check_capabiility(vspi->input.fd,
			       media_entity_get_devname(entity));

	/* UDS => WPF */
	weston_log("Setting up an output.\n");
	/* set up link */
	snprintf(buf, sizeof(buf), vsp_scaler_output_link, device_name,
		 device_name);
	weston_log("setting up link: '%s'\n", buf);
	link = media_parse_link(media, buf, &endp);
	if (media_setup_link(media, link->source, link->sink, 1)) {
		weston_log("link set up failed.\n");
		goto error;
	}
	vspi->uds.link = link;

	/* UDS output */
	snprintf(buf, sizeof(buf), vsp_scaler_uds_outfmt, device_name);
	weston_log("get a scaler sink: '%s'\n", buf);
	if (!(vspi->uds.outfmt = media_parse_pad(media, buf, NULL))) {
		weston_log("parse pad failed.\n");
		goto error;
	}

	/* WPF input */
	snprintf(buf, sizeof(buf), vsp_scaler_output_fmt, device_name);
	weston_log("get a output pad: '%s'\n", buf);
	if (!(vspi->output_fmt = media_parse_pad(media, buf, NULL))) {
		weston_log("parse pad faild.\n");
		goto error;
	}

	snprintf(buf, sizeof(buf), vsp_scaler_output, device_name);
	entity = media_get_entity_by_name(media, buf, strlen(buf));
	if (!entity) {
		weston_log("error... '%s' not found.\n", buf);
		goto error;
	}

	devname = media_entity_get_devname(entity);
	weston_log("output '%s' is associated with '%s'\n", buf, devname);
	vspi->uds.fd = open(devname, O_RDWR);
	if (vspi->uds.fd < 0) {
		weston_log("error... can't open '%s'.\n", devname);
		goto error;
	}
	vsp2_check_capabiility(vspi->uds.fd, devname);

	return vspi;

error:
	if (vspi)
		free(vspi);
	weston_log("VSPI device init failed...\n");
	return NULL;
}
#endif

static struct v4l2_renderer_device*
vsp2_init(struct media_device *media, struct weston_config *config)
{
	struct vsp_device *vsp = NULL;
	struct media_link *link;
	struct media_entity *entity;
	const struct media_device_info *info;
	char buf[128], *p, *endp;
	const char *device_name, *devname;
	int i;
	struct weston_config_section *section;

	/* Get device name */
	info = media_get_info(media);
	if ((p = strchr(info->bus_info, ':')))
		device_name = p + 1;
	else
		device_name = info->bus_info;

	if (strncmp(device_name, "soc:vsp", 7)) {
		weston_log("The device is not VSP.");
		goto error;
	}

	weston_log("Using the device %s\n", device_name);

	vsp = calloc(1, sizeof(struct vsp_device));
	if (!vsp)
		goto error;
	vsp->base.media = media;
	vsp->base.device_name = device_name;
	vsp->state = VSP_STATE_IDLE;
	vsp->scaler_max = VSP_SCALER_MAX;

	/* check configuration */
	section = weston_config_get_section(config,
					    "vsp-renderer", NULL, NULL);
	weston_config_section_get_int(section, "max_inputs", &vsp->input_max, VSP_INPUT_MAX);
#ifdef V4L2_GL_FALLBACK
	weston_config_section_get_int(section, "max_views_to_compose", &max_views_to_compose, -1);
#endif

	if (vsp->input_max < 2)
		vsp->input_max = 2;
	if (vsp->input_max > VSP_INPUT_MAX)
		vsp->input_max = VSP_INPUT_MAX;

	/* Reset links */
	if (media_reset_links(media)) {
		weston_log("Reset media controller links failed.\n");
		goto error;
	}

	/* Initialize inputs */
	weston_log("Setting up inputs. Use %d inputs.\n", vsp->input_max);
	for (i = 0; i < vsp->input_max; i++) {
		struct vsp_media_pad *pads = &vsp->inputs[i].input_pads;

		/* setup a link - do not enable yet */
		snprintf(buf, sizeof(buf), vsp_input_links[i], device_name, device_name);
		weston_log("setting up link: '%s'\n", buf);
		link = media_parse_link(media, buf, &endp);
		if (media_setup_link(media, link->source, link->sink, 0)) {
			weston_log("link set up failed.\n");
			goto error;
		}
		pads->link = link;

		/* get a pad to configure the compositor */
		snprintf(buf, sizeof(buf), vsp_input_infmt[i], device_name);
		weston_log("get an input pad: '%s'\n", buf);
		if (!(pads->infmt_pad = media_parse_pad(media, buf, NULL))) {
			weston_log("parse pad failed.\n");
			goto error;
		}

		snprintf(buf, sizeof(buf), vsp_input_outfmt[i], device_name);
		weston_log("get an input sink: '%s'\n", buf);
		if (!(pads->outfmt_pad = media_parse_pad(media, buf, NULL))) {
			weston_log("parse pad failed.\n");
			goto error;
		}

		snprintf(buf, sizeof(buf), vsp_input_composer[i], device_name);
		weston_log("get a composer pad: '%s'\n", buf);
		if (!(pads->compose_pad = media_parse_pad(media, buf, NULL))) {
			weston_log("parse pad failed.\n");
			goto error;
		}

		snprintf(buf, sizeof(buf), vsp_input_subdev[i], device_name);
		weston_log("get a input subdev pad: '%s'\n", buf);
		if (!(pads->input_entity = media_get_entity_by_name(media, buf, strlen(buf)))) {
			weston_log("parse entity failed.\n");
			goto error;
		}

		/* get a file descriptor for the input */
		snprintf(buf, sizeof(buf), vsp_inputs[i], device_name);
		entity = media_get_entity_by_name(media, buf, strlen(buf));
		if (!entity) {
			weston_log("error... '%s' not found.\n", buf);
			goto error;
		}

		if (v4l2_subdev_open(entity)) {
			weston_log("subdev '%s' open failed\n.", buf);
			goto error;
		}

		pads->fd = entity->fd;
		vsp2_check_capabiility(pads->fd, media_entity_get_devname(entity));

		/* set an input format for BRU to be ARGB (default) */
		{
			struct v4l2_mbus_framefmt format = {
				.width = 256,		// a random number
				.height = 256,		// a random number
				.code = V4L2_MBUS_FMT_ARGB8888_1X32
			};

			if (v4l2_subdev_set_format(pads->compose_pad->entity, &format,
						   pads->compose_pad->index,
						   V4L2_SUBDEV_FORMAT_ACTIVE)) {
				weston_log("setting default failed.\n");
				goto error;
			}

			if (format.code != V4L2_MBUS_FMT_ARGB8888_1X32) {
				weston_log("couldn't set to ARGB.\n");
				goto error;
			}
		}
	}

	/* Initialize output */
	weston_log("Setting up an output.\n");

	/* setup links for output - always on */
	for (i = 0; i < (int)ARRAY_SIZE(vsp_output_links); i++) {
		snprintf(buf, sizeof(buf), vsp_output_links[i], device_name, device_name);
		weston_log("setting up link: '%s'\n", buf);
		link = media_parse_link(media, buf, &endp);
		if (media_setup_link(media, link->source, link->sink, 1)) {
			weston_log("link set up failed.\n");
			goto error;
		}
	}

	/* get pads for output */
	for (i = 0; i < (int)ARRAY_SIZE(vsp_output_fmt); i++) {
		snprintf(buf, sizeof(buf), vsp_output_fmt[i], device_name);
		weston_log("get an output pad: '%s'\n", buf);
		if (!(vsp->output.pads[i] = media_parse_pad(media, buf, NULL))) {
			weston_log("parse pad failed.\n");
			goto error;
		}
	}

	/* get a file descriptor for the output */
	snprintf(buf, sizeof(buf), vsp_output, device_name);
	entity = media_get_entity_by_name(media, buf, strlen(buf));
	if (!entity) {
		weston_log("error... '%s' not found.\n", buf);
		goto error;
	}

	devname = media_entity_get_devname(entity);
	weston_log("output '%s' is associated with '%s'\n", buf, devname);
	vsp->output_pad.fd = open(devname, O_RDWR);
	if (vsp->output_pad.fd < 0) {
		weston_log("error... can't open '%s'.\n", devname);
		goto error;
	}
	vsp2_check_capabiility(vsp->output_pad.fd, devname);

#ifdef V4L2_GL_FALLBACK
	if (scaler_type == SCALER_TYPE_VSP) {
		vsp->scaler = vsp2_scaler_init(section);
		if (!vsp->scaler)
			scaler_type = SCALER_TYPE_OFF;
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
	case V4L2_PIX_FMT_NV12M:
	case V4L2_PIX_FMT_NV21M:
	case V4L2_PIX_FMT_NV16M:
	case V4L2_PIX_FMT_NV61M:
	case V4L2_PIX_FMT_YUV420M:
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
	struct v4l2_format current_fmt;
	int ret;
	unsigned int original_pixelformat = fmt->fmt.pix_mp.pixelformat;

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
	int i;
	struct v4l2_mbus_framefmt format = { 0 };

	DBG("Setting output size to %dx%d\n", out->base.width, out->base.height);

	/* set WPF output size  */
	format.width  = out->base.width;
	format.height = out->base.height;
	format.code   = V4L2_MBUS_FMT_ARGB8888_1X32;	// TODO: does this have to be flexible?

	for (i = 0; i < (int)ARRAY_SIZE(vsp->output.pads); i++) {
		struct media_pad *pad = vsp->output.pads[i];
		if (v4l2_subdev_set_format(pad->entity, &format, pad->index, V4L2_SUBDEV_FORMAT_ACTIVE)) {
			weston_log("set sbudev format for failed at index %d.\n", i);
			return -1;
		}
	}

	return 0;
}

#ifdef V4L2_GL_FALLBACK
static int
vsp2_scaler_create_buffer(struct vsp_scaler_device *vspi, int fd,
			  struct kms_driver *kms, int width, int height)
{
	unsigned attr[] = {
		KMS_BO_TYPE, KMS_BO_TYPE_SCANOUT_X8R8G8B8,
		KMS_WIDTH, 0,
		KMS_HEIGHT, 0,
		KMS_TERMINATE_PROP_LIST
	};
	unsigned int handle, stride;
	struct vsp_surface_state *vs = &vspi->state;
	if (vspi->width >= width && vspi->height >= height)
		return 0;

	if (vspi->width < width)
		vspi->width = width;
	if (vspi->height < height)
		vspi->height = height;

	if (vs->base.planes[0].dmafd) {
		close(vs->base.planes[0].dmafd);
		kms_bo_destroy(&vs->base.bo);
	}

	attr[3] = ((vspi->width + 31) >> 5) << 5;
	attr[5] = vspi->height;

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
	if (vs->base.planes[0].dmafd)
		close(vs->base.planes[0].dmafd);
	kms_bo_destroy(&vs->base.bo);
	return -1;
}
#endif

static struct v4l2_renderer_output*
vsp2_create_output(struct v4l2_renderer_device *dev, int width, int height)
{
	struct vsp_device *vsp = (struct vsp_device*)dev;
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

	/* we use this later to let output to be input for composition */
	fmt = &outdev->surface_state.fmt;
	fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt->fmt.pix_mp.width = width;
	fmt->fmt.pix_mp.height = height;
	fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_ABGR32;
	fmt->fmt.pix_mp.num_planes = 1;

#ifdef V4L2_GL_FALLBACK
	if (scaler_type == SCALER_TYPE_VSP) {
		int ret = vsp2_scaler_create_buffer(vsp->scaler,
						    vsp->base.drm_fd,
						    vsp->base.kms,
						    width, height);
		if (ret) {
			weston_log("Can't create buffer for scaling. Disabling VSP scaler.\n");
			scaler_type = SCALER_TYPE_OFF;
		}
	}
#endif

	return (struct v4l2_renderer_output*)outdev;
}

static int
vsp2_dequeue_buffer(int fd, int capture)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];

	memset(&buf, 0, sizeof buf);
	buf.type = (capture) ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_DMABUF;
	buf.index = 0;
	buf.m.planes = planes;
	buf.length = 1;
	memset(planes, 0, sizeof(planes));

	if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
		weston_log("VIDIOC_DQBUF failed on %d (%s).\n", fd, strerror(errno));
		return -1;
	}

	return 0;
}

static int
vsp2_queue_buffer(int fd, int capture, struct vsp_surface_state *vs)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	int i;

	memset(&buf, 0, sizeof buf);
	buf.type = (capture) ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_DMABUF;
	buf.index = 0;
	buf.m.planes = planes;
	buf.length = vs->base.num_planes;
	memset(planes, 0, sizeof(planes));
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

	if (capture) {
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

static int
vsp2_request_buffer(int fd, int capture, int count)
{
	struct v4l2_requestbuffers reqbuf;

	memset(&reqbuf, 0, sizeof(reqbuf));
	reqbuf.type = (capture) ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	reqbuf.memory = V4L2_MEMORY_DMABUF;
	reqbuf.count = count;
	if (ioctl(fd, VIDIOC_REQBUFS, &reqbuf) == -1) {
		weston_log("clearing VIDIOC_REQBUFS failed (%s).\n", strerror(errno));
		return -1;
	}

	return 0;
}

static void
vsp2_comp_begin(struct v4l2_renderer_device *dev, struct v4l2_renderer_output *out)
{
	struct vsp_device *vsp = (struct vsp_device*)dev;
	struct vsp_renderer_output *output = (struct vsp_renderer_output*)out;
	struct v4l2_format *fmt = &output->surface_state.fmt;

	DBG("start vsp composition.\n");

	vsp->state = VSP_STATE_START;

	vsp2_set_output(vsp, output);

	// just in case
	vsp2_request_buffer(vsp->output_pad.fd, 1, 0);

	fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	vsp2_set_format(vsp->output_pad.fd, fmt, 0);
	fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	vsp->output_surface_state = &output->surface_state;

	vsp2_request_buffer(vsp->output_pad.fd, 1, 1);

	DBG("output set to dmabuf=%d\n", vsp->output_surface_state->base.planes[0].dmafd);
}

static int
vsp2_set_alpha(struct media_entity *entity, float alpha)
{
	struct v4l2_control ctrl;

	ctrl.id = V4L2_CID_ALPHA_COMPONENT;
	ctrl.value = (__s32)(alpha * 0xff);

	if (ioctl(entity->fd, VIDIOC_S_CTRL, &ctrl) == -1) {
		weston_log("failed to set alpha value (%d)\n", ctrl.value);
		return -1;
	}

	return 0;
}

static int
vsp2_comp_setup_inputs(struct vsp_device *vsp, struct vsp_input *input, int enable)
{
	struct v4l2_mbus_framefmt format = { 0 };
	struct vsp_media_pad *mpad = &input->input_pads;
	struct vsp_surface_state *vs = input->input_surface_states;
	struct v4l2_rect *src = &input->src;
	struct v4l2_rect *dst = &input->dst;

	// enable link associated with this pad
	if (media_setup_link(vsp->base.media, mpad->link->source, mpad->link->sink, enable)) {
		weston_log("enabling media link setup failed.\n");
		return -1;
	}

	if (!enable)
		return 0;

	// set pixel format and size
	format.width = vs->base.width;
	format.height = vs->base.height;
	format.code = vs->mbus_code;	// this is input format
	if (v4l2_subdev_set_format(mpad->infmt_pad->entity, &format, mpad->infmt_pad->index,
				   V4L2_SUBDEV_FORMAT_ACTIVE)) {
		weston_log("set input format via subdev failed.\n");
		return -1;
	}

	// set an alpha
	if (vsp2_set_alpha(mpad->input_entity, vs->base.alpha)) {
		weston_log("setting alpha (=%f) failed.", vs->base.alpha);
		return -1;
	}

	// set a crop paramters
	if (v4l2_subdev_set_selection(mpad->infmt_pad->entity, src, mpad->infmt_pad->index,
				      V4L2_SEL_TGT_CROP, V4L2_SUBDEV_FORMAT_ACTIVE)) {
		weston_log("set crop parameter failed: %dx%d@(%d,%d).\n",
			   src->width, src->height, src->left, src->top);
		return -1;
	}
	format.width = src->width;
	format.height = src->height;

	// this is an output towards BRU. this shall be consistent among all inputs.
	format.code = V4L2_MBUS_FMT_ARGB8888_1X32;
	if (v4l2_subdev_set_format(mpad->outfmt_pad->entity, &format, mpad->outfmt_pad->index,
				   V4L2_SUBDEV_FORMAT_ACTIVE)) {
		weston_log("set output format via subdev failed.\n");
		return -1;
	}

	// so does the BRU input
	if (v4l2_subdev_set_format(mpad->compose_pad->entity, &format, mpad->compose_pad->index,
				   V4L2_SUBDEV_FORMAT_ACTIVE)) {
		weston_log("set composition format via subdev failed.\n");
		return -1;
	}

	// set a composition paramters
	if (v4l2_subdev_set_selection(mpad->compose_pad->entity, dst, mpad->compose_pad->index,
				      V4L2_SEL_TGT_COMPOSE, V4L2_SUBDEV_FORMAT_ACTIVE)) {
		weston_log("set compose parameter failed: %dx%d@(%d,%d).\n",
			   dst->width, dst->height, dst->left, dst->top);
		return -1;
	}

	// just in case
	if (vsp2_request_buffer(mpad->fd, 0, 0) < 0)
		return -1;

	// set input format
	if (vsp2_set_format(mpad->fd, &vs->fmt, input->opaque))
		return -1;

	// request a buffer
	if (vsp2_request_buffer(mpad->fd, 0, 1) < 0)
		return -1;

	// queue buffer
	if (vsp2_queue_buffer(mpad->fd, 0, vs) < 0)
		return -1;

	return 0;
}

static int
vsp2_comp_flush(struct vsp_device *vsp)
{
	int i, fd;
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	DBG("flush vsp composition.\n");

	// enable links and queue buffer
	for (i = 0; i < vsp->input_count; i++)
		vsp2_comp_setup_inputs(vsp, &vsp->inputs[i], 1);

	// disable unused inputs
	for (i = vsp->input_count; i < vsp->input_max; i++)
		vsp2_comp_setup_inputs(vsp, &vsp->inputs[i], 0);

	// get an output pad
	fd = vsp->output_pad.fd;

	// queue buffer
	if (vsp2_queue_buffer(fd, 1, vsp->output_surface_state) < 0)
		goto error;

//	video_debug_mediactl();

	// stream on
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	for (i = 0; i < vsp->input_count; i++) {
		if (ioctl(vsp->inputs[i].input_pads.fd, VIDIOC_STREAMON, &type) == -1) {
			weston_log("VIDIOC_STREAMON failed for input %d. (%s)\n", i, strerror(errno));
		}
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(fd, VIDIOC_STREAMON, &type) == -1) {
		weston_log("VIDIOC_STREAMON failed for output (%s).\n", strerror(errno));
		goto error;
	}

	// dequeue buffer
	if (vsp2_dequeue_buffer(fd, 1) < 0)
		goto error;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(fd, VIDIOC_STREAMOFF, &type) == -1) {
		weston_log("%s: VIDIOC_STREAMOFF failed on %d (%s).\n", __func__, fd, strerror(errno));
		goto error;
	}

	// stream off
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	for (i = 0; i < vsp->input_count; i++) {
		if (ioctl(vsp->inputs[i].input_pads.fd, VIDIOC_STREAMOFF, &type) == -1) {
			weston_log("VIDIOC_STREAMOFF failed for input %d.\n", i);
		}
	}

	vsp->input_count = 0;
	return 0;

error:
	video_debug_mediactl();
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

#ifdef V4L2_GL_FALLBACK
static int
vsp2_do_scaling(struct vsp_scaler_device *vspi, struct vsp_input *input,
		struct v4l2_rect *src, struct v4l2_rect *dst)
{
	struct v4l2_mbus_framefmt format = { 0 };
	struct vsp_surface_state *vs = input->input_surface_states;
	struct vsp_surface_state *scaler_vs = &vspi->state;
	struct v4l2_format *fmt = &scaler_vs->fmt;
	int type;

	/* RPF input format */
	format.width = vs->base.width;
	format.height = vs->base.height;
	format.code = vs->mbus_code;
	if (v4l2_subdev_set_format(vspi->input.infmt->entity, &format,
				   vspi->input.infmt->index,
				   V4L2_SUBDEV_FORMAT_ACTIVE)) {
		weston_log("set input format via subdev failed.\n");
		return -1;
	}

	if (v4l2_subdev_set_selection(vspi->input.infmt->entity, src,
				      vspi->input.infmt->index,
				      V4L2_SEL_TGT_CROP,
				      V4L2_SUBDEV_FORMAT_ACTIVE)) {
		weston_log("set rcop parameter failed: %dx%d@(%d,%d).\n",
			   src->width, src->height, src->left, src->top);
		return -1;
	}

	/* RPF output format */
	format.width = src->width;
	format.height = src->height;
	format.code = V4L2_MBUS_FMT_ARGB8888_1X32;
	if (v4l2_subdev_set_format(vspi->input.outfmt->entity, &format,
				   vspi->input.outfmt->index,
				   V4L2_SUBDEV_FORMAT_ACTIVE)) {
		weston_log("set output format via subdev failed.\n");
		return -1;
	}

	/* UDS input format */
	if (v4l2_subdev_set_format(vspi->uds.infmt->entity, &format,
				   vspi->uds.infmt->index,
				   V4L2_SUBDEV_FORMAT_ACTIVE)) {
		weston_log("set input format of UDS via subdev failed.\n");
		return -1;
	}

	/* UDS output format */
	format.width = dst->width;
	format.height = dst->height;
	if (v4l2_subdev_set_format(vspi->uds.outfmt->entity, &format,
				   vspi->uds.outfmt->index,
				   V4L2_SUBDEV_FORMAT_ACTIVE)) {
		weston_log("set output format of UDS via subdev failed.\n");
		return -1;
	}

	/* WPF input format */
	if (v4l2_subdev_set_format(vspi->output_fmt->entity, &format,
				   vspi->output_fmt->index,
				   V4L2_SUBDEV_FORMAT_ACTIVE)) {
		weston_log("set input format of WPF via subdev failed.\n");
		return -1;
	}

	/* queue buffer for input */
	if (vsp2_request_buffer(vspi->input.fd, 0, 0) < 0)
		return -1;

	if (vsp2_set_format(vspi->input.fd, &vs->fmt, input->opaque))
		return -1;

	if (vsp2_request_buffer(vspi->input.fd, 0, 1) < 0)
		return -1;

	if (vsp2_queue_buffer(vspi->input.fd, 0, vs) < 0)
		return -1;

	/* queue buffer for output */
	vsp2_request_buffer(vspi->uds.fd, 1, 0);

	fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt->fmt.pix_mp.width = dst->width;
	fmt->fmt.pix_mp.height = dst->height;
	fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_ABGR32;
	fmt->fmt.pix_mp.num_planes = 1;
	vsp2_set_format(vspi->uds.fd, fmt, 0);
	fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	vsp2_request_buffer(vspi->uds.fd, 1, 1);

	scaler_vs->base.num_planes = 1;
	if (vsp2_queue_buffer(vspi->uds.fd, 1, scaler_vs) < 0)
		return -1;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (ioctl(vspi->input.fd, VIDIOC_STREAMON, &type) == -1) {
		weston_log("VIDIOC_STREAMON failed for scaler input. (%s)\n",
			   strerror(errno));
		return -1;
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(vspi->uds.fd, VIDIOC_STREAMON, &type) == -1) {
		weston_log("VIDIOC_STREAMON failed for scaler output. (%s)\n",
			   strerror(errno));
		return -1;
	}

	if (vsp2_dequeue_buffer(vspi->uds.fd, 1) < 0)
		return -1;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(vspi->uds.fd, VIDIOC_STREAMOFF, &type) == -1) {
		weston_log("VIDIOC_STREAMOFF failed for scaler output. (%s)\n",
			   strerror(errno));
	}

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (ioctl(vspi->input.fd, VIDIOC_STREAMOFF, &type) == -1) {
		weston_log("VIDIOC_STREAMOFF failed for scaler input. (%s)\n",
			   strerror(errno));
	}

	vsp2_request_buffer(vspi->input.fd, 0, 0);
	vsp2_request_buffer(vspi->uds.fd, 1, 0);

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
#ifdef V4L2_GL_FALLBACK
	int should_use_scaler = 0;
#endif
	struct vsp_input *input;

	if (src->width < 1 || src->height < 1) {
		DBG("ignoring the size of zeros < (%dx%d)\n", src->width, src->height);
		return 0;
	}

	if (src->width > 8190 || src->height > 8190) {
		weston_log("ignoring the size exceeding the limit (8190x8190) < (%dx%d)\n", src->width, src->height);
		return 0;
	}

#ifdef V4L2_GL_FALLBACK
	if (scaler_type == SCALER_TYPE_VSP &&
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
		src->left = 0;
	}

	if (src->top < 0) {
		src->height += src->top;
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

	input = &vsp->inputs[vsp->input_count];

	// get an available input pad
	input->input_surface_states = vs;
	input->src = *src;
	input->dst = *dst;
	input->opaque = opaque;

#ifdef V4L2_GL_FALLBACK
	/* check if we need to use a scaler */
	if (should_use_scaler) {
		DBG("We need to use a scaler. (%dx%d)->(%dx%d)\n",
		    src->width, src->height, dst->width, dst->height);

		// if all scalers are oocupied, flush and then retry.
                if (vsp->scaler_count == vsp->scaler_max) {
			vsp2_comp_flush(vsp);
			vsp->scaler_count = 0;
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

#ifdef V4L2_GL_FALLBACK
static int
vsp2_can_compose(struct v4l2_view *view_list, int count)
{
	return !(max_views_to_compose > 0 && max_views_to_compose < count);
}
#endif

static uint32_t
vsp2_get_capabilities(void)
{
	return 0;
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

#ifdef V4L2_GL_FALLBACK
	.can_compose = vsp2_can_compose,
#endif

	.get_capabilities = vsp2_get_capabilities,
};

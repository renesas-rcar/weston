/*
 * Copyright © 2014-2018 Renesas Electronics Corp.
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

#ifndef V4L2_RENDERER_DEVICE_H
#define V4L2_RENDERER_DEVICE_H

#include "config.h"

#include "compositor.h"

#include <errno.h>
#include <sys/ioctl.h>
#include <linux/media.h>

#ifdef V4L2_GL_FALLBACK_ENABLED
#include <libkms/libkms.h>
#endif

struct v4l2_renderer_device {
	int media_fd;
	const char *device_name;
	bool enable_composition_with_damage;
#if defined(V4L2_GL_FALLBACK_ENABLED) || defined(VSP2_SCALER_ENABLED)
	struct kms_driver *kms;
	int drm_fd;
#endif
};

struct v4l2_renderer_output {
	int width;
	int height;
};

struct v4l2_renderer_plane {
	int dmafd;
	unsigned int stride;
	unsigned int length;

	/* for shm buffer */
	struct kms_bo *bo;
	void *addr;
	int height;
};

#ifdef V4L2_GL_FALLBACK_ENABLED
typedef enum {
	V4L2_SURFACE_DEFAULT,
	V4L2_SURFACE_GL_ATTACHED
} v4l2_surface_t;

struct v4l2_view {
	struct weston_view *view;
	struct v4l2_surface_state *state;
};

typedef enum {
	V4L2_RENDERER_STATE_V4L2,
	V4L2_RENDERER_STATE_GL
} v4l2_renderer_state_t;
#endif

struct v4l2_surface_state {
	struct weston_surface *surface;
	struct weston_buffer_reference buffer_ref;

	struct v4l2_renderer *renderer;

	int num_planes;
	struct v4l2_renderer_plane planes[VIDEO_MAX_PLANES];

	float alpha;
	int width;
	int height;
	unsigned int pixel_format;
	bool multi_sample_pixels;
	bool in_expanded_damage;

	struct v4l2_rect src_rect;
	struct v4l2_rect dst_rect;

	struct v4l2_rect opaque_src_rect;
	struct v4l2_rect opaque_dst_rect;

	struct wl_listener surface_destroy_listener;
	struct wl_listener renderer_destroy_listener;

#ifdef V4L2_GL_FALLBACK_ENABLED
	void *gl_renderer_state;

	v4l2_surface_t surface_type;
	v4l2_renderer_state_t state_type;
	bool notify_attach;
	bool flush_damage;
	pixman_region32_t damage;

	struct wl_listener surface_post_destroy_listener;
	struct wl_listener renderer_post_destroy_listener;
#endif
};

struct v4l2_device_interface {
	struct v4l2_renderer_device *(*init)(int media_fd, struct media_device_info *info, struct v4l2_renderer_backend_config *config);

	struct v4l2_renderer_output *(*create_output)(struct v4l2_renderer_device *dev, int width, int height);
	void (*set_output_buffer)(struct v4l2_renderer_output *out, struct v4l2_bo_state *bo);

	struct v4l2_surface_state *(*create_surface)(struct v4l2_renderer_device *dev);
	int (*attach_buffer)(struct v4l2_surface_state *vs);

	bool (*begin_compose)(struct v4l2_renderer_device *dev, struct v4l2_renderer_output *out);
	void (*finish_compose)(struct v4l2_renderer_device *dev);
	int (*draw_view)(struct v4l2_renderer_device *dev, struct v4l2_surface_state *vs);
#ifdef V4L2_GL_FALLBACK_ENABLED
	int (*can_compose)(struct v4l2_renderer_device *dev, struct v4l2_view *view_list, int count);
#endif

	uint32_t (*get_capabilities)(void);
	bool (*check_format)(uint32_t color_format, int num_planes);
};

#endif /* !V4L2_RENDERER_DEVICE_H */

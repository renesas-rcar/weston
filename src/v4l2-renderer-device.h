/*
 * Copyright © 2014 Renesas Electronics Corp.
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

#include "compositor.h"

/*
 * Enable gl-fallback feature.
 */
#define V4L2_GL_FALLBACK

#ifdef V4L2_GL_FALLBACK
#include <libkms/libkms.h>
#endif

struct v4l2_renderer_device {
	struct media_device *media;
	const char *device_name;
#ifdef V4L2_GL_FALLBACK
	struct kms_driver *kms;
	int drm_fd;
	bool disable_gl_fallback;
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
	unsigned int bytesused;
};

#ifdef V4L2_GL_FALLBACK
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

	struct kms_bo *bo;
	void *addr;
	int bpp;
	int bo_stride;

	int num_planes;
	struct v4l2_renderer_plane planes[VIDEO_MAX_PLANES];

	float alpha;
	int width;
	int height;
	unsigned int pixel_format;

	struct v4l2_rect src_rect;
	struct v4l2_rect dst_rect;

	struct v4l2_rect opaque_src_rect;
	struct v4l2_rect opaque_dst_rect;

	struct wl_listener buffer_destroy_listener;
	struct wl_listener surface_destroy_listener;
	struct wl_listener renderer_destroy_listener;
	struct wl_listener dmabuf_buffer_destroy_listener;

#ifdef V4L2_GL_FALLBACK
	void *gl_renderer_state;

	v4l2_surface_t surface_type;
	v4l2_renderer_state_t state_type;
	int notify_attach;
	int flush_damage;
	pixman_region32_t damage;

	struct wl_listener surface_post_destroy_listener;
	struct wl_listener renderer_post_destroy_listener;
#endif
};

struct v4l2_device_interface {
	struct v4l2_renderer_device *(*init)(struct media_device *media, struct weston_config *config);

	struct v4l2_renderer_output *(*create_output)(struct v4l2_renderer_device *dev, int width, int height);
	void (*set_output_buffer)(struct v4l2_renderer_output *out, struct v4l2_bo_state *bo);

	struct v4l2_surface_state *(*create_surface)(struct v4l2_renderer_device *dev);
	int (*attach_buffer)(struct v4l2_surface_state *vs);

	void (*begin_compose)(struct v4l2_renderer_device *dev, struct v4l2_renderer_output *out);
	void (*finish_compose)(struct v4l2_renderer_device *dev);
	int (*draw_view)(struct v4l2_renderer_device *dev, struct v4l2_surface_state *vs);
#ifdef V4L2_GL_FALLBACK
	int (*can_compose)(struct v4l2_view *view_list, int count);
#endif

	uint32_t (*get_capabilities)(void);
};

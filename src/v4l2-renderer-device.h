/*
 * Copyright © 2014 Renesas Electronics Corp.
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

#include "compositor.h"

/*
 * Enable gl-fallback feature.
 */
#define V4L2_GL_FALLBACK

struct v4l2_renderer_device {
	struct media_device *media;
	const char *device_name;
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

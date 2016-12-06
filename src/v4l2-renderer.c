/*
 * Copyright © 2014 Renesas Electronics Corp.
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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include <linux/media.h>
#include "v4l2-renderer.h"
#include "v4l2-renderer-device.h"

#include <xf86drm.h>
#include <libkms/libkms.h>
#include <drm_fourcc.h>

#include <wayland-kms.h>
#include <wayland-kms-server-protocol.h>
#include "linux-dmabuf.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "shared/helpers.h"

#ifdef V4L2_GL_FALLBACK_ENABLED
#include <dlfcn.h>
#include <gbm.h>
#include <gbm_kmsint.h>
#include "gl-renderer.h"
#endif

#include <linux/input.h>

#if 0
#define DBG(...) weston_log(__VA_ARGS__)
#define DBGC(...) weston_log_continue(__VA_ARGS__)
#define DEBUG
#else
#define DBG(...) do {} while (0)
#define DBGC(...) do {} while (0)
#ifdef DEBUG
#  undef DEBUG
#endif
#endif

struct v4l2_output_state {
	struct v4l2_renderer_output *output;
	uint32_t stride;
	void *map;
	struct v4l2_bo_state *bo;
	int bo_count;
	int bo_index;
#ifdef V4L2_GL_FALLBACK_ENABLED
	void *gl_renderer_state;
	struct gbm_surface *gbm_surface;
#endif
};

struct v4l2_renderer {
	struct weston_renderer base;

	struct kms_driver *kms;
	struct wl_kms *wl_kms;

	char *device_name;
	int drm_fd;
	int media_fd;

	struct v4l2_renderer_device *device;

	int repaint_debug;
	struct weston_binding *debug_binding;

	struct wl_signal destroy_signal;

#ifdef V4L2_GL_FALLBACK_ENABLED
	int gl_fallback;
	int defer_attach;
	struct gbm_device *gbm;
	struct weston_renderer *gl_renderer;
#endif
};

static struct v4l2_device_interface *device_interface = NULL;
#ifdef V4L2_GL_FALLBACK_ENABLED
static struct gl_renderer_interface *gl_renderer;
#endif

static inline struct v4l2_output_state *
get_output_state(struct weston_output *output)
{
	return (struct v4l2_output_state *)output->renderer_state;
}

static int
v4l2_renderer_create_surface(struct weston_surface *surface);

static inline struct v4l2_surface_state *
get_surface_state(struct weston_surface *surface)
{
	if (!surface->renderer_state)
		v4l2_renderer_create_surface(surface);

	return (struct v4l2_surface_state *)surface->renderer_state;
}

static inline struct v4l2_renderer *
get_renderer(struct weston_compositor *ec)
{
	return (struct v4l2_renderer *)ec->renderer;
}

#ifdef V4L2_GL_FALLBACK_ENABLED
static struct gbm_device *
v4l2_create_gbm_device(int fd)
{
	struct gbm_device *gbm;

	gl_renderer = weston_load_module("gl-renderer.so",
					 "gl_renderer_interface");
	if (!gl_renderer)
		return NULL;

	/* GBM will load a dri driver, but even though they need symbols from
	 * libglapi, in some version of Mesa they are not linked to it. Since
	 * only the gl-renderer module links to it, the call above won't make
	 * these symbols globally available, and loading the DRI driver fails.
	 * Workaround this by dlopen()'ing libglapi with RTLD_GLOBAL. */
	dlopen("libglapi.so.0", RTLD_LAZY | RTLD_GLOBAL);

	gbm = gbm_create_device(fd);

	return gbm;
}

static void
v4l2_destroy_gbm_device(struct gbm_device *gbm)
{
	if (gbm)
		gbm_device_destroy(gbm);
}

static int
v4l2_create_gl_renderer(struct weston_compositor *ec, struct v4l2_renderer *renderer)
{
	EGLint format = GBM_FORMAT_XRGB8888;

	if (gl_renderer->create(ec, EGL_PLATFORM_GBM_KHR, renderer->gbm,
				gl_renderer->opaque_attribs, &format, 1) < 0) {
		return -1;
	}
	renderer->gl_renderer = ec->renderer;

	return 0;
}

static int
v4l2_init_gl_output(struct weston_output *output, struct v4l2_renderer *renderer)
{
	EGLint format = GBM_FORMAT_XRGB8888;
	struct v4l2_output_state *state = get_output_state(output);
	int i;
	pixman_format_code_t read_format;

	state->gbm_surface = gbm_surface_create(renderer->gbm,
						output->current_mode->width,
						output->current_mode->height,
						format,
						GBM_BO_USE_SCANOUT |
						GBM_BO_USE_RENDERING |
						GBM_BO_CREATE_EMPTY);

	if (!state->gbm_surface) {
		weston_log("%s: failed to create gbm surface\n", __func__);
		return -1;
	}

	for (i = 0; i < 2; i++) {
		int n = i % state->bo_count;
		gbm_kms_set_bo((struct gbm_kms_surface *)state->gbm_surface,
			       n, state->bo[n].map, state->bo[n].stride);
	}

	output->compositor->renderer = renderer->gl_renderer;
	output->renderer_state = NULL;
	read_format = output->compositor->read_format;
	if (gl_renderer->output_create(output,
				       (EGLNativeDisplayType)state->gbm_surface,
				       state->gbm_surface,
				       gl_renderer->opaque_attribs,
				       &format, 1) < 0) {
		weston_log("%s: failed to create gl renderer output state\n", __func__);
		gbm_surface_destroy(state->gbm_surface);
		return -1;
	}
	output->compositor->read_format = read_format;
	state->gl_renderer_state = output->renderer_state;
	output->renderer_state = state;
	output->compositor->renderer = &renderer->base;

	return 0;
}

static void
v4l2_gl_output_destroy(struct weston_output *output,
		       struct v4l2_renderer *renderer)
{
	struct v4l2_output_state *state = get_output_state(output);
	output->compositor->renderer = renderer->gl_renderer;
	output->renderer_state = state->gl_renderer_state;
	gl_renderer->output_destroy(output);
	output->renderer_state = state;
	output->compositor->renderer = &renderer->base;
}

static void
v4l2_gl_flush_damage(struct weston_surface *surface)
{
	struct v4l2_surface_state *vs = get_surface_state(surface);
	struct v4l2_renderer *renderer = vs->renderer;

	surface->compositor->renderer = renderer->gl_renderer;
	surface->renderer_state = vs->gl_renderer_state;

	renderer->gl_renderer->flush_damage(surface);

	vs->gl_renderer_state = surface->renderer_state;
	surface->renderer_state = vs;
	surface->compositor->renderer = &renderer->base;
}

static void
v4l2_gl_surface_cleanup(struct v4l2_surface_state *vs)
{
	struct v4l2_renderer *renderer = vs->renderer;

	wl_list_remove(&vs->surface_post_destroy_listener.link);
	wl_list_remove(&vs->renderer_post_destroy_listener.link);

	vs->surface->compositor->renderer = &vs->renderer->base;
	vs->surface->renderer_state = NULL;

	if (renderer->defer_attach)
		pixman_region32_fini(&vs->damage);

	free(vs);
}

static void
v4l2_gl_surface_post_destroy(struct wl_listener *listener, void *data)
{
	struct v4l2_surface_state *vs;
	vs = container_of(listener, struct v4l2_surface_state,
			  surface_post_destroy_listener);
	v4l2_gl_surface_cleanup(vs);
}

static void
v4l2_gl_renderer_post_destroy(struct wl_listener *listener, void *data)
{
	struct v4l2_surface_state *vs;
	vs = container_of(listener, struct v4l2_surface_state,
			  renderer_post_destroy_listener);
	v4l2_gl_surface_cleanup(vs);
}

static void
v4l2_gl_attach(struct weston_surface *surface, struct weston_buffer *buffer)
{
	struct v4l2_surface_state *vs = get_surface_state(surface);
	struct v4l2_renderer *renderer = vs->renderer;

	surface->compositor->renderer = renderer->gl_renderer;
	surface->renderer_state = vs->gl_renderer_state;

	renderer->gl_renderer->attach(surface, buffer);

	vs->gl_renderer_state = surface->renderer_state;
	surface->renderer_state = vs;
	surface->compositor->renderer = &renderer->base;

	if ((buffer) && (vs->surface_type != V4L2_SURFACE_GL_ATTACHED)) {
		vs->surface_post_destroy_listener.notify = v4l2_gl_surface_post_destroy;
		wl_signal_add(&surface->destroy_signal, &vs->surface_post_destroy_listener);

		vs->renderer_post_destroy_listener.notify = v4l2_gl_renderer_post_destroy;
		wl_signal_add(&renderer->destroy_signal, &vs->renderer_post_destroy_listener);

		vs->surface_type = V4L2_SURFACE_GL_ATTACHED;
	}
}

struct stack {
	int stack_size;
	int element_size;
	void *stack;
};

#define V4L2_STACK_INIT(size) { .stack_size = 0, .element_size = (size), .stack = NULL }

static int
v4l2_stack_realloc(struct stack *stack, int target_size)
{
	if (target_size > stack->stack_size)
		target_size += 8;
	else if (target_size < stack->stack_size / 2)
		target_size = stack->stack_size / 2 + 1;
	else
		target_size = 0;

	if (target_size) {
		stack->stack = realloc(stack->stack, target_size * stack->element_size);

		if (!stack->stack) {
			weston_log("can't allocate memory for a stack. can't continue.\n");
			return -1;
		}

		stack->stack_size = target_size;
	}

	return 0;
}

static void
v4l2_gl_repaint(struct weston_output *output,
		pixman_region32_t *output_damage)
{
	struct weston_compositor *ec = output->compositor;
	struct v4l2_renderer *renderer = get_renderer(ec);
	struct v4l2_output_state *state = get_output_state(output);
	struct weston_view *ev;
	int view_count, i;
	static struct stack stacker = V4L2_STACK_INIT(sizeof(void *));
	void **stack;

	if (v4l2_stack_realloc(&stacker, wl_list_length(&ec->view_list)) < 0)
		return;
	stack = (void**)stacker.stack;

	view_count = 0;
	wl_list_for_each(ev, &ec->view_list, link) {
		struct v4l2_surface_state *vs = get_surface_state(ev->surface);

		if (renderer->defer_attach) {
			if (vs->notify_attach == 1) {
				DBG("%s: attach gl\n", __func__);
				v4l2_gl_attach(ev->surface, vs->buffer_ref.buffer);
				vs->notify_attach = 0;
			}
			if (vs->flush_damage) {
				DBG("%s: flush damage\n", __func__);
				pixman_region32_copy(&ev->surface->damage, &vs->damage);
				v4l2_gl_flush_damage(ev->surface);
				vs->flush_damage = 0;
				pixman_region32_clear(&ev->surface->damage);
			}
		}

		stack[view_count++] = vs;
	}

	for (i = 0; i < view_count; i++) {
		struct v4l2_surface_state *vs = stack[i];
		if (vs->state_type == V4L2_RENDERER_STATE_V4L2) {
			vs->surface->renderer_state = vs->gl_renderer_state;
			vs->state_type = V4L2_RENDERER_STATE_GL;
		}
	}

	ec->renderer = renderer->gl_renderer;
	output->renderer_state = state->gl_renderer_state;
	renderer->gl_renderer->repaint_output(output, output_damage);
	ec->renderer = &renderer->base;
	output->renderer_state = state;

	view_count = 0;
	wl_list_for_each(ev, &ec->view_list, link) {
		struct v4l2_surface_state *vs = stack[view_count++];
		if (vs->state_type == V4L2_RENDERER_STATE_GL) {
			ev->surface->renderer_state = vs;
			vs->state_type = V4L2_RENDERER_STATE_V4L2;
		}
	}
}

static bool
v4l2_gl_import_dmabuf(struct weston_compositor *ec,
		      struct linux_dmabuf_buffer *dmabuf)
{
	struct v4l2_renderer *renderer = get_renderer(ec);
	bool ret;

	ec->renderer = renderer->gl_renderer;
	ret = renderer->gl_renderer->import_dmabuf(ec, dmabuf);
	ec->renderer = &renderer->base;

	return ret;
}

#endif

static int
v4l2_renderer_read_pixels(struct weston_output *output,
			 pixman_format_code_t format, void *pixels,
			 uint32_t x, uint32_t y,
			 uint32_t width, uint32_t height)
{
	struct v4l2_output_state *vo = get_output_state(output);
	struct v4l2_bo_state *bo = &vo->bo[vo->bo_index];
	uint32_t v, len = width * 4;
	void *src, *dst;

	switch(format) {
	case PIXMAN_a8r8g8b8:
		break;
	default:
		return -1;
	}

#ifdef V4L2_GL_FALLBACK_ENABLED
	if (output->compositor->capabilities & WESTON_CAP_CAPTURE_YFLIP) {
		src = bo->map + x * 4 + (output->height - (y + height)) * bo->stride;
		dst = pixels + len * (height - 1);
		for (v = 0; v < height; v++) {
			memcpy(dst, src, len);
			src += bo->stride;
			dst -= len;
		}
		return 0;
	}
#endif

	if (x == 0 && y == 0 &&
	    width == (uint32_t)output->current_mode->width &&
	    height == (uint32_t)output->current_mode->height &&
	    bo->stride == len) {
		DBG("%s: copy entire buffer at once\n", __func__);
		// TODO: we may want to optimize this using underlying
		// V4L2 MC hardware if possible.
		memcpy(pixels, bo->map, bo->stride * height);
		return 0;
	}

	src = bo->map + x * 4 + y * bo->stride;
	dst = pixels;
	for (v = 0; v < height; v++) {
		memcpy(dst, src, len);
		src += bo->stride;
		dst += len;
	}

	return 0;
}

static void
region_global_to_output(struct weston_output *output, pixman_region32_t *region)
{
	pixman_region32_translate(region, -output->x, -output->y);
	weston_transformed_region(output->width, output->height,
				  output->transform, output->current_scale,
				  region, region);
}

#define D2F(v) pixman_double_to_fixed((double)v)
#define F2D(v) pixman_fixed_to_double(v)
#define F2I(v) pixman_fixed_to_int(v)


static void
transform_apply_viewport(pixman_transform_t *transform,
			 struct weston_surface *surface)
{
	struct weston_buffer_viewport *vp = &surface->buffer_viewport;
	double src_width, src_height;
	double src_x, src_y;

	if (vp->buffer.src_width == wl_fixed_from_int(-1)) {
		if (vp->surface.width == -1)
			return;

		src_x = 0.0;
		src_y = 0.0;
		src_width = surface->width_from_buffer;
		src_height = surface->height_from_buffer;
	} else {
		src_x = wl_fixed_to_double(vp->buffer.src_x);
		src_y = wl_fixed_to_double(vp->buffer.src_y);
		src_width = wl_fixed_to_double(vp->buffer.src_width);
		src_height = wl_fixed_to_double(vp->buffer.src_height);
	}

	pixman_transform_scale(transform, NULL,
			       D2F(src_width / surface->width),
			       D2F(src_height / surface->height));
	pixman_transform_translate(transform, NULL, D2F(src_x), D2F(src_y));
}

static void
transform_region(pixman_transform_t *transform, pixman_region32_t *src_region,
		 pixman_region32_t *dst_region)
{
	pixman_box32_t *bbox;
	pixman_vector_t q1, q2;

	pixman_region32_init(dst_region);
	if (!pixman_region32_not_empty(src_region))
		return;

	bbox = pixman_region32_extents(src_region);
	q1.vector[0] = pixman_int_to_fixed(bbox->x1);
	q1.vector[1] = pixman_int_to_fixed(bbox->y1);
	q1.vector[2] = pixman_int_to_fixed(1);

	q2.vector[0] = pixman_int_to_fixed(bbox->x2);
	q2.vector[1] = pixman_int_to_fixed(bbox->y2);
	q2.vector[2] = pixman_int_to_fixed(1);

	DBG("bbox: (%d,%d)-(%d,%d)\n", bbox->x1, bbox->y1, bbox->x2, bbox->y2);
	DBG("q1: (%f,%f,%f)\n", F2D(q1.vector[0]), F2D(q1.vector[1]), F2D(q1.vector[2]));
	DBG("q2: (%f,%f,%f)\n", F2D(q2.vector[0]), F2D(q2.vector[1]), F2D(q2.vector[2]));

	DBG("transform: (%f,%f,%f)(%f,%f,%f)(%f,%f,%f)\n",
	    F2D(transform->matrix[0][0]), F2D(transform->matrix[1][0]), F2D(transform->matrix[2][0]),
	    F2D(transform->matrix[0][1]), F2D(transform->matrix[1][1]), F2D(transform->matrix[2][1]),
	    F2D(transform->matrix[0][2]), F2D(transform->matrix[1][2]), F2D(transform->matrix[2][2])
	);

	pixman_transform_point(transform, &q1);
	pixman_transform_point(transform, &q2);

	DBG("q1': (%f,%f,%f)\n", F2D(q1.vector[0]), F2D(q1.vector[1]), F2D(q1.vector[2]));
	DBG("q2': (%f,%f,%f)\n", F2D(q2.vector[0]), F2D(q2.vector[1]), F2D(q2.vector[2]));

	pixman_region32_init_rect(dst_region,
				  F2I((q1.vector[0] < q2.vector[0]) ? q1.vector[0] : q2.vector[0]),
				  F2I((q1.vector[1] < q2.vector[1]) ? q1.vector[1] : q2.vector[1]),
				  abs(F2I(pixman_fixed_ceil(q2.vector[0] - q1.vector[0]))),
				  abs(F2I(pixman_fixed_ceil(q2.vector[1] - q1.vector[1]))));
}

static void
calculate_transform_matrix(struct weston_view *ev, struct weston_output *output,
			   pixman_transform_t *transform)
{
	struct weston_buffer_viewport *vp = &ev->surface->buffer_viewport;
	pixman_fixed_t fw, fh;

	/* Set up the source transformation based on the surface
	   position, the output position/transform/scale and the client
	   specified buffer transform/scale */
	pixman_transform_init_identity(transform);
	pixman_transform_scale(transform, NULL,
			       pixman_double_to_fixed((double)1.0 / output->current_scale),
			       pixman_double_to_fixed((double)1.0 / output->current_scale));

	fw = pixman_int_to_fixed(output->width);
	fh = pixman_int_to_fixed(output->height);
	switch (output->transform) {
	default:
	case WL_OUTPUT_TRANSFORM_NORMAL:
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		break;
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		pixman_transform_rotate(transform, NULL, 0, -pixman_fixed_1);
		pixman_transform_translate(transform, NULL, 0, fh);
		break;
	case WL_OUTPUT_TRANSFORM_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		pixman_transform_rotate(transform, NULL, -pixman_fixed_1, 0);
		pixman_transform_translate(transform, NULL, fw, fh);
		break;
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		pixman_transform_rotate(transform, NULL, 0, pixman_fixed_1);
		pixman_transform_translate(transform, NULL, fw, 0);
		break;
	}

	switch (output->transform) {
	case WL_OUTPUT_TRANSFORM_FLIPPED:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		pixman_transform_scale(transform, NULL,
				       pixman_int_to_fixed(-1),
				       pixman_int_to_fixed(1));
		pixman_transform_translate(transform, NULL, fw, 0);
		break;
	}

        pixman_transform_translate(transform, NULL,
				   pixman_double_to_fixed(output->x),
				   pixman_double_to_fixed(output->y));

	if (ev->transform.enabled) {
		/* Pixman supports only 2D transform matrix, but Weston uses 3D,
		 * so we're omitting Z coordinate here
		 */
		pixman_transform_t surface_transform = {{
				{ D2F(ev->transform.matrix.d[0]),
				  D2F(ev->transform.matrix.d[4]),
				  D2F(ev->transform.matrix.d[12]),
				},
				{ D2F(ev->transform.matrix.d[1]),
				  D2F(ev->transform.matrix.d[5]),
				  D2F(ev->transform.matrix.d[13]),
				},
				{ D2F(ev->transform.matrix.d[3]),
				  D2F(ev->transform.matrix.d[7]),
				  D2F(ev->transform.matrix.d[15]),
				}
			}};

		pixman_transform_invert(&surface_transform, &surface_transform);
		pixman_transform_multiply(transform, &surface_transform, transform);
	} else {
		pixman_transform_translate(transform, NULL,
					   pixman_double_to_fixed((double)-ev->geometry.x),
					   pixman_double_to_fixed((double)-ev->geometry.y));
	}

	transform_apply_viewport(transform, ev->surface);

	fw = pixman_int_to_fixed(ev->surface->width_from_buffer);
	fh = pixman_int_to_fixed(ev->surface->height_from_buffer);

	switch (vp->buffer.transform) {
	case WL_OUTPUT_TRANSFORM_FLIPPED:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		pixman_transform_scale(transform, NULL,
				       pixman_int_to_fixed(-1),
				       pixman_int_to_fixed(1));
		pixman_transform_translate(transform, NULL, fw, 0);
		break;
	}

	switch (vp->buffer.transform) {
	default:
	case WL_OUTPUT_TRANSFORM_NORMAL:
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		break;
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		pixman_transform_rotate(transform, NULL, 0, pixman_fixed_1);
		pixman_transform_translate(transform, NULL, fh, 0);
		break;
	case WL_OUTPUT_TRANSFORM_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		pixman_transform_rotate(transform, NULL, -pixman_fixed_1, 0);
		pixman_transform_translate(transform, NULL, fw, fh);
		break;
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		pixman_transform_rotate(transform, NULL, 0, -pixman_fixed_1);
		pixman_transform_translate(transform, NULL, 0, fw);
		break;
	}

	pixman_transform_scale(transform, NULL,
			       pixman_double_to_fixed(vp->buffer.scale),
			       pixman_double_to_fixed(vp->buffer.scale));

}

static void
set_v4l2_rect(pixman_region32_t *region, struct v4l2_rect *rect)
{
	pixman_box32_t *bbox;

	bbox = pixman_region32_extents(region);
	rect->left   = bbox->x1;
	rect->top    = bbox->y1;
	rect->width  = bbox->x2 - bbox->x1;
	rect->height = bbox->y2 - bbox->y1;
}

static void
draw_view(struct weston_view *ev, struct weston_output *output)
{
	struct v4l2_renderer *renderer = (struct v4l2_renderer*)output->compositor->renderer;
	struct v4l2_surface_state *vs = get_surface_state(ev->surface);
	pixman_region32_t dst_region, src_region;
	pixman_region32_t region, opaque_src_region, opaque_dst_region;
	pixman_transform_t transform;

	/* a surface in the repaint area? */
	pixman_region32_init(&region);
	pixman_region32_intersect(&region,
				  &ev->transform.boundingbox,
				  &output->region);
	pixman_region32_subtract(&region, &region, &ev->clip);
	if (!pixman_region32_not_empty(&region)) {
		DBG("%s: skipping a view: not visible: view=(%d,%d)-(%d,%d), repaint=(%d,%d)-(%d,%d)\n",
		    __func__,
		    ev->transform.boundingbox.extents.x1, ev->transform.boundingbox.extents.y1,
		    ev->transform.boundingbox.extents.x2, ev->transform.boundingbox.extents.y2,
		    output->region.extents.x1, output->region.extents.y1,
		    output->region.extents.x2, output->region.extents.y2);
		goto out;
	}

	/* you may sometime get not-yet-attached views */
	if (vs->planes[0].dmafd == 0)
		goto out;

	/*
	 * Check if the surface is still valid. OpenGL/ES apps may destroy
	 * buffers before they destroy a surface. This check works in the
	 * serialized world only.
	 */
	if (fcntl(vs->planes[0].dmafd, F_GETFD) < 0)
		goto out;

	if (output->zoom.active) {
		weston_log("v4l2 renderer does not support zoom\n");
		goto out;
	}

	/* we have to compute a transform matrix */
	calculate_transform_matrix(ev, output, &transform);

	pixman_region32_init(&opaque_src_region);
	pixman_region32_init(&opaque_dst_region);

	if (pixman_region32_not_empty(&ev->surface->opaque)) {
		pixman_region32_t output_region;
		pixman_transform_t inverse;

		pixman_transform_invert(&inverse, &transform);
		transform_region(&inverse, &ev->surface->opaque, &opaque_dst_region);

		pixman_region32_init_rect(&output_region, 0, 0, output->width, output->height);

		pixman_region32_intersect(&opaque_dst_region, &opaque_dst_region, &output_region);
		pixman_region32_subtract(&opaque_dst_region, &opaque_dst_region, &ev->clip);
		transform_region(&transform, &opaque_dst_region, &opaque_src_region);
	}

	/* find out the final destination in the output coordinate */
	pixman_region32_init(&dst_region);
	pixman_region32_copy(&dst_region, &region);
	region_global_to_output(output, &dst_region);

	transform_region(&transform, &dst_region, &src_region);
	set_v4l2_rect(&dst_region, &vs->dst_rect);
	set_v4l2_rect(&src_region, &vs->src_rect);

	/* setup opaque region */
	set_v4l2_rect(&opaque_dst_region, &vs->opaque_dst_rect);
	set_v4l2_rect(&opaque_src_region, &vs->opaque_src_rect);

	vs->alpha = ev->alpha;

	DBG("monitor: %dx%d@(%d,%d)\n", output->width, output->height, output->x, output->y);
	DBG("composing from %dx%d@(%d,%d) to %dx%d@(%d,%d)\n",
	    vs->src_rect.width, vs->src_rect.height, vs->src_rect.left, vs->src_rect.top,
	    vs->dst_rect.width, vs->dst_rect.height, vs->dst_rect.left, vs->dst_rect.top);
	DBG("composing from %dx%d@(%d,%d) to %dx%d@(%d,%d) [opaque region]\n",
	    vs->opaque_src_rect.width, vs->opaque_src_rect.height, vs->opaque_src_rect.left, vs->opaque_src_rect.top,
	    vs->opaque_dst_rect.width, vs->opaque_dst_rect.height, vs->opaque_dst_rect.left, vs->opaque_dst_rect.top);

	device_interface->draw_view(renderer->device, vs);

	pixman_region32_fini(&dst_region);
	pixman_region32_fini(&src_region);
	pixman_region32_fini(&opaque_src_region);
	pixman_region32_fini(&opaque_dst_region);
out:
	pixman_region32_fini(&region);
}

static void
repaint_surfaces(struct weston_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *compositor = output->compositor;
	struct v4l2_output_state *vo = get_output_state(output);
	struct v4l2_renderer *renderer = (struct v4l2_renderer*)compositor->renderer;
	struct weston_view *view;

	device_interface->begin_compose(renderer->device, vo->output);

	wl_list_for_each_reverse(view, &compositor->view_list, link) {
		if (view->plane == &compositor->primary_plane)
			draw_view(view, output);
	}

	device_interface->finish_compose(renderer->device);
}

#ifdef V4L2_GL_FALLBACK_ENABLED
static int
can_repaint(struct weston_compositor *c, pixman_region32_t *output_region)
{
	struct weston_view *ev;
	pixman_region32_t region;
	int need_repaint, view_count;
	static struct stack stacker = V4L2_STACK_INIT(sizeof(struct v4l2_view));
	struct v4l2_view *view_list;
	struct v4l2_renderer *vr = get_renderer(c);

	DBG("%s: checking...\n", __func__);

	/* we don't bother checking, if can_compose is not defined */
	if (!device_interface->can_compose)
		return 1;

	/* if stack realloc fails, there's not many things we can do... */
	if (v4l2_stack_realloc(&stacker, wl_list_length(&c->view_list)) < 0)
		return 1;
	view_list = (struct v4l2_view *)stacker.stack;

	view_count = 0;
	wl_list_for_each(ev, &c->view_list, link) {
		/* in the primary plane? */
		if (ev->plane != &c->primary_plane)
			continue;

		/* a surface in the repaint area? */
		pixman_region32_init(&region);
		pixman_region32_intersect(&region,
					  &ev->transform.boundingbox,
					  output_region);
		pixman_region32_subtract(&region, &region, &ev->clip);
		need_repaint = pixman_region32_not_empty(&region);
		pixman_region32_fini(&region);

		if (need_repaint) {
			struct v4l2_surface_state *vs = get_surface_state(ev->surface);
			view_list[view_count].view = ev;
			view_list[view_count].state = vs;
			view_count++;
		}
	}

	return device_interface->can_compose(vr->device, view_list, view_count);
}
#endif

static void
v4l2_renderer_repaint_output(struct weston_output *output,
			    pixman_region32_t *output_damage)
{
	DBG("%s\n", __func__);

#ifdef V4L2_GL_FALLBACK_ENABLED
	struct v4l2_renderer *renderer = (struct v4l2_renderer*)output->compositor->renderer;

	if ((renderer->gl_fallback) && (!can_repaint(output->compositor, &output->region))) {
		struct v4l2_output_state *vo = get_output_state(output);
		gbm_kms_set_front((struct gbm_kms_surface *)vo->gbm_surface, (!vo->bo_index));
		v4l2_gl_repaint(output, output_damage);
		return;
	}
#endif

	// render all views
	repaint_surfaces(output, output_damage);

	// remember the damaged area
	pixman_region32_copy(&output->previous_damage, output_damage);

	// emits signal
	wl_signal_emit(&output->frame_signal, output);

	/* Actual flip should be done by caller */
}

static inline void
v4l2_renderer_copy_buffer(struct v4l2_surface_state *vs, struct weston_buffer *buffer)
{
	void *src, *dst;
	int y, stride, bo_stride;

	src = wl_shm_buffer_get_data(buffer->shm_buffer);
	dst = vs->addr;

	stride = vs->planes[0].stride;
	bo_stride = vs->bo_stride;

	wl_shm_buffer_begin_access(buffer->shm_buffer);
	for (y = 0; y < buffer->height; y++) {
		memcpy(dst, src, buffer->width * vs->bpp);
		dst += bo_stride;
		src += stride;
	}
	wl_shm_buffer_end_access(buffer->shm_buffer);

}

static void
v4l2_renderer_flush_damage(struct weston_surface *surface)
{
	struct v4l2_surface_state *vs = get_surface_state(surface);
	struct weston_buffer *buffer = vs->buffer_ref.buffer;

	DBG("%s: flushing damage..\n", __func__);

	v4l2_renderer_copy_buffer(vs, buffer);

	/*
	 * TODO: We may consider use of surface->damage to
	 * optimize updates.
	 */

#ifdef V4L2_GL_FALLBACK_ENABLED
	if (vs->renderer->gl_fallback) {
		if (vs->renderer->defer_attach) {
			DBG("%s: set flush damage flag.\n", __func__);
			vs->flush_damage = 1;
			pixman_region32_copy(&vs->damage, &surface->damage);
		} else {
			v4l2_gl_flush_damage(surface);
		}
	}
#endif
}

static void
v4l2_release_kms_bo(struct v4l2_surface_state *vs)
{
	int i;

	if (!vs)
		return;

	if (vs->bo) {
		for (i = 0; i < vs->num_planes; i++) {
			if (vs->planes[i].dmafd >= 0) {
				close(vs->planes[i].dmafd);
				vs->planes[i].dmafd = -1;
			}
		}

		if (kms_bo_unmap(vs->bo))
			weston_log("kms_bo_unmap failed.\n");

		kms_bo_destroy(&vs->bo);
		vs->addr = NULL;
		vs->bo = NULL;
	}
}

static void
buffer_state_handle_buffer_destroy(struct wl_listener *listener, void *data)
{
	struct v4l2_surface_state *vs;

	vs = container_of(listener, struct v4l2_surface_state,
			  buffer_destroy_listener);

	v4l2_release_kms_bo(vs);

	vs->buffer_destroy_listener.notify = NULL;
}

static int
v4l2_renderer_attach_shm(struct v4l2_surface_state *vs, struct weston_buffer *buffer,
			 struct wl_shm_buffer *shm_buffer)
{
	unsigned int pixel_format;
	int bpp;
	int fd = vs->renderer->drm_fd;
	unsigned attr[] = {
		KMS_BO_TYPE, KMS_BO_TYPE_SCANOUT_X8R8G8B8,
		KMS_WIDTH, 0,
		KMS_HEIGHT, 0,
		KMS_TERMINATE_PROP_LIST
	};
	unsigned handle, stride, bo_stride;

	switch (wl_shm_buffer_get_format(shm_buffer)) {
	case WL_SHM_FORMAT_XRGB8888:
		pixel_format = V4L2_PIX_FMT_XBGR32;
		bpp = 4;
		break;

	case WL_SHM_FORMAT_ARGB8888:
		pixel_format = V4L2_PIX_FMT_ABGR32;
		bpp = 4;
		break;

	case WL_SHM_FORMAT_RGB565:
		pixel_format = V4L2_PIX_FMT_RGB565;
		bpp = 2;
		break;

	case WL_SHM_FORMAT_YUYV:
		pixel_format = V4L2_PIX_FMT_YUYV;
		bpp = 2;
		break;

	default:
		weston_log("Unsupported SHM buffer format\n");
		return -1;
	}

	buffer->shm_buffer = shm_buffer;
	buffer->width = wl_shm_buffer_get_width(shm_buffer);
	buffer->height = wl_shm_buffer_get_height(shm_buffer);
	stride = wl_shm_buffer_get_stride(shm_buffer);

	if (vs->bo && vs->width == buffer->width &&
	    vs->height == buffer->height &&
	    vs->planes[0].stride == stride && vs->bpp == bpp &&
	    vs->pixel_format == pixel_format) {
		// no need to recreate buffer
		return 0;
	}

	// release if there's allocated buffer
	v4l2_release_kms_bo(vs);
	if (vs->dmabuf_buffer_destroy_listener.notify) {
		wl_list_remove(&vs->dmabuf_buffer_destroy_listener.link);
		vs->dmabuf_buffer_destroy_listener.notify = NULL;
	}

	// create a reference to the shm_buffer.
	vs->width = buffer->width;
	vs->height = buffer->height;
	vs->pixel_format = pixel_format;
	vs->num_planes = 1;
	vs->planes[0].stride = stride;
	vs->planes[0].dmafd = -1;
	vs->planes[0].length = vs->planes[0].bytesused
		= stride * buffer->height;
	vs->bpp = bpp;

	if (device_interface->attach_buffer(vs) == -1)
		return -1;

	// create gbm_bo
	attr[3] = (buffer->width + 1) * bpp / 4;
	attr[5] = buffer->height;

	if (kms_bo_create(vs->renderer->kms, attr, &vs->bo)) {
		weston_log("kms_bo_create failed.\n");
		goto error;
	}

	if (kms_bo_map(vs->bo, &vs->addr)) {
		weston_log("kms_bo_map failed.\n");
		goto error;
	}

	if (kms_bo_get_prop(vs->bo, KMS_PITCH, &bo_stride)) {
		weston_log("kms_bo_get_prop failed.\n");
		goto error;
	}
	vs->bo_stride = stride;

	if (kms_bo_get_prop(vs->bo, KMS_HANDLE, &handle)) {
		weston_log("kms_bo_get_prop failed.\n");
		goto error;
	}
	if (drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC, &vs->planes[0].dmafd)) {
	    weston_log("drmPrimeHandleToFD failed.\n");
	    goto error;
	}

	v4l2_renderer_copy_buffer(vs, buffer);

	DBG("%s: %dx%d buffer attached (dmafd=%d).\n", __func__, buffer->width, buffer->height, vs->planes[0].dmafd);

	return 0;

error:
	v4l2_release_kms_bo(vs);
	return -1;
}

static void
dmabuf_buffer_state_handle_buffer_destroy(struct wl_listener *listener,
					void *data)
{
	struct v4l2_surface_state *vs;

	vs = container_of(listener, struct v4l2_surface_state,
			  dmabuf_buffer_destroy_listener);
	vs->planes[0].dmafd = 0;
	vs->dmabuf_buffer_destroy_listener.notify = NULL;
}

static inline unsigned int
v4l2_renderer_plane_height(int plane, int height, unsigned int format)
{
	switch (plane) {
	case 0:
		return height;
	case 1:
		switch (format) {
		case V4L2_PIX_FMT_NV12M:
		case V4L2_PIX_FMT_NV21M:
		case V4L2_PIX_FMT_YUV420M:
		case V4L2_PIX_FMT_YVU420M:
			return height / 2;
		case V4L2_PIX_FMT_NV16M:
		case V4L2_PIX_FMT_NV61M:
		case V4L2_PIX_FMT_YUV422M:
		case V4L2_PIX_FMT_YVU422M:
		case V4L2_PIX_FMT_YUV444M:
		case V4L2_PIX_FMT_YVU444M:
			return height;
		}
		break;
	case 2:
		switch (format) {
		case V4L2_PIX_FMT_YUV420M:
		case V4L2_PIX_FMT_YVU420M:
			return height / 2;
		case V4L2_PIX_FMT_YUV422M:
		case V4L2_PIX_FMT_YVU422M:
		case V4L2_PIX_FMT_YUV444M:
		case V4L2_PIX_FMT_YVU444M:
			return height;
		}
		break;
	}
	return 0;
}
static int
attach_linux_dmabuf_buffer(struct v4l2_surface_state *vs, struct weston_buffer *buffer,
		struct linux_dmabuf_buffer *dmabuf)
{
	unsigned int pixel_format;
	int bpp, i;

	switch (dmabuf->attributes.format) {
	case DRM_FORMAT_XRGB8888:
		pixel_format = V4L2_PIX_FMT_XBGR32;
		bpp = 4;
		break;

	case DRM_FORMAT_ARGB8888:
		pixel_format = V4L2_PIX_FMT_ABGR32;
		bpp = 4;
		break;

	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_XBGR8888: /* for backward compatibility */
		pixel_format = V4L2_PIX_FMT_XRGB32;
		bpp = 4;
		break;

	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_ABGR8888: /* for backward compatibility */
		pixel_format = V4L2_PIX_FMT_ARGB32;
		bpp = 4;
		break;

	case DRM_FORMAT_RGB888:
		pixel_format = V4L2_PIX_FMT_RGB24;
		bpp = 3;
		break;

	case DRM_FORMAT_BGR888:
		pixel_format = V4L2_PIX_FMT_BGR24;
		bpp = 3;
		break;

	case DRM_FORMAT_RGB565:
		pixel_format = V4L2_PIX_FMT_RGB565;
		bpp = 2;
		break;

	case DRM_FORMAT_RGB332:
		pixel_format = V4L2_PIX_FMT_RGB332;
		bpp = 1;
		break;

	case DRM_FORMAT_YUYV:
		pixel_format = V4L2_PIX_FMT_YUYV;
		bpp = 2;
		break;

	case DRM_FORMAT_YVYU:
		pixel_format = V4L2_PIX_FMT_YVYU;
		bpp = 2;
		break;

	case DRM_FORMAT_UYVY:
		pixel_format = V4L2_PIX_FMT_UYVY;
		bpp = 2;
		break;

	case DRM_FORMAT_VYUY:
		pixel_format = V4L2_PIX_FMT_VYUY;
		bpp = 2;
		break;

	case DRM_FORMAT_NV12:
		pixel_format = V4L2_PIX_FMT_NV12M;
		bpp = 2;
		break;

	case DRM_FORMAT_NV16:
		pixel_format = V4L2_PIX_FMT_NV16M;
		bpp = 2;
		break;

	case DRM_FORMAT_NV21:
		pixel_format = V4L2_PIX_FMT_NV21M;
		bpp = 2;
		break;

	case DRM_FORMAT_NV61:
		pixel_format = V4L2_PIX_FMT_NV61M;
		bpp = 2;
		break;

	case DRM_FORMAT_YUV420:
		pixel_format = V4L2_PIX_FMT_YUV420M;
		bpp = 2;
		break;

	case DRM_FORMAT_YVU420:
		pixel_format = V4L2_PIX_FMT_YVU420M;
		bpp = 2;
		break;

	case DRM_FORMAT_YUV422:
		pixel_format = V4L2_PIX_FMT_YUV422M;
		bpp = 2;
		break;

	case DRM_FORMAT_YVU422:
		pixel_format = V4L2_PIX_FMT_YVU422M;
		bpp = 2;
		break;

	case DRM_FORMAT_YUV444:
		pixel_format = V4L2_PIX_FMT_YUV444M;
		bpp = 3;
		break;

	case DRM_FORMAT_YVU444:
		pixel_format = V4L2_PIX_FMT_YVU444M;
		bpp = 3;
		break;

	default:
		weston_log("Unsupported DMABUF buffer format\n");
		return -1;
	}

	vs->width = buffer->width = dmabuf->attributes.width;
	vs->height = buffer->height = dmabuf->attributes.height;
	vs->pixel_format = pixel_format;
	vs->bpp = bpp;
	vs->num_planes = dmabuf->attributes.n_planes;
	for (i = 0; i < dmabuf->attributes.n_planes; i++) {
		vs->planes[i].stride = dmabuf->attributes.stride[i];
		vs->planes[i].dmafd = dmabuf->attributes.fd[i];
		vs->planes[i].length = vs->planes[i].bytesused
			= vs->planes[i].stride *
				v4l2_renderer_plane_height(i, vs->height,
							   vs->pixel_format);
	}

	DBG("%s: %dx%d buffer attached (dmabuf=%d, stride=%d).\n", __func__,
		dmabuf->attributes.width, dmabuf->attributes.height, dmabuf->attributes.fd[0], dmabuf->attributes.stride);

	return 0;
}

static int
attach_wl_kms_buffer(struct v4l2_surface_state *vs, struct weston_buffer *buffer,
		struct wl_kms_buffer *kbuf)
{
	unsigned int pixel_format;
	int bpp, i;

	switch (kbuf->format) {
	case WL_KMS_FORMAT_XRGB8888:
		pixel_format = V4L2_PIX_FMT_XBGR32;
		bpp = 4;
		break;

	case WL_KMS_FORMAT_ARGB8888:
		pixel_format = V4L2_PIX_FMT_ABGR32;
		bpp = 4;
		break;

	case WL_KMS_FORMAT_XBGR8888:
		pixel_format = V4L2_PIX_FMT_XRGB32;
		bpp = 4;
		break;

	case WL_KMS_FORMAT_ABGR8888:
		pixel_format = V4L2_PIX_FMT_ARGB32;
		bpp = 4;
		break;

	case WL_KMS_FORMAT_RGB888:
		pixel_format = V4L2_PIX_FMT_RGB24;
		bpp = 3;
		break;

	case WL_KMS_FORMAT_BGR888:
		pixel_format = V4L2_PIX_FMT_BGR24;
		bpp = 3;
		break;

	case WL_KMS_FORMAT_RGB565:
		pixel_format = V4L2_PIX_FMT_RGB565;
		bpp = 2;
		break;

	case WL_KMS_FORMAT_RGB332:
		pixel_format = V4L2_PIX_FMT_RGB332;
		bpp = 1;
		break;

	case WL_KMS_FORMAT_YUYV:
		pixel_format = V4L2_PIX_FMT_YUYV;
		bpp = 2;
		break;

	case WL_KMS_FORMAT_YVYU:
		pixel_format = V4L2_PIX_FMT_YVYU;
		bpp = 2;
		break;

	case WL_KMS_FORMAT_UYVY:
		pixel_format = V4L2_PIX_FMT_UYVY;
		bpp = 2;
		break;

	case WL_KMS_FORMAT_NV12:
		pixel_format = V4L2_PIX_FMT_NV12M;
		bpp = 2;
		break;

	case WL_KMS_FORMAT_NV16:
		pixel_format = V4L2_PIX_FMT_NV16M;
		bpp = 2;
		break;

	case WL_KMS_FORMAT_NV21:
		pixel_format = V4L2_PIX_FMT_NV21M;
		bpp = 2;
		break;

	case WL_KMS_FORMAT_NV61:
		pixel_format = V4L2_PIX_FMT_NV61M;
		bpp = 2;
		break;

	case WL_KMS_FORMAT_YUV420:
		pixel_format = V4L2_PIX_FMT_YUV420M;
		bpp = 2;
		break;

	default:
		weston_log("Unsupported DMABUF buffer format\n");
		return -1;
	}

	vs->width = buffer->width = kbuf->width;
	vs->height = buffer->height = kbuf->height;
	vs->pixel_format = pixel_format;
	vs->bpp = bpp;
	vs->num_planes = kbuf->num_planes;
	for (i = 0; i < kbuf->num_planes; i++) {
		vs->planes[i].stride = kbuf->planes[i].stride;
		vs->planes[i].dmafd = kbuf->planes[i].fd;
		vs->planes[i].length = vs->planes[i].bytesused
			= vs->planes[i].stride *
				v4l2_renderer_plane_height(i, vs->height,
							   vs->pixel_format);
	}

	DBG("%s: %dx%d buffer attached (dmabuf=%d, stride=%d).\n", __func__, kbuf->width, kbuf->height, kbuf->fd, kbuf->stride);

	return 0;
}

static int
v4l2_renderer_attach_dmabuf(struct v4l2_surface_state *vs, struct weston_buffer *buffer)
{
	struct linux_dmabuf_buffer *dmabuf;
	struct wl_kms_buffer *kbuf;

	buffer->legacy_buffer = (struct wl_buffer *)buffer->resource;

	v4l2_release_kms_bo(vs);

	if ((dmabuf = linux_dmabuf_buffer_get(buffer->resource))) {
		if (attach_linux_dmabuf_buffer(vs, buffer, dmabuf) < 0)
			return -1;
	} else if ((kbuf = wayland_kms_buffer_get(buffer->resource))) {
		if (attach_wl_kms_buffer(vs, buffer, kbuf) < 0)
			return -1;
	} else {
		return -1;
	}

	if (device_interface->attach_buffer(vs) == -1)
		return -1;

	if (vs->dmabuf_buffer_destroy_listener.notify) {
		wl_list_remove(&vs->dmabuf_buffer_destroy_listener.link);
		vs->dmabuf_buffer_destroy_listener.notify = NULL;
	}

	vs->dmabuf_buffer_destroy_listener.notify
		= dmabuf_buffer_state_handle_buffer_destroy;
	wl_resource_add_destroy_listener(buffer->resource,
				&vs->dmabuf_buffer_destroy_listener);

	return 0;
}

static void
v4l2_renderer_attach(struct weston_surface *es, struct weston_buffer *buffer)
{
	struct v4l2_surface_state *vs = get_surface_state(es);
	struct wl_shm_buffer *shm_buffer;
	int ret;

	// refer the given weston_buffer. if there's an existing reference,
	// release it first if not the same. if the buffer is the new one,
	// increment the refrence counter. all done in weston_buffer_reference().
	weston_buffer_reference(&vs->buffer_ref, buffer);

	// clear the destroy listener if set.
	if (vs->buffer_destroy_listener.notify) {
		wl_list_remove(&vs->buffer_destroy_listener.link);
		vs->buffer_destroy_listener.notify = NULL;
	}

	if (buffer) {
		// for shm_buffer.
		shm_buffer = wl_shm_buffer_get(buffer->resource);

		if (shm_buffer) {
			ret = v4l2_renderer_attach_shm(vs, buffer, shm_buffer);
		} else {
			ret = v4l2_renderer_attach_dmabuf(vs, buffer);
		}

		if (ret == -1) {
			weston_buffer_reference(&vs->buffer_ref, NULL);
			return;
		}

		// listen to the buffer destroy event.
		vs->buffer_destroy_listener.notify =
			buffer_state_handle_buffer_destroy;
		wl_signal_add(&buffer->destroy_signal,
			      &vs->buffer_destroy_listener);
	}

#ifdef V4L2_GL_FALLBACK_ENABLED
	if (vs->renderer->gl_fallback) {
		if (vs->renderer->defer_attach) {
			if (!vs->notify_attach)
				v4l2_gl_attach(es, NULL);
			vs->notify_attach = 1;
		} else {
			v4l2_gl_attach(es, buffer);
		}
	}
#endif
}

static void
v4l2_renderer_surface_state_destroy(struct v4l2_surface_state *vs)
{
	wl_list_remove(&vs->surface_destroy_listener.link);
	wl_list_remove(&vs->renderer_destroy_listener.link);
	if (vs->buffer_destroy_listener.notify) {
		wl_list_remove(&vs->buffer_destroy_listener.link);
		vs->buffer_destroy_listener.notify = NULL;
	}

	if (vs->dmabuf_buffer_destroy_listener.notify) {
		wl_list_remove(&vs->dmabuf_buffer_destroy_listener.link);
		vs->dmabuf_buffer_destroy_listener.notify = NULL;
	}

	// TODO: Release any resources associated to the surface here.

	v4l2_release_kms_bo(vs);
	weston_buffer_reference(&vs->buffer_ref, NULL);

#ifdef V4L2_GL_FALLBACK_ENABLED
	if (vs->surface_type == V4L2_SURFACE_GL_ATTACHED) {
		vs->surface->compositor->renderer = vs->renderer->gl_renderer;
		vs->surface->renderer_state = vs->gl_renderer_state;
		return;
	}
#endif

	vs->surface->renderer_state = NULL;
	free(vs);
}

static void
surface_state_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct v4l2_surface_state *vs;

	vs = container_of(listener, struct v4l2_surface_state,
				     surface_destroy_listener);

	v4l2_renderer_surface_state_destroy(vs);
}

static void
surface_state_handle_renderer_destroy(struct wl_listener *listener, void *data)
{
	struct v4l2_surface_state *vs;

	vs = container_of(listener, struct v4l2_surface_state,
				     renderer_destroy_listener);

	v4l2_renderer_surface_state_destroy(vs);
}

static int
v4l2_renderer_create_surface(struct weston_surface *surface)
{
	struct v4l2_surface_state *vs;
	struct v4l2_renderer *vr = get_renderer(surface->compositor);

	vs = device_interface->create_surface(vr->device);
	if (!vs)
		return -1;

	surface->renderer_state = vs;

	vs->surface = surface;
	vs->renderer = vr;

	vs->surface_destroy_listener.notify =
		surface_state_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &vs->surface_destroy_listener);

	vs->renderer_destroy_listener.notify =
		surface_state_handle_renderer_destroy;
	wl_signal_add(&vr->destroy_signal,
		      &vs->renderer_destroy_listener);

#ifdef V4L2_GL_FALLBACK_ENABLED
	vs->surface_type = V4L2_SURFACE_DEFAULT;
	vs->notify_attach = -1;
	if (vr->defer_attach)
		pixman_region32_init(&vs->damage);
#endif
	return 0;
}

static void
v4l2_renderer_surface_set_color(struct weston_surface *es,
				float red, float green, float blue, float alpha)
{
	DBG("%s\n", __func__);

	// struct v4l2_surface_state *vs = get_surface_state(es);

	// TODO: set solid color to the surface
}

static void
v4l2_renderer_destroy(struct weston_compositor *ec)
{
	struct v4l2_renderer *vr = get_renderer(ec);

	DBG("%s\n", __func__);

	wl_signal_emit(&vr->destroy_signal, vr);
	weston_binding_destroy(vr->debug_binding);
	free(vr);

	// TODO: release gl-renderer here.

	ec->renderer = NULL;
}

static void
debug_binding(struct weston_keyboard *keyboard, uint32_t time, uint32_t key,
	      void *data)
{
	struct weston_compositor *ec = data;
	struct v4l2_renderer *vr = (struct v4l2_renderer *) ec->renderer;

	vr->repaint_debug ^= 1;

	if (vr->repaint_debug) {
		// TODO: enable repaint debug
	} else {
		// TODO: disable repaint debug
		weston_compositor_damage_all(ec);
	}
}

static void
v4l2_load_device_module(const char *device_name)
{
	char path[1024];

	if (!device_name)
		return;

	snprintf(path, sizeof(path), "v4l2-%s-device.so", device_name);
	device_interface =
		(struct v4l2_device_interface*)weston_load_module(path, "v4l2_device_interface");
}

static char*
v4l2_get_cname(const char *bus_info)
{
	char *p, *device_name;

	if (!bus_info)
		return NULL;

	if ((p = strchr(bus_info, ':')))
		device_name = strdup(p + 1);
	else
		device_name = strdup(bus_info);

	p = strchr(device_name, '.');
	if (p)
		*p = '\0';

	return device_name;
}

static bool
v4l2_renderer_import_dmabuf(struct weston_compositor *ec,
                           struct linux_dmabuf_buffer *dmabuf)
{
	uint32_t mask = ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT |
			ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_INTERLACED |
			ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_BOTTOM_FIRST;

#ifdef V4L2_GL_FALLBACK_ENABLED
	struct v4l2_renderer *renderer = get_renderer(ec);
	if (renderer->gl_fallback) {
		return v4l2_gl_import_dmabuf(ec, dmabuf);
	}
#endif
	/* Always regard as downward y-coordinates and
	   interlaced images are not supported. */
	if ((dmabuf->attributes.flags & mask) ==
			ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT)
		return true;

	return false;
}

static int
v4l2_renderer_init(struct weston_compositor *ec, int drm_fd, char *drm_fn)
{
	struct v4l2_renderer *renderer;
	char *device;
	char *device_name = NULL;
	static struct media_device_info info;
	struct weston_config_section *section;

	if (!drm_fn)
		return -1;

	renderer = calloc(1, sizeof *renderer);
	if (renderer == NULL)
		return -1;

	renderer->wl_kms = wayland_kms_init(ec->wl_display, NULL, drm_fn, drm_fd);

	/* Get V4L2 media controller device to use */
	section = weston_config_get_section(ec->config, "v4l2-renderer", NULL, NULL);
	weston_config_section_get_string(section, "device", &device, "/dev/media0");
#ifdef V4L2_GL_FALLBACK_ENABLED
	weston_config_section_get_bool(section, "gl-fallback", &renderer->gl_fallback, 0);
	weston_config_section_get_bool(section, "defer-attach", &renderer->defer_attach, 0);
#endif

	/* Initialize V4L2 media controller */
	renderer->media_fd = open(device, O_RDWR);
	if (renderer->media_fd < 0) {
		weston_log("Can't open the media device.");
		goto error;
	}

	/* Device info */
	if (ioctl(renderer->media_fd, MEDIA_IOC_DEVICE_INFO, &info) < 0) {
		weston_log("Can't get media device info.");
		goto error;
	}
	weston_log("Media controller API version %u.%u.%u\n",
		   (info.media_version >> 16) & 0xff,
		   (info.media_version >>  8) & 0xff,
		   (info.media_version)       & 0xff);
	weston_log_continue("Media device information\n"
			    "------------------------\n"
			    "driver         %s\n"
			    "model          %s\n"
			    "serial         %s\n"
			    "bus info       %s\n"
			    "hw revision    0x%x\n"
			    "driver version %u.%u.%u\n",
			    info.driver, info.model,
			    info.serial, info.bus_info,
			    info.hw_revision,
			    (info.driver_version >> 16) & 0xff,
			    (info.driver_version >>  8) & 0xff,
			    (info.driver_version)       & 0xff);

	/* Get device module to use */
	weston_config_section_get_string(section, "device-module",
					 &device_name, NULL);
	if (!device_name)
		device_name = v4l2_get_cname(info.bus_info);
	v4l2_load_device_module(device_name);
	if (!device_interface)
		goto error;

	renderer->device = device_interface->init(renderer->media_fd, &info, ec->config);
	if (!renderer->device)
		goto error;

	weston_log("V4L2 media controller device initialized.\n");

	kms_create(drm_fd, &renderer->kms);

	/* initialize renderer base */
	renderer->drm_fd = drm_fd;
	renderer->repaint_debug = 0;

	renderer->base.read_pixels = v4l2_renderer_read_pixels;
	renderer->base.repaint_output = v4l2_renderer_repaint_output;
	renderer->base.flush_damage = v4l2_renderer_flush_damage;
	renderer->base.attach = v4l2_renderer_attach;
	renderer->base.surface_set_color = v4l2_renderer_surface_set_color;
	renderer->base.destroy = v4l2_renderer_destroy;
	renderer->base.import_dmabuf = v4l2_renderer_import_dmabuf;

#if defined(V4L2_GL_FALLBACK_ENABLED) || defined(VSP2_SCALER_ENABLED)
	renderer->device->kms = renderer->kms;
	renderer->device->drm_fd = drm_fd;

#  ifdef V4L2_GL_FALLBACK_ENABLED
	if (renderer->device->disable_gl_fallback)
		renderer->gl_fallback = 0;

	if (renderer->gl_fallback) {
		/* we now initialize gl-renderer for fallback */
		renderer->gbm = v4l2_create_gbm_device(drm_fd);
		if (renderer->gbm) {
			if (v4l2_create_gl_renderer(ec, renderer) < 0) {
				weston_log("GL Renderer fallback failed to initialize.\n");
				v4l2_destroy_gbm_device(renderer->gbm);
				renderer->gbm = NULL;
			}
		}
	}
#  endif
#endif

	ec->renderer = &renderer->base;
	ec->capabilities |= device_interface->get_capabilities();

	ec->read_format = PIXMAN_a8r8g8b8;

	renderer->debug_binding =
		weston_compositor_add_debug_binding(ec, KEY_R,
						    debug_binding, ec);

	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_RGB565);
	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_XRGB8888);
	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_ARGB8888);
	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_YUYV);

	wl_signal_init(&renderer->destroy_signal);

	free((void *)device_name);
	free(device);
	return 0;

error:
	if (device_name)
		free((void *)device_name);
	free(device);
	free(renderer);
	weston_log("V4L2 renderer initialization failed.\n");
	return -1;
}

static void
v4l2_renderer_output_set_buffer(struct weston_output *output, int bo_index)
{
	struct v4l2_output_state *vo = get_output_state(output);

	vo->bo_index = bo_index;
	device_interface->set_output_buffer(vo->output, &vo->bo[bo_index]);
	return;
}

static int
v4l2_renderer_output_create(struct weston_output *output, struct v4l2_bo_state *bo_states, int count)
{
	struct v4l2_renderer *renderer = (struct v4l2_renderer*)output->compositor->renderer;
	struct v4l2_output_state *vo;
	struct v4l2_renderer_output *outdev;
	int i;

	if (!renderer)
		return -1;

	outdev = device_interface->create_output(renderer->device,
						 output->current_mode->width,
						 output->current_mode->height);
	if (!outdev)
		return -1;

	if (!(vo = calloc(1, sizeof *vo))) {
		free(outdev);
		return -1;
	}

	vo->output = outdev;

	output->renderer_state = vo;

	if (!(vo->bo = calloc(1, sizeof(struct v4l2_bo_state) * count))) {
		free(vo);
		free(outdev);
		return -1;
	}

	for (i = 0; i < count; i++)
		vo->bo[i] = bo_states[i];
	vo->bo_count = count;

#ifdef V4L2_GL_FALLBACK_ENABLED
	if ((renderer->gl_fallback) && (v4l2_init_gl_output(output, renderer) < 0)) {
		// error...
		weston_log("Can't initialize gl-renderer. Disabling gl-fallback.\n");
		renderer->gl_fallback = 0;
	}
#endif

	return 0;
}

static void
v4l2_renderer_output_destroy(struct weston_output *output)
{
	struct v4l2_output_state *vo = get_output_state(output);

#ifdef V4L2_GL_FALLBACK_ENABLED
	struct v4l2_renderer *renderer =
		(struct v4l2_renderer*)output->compositor->renderer;

	if (renderer->gl_fallback)
		v4l2_gl_output_destroy(output, renderer);
#endif

	if (vo->bo)
		free(vo->bo);
	if (vo->output)
		free(vo->output);
	free(vo);
}

WL_EXPORT struct v4l2_renderer_interface v4l2_renderer_interface = {
	.init = v4l2_renderer_init,
	.output_create = v4l2_renderer_output_create,
	.output_destroy = v4l2_renderer_output_destroy,
	.set_output_buffer = v4l2_renderer_output_set_buffer
};

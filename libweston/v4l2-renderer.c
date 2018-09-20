/*
 * Copyright © 2014-2018 Renesas Electronics Corp.
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
#include "compositor/weston.h"

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

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) > (b)) ? (b) : (a))

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
	bool gl_fallback;
	bool defer_attach;
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
	if (!surface->renderer_state) {
		if (v4l2_renderer_create_surface(surface)) {
			weston_log("can't allocate memory for a v4l2 surface\n");
			return NULL;
		}
	}

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
	if (!dlopen("libglapi.so.0", RTLD_LAZY | RTLD_GLOBAL))
		return NULL;

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

	/* Don't support EGL_PLATFORM_GBM_KHR */
	if (gl_renderer->display_create(ec, 0, renderer->gbm,
					NULL, gl_renderer->opaque_attribs,
					&format, 1) < 0) {
		return -1;
	}
	renderer->gl_renderer = ec->renderer;

	return 0;
}

static void
v4l2_gl_gbm_surface_destroy(struct v4l2_output_state *state)
{
	int i;
	struct gbm_kms_surface *surface = (struct gbm_kms_surface *)state->gbm_surface;
	for (i = 0; i < 2; i++) {
		int n = i % state->bo_count;
		if (surface->bo[n])
			gbm_bo_destroy((struct gbm_bo *)surface->bo[n]);
	}
	gbm_surface_destroy(state->gbm_surface);
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
						GBM_BO_USE_RENDERING);

	if (!state->gbm_surface) {
		weston_log("%s: failed to create gbm surface\n", __func__);
		return -1;
	}

	for (i = 0; i < 2; i++) {
		int n = i % state->bo_count;
		if (gbm_kms_set_bo((struct gbm_kms_surface *)state->gbm_surface,
				   n, state->bo[n].map, state->bo[n].dmafd,
				   state->bo[n].stride) < 0) {
			weston_log("%s: failed to set bo to gbm surface\n", __func__);
			v4l2_gl_gbm_surface_destroy(state);
			return -1;
		}
	}

	output->compositor->renderer = renderer->gl_renderer;
	output->renderer_state = NULL;
	read_format = output->compositor->read_format;
	if (gl_renderer->output_window_create(output,
					      (EGLNativeDisplayType)state->gbm_surface,
					      state->gbm_surface,
					      gl_renderer->opaque_attribs,
					      &format, 1) < 0) {
		weston_log("%s: failed to create gl renderer output state\n", __func__);
		v4l2_gl_gbm_surface_destroy(state);
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

	v4l2_gl_gbm_surface_destroy(state);
}

static void
v4l2_gl_flush_damage(struct weston_surface *surface)
{
	struct v4l2_surface_state *vs = get_surface_state(surface);
	struct v4l2_renderer *renderer;

	if (!vs)
		return;

	renderer = vs->renderer;

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
	struct v4l2_renderer *renderer;

	if (!vs)
		return;

	renderer = vs->renderer;

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
		if (!vs)
			continue;

		if (renderer->defer_attach) {
			if (vs->notify_attach) {
				DBG("%s: attach gl\n", __func__);
				v4l2_gl_attach(ev->surface, vs->buffer_ref.buffer);
				vs->notify_attach = false;
			}
			if (vs->flush_damage) {
				DBG("%s: flush damage\n", __func__);
				pixman_region32_copy(&ev->surface->damage, &vs->damage);
				v4l2_gl_flush_damage(ev->surface);
				vs->flush_damage = false;
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
	uint32_t v, len = width * 4U;
	void *src, *dst;

	switch(format) {
	case PIXMAN_a8r8g8b8:
		break;
	default:
		return -1;
	}

#ifdef V4L2_GL_FALLBACK_ENABLED
	if (output->compositor->capabilities & WESTON_CAP_CAPTURE_YFLIP) {
		src = bo->map + x * 4U + (output->height - (y + height)) * bo->stride;
		dst = pixels + len * (height - 1u);
		for (v = 0; v < height; v++) {
			memcpy(dst, src, len);
			src += bo->stride;
			dst -= len;
		}
		return 0;
	}
#endif

	if (x == 0U && y == 0U &&
	    width == (uint32_t)output->current_mode->width &&
	    height == (uint32_t)output->current_mode->height &&
	    bo->stride == len) {
		DBG("%s: copy entire buffer at once\n", __func__);
		// TODO: we may want to optimize this using underlying
		// V4L2 MC hardware if possible.
		memcpy(pixels, bo->map, bo->stride * height);
		return 0;
	}

	src = bo->map + x * 4U + y * bo->stride;
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
	default:
		break; /* nothing to do */
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
	default:
		break; /* nothing to do */
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
view_to_global_region(struct weston_view *ev, pixman_region32_t *src_region,
		      pixman_region32_t *dst_region)
{
	pixman_region32_t region;
	pixman_box32_t *b = pixman_region32_extents(src_region);
	float surf_x[4] = {b->x1, b->x2, b->x2, b->x1};
	float surf_y[4] = {b->y1, b->y1, b->y2, b->y2};
	float min_x, min_y, max_x, max_y;
	int i;

	for (i = 0; i < 4; i++)
		weston_view_to_global_float(ev, surf_x[i], surf_y[i],
					    &surf_x[i], &surf_y[i]);

	min_x = max_x = surf_x[0];
	min_y = max_y = surf_y[0];
	for (i = 1; i < 4; i++) {
		min_x = min(min_x, surf_x[i]);
		max_x = max(max_x, surf_x[i]);
		min_y = min(min_y, surf_y[i]);
		max_y = max(max_y, surf_y[i]);
	}

	pixman_region32_init_rect(&region, (int)min_x, (int)min_y,
				  (int)(max_x - min_x), (int)(max_y -min_y));
	pixman_region32_copy(dst_region, &region);
	pixman_region32_fini(&region);
}

static void
set_v4l2_rect(pixman_region32_t *region, struct v4l2_rect *rect)
{
	pixman_box32_t *bbox;

	bbox = pixman_region32_extents(region);
	rect->left   = bbox->x1;
	rect->top    = bbox->y1;
	rect->width  = (unsigned int)(bbox->x2 - bbox->x1);
	rect->height = (unsigned int)(bbox->y2 - bbox->y1);
}

static void
draw_view(struct weston_view *ev, struct weston_output *output, pixman_region32_t *damage)
{
	struct v4l2_renderer *renderer = (struct v4l2_renderer*)output->compositor->renderer;
	struct v4l2_surface_state *vs = get_surface_state(ev->surface);
	pixman_region32_t dst_region, src_region, buffer_region;
	pixman_region32_t region, opaque_src_region, opaque_dst_region;
	pixman_region32_t tmp_region;
	pixman_transform_t transform;
	pixman_box32_t *box;

	if (!vs)
		return;

	/* No buffer attached */
	if (vs->num_planes == 0)
		return;

	vs->in_expanded_damage = false;

	/* a surface in the repaint area? */
	pixman_region32_init(&region);
	pixman_region32_intersect(&region,
				  &ev->transform.boundingbox,
				  damage);
	pixman_region32_init(&tmp_region);
	pixman_region32_subtract(&tmp_region, &region, &ev->clip);

	if (!pixman_region32_not_empty(&tmp_region)) {
		DBG("%s: skipping a view: not visible: view=(%d,%d)-(%d,%d), repaint=(%d,%d)-(%d,%d)\n",
		    __func__,
		    ev->transform.boundingbox.extents.x1, ev->transform.boundingbox.extents.y1,
		    ev->transform.boundingbox.extents.x2, ev->transform.boundingbox.extents.y2,
		    output->region.extents.x1, output->region.extents.y1,
		    output->region.extents.x2, output->region.extents.y2);
		goto out;
	}
	if (pixman_region32_equal(damage, &output->region))
		pixman_region32_copy(&region, &tmp_region);

	/* you may sometime get not-yet-attached views */
	if (vs->planes[0].dmafd < 0)
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

	/* find out the final destination in the output coordinate */
	pixman_region32_init(&dst_region);
	pixman_region32_copy(&dst_region, &region);
	region_global_to_output(output, &dst_region);

	transform_region(&transform, &dst_region, &src_region);

	/*
	  Prevent misalignment due to calculation error by calculating
	  intersection of source and buffer region.
	 */
	pixman_region32_init_rect(&buffer_region, 0, 0,
				  ev->surface->width_from_buffer * ev->surface->buffer_viewport.buffer.scale,
				  ev->surface->height_from_buffer * ev->surface->buffer_viewport.buffer.scale);
	pixman_region32_intersect(&src_region, &src_region, &buffer_region);

	pixman_region32_init(&opaque_src_region);
	pixman_region32_init(&opaque_dst_region);

	box = pixman_region32_extents(&ev->surface->opaque);
	/* Check if the opaque region is whole of surface */
	if (ev->surface->width == box->x2 && ev->surface->height == box->y2) {
		pixman_region32_copy(&opaque_src_region, &src_region);
		pixman_region32_copy(&opaque_dst_region, &dst_region);
	} else if (pixman_region32_not_empty(&ev->surface->opaque)) {
		pixman_region32_t clip_region;

		view_to_global_region(ev, &ev->surface->opaque,
				      &opaque_dst_region);
		pixman_region32_intersect(&opaque_dst_region, &opaque_dst_region, &region);
		region_global_to_output(output, &opaque_dst_region);

		pixman_region32_init_rect(&clip_region, output->x, output->y, output->width, output->height);
		pixman_region32_intersect(&clip_region, &clip_region, &ev->clip);

		/* clipping */
		if (pixman_region32_not_empty(&clip_region)) {
			pixman_region32_translate(&clip_region, -output->x, -output->y);
			pixman_region32_subtract(&opaque_dst_region, &opaque_dst_region, &clip_region);
		}
		transform_region(&transform, &opaque_dst_region, &opaque_src_region);
		pixman_region32_intersect(&opaque_src_region,
					  &opaque_src_region, &buffer_region);
	}

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

/* If color format of the surface in damage region is multi sample pixels,
   the damage region is expanded to contain whole of the surface. */
static void
expand_damage_region(struct weston_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *compositor = output->compositor;
	struct weston_view *view;
	struct v4l2_surface_state *vs;
	pixman_region32_t region;
	bool check_again, expanded = false;

	pixman_region32_init(&region);
	do {
		check_again = false;
		wl_list_for_each_reverse(view, &compositor->view_list, link) {
			if (view->plane != &compositor->primary_plane)
				continue;

			vs = get_surface_state(view->surface);
			if (vs->in_expanded_damage || !vs->multi_sample_pixels)
				continue;

			pixman_box32_t *b = pixman_region32_extents(damage);
			pixman_region32_intersect_rect(&region,
						       &view->transform.boundingbox,
						       b->x1, b->y1,
						       b->x2 - b->x1, b->y2 - b->y1);
			if (!pixman_region32_not_empty(&region))
				continue;

			pixman_region32_union(damage,
					      &view->transform.boundingbox,
					      damage);
			vs->in_expanded_damage = true;
			check_again = true;
			expanded = true;
		}
	} while (check_again);
	pixman_region32_fini(&region);

	if (!expanded)
		return;

	pixman_region32_intersect(damage, damage, &output->region);

	/* force update output image in new damage region */
	wl_list_for_each_reverse(view, &compositor->view_list, link)
		pixman_region32_subtract(&view->clip, &view->clip, damage);
}

static void
repaint_surfaces(struct weston_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *compositor = output->compositor;
	struct v4l2_output_state *vo = get_output_state(output);
	struct v4l2_renderer *renderer = (struct v4l2_renderer*)compositor->renderer;
	struct weston_view *view;
	pixman_region32_t damage_extents;

	if (!device_interface->begin_compose(renderer->device, vo->output))
		return;

	expand_damage_region(output, damage);
	pixman_region32_init_with_extents(&damage_extents,
					  pixman_region32_extents(damage));
	wl_list_for_each_reverse(view, &compositor->view_list, link) {
		if (view->plane == &compositor->primary_plane) {
			if (renderer->device->enable_composition_with_damage)
				draw_view(view, output, &damage_extents);
			else
				draw_view(view, output, &output->region);
		}
	}
	pixman_region32_fini(&damage_extents);

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
			if (vs) {
				view_list[view_count].view = ev;
				view_list[view_count].state = vs;
				view_count++;
			}
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

	if (renderer->gl_fallback) {
		if (!can_repaint(output->compositor, &output->region)) {
			struct v4l2_output_state *vo = get_output_state(output);
			gbm_kms_set_front((struct gbm_kms_surface *)vo->gbm_surface, (!vo->bo_index));
			v4l2_gl_repaint(output, output_damage);
			return;
		}
	}
#endif

	// render all views
	if (pixman_region32_not_empty(output_damage))
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
	void *src = wl_shm_buffer_get_data(buffer->shm_buffer);

	wl_shm_buffer_begin_access(buffer->shm_buffer);
	for (int i = 0; i < vs->num_planes; i++) {
		size_t sz = (size_t)(vs->planes[i].stride * vs->planes[i].height);
		memcpy(vs->planes[i].addr, src, sz);
		src += sz;
	}
	wl_shm_buffer_end_access(buffer->shm_buffer);
}

static void
v4l2_renderer_flush_damage(struct weston_surface *surface)
{
	struct v4l2_surface_state *vs = get_surface_state(surface);
	struct weston_buffer *buffer;

	if (!vs)
		return;
	buffer = vs->buffer_ref.buffer;

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
			vs->flush_damage = true;
			pixman_region32_copy(&vs->damage, &surface->damage);
		} else {
			v4l2_gl_flush_damage(surface);
		}
	}
#endif
}

static void
v4l2_release_dmabuf(struct v4l2_surface_state *vs)
{
    int i;

    for (i = 0; i < vs->num_planes; i++) {
	    if (vs->planes[i].dmafd >= 0) {
		    close(vs->planes[i].dmafd);
		    vs->planes[i].dmafd = -1;
	    }
    }
}

static void
v4l2_release_kms_bo(struct v4l2_surface_state *vs)
{
	int i;

	if (!vs)
		return;

	for (i = 0; i < vs->num_planes; i++) {
		if (vs->planes[i].bo) {
			if (kms_bo_unmap(vs->planes[i].bo))
				weston_log("kms_bo_unmap failed.\n");

			kms_bo_destroy(&vs->planes[i].bo);
			vs->planes[i].addr = NULL;
			vs->planes[i].bo = NULL;
		}
	}
}

static int
v4l2_renderer_attach_shm(struct v4l2_surface_state *vs, struct weston_buffer *buffer,
			 struct wl_shm_buffer *shm_buffer)
{
	unsigned int pixel_format;
	int fd = vs->renderer->drm_fd;
	unsigned attr[] = {
		KMS_BO_TYPE, KMS_BO_TYPE_SCANOUT_X8R8G8B8,
		KMS_WIDTH, 0,
		KMS_HEIGHT, 0,
		KMS_TERMINATE_PROP_LIST
	};
	unsigned handle, stride, uv_stride = 0U;
	int num_planes, width, height;
	unsigned bo_width[3];
	bool multi_sample_pixels = false;
	int i;

	width = wl_shm_buffer_get_width(shm_buffer);
	height = wl_shm_buffer_get_height(shm_buffer);
	stride = (unsigned int)wl_shm_buffer_get_stride(shm_buffer);

	num_planes = 1;

	switch (wl_shm_buffer_get_format(shm_buffer)) {
	case WL_SHM_FORMAT_XRGB8888:
		pixel_format = V4L2_PIX_FMT_XBGR32;
		bo_width[0] = (unsigned int)width;
		break;

	case WL_SHM_FORMAT_ARGB8888:
		pixel_format = V4L2_PIX_FMT_ABGR32;
		bo_width[0] = (unsigned int)width;
		break;

	case WL_SHM_FORMAT_RGB565:
		pixel_format = V4L2_PIX_FMT_RGB565;
		bo_width[0] = ((unsigned int)width + 1U) / 2U;
		break;

	case WL_SHM_FORMAT_YUYV:
		pixel_format = V4L2_PIX_FMT_YUYV;
		bo_width[0] = ((unsigned int)width + 1U) / 2U;
		multi_sample_pixels = true;
		break;

	case WL_SHM_FORMAT_NV12:
		pixel_format = V4L2_PIX_FMT_NV12M;
		num_planes = 2;

		// No odd sizes are expected
		uv_stride = stride;
		bo_width[0] = ((unsigned int)width + 2U) / 4U;
		bo_width[1] = bo_width[0];

		multi_sample_pixels = true;
		break;

	case WL_SHM_FORMAT_YUV420:
		pixel_format = V4L2_PIX_FMT_YUV420M;
		num_planes = 3;

		// No odd sizes are expected
		uv_stride = stride / 2U;
		bo_width[0] = ((unsigned int)width + 2U) / 4U;
		bo_width[1] = (bo_width[0] + 1U) / 2U;

		bo_width[2] = bo_width[1];
		multi_sample_pixels = true;
		break;

	default:
		weston_log("Unsupported SHM buffer format\n");
		return -1;
	}

	buffer->shm_buffer = shm_buffer;
	buffer->width = width;
	buffer->height = height;

	if (vs->planes[0].bo && vs->width == buffer->width &&
	    vs->height == buffer->height &&
	    vs->planes[0].stride == stride &&
	    vs->pixel_format == pixel_format) {
		// no need to recreate buffer
		return 0;
	}

	// release if there's allocated buffer
	v4l2_release_dmabuf(vs);
	v4l2_release_kms_bo(vs);

	// create a reference to the shm_buffer.
	vs->width = buffer->width;
	vs->height = buffer->height;
	vs->pixel_format = pixel_format;
	vs->num_planes = num_planes;

	vs->planes[0].dmafd = -1;
	vs->planes[0].stride = stride;
	vs->planes[0].height = height;
	vs->planes[0].length = stride * (unsigned int)height;

	if (num_planes > 1) {
		vs->planes[1].dmafd = -1;
		vs->planes[1].stride = uv_stride;
		vs->planes[1].height = height / 2;
		vs->planes[1].length = uv_stride * (unsigned int)height / 2U;

		if (num_planes == 3)
			vs->planes[2] = vs->planes[1];
	}

	vs->multi_sample_pixels = multi_sample_pixels;


	if (device_interface->attach_buffer(vs) == -1) {
		weston_log("attach_buffer failed.\n");
		return -1;
	}

	// create gbm_bo
	for (i = 0; i < num_planes; i++) {
		attr[3] = bo_width[i];
		attr[5] = (unsigned)vs->planes[i].height;

		if (kms_bo_create(vs->renderer->kms, attr, &vs->planes[i].bo)) {
			weston_log("kms_bo_create failed.\n");
			goto error;
		}

		if (kms_bo_map(vs->planes[i].bo, &vs->planes[i].addr)) {
			weston_log("kms_bo_map failed.\n");
			goto error;
		}

		if (kms_bo_get_prop(vs->planes[i].bo, KMS_HANDLE, &handle)) {
			weston_log("kms_bo_get_prop failed.\n");
			goto error;
		}
		if (drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC, &vs->planes[i].dmafd)) {
			weston_log("drmPrimeHandleToFD failed.\n");
			goto error;
		}
	}

	v4l2_renderer_copy_buffer(vs, buffer);

	DBG("%s: %dx%d buffer attached (dmafd=%d).\n", __func__, buffer->width, buffer->height, vs->planes[0].dmafd);

	return 0;

error:
	v4l2_release_dmabuf(vs);
	v4l2_release_kms_bo(vs);
	return -1;
}

static inline unsigned int
v4l2_renderer_plane_height(int plane, int _height, unsigned int format)
{
	unsigned int height = (unsigned int)_height;

	switch (plane) {
	case 0:
		return height;
	case 1:
		switch (format) {
		case V4L2_PIX_FMT_NV12M:
		case V4L2_PIX_FMT_NV21M:
		case V4L2_PIX_FMT_YUV420M:
		case V4L2_PIX_FMT_YVU420M:
			return height / 2U;
		case V4L2_PIX_FMT_NV16M:
		case V4L2_PIX_FMT_NV61M:
		case V4L2_PIX_FMT_YUV422M:
		case V4L2_PIX_FMT_YVU422M:
		case V4L2_PIX_FMT_YUV444M:
		case V4L2_PIX_FMT_YVU444M:
			return height;
		default:
			break;
		}
		break;
	case 2:
		switch (format) {
		case V4L2_PIX_FMT_YUV420M:
		case V4L2_PIX_FMT_YVU420M:
			return height / 2U;
		case V4L2_PIX_FMT_YUV422M:
		case V4L2_PIX_FMT_YVU422M:
		case V4L2_PIX_FMT_YUV444M:
		case V4L2_PIX_FMT_YVU444M:
			return height;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return 0;
}
static int
attach_linux_dmabuf_buffer(struct v4l2_surface_state *vs, struct weston_buffer *buffer,
		struct linux_dmabuf_buffer *dmabuf)
{
	unsigned int pixel_format;
	int i;
	bool multi_sample_pixels = false;

	switch (dmabuf->attributes.format) {
	case DRM_FORMAT_XRGB8888:
		pixel_format = V4L2_PIX_FMT_XBGR32;
		break;

	case DRM_FORMAT_ARGB8888:
		pixel_format = V4L2_PIX_FMT_ABGR32;
		break;

	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_XBGR8888: /* for backward compatibility */
		pixel_format = V4L2_PIX_FMT_XRGB32;
		break;

	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_ABGR8888: /* for backward compatibility */
		pixel_format = V4L2_PIX_FMT_ARGB32;
		break;

	case DRM_FORMAT_RGB888:
		pixel_format = V4L2_PIX_FMT_RGB24;
		break;

	case DRM_FORMAT_BGR888:
		pixel_format = V4L2_PIX_FMT_BGR24;
		break;

	case DRM_FORMAT_RGB565:
		pixel_format = V4L2_PIX_FMT_RGB565;
		break;

	case DRM_FORMAT_RGB332:
		pixel_format = V4L2_PIX_FMT_RGB332;
		break;

	case DRM_FORMAT_YUYV:
		pixel_format = V4L2_PIX_FMT_YUYV;
		multi_sample_pixels = true;
		break;

	case DRM_FORMAT_YVYU:
		pixel_format = V4L2_PIX_FMT_YVYU;
		multi_sample_pixels = true;
		break;

	case DRM_FORMAT_UYVY:
		pixel_format = V4L2_PIX_FMT_UYVY;
		multi_sample_pixels = true;
		break;

	case DRM_FORMAT_VYUY:
		pixel_format = V4L2_PIX_FMT_VYUY;
		multi_sample_pixels = true;
		break;

	case DRM_FORMAT_NV12:
		pixel_format = V4L2_PIX_FMT_NV12M;
		multi_sample_pixels = true;
		break;

	case DRM_FORMAT_NV16:
		pixel_format = V4L2_PIX_FMT_NV16M;
		multi_sample_pixels = true;
		break;

	case DRM_FORMAT_NV21:
		pixel_format = V4L2_PIX_FMT_NV21M;
		multi_sample_pixels = true;
		break;

	case DRM_FORMAT_NV61:
		pixel_format = V4L2_PIX_FMT_NV61M;
		multi_sample_pixels = true;
		break;

	case DRM_FORMAT_YUV420:
		pixel_format = V4L2_PIX_FMT_YUV420M;
		multi_sample_pixels = true;
		break;

	case DRM_FORMAT_YVU420:
		pixel_format = V4L2_PIX_FMT_YVU420M;
		multi_sample_pixels = true;
		break;

	case DRM_FORMAT_YUV422:
		pixel_format = V4L2_PIX_FMT_YUV422M;
		multi_sample_pixels = true;
		break;

	case DRM_FORMAT_YVU422:
		pixel_format = V4L2_PIX_FMT_YVU422M;
		multi_sample_pixels = true;
		break;

	case DRM_FORMAT_YUV444:
		pixel_format = V4L2_PIX_FMT_YUV444M;
		multi_sample_pixels = true;
		break;

	case DRM_FORMAT_YVU444:
		pixel_format = V4L2_PIX_FMT_YVU444M;
		multi_sample_pixels = true;
		break;

	default:
		weston_log("Unsupported DMABUF buffer format\n");
		return -1;
	}

	vs->width = buffer->width = dmabuf->attributes.width;
	vs->height = buffer->height = dmabuf->attributes.height;
	vs->pixel_format = pixel_format;
	vs->multi_sample_pixels = multi_sample_pixels;
	vs->num_planes = dmabuf->attributes.n_planes;
	for (i = 0; i < dmabuf->attributes.n_planes; i++) {
		if ((vs->planes[i].dmafd = dup(dmabuf->attributes.fd[i])) == -1)
			goto err;
		vs->planes[i].stride = dmabuf->attributes.stride[i];
		vs->planes[i].length = vs->planes[i].stride *
				v4l2_renderer_plane_height(i, vs->height, vs->pixel_format);
	}

	DBG("%s: %dx%d buffer attached (dmabuf=%d, stride=%d).\n", __func__,
		dmabuf->attributes.width, dmabuf->attributes.height, dmabuf->attributes.fd[0], dmabuf->attributes.stride);

	return 0;

err:
	v4l2_release_dmabuf(vs);
	return -1;
}

static int
attach_wl_kms_buffer(struct v4l2_surface_state *vs, struct weston_buffer *buffer,
		struct wl_kms_buffer *kbuf)
{
	unsigned int pixel_format;
	int i;
	bool multi_sample_pixels = false;

	switch (kbuf->format) {
	case WL_KMS_FORMAT_XRGB8888:
		pixel_format = V4L2_PIX_FMT_XBGR32;
		break;

	case WL_KMS_FORMAT_ARGB8888:
		pixel_format = V4L2_PIX_FMT_ABGR32;
		break;

	case WL_KMS_FORMAT_XBGR8888:
		pixel_format = V4L2_PIX_FMT_XRGB32;
		break;

	case WL_KMS_FORMAT_ABGR8888:
		pixel_format = V4L2_PIX_FMT_ARGB32;
		break;

	case WL_KMS_FORMAT_RGB888:
		pixel_format = V4L2_PIX_FMT_RGB24;
		break;

	case WL_KMS_FORMAT_BGR888:
		pixel_format = V4L2_PIX_FMT_BGR24;
		break;

	case WL_KMS_FORMAT_RGB565:
		pixel_format = V4L2_PIX_FMT_RGB565;
		break;

	case WL_KMS_FORMAT_RGB332:
		pixel_format = V4L2_PIX_FMT_RGB332;
		break;

	case WL_KMS_FORMAT_YUYV:
		pixel_format = V4L2_PIX_FMT_YUYV;
		multi_sample_pixels = true;
		break;

	case WL_KMS_FORMAT_YVYU:
		pixel_format = V4L2_PIX_FMT_YVYU;
		multi_sample_pixels = true;
		break;

	case WL_KMS_FORMAT_UYVY:
		pixel_format = V4L2_PIX_FMT_UYVY;
		multi_sample_pixels = true;
		break;

	case WL_KMS_FORMAT_NV12:
		pixel_format = V4L2_PIX_FMT_NV12M;
		multi_sample_pixels = true;
		break;

	case WL_KMS_FORMAT_NV16:
		pixel_format = V4L2_PIX_FMT_NV16M;
		multi_sample_pixels = true;
		break;

	case WL_KMS_FORMAT_NV21:
		pixel_format = V4L2_PIX_FMT_NV21M;
		multi_sample_pixels = true;
		break;

	case WL_KMS_FORMAT_NV61:
		pixel_format = V4L2_PIX_FMT_NV61M;
		multi_sample_pixels = true;
		break;

	case WL_KMS_FORMAT_YUV420:
		pixel_format = V4L2_PIX_FMT_YUV420M;
		multi_sample_pixels = true;
		break;

	default:
		weston_log("Unsupported DMABUF buffer format\n");
		return -1;
	}

	vs->width = buffer->width = kbuf->width;
	vs->height = buffer->height = kbuf->height;
	vs->pixel_format = pixel_format;
	vs->multi_sample_pixels = multi_sample_pixels;
	vs->num_planes = kbuf->num_planes;
	for (i = 0; i < kbuf->num_planes; i++) {
		if ((vs->planes[i].dmafd = dup(kbuf->planes[i].fd)) == -1)
			goto err;
		vs->planes[i].stride = kbuf->planes[i].stride;
		vs->planes[i].length = vs->planes[i].stride *
				v4l2_renderer_plane_height(i, vs->height, vs->pixel_format);
	}

	DBG("%s: %dx%d buffer attached (dmabuf=%d, stride=%d).\n", __func__, kbuf->width, kbuf->height, kbuf->fd, kbuf->stride);

	return 0;

err:
	v4l2_release_dmabuf(vs);
	return -1;
}

static int
v4l2_renderer_attach_dmabuf(struct v4l2_surface_state *vs, struct weston_buffer *buffer)
{
	struct linux_dmabuf_buffer *dmabuf;
	struct wl_kms_buffer *kbuf;

	buffer->legacy_buffer = (struct wl_buffer *)buffer->resource;

	v4l2_release_dmabuf(vs);
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

	if (device_interface->attach_buffer(vs) == -1) {
		v4l2_release_dmabuf(vs);
		return -1;
	}

	return 0;
}

static void
v4l2_renderer_attach(struct weston_surface *es, struct weston_buffer *buffer)
{
	struct v4l2_surface_state *vs = get_surface_state(es);
	struct wl_shm_buffer *shm_buffer;
	int ret;

	if (!vs)
		return;

	// refer the given weston_buffer. if there's an existing reference,
	// release it first if not the same. if the buffer is the new one,
	// increment the refrence counter. all done in weston_buffer_reference().
	weston_buffer_reference(&vs->buffer_ref, buffer);

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
	}

#ifdef V4L2_GL_FALLBACK_ENABLED
	if (vs->renderer->gl_fallback) {
		if (vs->renderer->defer_attach) {
			if (!vs->notify_attach)
				v4l2_gl_attach(es, NULL);
			vs->notify_attach = true;
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

	// TODO: Release any resources associated to the surface here.

	v4l2_release_dmabuf(vs);
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
	vs->notify_attach = false;
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

	if (snprintf(path, sizeof(path), "v4l2-%s-device.so", device_name) < 0) {
		weston_log("%s: fail to load device module: device=%s\n", __func__, device_name);
		return;
	}

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
	/* Reject all flags this renderer isn't supported. */
	if (dmabuf->attributes.flags)
		return false;

#ifdef V4L2_GL_FALLBACK_ENABLED
	struct v4l2_renderer *renderer = get_renderer(ec);
	if (renderer->gl_fallback) {
		return v4l2_gl_import_dmabuf(ec, dmabuf);
	}
#endif

	return device_interface->check_format(dmabuf->attributes.format,
					      dmabuf->attributes.n_planes);
}

static int
v4l2_renderer_init(struct weston_compositor *ec, struct v4l2_renderer_config *config, int drm_fd, char *drm_fn)
{
	struct v4l2_renderer *renderer;
	char *device;
	char *device_name = NULL;
	static struct media_device_info info;

	if (!drm_fn)
		return -1;

	renderer = calloc(1, sizeof *renderer);
	if (renderer == NULL)
		return -1;

	renderer->wl_kms = wayland_kms_init(ec->wl_display, NULL, drm_fn, drm_fd);

	/* Get V4L2 media controller device to use */
	device = config->device;
#ifdef V4L2_GL_FALLBACK_ENABLED
	renderer->gl_fallback = config->gl_fallback;
	renderer->defer_attach = config->defer_attach;
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

	weston_log("Media device info:\n"
		   "\tdriver		%s\n"
		   "\tmodel		%s\n"
		   "\tserial		%s\n"
		   "\tbus info		%s\n"
		   "\tmedia version	%u.%u.%u\n"
		   "\thw revision	0x%x\n"
		   "\tdriver version	%u\n",
		   info.driver, info.model, info.serial, info.bus_info,
		   (info.media_version >> 16u) & 0xff,
		   (info.media_version >> 8u) & 0xff, info.media_version,
		   info.hw_revision, info.driver_version);

	/* Get device module to use */
	if (config->device_module)
		device_name = strdup(config->device_module);
	else
		device_name = v4l2_get_cname(info.bus_info);
	v4l2_load_device_module(device_name);
	if (device_name)
		free(device_name);
	if (!device_interface)
		goto error;

	renderer->device = device_interface->init(renderer->media_fd, &info, &config->backend);
	if (!renderer->device)
		goto error;

	weston_log("V4L2 media controller device initialized.\n");

	if (kms_create(drm_fd, &renderer->kms))
		goto error;

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
	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_NV12);
	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_YUV420);

	wl_signal_init(&renderer->destroy_signal);

	return 0;

error:
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
		renderer->gl_fallback = false;
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

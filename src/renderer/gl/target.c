/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What the frame is drawn into: the window's image at App::extent, and the
 * scene's targets at the scene's own size. Every one of them is drawn with
 * row 0 at the top, as Vulkan's images are, by never flipping; the window's
 * is flipped once, by the blit onto the window's own framebuffer at the end
 * of the frame (frame.c). DESIGN *One shader source*.
 *
 * GL deletes a name that is still in use only once nothing submitted names
 * it, so nothing here drains before replacing what a frame in flight drew
 * into -- the drains of the Vulkan renderer's scene_set_size() have no
 * counterpart.
 */

#include "core/app.h"

#include "renderer/gl/gl_internal.h"

static void destroy_target(GlTarget *t)
{
	if (t->fbo)
		gl.DeleteFramebuffers(1, &t->fbo);
	if (t->color && t->sampled)
		gl.DeleteTextures(1, &t->color);
	else if (t->color)
		gl.DeleteRenderbuffers(1, &t->color);
	if (t->depth)
		gl.DeleteRenderbuffers(1, &t->depth);
	*t = (GlTarget){ 0, 0, 0, false };
}

/* A framebuffer of `w` x `h` with a colour image -- a texture when it is
 * sampled afterwards, a renderbuffer when it is only resolved -- and, when
 * `depth`, a depth renderbuffer. `samples` above one makes both
 * multisampled. False leaves a part-built target for destroy_target(). */
static bool make_target(GlTarget *t, int w, int h, int samples,
						bool sampled, bool depth)
{
	gl_clear_errors();
	gl.CreateFramebuffers(1, &t->fbo);
	t->sampled = sampled;
	if (sampled) {
		gl.CreateTextures(GL_TEXTURE_2D, 1, &t->color);
		gl.TextureStorage2D(t->color, 1, GL_COLOR_FORMAT, w, h);
		gl.NamedFramebufferTexture(t->fbo, GL_COLOR_ATTACHMENT0, t->color, 0);
	} else {
		gl.CreateRenderbuffers(1, &t->color);
		if (samples > 1)
			gl.NamedRenderbufferStorageMultisample(t->color, samples,
												   GL_COLOR_FORMAT, w, h);
		else
			gl.NamedRenderbufferStorage(t->color, GL_COLOR_FORMAT, w, h);
		gl.NamedFramebufferRenderbuffer(t->fbo, GL_COLOR_ATTACHMENT0,
										GL_RENDERBUFFER, t->color);
	}
	if (depth) {
		gl.CreateRenderbuffers(1, &t->depth);
		if (samples > 1)
			gl.NamedRenderbufferStorageMultisample(t->depth, samples,
												   GL_DEPTH_FORMAT, w, h);
		else
			gl.NamedRenderbufferStorage(t->depth, GL_DEPTH_FORMAT, w, h);
		gl.NamedFramebufferRenderbuffer(t->fbo, GL_DEPTH_ATTACHMENT,
										GL_RENDERBUFFER, t->depth);
	}
	/* An allocation the driver could not make is a GL_OUT_OF_MEMORY error
	 * and an incomplete framebuffer, either of which is a refusal here. */
	const GLenum err = gl.GetError();
	return err == GL_NO_ERROR &&
		   gl.CheckNamedFramebufferStatus(t->fbo, GL_DRAW_FRAMEBUFFER) ==
			   GL_FRAMEBUFFER_COMPLETE;
}

/* ---- the window's image -------------------------------------------------- */

/* The window's image at the size the frame policy settled, with the depth a
 * scene drawn straight into it may use; and the swap interval vsync asks
 * for, taken here because this is the rebuild the loop asks for when it
 * changes, as Vulkan's swapchain is. The frame gives the surface its
 * interval (gl_choose_interval()). */
FnlResult gl_window_image(App *a)
{
	/* The ceiling the scale is resolved against, published every build, as
	 * the Vulkan renderer publishes the surface's maxImageExtent. */
	GLint rb = 0, vp[2] = { 0, 0 };
	gl.GetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &rb);
	gl.GetIntegerv(GL_MAX_VIEWPORT_DIMS, vp);
	const int maxw = rb < vp[0] ? rb : vp[0];
	const int maxh = rb < vp[1] ? rb : vp[1];
	fnl_atomic_store_u32(&a->max_extent_w, (uint32_t)(maxw > 0 ? maxw : 0));
	fnl_atomic_store_u32(&a->max_extent_h, (uint32_t)(maxh > 0 ? maxh : 0));

	const Extent e = app_buffer_extent(a, (Extent){ 1, 1 },
									   (Extent){ maxw, maxh });
	if (e.w != a->extent.w || e.h != a->extent.h)
		GLS(a)->resized_at = now_seconds();
	a->extent = e;
	/* Before the interval, which a surface made again has lost: opaque with
	 * no grab ring, as Vulkan's composite alpha is (DESIGN *Composite
	 * alpha*). */
	FNL_TRY(gl_bridge_resize(a, e.w, e.h, a->draw_margin == 0));

	destroy_target(&GLS(a)->window);
	if (!make_target(&GLS(a)->window, e.w, e.h, 1, false, true)) {
		destroy_target(&GLS(a)->window);
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
						"could not make the window's %dx%d image", e.w, e.h);
	}

	GLS(a)->interval_asked = fnl_atomic_load_bool(&a->vsync) ? 1 : 0;
	fnl_atomic_store_u32(&a->present_mode,
						 (uint32_t)GLS(a)->interval_asked);
	return FNL_OK;
}

/* ---- the scene's targets ------------------------------------------------- */

static void destroy_scene_target(App *a)
{
	destroy_target(&GLS(a)->scene);
	destroy_target(&GLS(a)->scene_ms);
	destroy_target(&GLS(a)->mlaa);
	GLS(a)->scene_ms_refused = false;
	GLS(a)->mlaa_refused = false;
	a->scene_extent = (Extent){ 0, 0 };
}

/* As Vulkan's scene_ensure_aa(): only what a mode asks for, once it does,
 * and a refusal is remembered so a device with no room is asked once per
 * target rather than once per frame. */
bool gl_scene_ensure_aa(App *a, bool want_ms, bool want_mlaa)
{
	if (!GLS(a)->scene.fbo)
		return false;
	const int w = a->scene_extent.w, h = a->scene_extent.h;
	const int samples = (int)fnl_atomic_load_u32(&a->msaa_samples);
	bool built = false;

	if (want_ms && samples > 1 && !GLS(a)->scene_ms.fbo &&
		!GLS(a)->scene_ms_refused) {
		if (!FNL_FAULT("scene-ms") &&
			make_target(&GLS(a)->scene_ms, w, h, samples, false, true)) {
			built = true;
		} else {
			destroy_target(&GLS(a)->scene_ms);
			GLS(a)->scene_ms_refused = true;
			fnl_log(FNL_LOG_WARN,
					"no room for a %dx%d %dx multisampled scene target; "
					"FSAA is off at this size", w, h, samples);
		}
	}
	if (want_mlaa && !GLS(a)->mlaa.fbo && !GLS(a)->mlaa_refused) {
		if (!FNL_FAULT("scene-mlaa") &&
			make_target(&GLS(a)->mlaa, w, h, 1, true, false)) {
			built = true;
		} else {
			destroy_target(&GLS(a)->mlaa);
			GLS(a)->mlaa_refused = true;
			fnl_log(FNL_LOG_WARN,
					"no room for a %dx%d filtered scene image; "
					"MLAA is off at this size", w, h);
		}
	}
	return built;
}

/* As Vulkan's scene_set_size(): a refusal leaves no target and says so, and
 * the loop draws the scene at the window's own resolution. */
void gl_scene_set_size(App *a, int w, int h)
{
	if (!GLS(a)->loaded)
		return;
	if ((w > 0 ? w : 0) == a->scene_extent.w &&
		(h > 0 ? h : 0) == a->scene_extent.h)
		return;

	destroy_scene_target(a);
	if (w <= 0 || h <= 0)
		return;
	if (FNL_FAULT("scene-target") ||
		!make_target(&GLS(a)->scene, w, h, 1, true, true)) {
		destroy_scene_target(a);
		fnl_log(FNL_LOG_WARN,
				"no room for a %dx%d scene target; drawing the scene at the "
				"window's own resolution", w, h);
		return;
	}
	a->scene_extent = (Extent){ w, h };
}

void gl_destroy_targets(App *a)
{
	destroy_scene_target(a);
	destroy_target(&GLS(a)->window);
}

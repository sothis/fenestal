/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What the frame is drawn into: the layer's drawables and the window's
 * depth at App::extent, and the scene's targets at the scene's own size.
 *
 * Nothing replaced here waits for the queue: a command buffer keeps every
 * texture it names alive until it has run, so the old image is let go and
 * the frame still drawing from it is undisturbed -- the drain Direct3D 12's
 * target.c makes, and Vulkan's scene_set_size(), is Metal's to do.
 */

#include "core/app.h"

#include "renderer/metal/metal_internal.h"

/* A 2D image in the GPU's own memory: a colour the scene renders into and
 * the window samples, or a depth nothing samples. `samples` above 1 makes it
 * multisampled. Nil on a refusal. */
static id<MTLTexture> make_image(App *a, MTLPixelFormat fmt, int w, int h,
								 int samples, bool sampled)
{
	MTLTextureDescriptor *d = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:fmt
									 width:(NSUInteger)w
									height:(NSUInteger)h
								 mipmapped:NO];
	if (samples > 1) {
		d.textureType = MTLTextureType2DMultisample;
		d.sampleCount = (NSUInteger)samples;
	}
	d.storageMode = MTLStorageModePrivate;
	d.usage = MTLTextureUsageRenderTarget |
			  (sampled ? MTLTextureUsageShaderRead : 0);
	return [MT(a)->device newTextureWithDescriptor:d];
}

/* ---- the window's images ------------------------------------------------- */

void mt_release_window_images(App *a)
{
	MT(a)->window_depth = nil;
}

/* The layer's drawables at the size the frame policy settled, and the
 * window's depth beside them; and the present mode vsync asks for, taken
 * here because this is the rebuild the loop asks for when it changes, as
 * Vulkan's swapchain is.
 *
 * The drawable size is the layer's to keep, set on the render thread inside
 * a transaction of its own with the implicit animation off: the main
 * thread's transaction would otherwise carry it at its own commit, and
 * animate it. A drawable already handed out keeps its own size. */
FnlResult mt_window_images(App *a)
{
	struct RendererState *g = MT(a);
	fnl_atomic_store_u32(&a->max_extent_w, MT_MAX_EXTENT);
	fnl_atomic_store_u32(&a->max_extent_h, MT_MAX_EXTENT);
	const Extent e = app_buffer_extent(a, (Extent){ 1, 1 },
									   (Extent){ MT_MAX_EXTENT,
												 MT_MAX_EXTENT });
	const bool vsync = fnl_atomic_load_bool(&a->vsync);
	const bool resized = e.w != a->extent.w || e.h != a->extent.h ||
						 !g->window_depth;
	if (resized || vsync != g->display_sync) {
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		if (resized)
			g->layer.drawableSize = CGSizeMake(e.w, e.h);
		g->layer.displaySyncEnabled = vsync;
		[CATransaction commit];
	}
	if (resized) {
		g->window_depth = make_image(a, MT_DEPTH_FORMAT, e.w, e.h, 1, false);
		if (!g->window_depth) {
			if (!mt_device_lost(a))
				a->device_lost = true;
			return fnl_fail(FNL_ERR_OUT_OF_MEMORY, "could not make the "
							"window's %dx%d depth", e.w, e.h);
		}
		a->extent = e;
	}
	g->display_sync = vsync;
	fnl_atomic_store_u32(&a->present_mode, vsync ? 1u : 0u);
	return FNL_OK;
}

/* ---- the scene's targets ------------------------------------------------- */

static void destroy_scene_target(App *a)
{
	struct RendererState *g = MT(a);
	g->scene = (MtTarget){ nil, nil };
	g->scene_ms = (MtTarget){ nil, nil };
	g->mlaa = nil;
	g->scene_ms_refused = false;
	g->mlaa_refused = false;
	a->scene_extent = (Extent){ 0, 0 };
}

/* As Vulkan's scene_ensure_aa(): only what a mode asks for, once it does,
 * and a refusal remembered. The multisampled pair is drawn into and
 * resolved from, never sampled; MLAA's output is sampled by the window. */
bool mt_scene_ensure_aa(App *a, bool want_ms, bool want_mlaa)
{
	struct RendererState *g = MT(a);
	if (!g->scene.color)
		return false;
	const int w = a->scene_extent.w, h = a->scene_extent.h;
	bool built = false;
	if (want_ms && g->samples > 1 && !g->scene_ms.color &&
		!g->scene_ms_refused) {
		if (!FNL_FAULT("scene-ms")) {
			g->scene_ms.color = make_image(a, MT_COLOR_FORMAT, w, h,
										   g->samples, false);
			g->scene_ms.depth = make_image(a, MT_DEPTH_FORMAT, w, h,
										   g->samples, false);
		}
		if (g->scene_ms.color && g->scene_ms.depth) {
			built = true;
		} else {
			g->scene_ms = (MtTarget){ nil, nil };
			g->scene_ms_refused = true;
			fnl_log(FNL_LOG_WARN,
					"no room for a %dx%d %dx multisampled scene target; "
					"FSAA is off at this size", w, h, g->samples);
		}
	}
	if (want_mlaa && !g->mlaa && !g->mlaa_refused) {
		if (!FNL_FAULT("scene-mlaa"))
			g->mlaa = make_image(a, MT_COLOR_FORMAT, w, h, 1, true);
		if (g->mlaa) {
			built = true;
		} else {
			g->mlaa_refused = true;
			fnl_log(FNL_LOG_WARN,
					"no room for a %dx%d filtered scene image; "
					"MLAA is off at this size", w, h);
		}
	}
	return built;
}

/* As Vulkan's scene_set_size(): a refusal leaves no target and says so,
 * and the loop draws the scene at the window's own resolution. */
void mt_scene_set_size(App *a, int w, int h)
{
	struct RendererState *g = MT(a);
	if (!g->device)
		return;
	if ((w > 0 ? w : 0) == a->scene_extent.w &&
		(h > 0 ? h : 0) == a->scene_extent.h)
		return;
	destroy_scene_target(a);
	if (w <= 0 || h <= 0)
		return;
	/* The descriptors are autoreleased, and the render thread has no
	 * pool of its own between frames. */
	if (!FNL_FAULT("scene-target")) {
		@autoreleasepool {
			g->scene.color = make_image(a, MT_COLOR_FORMAT, w, h, 1, true);
			g->scene.depth = make_image(a, MT_DEPTH_FORMAT, w, h, 1, false);
		}
	}
	if (!g->scene.color || !g->scene.depth) {
		destroy_scene_target(a);
		fnl_log(FNL_LOG_WARN,
				"no room for a %dx%d scene target; drawing the scene at the "
				"window's own resolution", w, h);
		return;
	}
	a->scene_extent = (Extent){ w, h };
}

void mt_destroy_targets(App *a)
{
	destroy_scene_target(a);
}

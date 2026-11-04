/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The OpenGL device: the bridge's display and context, what the driver is,
 * the one pair of functions that builds and tears down everything below the
 * display for startup and loss recovery alike, the seam's table and the
 * public GL interop.
 *
 * Where the Vulkan renderer has an instance, a device and a swapchain, this
 * one has the bridge's display, its context, and the window's image the
 * frame is drawn into (target.c). The seam is the same; DESIGN *One shader
 * source* has the conventions GL is held to.
 */

#include "core/app.h"

#include "renderer/gl/gl_internal.h"

/* ---- loss ---------------------------------------------------------------- */

/* Whether a reset took the context. Only a robust context is told (egl.c
 * asks for one); a reset of any other is GL's undefined behaviour and is
 * not looked for. */
bool gl_reset_lost(App *a)
{
	if (!GLS(a)->robust || !gl.GetGraphicsResetStatus)
		return false;
	if (gl.GetGraphicsResetStatus() == GL_NO_ERROR)
		return false;
	a->device_lost = true;
	return true;
}

#ifndef NDEBUG
/* A Debug tree's context is a debug context (egl.c), and this is where what
 * it reports goes: the log, as a validation layer's messages do on Vulkan.
 * Notifications are the driver describing itself, and are left out. */
static void APIENTRY debug_message(GLenum source, GLenum type, GLuint id,
								   GLenum severity, GLsizei length,
								   const GLchar *message, const void *ud)
{
	(void)source; (void)id; (void)length;
	const App *a = ud;
	if (severity == GL_DEBUG_SEVERITY_NOTIFICATION ||
		(a && a->renderer_state && GLS(a)->debug_quiet))
		return;
	fnl_log(type == GL_DEBUG_TYPE_ERROR ? FNL_LOG_ERROR : FNL_LOG_WARN,
			"GL: %s", message);
}
#endif

/* ---- the device ---------------------------------------------------------- */

/* What FSAA multisamples at: the best of 4x and 2x the driver offers for a
 * renderbuffer, which is what the multisampled pair is made of (target.c);
 * 1 when it offers neither. */
static void choose_samples(App *a)
{
	GLint max = 0;
	gl.GetIntegerv(GL_MAX_SAMPLES, &max);
	uint32_t s = max >= 4 ? 4u : max >= 2 ? 2u : 1u;
	if (FNL_FAULT("no-msaa"))
		s = 1u;
	fnl_atomic_store_u32(&a->msaa_samples, s);
}

/* Names in GL_RENDERER the software rasterisers give themselves. Their swap
 * waits for the frame to be drawn, so its block is work, not the display's
 * pace. */
bool gl_renderer_is_software(const char *r)
{
	static const char *const SOFT[] = { "llvmpipe", "softpipe", "swrast",
										"Software Rasterizer",
										"Apple Software Renderer" };
	for (size_t i = 0; i < sizeof SOFT / sizeof SOFT[0]; i++)
		if (strstr(r, SOFT[i]))
			return true;
	return false;
}

/* Everything below the display, in dependency order, current on the calling
 * thread when it returns. As on Vulkan: no unwinding on failure, since
 * destroy_device_objects() is safe on any prefix -- every name is 0 until
 * the call that made it succeeded. */
static FnlResult create_device_objects(App *a)
{
	FNL_TRY(gl_bridge_context(a));
	FNL_TRY(gl_load(a));
	GLS(a)->loaded = true;
	fnl_atomic_store_u32(&a->gl_version, gl_profile41 ? 41u : 45u);

	const char *renderer = (const char *)gl.GetString(GL_RENDERER);
	const char *version  = (const char *)gl.GetString(GL_VERSION);
	if (!renderer)
		renderer = "(unnamed)";
	fnl_mutex_lock(&a->lock);
	snprintf(a->gpu_name, sizeof a->gpu_name, "%s", renderer);
	fnl_mutex_unlock(&a->lock);
	GLS(a)->software = gl_renderer_is_software(renderer);
	/* The GPUs the display can be opened on (egl.c), the one in use named
	 * as GL names it; where EGL lists none, or cannot say which the display
	 * is on, the list is that one alone. Printed as every renderer prints
	 * its inventory. */
	static FNL_THREAD_LOCAL GpuEntry list[GPU_LIST_MAX];
	int cur = -1;
	int n = gl_bridge_gpus(a, list, &cur);
	if (n == 0 || cur < 0) {
		n = 1;
		cur = 0;
	}
	snprintf(list[cur].name, sizeof list[cur].name, "%s", renderer);
	list[cur].kind = GLS(a)->software ? FNL_GPU_KIND_SOFTWARE
									  : FNL_GPU_KIND_UNKNOWN;
	list[cur].renderer = FNL_RENDERER_OPENGL;
	for (int i = 0; i < n; i++)
		app_gpu_inventory(a, i, list[i].name,
						  app_gpu_kind_word(list[i].kind));
	fnl_log(FNL_LOG_INFO, "GPU in use: %s (OpenGL %s)%s", renderer,
			version ? version : "?",
			gl_profile41 ? ", drawn through the 4.1 profile" : "");
	app_gpu_list_publish(a, list, n, cur);

#ifndef NDEBUG
	/* 4.3's, or KHR_debug's: a 4.1 context may have neither. */
	if (gl.DebugMessageCallback) {
		gl.Enable(GL_DEBUG_OUTPUT);
		gl.Enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		gl.DebugMessageCallback(debug_message, a);
	}
#endif
	/* The debug output's control: an enum no capability has, which a debug
	 * context reports as GL_INVALID_ENUM through debug_message(). Its flag
	 * is cleared again, so that no step reading glGetError() as its own
	 * result sees it. */
	if (FNL_SELFTEST("gl")) {
		gl.Enable(0);
		gl_clear_errors();
	}

	/* Depth from 0 to 1, Vulkan's. The origin stays GL's own: every image
	 * here is drawn with row 0 at the top by not flipping at all, and the
	 * window's is flipped once at the blit (DESIGN *One shader source*).
	 * Under 4.1 a stand-in that does nothing: the gl41 translation maps
	 * the depth itself. */
	gl.ClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

	choose_samples(a);
	a->srgb_surface = true;      /* every colour image is GL_SRGB8_ALPHA8 */
	/* The palette is converted for that blend space; until then every entry
	 * is zero, and the titlebar draws as transparent black. */
	app_palette_init(a);

	FNL_TRY(gl_create_overlay(a));
	FNL_TRY(gl_window_image(a));
	gl.GenQueries(2 * MAX_FRAMES_IN_FLIGHT, &GLS(a)->ts_query[0][0]);

	/* The interval is set with the surface current, which it is from here;
	 * gl_window_image() mirrors it into App::present_mode at every
	 * rebuild. */
	return FNL_OK;
}

/* What the deletes would have reset, where they cannot run: no context is
 * current, and destroying it frees every name and fence of its own. The
 * records go all the same -- a fence of a destroyed context waited on, or a
 * scene target taken for the new one's, is the next frame's fault. The
 * bridge's half is kept, since it outlives the context. */
static void forget_objects(App *a)
{
	struct RendererState *g = GLS(a);
	/* Client memory, the 4.1 profile's arenas, and the bridge's own of the
	 * window's way out are not the context's to free. */
	free(g->text_block);
	free(g->ovl_block);
	gl_bridge_scan_forget(a);
	const struct RendererState keep = *g;
	memset(g, 0, sizeof *g);
	g->bridge_display = keep.bridge_display;
	g->bridge_config  = keep.bridge_config;
	g->bridge_surface = keep.bridge_surface;
	g->bridge_context = keep.bridge_context;
	g->bridge_native  = keep.bridge_native;
	g->native_w       = keep.native_w;
	g->native_h       = keep.native_h;
	g->bridge_window  = keep.bridge_window;
	g->display_gone   = keep.display_gone;
	g->devs           = keep.devs;
	g->dev_cur        = keep.dev_cur;
	g->dev_listed     = keep.dev_listed;
	g->opaque         = keep.opaque;
	g->can_opaque     = keep.can_opaque;
	g->robust         = keep.robust;
	g->interval       = keep.interval;
	g->interval_asked = keep.interval_asked;
	g->resized_at     = keep.resized_at;
	g->software       = keep.software;
	for (int i = 0; i < OVERLAY_MAX_IMAGES; i++) {
		a->text_map[i] = NULL;
		a->ovl_map[i]  = NULL;
	}
	a->font_si = -1;
	a->scene_extent = (Extent){ 0, 0 };
}

/* The reverse, and safe on a lost context: deleting names on one is
 * harmless, and the context is destroyed regardless. `save_cache` has
 * nothing to save here: GL keeps no pipeline cache of ours. */
static void destroy_device_objects(App *a, bool save_cache)
{
	(void)save_cache;
	/* NVML, where the panel started it: it holds NVIDIA's driver, which a
	 * move to another kernel driver must find let go. */
	gl_memory_release();
	/* Not a GL name: freed whatever became of the context. */
	free(GLS(a)->readback);
	GLS(a)->readback = NULL;
	GLS(a)->readback_bytes = 0;
	if (!GLS(a)->bridge_context)
		return;

	if (GLS(a)->loaded) {
		if (!a->device_lost)
			gl.Finish();
		gl_bridge_scan_close(a);
		gl_destroy_targets(a);
		gl_destroy_overlay(a);
		gl.DeleteQueries(2 * MAX_FRAMES_IN_FLIGHT, &GLS(a)->ts_query[0][0]);
		memset(GLS(a)->ts_query, 0, sizeof GLS(a)->ts_query);
		if (GLS(a)->pack_buf)
			gl.DeleteBuffers(1, &GLS(a)->pack_buf);
		GLS(a)->pack_buf = 0;
		GLS(a)->pack_bytes = 0;
		GLS(a)->pack_pending = false;
		memset(GLS(a)->ts_unread, 0, sizeof GLS(a)->ts_unread);
	} else {
		forget_objects(a);
	}
	GLS(a)->loaded = false;
	gl_bridge_context_close(a);

	/* Per-frame bookkeeping that named the objects just destroyed. */
	a->frame   = 0;
	a->resized = false;
	/* The scene image went too, and a recovery onto the same window size
	 * rebuilds nothing else, so the loop must be told nothing is asked for. */
	a->draw_scene_w = a->draw_scene_h = SCENE_NATIVE;
}

/* Rebuild everything below the display, as Vulkan's recover_from_loss() does
 * and with its rule: every way out leaves a loss flag raised unless the
 * device is up. DESIGN *Device and surface loss*. */
static void recover_from_loss(App *a)
{
	const bool remake = a->surface_remake;
	a->surface_remake = false;
	/* A move to another GPU was said by the loop (gpu_switch_owed()), and
	 * has no driver reset behind it to wait out. */
	if (remake)
		fnl_log(FNL_LOG_INFO, "the surface is made again for the output's "
				"new mode");
	else if (!a->gpu_switching)
		fnl_log(FNL_LOG_ERROR, "%s lost -- rebuilding",
				a->surface_lost ? "surface" : "device");

	/* The gap's picture under the swapchain before it goes
	 * (FnlBackendVtbl::present_window_hold). */
	BACKEND(a)->present_window_hold(a);
	destroy_device_objects(a, false);

	/* A display terminated under the renderer (RendererState::display_gone)
	 * took its surface and every context not current with it: made again
	 * from the display up, as after a move. */
	const bool gone = GLS(a)->display_gone;
	if (gone)
		fnl_log(FNL_LOG_WARN, "the EGL display was terminated under the "
				"renderer; opened again, with a new context and surface");
	if (a->surface_lost || gone) {
		gl_bridge_surface_close(a);
		/* A move opens the display again on the GPU it moves to: a display
		 * is one device's (egl.c), and the window's native window is made
		 * again over it below. So does a recovery that finds none, after
		 * a move whose open failed and whose way back failed too. */
		if (a->gpu_switching || !GLS(a)->bridge_display || gone) {
			gl_bridge_close(a);
			GLS(a)->display_gone = false;
			if (gl_bridge_open(a) < 0)
				return;
		}
		/* A move presents into a window of its own, which a driver
		 * before has not claimed (FnlBackendVtbl::present_window_renew),
		 * between the old surface and the new as the other renderers'
		 * recoveries call it: on Wayland a surface that has shown one
		 * device's frames is not left to another's. */
		if (a->gpu_switching)
			BACKEND(a)->present_window_renew(a);
		if (FNL_FAULT("create-surface") || gl_bridge_surface(a) < 0)
			return;
		a->surface_lost = false;
		a->device_lost  = true;
	}

	if (!remake && !a->gpu_switching)
		fnl_sleep_seconds(0.2);

	if (FNL_FAULT("recover-objects") || create_device_objects(a) < 0)
		return;

	a->record_gen++;
	a->device_lost = false;
	fnl_log(FNL_LOG_INFO, "rebuilt on %s", a->gpu_name);
}

/* ---- the renderer seam ------------------------------------------------------
 *
 * This file's half of renderer.h. */

/* The seam's first entry, so the renderer's state is born here, zeroed: every
 * name 0 and every bridge handle null. A failure frees it again, since app.c
 * calls destroy_instance() only after this succeeded. */
static FnlResult gl_create_instance(App *a)
{
	FNL_ASSERT(!a->renderer_state);
	a->renderer_state = calloc(1, sizeof *a->renderer_state);
	if (!a->renderer_state)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
						"out of memory allocating the OpenGL renderer's "
						"state");
	FnlResult rc = gl_bridge_hold(a);
	if (rc == FNL_OK) {
		rc = gl_bridge_open(a);
		if (rc < 0) {
			gl_bridge_close(a);
			gl_bridge_let_go(a);
		}
	}
	if (rc < 0) {
		free(a->renderer_state);
		a->renderer_state = NULL;
	}
	return rc;
}

/* The seam's last entry, so the renderer's state dies here. */
static void gl_destroy_instance(App *a)
{
	if (!a->renderer_state)
		return;
	gl_bridge_close(a);
	gl_bridge_let_go(a);
	free(a->renderer_state);
	a->renderer_state = NULL;
}

/* A context that will not become current here is treated as lost: the
 * recovery makes a new one on this thread, and until then no GL call is made
 * here -- not even teardown's deletes, which destroying the context makes
 * unnecessary. */
static void gl_thread_enter(App *a)
{
	if (!gl_bridge_current(a, true)) {
		a->device_lost = true;
		GLS(a)->loaded = false;
	}
}

static void gl_thread_leave(App *a)
{
	(void)gl_bridge_current(a, false);
}

/* No rebuild of anything but the window's image: the window's own
 * framebuffer follows the window, or the size the bridge hands the native
 * window, and the image is what follows the size the frame policy settled.
 * False on a loss, or where the image or the bridge's resize fails; the
 * next turn asks again (swapchain_adopt()). */
static bool gl_recreate_swapchain(App *a)
{
	if (gl_reset_lost(a))
		return false;
	return gl_window_image(a) >= 0;
}

static bool gl_present_mode_matches(const App *a, bool vsync)
{
	return (int)fnl_atomic_load_u32(&a->present_mode) == (vsync ? 1 : 0);
}

/* Not on a lost context, unlike Metal's drain: GL promises no answer from
 * one -- a reset context's glFinish may not return on every driver -- and
 * a reset context runs nothing more that the caller's objects could be
 * read by. */
static void gl_wait_idle(App *a)
{
	if (GLS(a)->loaded && !a->device_lost)
		gl.Finish();
}

static bool gl_is_lost(const App *a)
{
	return a->device_lost || a->surface_lost;
}

/* As Vulkan's: a surface that names a display mode is made again through
 * the loss path. No EGL surface here names one -- KMS's bridge has none,
 * the backend's scanout buffers following the mode themselves -- so
 * elsewhere the flag simply makes the surface again. */
static void gl_remake_surface(App *a)
{
	/* No surface to name a mode: the backend's own buffers follow it. */
	if (a->surface_lost || gl_bridge_surfaceless(a))
		return;
	a->surface_lost = true;
	a->surface_remake = true;
}

/* The bridge's answer: Cocoa's occlusion on macOS, none under EGL. */
static bool gl_occluded(App *a)
{
	return gl_bridge_occluded(a);
}

static int gl_max_samples(const App *a)
{
	return (int)fnl_atomic_load_u32(&a->msaa_samples);
}

static const char *gl_device_name(const App *app)
{
	/* A per-thread snapshot under App::lock, as Vulkan's is: a recovery on
	 * the render thread rewrites the buffer. The const is cast away for the
	 * lock, as api_state.c does. */
	static FNL_THREAD_LOCAL char snap[GPU_NAME_MAX];
	App *a = (App *)app;

	fnl_mutex_lock(&a->lock);
	memcpy(snap, a->gpu_name, sizeof snap);
	fnl_mutex_unlock(&a->lock);
	snap[sizeof snap - 1] = '\0';
	return snap[0] ? snap : "(no device)";
}

/* The interval, said the way a person would; only the atomic crosses
 * threads, and a bridge that paces reads the same one. Where something
 * else paces, the swap waits for nothing and that pacing is the mode:
 * X11's under a compositor with vsync, KMS's flip with or without it. */
static const char *gl_present_mode_name(const App *a)
{
	const char *pacer = gl_bridge_pacer(a);
	if (pacer)
		return pacer;
	return fnl_atomic_load_u32(&a->present_mode) != 1u
			   ? "swap interval 0" : "swap interval 1 (vsync)";
}

static FnlResult gl_create_surface(App *a)
{
	return gl_bridge_surface(a);
}

static void gl_destroy_surface(App *a)
{
	gl_bridge_surface_close(a);
}

const FnlRendererVtbl fnl_renderer_gl = {
	.name                  = "OpenGL",
	.api                   = FNL_RENDERER_OPENGL,
	.key                   = "gl",
	.draws_overlay         = true,
	.offscreen_scene       = true,
	.create_instance       = gl_create_instance,
	.destroy_instance      = gl_destroy_instance,
	.create_surface        = gl_create_surface,
	.destroy_surface       = gl_destroy_surface,
	.list_gpus             = gl_bridge_list_gpus,
	/* A context is current on one thread at a time: these move it. */
	.thread_enter          = gl_thread_enter,
	.thread_leave          = gl_thread_leave,
	.create_device_objects = create_device_objects,
	.destroy_device_objects = destroy_device_objects,
	.recreate_swapchain    = gl_recreate_swapchain,
	.present_mode_matches  = gl_present_mode_matches,
	/* Nothing is retired: GL keeps a name it deleted alive until nothing
	 * submitted uses it. */
	.retire_tick           = renderer_noop,
	.scale_changed         = gl_text_set_scale,
	.set_scene_size        = gl_scene_set_size,
	.draw_frame            = gl_draw_frame,
	.wait_idle             = gl_wait_idle,
	.lost                  = gl_is_lost,
	.recover               = recover_from_loss,
	.remake_surface        = gl_remake_surface,
	.occluded              = gl_occluded,
	.max_samples           = gl_max_samples,
	.active_samples        = gl_active_samples,
	.gpu_time_poll         = gl_gpu_time_poll,
	.device_name           = gl_device_name,
	.gpu_memory            = gl_gpu_memory,
	.present_mode_name     = gl_present_mode_name,
	.overlay_flush_rects   = gl_overlay_flush_rects,
	.overlay_flush_shapes  = gl_overlay_flush_shapes,
	.overlay_scissor       = gl_overlay_scissor,
	.text_end              = gl_text_end,
	.overlay_cover         = gl_overlay_cover,
	.cover_release         = gl_cover_release,
};

/* ---- the public OpenGL interop ----------------------------------------------
 *
 * include/fenestal/fnl_gl.h: entry points, what each target is made of, and
 * where on_record draws.
 *
 * Each refuses an App that another renderer draws: a build may have several,
 * and every one's interop is exported. One any thread may call asks
 * renderer_any() and reads atomics alone: the render thread may free
 * App::renderer_state under it. */

FnlGlProc fnl_gl_proc(App *a, const char *name)
{
	if (!a || !name || renderer_any(a) != &fnl_renderer_gl)
		return NULL;
	/* A hold of its own: a move off OpenGL on the render thread may close
	 * the library under this one (egl_dyn.c). Only on a library the
	 * renderer holds open: one this thread opened would close again as it
	 * returned, and the entry with it. */
	if (!gl_bridge_hold_open(a))
		return NULL;
	const FnlGlProc p = gl_bridge_proc(name);
	gl_bridge_let_go(a);
	return p;
}

bool fnl_gl_scene_layout(App *a, FnlSceneTarget t, FnlGlSceneLayout *out)
{
	if (!a || !out || renderer_any(a) != &fnl_renderer_gl ||
		t < 0 || t >= FNL_TARGET_COUNT)
		return false;
	int samples = 1;
	if (t == FNL_TARGET_OFFSCREEN_MS) {
		samples = gl_max_samples(a);
		if (samples <= 1)
			return false;
	}
	*out = (FnlGlSceneLayout){
		.samples      = samples,
		.color_format = GL_COLOR_FORMAT,
		.depth_format = GL_DEPTH_FORMAT,
	};
	return true;
}

int fnl_gl_version(App *a)
{
	if (!a || renderer_any(a) != &fnl_renderer_gl)
		return 0;
	return (int)fnl_atomic_load_u32(&a->gl_version);
}

bool fnl_gl_scene_framebuffer(App *a, uint32_t *out)
{
	/* 0 outside on_record, and 0 is also the window's own framebuffer, which
	 * nothing of the caller's is drawn into: refused rather than handed
	 * back. The thread first, as fnl_vulkan_scene_cmd() tests it. */
	if (!a || !out || app_thread() != APP_THREAD_RENDER ||
		a->renderer != &fnl_renderer_gl ||
		!a->renderer_state || GLS(a)->scene_fbo_now == 0)
		return false;
	*out = GLS(a)->scene_fbo_now;
	return true;
}

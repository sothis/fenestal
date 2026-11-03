/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Metal device: choosing it, its queue, the one pair of functions that
 * builds and tears down everything below it for startup and loss recovery
 * alike, the seam's table and the public interop.
 *
 * Where the Vulkan renderer has an instance, a device and a swapchain, this
 * one has a device and the window's CAMetalLayer, whose drawables are the
 * swapchain's images (target.m, surface_cocoa.m). DESIGN *One shader source*
 * has the conventions the shaders keep; the MSL is spirv-cross's, compiled
 * here at run time, since only Xcode has Metal's offline compiler.
 */

#include "core/app.h"

#include "renderer/metal/metal_internal.h"

/* ---- loss ---------------------------------------------------------------- */

bool mt_cb_lost(App *a, id<MTLCommandBuffer> cb)
{
	if (cb.status != MTLCommandBufferStatusError)
		return false;
	const NSInteger code = cb.error.code;
	if (code == MTLCommandBufferErrorDeviceRemoved ||
		code == MTLCommandBufferErrorNotPermitted) {
		a->device_lost = true;
		return true;
	}
	/* Any other failure is that frame's, said once and drawn again. */
	fnl_log(FNL_LOG_WARN, "a frame's command buffer failed: %s",
			cb.error.localizedDescription.UTF8String ?: "(no reason)");
	return false;
}

/* Metal has no question to ask a device about itself: a GPU taken away --
 * an eGPU unplugged -- shows as a command buffer that failed with
 * MTLCommandBufferErrorDeviceRemoved, which mt_cb_lost() reads as each
 * frame slot comes round. So this is the flag that left. */
bool mt_device_lost(App *a)
{
	return a->device_lost;
}

void mt_wait_idle(App *a)
{
	struct RendererState *g = MT(a);
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		id<MTLCommandBuffer> cb = g->slot_cb[i];
		if (cb && cb.status < MTLCommandBufferStatusCompleted)
			[cb waitUntilCompleted];
		g->slot_cb[i] = nil;
	}
}

/* One stage, from MSL text: its own library, since spirv-cross names every
 * entry main0. Metal keeps what it has compiled in the system's shader
 * cache, so a second start costs a lookup. */
id<MTLFunction> mt_function(App *a, const char *what, const char *msl)
{
	MTLCompileOptions *o = [[MTLCompileOptions alloc] init];
	o.languageVersion = MTLLanguageVersion2_1;
	NSError *err = nil;
	id<MTLLibrary> lib = [MT(a)->device
		newLibraryWithSource:[NSString stringWithUTF8String:msl]
					 options:o
					   error:&err];
	if (!lib) {
		fnl_log(FNL_LOG_ERROR, "the %s shader did not compile: %s", what,
				err.localizedDescription.UTF8String ?: "(no log)");
		return nil;
	}
	return [lib newFunctionWithName:@"main0"];
}

/* ---- the device ---------------------------------------------------------- */

/* `d` as the list and fnl_gpu_kind_at() name it. Unified memory is the
 * integrated GPU's, and Apple silicon's; a removable one is an eGPU,
 * discrete by its nature. Every other field zero: the entry is published
 * whole, and a stale `via` is a ranking key no figure matches. */
static void metal_entry(id<MTLDevice> d, GpuEntry *e)
{
	*e = (GpuEntry){ 0 };
	snprintf(e->name, sizeof e->name, "%s", d.name.UTF8String ?: "(unnamed)");
	e->kind = d.removable ? FNL_GPU_KIND_DISCRETE
			: d.hasUnifiedMemory || d.lowPower ? FNL_GPU_KIND_INTEGRATED
											   : FNL_GPU_KIND_DISCRETE;
	e->renderer = FNL_RENDERER_METAL;
}

/* MTLCopyAllDevices() in its order, as choose_device() lists it: every
 * device draws into the window's layer, the one not driving its display by
 * a copy between GPUs. */
static int mt_list_gpus(App *a, GpuEntry *out, int max)
{
	(void)a;
	int n = 0;
	/* On a thread of the library's with no pool, and the names are
	 * autoreleased. */
	@autoreleasepool {
		for (id<MTLDevice> d in MTLCopyAllDevices())
			if (n < max)
				metal_entry(d, &out[n++]);
	}
	return n;
}

/* Every device Metal lists, as the Vulkan renderer lists its devices, and
 * the one to use: the name or the index the caller asked for
 * (FnlInitDesc::gpu_index, FNL_GPU), else the one the window's display is
 * driven by -- the layer's preferred device, which costs no copy between
 * GPUs -- else the system's default. */
static id<MTLDevice> choose_device(App *a)
{
	NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
	id<MTLDevice> pick = nil;
	int n = 0;
	/* The same list, for fnl_gpu_count() and the titlebar: entry i is
	 * gpu_index i + 1. */
	static FNL_THREAD_LOCAL GpuEntry list[GPU_LIST_MAX];
	for (id<MTLDevice> d in all) {
		const char *name = d.name.UTF8String ?: "(unnamed)";
		GpuEntry e;
		metal_entry(d, &e);
		if (n < GPU_LIST_MAX)
			list[n] = e;
		/* The kind as the list gives it, not lowPower's alone. */
		char kind[48];
		snprintf(kind, sizeof kind, "%s%s%s", app_gpu_kind_word(e.kind),
				 d.removable ? ", removable" : "",
				 d.headless ? ", headless" : "");
		app_gpu_inventory(a, n++, name, kind);
		const bool asked = (a->gpu_want[0] && app_name_has(name, a->gpu_want))
						|| (a->gpu_index > 0 && a->gpu_index == n);
		if (asked && !pick)
			pick = d;
	}
	if (!pick && (a->gpu_want[0] || a->gpu_index > 0))
		fnl_log(FNL_LOG_WARN, "no device is the GPU asked for; the "
				"display's is used");
	if (!pick)
		pick = MT(a)->layer.preferredDevice;
	if (!pick)
		pick = MTLCreateSystemDefaultDevice();
	int cur = -1, i = 0;
	for (id<MTLDevice> d in all) {
		if (d == pick)
			cur = i;
		i++;
	}
	app_gpu_list_publish(a, list, n, cur);
	return pick;
}

/* What FSAA multisamples at: the best of 4x and 2x the device takes; 1 when
 * it takes neither. One count serves the colour and the depth alike. */
static void choose_samples(App *a)
{
	int best = 1;
	for (int s = 4; s >= 2 && best == 1; s /= 2)
		if ([MT(a)->device supportsTextureSampleCount:(NSUInteger)s])
			best = s;
	if (FNL_FAULT("no-msaa"))
		best = 1;
	MT(a)->samples = best;
	fnl_atomic_store_u32(&a->msaa_samples, (uint32_t)best);
}

/* ---- building and tearing down ------------------------------------------- */

/* Everything below the layer, in dependency order. As on Vulkan: no
 * unwinding on failure, since destroy_device_objects() is safe on any
 * prefix -- every object is nil until the call that made it succeeded. */
static FnlResult create_device_objects(App *a)
{
	struct RendererState *g = MT(a);
	@autoreleasepool {
		g->device = choose_device(a);
		if (!g->device)
			return fnl_fail(FNL_ERR_NO_DEVICE, "no Metal device");
		char name[GPU_NAME_MAX];
		snprintf(name, sizeof name, "%s",
				 g->device.name.UTF8String ?: "(unnamed)");
		fnl_mutex_lock(&a->lock);
		snprintf(a->gpu_name, sizeof a->gpu_name, "%s", name);
		fnl_mutex_unlock(&a->lock);
		fnl_log(FNL_LOG_INFO, "GPU in use: %s (Metal)", name);

		g->queue = [g->device newCommandQueue];
		if (!g->queue)
			return fnl_fail(FNL_ERR_NO_DEVICE, "newCommandQueue failed");

		/* The layer is the window's, and every device made for it is
		 * handed it again: one rebuilt after a loss may be another GPU. */
		g->layer.device = g->device;
		g->layer.pixelFormat = MT_COLOR_FORMAT;
		/* Not framebuffer-only: the snapshot a move takes blits the
		 * drawable's texture into a buffer (frame.m), which a
		 * framebuffer-only drawable refuses. */
		g->layer.framebufferOnly = NO;
		g->layer.maximumDrawableCount = MT_DRAWABLES;
		/* A build that fails with the layer given this device: what the
		 * next renderer in the list then draws on is a layer Metal had
		 * (DESIGN *One build per operating system*). */
		if (FNL_FAULT("metal-after-layer"))
			return fnl_fail(FNL_ERR_SYSTEM, "FNL_FAULT: after the layer");

		choose_samples(a);
		a->srgb_surface = true;      /* every colour image is _sRGB */
		/* The palette is converted for that blend space; until then every
		 * entry is zero, and the titlebar draws as transparent black. */
		app_palette_init(a);

		FNL_TRY(mt_create_overlay(a));
		FNL_TRY(mt_window_images(a));
	}
	return FNL_OK;
}

/* The reverse, and safe on a lost device: letting go of an object on one is
 * harmless. Drained there too: a removed device's command buffers end in
 * an error, which waitUntilCompleted returns at, so the wait is bounded.
 * `save_cache` has nothing to save: Metal keeps its own. */
static void destroy_device_objects(App *a, bool save_cache)
{
	(void)save_cache;
	struct RendererState *g = MT(a);
	if (g->device)
		mt_wait_idle(a);
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		g->slot_cb[i] = nil;
	mt_destroy_overlay(a);
	mt_destroy_targets(a);
	mt_release_window_images(a);
	g->queue = nil;
	g->device = nil;

	/* Per-frame bookkeeping that named the objects just let go. */
	a->frame   = 0;
	a->resized = false;
	/* The scene image went too, and a recovery onto the same window size
	 * rebuilds nothing else, so the loop must be told nothing is asked
	 * for. */
	a->draw_scene_w = a->draw_scene_h = SCENE_NATIVE;
}

/* Rebuild everything below the layer, as Vulkan's recover_from_loss() does
 * and with its rule: every way out leaves a loss flag raised unless the
 * device is up. DESIGN *Device and surface loss*. */
static void recover_from_loss(App *a)
{
	const bool remake = a->surface_remake;
	a->surface_remake = false;
	/* A move to another GPU was said by the loop (gpu_switch_owed()), and
	 * has no driver reset behind it to wait out. */
	const bool quiet = remake || a->gpu_switching;
	if (!quiet)
		fnl_log(FNL_LOG_ERROR, "%s lost -- rebuilding",
				a->surface_lost ? "surface" : "device");

	/* The gap's picture under the swapchain before it goes
	 * (FnlBackendVtbl::present_window_hold). */
	BACKEND(a)->present_window_hold(a);
	destroy_device_objects(a, false);
	a->surface_lost = false;
	a->device_lost  = true;
	/* The swapchain went with the device: a move to another GPU presents
	 * into a window of its own, which a driver before has not claimed
	 * (FnlBackendVtbl::present_window_renew). */
	if (a->gpu_switching)
		BACKEND(a)->present_window_renew(a);

	if (!quiet)
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

/* The seam's first entry, so the renderer's state is born here, zeroed. Metal
 * has nothing above the device to make. */
static FnlResult mt_create_instance(App *a)
{
	FNL_ASSERT(!a->renderer_state);
	a->renderer_state = calloc(1, sizeof *a->renderer_state);
	if (!a->renderer_state)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY, "out of memory allocating "
						"the Metal renderer's state");
	return FNL_OK;
}

/* Every object let go before the free, which knows nothing of ARC: a
 * zeroing assignment of the whole struct releases each strong member. */
static void mt_destroy_instance(App *a)
{
	if (!a->renderer_state)
		return;
	*MT(a) = (struct RendererState){ 0 };
	free(a->renderer_state);
	a->renderer_state = NULL;
}

/* The surface is the window's layer, which the backend made with the
 * view; taking it is all there is to do. */
static FnlResult mt_create_surface(App *a)
{
	MT(a)->layer = mt_bridge_layer(a);
	if (!MT(a)->layer)
		return fnl_fail(FNL_ERR_SYSTEM, "the window has no Metal layer");
	return FNL_OK;
}

static void mt_destroy_surface(App *a)
{
	MT(a)->layer = nil;
}

/* The drawables and the window's depth at the size the frame policy
 * settled, and the present mode vsync asks for. False on a loss or a
 * failure the next turn asks again for (swapchain_adopt()). The
 * render thread has no pool outside the frame's, so this one lets go of
 * what the depth's descriptor and the transaction autorelease. */
static bool mt_recreate_swapchain(App *a)
{
	if (mt_device_lost(a))
		return false;
	bool ok;
	@autoreleasepool {
		ok = mt_window_images(a) >= 0;
	}
	return ok;
}

static bool mt_present_mode_matches(const App *a, bool vsync)
{
	return (int)fnl_atomic_load_u32(&a->present_mode) == (vsync ? 1 : 0);
}

/* Lost or not, as renderer.h asks of wait_idle before on_gpu_lost: a
 * removed device's buffers fail rather than hang (destroy_device_objects()). */
static void mt_seam_wait_idle(App *a)
{
	if (MT(a)->device)
		mt_wait_idle(a);
}

static bool mt_is_lost(const App *a)
{
	return a->device_lost || a->surface_lost;
}

/* No layer here names a display mode. */
static void mt_remake_surface(App *a)
{
	(void)a;
}

static bool mt_occluded(App *a)
{
	return mt_bridge_occluded(a);
}

static int mt_max_samples(const App *a)
{
	return (int)fnl_atomic_load_u32(&a->msaa_samples);
}

/* What the device has allocated, which Metal keeps for this process's
 * device alone: the panel's GPU memory. */
static bool mt_gpu_memory(App *a, double *mib)
{
	if (!MT(a)->device)
		return false;
	*mib = (double)MT(a)->device.currentAllocatedSize / (1024.0 * 1024.0);
	return true;
}

static const char *mt_device_name(const App *app)
{
	/* A per-thread snapshot under App::lock, as the other renderers' are. */
	static FNL_THREAD_LOCAL char snap[GPU_NAME_MAX];
	App *a = (App *)app;
	fnl_mutex_lock(&a->lock);
	memcpy(snap, a->gpu_name, sizeof snap);
	fnl_mutex_unlock(&a->lock);
	snap[sizeof snap - 1] = '\0';
	return snap[0] ? snap : "(no device)";
}

static const char *mt_present_mode_name(const App *a)
{
	return fnl_atomic_load_u32(&a->present_mode) == 1u
			   ? "display sync (vsync)"
			   : "display sync off";
}

const FnlRendererVtbl fnl_renderer_metal = {
	.name                  = "Metal",
	.api                   = FNL_RENDERER_METAL,
	.key                   = "metal",
	.draws_overlay         = true,
	.offscreen_scene       = true,
	.create_instance       = mt_create_instance,
	.destroy_instance      = mt_destroy_instance,
	.create_surface        = mt_create_surface,
	.destroy_surface       = mt_destroy_surface,
	.list_gpus             = mt_list_gpus,
	/* Any thread may use the device. */
	.thread_enter          = renderer_noop,
	.thread_leave          = renderer_noop,
	.create_device_objects = create_device_objects,
	.destroy_device_objects = destroy_device_objects,
	.recreate_swapchain    = mt_recreate_swapchain,
	.present_mode_matches  = mt_present_mode_matches,
	/* Nothing is retired: the command buffers keep what they name. */
	.retire_tick           = renderer_noop,
	.scale_changed         = mt_text_set_scale,
	.set_scene_size        = mt_scene_set_size,
	.draw_frame            = mt_draw_frame,
	.wait_idle             = mt_seam_wait_idle,
	.lost                  = mt_is_lost,
	.recover               = recover_from_loss,
	.remake_surface        = mt_remake_surface,
	.occluded              = mt_occluded,
	.max_samples           = mt_max_samples,
	.active_samples        = mt_active_samples,
	.gpu_time_poll         = mt_gpu_time_poll,
	.device_name           = mt_device_name,
	.gpu_memory            = mt_gpu_memory,
	.present_mode_name     = mt_present_mode_name,
	.overlay_flush_rects   = mt_overlay_flush_rects,
	.overlay_flush_shapes  = mt_overlay_flush_shapes,
	.overlay_scissor       = mt_overlay_scissor,
	.text_end              = mt_text_end,
	.overlay_cover         = mt_overlay_cover,
	.cover_release         = mt_cover_release,
};

/* ---- the public Metal interop -----------------------------------------------
 *
 * include/fenestal/fnl_metal.h.
 *
 * Each refuses an App that another renderer draws: a build may have several,
 * and every one's interop is exported. fnl_metal_gpu() is the render
 * thread's, since it reads the renderer's state, and refuses any other
 * (app_renderer_state_ours()); fnl_metal_scene_layout() any thread's,
 * reading atomics alone. */

bool fnl_metal_gpu(App *a, FnlMetalGpu *out)
{
	if (!a || !out || !app_renderer_state_ours(a) ||
		renderer_any(a) != &fnl_renderer_metal ||
		!a->renderer_state || !MT(a)->device)
		return false;
	out->device = MT(a)->device;
	out->queue  = MT(a)->queue;
	return true;
}

bool fnl_metal_scene_layout(App *a, FnlSceneTarget t,
							FnlMetalSceneLayout *out)
{
	if (!a || !out || renderer_any(a) != &fnl_renderer_metal ||
		t < 0 || t >= FNL_TARGET_COUNT)
		return false;
	int samples = 1;
	if (t == FNL_TARGET_OFFSCREEN_MS) {
		samples = mt_max_samples(a);
		if (samples <= 1)
			return false;
	}
	*out = (FnlMetalSceneLayout){
		.samples      = samples,
		.color_format = (uint32_t)MT_COLOR_FORMAT,
		.depth_format = (uint32_t)MT_DEPTH_FORMAT,
	};
	return true;
}

bool fnl_metal_scene(App *a, FnlMetalScene *out)
{
	/* on_record's thread alone, tested first as fnl_vulkan_scene_cmd() is. */
	if (!a || !out || app_thread() != APP_THREAD_RENDER ||
		a->renderer != &fnl_renderer_metal ||
		!a->renderer_state || !MT(a)->scene_enc_now)
		return false;
	struct RendererState *g = MT(a);
	out->encoder        = g->scene_enc_now;
	out->command_buffer = g->scene_cb_now;
	out->x = g->scene_x_now;
	out->y = g->scene_y_now;
	out->w = g->scene_w_now;
	out->h = g->scene_h_now;
	return true;
}

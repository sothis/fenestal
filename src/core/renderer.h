/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The renderer seam: everything the core asks of a graphics API. The other
 * of the two dispatch tables; see DESIGN *Two dispatch tables, not one*.
 *
 * Only what the *core* asks for is here. What a renderer's files share with
 * each other -- make_shader, begin_oneshot, the pipeline builder -- is not
 * part of the seam.
 *
 * create_surface is the bridge, the one entry that must know the window
 * server as well. It is here rather than in backend.h because the object
 * belongs to the renderer and dies with it: recovering a lost surface remakes
 * it while the window stays mapped.
 */

#ifndef FNL_RENDERER_H
#define FNL_RENDERER_H

/* Included from app.h, which has App and FnlResult by this point. */

typedef struct FnlRendererVtbl {
	/* As it goes on screen: "Vulkan". */
	const char *name;
	/* What fnl_renderer() reports, and what FnlInitDesc::renderer is
	 * checked against. */
	FnlRenderer api;
	/* As FNL_RENDERER spells it, at configure and in the environment:
	 * "vulkan", "gl", "d3d12", "metal". */
	const char *key;

	/* Whether this renderer can draw the overlay -- the text, rectangles and
	 * strokes the client titlebar is made of. Data, because fnl_init()
	 * consults it before any device exists: false is a window with no way to
	 * close it but the keyboard, so fnl_init() refuses outright. */
	bool draws_overlay;

	/* Whether this renderer can render the scene anywhere but straight into
	 * the window. The render scale and both antialiasing modes are built on
	 * that one target, so a renderer without it refuses all three. Wider than
	 * max_samples(), which answers only for FSAA. */
	bool offscreen_scene;

	/* ---- instance and bridge ------------------------------------------- */

	/* The API-level object that outlives a device loss: recovery rebuilds
	 * everything below it. The first entry and the last, so create_instance
	 * allocates the renderer's state (App::renderer_state) and destroy_instance
	 * frees it. destroy_instance is called only after create_instance
	 * succeeded, so a create that fails leaves nothing behind. */
	FNL_MUST_USE FnlResult (*create_instance)(App *a);
	void (*destroy_instance)(App *a);

	/* The bridge. Called once at startup and again whenever the surface
	 * itself is lost, always after the old one has been destroyed. */
	FNL_MUST_USE FnlResult (*create_surface)(App *a);
	void (*destroy_surface)(App *a);

	/* The GPUs this renderer would list for the window were it built, in
	 * the order its pick lists them, so that an index into them is the
	 * gpu_index that pick takes: found without building it, while another
	 * renderer draws, and without App::renderer_state, which is the other
	 * one's. At most `max`; 0 where it would draw nothing here. On a
	 * thread of its own after the first frame (app_gpu_list_others()), and
	 * again after a move to another kernel driver or a GPU's hotplug, which
	 * change what each lists; or on the main thread before the one drawing
	 * is built where the renderers share a driver
	 * (FnlBackendVtbl::shared_driver). Each entry's name, kind and
	 * renderer, as the pick's own list has them. */
	int (*list_gpus)(App *a, GpuEntry *out, int max);

	/* ---- which thread holds the device -------------------------------- */

	/* The calling thread is about to use the device, or has finished with it
	 * for now. For a graphics API whose context is current on one thread at
	 * a time -- OpenGL -- this is where it is made current and released; one
	 * whose device any thread may use names renderer_noop for both.
	 *
	 * The device is held by the thread that built it from create_device_
	 * objects on: the main thread, through fnl_init() and fnl_run()'s
	 * on_gpu_created. fnl_run() leaves it before starting the render thread,
	 * which enters at the top of render_loop() and leaves at the bottom;
	 * fnl_run() enters again after the join, for the last on_gpu_lost and
	 * the teardown. Loss recovery runs on the render thread, which holds it.
	 * Never called without a device instance (App::renderer_ready). */
	void (*thread_enter)(App *a);
	void (*thread_leave)(App *a);

	/* ---- device lifetime ----------------------------------------------- */

	/* Everything below the instance, in dependency order. One pair, used by
	 * both startup and loss recovery, so the two cannot build different sets
	 * of objects. destroy must be safe on a half-built device: create leaves
	 * without unwinding what it had. */
	FNL_MUST_USE FnlResult (*create_device_objects)(App *a);
	void (*destroy_device_objects)(App *a, bool save_cache);

	/* ---- per-frame surface --------------------------------------------- */

	/* Rebuild the swapchain and the framebuffers over it, retiring the old one
	 * rather than destroying it -- command buffers still name it. False means
	 * the device went away during the rebuild, which the loop answers by
	 * coming back round to recover(). */
	bool (*recreate_swapchain)(App *a);
	/* Resize the offscreen image the scene is rendered into, or drop it for
	 * the native path. Returns nothing: a device that will not allocate the
	 * target leaves the scene at the window's own resolution. */
	void (*set_scene_size)(App *a, int w, int h);

	/* ---- the frame ------------------------------------------------------ */

	/* Acquire, record if the draw state changed, submit, present. True iff
	 * something was actually submitted, which is what advances the retired
	 * swapchain countdown. The backend calls that ride on the present are
	 * made from inside this, through the backend table. */
	bool (*draw_frame)(App *a, uint32_t frame, const FnlDrawState *ds,
					   uint32_t cfg_serial, bool cfg_pending);

	/* Whether the swapchain that exists already presents the way `vsync`
	 * asks. False means the frame loop must rebuild, since no graphics API
	 * can change a present mode in place. Asked rather than compared because
	 * what a present mode *is* differs between graphics APIs. */
	bool (*present_mode_matches)(const App *a, bool vsync);

	/* One frame has been submitted; age whatever the GPU is holding on to
	 * until it is provably idle. Called only on real submissions: counting an
	 * acquire that timed out would free resources that may still be in
	 * use. */
	void (*retire_tick)(App *a);

	/* The surface scale moved, so anything rasterised for the old one is
	 * rasterised again -- the glyph atlas is baked per scale. A failure costs
	 * the frame its atlas rather than the frame, so it is reported and not
	 * propagated.
	 *
	 * Render thread, immediately before the rebuild that adopts the new
	 * scale. The queue has *not* been drained: an entry that replaces
	 * something a command buffer may still name drains it itself. */
	FNL_MUST_USE FnlResult (*scale_changed)(App *a, int s120);

	/* Block until the device has finished everything submitted to it. Called
	 * immediately before the caller's on_gpu_lost: joining the render thread
	 * ends the CPU side and says nothing about the queue, and a pipeline
	 * destroyed there may still be executing
	 * (VUID-vkDestroyPipeline-pipeline-00765). */
	void (*wait_idle)(App *a);

	/* ---- loss, occlusion and recovery ----------------------------------- */

	/* True when the objects underneath us are gone and the frame should
	 * unwind. Which of the two kinds it was is recorded on the App, because
	 * only one of them replaces the surface as well. */
	bool (*lost)(const App *a);

	/* Is the window covered, so that a present would go nowhere? The other
	 * half of FnlBackendVtbl::window_on_screen, for what only a swapchain
	 * can report. Not const, because asking may be how a renderer finds out
	 * the cover has gone. A renderer with no such report says false: it does
	 * not know, so it must not claim the window has gone. */
	bool (*occluded)(App *a);
	/* Tear down and rebuild after lost() said true. Called at the top of a
	 * frame, where nothing is half-recorded. */
	void (*recover)(App *a);
	/* The backend asked for its surface to be made again
	 * (app_surface_remake()): a surface that names a display mode, which
	 * another mode cannot be set under. Raises what lost() reports, so the
	 * rebuild is recover()'s, at the top of the same turn; a renderer whose
	 * surface follows the size by itself does nothing. */
	void (*remake_surface)(App *a);

	/* ---- reporting ------------------------------------------------------ */

	/* device_name, present_mode_name and max_samples are any thread's:
	 * fnl_gpu_name(), fnl_present_mode() and fnl_max_samples() reach them
	 * through renderer_any(), and a move to another renderer may free the
	 * drawing one's state while they run. They read App's own atomics,
	 * App::gpu_name under App::lock, and what the backend fixed before any
	 * thread started (X11's `composited`, for GL's pacer), and never
	 * App::renderer_state. */

	/* max_samples is the best count this device offers for both colour and
	 * depth, or 1 when it offers none worth having; it is what
	 * fnl_max_samples() reports and does not change with the frame.
	 *
	 * active_samples is what this frame got, which is not what was asked
	 * for: FSAA needs an offscreen target the renderer may have declined to
	 * build. draw.c hands it to on_overlay. */
	int  (*max_samples)(const App *a);
	int  (*active_samples)(const App *a, const FnlDrawState *ds);

	/* Hand app_gpu_time() the GPU's time on every finished frame not yet
	 * read, without waiting for one to finish. A frame's time is otherwise
	 * read when its slot or image is drawn again, which a window at rest
	 * never does: the last frames before it rested, and the first after a
	 * move to another GPU, would go unread. Render thread, every turn. */
	void (*gpu_time_poll)(App *a);

	/* The device's name, and the present mode the current swapchain was
	 * built with. Both are strings the renderer owns. */
	const char *(*device_name)(const App *a);
	const char *(*present_mode_name)(const App *a);
	/* The GPU memory this process holds on the device, in MiB, for the
	 * metrics panel (src/core/metrics.c): the API's own figure where it
	 * keeps one, the system's where it does not; false where neither
	 * says. Render thread, at most once a METRICS_WINDOW, and only while
	 * the panel is shown. */
	bool (*gpu_memory)(App *a, double *mib);

	/* ---- the overlay ---------------------------------------------------- */

	/* What core's overlay_flush_rects(), overlay_flush_shapes(),
	 * overlay_scissor() and text_end() hand on (app.h says what each does),
	 * on the render thread inside a recording. Entries rather than one
	 * symbol each, so that every renderer a build has can define its own. */
	void (*overlay_flush_rects)(App *a);
	void (*overlay_flush_shapes)(App *a);
	void (*overlay_scissor)(App *a, const FnlRect *r);
	FNL_MUST_USE FnlResult (*text_end)(App *a);
	/* The cover over the first frames after a move (draw_veil() in
	 * draw.c): App::snap, the last frame read back on the device before,
	 * drawn over the whole window at `alpha` through the fetch's shaders
	 * blended by a constant -- uploaded on first use on this device, which
	 * App::snap_gen says. False where there is none, or it would not
	 * upload: the core draws black instead. Inside a recording, after the
	 * glyphs. */
	bool (*overlay_cover)(App *a, float alpha);
	/* Let go of the cover and of the buffer a frame was read back into for
	 * App::snap, once the veil is down (VEIL_NONE): kept, they are some
	 * 33 MB at 4K for the device's life. Asked after every submit while it
	 * is, so nothing at all where neither is held; where one is, after
	 * whatever in flight still reads it. Render thread, outside a
	 * recording. */
	void (*cover_release)(App *a);
} FnlRendererVtbl;

/* An entry with nothing to do, for a renderer to name in its table rather
 * than write; in src/core/app.c, beside backend_noop. */
void renderer_noop(App *a);

/* Every renderer this build has, in the order fnl_init() tries them when the
 * caller accepts more than one (DESIGN *One build per operating system*):
 * on Windows Direct3D 12, which every Windows 10 has, on WARP where there
 * is no driver, then Vulkan, which wants a vendor's driver; on macOS Metal,
 * then Vulkan on MoltenVK, one layer more; on Linux Vulkan, then OpenGL.
 * Each is there by a -DFNL_HAVE_RENDERER_<NAME> that CMake passes with its
 * sources. A list, so that whatever reads it -- the core's choice, a table
 * of names -- cannot disagree with it. */
#if defined(FNL_HAVE_RENDERER_VULKAN)
#  define FNL_WITH_RENDERER_VULKAN(X) X(vulkan)
#else
#  define FNL_WITH_RENDERER_VULKAN(X)
#endif
#if defined(FNL_HAVE_RENDERER_GL)
#  define FNL_WITH_RENDERER_GL(X) X(gl)
#else
#  define FNL_WITH_RENDERER_GL(X)
#endif
#if defined(FNL_HAVE_RENDERER_D3D12)
#  define FNL_WITH_RENDERER_D3D12(X) X(d3d12)
#else
#  define FNL_WITH_RENDERER_D3D12(X)
#endif
#if defined(FNL_HAVE_RENDERER_METAL)
#  define FNL_WITH_RENDERER_METAL(X) X(metal)
#else
#  define FNL_WITH_RENDERER_METAL(X)
#endif

#if defined(_WIN32)
#  define FNL_RENDERERS(X) \
	FNL_WITH_RENDERER_D3D12(X) FNL_WITH_RENDERER_VULKAN(X)
#elif defined(__APPLE__)
#  define FNL_RENDERERS(X) \
	FNL_WITH_RENDERER_METAL(X) FNL_WITH_RENDERER_VULKAN(X) \
	FNL_WITH_RENDERER_GL(X)
#else
#  define FNL_RENDERERS(X) \
	FNL_WITH_RENDERER_VULKAN(X) FNL_WITH_RENDERER_GL(X)
#endif

extern const FnlRendererVtbl fnl_renderer_vulkan;
extern const FnlRendererVtbl fnl_renderer_gl;
extern const FnlRendererVtbl fnl_renderer_d3d12;
extern const FnlRendererVtbl fnl_renderer_metal;

/* In app.c. The first says, before the window opens, whether the caller
 * may be handed any renderer of this build (FnlInitDesc::renderer and
 * ::renderers, FNL_RENDERER); the second chooses one and builds it on the
 * window that is open -- each candidate in FNL_RENDERERS order, stepping
 * past one that fails, until one has an instance, a surface and a device. */
FNL_MUST_USE FnlResult renderer_check_choice(const FnlInitDesc *d);
FNL_MUST_USE FnlResult renderer_choose_and_build(App *a, const FnlInitDesc *d);

/* In app.c. A move to another renderer's GPU (App::renderer_next): the one
 * drawing torn down and the next built on the same window, on the render
 * thread, with the caller already told on_gpu_lost; where the next will not
 * build, the one before is built again, and where that will not either,
 * App::renderer_owed says so and renderer_owed_retry() tries it each turn.
 * DESIGN *Moving to another renderer*. */
void renderer_swap(App *a);
bool renderer_owed_retry(App *a);

/* The renderer drawing, for a thread that is not the render thread: the
 * render thread swaps App::renderer, and a reader elsewhere takes the
 * published copy, which names a table that is never freed. */
const FnlRendererVtbl *renderer_any(const App *a);

/* The build's renderer for `api`, or NULL where the build has none. */
const FnlRendererVtbl *renderer_of_api(FnlRenderer api);

/* The render thread's while it runs, and the main thread's before it
 * starts and after the join; anywhere else a read of a field that a move
 * to another renderer writes, which renderer_any() is for. */
#define RENDERER(a) ((a)->renderer)

#endif /* FNL_RENDERER_H */

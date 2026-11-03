/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The OpenGL half of the frame: the scene, the window's image, the overlay
 * over it, the flip onto the window, and the swap.
 *
 * GL has nothing to resubmit, so every frame drawn calls on_record and
 * on_overlay again (fenestal.h *on_record*, DESIGN *Recording*): there is no
 * record_gen test here. Whether a turn draws at all is the frame policy's,
 * as with every renderer.
 *
 * Every image is drawn with row 0 at the top, so the viewports and scissors
 * below are the Vulkan renderer's numbers as they stand; the window's image
 * is flipped once, onto the window's own framebuffer, at the end.
 */

#include "core/app.h"

#include "renderer/gl/gl_internal.h"

/* How long one wait for a frame slot's fence lasts before the loss is looked
 * for again. A robust context signals its fences on a reset; any other is
 * waited on for as long as the driver takes, as Vulkan's fences are and
 * Direct3D 12's until the device says it was removed: with no reset to
 * report there is no loss to give up for, and a ceiling would call a slow
 * frame on a software rasteriser one. */
#define FENCE_WAIT_NS 100000000ull   /* 100 ms */

static const float BLACK[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
static const float CLEAR[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static const float DEPTH_FAR = 1.0f;

/* ---- what this frame's scene is drawn into --------------------------------
 * Asked of the objects, as on Vulkan: gl_scene_set_size() and
 * gl_scene_ensure_aa() give up rather than fail. */

static bool scene_offscreen(const App *a, const FnlDrawState *ds)
{
	return ds->scene_w > 0 && GLS(a)->scene.fbo &&
		   a->scene_extent.w == ds->scene_w &&
		   a->scene_extent.h == ds->scene_h;
}

static bool scene_msaa(const App *a, const FnlDrawState *ds)
{
	return ds->aa == FNL_AA_MSAA && GLS(a)->scene_ms.fbo;
}

/* MLAA straight into the window's image when the scene is already the size
 * it lands at; through an image of its own when it is then scaled. */
static bool scene_mlaa_direct(const App *a, const FnlDrawState *ds)
{
	int ax, ay, aw, ah;
	if (ds->aa != FNL_AA_MLAA || !scene_offscreen(a, ds))
		return false;
	content_px(a, ds, &ax, &ay, &aw, &ah);
	return ds->scene_w == aw && ds->scene_h == ah;
}

static bool scene_mlaa(const App *a, const FnlDrawState *ds)
{
	return ds->aa == FNL_AA_MLAA && GLS(a)->mlaa.fbo &&
		   !scene_mlaa_direct(a, ds);
}

int gl_active_samples(const App *a, const FnlDrawState *ds)
{
	return scene_msaa(a, ds) ? (int)fnl_atomic_load_u32(&a->msaa_samples)
							 : 1;
}

/* ---- the scene ----------------------------------------------------------- */

/* The seam: `fbo` bound and cleared, the viewport and scissor on the
 * rectangle, and what is drawn the caller's. The framebuffer is stashed for
 * fnl_gl_scene_framebuffer(), as Vulkan stashes its command buffer. */
static void record_scene(App *a, const FnlDrawState *ds, unsigned fbo,
						 FnlSceneTarget t, int w, int h)
{
	if (!a->cb.on_record)
		return;
	int ax, ay, aw, ah;
	content_px(a, ds, &ax, &ay, &aw, &ah);
	FnlFrame f = {
		.index      = a->presents,
		.time       = now_seconds() - a->started_at,
		.area_w_px  = aw,
		.area_h_px  = ah,
		.scene_w_px = w,
		.scene_h_px = h,
		.target     = t,
		.samples    = t == FNL_TARGET_OFFSCREEN_MS
						  ? (int)fnl_atomic_load_u32(&a->msaa_samples) : 1,
		.scene_time = ds->scene_time,
	};
	GLS(a)->scene_fbo_now = fbo;
	a->cb.on_record(a->cb.ud, a, &f);
	GLS(a)->scene_fbo_now = 0;
}

/* Bound, cleared to opaque black and far depth, and the viewport and scissor
 * on the whole of it -- what a Vulkan pass's load-op clear and dynamic state
 * leave. Clears obey the scissor and the masks, which gl_own_state() sets. */
static void begin_target(App *a, const GlTarget *t, int w, int h)
{
	gl_own_state(a);
	gl.BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
	gl.Viewport(0, 0, w, h);
	gl.Scissor(0, 0, w, h);
	gl.ClearNamedFramebufferfv(t->fbo, GL_COLOR, 0, BLACK);
	if (t->depth)
		gl.ClearNamedFramebufferfv(t->fbo, GL_DEPTH, 0, &DEPTH_FAR);
}

/* The fetch or the filter: one covering triangle reading `tex`. */
static void fetch(App *a, unsigned prog, unsigned tex)
{
	gl.UseProgram(prog);
	gl.BindVertexArray(GLS(a)->vao_empty);
	gl.BindTextureUnit(0, tex);
	gl.BindSampler(0, GLS(a)->sampler_linear);
	gl.Disable(GL_BLEND);
	gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

/* The offscreen scene: rendered at its own resolution, resolved where it was
 * multisampled, and filtered into MLAA's image where it is then scaled. */
static void scene_pass(App *a, const FnlDrawState *ds)
{
	const int w = a->scene_extent.w, h = a->scene_extent.h;
	const bool ms = scene_msaa(a, ds);
	const GlTarget *t = ms ? &GLS(a)->scene_ms : &GLS(a)->scene;

	begin_target(a, t, w, h);
	record_scene(a, ds, t->fbo,
				 ms ? FNL_TARGET_OFFSCREEN_MS : FNL_TARGET_OFFSCREEN, w, h);

	gl_own_state(a);
	if (ms) {
		/* A blit obeys the scissor too. With GL_FRAMEBUFFER_SRGB on, the
		 * samples are averaged in linear light. */
		gl.Disable(GL_SCISSOR_TEST);
		gl.BlitNamedFramebuffer(GLS(a)->scene_ms.fbo, GLS(a)->scene.fbo,
								0, 0, w, h, 0, 0, w, h,
								GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	if (scene_mlaa(a, ds)) {
		begin_target(a, &GLS(a)->mlaa, w, h);
		fetch(a, GLS(a)->prog_mlaa, GLS(a)->scene.color);
	}
}

/* The scene onto the window's image, under the overlay: drawn straight in at
 * the area's own resolution, or the image it was rendered into fetched
 * across the letterboxed rectangle that keeps its aspect. */
static void draw_scene(App *a, const FnlDrawState *ds)
{
	int ax, ay, aw, ah;
	content_px(a, ds, &ax, &ay, &aw, &ah);
	if (aw < 1 || ah < 1)
		return;

	int dx = 0, dy = 0, dw = aw, dh = ah;
	const bool offscreen = scene_offscreen(a, ds);
	if (offscreen)
		scene_dst_rect(ds->scene_w, ds->scene_h, aw, ah, &dx, &dy, &dw, &dh);

	gl.Viewport(ax + dx, ay + dy, dw, dh);
	gl.Scissor(ax + dx, ay + dy, dw, dh);

	if (!offscreen) {
		record_scene(a, ds, GLS(a)->window.fbo, FNL_TARGET_DIRECT, dw, dh);
		return;
	}
	if (scene_mlaa_direct(a, ds))
		fetch(a, GLS(a)->prog_mlaa, GLS(a)->scene.color);
	else
		fetch(a, GLS(a)->prog_blit, scene_mlaa(a, ds)
										? GLS(a)->mlaa.color
										: GLS(a)->scene.color);
}

/* The transparent grab ring, as the four strips the window rectangle leaves
 * over, each cleared to alpha zero -- as the Vulkan renderer clears them, and
 * for its reason. Only where there is a ring, which is Wayland's. */
static void clear_grab_ring(App *a)
{
	FnlRect win;
	if (a->draw_margin <= 0 ||
		!overlay_clip(a, 0, 0, logical_w(a), logical_h(a), &win))
		return;
	const int fw = a->extent.w, fh = a->extent.h;
	const int wr = win.x + win.w, wb = win.y + win.h;
	const FnlRect ring[4] = {
		{ 0, 0, fw, win.y },                    /* top    */
		{ 0, wb, fw, fh - wb },                 /* bottom */
		{ 0, win.y, win.x, win.h },             /* left   */
		{ wr, win.y, fw - wr, win.h },          /* right  */
	};
	for (int i = 0; i < 4; i++) {
		if (ring[i].w <= 0 || ring[i].h <= 0)
			continue;
		gl.Scissor(ring[i].x, ring[i].y, ring[i].w, ring[i].h);
		gl.ClearNamedFramebufferfv(GLS(a)->window.fbo, GL_COLOR, 0, CLEAR);
	}
}

/* Everything drawn this frame, into the window's image. False is a frame to
 * bail out of: the overlay could not be drawn. */
static bool draw(App *a, const FnlDrawState *ds, uint32_t slot)
{
	if (scene_offscreen(a, ds))
		scene_pass(a, ds);

	begin_target(a, &GLS(a)->window, a->extent.w, a->extent.h);
	clear_grab_ring(a);
	draw_scene(a, ds);

	/* The overlay over the whole image, in a state of the library's own:
	 * on_record may have left anything set. */
	gl_own_state(a);
	gl.BindFramebuffer(GL_FRAMEBUFFER, GLS(a)->window.fbo);
	gl.Viewport(0, 0, a->extent.w, a->extent.h);
	gl.Scissor(0, 0, a->extent.w, a->extent.h);
	GLS(a)->overlay_on = true;
	const FnlResult orc = draw_overlay(a, ds, slot);
	GLS(a)->overlay_on = false;
	return orc >= 0;
}

/* ---- the frame ----------------------------------------------------------- */

/* Wait until the GPU has finished the frame that last used this slot, whose
 * arenas the core is about to write. False on a loss. */
static bool wait_slot(App *a, uint32_t slot)
{
	GLsync f = (GLsync)GLS(a)->fence[slot];
	if (!f)
		return true;
	if (a->timing &&
		gl.ClientWaitSync(f, 0, 0) == GL_ALREADY_SIGNALED)
		a->fc_idle++;
	for (;;) {
		const GLenum r = gl.ClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT,
										   FENCE_WAIT_NS);
		if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED)
			break;
		if (gl_reset_lost(a))
			return false;
		if (r == GL_WAIT_FAILED) {
			fnl_log(FNL_LOG_ERROR, "waiting on a frame's fence failed");
			break;
		}
	}
	gl.DeleteSync(f);
	GLS(a)->fence[slot] = NULL;
	return true;
}

/* The window's image read back into GLS(a)->readback: BGRA, the scanout's
 * byte order, row 0 the image's top, which is the order the backend wants
 * (DESIGN *One shader source*). For a surfaceless bridge's present and for
 * the snapshot a move takes. Synchronous: glReadPixels into client memory
 * waits for the frame, as the Vulkan copy path waits on its fence. A read
 * that failed is said as a swap that failed would be: a lost context as a
 * loss, anything else as a frame not shown. */
static GlSwap read_window(App *a)
{
	const int w = a->extent.w, h = a->extent.h;
	if (w <= 0 || h <= 0)
		return GL_SWAP_FAILED;
	const size_t pitch = (size_t)w * 4u;
	if ((size_t)h > SIZE_MAX / pitch)
		return GL_SWAP_FAILED;
	const size_t need = pitch * (size_t)h;
	if (need > GLS(a)->readback_bytes) {
		void *grown = realloc(GLS(a)->readback, need);
		if (!grown) {
			fnl_log(FNL_LOG_WARN, "out of memory reading back a %dx%d frame",
					w, h);
			return GL_SWAP_FAILED;
		}
		GLS(a)->readback = grown;
		GLS(a)->readback_bytes = need;
	}
	/* The pack state set, not assumed: on_record may have left any of it,
	 * and a row length or a skip left over writes past the buffer, a pack
	 * buffer bound reads into that instead. As the atlas upload resets the
	 * unpack state (overlay.c). */
	gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
	gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
	gl.PixelStorei(GL_PACK_SKIP_ROWS, 0);
	gl.PixelStorei(GL_PACK_SKIP_PIXELS, 0);
	/* The encoded bytes, as the scanout and the cover take them: a driver
	 * may decode an sRGB image to linear on the read while
	 * GL_FRAMEBUFFER_SRGB is on, which the overlay leaves it, and the
	 * snapshot is read before the swap turns it off. The next frame's
	 * overlay turns it on again (gl_own_state()). */
	gl.Disable(GL_FRAMEBUFFER_SRGB);
	gl.BindFramebuffer(GL_READ_FRAMEBUFFER, GLS(a)->window.fbo);
	gl_clear_errors();
	gl.ReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, GLS(a)->readback);
	if (gl.GetError() != GL_NO_ERROR)
		return gl_reset_lost(a) ? GL_SWAP_DEVICE_LOST : GL_SWAP_FAILED;
	return GL_SWAP_OK;
}

/* The copy path's readback, started: the window's image into the pack
 * buffer, a copy the GPU makes while the frame's end stamp waits behind
 * it. It is work every frame on this path costs, which the GPU's time on
 * the frame holds, as Vulkan's copy path's readback is in its span; read
 * into client memory instead, the copy was a wait inside the present that
 * neither cost figure counted (DESIGN *What a frame costs, compared*).
 * False where there is no buffer to be had, and the present then reads
 * back as the snapshot does. */
static bool pack_start(App *a)
{
	const int w = a->extent.w, h = a->extent.h;
	if (w <= 0 || h <= 0)
		return false;
	const size_t pitch = (size_t)w * 4u;
	if ((size_t)h > SIZE_MAX / pitch)
		return false;
	const size_t need = pitch * (size_t)h;
	gl_clear_errors();
	if (need > GLS(a)->pack_bytes) {
		/* Immutable storage, so a larger image is a new buffer. */
		if (GLS(a)->pack_buf)
			gl.DeleteBuffers(1, &GLS(a)->pack_buf);
		GLS(a)->pack_buf = 0;
		GLS(a)->pack_bytes = 0;
		gl.CreateBuffers(1, &GLS(a)->pack_buf);
		/* In the CPU's memory from the start: every frame is mapped and
		 * read there. Without the hint NVIDIA's GL put it in video memory
		 * and moved it at the first map, and said so in the debug output. */
		gl.NamedBufferStorage(GLS(a)->pack_buf, (GLsizeiptr)need, NULL,
							  GL_MAP_READ_BIT | GL_CLIENT_STORAGE_BIT);
		if (gl.GetError() != GL_NO_ERROR) {
			gl.DeleteBuffers(1, &GLS(a)->pack_buf);
			GLS(a)->pack_buf = 0;
			return false;
		}
		GLS(a)->pack_bytes = need;
	}
	/* The pack state as read_window() sets it, the buffer bound in place
	 * of client memory: the pointer below is an offset into it. */
	gl.BindBuffer(GL_PIXEL_PACK_BUFFER, GLS(a)->pack_buf);
	gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
	gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
	gl.PixelStorei(GL_PACK_SKIP_ROWS, 0);
	gl.PixelStorei(GL_PACK_SKIP_PIXELS, 0);
	gl.Disable(GL_FRAMEBUFFER_SRGB);
	gl.BindFramebuffer(GL_READ_FRAMEBUFFER, GLS(a)->window.fbo);
	gl.ReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	if (gl.GetError() != GL_NO_ERROR)
		return false;
	GLS(a)->pack_w = w;
	GLS(a)->pack_h = h;
	GLS(a)->pack_pending = true;
	return true;
}

/* A surfaceless bridge's present: the output's own buffer flipped on where
 * the frame was drawn into it (gl_bridge_scan_begin()), and otherwise the
 * window's image read back and handed to the backend, which flips it onto
 * the output -- waiting for the last flip under vsync, which makes this the
 * throttle as a swap is. From the pack buffer where pack_start() filled
 * it: the map waits for the GPU's copy, and the backend copies the pixels
 * out before it returns. */
static GlSwap gl_present_pixels(App *a)
{
	if (GLS(a)->scan_direct)
		return gl_bridge_scan_show(a) ? GL_SWAP_OK : GL_SWAP_FAILED;
	if (GLS(a)->pack_pending) {
		GLS(a)->pack_pending = false;
		const int w = GLS(a)->pack_w, h = GLS(a)->pack_h;
		const size_t pitch = (size_t)w * 4u;
		const void *px = gl.MapNamedBufferRange(GLS(a)->pack_buf, 0,
												(GLsizeiptr)(pitch *
															 (size_t)h),
												GL_MAP_READ_BIT);
		if (px) {
			BACKEND(a)->present_pixels(a, px, w, h, pitch, true);
			gl.UnmapNamedBuffer(GLS(a)->pack_buf);
			return GL_SWAP_OK;
		}
		if (gl_reset_lost(a))
			return GL_SWAP_DEVICE_LOST;
	}
	const GlSwap r = read_window(a);
	if (r != GL_SWAP_OK)
		return r;
	BACKEND(a)->present_pixels(a, GLS(a)->readback, a->extent.w,
							   a->extent.h, (size_t)a->extent.w * 4u, true);
	return GL_SWAP_OK;
}

/* The snapshot a move takes (App::snap_owed): this frame's image, read
 * back as the surfaceless present reads it, before it goes on the window.
 * The read is the one stall a move costs here. */
static void snapshot(App *a)
{
	if (read_window(a) == GL_SWAP_OK)
		app_snapshot_set(a, GLS(a)->readback, a->extent.w, a->extent.h,
						 (size_t)a->extent.w * 4u, true);
	else
		app_snapshot_decline(a);
}

/* The surface's interval for this swap. 1 where vsync asks for it and the
 * swap is the throttle; 0 where something else paces (gl_bridge_pacer()),
 * and 0 for GL_RESIZE_HOLD after a size change even where the swap is the
 * throttle: under Mesa's DRI3 swap an interval of 1 keeps one present in
 * flight and waits for the last one's completion, which during a resize
 * comes late, so each step of a drag lost a refresh and the window hung
 * behind the pointer (DESIGN *X11*). The loop spaces those frames at the
 * output's refresh instead; with nobody compositing they may tear while
 * the size changes, and the interval comes back once it holds still. */
static void gl_choose_interval(App *a)
{
	const bool resizing = now_seconds() - GLS(a)->resized_at < GL_RESIZE_HOLD;
	const int give = GLS(a)->interval_asked && !gl_bridge_pacer(a) &&
					 !resizing ? 1 : 0;
	/* Kept only once the driver took it, so a refusal is asked again --
	 * on Wayland, 0 refused would leave Mesa's untimed wait in place. */
	if (give != GLS(a)->interval && gl_bridge_interval(a, give))
		GLS(a)->interval = give;
}

/* The GPU's time on this slot's last frame, which the slot's fence -- just
 * waited for -- says is done: once a frame, and never waited for. */
static void gl_time_read(App *a, uint32_t slot)
{
	if (!GLS(a)->ts_unread[slot] || !GLS(a)->ts_query[slot][1])
		return;
	GLint ready = 0;
	gl.GetQueryObjectiv(GLS(a)->ts_query[slot][1], GL_QUERY_RESULT_AVAILABLE,
						&ready);
	if (!ready)
		return;
	GLuint64 t0 = 0, t1 = 0;
	gl.GetQueryObjectui64v(GLS(a)->ts_query[slot][0], GL_QUERY_RESULT, &t0);
	gl.GetQueryObjectui64v(GLS(a)->ts_query[slot][1], GL_QUERY_RESULT, &t1);
	GLS(a)->ts_unread[slot] = false;
	if (t1 >= t0) {
		static const char *const NAMES[] = { "span" };
		const double s = (double)(t1 - t0) * 1e-9;
		app_gpu_time(a, s, GLS(a)->ts_index[slot]);
		parts_gpu(a, NAMES, &s, 1);
	}
}

/* Every slot's: a pair not yet there says so and is asked again. */
void gl_gpu_time_poll(App *a)
{
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		gl_time_read(a, i);
}

bool gl_draw_frame(App *a, uint32_t frame, const FnlDrawState *ds,
				   uint32_t cfg_serial, bool cfg_pending)
{
	const double t_enter = a->timing ? now_seconds() : 0.0;
	a->present_block = -1.0;
	a->present_untimed = GLS(a)->software;

	/* MAX_FRAMES_IN_FLIGHT slots, one a turn: the frame index is the slot,
	 * and the overlay's arenas are the slot's. */
	const uint32_t slot = frame % MAX_FRAMES_IN_FLIGHT;

	const double t_fence = now_seconds();
	if (!wait_slot(a, slot))
		return false;
	const double t_ready = now_seconds();
	a->device_block = t_ready - t_fence;
	if (a->timing)
		a->fc_wait = t_ready - t_enter;

	if (gl_reset_lost(a))
		return false;
	gl_time_read(a, slot);
	if (GLS(a)->ts_query[slot][0])
		gl.QueryCounter(GLS(a)->ts_query[slot][0], GL_TIMESTAMP);

	/* The half of the offscreen target this frame's antialiasing wants,
	 * built the first time a frame wants it. Nothing to re-record: every
	 * frame is drawn afresh here. */
	if (scene_offscreen(a, ds))
		(void)gl_scene_ensure_aa(a, ds->aa == FNL_AA_MSAA,
								 ds->aa == FNL_AA_MLAA &&
									 !scene_mlaa_direct(a, ds));

	const double t_rec = now_seconds();
	app_frame_mark(a, FRAME_MARK_REC);
	if (!draw(a, ds, slot)) {
		/* What was drawn may still read the slot's arenas, so the slot is
		 * fenced as a finished frame's is before it is given up. */
		GLS(a)->fence[slot] = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		return false;
	}
	if (a->timing)
		a->fc_record = now_seconds() - t_rec;
	/* A snapshot's read is synchronous and inside this frame's span, so a
	 * frame that takes one is not sampled: the copy is no part of drawing
	 * it, as a snapshot's is not on the other renderers. */
	const bool snapped = a->snap_owed;
	if (a->snap_owed)
		snapshot(a);

	/* What rides on the swap's commit is requested immediately before it,
	 * as on Vulkan (app_surface_sync()). */
	app_surface_sync(a, cfg_serial, cfg_pending);
	BACKEND(a)->frame_request(a);

	/* With no window surface the image is read back instead, after the
	 * timing below begins, as the swap is (gl_present_pixels()). */
	const bool surfaceless = gl_bridge_surfaceless(a);

	/* The window's image onto the window, flipped: the image's row 0 is the
	 * top, the window's framebuffer's is the bottom. Raw bytes, both already
	 * sRGB-encoded, so GL_FRAMEBUFFER_SRGB is off for the copy.
	 *
	 * Framebuffer 0 is bound first although the blit names it: Mesa attaches
	 * the window's current back buffer when the default framebuffer is
	 * bound, and a blit into it unbound can land in a buffer the swap does
	 * not present -- measured on radeonsi, a black frame after each change. */
	gl.Disable(GL_SCISSOR_TEST);
	gl.Disable(GL_FRAMEBUFFER_SRGB);
	if (surfaceless) {
		/* Onto the output's own buffer where the bridge has one, read back
		 * where not: either way the copy is in the span. */
		if (!gl_bridge_scan_begin(a, GLS(a)->window.fbo, a->extent.w,
								  a->extent.h))
			(void)pack_start(a);
	} else {
		/* At the extent, which is the back buffer's height: Wayland's
		 * wl_egl_window is sized by the rebuild before the frame. On X11
		 * the server sizes the window and Mesa takes the new size at the
		 * next draw, so a frame in flight when a resize lands could blit
		 * into a taller buffer; the mid-drag probe found none in 36
		 * captures. */
		gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		gl.BlitNamedFramebuffer(GLS(a)->window.fbo, 0,
								0, 0, a->extent.w, a->extent.h,
								0, a->extent.h, a->extent.w, 0,
								GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	if (GLS(a)->ts_query[slot][1]) {
		gl.QueryCounter(GLS(a)->ts_query[slot][1], GL_TIMESTAMP);
		GLS(a)->ts_unread[slot] = !snapped;
		if (!snapped)
			GLS(a)->ts_index[slot] = app_gpu_timed(a);
	}
	GLS(a)->fence[slot] = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

	gl_choose_interval(a);
	const double t_present = now_seconds();
	app_frame_mark(a, FRAME_MARK_PRESENT);
	a->late_cost = a->late_cost > 0.0
				 ? a->late_cost * 0.9 + (t_present - t_rec) * 0.1
				 : t_present - t_rec;
	if (a->timing)
		a->fc_submit = (t_present - t_rec) - a->fc_record;

	const GlSwap sw = surfaceless ? gl_present_pixels(a) : gl_bridge_swap(a);
	a->present_calls++;
	const double t_presented = now_seconds();
	app_frame_mark(a, FRAME_MARK_PRESENTED);
	if (a->timing)
		a->fc_present = t_presented - t_present;
	if (sw == GL_SWAP_OK) {
		a->presents++;
		/* The swap is the throttle under an interval of 1. Not on a
		 * software device, whose swap waits for the rasteriser. */
		if (!GLS(a)->software)
			a->present_block = t_presented - t_present;
	}
	BACKEND(a)->flush(a);

	switch (sw) {
	case GL_SWAP_OK:
		break;
	case GL_SWAP_DEVICE_LOST:
		a->device_lost = true;
		app_frame_take_back(a);
		break;
	case GL_SWAP_SURFACE_LOST:
		a->surface_lost = true;
		app_frame_take_back(a);
		break;
	case GL_SWAP_FAILED:
		app_frame_take_back(a);
		break;
	}

	if (FNL_FAULT("device-lost"))
		a->device_lost = true;
	if (FNL_FAULT("surface-lost"))
		a->surface_lost = true;

	/* Drawn and handed over, whatever the swap made of it. */
	return true;
}

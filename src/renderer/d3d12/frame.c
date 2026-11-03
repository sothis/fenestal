/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Direct3D 12 half of the frame: the scene, the window's image, the
 * overlay over it, and the present.
 *
 * Recorded afresh every frame, as GL is (fenestal.h *on_record*, DESIGN
 * *Recording*): one command list, reset on the frame slot's allocator, so
 * there is no record_gen test here. Whether a turn draws at all is the
 * frame policy's, as with every renderer.
 *
 * Every image has row 0 at the top and a viewport's y counts down, so the
 * viewports and scissors are the Vulkan renderer's numbers as they stand.
 */

#include "core/app.h"

#include "renderer/d3d12/d3d12_internal.h"

/* How long the frame-latency wait lasts before the frame goes ahead
 * anyway: a swapchain that stopped signalling (a window gone off screen)
 * must not hold the loop. Longer than any refresh. */
#define LATENCY_WAIT_MS 250
/* How long a skipped frame's count is waited for when taken back: longer
 * than a refresh down to 10 Hz, and short enough that a count that never
 * comes costs one hitch, not the loop's bound. */
#define TAKE_BACK_MS 100

/* How long the frame that acks a size waits to be shown before the ack goes
 * anyway (FnlBackendVtbl::ack_when_shown): the statistics report a frame
 * shown on the third vertical blank after its present (measured on both of
 * schenker's devices, 36 to 49 ms at 60 Hz), so three blanks, and a time
 * as well for an output whose blanks do not come at its rate. And how many
 * such waits in a row may see no statistics at all before they stop being
 * made. */
#define ACK_SHOWN_VBLANKS 3
#define ACK_SHOWN_WAIT_S  0.060
#define DISJOINT_GIVE_UP  3

static const float BLACK[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
static const float CLEAR[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

/* ---- what this frame's scene is drawn into --------------------------------
 * Asked of the objects, as on Vulkan: d3d_scene_set_size() and
 * d3d_scene_ensure_aa() give up rather than fail. */

static bool scene_offscreen(const App *a, const FnlDrawState *ds)
{
	return ds->scene_w > 0 && D3D(a)->scene.color &&
		   a->scene_extent.w == ds->scene_w &&
		   a->scene_extent.h == ds->scene_h;
}

static bool scene_msaa(const App *a, const FnlDrawState *ds)
{
	return ds->aa == FNL_AA_MSAA && D3D(a)->scene_ms.color;
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
	return ds->aa == FNL_AA_MLAA && D3D(a)->mlaa.color &&
		   !scene_mlaa_direct(a, ds);
}

int d3d_active_samples(const App *a, const FnlDrawState *ds)
{
	return scene_msaa(a, ds) ? (int)fnl_atomic_load_u32(&a->msaa_samples)
							 : 1;
}

/* ---- binding ------------------------------------------------------------ */

static void set_rect(App *a, int x, int y, int w, int h)
{
	ID3D12GraphicsCommandList *l = D3D(a)->list;
	const D3D12_VIEWPORT v = { (float)x, (float)y, (float)w, (float)h,
							   0.0f, 1.0f };
	const D3D12_RECT s = { x, y, x + w, y + h };
	ID3D12GraphicsCommandList_RSSetViewports(l, 1, &v);
	ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &s);
}

/* Bound, cleared to opaque black and far depth, the viewport and scissor on
 * the whole of it -- what a Vulkan pass's load-op clear leaves. A null
 * `dsv` binds no depth. */
static void begin_target(App *a, const D3D12_CPU_DESCRIPTOR_HANDLE *rtv,
						 const D3D12_CPU_DESCRIPTOR_HANDLE *dsv, int w,
						 int h)
{
	ID3D12GraphicsCommandList *l = D3D(a)->list;
	d3d_own_state(a);
	ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, rtv, FALSE, dsv);
	ID3D12GraphicsCommandList_ClearRenderTargetView(l, *rtv, BLACK, 0, NULL);
	if (dsv)
		ID3D12GraphicsCommandList_ClearDepthStencilView(
			l, *dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
	set_rect(a, 0, 0, w, h);
}

/* An offscreen target's colour into `to`, from the state the last frame
 * left it in. */
static void target_to(App *a, D3dTarget *t, D3D12_RESOURCE_STATES to)
{
	d3d_barrier(a, t->color, t->state, to);
	t->state = to;
}

/* ---- the scene ----------------------------------------------------------- */

/* The seam: the list, its render target bound and cleared, the viewport on
 * the rectangle, and what is drawn the caller's -- stashed for
 * fnl_d3d12_scene(), as Vulkan stashes its command buffer. */
static void record_scene(App *a, const FnlDrawState *ds,
						 D3D12_CPU_DESCRIPTOR_HANDLE rtv,
						 D3D12_CPU_DESCRIPTOR_HANDLE dsv, FnlSceneTarget t,
						 int x, int y, int w, int h)
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
	struct RendererState *g = D3D(a);
	g->scene_list_now = g->list;
	g->scene_rtv_now  = rtv;
	g->scene_dsv_now  = dsv;
	g->scene_rect_now = (D3D12_RECT){ x, y, x + w, y + h };
	a->cb.on_record(a->cb.ud, a, &f);
	g->scene_list_now = NULL;
}

/* The offscreen scene: rendered at its own resolution, resolved where it was
 * multisampled, and filtered into MLAA's image where it is then scaled.
 * Each image ends where the window's pass reads it: a shader resource. */
static void scene_pass(App *a, const FnlDrawState *ds)
{
	struct RendererState *g = D3D(a);
	const int w = a->scene_extent.w, h = a->scene_extent.h;
	const bool ms = scene_msaa(a, ds);
	D3dTarget *t = ms ? &g->scene_ms : &g->scene;
	const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
		d3d_rtv(a, ms ? RTV_SCENE_MS : RTV_SCENE);
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
		d3d_dsv(a, ms ? DSV_SCENE_MS : DSV_SCENE);

	target_to(a, t, D3D12_RESOURCE_STATE_RENDER_TARGET);
	begin_target(a, &rtv, &dsv, w, h);
	record_scene(a, ds, rtv, dsv,
				 ms ? FNL_TARGET_OFFSCREEN_MS : FNL_TARGET_OFFSCREEN, 0, 0,
				 w, h);
	d3d_own_state(a);

	if (ms) {
		/* Resolved in the _SRGB format, so the samples are averaged in
		 * linear light, as Vulkan's resolve attachment does. */
		target_to(a, &g->scene_ms, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
		target_to(a, &g->scene, D3D12_RESOURCE_STATE_RESOLVE_DEST);
		ID3D12GraphicsCommandList_ResolveSubresource(
			g->list, g->scene.color, 0, g->scene_ms.color, 0,
			D3D_COLOR_FORMAT);
	}
	target_to(a, &g->scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	if (scene_mlaa(a, ds)) {
		const D3D12_CPU_DESCRIPTOR_HANDLE mrtv = d3d_rtv(a, RTV_MLAA);
		target_to(a, &g->mlaa, D3D12_RESOURCE_STATE_RENDER_TARGET);
		begin_target(a, &mrtv, NULL, w, h);
		d3d_fetch(a, g->pso_mlaa, SRV_SCENE);
		target_to(a, &g->mlaa, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
}

/* The scene onto the window's image, under the overlay: drawn straight in at
 * the area's own resolution, or the image it was rendered into fetched
 * across the letterboxed rectangle that keeps its aspect. */
static void draw_scene(App *a, const FnlDrawState *ds,
					   D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
	struct RendererState *g = D3D(a);
	int ax, ay, aw, ah;
	content_px(a, ds, &ax, &ay, &aw, &ah);
	if (aw < 1 || ah < 1)
		return;

	int dx = 0, dy = 0, dw = aw, dh = ah;
	const bool offscreen = scene_offscreen(a, ds);
	if (offscreen)
		scene_dst_rect(ds->scene_w, ds->scene_h, aw, ah, &dx, &dy, &dw, &dh);
	set_rect(a, ax + dx, ay + dy, dw, dh);

	if (!offscreen) {
		record_scene(a, ds, rtv, d3d_dsv(a, DSV_WINDOW), FNL_TARGET_DIRECT,
					 ax + dx, ay + dy, dw, dh);
		return;
	}
	/* No depth under the fetch: its pipeline states, as every one of the
	 * library's, are built with none (pso_make()), and a depth bound
	 * against them is a mismatch the debug layer reports. The window's
	 * depth is for a scene drawn straight in; the overlay after binds
	 * none either (draw()). */
	ID3D12GraphicsCommandList_OMSetRenderTargets(g->list, 1, &rtv, FALSE,
												 NULL);
	if (scene_mlaa_direct(a, ds))
		d3d_fetch(a, g->pso_mlaa, SRV_SCENE);
	else
		d3d_fetch(a, g->pso_blit, scene_mlaa(a, ds) ? SRV_MLAA : SRV_SCENE);
}

/* The transparent grab ring, as the four strips the window rectangle leaves
 * over, each cleared to alpha zero -- as the Vulkan renderer clears them, and
 * for its reason. Only where there is a ring, which is Wayland's, so never
 * here today; kept so that a margin is honoured wherever it comes from. */
static void clear_grab_ring(App *a, D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
	FnlRect win;
	if (a->draw_margin <= 0 ||
		!overlay_clip(a, 0, 0, logical_w(a), logical_h(a), &win))
		return;
	const int fw = a->extent.w, fh = a->extent.h;
	const int wr = win.x + win.w, wb = win.y + win.h;
	const D3D12_RECT ring[4] = {
		{ 0, 0, fw, win.y },                    /* top    */
		{ 0, wb, fw, fh },                      /* bottom */
		{ 0, win.y, win.x, wb },                /* left   */
		{ wr, win.y, fw, wb },                  /* right  */
	};
	D3D12_RECT keep[4];
	UINT n = 0;
	for (int i = 0; i < 4; i++)
		if (ring[i].right > ring[i].left && ring[i].bottom > ring[i].top)
			keep[n++] = ring[i];
	if (n)
		ID3D12GraphicsCommandList_ClearRenderTargetView(D3D(a)->list, rtv,
														CLEAR, n, keep);
}

/* Everything drawn this frame, into the back buffer `img`, which is a
 * render target throughout. False is a frame to bail out of: the overlay
 * could not be drawn. */
static bool draw(App *a, const FnlDrawState *ds, uint32_t slot, UINT img)
{
	struct RendererState *g = D3D(a);
	if (scene_offscreen(a, ds))
		scene_pass(a, ds);

	const D3D12_CPU_DESCRIPTOR_HANDLE rtv = d3d_rtv(a, RTV_BACK0 + (int)img);
	const D3D12_CPU_DESCRIPTOR_HANDLE dsv = d3d_dsv(a, DSV_WINDOW);
	begin_target(a, &rtv, &dsv, a->extent.w, a->extent.h);
	clear_grab_ring(a, rtv);
	draw_scene(a, ds, rtv);

	/* The overlay over the whole image, in a state of the library's own:
	 * on_record may have left anything set. No depth. */
	d3d_own_state(a);
	ID3D12GraphicsCommandList_OMSetRenderTargets(g->list, 1, &rtv, FALSE,
												 NULL);
	set_rect(a, 0, 0, a->extent.w, a->extent.h);
	g->overlay_on = true;
	const FnlResult orc = draw_overlay(a, ds, slot);
	g->overlay_on = false;
	return orc >= 0;
}

/* ---- the frame ----------------------------------------------------------- */

/* Wait until the GPU has finished the frame that last used this slot, whose
 * allocator and arenas are about to be reused. False on a loss. */
static bool wait_slot(App *a, uint32_t slot)
{
	struct RendererState *g = D3D(a);
	const uint64_t v = g->slot_value[slot];
	if (ID3D12Fence_GetCompletedValue(g->fence) >= v) {
		if (a->timing && v)
			a->fc_idle++;
		return true;
	}
	while (ID3D12Fence_GetCompletedValue(g->fence) < v) {
		if (d3d_device_lost(a))
			return false;
		ID3D12Fence_SetEventOnCompletion(g->fence, v, g->fence_event);
		WaitForSingleObject(g->fence_event, FENCE_WAIT_MS);
	}
	return true;
}

/* The GPU's time on this slot's last frame, which the slot's fence --
 * waited for before the list was reset -- says is done: once a frame. */
static void gpu_time_read(App *a, uint32_t slot)
{
	struct RendererState *g = D3D(a);
	if (!g->ts_read || !g->ts_unread[slot])
		return;
	const D3D12_RANGE r = { 2 * slot * sizeof(UINT64),
							2 * (slot + 1) * sizeof(UINT64) };
	UINT64 *t = NULL;
	if (FAILED(ID3D12Resource_Map(g->ts_read, 0, &r, (void **)&t)))
		return;
	const UINT64 t0 = t[2 * slot], t1 = t[2 * slot + 1];
	const D3D12_RANGE none = { 0, 0 };
	ID3D12Resource_Unmap(g->ts_read, 0, &none);
	g->ts_unread[slot] = false;
	if (t1 >= t0)
		app_gpu_time(a, (double)(t1 - t0) / (double)g->ts_freq,
					 g->ts_index[slot]);
}

/* Every slot whose fence has passed, which is what says its pair is in. */
void d3d_gpu_time_poll(App *a)
{
	struct RendererState *g = D3D(a);
	if (!g->fence)
		return;
	const uint64_t done = ID3D12Fence_GetCompletedValue(g->fence);
	for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		if (g->ts_unread[i] && done >= g->slot_value[i])
			gpu_time_read(a, i);
}

/* Close the list and submit it, and fence the slot: whatever was recorded
 * reads the slot's arenas until the fence passes. False on a loss. */
static bool submit(App *a, uint32_t slot)
{
	struct RendererState *g = D3D(a);
	g->recording = false;
	HRESULT hr = ID3D12GraphicsCommandList_Close(g->list);
	if (FAILED(hr)) {
		if (!d3d_hr_lost(a, hr))
			fnl_log(FNL_LOG_ERROR, "the frame's list did not close "
					"(0x%08lx)", (unsigned long)hr);
		return false;
	}
	ID3D12CommandList *lists[1] = { (ID3D12CommandList *)g->list };
	ID3D12CommandQueue_ExecuteCommandLists(g->queue, 1, lists);
	hr = ID3D12CommandQueue_Signal(g->queue, g->fence, g->fenced + 1);
	if (FAILED(hr)) {
		/* Nothing then says when the slot is free: a value never queued
		 * would be waited for for good. Rebuilt instead, as on a loss --
		 * once the queue has passed the last value it took, since the
		 * device is not gone and the teardown waits for nothing on a loss.
		 * The list just submitted is behind no fence, so that is as near
		 * idle as can be waited for (d3d_wait_idle()). */
		if (!d3d_hr_lost(a, hr)) {
			fnl_log(FNL_LOG_ERROR, "the frame's fence was not signalled "
					"(0x%08lx); rebuilding", (unsigned long)hr);
			(void)d3d_wait_idle(a);
			a->device_lost = true;
		}
		return false;
	}
	g->slot_value[slot] = ++g->fenced;
	return true;
}

/* The frame just presented, waited for until the display has it: the
 * swapchain's count of presents against the count of the last one shown
 * (DXGI_FRAME_STATISTICS::PresentCount), a vertical blank at a time. On
 * the shown waiter's thread, or the render thread's where there is none;
 * either way after the present, whose count `want` is, and with the
 * swapchain neither resized nor released meanwhile (d3d_shown_idle()).
 * Answers whether the statistics were seen at all, which the caller
 * folds into no_stats under shown_lock.
 *
 * Present returns with the frame queued, not shown, and on Win32 the size
 * it answers is released then (size_await()), so Windows applied the next
 * size while this frame was still queued. DESIGN *Measured on Windows
 * itself* has the drag that counted it, and the two signals that were
 * tried instead: the frame-latency object, whose count runs ahead of the
 * frames after the skipped waits, and DwmFlush, which returns when the
 * desktop has composed and not when it has composed this frame. Frame
 * statistics and the vertical blank are DXGI's own, so nothing joins the
 * link line. Waited a vertical blank at a time, which releases the size
 * just after a blank: three rounds of the drag each way, interleaved on
 * the GTX 1050 Ti, let through 22 to 27 frames of another size in 45
 * waited so and 30 to 31 polled every half millisecond. WaitForVBlank
 * takes no timeout, so the time is read after each. An output that is not
 * scanning is reported to answer at once rather than not at all (games
 * spin on it with a display off), which ends here at ACK_SHOWN_VBLANKS;
 * one that does not answer is not handled, and not measured here.
 * Under wine the statistics are polled every millisecond instead, within
 * ACK_SHOWN_WAIT_S: DXVK's WaitForVBlank waits for no blank but for one
 * estimated from the refresh rate, spinning on the clock through the end
 * of it, which was most of a drag's CPU there (DESIGN *Measured on
 * Windows itself*).
 * DISJOINT, which the statistics say while they cannot be trusted, is
 * waited through; a lost device is the next frame's to find
 * (d3d_device_lost()); a swapchain that answers anything else is not asked
 * again. */
typedef enum { SHOWN_SEEN, SHOWN_UNSEEN, SHOWN_REFUSED } ShownSaid;

static ShownSaid wait_shown(App *a, UINT want, IDXGIOutput *out)
{
	struct RendererState *g = D3D(a);
	const double end = now_seconds() + ACK_SHOWN_WAIT_S;
	bool seen = false;
	for (int blanks = 0;; ) {
		DXGI_FRAME_STATISTICS st;
		const HRESULT hr = IDXGISwapChain3_GetFrameStatistics(g->swapchain,
															  &st);
		if (SUCCEEDED(hr)) {
			seen = true;
			/* Signed, so a count that wrapped still orders. */
			if ((int32_t)(st.PresentCount - want) >= 0)
				break;
		} else if (hr == DXGI_ERROR_DEVICE_REMOVED ||
				   hr == DXGI_ERROR_DEVICE_RESET ||
				   hr == DXGI_ERROR_DEVICE_HUNG) {
			seen = true;            /* nothing to count against the stats */
			break;
		} else if (hr != DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
			fnl_log(FNL_LOG_INFO, "no frame statistics (0x%08lx): a frame "
					"that answers a size is not waited for until shown",
					(unsigned long)hr);
			return SHOWN_REFUSED;
		}
		if (now_seconds() >= end ||
			(!g->blank_estimated && blanks == ACK_SHOWN_VBLANKS))
			break;
		blanks++;
		if (!out || FAILED(IDXGIOutput_WaitForVBlank(out)))
			Sleep(1);
	}
	return seen ? SHOWN_SEEN : SHOWN_UNSEEN;
}

/* What one wait said, kept for the next: a swapchain that refuses the
 * statistics, or gives none in DISJOINT_GIVE_UP waits, is not asked
 * again. */
static void shown_note(App *a, ShownSaid said)
	FNL_REQUIRES(D3D(a)->shown_lock)
{
	struct RendererState *g = D3D(a);
	if (said == SHOWN_SEEN) {
		g->stats_unseen = 0;
	} else if (said == SHOWN_REFUSED) {
		g->no_stats = true;
	} else if (!g->no_stats && ++g->stats_unseen >= DISJOINT_GIVE_UP) {
		g->no_stats = true;
		fnl_log(FNL_LOG_INFO, "no frame statistics in %d waits: a frame "
				"that answers a size is not waited for until shown",
				DISJOINT_GIVE_UP);
	}
}

/* ---- the shown waiter -----------------------------------------------------
 *
 * Waited for on the render thread, the frame that answers a size held the
 * loop until it was shown, and every frame of a drag after it started past
 * the refresh it could have made: Vulkan's measure on X11 (vulkan/frame.c),
 * and on Windows the window's thread waits for the size besides
 * (size_await()). So the wait is made on a thread of the renderer's, which
 * releases the size itself. */

static void *shown_main(void *arg)
{
	App *a = arg;
	struct RendererState *g = D3D(a);

	fnl_mutex_lock(&g->shown_lock);
	for (;;) {
		while (!g->shown_busy && !g->shown_quit)
			fnl_cond_wait_for(&g->shown_cond, &g->shown_lock, 1.0);
		if (!g->shown_busy)
			break;
		const UINT want = g->shown_want;
		IDXGIOutput *out = g->shown_out;
		g->shown_out = NULL;
		fnl_mutex_unlock(&g->shown_lock);

		const ShownSaid said = wait_shown(a, want, out);
		if (out)
			IDXGIOutput_Release(out);
		fnl_atomic_store_bool(&a->ack_held, false);
		BACKEND(a)->ack_release(a);

		fnl_mutex_lock(&g->shown_lock);
		shown_note(a, said);
		g->shown_busy = false;
		fnl_cond_signal(&g->shown_cond);
	}
	fnl_mutex_unlock(&g->shown_lock);
	return NULL;
}

/* Whether the waiter is there, starting it the first time. Its lock is made
 * either way, and outlives a thread that could not be had. */
static bool shown_start(App *a)
{
	struct RendererState *g = D3D(a);
	if (!g->shown_ready) {
		fnl_mutex_init(&g->shown_lock);
		fnl_cond_init(&g->shown_cond);
		g->shown_busy = g->shown_quit = false;
		g->shown_out = NULL;
		g->shown_ready = true;
		g->shown_started = fnl_thread_start(&g->shown_thread, shown_main, a);
	}
	return g->shown_started;
}

/* ---- the way to the screen ------------------------------------------------
 *
 * How many presents at rest a window is measured over, and how many of
 * them late -- two frames or more between a present and the one on
 * screen, where a flip has one -- say DWM composes it: on the HD 530 a
 * composed window read three or more on 478 presents of 480, and the GTX
 * 960M's flipped one one on 597 of 600 (present_d3d11.c). */
#define VIA11_MEASURE 60
#define VIA11_LATE    54

/* After a present that was shown: one more measured, with vsync on, at
 * rest and with buffers the window's size -- a drag's larger ones are
 * composed whichever device presents them -- and never on a software
 * adapter, which has no plane, or under wine, whose statistics are
 * estimated. Composed on this device's own swapchain, the next frame makes
 * one of a Direct3D 11 device; composed on that as well -- another window
 * over it, the taskbar over its edge -- it goes back; flipped, it stays.
 * Either way it is measured again at its next size, which is where the
 * window that was covered is likely clear: settled for good, a window first
 * measured under the taskbar would keep a composed way for its life. Asked
 * as shown_stamp() asks, not while the shown waiter is. */
static void via11_measure(App *a)
{
	struct RendererState *g = D3D(a);
	if (g->via11_settled && !g->via11_forced && a->size_at != g->via11_at) {
		g->via11_settled = false;
		g->via11_seen = g->via11_late = 0;
	}
	if (g->via11_settled || g->interval != 1 || g->software ||
		g->blank_estimated)
		return;
	if (!app_buffer_at_rest(a) || a->buffer.w > 0) {
		g->via11_seen = g->via11_late = 0;
		return;
	}
	bool ask = true;
	if (g->shown_ready) {
		fnl_mutex_lock(&g->shown_lock);
		ask = !g->no_stats && !g->shown_busy;
		fnl_mutex_unlock(&g->shown_lock);
	}
	UINT last = 0;
	DXGI_FRAME_STATISTICS st;
	if (!ask ||
		FAILED(IDXGISwapChain3_GetLastPresentCount(g->swapchain, &last)) ||
		FAILED(IDXGISwapChain3_GetFrameStatistics(g->swapchain, &st)))
		return;
	g->via11_seen++;
	if (last - st.PresentCount >= 2)
		g->via11_late++;
	if (g->via11_seen < VIA11_MEASURE)
		return;
	const bool composed = g->via11_late >= VIA11_LATE ||
						  FNL_FAULT("d3d12-composed");
	g->via11_seen = g->via11_late = 0;
	if (!g->via11_on && composed) {
		fnl_log(FNL_LOG_INFO, "DWM composes this window's frames; they go "
				"through a Direct3D 11 swapchain instead");
		g->via11_on = g->via11_switch = true;
		return;
	}
	if (g->via11_on && composed) {
		fnl_log(FNL_LOG_INFO, "DWM composes the Direct3D 11 swapchain's "
				"frames too; back to this device's own");
		g->via11_on = false;
		g->via11_switch = true;
	}
	g->via11_settled = true;
	g->via11_at = a->size_at;
}

/* The swapchain made again from the other device (via11_measure()), at the
 * size it had. One the Direct3D 11 device cannot make is this device's
 * again, and the window is measured no more. False where neither could be
 * made, which raises the loss. */
static bool via11_remake(App *a)
{
	struct RendererState *g = D3D(a);
	g->via11_switch = false;
	for (int tries = 0; tries < 2; tries++) {
		if (!d3d_wait_idle(a))
			return false;
		d3d_release_window_images(a);
		d3d_shown_stop(a);
		if (g->frame_wait) {
			CloseHandle(g->frame_wait);
			g->frame_wait = NULL;
		}
		D3D_RELEASE(g->swapchain);
		if (!g->via11_on)
			d3d_via11_end(a);
		if (d3d_bridge_swapchain(a) == FNL_OK &&
			d3d_window_images(a) == FNL_OK)
			return true;
		if (!g->via11_on)
			break;
		fnl_log(FNL_LOG_WARN, "the Direct3D 11 swapchain could not be "
				"made; back to this device's own");
		g->via11_on = false;
		g->via11_settled = g->via11_forced = true;
	}
	if (!d3d_device_lost(a))
		a->device_lost = true;
	return false;
}

/* For fnl_frame_shown(): the number DXGI gave the present just made, as
 * frame `index`, and the number it last showed with the vertical blank's
 * QPC time, which is now_seconds()'s clock. Not asked while the shown
 * waiter is asking (shown_busy): the frames of a drag go unstamped rather
 * than two threads reading the statistics at once. A tearing present is
 * shown when it lands, which is not a blank. Never waits. */
static void shown_stamp(App *a, uint64_t index, double at)
{
	struct RendererState *g = D3D(a);
	UINT number;
	if (FAILED(IDXGISwapChain3_GetLastPresentCount(g->swapchain, &number)))
		return;
	app_present_numbered(a, index, number, at);
	bool ask = true;
	if (g->shown_ready) {
		fnl_mutex_lock(&g->shown_lock);
		ask = !g->no_stats && !g->shown_busy;
		fnl_mutex_unlock(&g->shown_lock);
	}
	DXGI_FRAME_STATISTICS st;
	LARGE_INTEGER freq;
	if (!ask || FAILED(IDXGISwapChain3_GetFrameStatistics(g->swapchain,
														  &st)) ||
		!QueryPerformanceFrequency(&freq) || !freq.QuadPart)
		return;
	app_present_number_shown(a, st.PresentCount,
							 (double)st.SyncQPCTime.QuadPart /
								 (double)freq.QuadPart,
							 !(g->interval == 0 &&
							   (g->sc_flags &
								DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)));
}

/* The frame just presented answers a size: handed to the waiter, or waited
 * for here where there is none. Nothing where the statistics are not to be
 * had, and the flush after the present releases the size as it is. */
static void shown_begin(App *a)
{
	struct RendererState *g = D3D(a);
	const bool threaded = shown_start(a);
	fnl_mutex_lock(&g->shown_lock);
	const bool no_stats = g->no_stats;
	fnl_mutex_unlock(&g->shown_lock);
	UINT want = 0;
	if (no_stats ||
		FAILED(IDXGISwapChain3_GetLastPresentCount(g->swapchain, &want)))
		return;
	IDXGIOutput *out = NULL;
	if (!g->blank_estimated &&
		FAILED(IDXGISwapChain3_GetContainingOutput(g->swapchain, &out)))
		out = NULL;
	if (!threaded) {
		const ShownSaid said = wait_shown(a, want, out);
		if (out)
			IDXGIOutput_Release(out);
		fnl_mutex_lock(&g->shown_lock);
		shown_note(a, said);
		fnl_mutex_unlock(&g->shown_lock);
		return;
	}
	/* Ahead of the flush after this present, which keeps the size. */
	fnl_atomic_store_bool(&a->ack_held, true);
	fnl_mutex_lock(&g->shown_lock);
	g->shown_want = want;
	g->shown_out = out;
	g->shown_busy = true;
	fnl_mutex_unlock(&g->shown_lock);
	fnl_cond_signal(&g->shown_cond);
}

void d3d_shown_idle(App *a)
{
	struct RendererState *g = D3D(a);
	if (!g->shown_started)
		return;
	fnl_mutex_lock(&g->shown_lock);
	while (g->shown_busy)
		fnl_cond_wait_for(&g->shown_cond, &g->shown_lock, 1.0);
	fnl_mutex_unlock(&g->shown_lock);
}

void d3d_shown_stop(App *a)
{
	struct RendererState *g = D3D(a);
	if (!g->shown_ready)
		return;
	if (g->shown_started) {
		fnl_mutex_lock(&g->shown_lock);
		g->shown_quit = true;
		fnl_mutex_unlock(&g->shown_lock);
		fnl_cond_signal(&g->shown_cond);
		fnl_thread_join(g->shown_thread);
	}
	fnl_cond_destroy(&g->shown_cond);
	fnl_mutex_destroy(&g->shown_lock);
	g->shown_ready = g->shown_started = false;
}

/* ---- the snapshot -------------------------------------------------------- */

/* The back buffer copied into a readback buffer at the end of the list,
 * for the snapshot a move takes (App::snap_owed): rows on the pitch
 * alignment, the buffer made for the largest extent yet, after a drain
 * where one before it may still be a list's. False with the snapshot
 * declined where the buffer cannot be had. */
static bool snapshot_record(App *a, ID3D12Resource *back)
{
	struct RendererState *g = D3D(a);
	const size_t row = (size_t)a->extent.w * 4u;
	const size_t pitch = (row + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
					   & ~(size_t)(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	const size_t need = pitch * (size_t)a->extent.h;
	if (need > g->snap_read_bytes) {
		if (g->snap_read && !d3d_wait_idle(a)) {
			app_snapshot_decline(a);
			return false;
		}
		D3D_RELEASE(g->snap_read);
		g->snap_read_bytes = 0;
		const D3D12_HEAP_PROPERTIES rb = { .Type = D3D12_HEAP_TYPE_READBACK };
		const D3D12_RESOURCE_DESC bd = {
			.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
			.Width = (UINT64)need,
			.Height = 1,
			.DepthOrArraySize = 1,
			.MipLevels = 1,
			.Format = DXGI_FORMAT_UNKNOWN,
			.SampleDesc = { 1, 0 },
			.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
		};
		if (FAILED(ID3D12Device_CreateCommittedResource(
				g->device, &rb, D3D12_HEAP_FLAG_NONE, &bd,
				D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
				(void **)&g->snap_read))) {
			g->snap_read = NULL;
			app_snapshot_decline(a);
			return false;
		}
		g->snap_read_bytes = need;
	}
	g->snap_pitch = pitch;
	d3d_barrier(a, back, D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_COPY_SOURCE);
	/* The footprint in the back buffer's own format: the swapchain's, or
	 * the _SRGB of the images shared with Direct3D 11 (present_d3d11.c).
	 * The same bytes either way. */
	const D3D12_TEXTURE_COPY_LOCATION dst = {
		.pResource = g->snap_read,
		.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
		.PlacedFootprint = {
			.Offset = 0,
			.Footprint = {
				.Format = g->via11 ? D3D_COLOR_FORMAT : D3D_BACK_FORMAT,
				.Width = (UINT)a->extent.w,
				.Height = (UINT)a->extent.h,
				.Depth = 1,
				.RowPitch = (UINT)pitch,
			},
		},
	};
	const D3D12_TEXTURE_COPY_LOCATION src = {
		.pResource = back,
		.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
		.SubresourceIndex = 0,
	};
	/* The window's part: the buffer can be larger (d3d_window_images()). */
	const D3D12_BOX box = { 0, 0, 0, (UINT)a->extent.w, (UINT)a->extent.h,
							1 };
	ID3D12GraphicsCommandList_CopyTextureRegion(g->list, &dst, 0, 0, 0,
												&src, &box);
	d3d_barrier(a, back, D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET);
	return true;
}

/* Once the slot's fence says the list has run: the rows handed over, with
 * the pitch they were copied at. The one stall a move costs here. */
static void snapshot_take(App *a, uint32_t slot)
{
	struct RendererState *g = D3D(a);
	const uint64_t v = g->slot_value[slot];
	while (ID3D12Fence_GetCompletedValue(g->fence) < v) {
		if (d3d_device_lost(a)) {
			app_snapshot_decline(a);
			return;
		}
		ID3D12Fence_SetEventOnCompletion(g->fence, v, g->fence_event);
		WaitForSingleObject(g->fence_event, FENCE_WAIT_MS);
	}
	const D3D12_RANGE all = { 0, g->snap_pitch * (size_t)a->extent.h };
	void *map = NULL;
	if (FAILED(ID3D12Resource_Map(g->snap_read, 0, &all, &map))) {
		app_snapshot_decline(a);
		return;
	}
	app_snapshot_set(a, map, a->extent.w, a->extent.h, g->snap_pitch,
					 false);
	const D3D12_RANGE none = { 0, 0 };
	ID3D12Resource_Unmap(g->snap_read, 0, &none);
}

bool d3d_draw_frame(App *a, uint32_t frame, const FnlDrawState *ds,
					uint32_t cfg_serial, bool cfg_pending)
{
	struct RendererState *g = D3D(a);
	const double t_enter = a->timing ? now_seconds() : 0.0;
	a->present_block = -1.0;
	a->present_untimed = g->software;

	/* MAX_FRAMES_IN_FLIGHT slots, one a turn: the frame index is the slot,
	 * and the allocator and the overlay's arenas are the slot's. */
	const uint32_t slot = frame % MAX_FRAMES_IN_FLIGHT;

	const double t_fence = now_seconds();
	if (!wait_slot(a, slot))
		return false;
	if (g->via11_switch && !via11_remake(a))
		return false;
	/* The swapchain's throttle, as Vulkan's acquire is: signalled once the
	 * swapchain will take another frame. Not a device wait, so not in
	 * device_block; added to the present's, as an acquire's block is.
	 *
	 * Never on the frame that answers a size (cfg_pending). The window's
	 * thread is held in size_await() until that frame is shown, and under
	 * wine on Wayland the object is not signalled while it is: measured,
	 * the wait ran out its whole bound and so did the size's, on every
	 * maximize, and the window stopped waiting on sizes. Skipped, the
	 * present goes through (110 to 175 ms at 4K there) and the size is
	 * answered; the frame after waits as usual.
	 *
	 * Nor under sync interval 0, which is vsync off: in a window DWM
	 * composes, the object is released as DWM takes the queued frame,
	 * once a refresh, so waiting on it holds "off" to the display's rate
	 * (measured on both laptops: 8 to 22% of a core flat out, where
	 * Vulkan's mailbox costs 23 to 106%). The slot's fence paces it then,
	 * as it paces Vulkan's mailbox, and DXGI keeps the newest frame.
	 *
	 * The present releases the object whether it was waited on or not, so
	 * each skip leaves it a count ahead, which the next wait takes back --
	 * or the throttle would loosen by one a skip. Taken back by waiting,
	 * not by polling: the count is the skipped frame's, released when it
	 * leaves the queue at a refresh, and a poll made just after the
	 * frame's own wait comes before it every time. Polled, the count stays
	 * and two frames are queued for the window's life: a refresh more from
	 * latch to screen on all four laptop GPUs (19.8 ms against 36.4 on the
	 * GTX 960M, PresentMon). A take that runs out TAKE_BACK_MS means none
	 * is coming. */
	/* Two frames queued while the window is being sized, one at rest
	 * (app_buffer_at_rest()). Its buffers are larger than it then, so DWM
	 * composes it, and on aspire's WDDM 2.1 display a composed frame holds
	 * the object a refresh more: one queued, a drag drew 47 fps on the
	 * 960M and 42 on the HD 530 through Direct3D 11, and two hold 60 on
	 * both. Back to one at rest leaves nothing queued: 20.1 ms from latch
	 * to screen after five seconds at two, as without. */
	const UINT latency = app_buffer_at_rest(a) ? 1 : 2;
	if (latency != g->latency &&
		SUCCEEDED(IDXGISwapChain3_SetMaximumFrameLatency(g->swapchain,
														 latency)))
		g->latency = latency;
	const double t_lat = now_seconds();
	a->device_block = t_lat - t_fence;
	bool skipped = false;
	if (g->frame_wait && !g->frame_waited) {
		if (cfg_pending || g->interval == 0) {
			skipped = true;
		} else {
			/* A wait that ran out its bound took nothing from the object,
			 * so it is counted as a skip. */
			if (WaitForSingleObject(g->frame_wait, LATENCY_WAIT_MS) ==
				WAIT_OBJECT_0)
				g->frame_waited = true;
			else
				skipped = true;
			while (g->frame_owed > 0) {
				if (WaitForSingleObject(g->frame_wait, TAKE_BACK_MS) !=
					WAIT_OBJECT_0) {
					g->frame_owed = 0;
					break;
				}
				g->frame_owed--;
			}
		}
	}
	const double t_ready = now_seconds();
	const double lat_block = t_ready - t_lat;
	if (a->timing)
		a->fc_wait = t_ready - t_enter;

	if (d3d_device_lost(a))
		return false;

	/* The half of the offscreen target this frame's antialiasing wants,
	 * built the first time a frame wants it. Nothing to re-record: every
	 * frame is recorded afresh here. */
	if (scene_offscreen(a, ds))
		(void)d3d_scene_ensure_aa(a, ds->aa == FNL_AA_MSAA,
								  ds->aa == FNL_AA_MLAA &&
									  !scene_mlaa_direct(a, ds));

	const double t_rec = now_seconds();
	app_frame_mark(a, FRAME_MARK_REC);
	HRESULT hr = ID3D12CommandAllocator_Reset(g->alloc[slot]);
	if (SUCCEEDED(hr))
		hr = ID3D12GraphicsCommandList_Reset(g->list, g->alloc[slot], NULL);
	if (FAILED(hr)) {
		if (!d3d_hr_lost(a, hr))
			fnl_log(FNL_LOG_ERROR, "the frame's list did not reset "
					"(0x%08lx)", (unsigned long)hr);
		return false;
	}
	g->recording = true;
	gpu_time_read(a, slot);
	if (g->ts_heap)
		ID3D12GraphicsCommandList_EndQuery(g->list, g->ts_heap,
										   D3D12_QUERY_TYPE_TIMESTAMP,
										   2 * slot);

	const UINT img = g->via11
		? d3d_via11_next(a)
		: IDXGISwapChain3_GetCurrentBackBufferIndex(g->swapchain);
	ID3D12Resource *back = g->back[img];
	d3d_barrier(a, back, D3D12_RESOURCE_STATE_PRESENT,
				D3D12_RESOURCE_STATE_RENDER_TARGET);
	const bool drawn = draw(a, ds, slot, img);
	/* The frame drawn is the GPU's time on it: a snapshot's copy is no
	 * part of drawing it, as Vulkan's readback is not (DESIGN *What a
	 * frame costs, compared*). */
	if (g->ts_heap)
		ID3D12GraphicsCommandList_EndQuery(g->list, g->ts_heap,
										   D3D12_QUERY_TYPE_TIMESTAMP,
										   2 * slot + 1);
	/* The frame read back for the snapshot a move takes: copied here,
	 * where the back buffer is still this list's, and handed over once
	 * the list has run (snapshot_take()). */
	const bool snap = drawn && a->snap_owed && snapshot_record(a, back);
	d3d_barrier(a, back, D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PRESENT);
	if (g->ts_heap) {
		ID3D12GraphicsCommandList_ResolveQueryData(
			g->list, g->ts_heap, D3D12_QUERY_TYPE_TIMESTAMP, 2 * slot, 2,
			g->ts_read, 2 * slot * sizeof(UINT64));
	}
	/* The recording alone, as every renderer's `record` is; submitting
	 * is the next phase. */
	if (a->timing)
		a->fc_record = now_seconds() - t_rec;
	/* Submitted either way: the list holds the barriers that put the back
	 * buffer back, and the slot is fenced as a finished frame's is. */
	if (g->via11)
		d3d_via11_wait(a, img);
	if (!submit(a, slot) || !drawn) {
		d3d_drain_messages(a);
		return false;
	}
	if (g->via11 && !d3d_via11_copy(a, img))
		fnl_log(FNL_LOG_WARN, "the frame was not copied into the "
				"Direct3D 11 swapchain");
	g->ts_unread[slot] = g->ts_heap != NULL;
	if (g->ts_unread[slot])
		g->ts_index[slot] = app_gpu_timed(a);
	if (snap)
		snapshot_take(a, slot);

	/* What rides on the present's commit is requested immediately before
	 * it, as on Vulkan (app_surface_sync()); the ack after the waiter has
	 * released the last, whose word must not carry this one out. */
	if (cfg_pending && BACKEND(a)->ack_when_shown)
		d3d_shown_idle(a);
	app_surface_sync(a, cfg_serial, cfg_pending);
	BACKEND(a)->frame_request(a);

	const double t_present = now_seconds();
	app_frame_mark(a, FRAME_MARK_PRESENT);
	a->late_cost = a->late_cost > 0.0
				 ? a->late_cost * 0.9 + (t_present - t_rec) * 0.1
				 : t_present - t_rec;
	if (a->timing)
		a->fc_submit = (t_present - t_rec) - a->fc_record;

	/* Tearing only with an interval of 0 and a swapchain made for it;
	 * DXGI refuses the flag otherwise. */
	const UINT flags = g->interval == 0 &&
					   (g->sc_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
						   ? DXGI_PRESENT_ALLOW_TEARING : 0;
	hr = IDXGISwapChain3_Present(g->swapchain, (UINT)g->interval, flags);
	a->present_calls++;
	const double t_presented = now_seconds();
	app_frame_mark(a, FRAME_MARK_PRESENTED);
	if (a->timing)
		a->fc_present = t_presented - t_present;

	g->occluded = hr == DXGI_STATUS_OCCLUDED;
	/* Every success but the occluded one queues the frame and so releases
	 * the object in its turn. */
	const bool shown = SUCCEEDED(hr) && !g->occluded;
	if (shown) {
		if (a->counting)
			shown_stamp(a, a->presents, t_present);
		via11_measure(a);
		a->presents++;
		g->frame_waited = false;
		/* Released with no wait to match it. Bounded: with vsync off this
		 * is every frame, and an empty object resets it anyway. */
		if (skipped && g->frame_owed < 64)
			g->frame_owed++;
		/* Both halves of the throttle, added. Not on a software adapter,
		 * whose present waits for the rasteriser. */
		if (!g->software)
			a->present_block = lat_block + (t_presented - t_present);
		/* Before the flush, which releases the size. */
		if (cfg_pending && BACKEND(a)->ack_when_shown)
			shown_begin(a);
	}
	BACKEND(a)->flush(a);

	if (!shown) {
		if (FAILED(hr) && !d3d_hr_lost(a, hr))
			fnl_log(FNL_LOG_WARN, "Present failed (0x%08lx)",
					(unsigned long)hr);
		app_frame_take_back(a);
	}
	d3d_drain_messages(a);

	if (FNL_FAULT("device-lost"))
		a->device_lost = true;
	if (FNL_FAULT("surface-lost"))
		a->surface_lost = true;

	/* Drawn and handed over, whatever the present made of it. */
	return true;
}

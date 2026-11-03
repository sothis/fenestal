/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The public knobs: what the window is doing, and what to ask it to do instead.
 *
 * ---- what "safe from any thread" means here -------------------------------
 *
 *   atomics       what both threads touch: the close flag, cost sampling,
 *                 the transition, text entry.
 *
 *   publish       everything else goes through App::lock. The scene size is a
 *                 pair rather than two atomics: a render thread that latched
 *                 a new width with an old height would allocate an image
 *                 nothing asked for.
 *
 *                 Everything describing the *window* comes back out of the
 *                 App::pub_* block the render thread fills once a frame. None
 *                 of it may be computed here from App::extent, draw_margin,
 *                 draw_scale120 or scene_extent: the render thread writes all
 *                 four, and reading them from another thread is undefined
 *                 rather than merely stale.
 *
 * The window-state bools follow the same rule: each is read from its
 * published copy under the lock the writers hold.
 */

#include "core/app.h"

/* ---- reading ------------------------------------------------------------- */

/* The four window states are the want_* copies every backend writes under
 * App::lock. Fullscreen is the state, not the request: on Wayland and X11 it
 * moves when the compositor or the window manager says so. */
bool fnl_is_fullscreen(const FnlApp *app)
{
	App *a = (App *)app;
	if (!a)
		return false;
	fnl_mutex_lock(&a->lock);
	const bool v = a->want_fullscreen;
	fnl_mutex_unlock(&a->lock);
	return v;
}

bool fnl_is_active(const FnlApp *app)
{
	App *a = (App *)app;
	if (!a)
		return false;
	fnl_mutex_lock(&a->lock);
	const bool v = a->want_active;
	fnl_mutex_unlock(&a->lock);
	return v;
}

bool fnl_is_maximized(const FnlApp *app)
{
	App *a = (App *)app;
	if (!a)
		return false;
	fnl_mutex_lock(&a->lock);
	const bool v = a->want_maximized;
	fnl_mutex_unlock(&a->lock);
	return v;
}

bool fnl_is_pointer_captured(const FnlApp *app)
{
	App *a = (App *)app;
	if (!a)
		return false;
	fnl_mutex_lock(&a->lock);
	const bool v = a->want_captured;
	fnl_mutex_unlock(&a->lock);
	return v;
}

/* The one reader outside the render thread, and the reason App::fps_display is
 * an atomic rather than a plain int. See the field's own note in app.h. */
int fnl_fps(const FnlApp *app)
{
	const App *a = (const App *)app;
	return a ? (int)fnl_atomic_load_u32(&a->fps_display) : 0;
}

/* 1 when the device offers nothing worth multisampling at, or the renderer
 * cannot multisample at all. */
int fnl_max_samples(const FnlApp *app)
{
	const App *a = (const App *)app;
	const FnlRendererVtbl *r = a ? renderer_any(a) : NULL;
	return r ? r->max_samples(a) : 1;
}

/* Whether there is anywhere to render the scene but the window itself. The
 * render scale, FSAA and MLAA are one target between them, so this is one
 * question for all three; fnl_max_samples() covers FSAA and nothing else. */
bool fnl_offscreen_supported(const FnlApp *app)
{
	const App *a = (const App *)app;
	const FnlRendererVtbl *r = a ? renderer_any(a) : NULL;
	return r && r->offscreen_scene;
}

/* One mode, not two flags: MSAA and MLAA solve the same problem twice and the
 * frame only ever takes one answer. */
FnlAntialias fnl_antialias(const FnlApp *app)
{
	App *a = (App *)app;
	if (!a)
		return FNL_AA_NONE;
	fnl_mutex_lock(&a->lock);
	const FnlAntialias aa = a->want_aa;
	fnl_mutex_unlock(&a->lock);
	return aa;
}

/* What exists, not what was asked for. With nothing pinned, a pin larger
 * than the area, or a target the device refused, the scene is rendered at
 * the area's own size, so that is the answer rather than 0x0. */
void fnl_scene_size_px(const FnlApp *app, int *w, int *h)
{
	App *a = (App *)app;
	int sw = 0, sh = 0;
	if (a) {
		/* Published rather than read from App::scene_extent, which
		 * scene_set_size() writes on the render thread while this call is
		 * [any]. */
		fnl_mutex_lock(&a->lock);
		sw = a->pub_scene_w;
		sh = a->pub_scene_h;
		fnl_mutex_unlock(&a->lock);
	}
	if (sw > 0 && sh > 0) {
		if (w) *w = sw;
		if (h) *h = sh;
		return;
	}
	fnl_content_size_px(app, w, h);
}

void fnl_content_size_px(const FnlApp *app, int *w, int *h)
{
	App *a = (App *)app;
	int cw = 0, ch = 0;
	if (a) {
		fnl_mutex_lock(&a->lock);
		cw = a->pub_area_w_px;
		ch = a->pub_area_h_px;
		fnl_mutex_unlock(&a->lock);
	}
	if (w) *w = cw;
	if (h) *h = ch;
}

/* A call rather than a macro, so a caller cannot filter against a stale copy
 * of the library's figure. */
void fnl_min_content_size_pt(const FnlApp *app, int *w, int *h)
{
	(void)app;
	if (w) *w = CONTENT_MIN_W;
	if (h) *h = CONTENT_MIN_H;
}

/* The same area in logical points, origin included. content_rect() is the
 * decoration layout's own function, so this cannot drift from where the
 * titlebar actually leaves off. */
void fnl_content_rect_pt(const FnlApp *app, int *x, int *y, int *w, int *h)
{
	App *a = (App *)app;
	int cx = 0, cy = 0, cw = 0, ch = 0;
	if (a) {
		fnl_mutex_lock(&a->lock);
		cx = a->pub_cx;
		cy = a->pub_cy;
		cw = a->pub_cw;
		ch = a->pub_ch;
		fnl_mutex_unlock(&a->lock);
	}
	if (x) *x = cx;
	if (y) *y = cy;
	if (w) *w = cw;
	if (h) *h = ch;
}

/* ---- writing ------------------------------------------------------------- */

/* Everything the caller draws that the library cannot see, as one counter.
 *
 * Under the lock, since compiler.h has no atomic add and a lost bump is a
 * frame that never re-records. Signalled for a loop at rest, where the next
 * frame is not already coming; a hold it lands in waits out what is left of
 * itself (hold_wait()). */
void fnl_invalidate(FnlApp *app)
{
	App *a = (App *)app;
	if (!a)
		return;
	fnl_mutex_lock(&a->lock);
	a->want_user_gen++;
	fnl_cond_signal(&a->cond);
	fnl_mutex_unlock(&a->lock);
}

/* ---- the knobs that reach the window server --------------------------------
 *
 * Each records a request under App::lock and wakes the pump with
 * app_wakeup(), which spends it on its own thread -- see App::req and
 * app_drain_requests().
 *
 * They cannot act where they are called. The backend calls behind them read
 * and write the pump's own unguarded state, and a lock here would not fix it:
 * destroying a Wayland proxy the pump may be dispatching an event to is a
 * use-after-free in libwayland, not a torn read. Forwarding is what lets the
 * block be [any], which it has to be -- an application answers its events in
 * on_overlay, on the render thread. */

void fnl_set_fullscreen(FnlApp *app, bool on)
{
	App *a = (App *)app;
	if (!a || !a->backend)
		return;
	fnl_mutex_lock(&a->lock);
	a->req.fullscreen     = true;
	a->req.fullscreen_val = on;
	fnl_mutex_unlock(&a->lock);
	app_wakeup(a);
}

void fnl_minimize(FnlApp *app)
{
	App *a = (App *)app;
	if (!a || !a->backend)
		return;
	fnl_mutex_lock(&a->lock);
	a->req.minimize = true;
	fnl_mutex_unlock(&a->lock);
	app_wakeup(a);
}

void fnl_set_maximized(FnlApp *app, bool on)
{
	App *a = (App *)app;
	if (!a || !a->backend)
		return;
	fnl_mutex_lock(&a->lock);
	a->req.maximized     = true;
	a->req.maximized_val = on;
	fnl_mutex_unlock(&a->lock);
	app_wakeup(a);
}

/* Both refuse rather than half-act: nothing to drag in fullscreen, no edge to
 * pull while maximized, and a captured pointer is not the user's to hand
 * over.
 *
 * Tested on the *published* copies, inside the critical section that records
 * the request: the is_* fields are the pump thread's and these calls are
 * [any], and a request must not be recorded against a window that stopped
 * qualifying between the guard and the store. */
bool fnl_begin_move(FnlApp *app)
{
	App *a = (App *)app;
	if (!a || !a->backend)
		return false;

	fnl_mutex_lock(&a->lock);
	const bool ok = !a->want_fullscreen && !a->want_captured;
	if (ok)
		a->req.move = true;
	fnl_mutex_unlock(&a->lock);

	if (!ok)
		return false;
	app_wakeup(a);
	return true;
}

bool fnl_begin_resize(FnlApp *app, FnlEdge edge)
{
	App *a = (App *)app;
	if (!a || !a->backend)
		return false;
	if (edge <= FNL_EDGE_NONE || edge > FNL_EDGE_BOTTOM_RIGHT)
		return false;

	fnl_mutex_lock(&a->lock);
	const bool ok = !a->want_fullscreen && !a->want_captured &&
					!a->want_maximized;
	if (ok) {
		a->req.resize      = true;
		a->req.resize_edge = edge;
	}
	fnl_mutex_unlock(&a->lock);

	if (!ok)
		return false;
	app_wakeup(a);
	return true;
}

/* Read by the render loop every turn, at rest too: nothing to wake. */
void fnl_set_cost_sampling(FnlApp *app, bool on)
{
	App *a = (App *)app;
	if (a)
		fnl_atomic_store_bool(&a->cost_sampling, on);
}

unsigned fnl_output_refresh_mhz(const FnlApp *app)
{
	App *a = (App *)app;
	return a ? fnl_atomic_load_u32(&a->out_refresh_mhz) : 0;
}

void fnl_set_scene_motion(FnlApp *app, bool on)
{
	App *a = (App *)app;
	if (!a)
		return;
	fnl_atomic_store_bool(&a->scene_motion, on);
	/* A window at rest draws nothing until something wakes it, and the
	 * clock starting is a change it cannot see until it draws. */
	fnl_invalidate(app);
}

bool fnl_scene_motion(const FnlApp *app)
{
	App *a = (App *)app;
	return a && fnl_atomic_load_bool(&a->scene_motion);
}

void fnl_set_frame_stats(FnlApp *app, bool on)
{
	App *a = (App *)app;
	if (!a)
		return;
	if (on)
		fnl_atomic_store_bool(&a->stats_ever, true);
	fnl_atomic_store_bool(&a->stats_on, on);
}

bool fnl_frame_stats(const FnlApp *app, FnlFrameStats *out)
{
	App *a = (App *)app;
	if (!a || !out || out->size < sizeof out->size ||
		!fnl_atomic_load_bool(&a->stats_ever))
		return false;
	FnlFrameStats st;
	fnl_mutex_lock(&a->lock);
	st = a->pub_st;
	fnl_mutex_unlock(&a->lock);
	/* A prefix, for a caller built against a struct that ended sooner;
	 * the struct only ever grows at its end. */
	const size_t n = out->size < sizeof st ? out->size : sizeof st;
	st.size = n;
	memcpy(out, &st, n);
	return true;
}

int fnl_frame_samples(FnlApp *app, FnlFrameSample *out, int max,
					  size_t stride, uint64_t *lost)
{
	App *a = (App *)app;
	if (lost)
		*lost = 0;
	if (!a || !out || max <= 0 || stride < sizeof out->index)
		return 0;
	/* A prefix of each, for a caller built against a shorter struct. */
	const size_t each = stride < sizeof *out ? stride : sizeof *out;
	const double now = now_seconds() - a->started_at;
	int n = 0;
	fnl_mutex_lock(&a->lock);
	while (n < max && a->ring_n > 0) {
		const uint32_t oldest = (a->ring_head + FRAME_RING - a->ring_n) %
								FRAME_RING;
		FnlFrameSample row = a->ring[oldest];
		/* Waiting for its GPU time while it is young, and the frames after
		 * it with it: the order is the order drawn (GPU_HOLD_FRAMES). */
		if (row.gpu_ms == GPU_PENDING) {
			if (!a->ring_done && a->ring_n <= GPU_HOLD_FRAMES &&
				now - row.time < GPU_HOLD_S)
				break;
			row.gpu_ms = -1.0f;
		}
		memcpy((char *)out + (size_t)n * stride, &row, each);
		a->ring_n--;
		n++;
	}
	if (lost) {
		*lost = a->ring_lost;
		a->ring_lost = 0;
	}
	fnl_mutex_unlock(&a->lock);
	return n;
}

void app_frame_shown(App *a, uint64_t index, double mono, bool vblank)
{
	if (!fnl_atomic_load_bool(&a->stats_on))
		return;
	const FnlFrameShown s = { index, mono - a->started_at, vblank };
	fnl_mutex_lock(&a->lock);
	a->shown[a->shown_head] = s;
	a->shown_head = (a->shown_head + 1) % FRAME_RING;
	if (a->shown_n < FRAME_RING)
		a->shown_n++;
	else
		a->shown_lost++;
	fnl_mutex_unlock(&a->lock);
}

void app_present_numbered(App *a, uint64_t index, uint32_t number,
						  double at)
{
	const uint32_t cap = sizeof a->numbered / sizeof a->numbered[0];
	if (a->numbered_n) {
		const uint32_t last = a->numbered[(a->numbered_head + cap - 1) %
										  cap].number;
		/* Signed, so a count that wraps still runs forward. */
		if ((int32_t)(number - last) <= 0) {
			a->numbered_n = 0;
			a->number_stamped = 0;
		}
	}
	a->numbered[a->numbered_head].number = number;
	a->numbered[a->numbered_head].index = index;
	a->numbered[a->numbered_head].at = at;
	a->numbered_head = (a->numbered_head + 1) % cap;
	if (a->numbered_n < cap)
		a->numbered_n++;
}

void app_present_number_shown(App *a, uint32_t number, double mono,
							  bool vblank)
{
	if (number == a->number_stamped)
		return;
	const uint32_t cap = sizeof a->numbered / sizeof a->numbered[0];
	for (uint32_t i = 1; i <= a->numbered_n; i++) {
		const uint32_t k = (a->numbered_head + cap - i) % cap;
		if (a->numbered[k].number == number) {
			a->number_stamped = number;
			if (mono >= a->numbered[k].at)
				app_frame_shown(a, a->numbered[k].index, mono, vblank);
			return;
		}
	}
}

void app_frame_answered(App *a, uint64_t index)
{
	fnl_mutex_lock(&a->lock);
	fnl_atomic_store_u32(&a->frame_answered, (uint32_t)(index + 1));
	fnl_cond_signal(&a->cond);
	fnl_mutex_unlock(&a->lock);
}

int fnl_frame_shown(FnlApp *app, FnlFrameShown *out, int max, size_t stride,
					uint64_t *lost)
{
	App *a = (App *)app;
	if (lost)
		*lost = 0;
	if (!a || !out || max <= 0 || stride < sizeof out->index)
		return 0;
	const size_t each = stride < sizeof *out ? stride : sizeof *out;
	int n = 0;
	fnl_mutex_lock(&a->lock);
	while (n < max && a->shown_n > 0) {
		const uint32_t oldest = (a->shown_head + FRAME_RING - a->shown_n) %
								FRAME_RING;
		memcpy((char *)out + (size_t)n * stride, &a->shown[oldest], each);
		a->shown_n--;
		n++;
	}
	if (lost) {
		*lost = a->shown_lost;
		a->shown_lost = 0;
	}
	fnl_mutex_unlock(&a->lock);
	return n;
}

void fnl_set_transition_ms(FnlApp *app, int ms)
{
	App *a = (App *)app;
	if (!a)
		return;
	/* Held to VEIL_MS_MAX: a dissolve is a fraction of a second, and a
	 * figure past a few is a move that never comes. Below zero is none.
	 * One figure for both of the veil's lengths; the fade down is only
	 * ever the fallback's (App::veil_black). */
	if (ms < 0)
		ms = 0;
	if (ms > VEIL_MS_MAX)
		ms = VEIL_MS_MAX;
	fnl_atomic_store_u32(&a->veil_out_ms, (uint32_t)ms);
	fnl_atomic_store_u32(&a->veil_in_ms, (uint32_t)ms);
}

/* What the mode `aa` comes to on the device drawing now. Both modes need
 * the offscreen target; FSAA also needs a sample count worth having. Any
 * thread: both answers are atomics the device build writes. */
static FnlAntialias aa_resolve(const App *a, FnlAntialias aa)
{
	const FnlRendererVtbl *r = renderer_any(a);
	const bool off = r && r->offscreen_scene;
	if (off && aa == FNL_AA_MSAA && r->max_samples(a) > 1)
		return FNL_AA_MSAA;
	if (off && aa == FNL_AA_MLAA)
		return FNL_AA_MLAA;
	return FNL_AA_NONE;
}

/* The mode in force, set under the lock. Signalled only for a change: the
 * wake is for a render thread parked on a window that is off screen, and
 * an unchanged mode gives it nothing to do. */
static void aa_force(App *a, FnlAntialias want) FNL_REQUIRES(a->lock);
static void aa_force(App *a, FnlAntialias want)
{
	if (a->want_aa != want) {
		a->want_aa = want;
		fnl_cond_signal(&a->cond);
	}
}

/* The request is kept and the mode in force is what the device makes of it,
 * so a caller that reads the value back gets the truth, and the request
 * still holds on the next device (app_aa_settle()). */
void fnl_set_antialias(FnlApp *app, FnlAntialias aa)
{
	App *a = (App *)app;
	if (!a)
		return;
	if (aa != FNL_AA_MSAA && aa != FNL_AA_MLAA)
		aa = FNL_AA_NONE;
	const FnlAntialias want = aa_resolve(a, aa);
	fnl_mutex_lock(&a->lock);
	a->aa_asked = aa;
	aa_force(a, want);
	fnl_mutex_unlock(&a->lock);
}

/* Worked out off the lock, as fnl_set_antialias() does, and set only where
 * no newer request came in meanwhile: that one was resolved by its own
 * call against the same device. */
void app_aa_settle(App *a)
{
	fnl_mutex_lock(&a->lock);
	const FnlAntialias asked = a->aa_asked;
	fnl_mutex_unlock(&a->lock);
	const FnlAntialias want = aa_resolve(a, asked);
	fnl_mutex_lock(&a->lock);
	if (a->aa_asked == asked)
		aa_force(a, want);
	fnl_mutex_unlock(&a->lock);
}

void fnl_release_pointer(FnlApp *app)
{
	App *a = (App *)app;
	if (!a || !a->backend)
		return;
	fnl_mutex_lock(&a->lock);
	a->req.release_ptr = true;
	fnl_mutex_unlock(&a->lock);
	app_wakeup(a);
}

/* Forwarded like the rest of the block: which shape is on screen is decided
 * by the backend's pointer path, which is the pump's. */
void fnl_set_cursor(FnlApp *app, FnlCursor shape)
{
	App *a = (App *)app;
	if (!a || !a->backend)
		return;
	/* One unsigned compare, so a negative value from a caller's own
	 * arithmetic is out of range rather than an index. */
	if ((unsigned)shape > (unsigned)FNL_CURSOR_WAIT)
		return;
	/* Compared before it is recorded: a caller sets this every frame, and
	 * each request is a wakeup of a pump that would find nothing to do. */
	fnl_mutex_lock(&a->lock);
	const bool moved = a->cursor_asked != shape;
	if (moved) {
		a->cursor_asked   = shape;
		a->req.cursor     = true;
		a->req.cursor_val = shape;
	}
	fnl_mutex_unlock(&a->lock);
	if (moved)
		app_wakeup(a);
}

/* Stored and asked for under the lock; compared first, as fnl_set_cursor()
 * is, so a caller setting the same title every frame wakes nothing. Not
 * measured here: the width is the frame's, taken when it latches this
 * string (draw_state_update()), so the bar's layout and its text agree. */
void fnl_set_title(FnlApp *app, const char *utf8)
{
	App *a = (App *)app;
	if (!a || !a->backend)
		return;
	char t[TITLE_MAX];
	app_title_copy(t, utf8);
	fnl_mutex_lock(&a->lock);
	const bool moved = strcmp(a->title, t) != 0;
	if (moved) {
		memcpy(a->title, t, sizeof t);
		a->req.title = true;
	}
	fnl_mutex_unlock(&a->lock);
	if (moved)
		app_wakeup(a);
}

/* Text entry. Mostly a flag the pump reads per key from the atomic. The
 * request beside it is for a backend with an input method to enable, which
 * is a call on the pump's own objects. Compared first, as fnl_set_cursor()
 * is, so an unchanged mode does not wake the pump. */
void fnl_set_text_input(FnlApp *app, bool on)
{
	App *a = (App *)app;
	if (!a)
		return;
	fnl_mutex_lock(&a->lock);
	const bool moved = fnl_atomic_load_bool(&a->text_input) != on;
	if (moved) {
		fnl_atomic_store_bool(&a->text_input, on);
		if (a->backend)
			a->req.text_mode = true;
	}
	fnl_mutex_unlock(&a->lock);
	if (moved && a->backend)
		app_wakeup(a);
}

bool fnl_is_text_input(const FnlApp *app)
{
	const App *a = (const App *)app;
	return a && app_text_input(a);
}

void fnl_set_text_rect_pt(FnlApp *app, int x, int y, int w, int h)
{
	App *a = (App *)app;
	if (!a || !a->backend)
		return;
	const uint32_t v[4] = { (uint32_t)x, (uint32_t)y, (uint32_t)w,
							(uint32_t)h };
	/* A caret is drawn every frame and this is the natural place to say
	 * where, so the unchanged case has to cost nothing -- no request, no
	 * wakeup of a pump that would find the same rectangle. */
	bool moved = !fnl_atomic_load_bool(&a->text_rect_set);
	for (int i = 0; i < 4; i++)
		if (fnl_atomic_load_u32(&a->text_rect[i]) != v[i])
			moved = true;
	if (!moved)
		return;
	for (int i = 0; i < 4; i++)
		fnl_atomic_store_u32(&a->text_rect[i], v[i]);
	fnl_atomic_store_bool(&a->text_rect_set, true);
	fnl_mutex_lock(&a->lock);
	a->req.text_mode = true;
	fnl_mutex_unlock(&a->lock);
	app_wakeup(a);
}

/* The clipboard's two requests. The string is copied here, on the caller's
 * thread, and handed to the pump by pointer: what is offered to the desktop
 * has to outlive this call by however long another program takes to ask. */
bool fnl_clipboard_set(FnlApp *app, const char *utf8)
{
	App *a = (App *)app;
	if (!a || !a->backend || !utf8)
		return false;
	const size_t n = strlen(utf8);
	char *copy = malloc(n + 1);
	if (!copy)
		return false;
	memcpy(copy, utf8, n + 1);

	fnl_mutex_lock(&a->lock);
	/* Two before the pump came round: the newer one is the clipboard. */
	free(a->clip_pending);
	a->clip_pending = copy;
	fnl_mutex_unlock(&a->lock);
	app_wakeup(a);
	return true;
}

void fnl_clipboard_request(FnlApp *app)
{
	App *a = (App *)app;
	if (!a || !a->backend)
		return;
	fnl_mutex_lock(&a->lock);
	a->req.clip_get = true;
	fnl_mutex_unlock(&a->lock);
	app_wakeup(a);
}

void fnl_set_scene_size_px(FnlApp *app, int w_px, int h_px)
{
	App *a = (App *)app;
	if (!a)
		return;
	if (w_px < 0) w_px = 0;
	if (h_px < 0) h_px = 0;

	/* Under the lock and as a pair, for the reason at the top of this file:
	 * the two halves of a size have to cross to the render thread together.
	 * Signalled only for a change, as fnl_set_antialias() is. */
	fnl_mutex_lock(&a->lock);
	if (a->want_scene_w != w_px || a->want_scene_h != h_px) {
		a->want_scene_w = w_px;
		a->want_scene_h = h_px;
		fnl_cond_signal(&a->cond);
	}
	fnl_mutex_unlock(&a->lock);
}

/* ---- diagnostics --------------------------------------------------------- */

/* The output the surface is on: want_output, which is latched with the size
 * it belongs to.
 *
 * Copied under the lock into a per-thread slot, the shape fnl_last_error()
 * has. The backends rewrite want_output under App::lock and this call is
 * [any], so a pointer into that buffer could be read *unterminated*
 * mid-snprintf, not merely stale. */
const char *fnl_output_name(const FnlApp *app)
{
	static FNL_THREAD_LOCAL char snap[OUTPUT_NAME_MAX];
	App *a = (App *)app;
	if (!a)
		return "";
	fnl_mutex_lock(&a->lock);
	memcpy(snap, a->want_output, sizeof snap);
	fnl_mutex_unlock(&a->lock);
	snap[sizeof snap - 1] = '\0';
	return snap;
}

const char *fnl_present_mode(const FnlApp *app)
{
	const App *a = (const App *)app;
	const FnlRendererVtbl *r = a ? renderer_any(a) : NULL;
	return r ? r->present_mode_name(a) : "";
}

/* The two seams, named: which backend and which renderer drive the device
 * that fnl_gpu_name() names. Both are string literals off the dispatch
 * tables and neither changes over a window's life, so a caller may keep the
 * pointer. */
const char *fnl_backend_name(const FnlApp *app)
{
	const App *a = (const App *)app;
	return a && a->backend ? BACKEND(a)->name : "";
}

const char *fnl_window_host(const FnlApp *app)
{
	const App *a = (const App *)app;
	return a && a->backend ? BACKEND(a)->host(a) : "";
}

const char *fnl_renderer_name(const FnlApp *app)
{
	const App *a = (const App *)app;
	const FnlRendererVtbl *r = a ? renderer_any(a) : NULL;
	return r ? r->name : "";
}

/* Under the lock and as a pair: a width from one monitor beside a height from
 * another is a box no screen has. */
void fnl_output_size_pt(const FnlApp *app, int *w, int *h)
{
	App *a = (App *)app;
	int ow = 0, oh = 0;
	if (a) {
		fnl_mutex_lock(&a->lock);
		ow = a->want_out_w_pt;
		oh = a->want_out_h_pt;
		fnl_mutex_unlock(&a->lock);
	}
	if (w) *w = ow;
	if (h) *h = oh;
}

/* The published scale, not scale120() directly: that reads App::draw_scale120,
 * which the render loop writes, and this call is [any]. See App::pub_*. */
int fnl_scale120(const FnlApp *app)
{
	App *a = (App *)app;
	if (!a)
		return 120;
	fnl_mutex_lock(&a->lock);
	const int s = a->pub_scale120 > 0 ? a->pub_scale120 : 120;
	fnl_mutex_unlock(&a->lock);
	return s;
}

/* ---- display modes and outputs ---------------------------------------------
 *
 * Read through the seam each time rather than kept here: the lists are the
 * backend's, fixed at open but for which entry is current, and the getters
 * are [any] -- FnlBackendVtbl::modes says what makes that safe. */

int fnl_mode_count(const FnlApp *app)
{
	const App *a = (const App *)app;
	if (!a || !a->backend)
		return 0;
	FnlMode modes[OUTPUT_MODES_MAX];
	int cur;
	return BACKEND(a)->modes(a, modes, OUTPUT_MODES_MAX, &cur);
}

bool fnl_mode_get(const FnlApp *app, int i, FnlMode *mode)
{
	const App *a = (const App *)app;
	if (!a || !a->backend || !mode || i < 0)
		return false;
	FnlMode modes[OUTPUT_MODES_MAX];
	int cur;
	const int n = BACKEND(a)->modes(a, modes, OUTPUT_MODES_MAX, &cur);
	if (i >= n)
		return false;
	*mode = modes[i];
	return true;
}

bool fnl_mode_current(const FnlApp *app, FnlMode *mode)
{
	const App *a = (const App *)app;
	if (!a || !a->backend || !mode)
		return false;
	FnlMode modes[OUTPUT_MODES_MAX];
	int cur;
	const int n = BACKEND(a)->modes(a, modes, OUTPUT_MODES_MAX, &cur);
	if (cur < 0 || cur >= n)
		return false;
	*mode = modes[cur];
	return true;
}

void fnl_request_mode(FnlApp *app, const FnlMode *mode)
{
	App *a = (App *)app;
	if (!a || !a->backend || !mode || mode->w_px <= 0 || mode->h_px <= 0)
		return;
	fnl_mutex_lock(&a->lock);
	a->req.mode     = true;
	a->req.mode_val = *mode;
	fnl_mutex_unlock(&a->lock);
	app_wakeup(a);
}

int fnl_output_count(const FnlApp *app)
{
	const App *a = (const App *)app;
	if (!a || !a->backend)
		return 0;
	char names[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	int cur;
	return BACKEND(a)->outputs(a, names, OUTPUTS_MAX, &cur);
}

const char *fnl_output_name_at(const FnlApp *app, int i)
{
	static FNL_THREAD_LOCAL char snap[OUTPUT_NAME_MAX];
	const App *a = (const App *)app;
	snap[0] = '\0';
	if (!a || !a->backend || i < 0)
		return snap;
	char names[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	int cur;
	if (i < BACKEND(a)->outputs(a, names, OUTPUTS_MAX, &cur))
		snprintf(snap, sizeof snap, "%s", names[i]);
	return snap;
}

void fnl_request_output(FnlApp *app, const char *name)
{
	App *a = (App *)app;
	if (!a || !a->backend || !name || !name[0])
		return;
	fnl_mutex_lock(&a->lock);
	a->req.output = true;
	snprintf(a->req.output_name, sizeof a->req.output_name, "%s", name);
	fnl_mutex_unlock(&a->lock);
	app_wakeup(a);
}

/* ---- the GPUs ---------------------------------------------------------------
 *
 * The list is the renderers' (app_gpu_list_publish(), app_gpu_list_others()),
 * read under App::lock like gpu_name; the request is the render loop's to
 * spend, since the move is a rebuild of the device, or of the renderer
 * (gpu_switch_owed() in src/core/frame.c). */

int fnl_gpu_count(const FnlApp *app)
{
	App *a = (App *)app;               /* the lock is not const */
	if (!a)
		return 0;
	fnl_mutex_lock(&a->lock);
	const int n = a->gpu_list_n;
	fnl_mutex_unlock(&a->lock);
	return n;
}

int fnl_gpu_current(const FnlApp *app)
{
	App *a = (App *)app;
	if (!a)
		return -1;
	fnl_mutex_lock(&a->lock);
	const int cur = a->gpu_list_cur;
	fnl_mutex_unlock(&a->lock);
	return cur;
}

const char *fnl_gpu_name_at(const FnlApp *app, int i)
{
	static FNL_THREAD_LOCAL char snap[GPU_NAME_MAX];
	App *a = (App *)app;
	snap[0] = '\0';
	if (!a || i < 0)
		return snap;
	fnl_mutex_lock(&a->lock);
	if (i < a->gpu_list_n)
		memcpy(snap, a->gpu_list[i].name, sizeof snap);
	fnl_mutex_unlock(&a->lock);
	return snap;
}

const char *fnl_gpu_via_at(const FnlApp *app, int i)
{
	static FNL_THREAD_LOCAL char snap[GPU_VIA_MAX];
	App *a = (App *)app;
	snap[0] = '\0';
	if (!a || i < 0)
		return snap;
	fnl_mutex_lock(&a->lock);
	if (i < a->gpu_list_n)
		memcpy(snap, a->gpu_list[i].via, sizeof snap);
	fnl_mutex_unlock(&a->lock);
	return snap;
}

const char *fnl_renderer_via(const FnlApp *app)
{
	static FNL_THREAD_LOCAL char snap[GPU_VIA_MAX];
	App *a = (App *)app;
	snap[0] = '\0';
	if (!a)
		return snap;
	fnl_mutex_lock(&a->lock);
	memcpy(snap, a->gpu_via, sizeof snap);
	fnl_mutex_unlock(&a->lock);
	return snap;
}

FnlGpuKind fnl_gpu_kind_at(const FnlApp *app, int i)
{
	App *a = (App *)app;
	if (!a || i < 0)
		return FNL_GPU_KIND_UNKNOWN;
	fnl_mutex_lock(&a->lock);
	const FnlGpuKind k = i < a->gpu_list_n ? a->gpu_list[i].kind
										   : FNL_GPU_KIND_UNKNOWN;
	fnl_mutex_unlock(&a->lock);
	return k;
}

FnlRenderer fnl_gpu_renderer_at(const FnlApp *app, int i)
{
	App *a = (App *)app;
	if (!a || i < 0)
		return FNL_RENDERER_DEFAULT;
	fnl_mutex_lock(&a->lock);
	const FnlRenderer r = i < a->gpu_list_n ? a->gpu_list[i].renderer
											: FNL_RENDERER_DEFAULT;
	fnl_mutex_unlock(&a->lock);
	return r;
}

/* Recorded and the render thread woken, as fnl_invalidate() wakes it: the
 * loop spends the wish at the top of its next turn. */
void fnl_request_gpu(FnlApp *app, int i)
{
	App *a = (App *)app;
	if (!a || i < 0)
		return;
	fnl_mutex_lock(&a->lock);
	if (i < a->gpu_list_n && i != a->gpu_list_cur) {
		a->gpu_req = i + 1;
		a->gpu_req_entry = a->gpu_list[i];
		a->want_user_gen++;
		fnl_cond_signal(&a->cond);
	}
	fnl_mutex_unlock(&a->lock);
}

void fnl_request_size_pt(FnlApp *app, int w_pt, int h_pt)
{
	App *a = (App *)app;
	if (!a || !a->backend)
		return;
	/* Bounded before it is recorded (see clamp_pt in app.h). The backends
	 * already refuse the other end -- a request of zero or less is not a
	 * window -- so only the ceiling is applied here. */
	fnl_mutex_lock(&a->lock);
	a->req.size   = true;
	a->req.size_w = clamp_pt(w_pt);
	a->req.size_h = clamp_pt(h_pt);
	fnl_mutex_unlock(&a->lock);
	app_wakeup(a);
}

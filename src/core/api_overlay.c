/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The public overlay API: fenestal.h's drawing calls, over the internal
 * appenders in src/core/overlay.c. Two translations and nothing else:
 *
 *   colour     Public colours are authored sRGB. The appenders want them in
 *              whatever space the surface blends in, which depends on the
 *              format the driver handed back.
 *
 *   null       Every entry point tolerates no window, no device, or a call
 *              from outside a recording. The appenders no-op on a null map;
 *              this adds the null-app check.
 */

#include "core/app.h"

/* sRGB -> the surface's blend space, per call because a caller's colours are
 * not a table that can be converted once. Alpha passes through untouched.
 *
 * Memoised four deep. Drawing arrives in runs of one colour and as often in a
 * pair that alternates -- a label and its value, down every row -- which one
 * entry misses on every call, at three powf() a miss. The oldest is the one
 * replaced.
 *
 * Keyed on the blend space rather than on the App: a device-loss recovery
 * re-resolves the surface format on the same App.
 *
 * Thread-local, so it needs no lock. The zeroed initial state is four
 * *correct* entries: a linear surface converts transparent black to
 * itself. */
#define COLOR_MEMO 4
static void to_surface(const App *a, FnlColor c, float out[4])
{
	static FNL_THREAD_LOCAL struct {
		bool     srgb;
		FnlColor in;
		float    out[4];
	} memo[COLOR_MEMO];
	static FNL_THREAD_LOCAL unsigned memo_next;

	for (int i = 0; i < COLOR_MEMO; i++)
		if (a->srgb_surface == memo[i].srgb &&
			c.r == memo[i].in.r && c.g == memo[i].in.g &&
			c.b == memo[i].in.b && c.a == memo[i].in.a) {
			memcpy(out, memo[i].out, sizeof memo[i].out);
			return;
		}

	const float rgb[3] = { c.r, c.g, c.b };
	if (a->srgb_surface) {
		for (int i = 0; i < 3; i++)
			out[i] = srgb_to_linear(rgb[i]);
	} else {
		for (int i = 0; i < 3; i++)
			out[i] = rgb[i];
	}
	out[3] = c.a;

	const unsigned slot = memo_next++ % COLOR_MEMO;
	memo[slot].srgb = a->srgb_surface;
	memo[slot].in   = c;
	memcpy(memo[slot].out, out, sizeof memo[slot].out);
}

/* Bounds-check the size as well as the pointer: a caller reading a size out of
 * a config file and passing it straight in should get nothing drawn, not an
 * index off the end of the baked metric tables. */
static bool size_ok(FnlTextSize sz)
{
	return (int)sz >= 0 && (int)sz < (int)TEXT_SIZE_COUNT;
}

/* Whether this call is inside on_overlay: the render thread, with a
 * recording open. The thread first: App::overlay_open is that thread's
 * plain bool, and a call from another while it is set would append into
 * the arenas the recording is writing, or flush into the renderer's state,
 * which a move to another renderer frees -- where fenestal.h promises that
 * outside the callback nothing is drawn. */
static bool overlay_here(const App *a)
{
	return a && app_thread() == APP_THREAD_RENDER && a->overlay_open;
}

/* ---- metrics -------------------------------------------------------------
 *
 * No App argument: these read the baked metric tables, so a label can be
 * measured before there is a window, and from any thread. */

int fnl_text_width(const char *utf8, FnlTextSize sz)
{
	if (!utf8 || !size_ok(sz))
		return 0;
	return text_width(utf8, sz);
}

int fnl_text_line(FnlTextSize sz)
{
	return size_ok(sz) ? FONT_LINE_PT[sz] : 0;
}

int fnl_text_height(FnlTextSize sz)
{
	return size_ok(sz) ? text_height(sz) : 0;
}

/* Zero for the proportional face rather than an average, which would be wrong
 * in both directions; fnl_text_width() answers for every face. */
int fnl_text_advance(FnlTextSize sz)
{
	if (!size_ok(sz) || !text_is_mono(sz))
		return 0;
	return text_advance(sz);
}

void fnl_window_size_pt(const FnlApp *app, int *w, int *h)
{
	App *a = (App *)app;
	int ww = 0, hh = 0;
	if (a) {
		/* The published pair rather than logical_w()/logical_h() directly:
		 * those read the extent and the scale the render thread writes, and
		 * this call is [any]. See App::pub_* in app.h. */
		fnl_mutex_lock(&a->lock);
		ww = a->pub_win_w;
		hh = a->pub_win_h;
		fnl_mutex_unlock(&a->lock);
	}
	if (w) *w = ww;
	if (h) *h = hh;
}

/* ---- drawing ------------------------------------------------------------- */

void fnl_overlay_text(FnlApp *app, int x, int y, FnlTextSize sz, FnlColor c,
					  const char *utf8)
{
	App *a = (App *)app;
	if (!overlay_here(a) || !utf8 || !size_ok(sz))
		return;
	float col[4];
	to_surface(a, c, col);
	text_draw(a, clamp_pt(x), clamp_pt(y), sz, col, utf8);
}

void fnl_overlay_rect(FnlApp *app, int x, int y, int w, int h, FnlColor c)
{
	App *a = (App *)app;
	if (!overlay_here(a))
		return;
	float col[4];
	to_surface(a, c, col);
	overlay_rect(a, clamp_pt(x), clamp_pt(y), clamp_pt(w), clamp_pt(h), col);
}

void fnl_overlay_stroke(FnlApp *app, float x0, float y0, float x1, float y1,
						float width, FnlColor c)
{
	App *a = (App *)app;
	if (!overlay_here(a))
		return;
	float col[4];
	to_surface(a, c, col);
	overlay_stroke(a, x0, y0, x1, y1, width, col);
}

void fnl_overlay_elbow(FnlApp *app, float x0, float y0, float x1, float y1,
					   float x2, float y2, float width, FnlColor c)
{
	App *a = (App *)app;
	if (!overlay_here(a))
		return;

	/* p2 == p0 is how overlay_box() spells a rectangle to the shader (see
	 * shape.frag), and a polyline computed from data reaches it the moment
	 * two of its points coincide. Collapsed to the plain stroke it
	 * geometrically is. Compared with == because that is the test the shader
	 * makes, and only here: overlay_box() itself goes through
	 * overlay_elbow(). */
	if (x2 == x0 && y2 == y0) {
		x2 = x1;
		y2 = y1;
	}

	float col[4];
	to_surface(a, c, col);
	overlay_elbow(a, x0, y0, x1, y1, x2, y2, width, col);
}

void fnl_overlay_box(FnlApp *app, float x0, float y0, float x1, float y1,
					 float width, FnlColor c)
{
	App *a = (App *)app;
	if (!overlay_here(a))
		return;
	float col[4];
	to_surface(a, c, col);
	overlay_box(a, x0, y0, x1, y1, width, col);
}

/* Both streams, in the order the invariant requires. Not text: it is one draw
 * for the frame and goes last, so flushing it here would put every glyph
 * appended afterwards under the rectangles that follow.
 *
 * Refused outside on_overlay, as the appenders refuse: a flush is a call
 * into the renderer drawing, whose state is the render thread's and is
 * freed under any other by a move to another renderer. */
void fnl_overlay_flush(FnlApp *app)
{
	App *a = (App *)app;
	if (!overlay_here(a))
		return;
	overlay_flush_rects(a);
	overlay_flush_shapes(a);
}

void fnl_overlay_clip(FnlApp *app, int x, int y, int w, int h)
{
	App *a = (App *)app;
	/* fnl_overlay_flush()'s refusal, and for its reason. */
	if (!overlay_here(a))
		return;

	/* Everything appended so far is issued first: a scissor is command state
	 * and applies to the *draw*, not to the append. Not text, which is one
	 * draw for the frame, issued after draw_overlay() has taken a clip left
	 * in force back -- so no clip reaches a glyph, as fenestal.h says. */
	fnl_overlay_flush(app);

	/* A degenerate rectangle means "all of it", not "none of it": an empty
	 * clip computed from a collapsed panel must not switch drawing off for
	 * the rest of the frame. */
	FnlRect r;
	if (w <= 0 || h <= 0 ||
		!overlay_clip(a, clamp_pt(x), clamp_pt(y), clamp_pt(w), clamp_pt(h),
					  &r)) {
		overlay_scissor(a, NULL);
		a->overlay_clipped = false;
		return;
	}
	overlay_scissor(a, &r);
	/* Recorded so the frame can put it back when this callback returns; see
	 * App::overlay_clipped and draw_overlay(). */
	a->overlay_clipped = true;
}

/* ---- controls in the titlebar -------------------------------------------- */

bool fnl_titlebar_reserve(FnlApp *app, int want_w, int *x, int *y,
						  int *w, int *h)
{
	App *a = (App *)app;
	if (!a)
		return false;
	/* Bounded like every point figure a caller passes (see clamp_pt in
	 * app.h). Negatives collapse to zero, which is the documented way to give
	 * the strip back. */
	want_w = want_w > 0 ? clamp_pt(want_w) : 0;

	/* Recorded whatever the answer, so that giving the strip back with a zero
	 * width works even in the frames where it would not have fitted anyway.
	 * Only when it moved: this is called every frame, and the pump's hit test
	 * reads the same word. */
	if (fnl_atomic_load_u32(&a->titlebar_reserved) != (uint32_t)want_w)
		fnl_atomic_store_u32(&a->titlebar_reserved, (uint32_t)want_w);

	if (want_w == 0)
		return false;

	/* All three from under one lock: the published pair because the extent
	 * and the scale are the render thread's (App::pub_*), and deco_custom
	 * because deco_apply_custom() writes it under this lock. The answer
	 * describes the last frame built. */
	fnl_mutex_lock(&a->lock);
	const int  win_w  = a->pub_win_w;
	const bool csd    = a->pub_csd;
	const bool custom = a->deco_custom;
	fnl_mutex_unlock(&a->lock);

	/* No titlebar of ours, nothing to reserve in. Fullscreen is folded into
	 * pub_csd; a bar the caller replaced has no button strip to lay a run out
	 * from, and fnl_titlebar_claim() is what that caller wants instead. */
	if (!csd || custom)
		return false;

	int rx, ry, rw, rh;
	bool vis;
	deco_reserved_rect(a, win_w, want_w, &rx, &ry, &rw, &rh, &vis);
	if (!vis)
		return false;

	if (x) *x = rx;
	if (y) *y = ry;
	if (w) *w = rw;
	if (h) *h = rh;
	return true;
}

/* ---- a titlebar of the caller's own -------------------------------------- */

bool fnl_titlebar_custom(FnlApp *app, int height_pt, int min_width_pt)
{
	App *a = (App *)app;
	if (!a || !a->backend)
		return false;
	/* Both bounded (see clamp_pt in app.h). Zero means "the library's own
	 * bar" for the first and "no width floor" for the second, so a negative
	 * collapses to it rather than being clamped symmetrically. */
	height_pt    = height_pt    > 0 ? clamp_pt(height_pt)    : 0;
	min_width_pt = min_width_pt > 0 ? clamp_pt(min_width_pt) : 0;

	/* Recorded, not applied. What follows from it is a size request and a
	 * floor restated to the window server, both of which belong to the pump's
	 * thread -- see App::req. */
	fnl_mutex_lock(&a->lock);
	a->req.bar       = true;
	a->req.bar_h     = height_pt;
	a->req.bar_min_w = min_width_pt;
	fnl_mutex_unlock(&a->lock);
	app_wakeup(a);
	return true;
}

/* The other half, on the pump's thread. */
void deco_apply_custom(App *a, int height_pt, int min_width_pt)
{
	const bool custom = height_pt > 0;
	const int  new_h  = custom ? height_pt : DECO_TITLEBAR_H;
	const int  old_h  = a->deco_h;
	if (custom == a->deco_custom && new_h == old_h &&
		min_width_pt == a->deco_min_w)
		return;

	/* Under the lock, because the render thread latches these with the
	 * frame's size (latch_take in frame.c): plain stores could give a frame
	 * laid out at the old height and painted by the new owner. Released
	 * before the backend calls below, which must not run holding it. */
	fnl_mutex_lock(&a->lock);
	a->deco_custom = custom;
	a->deco_h      = new_h;
	a->deco_min_w  = custom ? min_width_pt : 0;
	fnl_mutex_unlock(&a->lock);

	/* The floor moved, so the window server has to be told again: each
	 * backend holds the old one in its own way, whether as a hint the server
	 * enforces during a drag or as a floor it re-arms after one. */
	BACKEND(a)->apply_size_bounds(a);

	/* And the window keeps the presentable area it had, rather than the height
	 * it had: a taller bar comes off the chrome, not off the picture. Skipped
	 * in fullscreen, where there is no bar and nothing to keep whole. */
	if (!a->is_fullscreen && a->cur_h > old_h && old_h != new_h)
		app_request_size(a, a->cur_w, a->cur_h - old_h);
}

/* The one list both claims append to. The caller has checked that this is
 * on_overlay, where the list is the render thread's alone. */
static bool claim_add(App *a, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return false;

	if (a->claim_count >= CLAIMS_MAX) {
		/* Once, not once a frame: this fires from inside the frame path and a
		 * line per frame per claim is a log nobody can read and a stall
		 * nobody asked for. */
		if (!a->claim_overflow_warned) {
			a->claim_overflow_warned = true;
			fnl_log(FNL_LOG_WARN,
					"more than %d claims in one frame; the rest are not "
					"the caller's. Group controls into fewer rectangles",
					CLAIMS_MAX);
		}
		return false;
	}

	/* Bounded like everything else a caller passes in points: claim_at()
	 * tests `x < r->x + r->w`, and a corner that overflows an int is a claim
	 * that swallows presses nowhere near it. */
	a->claims[a->claim_count++] = (FnlRect){ clamp_pt(x), clamp_pt(y),
											 clamp_pt(w), clamp_pt(h) };
	return true;
}

bool fnl_titlebar_claim(FnlApp *app, int x, int y, int w, int h)
{
	App *a = (App *)app;
	/* Three refusals: not inside on_overlay, no bar of the caller's, or more
	 * rectangles in one frame than the array holds.
	 *
	 * The frame's latched answer and not App::deco_custom, which the pump's
	 * thread writes: inside a recording it needs neither lock nor atomic. */
	if (!overlay_here(a) || !a->last_draw.custom_bar)
		return false;
	return claim_add(a, x, y, w, h);
}

bool fnl_claim(FnlApp *app, int x, int y, int w, int h)
{
	App *a = (App *)app;
	/* Any bar, or none: over the library's own bar the claim is inert,
	 * because deco_zone_at() answers for the band before claims are read,
	 * and that is what keeps the library's buttons and drag whole. */
	if (!overlay_here(a))
		return false;
	return claim_add(a, x, y, w, h);
}

/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Everything this library draws for itself: the client titlebar with its
 * four buttons and its controls, the open drop-down's list, the metrics and
 * ranking panels (drawn by metrics.c and rank.c, ordered here) and the veil
 * over a transition. Everything else a program puts on screen is the
 * program's, drawn through the public overlay.
 *
 * The bar stays because on Wayland a client that does not draw one has no
 * way to be moved or closed. Written in the overlay's three primitives and in
 * logical points; what only the renderer can answer is asked through
 * renderer.h.
 */

#include "core/app.h"

/* Logical (unscaled) size of the surface: the coordinate space the decorations
 * and overlay are laid out in.
 *
 * From the extent actually allocated, which create_swapchain may have
 * clamped -- but rounded to nearest, not truncated: scale_pt() rounds on the
 * way out, so truncating back loses a point whenever that went up, and the
 * right and bottom edges fall out of step with the backend's hit test. */
int logical_w(const App *a)
{
	int s120 = scale120(a);
	return ((int)a->extent.w * 120 + s120 / 2) / s120 - 2 * a->draw_margin;
}

int logical_h(const App *a)
{
	int s120 = scale120(a);
	return ((int)a->extent.h * 120 + s120 / 2) / s120 - 2 * a->draw_margin;
}

/* The presentable area in pixels: what a scene renders into and what
 * fnl_content_size_px() reports. Measured between its converted edges, not as
 * a converted width, which would round differently.
 *
 * Clamped to the buffer: where create_swapchain() clamped the extent, the
 * point count read back can convert to a pixel one past the edge. */
void content_px(const App *a, const FnlDrawState *ds, int *x, int *y,
				int *w, int *h)
{
	int s120 = scale120(a);
	int cx, cy, cw, ch;
	content_rect(logical_w(a), logical_h(a), ds->bar_h, &cx, &cy, &cw, &ch);

	const int fw = (int)a->extent.w, fh = (int)a->extent.h;
	int x0 = surf_pt(a, cx, s120),      y0 = surf_pt(a, cy, s120);
	int x1 = surf_pt(a, cx + cw, s120), y1 = surf_pt(a, cy + ch, s120);
	if (x1 > fw) x1 = fw;
	if (y1 > fh) y1 = fh;
	if (x0 > x1) x0 = x1;
	if (y0 > y1) y0 = y1;

	*x = x0;
	*y = y0;
	*w = x1 - x0;
	*h = y1 - y0;
}

/* The window's buffer at the latched scale: the area and the grab ring, in
 * pixels. Brought under `max` by one factor, never each axis to its own
 * limit -- the viewport stretches the buffer across the window's aspect, so
 * clamping the axes apart distorts the picture -- and then over `min`, and
 * cut to whole multiples of what the backend asks
 * (FnlBackendVtbl::buffer_granularity).
 *
 * `max` is a backstop: the pointer thread already cut the scale to fit the
 * ceiling the renderer published (App::max_extent_w). Each renderer passes
 * its own limits, and asks this rather than computing a size of its own, so
 * the two cannot draw one window at two sizes. */
Extent app_buffer_extent(App *a, Extent min, Extent max)
{
	const int s120 = scale120(a);
	long long w = scale_pt(a->draw_w + 2 * a->draw_margin, s120);
	long long h = scale_pt(a->draw_h + 2 * a->draw_margin, s120);
	if (max.w > 0 && w > max.w) {
		h = h * max.w / w;
		w = max.w;
	}
	if (max.h > 0 && h > max.h) {
		w = w * max.h / h;
		h = max.h;
	}
	if (w < min.w) w = min.w;
	if (h < min.h) h = min.h;
	if (w < 1) w = 1;
	if (h < 1) h = 1;

	const long long gran = BACKEND(a)->buffer_granularity(a, s120);
	if (gran > 1) {
		w -= w % gran;
		h -= h % gran;
		if (w < gran) w = gran;
		if (h < gran) h = gran;
	}
	return (Extent){ (int)w, (int)h };
}

/* How much larger than the window the images it shows part of are made
 * (App::buffer), a step at a time. */
#define BUFFER_ROOM_STEP 256

/* How long a size stands before the window's images are made its own size
 * (app_buffer_at_rest()). Longer than the gaps between a drag's sizes,
 * which come a frame or two apart, so a drag is not answered with exact
 * images at each pause; short enough that a window at rest is soon on a
 * plane. */
#define BUFFER_SETTLE_S 0.5

/* The output at the latched scale, in pixels; zero where none is known. */
static Extent output_px(const App *a)
{
	const int s120 = scale120(a);
	return (Extent){ (int)scale_pt(a->last_draw.out_w_pt, s120),
					 (int)scale_pt(a->last_draw.out_h_pt, s120) };
}

bool app_buffer_at_rest(const App *a)
{
	return now_seconds() - a->size_at >= BUFFER_SETTLE_S;
}

static int room_side(int want, int max, int out, bool rest)
{
	if (rest)
		return want;
	int side = (want + BUFFER_ROOM_STEP - 1) / BUFFER_ROOM_STEP *
			   BUFFER_ROOM_STEP;
	/* The output is the room a window has there -- on Win32 the work
	 * area, which a fullscreen window is larger than -- so a window at
	 * least that large is not rounded past itself. */
	if (out > 0 && side > out)
		side = want > out ? want : out;
	if (max > 0 && side > max)
		side = max >= want ? max : want;
	return side;
}

/* The images to make for a window of `e` that shows their top-left part:
 * while its size is changing, `e` rounded up a BUFFER_ROOM_STEP, so the
 * sizes of a drag fit inside and are drawn into them with no new ones --
 * which is what a size costs where a driver allocates and clears images
 * slowly (DESIGN *A swapchain larger than the window*) -- and no larger
 * than the output. At rest, `e` itself: only images the window's size are
 * flipped onto a plane, and Windows composes the rest, a refresh more on
 * every frame (DESIGN *Benchmarking*). Under `max`, where that is not
 * zero. */
Extent app_buffer_room(const App *a, Extent e, Extent max)
{
	const Extent out = output_px(a);
	const bool rest = app_buffer_at_rest(a);
	return (Extent){ room_side(e.w, max.w, out.w, rest),
					 room_side(e.h, max.h, out.h, rest) };
}

/* Whether images of `room` still serve a window of `e`: at rest, only
 * images its own size; otherwise it is inside them, at least half of them
 * on each axis -- or a drag's memory is kept for good -- and it is not the
 * output's size, which gets images its own. */
bool app_buffer_room_fits(const App *a, Extent e, Extent room)
{
	if (app_buffer_at_rest(a))
		return e.w == room.w && e.h == room.h;
	const Extent out = output_px(a);
	if (e.w > room.w || e.h > room.h || 2 * e.w < room.w ||
		2 * e.h < room.h)
		return false;
	return out.w <= 0 || e.w < out.w || e.h < out.h ||
		   (e.w == room.w && e.h == room.h);
}

/* Where a scene of scene_w x scene_h lands inside an area_w x area_h
 * presentable area: scaled to fit, centred, aspect kept, in pixels of the
 * area. The letterbox bars are what is left over. */
void scene_dst_rect(int scene_w, int scene_h, int area_w, int area_h,
					int *x, int *y, int *w, int *h)
{
	/* The smaller of the two ratios, in integers: w = scene_w * area_h /
	 * scene_h when height binds, and the other way round when width does. */
	long long by_w = (long long)scene_h * area_w;   /* height if width binds  */
	long long by_h = (long long)scene_w * area_h;   /* width  if height binds */
	if (by_w <= (long long)scene_w * area_h) {      /* width binds            */
		*w = area_w;
		*h = (int)(by_w / scene_w);
	} else {
		*w = (int)(by_h / scene_h);
		*h = area_h;
	}
	if (*w < 1) *w = 1;
	if (*h < 1) *h = 1;
	*x = (area_w - *w) / 2;
	*y = (area_h - *h) / 2;
}

/* Publish the window's measurements for the [any]-thread callers; see
 * App::pub_* for why the public calls may not compute them themselves.
 *
 * The draw state's is_fullscreen, not App::is_fullscreen: the two disagree for
 * the frame a fullscreen toggle is answered in, and these figures describe
 * the frame the caller is drawing into. */
void app_publish_window(App *a, const FnlDrawState *ds)
{
	const int w = logical_w(a), h = logical_h(a);
	const bool csd = deco_client(ds->is_fullscreen);

	int cx, cy, cw, ch;
	content_rect(w, h, ds->bar_h, &cx, &cy, &cw, &ch);

	int px, py, pw, ph;
	content_px(a, ds, &px, &py, &pw, &ph);

	fnl_mutex_lock(&a->lock);
	a->pub_win_w = w;
	a->pub_win_h = h;
	a->pub_cx = cx;
	a->pub_cy = cy;
	a->pub_cw = cw;
	a->pub_ch = ch;
	a->pub_area_w_px = pw;
	a->pub_area_h_px = ph;
	a->pub_scene_w = (int)a->scene_extent.w;
	a->pub_scene_h = (int)a->scene_extent.h;
	a->pub_scale120 = scale120(a);
	a->pub_out_w_pt = ds->out_w_pt;
	a->pub_out_h_pt = ds->out_h_pt;
	a->pub_csd = csd;
	a->pub_bar_h = ds->bar_h;
	fnl_mutex_unlock(&a->lock);

	/* And whatever moved since the last frame, as events. After the publish
	 * and outside its lock: an application draining the queue will read these
	 * figures back through the public calls, and it must not find them
	 * describing an older frame than the event that sent it looking. */
	events_post_state(a, ds, cw, ch, pw, ph);
}

/* ---- client-side decorations ------------------------------------------- */

/* `w` and `h` are the window in logical points, `area_w` and `area_h` the
 * presentable area in pixels. */
static void draw_decorations(App *a, const FnlDrawState *ds, int w, int h,
							 int area_w, int area_h)
{
	/* Nothing to draw in fullscreen, which is the only thing that takes the
	 * frame away: no backend shows one there and the presentable area is the
	 * whole window. */
	if (!deco_client(ds->is_fullscreen))
		return;

	/* Two palettes: with client-side decorations the titlebar is the only
	 * thing that can show which window has the keyboard, and the compositor
	 * says which that is through xdg_toplevel's activated state. */
	const float *bar   = a->pal[ds->is_active ? COL_BAR_ON  : COL_BAR_OFF];
	const float *line  = a->pal[ds->is_active ? COL_LINE_ON : COL_LINE_OFF];
	const float *white = a->pal[ds->is_active ? COL_ICON_ON : COL_ICON_OFF];

	if (!ds->custom_bar)
		overlay_rect(a, 0, 0, w, DECO_TITLEBAR_H, bar);

	/* The window outline, and for a caller's bar too: it is the window's edge
	 * and not the titlebar's, and the black presentable area needs one to be
	 * seen against whatever is behind it. */
	overlay_frame(a, 0, 0, w, h, line);
	if (ds->custom_bar)
		return;

	int  min_x, max_x, full_x, close_x, by;
	bool vis;
	deco_button_rects(a, w, &min_x, &max_x, &full_x, &close_x, &by, &vis);

	/* Hover highlight behind the button under the pointer. Ordered by Zone, not
	 * by screen position, so this cannot drift out of step with the backend's
	 * hit test however the buttons are arranged. */
	const int btn_x[] = { min_x, max_x, full_x, close_x };
	Zone hb = ds->hovered_button;
	if (vis && hb >= ZONE_BUTTON_MIN && hb <= ZONE_BUTTON_CLOSE &&
		btn_x[hb - ZONE_BUTTON_MIN] >= 0)
		overlay_rect(a, btn_x[hb - ZONE_BUTTON_MIN], by,
					 DECO_BTN_W, DECO_BTN_H,
					 hb == ZONE_BUTTON_CLOSE ? a->pal[COL_CLOSE] : line);

	if (!vis)
		return;

	/* Icons are antialiased strokes rather than filled rectangles: the close
	 * "X" is diagonal, and at fractional scales even the axis-aligned ones
	 * land off the pixel grid. The stroke's spine runs down its middle, hence
	 * the half-width insets that keep each icon inside its box.
	 *
	 * Coordinates use the box corners (l, t) - (r, b). */
	const float icon  = 12.0f;        /* icon box edge */
	const float thick = 1.8f;         /* stroke width  */
	const float half  = thick * 0.5f;

	/* Spine length of a fullscreen bracket arm. The stroke is a capsule, so
	 * its round cap paints half the width plus the feather past each endpoint;
	 * the drawn arm is that much longer at both ends. Sized so the two arms on
	 * an edge still leave a clear gap rather than closing into a box. */
	const float arm = 2.8f;

	const float t = (float)by + (DECO_BTN_H - icon) / 2.0f;
	const float b = t + icon;
	/* Stroke spines, inset by half the width so the painted outer edge lands
	 * on the icon box rather than straddling it. */
	const float t_in = t + half;
	const float b_in = b - half;

#define STROKE(x0, y0, x1, y1) \
	overlay_stroke(a, (x0), (y0), (x1), (y1), thick, white)
#define ELBOW(x0, y0, x1, y1, x2, y2) \
	overlay_elbow(a, (x0), (y0), (x1), (y1), (x2), (y2), thick, white)
#define BOX(x0, y0, x1, y1) \
	overlay_box(a, (x0), (y0), (x1), (y1), thick, white)

	/* minimize: a single bar sharing the maximize box's bottom edge -- same
	 * baseline and same span, so the two icons line up across the strip. */
	if (min_x >= 0) {
		const float nl = (float)min_x + (DECO_BTN_W - icon) / 2.0f;
		STROKE(nl + half, b_in, nl + icon - half, b_in);
	}

	/* maximize: a hollow box -- or, once maximized, the restore icon: two
	 * boxes offset along the diagonal, the back one contributing only the two
	 * arms not hidden behind the front one.
	 *
	 * Each box is one draw, and the back one's two arms a single bend.
	 * Strokes drawn separately blend their fringes twice where they meet,
	 * which is denser than once: a bright dot at every corner, most obvious
	 * on the dimmed icon of an unfocused titlebar. */
	if (max_x >= 0) {
		const float ml = (float)max_x + (DECO_BTN_W - icon) / 2.0f;
		const float mr = ml + icon;
		if (ds->is_maximized) {
			const float off = 3.0f;           /* diagonal offset between them */
			const float fl = ml + half, fr = mr - half - off;  /* front box */
			const float ft = t_in + off, fb = b_in;
			BOX(fl, ft, fr, fb);
			ELBOW(fl + off, ft - off,       /* back box: along the top edge,  */
				  fr + off, ft - off,       /* round the corner, and down the */
				  fr + off, fb - off);      /* right side                     */
		} else {
			BOX(ml + half, t_in, mr - half, b_in);
		}
	}

	/* fullscreen: four corner brackets, each one draw bent at the corner
	 * rather than two strokes meeting there, for the same reason. */
	const float fl = (float)full_x + (DECO_BTN_W - icon) / 2.0f;
	const float fr = fl + icon;
	const float fl_in = fl + half, fr_in = fr - half;
	ELBOW(fl_in + arm, t_in, fl_in, t_in, fl_in, t_in + arm);  /* top-left    */
	ELBOW(fr_in - arm, t_in, fr_in, t_in, fr_in, t_in + arm);  /* top-right   */
	ELBOW(fl_in + arm, b_in, fl_in, b_in, fl_in, b_in - arm);  /* bottom-left */
	ELBOW(fr_in - arm, b_in, fr_in, b_in, fr_in, b_in - arm);  /* bottom-right*/

	/* close: two diagonals -- the reason this pipeline exists */
	const float cl = (float)close_x + (DECO_BTN_W - icon) / 2.0f;
	const float cr = cl + icon;
	STROKE(cl + half, t + half, cr - half, b - half);
	STROKE(cr - half, t + half, cl + half, b - half);

#undef STROKE
#undef ELBOW
#undef BOX

	/* The one string set in the proportional face: the caller's title, the
	 * same string the window server was given. Dropped when it would run
	 * into the controls or the buttons; see deco_title_fits(). */
	if (deco_title_fits(a, w))
		text_draw(a, DECO_TITLE_X,
				  (DECO_TITLEBAR_H - text_height(TEXT_UI)) / 2, TEXT_UI,
				  white, ds->title);

	/* The controls, last of everything the bar paints. Given the presentable
	 * area because the size box reads it out -- see ctl_combo_label(). */
	ctl_draw(a, ds, w, area_w, area_h);
}

/* The cover at `alpha` (DESIGN *The veil*): the last frame before a move,
 * read back and drawn over the whole window by the renderer
 * (FnlRendererVtbl::overlay_cover); or black, where this transition has
 * no frame to show (App::veil_black) or the renderer would not upload it,
 * as one capsule stroke down the window's middle, `w` and `h` the window
 * in points, wide enough that every pixel of the window lies inside its
 * body -- half the height, plus two points past the feather -- so the
 * stroke pipeline, which blends and keeps the alpha channel "over",
 * darkens the whole window in one draw with no pipeline of its own in any
 * renderer. Scissored to the window either way: the capsule's round ends
 * and its feather fall outside it, the frame read back has the grab ring
 * in it, and the ring is not the window's to paint. After text, which is
 * one draw for the whole frame, so the glyphs are under it too. A stroke
 * arena a caller has filled drops the black, as it drops the caller's
 * own. */
static void draw_veil(App *a, float alpha, int w, int h)
{
	FnlRect win;
	if (!overlay_clip(a, 0, 0, w, h, &win))
		return;
	const float black[4] = { 0.0f, 0.0f, 0.0f, alpha };
	const float mid = (float)h * 0.5f;
	overlay_scissor(a, &win);
	if (a->veil_black || !RENDERER(a)->overlay_cover(a, alpha)) {
		overlay_stroke(a, 0.0f, mid, (float)w, mid, (float)h + 4.0f, black);
		overlay_flush_shapes(a);
	}
	overlay_scissor(a, NULL);
}

/* The whole overlay, in the order app.h's invariant requires: rectangles are
 * all backgrounds, so every rectangle, then every stroke, then every glyph
 * puts each primitive where call order would have, in three draws.
 *
 * `img` is which per-image instance arena to append into, bounded by the
 * OVERLAY_MAX_IMAGES create_swapchain() refuses to exceed. */
FnlResult draw_overlay(App *a, const FnlDrawState *ds, uint32_t img)
{
	overlay_begin(a, img);
	text_begin(a, img);

	const int w = logical_w(a), h = logical_h(a);
	int ax, ay, aw, ah;
	content_px(a, ds, &ax, &ay, &aw, &ah);

	draw_decorations(a, ds, w, h, aw, ah);

	/* The claim list is rebuilt from nothing every frame, so a control that
	 * stopped being drawn stops being hit-testable in the same frame. Cleared
	 * even with no callback, which is what publishes the empty list a caller
	 * that stopped drawing its bar should get. */
	a->claim_count = 0;

	/* The caller's, on top of the library's and into the same three streams,
	 * so the flushes below cover both. Here rather than in on_record because
	 * this is the window pass: a flush or a clip issued from the scene's pass
	 * would land in the wrong one whenever the scene is drawn offscreen. */
	if (a->cb.on_overlay) {
		/* The frame the *scene* was drawn for. `target` is derived rather
		 * than assumed, or the pair can contradict itself: ds->scene_w is
		 * non-zero exactly when an offscreen target was built, and
		 * active_samples() answers for a device that refused the
		 * multisampled one. */
		const int scene_samples = RENDERER(a)->active_samples(a, ds);
		FnlFrame f = {
			.index      = a->presents,
			.time       = now_seconds() - a->started_at,
			.scene_w_px = ds->scene_w,
			.scene_h_px = ds->scene_h,
			.target     = ds->scene_w <= 0    ? FNL_TARGET_DIRECT
						: scene_samples > 1   ? FNL_TARGET_OFFSCREEN_MS
											  : FNL_TARGET_OFFSCREEN,
			.samples    = scene_samples,
			.scene_time = ds->scene_time,
		};
		f.area_w_px = aw;
		f.area_h_px = ah;
		if (f.scene_w_px <= 0) {
			f.scene_w_px = aw;
			f.scene_h_px = ah;
		}
		a->cb.on_overlay(a->cb.ud, a, &f);
	}

	/* A clip the caller left in force, taken back, with everything it was set
	 * for issued first, under it. A scissor is command state and would reach
	 * two draws that are not the caller's: the library's modal drop-down
	 * below, and text, which is one draw for the whole frame. */
	if (a->overlay_clipped) {
		overlay_flush_rects(a);
		overlay_flush_shapes(a);
		overlay_scissor(a, NULL);
		a->overlay_clipped = false;
	}

	/* Published for the input thread's hit test, under the lock the rest of
	 * the published block takes -- see App::pub_claims. After the callback
	 * rather than inside fnl_claim(), so the list swaps over whole and a hit
	 * test can never catch it half-rebuilt. Every frame, whoever owns the
	 * bar: fnl_claim() reaches the presentable area under either. */
	fnl_mutex_lock(&a->lock);
	memcpy(a->pub_claims, a->claims,
		   (size_t)a->claim_count * sizeof a->claims[0]);
	a->pub_claim_count = a->claim_count;
	fnl_mutex_unlock(&a->lock);

	/* The metrics panel over the caller's drawing (src/core/metrics.c),
	 * and under the library's own drop-down, which opens from the bar
	 * over the same corner. */
	const FnlResult mrc = app_metrics_draw(a, ds, w, h);
	/* The library's own drop-down, after the caller so that it is over the
	 * caller's drawing; see ctl_draw_popup(). */
	const FnlResult prc = ctl_draw_popup(a, ds, w);
	/* A calibration's panel over all of it (src/core/rank.c). */
	const FnlResult brc = app_rank_draw(a, w, h);

	overlay_flush_rects(a);        /* backgrounds  */
	overlay_flush_shapes(a);       /* icon strokes */
	FnlResult rc = text_end(a);    /* every glyph not yet drawn, one draw */
	if (rc == FNL_OK)
		rc = mrc;
	if (rc == FNL_OK)
		rc = prc;
	if (rc == FNL_OK)
		rc = brc;

	/* The veil over all of it, the glyphs' one draw included. */
	if (ds->veil > 0.0f)
		draw_veil(a, ds->veil, w, h);

	/* Closes the three arenas. Everything above may append into them and
	 * nothing below may, which is what the [overlay] tag in fenestal.h means
	 * and what App::overlay_open enforces. */
	overlay_end(a);
	return rc;
}

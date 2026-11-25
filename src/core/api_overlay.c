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
 * Memoised by colour. Drawing arrives in runs of one colour and as often in a
 * pair that alternates -- a label and its value, down every row -- and a
 * window of panels is a palette of thirty used in an order no small table can
 * hold: four entries missed on every call, at three powf() a miss, and the
 * miss is the whole cost of a colour a caller has already used. So the memo is
 * keyed on the colour and sized for a palette.
 *
 * Eight slots are probed from the hash and then the first is overwritten, so a
 * lookup is bounded however the table fills, and a miss costs what an uncached
 * conversion would have less the probe.
 *
 * Keyed on the blend space rather than on the App: a device-loss recovery
 * re-resolves the surface format on the same App.
 *
 * Thread-local, so it needs no lock. The zeroed initial state is correct:
 * a linear surface converts transparent black to itself, and every other
 * first use misses.
 */
#define COLOR_MEMO 64
#define COLOR_PROBE 8

static unsigned color_hash(FnlColor c)
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < 4; i++) {
		uint32_t u;
		memcpy(&u, (const char *)&c + i * sizeof u, sizeof u);
		h = (h ^ u) * 16777619u;
	}
	return h;
}

static void to_surface(const App *a, FnlColor c, float out[4])
{
	static FNL_THREAD_LOCAL struct {
		bool linear;
		float scale;
		FnlColor in;
		float out[4];
	} memo[COLOR_MEMO];
	const unsigned start = color_hash(c) % COLOR_MEMO;
	/* The target's unit for SDR white, latched for the frame being drawn
	 * (the render thread's, like App::draw_scale120). */
	const float scale = a->draw_color.sdr_scale > 0.0f
							? a->draw_color.sdr_scale
							: 1.0f;

	/* The tag is the whole colour and the blend space it was converted for,
	 * so a collision is a miss and the probe goes on; a slot holding
	 * another colour is never returned. */
	for (unsigned n = 0; n < COLOR_PROBE; n++) {
		const unsigned i = (start + n) % COLOR_MEMO;
		if (a->linear_target == memo[i].linear && memo[i].scale == scale &&
			c.r == memo[i].in.r && c.g == memo[i].in.g &&
			c.b == memo[i].in.b && c.a == memo[i].in.a) {
			memcpy(out, memo[i].out, sizeof memo[i].out);
			return;
		}
	}

	const float rgb[3] = { c.r, c.g, c.b };
	if (a->linear_target) {
		for (int i = 0; i < 3; i++)
			out[i] = srgb_to_linear(rgb[i]) * scale;
	} else {
		for (int i = 0; i < 3; i++)
			out[i] = rgb[i];
	}
	out[3] = c.a;

	/* The first slot, which the probe has not moved: a table that fills is a
	 * caller with more colours than slots, and the oldest answer is the cheapest
	 * one to lose. */
	memo[start].linear = a->linear_target;
	memo[start].scale  = scale;
	memo[start].in     = c;
	memcpy(memo[start].out, out, sizeof memo[start].out);
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
int fnl_text_wrap(char *dst, size_t cap, const char *src, int max_w,
				  FnlTextSize sz)
{
	if (!size_ok(sz)) {
		if (dst && cap)
			dst[0] = '\0';
		return 0;
	}
	return text_wrap(dst, cap, src, max_w, (TextSize)sz);
}

void fnl_text_elide(char *dst, size_t cap, const char *src, int max_w,
					FnlTextSize sz)
{
	if (!size_ok(sz)) {
		if (dst && cap)
			dst[0] = '\0';
		return;
	}
	text_elide(dst, cap, src, max_w, (TextSize)sz);
}
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
	if (w)
		*w = ww;
	if (h)
		*h = hh;
}

/* ---- drawing ------------------------------------------------------------- */

void fnl_overlay_text_in(FnlApp *app, int x, int y, int w, FnlTextAlign align,
						 FnlTextSize sz, FnlColor c, const char *utf8)
{
	App *a = (App *)app;
	if (!overlay_here(a) || !utf8 || !size_ok(sz))
		return;
	/* Bounded before they meet: the sum below is two caller figures, and a
	 * pair of extremes wraps an int before the clamp on the answer reaches
	 * it. clamp_pt() is the bound every other overlay coordinate takes. */
	x = clamp_pt(x);
	w = clamp_pt(w);
	/* The end the rectangle's edge lands on, measured before the string is
	 * drawn: the width is fnl_text_width()'s, so a string measured once a
	 * frame is free, and a string the memo holds costs a compare. */
	if (w > 0 && align != FNL_TEXT_ALIGN_LEFT) {
		const int tw = text_width(utf8, sz);
		if (align == FNL_TEXT_ALIGN_RIGHT)
			x += w - tw;
		else
			x += (w - tw) / 2;
	}
	float col[4];
	to_surface(a, c, col);
	text_draw(a, clamp_pt(x), clamp_pt(y), sz, col, utf8);
}

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

/* ---- images ----------------------------------------------------------------
 *
 * The caller's picture, the library's copy of it, and the renderer's object.
 * All of it on the render thread -- the thread that runs on_overlay, a post
 * and on_gpu_created -- which is what lets the copy be read and the device
 * written without a lock between them: fenestal.h says so where the calls are
 * declared, and a worker that has pixels posts them (fnl_post()). */

/* Bytes a pixel of this format, or 0 for one there is none of. */
static unsigned image_pixel_bytes(FnlImageFormat fmt)
{
	return fmt == FNL_IMAGE_RGBA8 ? 4u : 0u;
}

bool fnl_image_supported(const FnlApp *app)
{
	const App *a = (App *)app;
	/* The published table, not App::renderer: a caller on any thread asks
	 * this, and a move to another renderer may be freeing the drawing one's
	 * as it does (renderer.h). */
	const FnlRendererVtbl *r = a ? renderer_any(a) : NULL;
	if (!r || !r->image_supported)
		return false;
	return r->image_supported(a);
}

bool fnl_image_valid(const FnlApp *app, const FnlImage *img)
{
	App *a = (App *)app;
	if (!a || !img || !img->pixels)
		return false;
	/* An image is drawable when the renderer drawing now can draw one and a
	 * device has been handed back since the last loss. App::cb_gpu_created
	 * is true from the frame on_gpu_created is delivered until the next loss,
	 * so a caller gating a draw on this skips the frame a loss would
	 * otherwise take. */
	if (!fnl_image_supported(a))
		return false;
	return a->cb_gpu_created;
}

FnlImage *fnl_image_create(FnlApp *app, int w, int h, FnlImageFormat fmt,
						   const void *pixels)
{
	App *a             = (App *)app;
	const unsigned bpp = image_pixel_bytes(fmt);

	if (!a || !pixels)
		return NULL;
	if (app_thread() != APP_THREAD_RENDER) {
		fnl_log(FNL_LOG_ERROR, "fnl_image_create() is the render thread's: "
							   "call it in on_overlay, in a fnl_post(), or in "
							   "on_gpu_created");
		return NULL;
	}
	if (w <= 0 || h <= 0 || bpp == 0) {
		fnl_log(FNL_LOG_ERROR, "fnl_image_create(): %dx%d, format %d", w, h,
				(int)fmt);
		return NULL;
	}
	/* The same bound the drawing calls hold their points to, since the pixels
	 * are the same size as the rectangle they may be drawn into. */
	if (w > OVERLAY_COORD_MAX || h > OVERLAY_COORD_MAX)
		return NULL;
	if (!fnl_image_supported(a)) {
		fnl_log(FNL_LOG_WARN, "fnl_image_create(): %s cannot draw an image",
				fnl_renderer_name(a));
		return NULL;
	}

	FnlImage *img = calloc(1, sizeof *img);
	if (!img)
		return NULL;
	const size_t n = (size_t)w * (size_t)h * bpp;
	img->pixels    = malloc(n);
	if (!img->pixels) {
		free(img);
		return NULL;
	}
	/* The copy: the caller's may be a stack buffer, and a rebuild reads this
	 * one again. */
	memcpy(img->pixels, pixels, n);
	img->app  = a;
	img->w    = w;
	img->h    = h;
	img->fmt  = fmt;
	img->next = a->images;
	a->images = img;
	return img;
}

void fnl_image_destroy(FnlImage *img)
{
	if (!img)
		return;
	App *a = img->app;
	if (!a)
		return;
	for (FnlImage **pp = &a->images; *pp; pp = &(*pp)->next)
		if (*pp == img) {
			*pp = img->next;
			break;
		}
	if (img->gpu && RENDERER(a)->image_free)
		RENDERER(a)->image_free(a, img);
	free(img->pixels);
	free(img);
}

/* What is left at teardown, after the render thread has been joined and
 * before the device goes: fnl_image_destroy() on each. */
void app_images_free(App *a)
{
	while (a->images)
		fnl_image_destroy(a->images);
}

void fnl_overlay_image(FnlApp *app, int x, int y, int w, int h,
					   const FnlImage *img)
{
	App *a = (App *)app;
	if (!overlay_here(a) || !img || !RENDERER(a)->overlay_image)
		return;
	/* The rectangle in the framebuffer's pixels, as a rectangle's own is
	 * (overlay_rect), so a picture and a rectangle at the same coordinates
	 * land on the same pixels however the scale and the margin are. */
	/* Bounded like every other overlay coordinate (clamp_pt): x + w and the
	 * margin's product must not run past an int. */
	FnlRect r;
	if (!overlay_clip(a, clamp_pt(x), clamp_pt(y), clamp_pt(w), clamp_pt(h),
					  &r))
		return;
	/* What was appended before it is behind it, as it was drawn before it:
	 * both streams go out first, so the order is the call order. Not text,
	 * which is one draw for the frame and goes last (see the note on
	 * fnl_overlay_flush()). */
	overlay_flush_rects(a);
	overlay_flush_shapes(a);
	RENDERER(a)->overlay_image(a, (FnlImage *)img, r.x, r.y, r.w, r.h);
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

void fnl_overlay_rect_blend(FnlApp *app, int x, int y, int w, int h, FnlColor c)
{
	App *a = (App *)app;
	if (!overlay_here(a))
		return;
	/* The solid stream is drawn before the blended one, so what a wash is
	 * over is already there: the library's own background rules end here. */
	overlay_flush_rects(a);
	float col[4];
	to_surface(a, c, col);
	overlay_rect_blend(a, clamp_pt(x), clamp_pt(y), clamp_pt(w), clamp_pt(h),
					   col);
}

void fnl_overlay_rect_round(FnlApp *app, int x, int y, int w, int h,
							float radius, FnlColor c)
{
	App *a = (App *)app;
	if (!overlay_here(a))
		return;
	/* A corner past half the shorter side folds the box in on itself, so it
	 * is held to the disc the box can carry -- the same clamp the shader's
	 * coverage assumes. */
	const int short_side = w < h ? w : h;
	if (radius > (float)short_side * 0.5f)
		radius = (float)short_side * 0.5f;
	if (radius < 0.0f)
		radius = 0.0f;
	float col[4];
	to_surface(a, c, col);
	overlay_rect_round(a, clamp_pt(x), clamp_pt(y), clamp_pt(w), clamp_pt(h),
					   radius, col);
}

void fnl_overlay_circle(FnlApp *app, float cx, float cy, float r, FnlColor c)
{
	App *a = (App *)app;
	if (!overlay_here(a) || !(r > 0.0f))
		return;
	/* The centre is a position, and is clamped like every other point a caller
	 * hands us: a disc whose middle is off the map can still be worth drawing.
	 * The radius is not -- a disc of a clamped radius is silently a different
	 * picture from the one asked for, and one large enough to be past the bound
	 * would cover the caller's whole window. So it is refused, on
	 * fnl_image_create()'s terms, and said once. */
	const float lim = (float)OVERLAY_COORD_MAX;
	if (r > lim) {
		static bool said;
		if (!said) {
			said = true;
			fnl_log(FNL_LOG_WARN,
					"fnl_overlay_circle(): a radius of %.0f points is past "
					"the bound of %d, so nothing is drawn",
					(double)r, OVERLAY_COORD_MAX);
		}
		return;
	}
	cx = cx > lim ? lim : cx < -lim ? -lim : cx;
	cy = cy > lim ? lim : cy < -lim ? -lim : cy;
	float col[4];
	to_surface(a, c, col);
	overlay_circle(a, cx, cy, r, col);
}

void fnl_overlay_gradient(FnlApp *app, int x, int y, int w, int h, float angle,
						  FnlColor c, FnlColor c2)
{
	App *a = (App *)app;
	if (!overlay_here(a))
		return;
	float col[4], col2[4];
	to_surface(a, c, col);
	to_surface(a, c2, col2);
	overlay_gradient(a, clamp_pt(x), clamp_pt(y), clamp_pt(w), clamp_pt(h),
					 angle, col, col2);
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

void fnl_overlay_counts(const FnlApp *app, FnlOverlayCounts *out)
{
	App *a = (App *)app;
	if (!out)
		return;
	/* Zeroed rather than left alone: a caller that asks outside a recording is
	 * asking before anything was appended, and the ceilings are the same
	 * whenever it asks. */
	out->rects = out->strokes = out->glyphs = 0;
	out->rects_max                          = OVERLAY_MAX_RECTS;
	out->strokes_max                        = OVERLAY_MAX_SHAPES;
	out->glyphs_max                         = TEXT_MAX_GLYPHS;
	if (!overlay_here(a))
		return;
	out->rects   = (int)a->rect_count;
	out->strokes = (int)a->shape_count;
	out->glyphs  = (int)a->text_count;
}

/* ---- controls in the titlebar -------------------------------------------- */

bool fnl_titlebar_reserve(FnlApp *app, int want_w, int *x, int *y, int *w,
						  int *h)
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
	const int win_w   = a->pub_win_w;
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

	if (x)
		*x = rx;
	if (y)
		*y = ry;
	if (w)
		*w = rw;
	if (h)
		*h = rh;
	return true;
}

/* ---- a titlebar of the caller's own -------------------------------------- */

bool fnl_titlebar_custom(FnlApp *app, int height_pt, int min_width_pt)
{
	App *a = (App *)app;
	if (!a || !a->host->backend)
		return false;
	/* Both bounded (see clamp_pt in app.h). Zero means "the library's own
	 * bar" for the first and "no width floor" for the second, so a negative
	 * collapses to it rather than being clamped symmetrically. */
	height_pt    = height_pt > 0 ? clamp_pt(height_pt) : 0;
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
	const int new_h   = custom ? height_pt : DECO_TITLEBAR_H;
	const int old_h   = a->deco_h;
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
	const FnlRect claim = { clamp_pt(x), clamp_pt(y), clamp_pt(w),
							clamp_pt(h) };
	a->claims[a->claim_count++] = claim;
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

int fnl_claim_at(const FnlApp *app, double x_pt, double y_pt)
{
	/* The range every point figure takes (app.h, OVERLAY_COORD_MAX), tested
	 * as the bounds themselves: a NaN fails both comparisons, and a cast of
	 * one -- or of a figure past an int -- is undefined. */
	if (!(x_pt >= -(double)OVERLAY_COORD_MAX &&
		  x_pt <= (double)OVERLAY_COORD_MAX &&
		  y_pt >= -(double)OVERLAY_COORD_MAX &&
		  y_pt <= (double)OVERLAY_COORD_MAX))
		return -1;
	/* Floored, not truncated, as deco_hit() is: half a point outside the left
	 * edge is outside it. */
	return deco_claim_at((App *)app, (int)floor(x_pt), (int)floor(y_pt));
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

bool fnl_claim_focus(FnlApp *app, int x, int y, int w, int h)
{
	App *a = (App *)app;
	if (!overlay_here(a))
		return false;
	/* A claim that asks for the keyboard: the library moves the focus to it
	 * on Tab or Shift+Tab from inside a claim that already has it, and the
	 * caller draws the ring (FNL_EVENT_CLAIM_FOCUS says where the focus
	 * landed). The claim is an ordinary one for the pointer -- fnl_claim_at()
	 * answers for it the same way -- so a caller that wants both calls both,
	 * or this once with the rectangle it drew the control in. */
	const int at = a->claim_count;
	if (!claim_add(a, x, y, w, h))
		return false;
	a->claim_focus[at] = true;
	return true;
}

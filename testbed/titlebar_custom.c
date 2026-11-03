/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * A titlebar drawn entirely by this program, in place of the library's:
 * reached with --custom-titlebar, written against the public API and nothing
 * else, at a height the library's own bar does not have.
 *
 * The split is picture and behaviour. The *picture* is all here -- colours,
 * height, button shapes, where they sit and what a click on one does. The
 * *behaviour* stays the library's: a press on any part of the bar that is not
 * claimed drags the window, through the window server's own interactive
 * move, which is what buys snap, edge resistance and the double-click that
 * maximizes. So: say which rectangles are yours, draw whatever you like, and
 * the rest of the strip goes on being a titlebar.
 *
 * fnl_titlebar_claim() is called from the draw, beside the button it
 * describes, and the list it builds lasts exactly one frame. The rectangle
 * handed to the library is computed by the same line that drew the button,
 * so a control is never clickable somewhere it is not shown, and one that
 * stops being drawn goes back to being drag strip in the same frame. Claims
 * registered once at startup go stale the first time the window resizes.
 */

#include <fenestal/fenestal.h>

#include "demo.h"

#include <stdbool.h>

/* Taller than the library's bar, visibly, to show that the geometry follows. */
#define BAR_H        44
/* The width below which the button strip would be drawn over the title. Handed
 * to fnl_titlebar_custom() as the window's floor, because the library cannot
 * measure a bar it does not draw. */
#define BAR_MIN_W    260

#define BTN_W        34
#define BTN_H        24
#define BTN_GAP       4
#define BTN_MARGIN    8
#define TITLE_X      16

static const FnlColor COL_BAR_ON   = DEMO_RGB(0.10f, 0.11f, 0.16f);
static const FnlColor COL_BAR_OFF  = DEMO_RGB(0.08f, 0.08f, 0.10f);
static const FnlColor COL_ACCENT   = DEMO_RGB(0.35f, 0.75f, 1.00f);
static const FnlColor COL_TEXT_ON  = DEMO_RGB(0.92f, 0.94f, 1.00f);
static const FnlColor COL_TEXT_OFF = DEMO_RGB(0.50f, 0.52f, 0.58f);
static const FnlColor COL_HOVER    = DEMO_RGB(0.20f, 0.22f, 0.30f);
static const FnlColor COL_CLOSE    = DEMO_RGB(0.80f, 0.25f, 0.25f);

/* Which button, in the order they are laid out from the right. One enum, so
 * the draw and the hit test index the same list. */
enum { BTN_CLOSE, BTN_MAX, BTN_MIN, BTN_FULL, BTN_COUNT };

static int   hovered = -1;    /* which button the pointer is over, or -1 */
static bool  enabled = false; /* --custom-titlebar was passed */

void demo_titlebar_custom_enable(void) { enabled = true; }
bool demo_titlebar_custom_enabled(void) { return enabled; }
int  demo_titlebar_custom_height(void) { return BAR_H; }
int  demo_titlebar_custom_min_width(void) { return BAR_MIN_W; }

/* Where button `i` sits, in window points, right to left from the window's
 * right edge. A pure function of the window width, so the draw and the hit
 * test cannot disagree. Returns false once the strip would collide with the
 * title: nothing is drawn, nothing is claimed, and the bar is all drag strip.
 */
static bool button_rect(int win_w, int i, int *x, int *y, int *w, int *h)
{
	const int strip = BTN_MARGIN + BTN_COUNT * BTN_W +
					  (BTN_COUNT - 1) * BTN_GAP;
	if (i < 0 || i >= BTN_COUNT)
		return false;
	/* All four or none. One at a time, the leftmost would go first and close
	 * (i == 0, against the right edge) last, which would serve; but a bar
	 * whose buttons come and go with its width is one more state to draw
	 * and claim, for a case BAR_MIN_W keeps hard to reach. */
	if (win_w - strip <= TITLE_X)
		return false;

	*x = win_w - BTN_MARGIN - BTN_W - i * (BTN_W + BTN_GAP);
	*y = (BAR_H - BTN_H) / 2;
	*w = BTN_W;
	*h = BTN_H;
	return true;
}

/* An icon inside its button box: thin antialiased strokes rather than filled
 * rectangles, so they stay crisp at a fractional scale. */
static void draw_icon(FnlApp *app, int i, int bx, int by, FnlColor c)
{
	const float cx = (float)bx + BTN_W * 0.5f;
	const float cy = (float)by + BTN_H * 0.5f;
	const float r  = 5.0f;
	const float t  = 1.6f;

	switch (i) {
	case BTN_CLOSE:
		fnl_overlay_stroke(app, cx - r, cy - r, cx + r, cy + r, t, c);
		fnl_overlay_stroke(app, cx - r, cy + r, cx + r, cy - r, t, c);
		break;
	case BTN_MAX:
		fnl_overlay_box(app, cx - r, cy - r, cx + r, cy + r, t, c);
		break;
	case BTN_MIN:
		fnl_overlay_stroke(app, cx - r, cy + r, cx + r, cy + r, t, c);
		break;
	default:   /* BTN_FULL: a box with its middle two edges opened out */
		fnl_overlay_stroke(app, cx - r, cy - r, cx,     cy - r, t, c);
		fnl_overlay_stroke(app, cx - r, cy - r, cx - r, cy,     t, c);
		fnl_overlay_stroke(app, cx + r, cy + r, cx,     cy + r, t, c);
		fnl_overlay_stroke(app, cx + r, cy + r, cx + r, cy,     t, c);
		break;
	}
}

void demo_titlebar_custom_draw(FnlApp *app, const FnlFrame *f)
{
	(void)f;
	if (!enabled || fnl_is_fullscreen(app))
		return;

	int win_w = 0, win_h = 0;
	fnl_window_size_pt(app, &win_w, &win_h);
	(void)win_h;

	const bool act  = fnl_is_active(app);
	const FnlColor bg   = act ? COL_BAR_ON   : COL_BAR_OFF;
	const FnlColor text = act ? COL_TEXT_ON  : COL_TEXT_OFF;

	/* The bar itself, then everything on it. Every filled rectangle is a
	 * background -- the overlay's rule in fenestal.h -- so the accent and the
	 * hover highlight go down before any stroke or glyph. */
	fnl_overlay_rect(app, 0, 0, win_w, BAR_H, bg);
	fnl_overlay_rect(app, 0, 0, 3, BAR_H, COL_ACCENT);

	for (int i = 0; i < BTN_COUNT; i++) {
		int bx, by, bw, bh;
		if (!button_rect(win_w, i, &bx, &by, &bw, &bh))
			continue;
		if (i == hovered)
			fnl_overlay_rect(app, bx, by, bw, bh,
							 i == BTN_CLOSE ? COL_CLOSE : COL_HOVER);
		/* Claimed beside the rectangle it was drawn at, so the two cannot
		 * describe different places. Without this the press would reach the
		 * window server as a drag and the button would never fire. */
		fnl_titlebar_claim(app, bx, by, bw, bh);
	}

	fnl_overlay_text(app, TITLE_X, (BAR_H - fnl_text_height(FNL_TEXT_UI)) / 2,
					 FNL_TEXT_UI, text,
					 "fenestal-testbed \xe2\x80\x94 custom bar");

	for (int i = 0; i < BTN_COUNT; i++) {
		int bx, by, bw, bh;
		if (button_rect(win_w, i, &bx, &by, &bw, &bh))
			draw_icon(app, i, bx, by, text);
	}
}

/* Which button a window-space point is over, or -1. The same layout function
 * the draw walks, in the same order. */
static int button_at(FnlApp *app, double px, double py)
{
	int win_w = 0, win_h = 0;
	fnl_window_size_pt(app, &win_w, &win_h);
	(void)win_h;
	if (py < 0 || py >= BAR_H)
		return -1;

	for (int i = 0; i < BTN_COUNT; i++) {
		int bx, by, bw, bh;
		if (button_rect(win_w, i, &bx, &by, &bw, &bh) &&
			px >= bx && px < bx + bw && py >= by && py < by + bh)
			return i;
	}
	return -1;
}

void demo_titlebar_custom_event(FnlApp *app, const FnlEvent *ev)
{
	if (!enabled)
		return;
	/* No bar in fullscreen, so nothing is hovered. The pointer's shape is
	 * state: the button that asked for fullscreen was under the pointer, and
	 * the hand it set would otherwise sit over the whole scene. */
	if (fnl_is_fullscreen(app)) {
		if (hovered >= 0) {
			hovered = -1;
			fnl_set_cursor(app, FNL_CURSOR_DEFAULT);
		}
		return;
	}

	if (ev->type == FNL_EVENT_MOTION) {
		const int was = hovered;
		hovered = button_at(app, ev->motion.x, ev->motion.y);
		/* The library cannot see that this changed, so without the
		 * invalidate the frame would not re-record and the highlight would
		 * not appear. */
		if (hovered != was) {
			fnl_invalidate(app);
			/* A hand over anything that can be pressed, and put back on the
			 * way off: this bar is the caller's, so its shape is too. */
			fnl_set_cursor(app, hovered >= 0 ? FNL_CURSOR_POINTER
											 : FNL_CURSOR_DEFAULT);
		}
		return;
	}

	if (ev->type != FNL_EVENT_BUTTON || !ev->button.pressed ||
		ev->button.button != FNL_BUTTON_LEFT)
		return;

	switch (button_at(app, ev->button.x, ev->button.y)) {
	case BTN_CLOSE: fnl_quit(app);                                    break;
	case BTN_MIN:   fnl_minimize(app);                                break;
	case BTN_MAX:   fnl_set_maximized(app, !fnl_is_maximized(app));   break;
	case BTN_FULL:  fnl_set_fullscreen(app, !fnl_is_fullscreen(app)); break;
	default:        break;
	}
}

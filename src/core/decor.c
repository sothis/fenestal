/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The client-side titlebar: where the buttons sit, where a caller's run of
 * the strip goes, whether the title still fits, what a point in the window
 * lands on, and what a press there does. Every backend calls it. All figures
 * are in logical points; scale_pt() converts.
 */

#include "core/app.h"

/* Button strip, laid out right-to-left from the window's right edge:
 *
 *     [ full ]   [ min ][ max ][ close ]
 *              ^ DECO_BTN_GROUP_GAP
 *
 * Fullscreen leads on the left, set off from the minimize/maximize/close trio
 * by the wider group gap. Callers address the buttons by name, so this is the
 * only place the on-screen order is decided.
 *
 * Where the window is the output (FnlBackendVtbl::is_output) there is
 * nothing to minimize into or restore from, so the two are left out rather
 * than drawn dead, and fullscreen keeps the group gap to close:
 *
 *     [ full ]   [ close ]
 *
 * A button left out has -1 for its x, which no shown button can have. The
 * floor, DECO_MIN_W, stays the whole strip's: such a window is its mode. */
void deco_button_rects(const App *a, int w, int *min_x, int *max_x,
					   int *full_x, int *close_x, int *by, bool *visible)
{
	*by      = (DECO_TITLEBAR_H - DECO_BTN_H) / 2;
	*close_x = w - DECO_BTN_MARGIN - DECO_BTN_W;
	if (BACKEND(a)->is_output) {
		*max_x  = *min_x = -1;
		*full_x = *close_x - DECO_BTN_GROUP_GAP - DECO_BTN_W;
	} else {
		*max_x  = *close_x - DECO_BTN_GAP - DECO_BTN_W;
		*min_x  = *max_x   - DECO_BTN_GAP - DECO_BTN_W;
		*full_x = *min_x   - DECO_BTN_GROUP_GAP - DECO_BTN_W;
	}
	*visible = (*full_x > DECO_BTN_MARGIN); /* the strip clears the left edge */
}

/* Where the title stops, with the clear space anything to its right keeps.
 * App::title_w rather than a measurement: this runs from the hit test, once
 * per motion event. */
static int deco_title_end(const App *a)
{
	return DECO_TITLE_X + (int)fnl_atomic_load_u32(&a->title_w) +
		   DECO_TITLE_GAP;
}

/* Neither the buttons nor the controls give way, so the title is what goes.
 * The floor holds both, not the title: the title is the caller's string, and
 * one naming the platform and the device is half a monitor wide. */
bool deco_title_fits(const App *a, int w)
{
	int  min_x, max_x, full_x, close_x, by;
	bool btn_vis;
	deco_button_rects(a, w, &min_x, &max_x, &full_x, &close_x, &by,
					  &btn_vis);
	if (!btn_vis)
		return true;

	const int run_x = ctl_run_x(a, w);
	return (run_x >= 0 ? run_x : full_x) > deco_title_end(a);
}

void deco_min_window(const App *a, bool fullscreen, int *w, int *h)
{
	if (!deco_client(fullscreen)) {
		*w = 1;
		*h = 1;
		return;
	}
	if (a->deco_custom) {
		*w = a->deco_min_w > 1 ? a->deco_min_w : 1;
		*h = a->deco_h      > 1 ? a->deco_h    : 1;
		return;
	}
	/* One point past where ctl_run_x() drops the run: the strip, the gap
	 * before it, the run and the left margin. Where the window is the output
	 * the strip is two buttons short of DECO_BTN_STRIP_W, which only adds
	 * room. The run is ctl_run_w()'s, without the output box where that
	 * gives way. */
	*w = DECO_MIN_W + DECO_BTN_GROUP_GAP + ctl_run_w(a);
	*h = DECO_MIN_H;
	/* The size list is filtered against this floor and offers nothing below
	 * CONTENT_MIN_W, so a floor above it would empty the list. Not where
	 * the window is the output: its list is the output's modes, unfiltered,
	 * and its run, with the output and GPU boxes, is past 640 (652). The
	 * run's width is the baked font's advance, so this cannot be a static
	 * assert. */
	FNL_ASSERT(BACKEND(a)->is_output || *w <= CONTENT_MIN_W);
}

/* ---- the caller's run of the titlebar --------------------------------------
 *
 * Laid out right-to-left from just left of the library's controls, granted
 * only while it clears the window title.
 *
 * The give-way rule is public contract: the buttons and the library's own
 * controls never yield, the title yields to both, and the caller's run
 * yields to all three. DESIGN *Give-way rules*.
 *
 * A pure function of the window width and the width asked for. The render
 * thread calls it to draw and the input thread to hit-test, and they agree
 * by computing it twice from the same inputs: sharing the answer would need
 * a lock and would still be a frame stale. */
void deco_reserved_rect(const App *a, int w, int want_w, int *x, int *y,
						int *rw, int *rh, bool *visible)
{
	int  min_x, max_x, full_x, close_x, by;
	bool btn_vis;
	deco_button_rects(a, w, &min_x, &max_x, &full_x, &close_x, &by,
					  &btn_vis);

	if (want_w < 0)
		want_w = 0;

	*rw = want_w;
	*rh = DECO_ITEM_H;
	*y  = by;
	/* Left of the library's own controls, which are themselves left of the
	 * buttons. */
	*x  = full_x - DECO_BTN_GROUP_GAP - ctl_run_w_at(a, w)
		- DECO_BTN_GROUP_GAP - want_w;

	*visible = btn_vis && want_w > 0 && *x > deco_title_end(a);
}

/* ---- whether the caller claimed a point ------------------------------------
 *
 * What the last frame's on_overlay claimed (fnl_claim(), fnl_titlebar_claim()).
 * The whole scan is under the lock: the list is the render thread's, and the
 * count and the rectangles have to come from the same frame. */
static bool claim_at(App *a, int x, int y)
{
	bool hit = false;
	fnl_mutex_lock(&a->lock);
	for (int i = 0; i < a->pub_claim_count; i++) {
		const FnlRect *r = &a->pub_claims[i];
		if (x >= r->x && x < r->x + r->w &&
			y >= r->y && y < r->y + r->h) {
			hit = true;
			break;
		}
	}
	fnl_mutex_unlock(&a->lock);
	return hit;
}

/* ---- what a point in the titlebar lands on ---------------------------------
 *
 * For a point already known to be inside the window and inside the band;
 * deco_hit() below decides both. Buttons first, so the window stays closable
 * whatever else is laid out. For a custom bar the rectangles it claimed are
 * ZONE_CLIENT_ITEM and everything else in the band is ZONE_TITLEBAR. Over
 * the library's bar claims are not read: its buttons, controls and drag
 * strip are not the caller's to take. */
static Zone deco_zone_at(App *a, int w, int x, int y)
{
	/* A titlebar the caller drew has only what it claimed last frame; the
	 * rest of the band is a drag strip. */
	if (a->deco_custom)
		return claim_at(a, x, y) ? ZONE_CLIENT_ITEM : ZONE_TITLEBAR;

	int  min_x, max_x, full_x, close_x, by;
	bool vis;
	deco_button_rects(a, w, &min_x, &max_x, &full_x, &close_x, &by, &vis);

	if (vis && y >= by && y < by + DECO_BTN_H) {
		/* Ordered by Zone, not by screen position -- deco_button_rects()
		 * decides the latter, and draw_decorations() indexes this same way. */
		const int bx[] = { min_x, max_x, full_x, close_x };
		for (int i = 0; i < 4; i++)
			if (bx[i] >= 0 && x >= bx[i] && x < bx[i] + DECO_BTN_W)
				return (Zone)(ZONE_BUTTON_MIN + i);
	}

	/* The library's own controls, then a caller's run, then the bar: the
	 * order they are laid out in. */
	if (ctl_hit(a, w, x, y) >= 0)
		return ZONE_CONTROL;

	int ix, iy, iw, ih;
	bool ivis;
	deco_reserved_rect(a, w, (int)fnl_atomic_load_u32(&a->titlebar_reserved),
					   &ix, &iy, &iw, &ih, &ivis);
	if (ivis && y >= iy && y < iy + ih && x >= ix && x < ix + iw)
		return ZONE_CLIENT_ITEM;

	return ZONE_TITLEBAR;
}

/* ---- what a point in the window lands on ----------------------------------
 *
 * Window points, origin at the window's top-left, so a point in a grab ring
 * is negative or past the size -- and floored, not truncated, or half a point
 * outside the left edge would be inside it.
 *
 * The order is the rule: an open drop-down owns every press (see ctl_modal),
 * then the edges, so a corner resizes rather than drags, then the bar, then
 * what the caller claimed below it, and only then the content.
 *
 * The edges are the ring where the backend has one, and an inner margin of
 * the same depth where it has none. The inner margin goes while maximized,
 * and nothing is an edge before the first size is known, where every test
 * would read true against a zero. */
Zone deco_hit(App *a, double px, double py, Edge *edge)
{
	*edge = EDGE_NONE;

	/* A captured pointer is in the content wherever its hidden cursor has
	 * drifted, claims included: deco_press() touches no decoration while
	 * captured, and on Win32 this answers WM_NCHITTEST too, where a bar or
	 * an edge would hand the click to the window manager's move or resize
	 * instead. */
	if (a->pointer_captured)
		return ZONE_CONTENT;

	const int  x = (int)floor(px), y = (int)floor(py);
	/* The presentable area is the content but for what the caller claimed
	 * in it, which is a control of its own: a press there is its event and
	 * takes no pointer. In fullscreen, too, where there is nothing else. */
	if (!deco_client(a->is_fullscreen))
		return claim_at(a, x, y) ? ZONE_CLIENT_ITEM : ZONE_CONTENT;
	if (ctl_modal(a))
		return ZONE_CONTROL;

	const int  w = a->cur_w, h = a->cur_h;
	const bool ring = deco_margin(a, a->is_fullscreen) > 0;
	const int  in = ring ? 0 : DECO_GRAB;

	if ((ring || !a->is_maximized) && w > 0 && h > 0) {
		const bool L = x < in, R = x >= w - in;
		const bool T = y < in, B = y >= h - in;
		if      (T && L) *edge = EDGE_TL;
		else if (T && R) *edge = EDGE_TR;
		else if (B && L) *edge = EDGE_BL;
		else if (B && R) *edge = EDGE_BR;
		else if (T)      *edge = EDGE_T;
		else if (B)      *edge = EDGE_B;
		else if (L)      *edge = EDGE_L;
		else if (R)      *edge = EDGE_R;
		if (*edge != EDGE_NONE)
			return ZONE_RESIZE;
	}

	if (y >= 0 && y < deco_bar_h(a, a->is_fullscreen))
		return deco_zone_at(a, w, x, y);
	return claim_at(a, x, y) ? ZONE_CLIENT_ITEM : ZONE_CONTENT;
}

/* Which of the library's own buttons is lit; any other zone is none. Where
 * nothing invites a frame the bar is repainted at once. */
void deco_publish_hover(App *a, Zone z)
{
	const Zone lit = z >= ZONE_BUTTON_MIN && z <= ZONE_BUTTON_CLOSE
						 ? z : ZONE_CONTENT;
	if (a->hovered_button == lit)
		return;
	fnl_mutex_lock(&a->lock);
	a->hovered_button = lit;
	fnl_cond_signal(&a->cond);             /* for a loop at rest */
	fnl_mutex_unlock(&a->lock);
	if (!BACKEND(a)->invites)
		app_mark_urgent(a);
}

/* The pointer is here: light what is under it and follow an open list's rows.
 * Answers with the zone and edge, which is what the backend picks the
 * pointer's shape from (see deco_cursor). */
Zone deco_pointer_at(App *a, double px, double py, Edge *edge)
{
	const Zone z = deco_hit(a, px, py, edge);
	ctl_motion(a, a->cur_w, (int)floor(px), (int)floor(py));
	deco_publish_hover(a, z);
	return z;
}

/* ---- a left press ----------------------------------------------------------
 *
 * Acted on through the seam, after the backend has posted its
 * FNL_EVENT_BUTTON: the event goes alongside what the library does, never
 * instead of it. `time_ms` is the window server's stamp for the press, which
 * a double click is measured between. */
void deco_press(App *a, double px, double py, uint32_t time_ms)
{
	/* A captured pointer touches no decoration: the cursor is invisible, and
	 * the press could close the window from under a reticle that is
	 * elsewhere. F9 is the way out. */
	if (a->pointer_captured)
		return;

	Edge edge;
	switch (deco_hit(a, px, py, &edge)) {
	case ZONE_BUTTON_CLOSE:
		app_close_requested(a);
		break;
	case ZONE_BUTTON_MIN:
		BACKEND(a)->minimize(a);
		break;
	case ZONE_BUTTON_MAX:
		BACKEND(a)->set_maximized(a, !a->is_maximized);
		break;
	case ZONE_BUTTON_FULL:
		BACKEND(a)->set_fullscreen(a, !a->is_fullscreen);
		break;
	case ZONE_CONTROL: {
		const int x = (int)floor(px), y = (int)floor(py);
		ctl_press(a, a->cur_w, x, y, ctl_popup_hit(a, a->cur_w, x, y));
		break;
	}
	case ZONE_CLIENT_ITEM:
		/* The caller's: it has had the event, and the library does nothing
		 * more. Starting neither the drag nor the capture below is the whole
		 * of what reserving or claiming a rectangle buys. */
		break;
	case ZONE_TITLEBAR:
		if (a->last_titlebar_click_ms &&
			time_ms - a->last_titlebar_click_ms <= DECO_DBLCLICK_MS) {
			a->last_titlebar_click_ms = 0;
			a->titlebar_armed = false;
			BACKEND(a)->set_maximized(a, !a->is_maximized);
		} else {
			/* Armed, not moved: deco_drag() starts the move if this turns
			 * out to be one. Handed straight to the window server, every
			 * click would lose the pointer to it and still stand as the
			 * first half of a double click. */
			a->last_titlebar_click_ms = time_ms;
			a->titlebar_armed = true;
			a->titlebar_press_x = px;
			a->titlebar_press_y = py;
		}
		break;
	case ZONE_RESIZE:
		/* Not the first half of a double click on the bar either. */
		a->titlebar_armed = false;
		a->last_titlebar_click_ms = 0;
		BACKEND(a)->begin_resize(a, edge);
		break;
	case ZONE_CONTENT:
		/* A click into the presentable area takes the pointer. */
		BACKEND(a)->capture_pointer(a);
		break;
	}
}

/* The left button came up, or the pointer left: the press stayed a click. */
void deco_release(App *a)
{
	a->titlebar_armed = false;
}

/* The pointer moved with a titlebar press held. Past DECO_DRAG_SLOP it is a
 * drag rather than a click, and the window server is handed the move. */
void deco_drag(App *a, double px, double py)
{
	if (!a->titlebar_armed ||
		(fabs(px - a->titlebar_press_x) <= DECO_DRAG_SLOP &&
		 fabs(py - a->titlebar_press_y) <= DECO_DRAG_SLOP))
		return;
	a->titlebar_armed = false;
	a->last_titlebar_click_ms = 0;          /* a drag, so not a first click */
	BACKEND(a)->begin_move(a);
}

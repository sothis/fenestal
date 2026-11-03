/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The X11 backend: one implementation of core/backend.h. This file
 * is the window, the pointer, the event loop and the seam; the keyboard is
 * x11_keys.c, the clipboard x11_clipboard.c, the extensions x11_ext.c.
 *
 * Core xcb only -- DESIGN *X11: xcb, not Xlib* -- so the extensions it uses
 * are hand-encoded and the ICCCM structures laid out as arrays of CARD32.
 *
 * UNVERIFIED, and marked at its site: the auto-repeat filter on a server
 * that refuses detectable auto-repeat. The no-window-manager fallbacks are
 * driven by the harness's x11-vulkan-bare round (DESIGN *X11: xcb, not
 * Xlib*).
 *
 * The harness delivers one motion event per pump turn where a mouse delivers
 * a batch, so a green round says nothing about whatever on the pointer path
 * depends on how many events one turn collected. A change to on_motion()
 * wants a hand on a mouse as well as the round.
 */

#include "core/app.h"

#include "backend/x11/x11_state.h"
#include "backend/loop_linux.h"

/* How long a _NET_WM_SYNC_REQUEST waits for the ConfigureNotify that
 * follows it before its value goes out with the size the window has: one
 * that never comes is a size the window already has. The manager sends the
 * two together, so this is the bound and not the wait. */
#define SYNC_CONFIGURE_WAIT 0.05

/* How long a sync value kept for its frame's completion waits for it
 * before going anyway (x11_flush()): two periods at 60 Hz, Vulkan's bound
 * for the same wait. */
#define SYNC_SHOWN_WAIT 0.034

/* ---- the atoms ------------------------------------------------------------
 *
 * One list: X11_ATOMS in x11_state.h. */
static const char *const ATOM_NAMES[FNL_ATOM_COUNT] = {
#define X11_ATOM_NAME(suffix, name) [FNL_ATOM_##suffix] = (name),
	X11_ATOMS(X11_ATOM_NAME)
#undef X11_ATOM_NAME
};

static void intern_atoms(struct BackendState *p)
{
	xcb_intern_atom_cookie_t cookie[FNL_ATOM_COUNT];

	/* Everything whose answer the second batch of requests is encoded from,
	 * in one round trip: the atoms, whether the extensions are there, and
	 * how long a request may be. The last is asked last because libxcb waits
	 * in it, and by then everything before it has been answered. */
	x11_ext_prefetch(p);
	for (int i = 0; i < FNL_ATOM_COUNT; i++)
		cookie[i] = xcb_intern_atom(p->conn, 0,
									(uint16_t)strlen(ATOM_NAMES[i]),
									ATOM_NAMES[i]);
	char cm[32];
	const int cm_len = snprintf(cm, sizeof cm, "_NET_WM_CM_S%d",
								p->screen_num);
	const xcb_intern_atom_cookie_t cm_cookie =
		xcb_intern_atom(p->conn, 0, (uint16_t)cm_len, cm);
	xcb_prefetch_maximum_request_length(p->conn);
	for (int i = 0; i < FNL_ATOM_COUNT; i++) {
		xcb_intern_atom_reply_t *r =
			xcb_intern_atom_reply(p->conn, cookie[i], NULL);
		/* XCB_ATOM_NONE on failure; prop_get() refuses it. */
		p->atom[i] = r ? r->atom : XCB_ATOM_NONE;
		free(r);
	}
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(p->conn, cm_cookie,
													   NULL);
	p->atom_cm = r ? r->atom : XCB_ATOM_NONE;
	free(r);
}

/* One property, or NULL; the caller frees the reply. `words` is in units of the
 * format, which is 32 bits for every property this backend reads. */
static xcb_get_property_reply_t *prop_get(struct BackendState *p,
										  xcb_window_t win, xcb_atom_t prop,
										  xcb_atom_t type, uint32_t words)
{
	if (prop == XCB_ATOM_NONE)
		return NULL;
	xcb_get_property_cookie_t c =
		xcb_get_property(p->conn, 0, win, prop, type, 0, words);
	return xcb_get_property_reply(p->conn, c, NULL);
}

/* How many 32-bit values a reply carries. A property of the wrong type comes
 * back with format 0 and no data, and every caller would index into it. */
uint32_t x11_prop_words(const xcb_get_property_reply_t *r)
{
	if (!r || r->format != 32)
		return 0;
	return (uint32_t)xcb_get_property_value_length(r) / 4;
}

/* ---- scale ----------------------------------------------------------------
 *
 * Xft.dpi on the root's RESOURCE_MANAGER, where 96 is 100%, and 100% where
 * there is none. Not derived from the monitor's physical size: a DPI computed
 * from millimetres is absurd on a television that reports 160x90 mm. */
static xcb_get_property_cookie_t resources_ask(struct BackendState *p)
{
	return xcb_get_property(p->conn, 0, p->root, XCB_ATOM_RESOURCE_MANAGER,
							XCB_ATOM_STRING, 0, 16 * 1024);
}

/* The scale in 120ths out of the answer to resources_ask(), or 0. */
static int scale_from_resource_manager(struct BackendState *p,
									   xcb_get_property_cookie_t cookie)
{
	/* One string of "name:\tvalue\n" lines: a substring search, not Xrm. */
	xcb_get_property_reply_t *r =
		xcb_get_property_reply(p->conn, cookie, NULL);
	if (!r)
		return 0;

	const char *db = xcb_get_property_value(r);
	const int   n  = xcb_get_property_value_length(r);
	int s120 = 0;

	for (int i = 0; i + 8 <= n; i++) {
		/* At the start of a line, and the whole name: "Xft.dpi" and not the
		 * tail of "Xcursor.Xft.dpi". */
		if ((i != 0 && db[i - 1] != '\n') || memcmp(db + i, "Xft.dpi", 7) != 0)
			continue;
		int j = i + 7;
		while (j < n && (db[j] == ':' || db[j] == ' ' || db[j] == '\t'))
			j++;
		int dpi = 0;
		while (j < n && db[j] >= '0' && db[j] <= '9' && dpi < 10000)
			dpi = dpi * 10 + (db[j++] - '0');
		if (dpi > 0)
			s120 = (dpi * 120 + 48) / 96;
		break;
	}
	free(r);

	/* Somebody else's file: a mistyped Xft.dpi of 9600 would ask for an
	 * eightyfold buffer. Refused rather than clamped, since 100% is a good
	 * answer to fall back on. */
	if (s120 && s120 != clamp_scale120(s120))
		s120 = 0;
	return s120;
}

static int x11_resolve_scale120(const App *a)
{
	if (a->scale_override120 > 0)
		return a->scale_override120;
	return a->backend_state->xft_scale120 > 0 ? a->backend_state->xft_scale120
											  : 120;
}

/* ---- publishing to the render thread ------------------------------------- */

/* Which monitor the window is on and the room it leaves. `force` republishes
 * for an unchanged monitor, which a scale change needs: the same rectangle in
 * different points. */
static void x11_publish_output(App *a, bool force)
{
	struct BackendState *p = a->backend_state;

	const int idx = x11_monitor_index_for(p, p->win_x, p->win_y, p->win_w,
										  p->win_h);
	if (idx < 0)
		return;
	if (idx == p->published_monitor && !force)
		return;
	p->published_monitor = idx;

	const X11Monitor *m = &p->monitors[idx];
	int x, y, w, h;
	x11_monitor_room(p, idx, true, &x, &y, &w, &h);
	app_publish_refresh(a, m->refresh_mhz);

	const int s120 = want_s120(a);
	app_publish_output(a, m->name, px_to_pt(w, s120), px_to_pt(h, s120));

	/* The titlebar's output box follows the window from here: a re-read
	 * list comes through forced, and a move onto another monitor through
	 * the index. */
	char names[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	const int n = p->monitor_count < OUTPUTS_MAX ? p->monitor_count
												 : OUTPUTS_MAX;
	for (int i = 0; i < n; i++)
		snprintf(names[i], sizeof names[i], "%s", p->monitors[i].name);
	app_publish_outputs(a, names, n, idx);
}

/* The window's size and state, from this backend's own record rather than a
 * GetGeometry: ConfigureNotify describes what the window became, so the last of
 * a burst is the size it has. */
static void x11_publish_size(App *a)
{
	struct BackendState *p = a->backend_state;

	if (p->win_w <= 0 || p->win_h <= 0)
		return;

	const int s120 = x11_resolve_scale120(a);
	const int w_pt = px_to_pt(p->win_w, s120);
	const int h_pt = px_to_pt(p->win_h, s120);

	/* A sync request's value rides as the configure's serial: the loop then
	 * draws this size even at rest, and acks it (x11_ack_configure()). On
	 * every publish until the counter has been set to it, not only the
	 * first: the ConfigureNotify after a request can arrive a turn later,
	 * and a publish without a serial clears the pending one, which would
	 * leave the manager waiting out its own timeout on every step of a
	 * drag. */
	if (p->sync_asked &&
		p->sync_lo == fnl_atomic_load_u32(&p->sync_set_lo))
		p->sync_asked = false;
	/* Not before the ConfigureNotify the request precedes: published with
	 * the size the window still has, a frame at rest would answer it, and
	 * the manager would show the new size over that frame. Until then the
	 * size goes out alone, and the publish is owed again, which the pump
	 * comes back for by the deadline (x11_pump()). */
	const bool owed = p->sync_asked && !p->sync_configured &&
					  now_seconds() < p->sync_deadline;
	if (p->sync_asked && !owed)
		app_publish_size_serial(a, w_pt, h_pt, s120, p->sync_lo);
	else
		app_publish_size(a, w_pt, h_pt, s120);
	if (owed)
		p->publish_pending = true;

	/* A capture re-centres on the middle of the window, and the middle has
	 * just moved. The grab itself needs nothing: confine_to names a window
	 * rather than a rectangle, so it follows the resize on its own. */
	if (p->captured && !p->grab_suspended) {
		p->warp_x = p->win_w / 2;
		p->warp_y = p->win_h / 2;
		p->warp_pending = true;
		xcb_warp_pointer(p->conn, XCB_NONE, p->window, 0, 0, 0, 0,
						 (int16_t)p->warp_x, (int16_t)p->warp_y);
		/* last_* is deliberately left alone. A resize keeps the window's origin
		 * where it was -- north-west bit gravity, see create_window_x11 -- so
		 * window-relative pointer coordinates are continuous across it and the
		 * next delta measured against the old landing is still a real one. */
	}

	/* After the scale is published: the output is reported in points. */
	x11_publish_output(a, true);
}

/* What the handlers asked for, spent once a pump turn; see
 * BackendState::publish_pending. */
static void bring_on_screen(App *a);

static void outputs_flush(App *a)
{
	struct BackendState *p = a->backend_state;
	/* After a list is read, alone or as part of the re-read below. */
	if (p->refresh_stale && !p->outputs_stale) {
		x11_refresh_read(p);
		x11_publish_output(a, true);
	}
	if (!p->outputs_stale && !p->work_area_stale)
		return;
	/* The work area on both paths: a screen that changed moved the shell's
	 * panels with it. Both asked before either is taken. */
	const bool         monitors = p->outputs_stale;
	const unsigned int seq      = monitors ? x11_monitors_ask(p) : 0;
	const X11WorkAreaAsk area   = x11_work_area_ask(p);
	if (monitors)
		x11_monitors_take(p, seq);
	x11_work_area_take(p, area);
	p->outputs_stale = p->work_area_stale = false;
	if (p->refresh_stale)
		x11_refresh_read(p);
	if (monitors && !p->ewmh)
		bring_on_screen(a);
	x11_publish_output(a, true);
}

static void publish_flush(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->publish_pending)
		return;
	p->publish_pending = false;
	x11_publish_size(a);
}

/* ---- cursors --------------------------------------------------------------
 *
 * Cut from the core cursor font, lazily, because themed cursors need libXcursor
 * or libxcb-cursor on the link line. The font is what bounds FnlCursor: it has
 * corners and single-axis double arrows, no diagonal one. */
static const uint16_t CURSOR_GLYPH[X11_CURSOR_COUNT] = {
	[X11_CURSOR_DEFAULT]     = 68,     /* XC_left_ptr            */
	[X11_CURSOR_N]           = 138,    /* XC_top_side            */
	[X11_CURSOR_S]           = 16,     /* XC_bottom_side         */
	[X11_CURSOR_W]           = 70,     /* XC_left_side           */
	[X11_CURSOR_E]           = 96,     /* XC_right_side          */
	[X11_CURSOR_NW]          = 134,    /* XC_top_left_corner     */
	[X11_CURSOR_NE]          = 136,    /* XC_top_right_corner    */
	[X11_CURSOR_SW]          = 12,     /* XC_bottom_left_corner  */
	[X11_CURSOR_SE]          = 14,     /* XC_bottom_right_corner */
	[X11_CURSOR_TEXT]        = 152,    /* XC_xterm               */
	[X11_CURSOR_POINTER]     = 60,     /* XC_hand2               */
	[X11_CURSOR_CROSSHAIR]   = 34,     /* XC_crosshair           */
	[X11_CURSOR_MOVE]        = 52,     /* XC_fleur               */
	[X11_CURSOR_EW]          = 108,    /* XC_sb_h_double_arrow   */
	[X11_CURSOR_NS]          = 116,    /* XC_sb_v_double_arrow   */
	[X11_CURSOR_NOT_ALLOWED] = 0,      /* XC_X_cursor            */
	[X11_CURSOR_WAIT]        = 150,    /* XC_watch               */
};

/* fnl_set_cursor()'s shapes, as rows of the table above. */
static const X11Cursor CALLER_CURSORS[] = {
	[FNL_CURSOR_DEFAULT]     = X11_CURSOR_DEFAULT,
	[FNL_CURSOR_TEXT]        = X11_CURSOR_TEXT,
	[FNL_CURSOR_POINTER]     = X11_CURSOR_POINTER,
	[FNL_CURSOR_CROSSHAIR]   = X11_CURSOR_CROSSHAIR,
	[FNL_CURSOR_MOVE]        = X11_CURSOR_MOVE,
	[FNL_CURSOR_EW_RESIZE]   = X11_CURSOR_EW,
	[FNL_CURSOR_NS_RESIZE]   = X11_CURSOR_NS,
	[FNL_CURSOR_NOT_ALLOWED] = X11_CURSOR_NOT_ALLOWED,
	[FNL_CURSOR_WAIT]        = X11_CURSOR_WAIT,
};
_Static_assert(sizeof CALLER_CURSORS / sizeof CALLER_CURSORS[0] ==
			   FNL_CURSOR_WAIT + 1,
			   "a FnlCursor was appended without a glyph for it here");

/* No cursor at all: a one-by-one bitmap with nothing set in it, which is the
 * only way to say it without XFixes. */
static xcb_cursor_t blank_cursor(struct BackendState *p)
{
	if (p->blank_cursor)
		return p->blank_cursor;

	xcb_pixmap_t pix = xcb_generate_id(p->conn);
	xcb_gcontext_t gc = xcb_generate_id(p->conn);
	xcb_cursor_t   cur = xcb_generate_id(p->conn);

	xcb_create_pixmap(p->conn, 1, pix, p->root, 1, 1);
	/* A new pixmap's contents are undefined, and an undefined bit is a cursor
	 * that is sometimes a dot. */
	const uint32_t zero = 0;
	xcb_create_gc(p->conn, gc, pix, XCB_GC_FOREGROUND, &zero);
	const xcb_rectangle_t r = { 0, 0, 1, 1 };
	xcb_poly_fill_rectangle(p->conn, pix, gc, 1, &r);
	xcb_free_gc(p->conn, gc);

	xcb_create_cursor(p->conn, cur, pix, pix, 0, 0, 0, 0, 0, 0, 0, 0);
	xcb_free_pixmap(p->conn, pix);

	p->blank_cursor = cur;
	return cur;
}

static const char CURSOR_FONT[] = "cursor";

static xcb_cursor_t cursor_load(struct BackendState *p, X11Cursor which)
{
	if (which < 0 || which >= X11_CURSOR_COUNT)
		return XCB_NONE;
	if (p->cursors[which])
		return p->cursors[which];

	if (!p->cursor_font) {
		p->cursor_font = xcb_generate_id(p->conn);
		xcb_open_font(p->conn, p->cursor_font,
					  (uint16_t)(sizeof CURSOR_FONT - 1), CURSOR_FONT);
	}

	xcb_cursor_t cur = xcb_generate_id(p->conn);
	/* The mask glyph is the shape's own plus one: the cursor font's layout. */
	xcb_create_glyph_cursor(p->conn, cur, p->cursor_font, p->cursor_font,
							CURSOR_GLYPH[which],
							(uint16_t)(CURSOR_GLYPH[which] + 1),
							0, 0, 0, 0xffff, 0xffff, 0xffff);
	p->cursors[which] = cur;
	return cur;
}

/* Compares first: a pointer crossing the presentable area would otherwise cost
 * a request per motion event. */
static void set_cursor(App *a, X11Cursor which)
{
	struct BackendState *p = a->backend_state;
	if (p->cursor_applied == which)
		return;
	p->cursor_applied = which;

	const uint32_t cur = which == X11_CURSOR_NONE ? blank_cursor(p)
												  : cursor_load(p, which);
	xcb_change_window_attributes(p->conn, p->window, XCB_CW_CURSOR, &cur);
}

/* ---- the client titlebar --------------------------------------------------
 *
 * The layout and the hit test are the core's. A drag or an edge press is handed
 * to the manager as _NET_WM_MOVERESIZE, which buys its own drag, snap and edge
 * resistance. */

/* _NET_WM_MOVERESIZE's direction codes, which are the EWMH order: the eight
 * edges clockwise from the top-left, then MOVE. */
enum {
	MOVERESIZE_SIZE_TOPLEFT     = 0,
	MOVERESIZE_SIZE_TOP         = 1,
	MOVERESIZE_SIZE_TOPRIGHT    = 2,
	MOVERESIZE_SIZE_RIGHT       = 3,
	MOVERESIZE_SIZE_BOTTOMRIGHT = 4,
	MOVERESIZE_SIZE_BOTTOM      = 5,
	MOVERESIZE_SIZE_BOTTOMLEFT  = 6,
	MOVERESIZE_SIZE_LEFT        = 7,
	MOVERESIZE_MOVE             = 8,
};

/* An edge as both vocabularies spell it, side by side so the cursor cannot say
 * one thing while the drag does another. */
typedef struct {
	uint8_t   moveresize;
	X11Cursor cursor;
} EdgeCodes;

static const EdgeCodes EDGE_CODES[EDGE_COUNT] = {
	{ MOVERESIZE_MOVE,             X11_CURSOR_DEFAULT },  /* 0: not an edge */
	{ MOVERESIZE_SIZE_TOP,         X11_CURSOR_N  },
	{ MOVERESIZE_SIZE_BOTTOM,      X11_CURSOR_S  },
	{ MOVERESIZE_SIZE_LEFT,        X11_CURSOR_W  },
	{ MOVERESIZE_SIZE_RIGHT,       X11_CURSOR_E  },
	{ MOVERESIZE_SIZE_TOPLEFT,     X11_CURSOR_NW },
	{ MOVERESIZE_SIZE_TOPRIGHT,    X11_CURSOR_NE },
	{ MOVERESIZE_SIZE_BOTTOMLEFT,  X11_CURSOR_SW },
	{ MOVERESIZE_SIZE_BOTTOMRIGHT, X11_CURSOR_SE },
};

/* Indexed by Edge, which is what begin_resize is handed. */
_Static_assert(sizeof EDGE_CODES / sizeof EDGE_CODES[0] == EDGE_COUNT,
			   "EDGE_CODES has drifted from Edge");

static void cursor_update(App *a, Zone z, Edge edge)
{
	const DecoCursor c = deco_cursor(a, z, edge);
	set_cursor(a, c.kind == DECO_CURSOR_HIDDEN ? X11_CURSOR_NONE
				: c.kind == DECO_CURSOR_EDGE   ? EDGE_CODES[c.edge].cursor
											   : CALLER_CURSORS[c.shape]);
}

/* ICCCM's and Motif's figures, for the structures laid out by hand below. */
#define ICCCM_INPUT_HINT    (1u << 0)
#define ICCCM_STATE_HINT    (1u << 1)
#define ICCCM_NORMAL_STATE  1u
#define ICCCM_ICONIC_STATE  3u

#define ICCCM_P_MIN_SIZE        (1u << 4)
#define ICCCM_SIZE_HINTS_WORDS  18
#define ICCCM_SIZE_HINTS_MIN_W  5
#define ICCCM_SIZE_HINTS_MIN_H  6

#define MWM_HINTS_DECORATIONS   (1u << 1)

/* ---- window state ---------------------------------------------------------
 *
 * Asked for rather than set: the manager owns the answer and reports it through
 * _NET_WM_STATE, so nothing on the managed path writes App::is_fullscreen. With
 * no EWMH manager this backend does the arithmetic itself. */

/* A ClientMessage to the root under the mask ICCCM and EWMH both name, which
 * is what delivers it to the window manager. */
static void root_send(struct BackendState *p,
					  const xcb_client_message_event_t *ev)
{
	xcb_send_event(p->conn, 0, p->root,
				   XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
				   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
				   (const char *)ev);
}

static void root_message(struct BackendState *p, xcb_atom_t type,
						 uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
						 uint32_t d4)
{
	const xcb_client_message_event_t ev = {
		.response_type = XCB_CLIENT_MESSAGE,
		.format = 32,
		.sequence = 0,
		.window = p->window,
		.type = type,
		.data = { .data32 = { d0, d1, d2, d3, d4 } },
	};
	root_send(p, &ev);
}

/* One _NET_WM_STATE change, as the client message EWMH specifies: action,
 * up to two atoms, and a source indication of 1 for a normal application. */
static void wm_state_set(struct BackendState *p, xcb_atom_t a1, xcb_atom_t a2,
						 bool on)
{
	root_message(p, p->atom[FNL_ATOM_NET_WM_STATE], on ? 1u : 0u, a1, a2, 1u,
				 0u);
}

/* ---- one size at a time, with no manager ----------------------------------
 *
 * With no manager this backend sizes its own window -- a step of a drag, a
 * caller's size, fullscreen -- and nothing waited for a frame at the last
 * size before the next: NVIDIA's own driver calls a swapchain out of date
 * as soon as the window has moved on, a rebuild there is 11 ms, and at
 * sixty sizes a second most never drew. tools/probe/wsidrag.c at 60: sizes
 * that reached the screen with a frame, 24 and 35 a second on aspire's and
 * schenker's NVIDIA GPUs; holding each size until a frame at the last had
 * been presented, 56 and 53, and Intel's 59 to 57 against 59. Win32's rule
 * (size_await()), and for the reason it has. A manager holds the window to
 * its sync request instead, so this is the unmanaged window's alone.
 *
 * Held for SIZE_HOLD_S at most, for a frame that is not owed -- a window
 * unmapped, or a device lost -- and the latest asked for applied then: the
 * ones between are never the window's. A move at the size the window has is
 * not held, unless a size is, which it then rides with. */
#define SIZE_HOLD_S 0.25

/* Whether the frame last presented is at the size last given. */
static bool size_shown(const struct BackendState *p)
{
	return (int)fnl_atomic_load_u32(&p->shown_w) == p->given_w &&
		   (int)fnl_atomic_load_u32(&p->shown_h) == p->given_h;
}

static void size_apply(struct BackendState *p, uint16_t mask, int x, int y,
					   int w, int h)
{
	uint32_t v[4];
	int n = 0;
	if (mask & XCB_CONFIG_WINDOW_X)      v[n++] = (uint32_t)x;
	if (mask & XCB_CONFIG_WINDOW_Y)      v[n++] = (uint32_t)y;
	if (mask & XCB_CONFIG_WINDOW_WIDTH)  v[n++] = (uint32_t)w;
	if (mask & XCB_CONFIG_WINDOW_HEIGHT) v[n++] = (uint32_t)h;
	xcb_configure_window(p->conn, p->window, mask, v);
	if (mask & XCB_CONFIG_WINDOW_WIDTH)
		p->given_w = w;
	if (mask & XCB_CONFIG_WINDOW_HEIGHT)
		p->given_h = h;
	if (mask & (XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT))
		p->given_at = now_seconds();
	p->held = false;
}

/* Give the window this geometry now, or hold it. `mask` is ConfigureWindow's,
 * of X, Y, WIDTH and HEIGHT. */
static void size_give(struct BackendState *p, uint16_t mask, int x, int y,
					  int w, int h)
{
	const uint16_t size = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	if (!(mask & XCB_CONFIG_WINDOW_WIDTH))
		w = p->held ? p->held_w : p->given_w;
	if (!(mask & XCB_CONFIG_WINDOW_HEIGHT))
		h = p->held ? p->held_h : p->given_h;
	const bool resizes = (mask & size) &&
						 (w != p->given_w || h != p->given_h);
	const bool wait = !p->ewmh && p->given_w > 0 &&
					  (p->held || (resizes && !size_shown(p))) &&
					  now_seconds() - p->given_at < SIZE_HOLD_S;
	if (!wait) {
		size_apply(p, mask, x, y, w, h);
		return;
	}
	p->held_mask = (uint16_t)((p->held ? p->held_mask : 0) | mask | size);
	if (mask & XCB_CONFIG_WINDOW_X) p->held_x = x;
	if (mask & XCB_CONFIG_WINDOW_Y) p->held_y = y;
	p->held_w = w;
	p->held_h = h;
	p->held = true;
	fnl_atomic_store_bool(&p->held_wake, true);
}

/* From each pump turn: the size held goes out once its turn has come. */
static void size_release(struct BackendState *p)
{
	if (p->held && (size_shown(p) ||
					now_seconds() - p->given_at >= SIZE_HOLD_S))
		size_apply(p, p->held_mask, p->held_x, p->held_y, p->held_w,
				   p->held_h);
}

/* How long the pump may wait with a size held, or -1 with none. */
static int size_wait_ms(const struct BackendState *p)
{
	if (!p->held)
		return -1;
	const double left = p->given_at + SIZE_HOLD_S - now_seconds();
	return left > 0.0 ? (int)(left * 1e3) + 1 : 0;
}

static void window_move_resize(struct BackendState *p, int x, int y,
							   int w, int h)
{
	size_give(p, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
				 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
			  x, y, w, h);
}

static void save_restore_rect(struct BackendState *p)
{
	if (p->saved_valid)
		return;
	p->saved_x = p->win_x;
	p->saved_y = p->win_y;
	p->saved_w = p->win_w;
	p->saved_h = p->win_h;
	p->saved_valid = true;
}

/* Fullscreen or maximize with no manager to ask: a size and a position.
 * One saved rectangle serves both, and it is the window with
 * neither on: so the geometry after a change is whichever state is still on,
 * fullscreen over maximized, and the rectangle is put back only when neither
 * is. Restoring it when one of two goes off is a maximized window that
 * leaves fullscreen at its restored size, still flagged maximized. */
static void unmanaged_state_set(App *a, bool *flag, bool on)
{
	struct BackendState *p = a->backend_state;

	*flag = on;
	if (a->is_fullscreen || a->is_maximized) {
		save_restore_rect(p);
		int x, y, w, h;
		const int idx = x11_monitor_index_for(p, p->win_x, p->win_y,
											  p->win_w, p->win_h);
		/* Maximized takes the part of the monitor the shell leaves free. */
		x11_monitor_room(p, idx, !a->is_fullscreen, &x, &y, &w, &h);
		window_move_resize(p, x, y, w, h);
	} else if (p->saved_valid) {
		window_move_resize(p, p->saved_x, p->saved_y, p->saved_w,
						   p->saved_h);
		p->saved_valid = false;
	}
	p->publish_pending = true;
	app_mark_urgent(a);
}

/* With no manager, a window left on an output that has gone -- switched off,
 * which is what a desktop does with one pulled out -- is on no screen at
 * all, and nothing else will move it. So this does what a manager would:
 * onto the primary monitor (or the first), centred in its room, no larger
 * than it. Fullscreen and maximized are the monitor's own rectangle again,
 * on the new one; the rectangle saved for leaving them is brought along,
 * or leaving would put the window back where nobody can see it. */
static bool on_some_monitor(const struct BackendState *p, int x, int y,
							int w, int h)
{
	for (int i = 0; i < p->monitor_count; i++) {
		const X11Monitor *m = &p->monitors[i];
		if (x < m->x + m->w && m->x < x + w && y < m->y + m->h &&
			m->y < y + h)
			return true;
	}
	return false;
}

static void fit_into(int rx, int ry, int rw, int rh, int *x, int *y, int *w,
					 int *h)
{
	if (*w > rw) *w = rw;
	if (*h > rh) *h = rh;
	*x = rx + (rw - *w) / 2;
	*y = ry + (rh - *h) / 2;
}

static void bring_on_screen(App *a)
{
	struct BackendState *p = a->backend_state;
	if (p->monitor_count <= 0 || p->win_w <= 0 || p->win_h <= 0 ||
		on_some_monitor(p, p->win_x, p->win_y, p->win_w, p->win_h))
		return;

	int idx = 0;
	for (int i = 0; i < p->monitor_count; i++)
		if (p->monitors[i].primary) {
			idx = i;
			break;
		}
	int rx, ry, rw, rh;
	x11_monitor_room(p, idx, true, &rx, &ry, &rw, &rh);
	fnl_log(FNL_LOG_INFO, "the window was on no monitor; moved onto %s",
			p->monitors[idx].name);

	if (p->saved_valid)
		fit_into(rx, ry, rw, rh, &p->saved_x, &p->saved_y, &p->saved_w,
				 &p->saved_h);
	int x = p->win_x, y = p->win_y, w = p->win_w, h = p->win_h;
	if (a->is_fullscreen || a->is_maximized)
		x11_monitor_room(p, idx, !a->is_fullscreen, &x, &y, &w, &h);
	else
		fit_into(rx, ry, rw, rh, &x, &y, &w, &h);
	window_move_resize(p, x, y, w, h);
	/* Taken as done: with nobody to refuse it, the request is the outcome,
	 * and a second re-read of a RandR burst arriving before the
	 * ConfigureNotify must not find the window stranded again. */
	p->win_x = x;
	p->win_y = y;
	p->win_w = w;
	p->win_h = h;
	p->publish_pending = true;
	app_mark_urgent(a);
}

/* With no manager, the opening size fitted to the room there is. Nothing
 * else declines a size: a window asked for larger than its monitor opens
 * larger than it, the right-hand border and the bar's controls where no
 * pointer reaches -- the default 1280x720 on a 1024x768 output. So the
 * answer a manager would have given is given here, as win32_open() gives
 * it: the room's size at most, the floor's at least, and moved back inside
 * where it now overhangs. Before the map, so it is one window at one size;
 * only the opening size, since one asked for afterwards is the caller's to
 * have filtered. */
static void fit_opening(App *a, int s120)
{
	struct BackendState *p = a->backend_state;
	const int idx = x11_monitor_index_for(p, p->win_x, p->win_y, p->win_w,
										  p->win_h);
	int rx, ry, rw, rh;
	x11_monitor_room(p, idx, true, &rx, &ry, &rw, &rh);
	int min_w, min_h;
	deco_min_window(a, false, &min_w, &min_h);
	min_w = scale_pt(min_w, s120);
	min_h = scale_pt(min_h, s120);

	int w = p->win_w < rw ? p->win_w : rw;
	int h = p->win_h < rh ? p->win_h : rh;
	if (w < min_w) w = min_w;
	if (h < min_h) h = min_h;
	int x = p->win_x, y = p->win_y;
	if (x + w > rx + rw) x = rx + rw - w;
	if (y + h > ry + rh) y = ry + rh - h;
	if (x < rx) x = rx;
	if (y < ry) y = ry;
	if (x == p->win_x && y == p->win_y && w == p->win_w && h == p->win_h)
		return;
	window_move_resize(p, x, y, w, h);
	p->win_x = x;
	p->win_y = y;
	p->win_w = w;
	p->win_h = h;
}

static void x11_set_fullscreen(App *a, bool on)
{
	struct BackendState *p = a->backend_state;
	if (on == a->is_fullscreen)
		return;

	if (p->ewmh) {
		wm_state_set(p, p->atom[FNL_ATOM_NET_WM_STATE_FULLSCREEN],
					 XCB_ATOM_NONE, on);
		/* The state is adopted when the manager's property change arrives;
		 * urgent, so the frame that answers waits for nothing else. */
		app_mark_urgent(a);
		return;
	}
	unmanaged_state_set(a, &a->is_fullscreen, on);
}

static void x11_set_maximized(App *a, bool on)
{
	struct BackendState *p = a->backend_state;

	if (p->ewmh) {
		wm_state_set(p, p->atom[FNL_ATOM_NET_WM_STATE_MAXIMIZED_VERT],
					 p->atom[FNL_ATOM_NET_WM_STATE_MAXIMIZED_HORZ], on);
		app_mark_urgent(a);
		return;
	}
	unmanaged_state_set(a, &a->is_maximized, on);
}

/* To the monitor of this name, keeping the window's size and state, where
 * app_place_on_output() puts it in that monitor's room.
 *
 * With no manager the geometry is this backend's to give, as
 * bring_on_screen() gives it: fullscreen and maximized are the new
 * monitor's rectangle, the one saved for leaving them comes along, and
 * the request is taken as done. With one, the window is placed by asking:
 * _NET_MOVERESIZE_WINDOW as a pager (source 2), whose request a manager
 * honours where it may place an application's own ConfigureRequest itself,
 * and at StaticGravity, so the position is the window's and not its
 * frame's, as win_x is. A fullscreen or maximized window is the manager's
 * to place: it leaves the state, is moved, and takes it again there --
 * messages the manager handles in the order sent. The change of monitor
 * comes back with the ConfigureNotify, which publishes it. */
static void x11_set_output(App *a, const char *name)
{
	struct BackendState *p = a->backend_state;
	int to = -1;
	for (int i = 0; i < p->monitor_count && to < 0; i++)
		if (strcmp(p->monitors[i].name, name) == 0)
			to = i;
	const int from = x11_monitor_index_for(p, p->win_x, p->win_y, p->win_w,
										   p->win_h);
	if (to < 0 || to == from || p->win_w <= 0 || p->win_h <= 0)
		return;
	FnlRect fr, tr;
	x11_monitor_room(p, from, true, &fr.x, &fr.y, &fr.w, &fr.h);
	x11_monitor_room(p, to, true, &tr.x, &tr.y, &tr.w, &tr.h);

	if (!p->ewmh) {
		if (p->saved_valid)
			app_place_on_output(p->saved_x, p->saved_y, p->saved_w,
								p->saved_h, &fr, &tr, &p->saved_x,
								&p->saved_y);
		int x, y, w = p->win_w, h = p->win_h;
		if (a->is_fullscreen || a->is_maximized)
			x11_monitor_room(p, to, !a->is_fullscreen, &x, &y, &w, &h);
		else
			app_place_on_output(p->win_x, p->win_y, w, h, &fr, &tr, &x, &y);
		window_move_resize(p, x, y, w, h);
		p->win_x = x;
		p->win_y = y;
		p->win_w = w;
		p->win_h = h;
		p->publish_pending = true;
		app_mark_urgent(a);
		return;
	}

	const xcb_atom_t fs = p->atom[FNL_ATOM_NET_WM_STATE_FULLSCREEN];
	const xcb_atom_t mv = p->atom[FNL_ATOM_NET_WM_STATE_MAXIMIZED_VERT];
	const xcb_atom_t mh = p->atom[FNL_ATOM_NET_WM_STATE_MAXIMIZED_HORZ];
	const bool was_fs = a->is_fullscreen, was_max = a->is_maximized;
	if (was_fs)
		wm_state_set(p, fs, XCB_ATOM_NONE, false);
	if (was_max)
		wm_state_set(p, mv, mh, false);

	/* From the rectangle the window is put back to: fullscreen or
	 * maximized, win_* is the monitor's, which would land the restored
	 * window in the destination's corner. That rectangle is then the
	 * destination's, for a second move before the manager says. */
	const bool norm = (was_fs || was_max) && p->norm_valid;
	int x, y;
	app_place_on_output(norm ? p->norm_x : p->win_x,
						norm ? p->norm_y : p->win_y,
						norm ? p->norm_w : p->win_w,
						norm ? p->norm_h : p->win_h, &fr, &tr, &x, &y);
	if (norm) {
		p->norm_x = x;
		p->norm_y = y;
	}
	/* StaticGravity, x and y given, from a pager. */
	const uint32_t how = (uint32_t)XCB_GRAVITY_STATIC | (1u << 8) |
						 (1u << 9) | (2u << 12);
	root_message(p, p->atom[FNL_ATOM_NET_MOVERESIZE_WINDOW], how,
				 (uint32_t)x, (uint32_t)y, 0u, 0u);

	if (was_max)
		wm_state_set(p, mv, mh, true);
	if (was_fs)
		wm_state_set(p, fs, XCB_ATOM_NONE, true);
	app_mark_urgent(a);
}

/* ICCCM's WM_CHANGE_STATE: there is no EWMH message for it, and unmapping the
 * window directly would leave nothing to bring it back. Without a manager this
 * does nothing, for the same reason. */
static void x11_minimize(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->ewmh)
		return;
	root_message(p, p->atom[FNL_ATOM_WM_CHANGE_STATE], ICCCM_ICONIC_STATE,
				 0u, 0u, 0u, 0u);
}

/* Hand an interactive move or resize to the window manager. The ungrab is
 * EWMH's instruction: the press established an automatic grab on this window,
 * and a manager that cannot take the pointer cannot run the drag. */
static void wm_moveresize(App *a, int root_x, int root_y, uint32_t direction,
						  uint8_t button)
{
	struct BackendState *p = a->backend_state;

	xcb_ungrab_pointer(p->conn, XCB_CURRENT_TIME);
	/* The last word is the source indication: a normal application. */
	root_message(p, p->atom[FNL_ATOM_NET_WM_MOVERESIZE], (uint32_t)root_x,
				 (uint32_t)root_y, direction, button, 1u);

	/* The manager owns the pointer now, so no leave event is coming for the
	 * button that was lit under it. */
	deco_release(a);
	deco_publish_hover(a, ZONE_CONTENT);
}

/* An 8-bit sRGB colour as a pixel of the root visual, or black where the
 * visual has no masks to place it by. */
static uint32_t rgb_pixel(const struct BackendState *p, const uint8_t rgb[3])
{
	xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(p->screen);
	for (; di.rem; xcb_depth_next(&di)) {
		xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data);
		for (; vi.rem; xcb_visualtype_next(&vi)) {
			const xcb_visualtype_t *v = vi.data;
			if (v->visual_id != p->screen->root_visual)
				continue;
			if (v->_class != XCB_VISUAL_CLASS_TRUE_COLOR &&
				v->_class != XCB_VISUAL_CLASS_DIRECT_COLOR)
				return p->screen->black_pixel;
			const uint32_t mask[3] = { v->red_mask, v->green_mask,
									   v->blue_mask };
			uint32_t px = 0;
			for (int i = 0; i < 3; i++) {
				if (!mask[i])
					continue;
				int shift = 0;
				while (!((mask[i] >> shift) & 1u))
					shift++;
				const uint32_t top = mask[i] >> shift;
				px |= ((rgb[i] * top + 127u) / 255u) << shift;
			}
			return px;
		}
	}
	return p->screen->black_pixel;
}

/* The window's background: what the server paints into a window the moment
 * it grows, and what shows there until a frame of the new size arrives --
 * one lavapipe frame, tens of milliseconds, on a server nobody composites.
 * Black alone was a black slab across the titlebar on every widening step.
 * A tile one pixel wide, the library's bar colour above and black below,
 * makes that slab the bar carried on and the letterbox black the frame
 * itself would leave. Remade when the bar, the focus or a taller window
 * than the tile asks; a bar the caller draws, or none, is black throughout. */
static void background_update(App *a)
{
	struct BackendState *p = a->backend_state;
	const int bar = deco_client(a->is_fullscreen) && !a->deco_custom
				  ? scale_pt(deco_bar_h(a, a->is_fullscreen), want_s120(a))
				  : 0;
	const bool active = p->focus_in;
	if (p->bg_h >= p->win_h && bar == p->bg_bar &&
		(bar == 0 || active == p->bg_active))
		return;

	int h = p->screen->height_in_pixels;
	if (h < p->win_h)
		h = p->win_h;
	if (h < 1 || h > UINT16_MAX)
		return;

	const xcb_pixmap_t pm = xcb_generate_id(p->conn);
	xcb_create_pixmap(p->conn, p->screen->root_depth, pm, p->window, 1,
					  (uint16_t)h);
	const xcb_gcontext_t gc = xcb_generate_id(p->conn);
	uint32_t fg = p->screen->black_pixel;
	xcb_create_gc(p->conn, gc, pm, XCB_GC_FOREGROUND, &fg);
	const xcb_rectangle_t all = { 0, 0, 1, (uint16_t)h };
	xcb_poly_fill_rectangle(p->conn, pm, gc, 1, &all);
	if (bar > 0) {
		uint8_t rgb[3];
		app_bar_srgb8(active, rgb);
		fg = rgb_pixel(p, rgb);
		xcb_change_gc(p->conn, gc, XCB_GC_FOREGROUND, &fg);
		const xcb_rectangle_t top = { 0, 0, 1,
									  (uint16_t)(bar < h ? bar : h) };
		xcb_poly_fill_rectangle(p->conn, pm, gc, 1, &top);
	}
	xcb_free_gc(p->conn, gc);
	/* The window keeps its own reference: freed here, the tile lives as
	 * long as it is the background. */
	xcb_change_window_attributes(p->conn, p->window, XCB_CW_BACK_PIXMAP, &pm);
	xcb_free_pixmap(p->conn, pm);

	p->bg_bar    = bar;
	p->bg_h      = h;
	p->bg_active = active;
}

/* The width a resize run here moves in. With nobody compositing, a frame
 * drawn at the old width can land in a window already widened, and Mesa's
 * software present without MIT-SHM sends each row at the image's row pitch,
 * which lavapipe pads to 64 bytes: the padding is uninitialised memory, and
 * it showed as noise down the edge on every widening step. 16 pixels of
 * four bytes is a row with no padding, so only the server's black shows
 * until the next frame. Rounded on every device, since the backend does not
 * know which one draws; on a GPU it costs only a coarser drag. A size the
 * caller asks for is never rounded. */
#define MANUAL_WIDTH_STEP 16

static int manual_width(int w)
{
	return (w + MANUAL_WIDTH_STEP / 2) / MANUAL_WIDTH_STEP * MANUAL_WIDTH_STEP;
}

/* The same for a desktop with no EWMH manager: this backend moves its own
 * window on every motion event. */
static void manual_moveresize(App *a, int root_x, int root_y, uint32_t edge)
{
	struct BackendState *p = a->backend_state;
	int min_w_pt, min_h_pt;
	deco_min_window(a, a->is_fullscreen, &min_w_pt, &min_h_pt);
	const int s120  = want_s120(a);
	const int min_w = scale_pt(min_w_pt, s120);
	const int min_h = scale_pt(min_h_pt, s120);

	int x = p->win_x, y = p->win_y, w = p->win_w, h = p->win_h;

	if (edge == EDGE_NONE) {                       /* a move */
		x = root_x - p->drag_off_x;
		y = root_y - p->drag_off_y;
	} else {
		const bool T = edge == EDGE_T || edge == EDGE_TL || edge == EDGE_TR;
		const bool B = edge == EDGE_B || edge == EDGE_BL || edge == EDGE_BR;
		const bool L = edge == EDGE_L || edge == EDGE_TL || edge == EDGE_BL;
		const bool R = edge == EDGE_R || edge == EDGE_TR || edge == EDGE_BR;
		const int right = p->win_x + p->win_w;
		if (L) w = manual_width(right - root_x);
		if (R) w = manual_width(root_x - p->win_x);
		if (T) { h = p->win_y + p->win_h - root_y; y = root_y; }
		if (B) { h = root_y - p->win_y; }
		/* The floor, enforced here because there is no manager reading
		 * WM_NORMAL_HINTS, and kept to the step. Anchored: shrinking past it
		 * from a top or left edge must not walk the far edge away. */
		if (w < min_w)
			w = (min_w + MANUAL_WIDTH_STEP - 1) / MANUAL_WIDTH_STEP
			  * MANUAL_WIDTH_STEP;
		if (L) x = right - w;
		if (h < min_h) { if (T) y -= min_h - h; h = min_h; }
	}
	window_move_resize(p, x, y, w, h);
}

/* ---- pointer capture ------------------------------------------------------
 *
 * A grab confined to the window; DESIGN *Pointer capture*. The warp keeps the
 * confined pointer off the edge and hands the cursor back where the reticle
 * was. It is not what the delta is measured against: see on_motion(). */
static bool grab_confined(struct BackendState *p)
{
	xcb_grab_pointer_reply_t *r = xcb_grab_pointer_reply(
		p->conn,
		xcb_grab_pointer(p->conn, 1 /* owner_events: we still get them */,
						 p->window,
						 XCB_EVENT_MASK_BUTTON_PRESS |
						 XCB_EVENT_MASK_BUTTON_RELEASE |
						 XCB_EVENT_MASK_POINTER_MOTION,
						 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
						 p->window, blank_cursor(p), XCB_CURRENT_TIME),
		NULL);
	const bool ok = r && r->status == XCB_GRAB_STATUS_SUCCESS;
	free(r);
	return ok;
}

static void x11_capture(App *a)
{
	struct BackendState *p = a->backend_state;
	if (p->captured)
		return;

	const int s120 = want_s120(a);

	if (!grab_confined(p)) {
		/* Somebody else holds the pointer -- a menu, or a manager running a
		 * drag -- and the click stays an ordinary one. */
		fnl_log(FNL_LOG_WARN, "the pointer could not be grabbed; it stays "
							  "uncaptured");
		return;
	}

	p->captured = true;
	p->grab_suspended = false;
	a->pointer_captured = true;
	p->warp_x = p->win_w / 2;
	p->warp_y = p->win_h / 2;
	/* Where the pointer is *now*, not where the warp below is about to put it:
	 * the first deltas are measured against this, and any motion event that
	 * beat the warp to us carries a position in the space the click was in. The
	 * warp's own event moves last_* to the middle when it arrives. */
	p->last_x = (int)pt_to_pxf(a->ptr_x, s120);
	p->last_y = (int)pt_to_pxf(a->ptr_y, s120);
	p->warp_pending = true;
	xcb_warp_pointer(p->conn, XCB_NONE, p->window, 0, 0, 0, 0,
					 (int16_t)p->warp_x, (int16_t)p->warp_y);
	set_cursor(a, X11_CURSOR_NONE);

	app_reticle_start(a);
	app_reticle_publish(a);
}

/* The grab goes with the focus and the capture does not, as Win32's clip and
 * Cocoa's detachment do: a confined pointer outlasting the focus is one the
 * user cannot take to the window they switched to. The reticle stays where
 * it was, and the window's cursor becomes an arrow, since the desktop's
 * pointer may now cross it unfocused. */
static void capture_suspend(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->captured || p->grab_suspended)
		return;
	xcb_ungrab_pointer(p->conn, XCB_CURRENT_TIME);
	p->grab_suspended = true;
	p->warp_pending = false;
	set_cursor(a, X11_CURSOR_DEFAULT);
}

/* The grab again, with the focus back. Refused while another client holds
 * the pointer -- a manager's switcher still running at our focus-in -- and
 * then tried again at its keyboard ungrab and at the next motion over the
 * window (on_focus(), on_motion()), so a refusal costs nothing but a
 * moment. */
static void capture_resume(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->captured || !p->grab_suspended || !p->focus_in)
		return;
	if (!grab_confined(p))
		return;
	p->grab_suspended = false;

	/* Deltas from where the pointer is once grabbed: confine_to has already
	 * pulled one that was outside into the window, and that pull is no
	 * movement of the user's. A round trip, once per focus-in. */
	p->warp_x = p->win_w / 2;
	p->warp_y = p->win_h / 2;
	p->last_x = p->warp_x;
	p->last_y = p->warp_y;
	xcb_query_pointer_reply_t *q = xcb_query_pointer_reply(
		p->conn, xcb_query_pointer(p->conn, p->window), NULL);
	if (q && q->same_screen) {
		p->last_x = q->win_x;
		p->last_y = q->win_y;
	}
	free(q);
	p->warp_pending = true;
	xcb_warp_pointer(p->conn, XCB_NONE, p->window, 0, 0, 0, 0,
					 (int16_t)p->warp_x, (int16_t)p->warp_y);
	set_cursor(a, X11_CURSOR_NONE);
}

static void x11_release(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->captured)
		return;

	/* The desktop cursor comes back where the reticle is -- unless the grab
	 * went with the focus, when the pointer is the user's, somewhere else. */
	int cx, cy;
	const int s120 = want_s120(a);
	app_content_px_rect(a, &cx, &cy, NULL, NULL);
	const int x = cx + (int)(a->cur_px + 0.5);
	const int y = cy + (int)(a->cur_py + 0.5);
	if (!p->grab_suspended) {
		xcb_warp_pointer(p->conn, XCB_NONE, p->window, 0, 0, 0, 0,
						 (int16_t)x, (int16_t)y);
		xcb_ungrab_pointer(p->conn, XCB_CURRENT_TIME);
	}
	p->grab_suspended = false;
	p->captured = false;
	a->pointer_captured = false;
	/* The shape follows from the zone under the reticle, not an assumed arrow:
	 * that is the presentable area, where a caller's shape applies. */
	a->ptr_x = px_to_pt(x, s120);
	a->ptr_y = px_to_pt(y, s120);
	Edge edge;
	const Zone z = deco_hit(a, a->ptr_x, a->ptr_y, &edge);
	cursor_update(a, z, edge);

	app_reticle_publish(a);
}

/* The dead zone as a divisor on each axis: the middle half, leaving a quarter
 * of room for the events still in flight. Smaller warps more often; larger
 * risks the confined pointer stopping at the edge mid-flick. */
#define WARP_DEADZONE_DIV 4

/* Put the pointer back in the middle once it has left the dead zone.
 *
 * Flushed on the spot: the pump dispatches every queued event before it
 * flushes, so a warp left in the output buffer is one the pointer spends a
 * whole burst of motion travelling without. */
static void recentre_pointer(App *a, int x, int y)
{
	struct BackendState *p = a->backend_state;
	const int mx = p->win_w / WARP_DEADZONE_DIV;
	const int my = p->win_h / WARP_DEADZONE_DIV;
	const int rx = x - p->warp_x, ry = y - p->warp_y;

	/* Inside the dead zone clears the flag as well as declining to warp: a
	 * warp whose own event never came back -- a broken grab, a window that
	 * resized under it -- would otherwise leave the flag set for good and stop
	 * every later re-centring, which is the one failure worse than warping too
	 * often. Being back in the middle half is proof there is nothing owed. */
	if (rx >= -mx && rx <= mx && ry >= -my && ry <= my) {
		p->warp_pending = false;
		return;
	}
	if (p->warp_pending)
		return;

	p->warp_pending = true;
	xcb_warp_pointer(p->conn, XCB_NONE, p->window, 0, 0, 0, 0,
					 (int16_t)p->warp_x, (int16_t)p->warp_y);
	xcb_flush(p->conn);
}

/* ---- properties the window advertises ------------------------------------ */

/* The size floor, as ICCCM's WM_NORMAL_HINTS, which a manager honours *during*
 * the drag. In *pixels*, so it is written again whenever the scale changes --
 * see BackendState::hints_scale120. */
static void set_size_hints(App *a)
{
	struct BackendState *p = a->backend_state;
	int min_w, min_h;
	deco_min_window(a, false, &min_w, &min_h);
	/* Resolved, and not want_scale120: that is the scale last published,
	 * which is none before the first publish and the old one while a change
	 * of density waits for the end of the pump turn. */
	const int s120 = x11_resolve_scale120(a);

	uint32_t hints[ICCCM_SIZE_HINTS_WORDS] = { 0 };
	hints[0] = ICCCM_P_MIN_SIZE;
	hints[ICCCM_SIZE_HINTS_MIN_W] = (uint32_t)scale_pt(min_w, s120);
	hints[ICCCM_SIZE_HINTS_MIN_H] = (uint32_t)scale_pt(min_h, s120);
	p->hints_scale120 = s120;

	xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE, p->window,
						XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32,
						ICCCM_SIZE_HINTS_WORDS, hints);
}

/* Motif's decoration hint: five CARD32s -- flags, functions, decorations, input
 * mode, status -- and a decorations field of zero is the whole request. Only
 * ever cleared, so there is no state to restore. */
static void clear_motif_decorations(struct BackendState *p)
{
	const uint32_t hints[5] = { MWM_HINTS_DECORATIONS, 0, 0, 0, 0 };
	xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE, p->window,
						p->atom[FNL_ATOM_MOTIF_WM_HINTS],
						p->atom[FNL_ATOM_MOTIF_WM_HINTS], 32, 5, hints);
}

/* Twice: _NET_WM_NAME is UTF-8, and WM_NAME is ICCCM's Latin-1, set to the
 * same bytes for a manager that reads only it. At open and from the drain;
 * the pump's turn flushes it. */
static void x11_set_title(App *a, const char *utf8)
{
	struct BackendState *p = a->backend_state;
	const size_t title_len = strlen(utf8);
	xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE, p->window,
						p->atom[FNL_ATOM_NET_WM_NAME],
						p->atom[FNL_ATOM_UTF8_STRING], 8,
						(uint32_t)title_len, utf8);
	xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE, p->window,
						XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
						(uint32_t)title_len, utf8);
}

static void set_window_properties(App *a)
{
	struct BackendState *p = a->backend_state;

	x11_set_title(a, a->title);

	/* Instance and class, NUL-separated and NUL-terminated: what a manager
	 * matches an icon and a window rule against. The library's own until there
	 * is a descriptor field for it. */
	static const char WM_CLASS_VALUE[] = "fenestal\0Fenestal";
	xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE, p->window,
						XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
						(uint32_t)sizeof WM_CLASS_VALUE, WM_CLASS_VALUE);

	/* WM_DELETE_WINDOW makes the close button an event instead of a killed
	 * client; _NET_WM_PING keeps "not responding" off a busy window; and
	 * _NET_WM_SYNC_REQUEST, where the server has SYNC, has a manager show a
	 * new size only once the frame drawn at it has gone to the server --
	 * without it the window wears the new size with the old frame, stretched
	 * or edged in black, until the next one lands. */
	const xcb_atom_t protocols[3] = { p->atom[FNL_ATOM_WM_DELETE_WINDOW],
									  p->atom[FNL_ATOM_NET_WM_PING],
									  p->atom[FNL_ATOM_NET_WM_SYNC_REQUEST] };
	xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE, p->window,
						p->atom[FNL_ATOM_WM_PROTOCOLS], XCB_ATOM_ATOM, 32,
						p->sync_counter ? 3 : 2, protocols);
	if (p->sync_counter)
		xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE, p->window,
							p->atom[FNL_ATOM_NET_WM_SYNC_REQUEST_COUNTER],
							XCB_ATOM_CARDINAL, 32, 1, &p->sync_counter);

	/* EWMH: the pid is only meaningful beside WM_CLIENT_MACHINE. */
	const uint32_t pid = (uint32_t)getpid();
	xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE, p->window,
						p->atom[FNL_ATOM_NET_WM_PID], XCB_ATOM_CARDINAL, 32,
						1, &pid);
	char host[256];
	if (gethostname(host, sizeof host) == 0) {
		host[sizeof host - 1] = '\0';
		xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE, p->window,
							XCB_ATOM_WM_CLIENT_MACHINE, XCB_ATOM_STRING, 8,
							(uint32_t)strlen(host), host);
	}

	/* An ordinary top-level window: taskbar button, alt-tab, snap. */
	const xcb_atom_t type = p->atom[FNL_ATOM_NET_WM_WINDOW_TYPE_NORMAL];
	xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE, p->window,
						p->atom[FNL_ATOM_NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM,
						32, 1, &type);

	/* WM_HINTS: nine CARD32s, of which three are written. Without InputHint a
	 * manager following ICCCM to the letter may never give this window the
	 * keyboard. */
	uint32_t wm_hints[9] = { 0 };
	wm_hints[0] = ICCCM_INPUT_HINT | ICCCM_STATE_HINT;
	wm_hints[1] = 1;                           /* input: yes            */
	wm_hints[2] = ICCCM_NORMAL_STATE;
	xcb_change_property(p->conn, XCB_PROP_MODE_REPLACE, p->window,
						XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32, 9,
						wm_hints);

	set_size_hints(a);

	clear_motif_decorations(p);
}

/* ---- reading the manager's answers --------------------------------------- */

/* _NET_WM_STATE: fullscreen, maximize and "hidden by the manager". */
static void read_wm_state(App *a)
{
	struct BackendState *p = a->backend_state;

	bool fs = false, vert = false, horz = false, hidden = false;

	xcb_get_property_reply_t *r =
		prop_get(p, p->window, p->atom[FNL_ATOM_NET_WM_STATE],
				 XCB_ATOM_ATOM, 64);
	const uint32_t n = x11_prop_words(r);
	const xcb_atom_t *v = n ? xcb_get_property_value(r) : NULL;
	for (uint32_t i = 0; i < n; i++) {
		const xcb_atom_t at = v[i];
		if (at == p->atom[FNL_ATOM_NET_WM_STATE_FULLSCREEN])     fs = true;
		if (at == p->atom[FNL_ATOM_NET_WM_STATE_MAXIMIZED_VERT]) vert = true;
		if (at == p->atom[FNL_ATOM_NET_WM_STATE_MAXIMIZED_HORZ]) horz = true;
		if (at == p->atom[FNL_ATOM_NET_WM_STATE_HIDDEN])         hidden = true;
	}
	free(r);

	/* Both axes: a window maximized in one direction can still be dragged, and
	 * the titlebar's icon should still offer to make it bigger. */
	const bool mx = vert && horz;
	if (fs == a->is_fullscreen && mx == a->is_maximized &&
		fnl_atomic_load_bool(&p->on_screen) == (p->mapped && !hidden))
		return;

	a->is_fullscreen = fs;
	a->is_maximized  = mx;
	fnl_atomic_store_bool(&p->on_screen, p->mapped && !hidden);
	p->publish_pending = true;
	app_mark_urgent(a);
}

/* ---- the event handlers -------------------------------------------------- */

/* win_* kept as the window's restored rectangle while it is in neither
 * state (BackendState::norm_x). */
static void note_normal(App *a, struct BackendState *p)
{
	if (a->is_fullscreen || a->is_maximized)
		return;
	p->norm_x = p->win_x;
	p->norm_y = p->win_y;
	p->norm_w = p->win_w;
	p->norm_h = p->win_h;
	p->norm_valid = true;
}

static void on_configure(App *a, const xcb_configure_notify_event_t *e,
						 bool synthetic)
{
	struct BackendState *p = a->backend_state;

	if (e->window == p->window) {
		/* A synthetic ConfigureNotify is the one ICCCM makes the manager send
		 * when it moves a window without resizing it, and its coordinates are
		 * root-relative by definition. A real one from a reparenting manager
		 * is relative to the frame, so it contributes the offset inside the
		 * frame and the frame's own configure contributes the rest. */
		if (synthetic) {
			p->win_x = e->x;
			p->win_y = e->y;
		} else {
			p->child_x = e->x;
			p->child_y = e->y;
			p->win_x = p->parent_x + e->x;
			p->win_y = p->parent_y + e->y;
		}
		p->win_w = e->width;
		p->win_h = e->height;
		note_normal(a, p);
		p->sync_configured = true;
		p->publish_pending = true;
		return;
	}

	if (e->window == p->parent && p->parent != p->root) {
		p->parent_x = e->x;
		p->parent_y = e->y;
		p->win_x = e->x + p->child_x;
		p->win_y = e->y + p->child_y;
		note_normal(a, p);
		/* Only the position moved, so only the output can have changed. Not
		 * the size, and therefore not the swapchain. */
		x11_publish_output(a, false);
	}
}

static void on_reparent(App *a, const xcb_reparent_notify_event_t *e)
{
	struct BackendState *p = a->backend_state;
	if (e->window != p->window)
		return;

	p->parent = e->parent;
	p->parent_x = p->parent_y = 0;
	p->child_x = e->x;
	p->child_y = e->y;

	/* Watch the frame the manager just put around us, so its moves arrive as
	 * events rather than as a TranslateCoordinates round trip per drag step.
	 * StructureNotify on a window somebody else owns is allowed -- it is
	 * SubstructureRedirect that only one client may hold. */
	if (p->parent != p->root && p->parent != XCB_NONE) {
		const uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
		xcb_change_window_attributes(p->conn, p->parent, XCB_CW_EVENT_MASK,
									 &mask);
		/* And where it is now, which no event will restate. */
		xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(
			p->conn, xcb_get_geometry(p->conn, p->parent), NULL);
		if (g) {
			p->parent_x = g->x;
			p->parent_y = g->y;
			free(g);
		}
	}
	p->win_x = p->parent_x + p->child_x;
	p->win_y = p->parent_y + p->child_y;
	note_normal(a, p);
	p->publish_pending = true;
}

/* A grab -- ours, a manager's, a menu's -- moves the focus without the window
 * having gained or lost anything, and NotifyPointer is not a focus change at
 * all. Both are filtered, or a titlebar would go grey every time the pointer
 * was captured. */
static void on_focus(App *a, const xcb_focus_in_event_t *e, bool in)
{
	/* A manager's keyboard grab ending with the focus ours: the pointer grab
	 * it refused at the focus-in can be had now. */
	if (in && e->mode == XCB_NOTIFY_MODE_UNGRAB)
		capture_resume(a);
	if (e->mode == XCB_NOTIFY_MODE_GRAB ||
		e->mode == XCB_NOTIFY_MODE_UNGRAB ||
		e->detail == XCB_NOTIFY_DETAIL_POINTER)
		return;

	if (in) {
		/* The keys already down, before the caller hears about the focus:
		 * fnl_key_down() should already answer by then. */
		app_keys_clear(a);
		x11_keys_seed(a);
	} else {
		/* Focus loss never delivers the matching releases: drop every held key
		 * so the character does not run off on its own. */
		app_keys_clear(a);
		memset(a->backend_state->key_down, 0,
			   sizeof a->backend_state->key_down);
		/* And an accent waiting for its letter: the letter is somebody
		 * else's now. */
		a->backend_state->compose.dead = 0;
		app_compose_set(a, NULL);
		capture_suspend(a);
	}
	a->backend_state->focus_in = in;
	if (in)
		capture_resume(a);
	app_publish_focus(a, in);
	app_post_focus(a, in);
}

/* fnl_set_cursor() moved the shape. The cursor is an attribute of the window,
 * so this is right wherever the pointer is. Only the cursor is re-decided, not
 * the hover: relighting a button from a position the pointer has left is what
 * LeaveNotify exists to undo. */
static void x11_cursor_changed(App *a)
{
	Edge edge;
	const Zone z = deco_hit(a, a->ptr_x, a->ptr_y, &edge);
	cursor_update(a, z, edge);
}

/* The pointer is at this pixel of the window, which a motion and an enter both
 * say. The event goes first, alongside whatever the library then does. */
static void pointer_at(App *a, int ex, int ey)
{
	const int s120 = want_s120(a);
	a->ptr_x = px_to_ptf(ex, s120);
	a->ptr_y = px_to_ptf(ey, s120);
	app_pointer_motion(a, a->ptr_x, a->ptr_y);
}

static void pointer_update(App *a, double x, double y)
{
	Edge edge;
	const Zone z = deco_pointer_at(a, x, y, &edge);
	cursor_update(a, z, edge);
}

static void on_motion(App *a, const xcb_motion_notify_event_t *e)
{
	struct BackendState *p = a->backend_state;

	if (p->captured && p->grab_suspended) {
		/* Over the window unfocused, or focused with the grab refused: the
		 * desktop's pointer, no movement of the reticle's. */
		capture_resume(a);
		return;
	}
	if (p->captured) {
		/* Against the previous event's landing, never against the middle.
		 *
		 * A warp is a request, and the server runs it when it reaches it.
		 * Every motion event already on its way carries a position from
		 * before that happened, so measured from the middle each would
		 * re-count what the ones before it had reported: a turn that
		 * collected n of them moves the reticle (n+1)/2 times too far, and n
		 * is however many that turn happened to collect. Measured from the
		 * last landing the positions telescope, and the arithmetic does not
		 * care when the warp lands. DESIGN *Pointer capture*. */
		const int dx = e->event_x - p->last_x;
		const int dy = e->event_y - p->last_y;
		p->last_x = e->event_x;
		p->last_y = e->event_y;

		/* The warp's own event coming back: it lands exactly on the middle.
		 * Dropped, at the cost of a genuine motion that ends there. */
		if (e->event_x == p->warp_x && e->event_y == p->warp_y) {
			p->warp_pending = false;
			return;
		}
		if (dx == 0 && dy == 0)
			return;

		const int s120 = want_s120(a);
		/* Points, the space the positions a caller is given are in. */
		app_pointer_relative(a, px_to_ptf(dx, s120), px_to_ptf(dy, s120));

		/* Nothing below may run for a capture that has ended: re-centring
		 * would take the desktop cursor a release had just placed on the
		 * reticle and throw it back into the middle of the window. */
		if (!p->captured)
			return;

		app_reticle_move(a, dx, dy);

		recentre_pointer(a, e->event_x, e->event_y);
		return;
	}

	/* Before deco_drag(): x11_begin_move() reads where the pointer is. */
	p->ptr_root_x = e->root_x;
	p->ptr_root_y = e->root_y;
	pointer_at(a, e->event_x, e->event_y);

	deco_drag(a, a->ptr_x, a->ptr_y);

	/* With no window manager the drag is run here, one step per motion
	 * event. */
	if (p->manual_drag)
		manual_moveresize(a, e->root_x, e->root_y,
						  (uint32_t)(p->manual_drag - 1));

	pointer_update(a, a->ptr_x, a->ptr_y);
}

static void on_enter(App *a, const xcb_enter_notify_event_t *e)
{
	if (a->backend_state->captured)
		return;
	/* The manager may have set its own. */
	a->backend_state->cursor_applied = X11_CURSOR_UNKNOWN;
	/* A caller should hear where: a hover that waits for the pointer to move
	 * again is stale for one that enters and stops. */
	pointer_at(a, e->event_x, e->event_y);
	pointer_update(a, a->ptr_x, a->ptr_y);
}

static void on_leave(App *a)
{
	if (a->backend_state->captured)
		return;
	deco_release(a);                /* the press is someone else's now */
	deco_publish_hover(a, ZONE_CONTENT);
}

/* X11 has no scroll axis: it has four more buttons. */
enum {
	WHEEL_UP    = 4,
	WHEEL_DOWN  = 5,
	WHEEL_LEFT  = 6,
	WHEEL_RIGHT = 7,
};

static void on_button(App *a, const xcb_button_press_event_t *e, bool pressed)
{
	struct BackendState *p = a->backend_state;
	const int s120 = want_s120(a);

	/* Before every early return below, because fnl_begin_move() is answered
	 * from a press and needs where that press was in root coordinates -- as
	 * do x11_begin_move() and x11_begin_resize() under deco_press(). */
	p->ptr_root_x = e->root_x;
	p->ptr_root_y = e->root_y;

	/* The wheel, turned back into a scroll event; the release that follows
	 * each click is dropped. One detent is 1.0 and positive is down: how far
	 * the wheel turned is not in the protocol. */
	if (e->detail >= WHEEL_UP && e->detail <= WHEEL_RIGHT) {
		if (!pressed)
			return;
		double dx = 0.0, dy = 0.0;
		switch (e->detail) {
		case WHEEL_UP:   dy = -1.0; break;
		case WHEEL_DOWN: dy =  1.0; break;
		case WHEEL_LEFT: dx = -1.0; break;
		default:         dx =  1.0; break;
		}
		app_pointer_scroll(a, a->ptr_x, a->ptr_y, dx, dy);
		return;
	}

	/* The three FnlButton names, and nothing for the rest: a side button
	 * reported as some other button would be a click nobody made. */
	FnlButton button;
	switch (e->detail) {
	case XCB_BUTTON_INDEX_1: button = FNL_BUTTON_LEFT;   break;
	case XCB_BUTTON_INDEX_2: button = FNL_BUTTON_MIDDLE; break;
	case XCB_BUTTON_INDEX_3: button = FNL_BUTTON_RIGHT;  break;
	default:
		return;
	}

	app_pointer_button(a, button, pressed, px_to_ptf(e->event_x, s120),
					   px_to_ptf(e->event_y, s120), e->time);
	/* A drag this backend was running itself ends with the button. */
	if (button == FNL_BUTTON_LEFT && !pressed)
		p->manual_drag = 0;
}

/* The two below name a window, and it has to be checked: this client watches
 * the manager's frame as well as its own (see on_reparent), and the frame is
 * mapped, unmapped and destroyed on its own schedule. */
static void on_map(App *a, const xcb_map_notify_event_t *e)
{
	struct BackendState *p = a->backend_state;
	if (e->window != p->window)
		return;
	p->mapped = true;
	fnl_atomic_store_bool(&p->on_screen, true);
	/* The window exists at a size now, whether or not a ConfigureNotify came
	 * with it -- a manager that grants exactly what was asked for sends none.
	 * This is what ends the wait in x11_open(). */
	a->configured = true;
	p->publish_pending = true;
}

static void on_unmap(App *a, const xcb_unmap_notify_event_t *e)
{
	struct BackendState *p = a->backend_state;
	if (e->window != p->window)
		return;
	/* Iconified, or on another workspace. The size is left alone: coming back
	 * should cost no swapchain rebuild. */
	p->mapped = false;
	fnl_atomic_store_bool(&p->on_screen, false);
}

static void on_client_message(App *a, const xcb_client_message_event_t *e)
{
	struct BackendState *p = a->backend_state;

	if (e->type != p->atom[FNL_ATOM_WM_PROTOCOLS] || e->format != 32)
		return;

	if (e->data.data32[0] == p->atom[FNL_ATOM_WM_DELETE_WINDOW]) {
		app_close_requested(a);
		return;
	}

	/* The value the counter is to reach once the size that follows is
	 * drawn. Published with the size as the configure's serial once the
	 * ConfigureNotify after it has come, or SYNC_CONFIGURE_WAIT after it
	 * where none does -- a size the window already has -- or the manager
	 * would wait for a frame nothing asks for (x11_publish_size()). */
	if (e->data.data32[0] == p->atom[FNL_ATOM_NET_WM_SYNC_REQUEST]) {
		p->sync_lo = e->data.data32[2];
		fnl_atomic_store_u32(&p->sync_hi, e->data.data32[3]);
		p->sync_asked = true;
		p->sync_configured = false;
		p->sync_deadline = now_seconds() + SYNC_CONFIGURE_WAIT;
		p->publish_pending = true;
		return;
	}

	if (e->data.data32[0] == p->atom[FNL_ATOM_NET_WM_PING]) {
		/* Bounced back to the root with our window id left in place, as EWMH
		 * asks. */
		xcb_client_message_event_t reply = *e;
		reply.window = p->root;
		root_send(p, &reply);
	}
}

static void on_property(App *a, const xcb_property_notify_event_t *e)
{
	struct BackendState *p = a->backend_state;

	if (e->window == p->window) {
		if (e->atom == p->atom[FNL_ATOM_NET_WM_STATE])
			read_wm_state(a);
		else
			x11_clip_property(a, e);
		return;
	}
	if (e->window != p->root)
		return;

	/* The desktop's density changed: everything laid out in points is a
	 * different number of pixels now. */
	if (e->atom == XCB_ATOM_RESOURCE_MANAGER) {
		const int s = scale_from_resource_manager(p, resources_ask(p));
		if (s != p->xft_scale120) {
			p->xft_scale120 = s;
			set_size_hints(a);           /* the floor is in pixels */
			p->publish_pending = true;
		}
		return;
	}
	if (e->atom == p->atom[FNL_ATOM_NET_WORKAREA] ||
		e->atom == p->atom[FNL_ATOM_NET_CURRENT_DESKTOP])
		p->work_area_stale = true;
}

/* One X error, which arrives long after the request that caused it. Capped:
 * see BackendState::errors_logged. */
#define X_ERRORS_LOGGED_MAX 8

static void on_error(App *a, const xcb_generic_error_t *e, const char *which)
{
	struct BackendState *p = a->backend_state;
	if (p->errors_logged >= X_ERRORS_LOGGED_MAX)
		return;
	if (++p->errors_logged == X_ERRORS_LOGGED_MAX) {
		fnl_log(FNL_LOG_WARN, "further X errors are not reported");
		return;
	}
	/* The two connections fail for unrelated reasons. */
	fnl_log(FNL_LOG_WARN, "X error %u on the %s connection, request %u.%u "
						  "(resource %u)",
			(unsigned)e->error_code, which, (unsigned)e->major_code,
			(unsigned)e->minor_code, (unsigned)e->resource_id);
}

static void dispatch(App *a, const xcb_generic_event_t *ev)
{
	/* Only ICCCM's synthetic ConfigureNotify cares whether it was sent, but the
	 * bit has to come off the type before any comparison. */
	const bool synthetic = (ev->response_type & X11_EVENT_SENT) != 0;
	const uint8_t type = ev->response_type & (uint8_t)X11_EVENT_TYPE_MASK;

	if (x11_present_event(a, ev) || x11_ext_event(a->backend_state, ev))
		return;

	switch (type) {
	case 0:
		on_error(a, (const xcb_generic_error_t *)ev, "window");
		break;

	case XCB_CONFIGURE_NOTIFY:
		on_configure(a, (const xcb_configure_notify_event_t *)ev, synthetic);
		break;
	case XCB_REPARENT_NOTIFY:
		on_reparent(a, (const xcb_reparent_notify_event_t *)ev);
		break;
	case XCB_MAP_NOTIFY:
		on_map(a, (const xcb_map_notify_event_t *)ev);
		break;
	case XCB_UNMAP_NOTIFY:
		on_unmap(a, (const xcb_unmap_notify_event_t *)ev);
		break;
	case XCB_DESTROY_NOTIFY:
		/* Ours and not the frame's, or the window would close every time a
		 * manager reparented it back. */
		if (((const xcb_destroy_notify_event_t *)ev)->window ==
			a->backend_state->window)
			fnl_atomic_store_bool(&a->should_close, true);
		break;

	/* Each input stamped with the server's time first, for
	 * fnl_input_latency_ms(). */
	case XCB_KEY_PRESS:
		loop_input_ms(a, ((const xcb_key_press_event_t *)ev)->time);
		x11_on_key(a, (const xcb_key_press_event_t *)ev, true);
		break;
	case XCB_KEY_RELEASE:
		loop_input_ms(a, ((const xcb_key_press_event_t *)ev)->time);
		x11_on_key(a, (const xcb_key_press_event_t *)ev, false);
		break;

	case XCB_BUTTON_PRESS:
		loop_input_ms(a, ((const xcb_button_press_event_t *)ev)->time);
		on_button(a, (const xcb_button_press_event_t *)ev, true);
		break;
	case XCB_BUTTON_RELEASE:
		loop_input_ms(a, ((const xcb_button_press_event_t *)ev)->time);
		on_button(a, (const xcb_button_press_event_t *)ev, false);
		break;
	case XCB_MOTION_NOTIFY:
		loop_input_ms(a, ((const xcb_motion_notify_event_t *)ev)->time);
		on_motion(a, (const xcb_motion_notify_event_t *)ev);
		break;
	case XCB_ENTER_NOTIFY:
		loop_input_ms(a, ((const xcb_enter_notify_event_t *)ev)->time);
		on_enter(a, (const xcb_enter_notify_event_t *)ev);
		break;
	case XCB_LEAVE_NOTIFY:
		on_leave(a);
		break;

	case XCB_FOCUS_IN:
		on_focus(a, (const xcb_focus_in_event_t *)ev, true);
		break;
	case XCB_FOCUS_OUT:
		on_focus(a, (const xcb_focus_in_event_t *)ev, false);
		break;

	case XCB_CLIENT_MESSAGE:
		on_client_message(a, (const xcb_client_message_event_t *)ev);
		break;
	case XCB_PROPERTY_NOTIFY:
		on_property(a, (const xcb_property_notify_event_t *)ev);
		break;

	case XCB_SELECTION_REQUEST:
		x11_selection_request(a, (const xcb_selection_request_event_t *)ev);
		break;
	case XCB_SELECTION_NOTIFY:
		x11_selection_notify(a, (const xcb_selection_notify_event_t *)ev);
		break;
	case XCB_SELECTION_CLEAR:
		/* Somebody else copied; the string stays ours to free. */
		break;

	case XCB_MAPPING_NOTIFY:
		/* Only a server without XKB sends this; x11_ext_event() takes the XKB
		 * form. FnlKey is positional and unaffected; what a key *types*
		 * changed, unless only the pointer's buttons were remapped. */
		if (((const xcb_mapping_notify_event_t *)ev)->request !=
			XCB_MAPPING_POINTER)
			a->backend_state->keymap_stale = true;
		break;

	default:
		break;
	}
}

/* ---- the event loop -------------------------------------------------------
 *
 * xcb decodes events into a queue of its own, so the socket can be empty while
 * there is still work: the queued pass comes before the wait, and the wait only
 * when it found nothing. The wait may be indefinite because nothing else reads
 * this connection -- see BackendState::conn. */

static bool dispatch_queued(App *a)
{
	struct BackendState *p = a->backend_state;
	xcb_generic_event_t *ev;
	bool any = false;

	while ((ev = xcb_poll_for_queued_event(p->conn))) {
		dispatch(a, ev);
		free(ev);
		any = true;
	}
	return any;
}

static void dispatch_read(App *a)
{
	struct BackendState *p = a->backend_state;
	xcb_generic_event_t *ev;

	/* Runs to empty, so nothing is left for a level-triggered epoll to report
	 * again. */
	while ((ev = xcb_poll_for_event(p->conn))) {
		dispatch(a, ev);
		free(ev);
	}
}

/* Whatever the presentation engine's connection has decoded and nobody wants:
 * errors, worth a line, and Present events that missed the special queue a
 * destroyed swapchain unregistered. Undrained, the queue only grows.
 *
 * xcb_poll_for_queued_event and never xcb_poll_for_event: the queued form
 * cannot touch the socket, so this does not become a second reader of it. */
static void gfx_drain(App *a)
{
	struct BackendState *p = a->backend_state;
	xcb_generic_event_t *ev;

	if (!p->gfx)
		return;
	while ((ev = xcb_poll_for_queued_event(p->gfx))) {
		if ((ev->response_type & (uint8_t)X11_EVENT_TYPE_MASK) == 0)
			on_error(a, (const xcb_generic_error_t *)ev, "graphics");
		free(ev);
	}
}

/* Block until the connection or the wakeup has something, or the timeout runs
 * out; negative waits indefinitely. */
static void wait_fds(App *a, int timeout_ms)
{
	struct BackendState *p = a->backend_state;
	struct epoll_event evs[2];

	const int n = epoll_wait(p->epoll_fd, evs, 2, timeout_ms);
	for (int i = 0; i < n; i++)
		if (evs[i].data.fd == p->wake_fd)
			loop_wake_drain(p->wake_fd);
}

/* One turn. This is also where every request made on the pump's thread is
 * flushed -- before the wait, and at the end -- so a handler under dispatch()
 * and a vtable entry spent by app_drain_requests() queue theirs and leave it.
 * recentre_pointer() is the one that cannot wait, and says why. */
static int sync_await_ms(struct BackendState *p);
static void sync_await_expire(struct BackendState *p);

static void x11_pump(App *a)
{
	struct BackendState *p = a->backend_state;

	/* Never block with a request unsent, and the flush ahead of the queued
	 * pass rather than between it and the wait: a flush that writes also
	 * reads what the socket holds into xcb's queue, where the wait would
	 * not see it. */
	xcb_flush(p->conn);
	/* Not with the refresh rates owed: they are read on the turn after the
	 * list, and a window that is left alone once open has no event to wake
	 * that turn -- an idle window would be spaced by no refresh at all. */
	if (!dispatch_queued(a) && !p->refresh_stale) {
		/* For ever, unless a paste is owed: then only for as long as its
		 * owner is given to send the next piece. */
		int ms = loop_wait_ms(p->clip_waiting, p->clip.deadline);
		/* And back by a sync request's deadline, where its configure has
		 * not come: the publish that answers it is owed then. */
		if (p->sync_asked && !p->sync_configured) {
			const int sync_ms = loop_wait_ms(true, p->sync_deadline);
			ms = ms < 0 || sync_ms < ms ? sync_ms : ms;
		}
		/* And by a size's hold, which a frame presented also ends. */
		const int held_ms = size_wait_ms(p);
		if (held_ms >= 0)
			ms = ms < 0 || held_ms < ms ? held_ms : ms;
		/* And by a sync value kept for its frame's completion. */
		const int await_ms = sync_await_ms(p);
		if (await_ms >= 0)
			ms = ms < 0 || await_ms < ms ? await_ms : ms;
		wait_fds(a, ms);
		dispatch_read(a);
	}
	size_release(p);
	sync_await_expire(p);
	x11_clip_expire(a);
	/* One answer per turn, however many events asked for it. */
	outputs_flush(a);
	publish_flush(a);
	background_update(a);
	gfx_drain(a);
	xcb_flush(p->conn);
}

static void x11_wakeup(App *a)
{
	if (a->backend_state)
		loop_wake(a->backend_state->wake_fd);
}

static FnlResult x11_check_error(App *a)
{
	/* Both: a graphics connection that dropped is as fatal as the window's, and
	 * is otherwise reported only as whatever the next present returns. */
	const char *which = "window";
	int err = xcb_connection_has_error(a->backend_state->conn);
	if (!err && a->backend_state->gfx) {
		err = xcb_connection_has_error(a->backend_state->gfx);
		which = "graphics";
	}
	if (!err)
		return FNL_OK;

	static const char *const WHY[] = {
		"", "socket, pipe or stream error", "an extension is not supported",
		"out of memory", "request longer than the server accepts",
		"the display string would not parse", "no such screen",
		"file-descriptor passing failed",
	};
	return fnl_fail(FNL_ERR_NO_DISPLAY, "the %s X connection failed: %s", which,
					err > 0 && err < (int)(sizeof WHY / sizeof WHY[0])
						? WHY[err] : "unknown error");
}

/* ---- the backend seam -----------------------------------------------------
 *
 * Only close() and wakeup() test a->backend_state: core/backend.h has the
 * rule. */

/* [render] The frame answering a sync request is about to be presented:
 * the counter is owed, and set by the flush after the present
 * (x11_flush()), so the manager cannot see it before the frame. The present
 * goes out on the graphics connection and the counter on the window's, and
 * nothing orders two connections; after the present has been sent is the
 * nearest this side can come. */
static void x11_ack_configure(App *a, uint32_t serial)
{
	struct BackendState *p = a->backend_state;
	/* Answered, as Win32's and Wayland's acks say: left pending, every frame
	 * after this one would ack and wait again, and the loop would never
	 * rest. */
	fnl_mutex_lock(&a->lock);
	if (a->cfg_serial == serial)
		a->cfg_pending = false;
	fnl_mutex_unlock(&a->lock);
	fnl_mutex_lock(&p->sync_lock);
	p->sync_release = (uint64_t)fnl_atomic_load_u32(&p->sync_hi) << 32 |
					  serial;
	p->sync_presents = a->presents;
	p->sync_due = true;
	/* A value kept for an earlier frame is spent: this one goes with its
	 * own frame, and is the larger. */
	p->sync_await = false;
	fnl_mutex_unlock(&p->sync_lock);
}

/* The counter set to the value owed, if one is; whether it was, which off
 * the pump is a write that owes the pump a wake (BackendState::conn). */
static bool sync_release_owed(struct BackendState *p)
	FNL_REQUIRES(p->sync_lock)
{
	p->sync_await = false;
	if (!p->sync_due)
		return false;
	x11_sync_set(p, p->sync_release);
	fnl_atomic_store_u32(&p->sync_set_lo, (uint32_t)p->sync_release);
	p->sync_due = false;
	return true;
}

/* [main] Present answered frames up to this index: a value kept for one of
 * them goes now (x11_ext.c, under no lock). */
void x11_sync_answered(struct BackendState *p, uint64_t index)
{
	fnl_mutex_lock(&p->sync_lock);
	if (p->sync_await && index >= p->sync_await_index)
		sync_release_owed(p);
	fnl_mutex_unlock(&p->sync_lock);
}

/* [main] How long the pump may sleep before a kept value is due anyway;
 * negative for none kept. */
static int sync_await_ms(struct BackendState *p)
{
	fnl_mutex_lock(&p->sync_lock);
	const int ms = loop_wait_ms(p->sync_await, p->sync_await_until);
	fnl_mutex_unlock(&p->sync_lock);
	return ms;
}

/* [main] A kept value whose completion has not come goes now, and the
 * window's presents are taken for unanswered until one is answered: a
 * presenter that is not Present's would otherwise hold every size its
 * bound. */
static void sync_await_expire(struct BackendState *p)
{
	bool ran_out = false;
	uint64_t kept = 0;
	fnl_mutex_lock(&p->sync_lock);
	if (p->sync_await && now_seconds() >= p->sync_await_until) {
		kept = p->sync_await_index;
		sync_release_owed(p);
		ran_out = true;
	}
	fnl_mutex_unlock(&p->sync_lock);
	if (!ran_out)
		return;
	if (p->present_log) {
		fnl_mutex_lock(&p->present_lock);
		const int n = p->present_n;
		const uint64_t oldest = n ? p->present_q[p->present_head] : 0;
		fnl_mutex_unlock(&p->present_lock);
		fnl_log(FNL_LOG_INFO, "X11 present: the value kept for frame %llu "
				"ran out, %d queued, the oldest %llu",
				(unsigned long long)kept, n, (unsigned long long)oldest);
	}
	fnl_atomic_store_bool(&p->present_live, false);
	if (!p->sync_await_said) {
		p->sync_await_said = true;
		fnl_log(FNL_LOG_INFO, "X11: a frame that answers a size was not "
							  "answered by Present within %.0f ms; its "
							  "sync value went without it",
				SYNC_SHOWN_WAIT * 1e3);
	}
}

/* [render] The frame about to be presented, queued for the Present
 * extension's completion that will say when it was shown (x11_ext.c):
 * what the frame loop's hold waits on, and the stamp where the caller
 * counts. */
static void x11_frame_request(App *a)
{
	struct BackendState *p = a->backend_state;
	p->present_pushed = false;
	if (!p->present_opcode)
		return;
	fnl_mutex_lock(&p->present_lock);
	if (p->present_n == X11_PRESENT_Q) {
		p->present_head = (p->present_head + 1) % X11_PRESENT_Q;
		p->present_n--;
	}
	if (p->present_chain_seen != a->present_chain) {
		p->present_chain_seen = a->present_chain;
		p->present_ord_next = 0;
	}
	const int at = (p->present_head + p->present_n) % X11_PRESENT_Q;
	p->present_q[at] = a->presents;
	p->present_at[at] = now_seconds();
	p->present_ord[at] = ++p->present_ord_next;
	p->present_n++;
	const int queued = p->present_n;
	fnl_mutex_unlock(&p->present_lock);
	p->present_index = a->presents;
	p->present_pushed = true;
	if (p->present_log)
		fnl_log(FNL_LOG_INFO, "X11 present: frame %llu pushed, %d queued",
				(unsigned long long)a->presents, queued);
}

/* [render] The present it was for never went out: no completion comes. */
static void x11_frame_cancel(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p->present_pushed)
		return;
	p->present_pushed = false;
	/* Answered already, on the main thread, before the present returned
	 * its refusal: the frame went up whatever the graphics API says --
	 * NVIDIA's driver says out of date of a present it shows -- and is
	 * counted (app_frame_take_back()), so its index is not drawn again and
	 * stamped twice. Completions come in order, so its entry is gone. */
	fnl_mutex_lock(&p->present_lock);
	const bool answered = p->present_answered > p->present_index;
	if (!answered && p->present_n > 0)
		p->present_n--;
	const int queued = p->present_n;
	fnl_mutex_unlock(&p->present_lock);
	if (answered) {
		a->present_answered_anyway = true;
		if (p->present_log)
			fnl_log(FNL_LOG_INFO, "X11 present: frame %llu refused, and "
					"answered already: counted as shown",
					(unsigned long long)p->present_index);
		return;
	}
	/* Nothing went to the server, so the presenter's count did not move. */
	if (p->present_ord_next > 0)
		p->present_ord_next--;
	if (p->present_log)
		fnl_log(FNL_LOG_INFO, "X11 present: frame %llu taken back, %d "
				"queued", (unsigned long long)p->present_index, queued);
	/* A value kept for this frame's completion would wait out its bound.
	 * Queued, not flushed: the pump's turn sends it. */
	bool sent = false;
	fnl_mutex_lock(&p->sync_lock);
	if (p->sync_await && p->sync_await_index == p->present_index)
		sent = sync_release_owed(p);
	fnl_mutex_unlock(&p->sync_lock);
	if (sent)
		loop_wake(p->wake_fd);
}

static void x11_flush(App *a)
{
	struct BackendState *p = a->backend_state;
	/* Only after a present since the ack: app_surface_sync() can flush
	 * between the two. Not while the renderer still waits for that
	 * present's frame to be shown (App::ack_held): x11_ack_release() is
	 * then the one that sets it. Where it does not wait -- OpenGL, whose
	 * eglSwapBuffers returns before the server has the frame, as Mesa's
	 * Vulkan present does -- the value is kept for Present's completion of
	 * this frame (x11_sync_answered()), which the pump's turn bounds, so
	 * the manager sees it only once the frame is shown (DESIGN *X11: xcb,
	 * not Xlib*). Not where the window's presents go unanswered
	 * (present_live). */
	bool kept = false, sent = false;
	fnl_mutex_lock(&p->sync_lock);
	if (a->presents > p->sync_presents &&
		!fnl_atomic_load_bool(&a->ack_held) && !p->sync_await) {
		if (p->sync_due && p->present_pushed &&
			fnl_atomic_load_bool(&p->present_live) &&
			!FNL_FAULT("x11-sync-at-swap")) {
			p->sync_await = true;
			p->sync_await_index = p->present_index;
			p->sync_await_until = now_seconds() + SYNC_SHOWN_WAIT;
			/* Answered already, between the present and here. */
			fnl_mutex_lock(&p->present_lock);
			const bool answered = p->present_answered > p->present_index;
			fnl_mutex_unlock(&p->present_lock);
			if (answered)
				sent = sync_release_owed(p);
			else
				kept = true;
			if (p->present_log)
				fnl_log(FNL_LOG_INFO, "X11 present: frame %llu answers a "
						"size, its value %s",
						(unsigned long long)p->present_index,
						answered ? "sent, the frame answered already"
								 : "kept");
		} else {
			sent = sync_release_owed(p);
		}
	}
	fnl_mutex_unlock(&p->sync_lock);
	/* The window's connection, from the render thread: the crossing
	 * BackendState::conn allows, with the wake after it that the crossing
	 * owes. The value is all this thread queues there, so nothing else is
	 * flushed. Where kept, the pump may be asleep with no deadline and
	 * takes the bound. */
	if (sent)
		xcb_flush(p->conn);
	if (kept || sent)
		loop_wake(p->wake_fd);
	/* The size the frame just presented was drawn at, for a size held
	 * behind it (size_give()), and the pump woken for that size. */
	if (a->presents != p->flush_presents) {
		p->flush_presents = a->presents;
		fnl_atomic_store_u32(&p->shown_w, (uint32_t)a->extent.w);
		fnl_atomic_store_u32(&p->shown_h, (uint32_t)a->extent.h);
		if (fnl_atomic_load_bool(&p->held_wake)) {
			fnl_atomic_store_bool(&p->held_wake, false);
			loop_wake(p->wake_fd);
		}
	}
}

/* [renderer's waiter] The frame the value is owed for is on the screen.
 * The second thread that writes on the window's connection, as the render
 * thread's flush does: xcb takes a request and a flush from any thread, and
 * the request is built in a buffer of its own (x11_sync_set()). It wakes
 * the pump after, as every writer off the pump does (BackendState::conn). */
static void x11_ack_release(App *a)
{
	struct BackendState *p = a->backend_state;
	fnl_mutex_lock(&p->sync_lock);
	const bool sent = sync_release_owed(p);
	fnl_mutex_unlock(&p->sync_lock);
	if (!sent)
		return;
	xcb_flush(p->conn);
	loop_wake(p->wake_fd);
}

/* Mapped, and not hidden by the manager. Not "covered": no window server
 * answers that, and FnlRendererVtbl::occluded exists for it. An atomic read,
 * because asking the server is a round trip on the frame path. */
static bool x11_window_on_screen(const App *a)
{
	const struct BackendState *p = a->backend_state;
	return fnl_atomic_load_bool(&p->on_screen);
}

/* Xft.dpi is kept current by on_property(), so this resolves from what is
 * held and says only what has moved. Both of fnl_init()'s calls find the
 * scale the window was opened at, and cost nothing. */
static void x11_update_scale(App *a)
{
	struct BackendState *p = a->backend_state;
	const int s120 = x11_resolve_scale120(a);
	const bool moved = app_publish_scale(a, s120);

	/* The floor is held by the manager in pixels, the output reported in
	 * points. */
	if (moved)
		x11_publish_output(a, true);
	if (p->hints_scale120 != s120) {
		set_size_hints(a);
		/* fnl_init() calls this ahead of the first pump turn, so the pump's
		 * flush is not there to rely on. */
		xcb_flush(p->conn);
	}
}

/* A manager may decline; the ConfigureNotify that follows a size that changed
 * is the answer, so nothing is kept against it. */
static void x11_request_size(App *a, int w_pt, int h_pt)
{
	struct BackendState *p = a->backend_state;
	const int s120 = want_s120(a);
	size_give(p, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, 0, 0,
			  scale_pt(w_pt, s120), scale_pt(h_pt, s120));
}

/* A pixel wider and straight back. A configure at the size the window has
 * is announced too, but does not free NVIDIA's swapchain; a size that
 * changes does (DESIGN *X11: xcb, not Xlib*). Whichever of the two sizes
 * the pump publishes, the window ends at its own. Not where a manager
 * holds it maximized or fullscreen: a configure there asks the manager to
 * let go of that state.
 *
 * The width is the server's, by a round trip, and not win_w: that is the
 * last ConfigureNotify's, and a configure still unanswered -- a step of a
 * drag, a caller's size, an earlier nudge's way back -- would be undone by
 * a way back to it. Asked at most once in eight rebuilds that drew
 * nothing, so the wait costs no frame. */
static void x11_nudge_size(App *a)
{
	struct BackendState *p = a->backend_state;
	if (p->ewmh && (a->is_maximized || a->is_fullscreen))
		return;
	xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(p->conn,
		xcb_get_geometry(p->conn, p->window), NULL);
	if (!g)
		return;
	const uint32_t w = g->width;
	free(g);
	const uint32_t wider[1] = { w + 1 };
	const uint32_t back[1]  = { w };
	xcb_configure_window(p->conn, p->window, XCB_CONFIG_WINDOW_WIDTH, wider);
	xcb_configure_window(p->conn, p->window, XCB_CONFIG_WINDOW_WIDTH, back);
}

/* ---- open and close ------------------------------------------------------ */

/* The presentation engine's own connection; see BackendState::conn.
 *
 * xcb_connect never returns NULL -- a failure is a connection with its error
 * set, which xcb_disconnect still accepts -- so x11_close() copes with this
 * having failed as readily as with it never having been called. */
static FNL_MUST_USE FnlResult create_gfx_connection(App *a)
{
	struct BackendState *p = a->backend_state;

	/* The same display string, so the same screen; the number is not used. */
	int screen_num = 0;
	p->gfx = xcb_connect(NULL, &screen_num);
	if (xcb_connection_has_error(p->gfx))
		return fnl_fail(FNL_ERR_NO_DISPLAY,
						"the second X connection, for the presentation "
						"engine, could not be opened");
	return FNL_OK;
}

static FNL_MUST_USE FnlResult create_window_x11(App *a)
{
	struct BackendState *p = a->backend_state;

	p->conn = xcb_connect(NULL, &p->screen_num);
	if (xcb_connection_has_error(p->conn))
		return fnl_fail(FNL_ERR_NO_DISPLAY,
						"no X display (is DISPLAY set?)");

	/* The screen the display string named, out of the setup. */
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(p->conn));
	for (int i = 0; it.rem; xcb_screen_next(&it), i++) {
		if (i == p->screen_num) {
			p->screen = it.data;
			break;
		}
	}
	if (!p->screen)
		return fnl_fail(FNL_ERR_NO_DISPLAY, "the display has no screen %d",
						p->screen_num);

	p->root   = p->screen->root;
	p->parent = p->root;

	/* Opening costs three waits on the server, and the order below is what
	 * keeps it at three: every request whose inputs are known is sent before
	 * the first reply to any of them is taken. A reply taken early is a round
	 * trip of its own. */
	intern_atoms(p);

	/* The root's properties carry the density and the work area. PropertyChange
	 * on the root is shared, so this takes nothing from the manager. Selected
	 * before either is read, so no change falls between. */
	const uint32_t root_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(p->conn, p->root, XCB_CW_EVENT_MASK,
								 &root_mask);

	const X11ExtAsk    ext_ask    = x11_ext_ask(p);
	const X11KeymapAsk keymap_ask = x11_keymap_ask(p);
	const xcb_get_property_cookie_t wm_cookie =
		xcb_get_property(p->conn, 0, p->root,
						 p->atom[FNL_ATOM_NET_SUPPORTING_WM_CHECK],
						 XCB_ATOM_WINDOW, 0, 1);
	const xcb_get_property_cookie_t resources_cookie = resources_ask(p);
	const X11WorkAreaAsk area_ask = x11_work_area_ask(p);
	/* In the same wait: whether a compositor owns the screen's selection. */
	const xcb_get_selection_owner_cookie_t cm_cookie =
		xcb_get_selection_owner(p->conn, p->atom_cm);

	/* _NET_SUPPORTING_WM_CHECK is EWMH's own answer to whether anybody is
	 * managing; everything asked of a manager has a manual path behind it. */
	xcb_get_property_reply_t *wm =
		xcb_get_property_reply(p->conn, wm_cookie, NULL);
	p->ewmh = x11_prop_words(wm) >= 1;
	free(wm);
	if (!p->ewmh)
		fnl_log(FNL_LOG_WARN, "no EWMH window manager is running; the "
							  "titlebar moves and resizes the window itself");

	xcb_get_selection_owner_reply_t *cm =
		xcb_get_selection_owner_reply(p->conn, cm_cookie, NULL);
	p->composited = cm && cm->owner != XCB_WINDOW_NONE;
	free(cm);

	p->xft_scale120 = scale_from_resource_manager(p, resources_cookie);
	x11_work_area_take(p, area_ask);
	x11_keymap_take(p, keymap_ask);

	/* Final before there is a window: no scale here depends on the monitor. */
	const int s120 = x11_resolve_scale120(a);
	int w_pt, h_pt;
	deco_default_window_pt(a, s120, &w_pt, &h_pt);
	p->win_w = scale_pt(w_pt, s120);
	p->win_h = scale_pt(h_pt, s120);

	p->window = xcb_generate_id(p->conn);

	/* Black rather than nothing behind the window, and north-west bit gravity
	 * over it. Neither is cosmetic: a window with no background shows whatever
	 * was behind it until the first present, and one with no gravity has its
	 * contents discarded on every resize step. */
	const uint32_t values[3] = {
		p->screen->black_pixel,
		XCB_GRAVITY_NORTH_WEST,
		XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
		XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
		XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW |
		XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
		XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE,
	};
	xcb_create_window(p->conn, XCB_COPY_FROM_PARENT, p->window, p->root,
					  0, 0, (uint16_t)p->win_w, (uint16_t)p->win_h, 0,
					  XCB_WINDOW_CLASS_INPUT_OUTPUT, p->screen->root_visual,
					  XCB_CW_BACK_PIXEL | XCB_CW_BIT_GRAVITY |
					  XCB_CW_EVENT_MASK, values);

	/* Before the properties, which advertise it. */
	x11_sync_open(p);
	x11_present_open(p);
	set_window_properties(a);
	set_cursor(a, X11_CURSOR_DEFAULT);

	/* Last of the takes: reading the monitor list sends for the monitors'
	 * names, and that wait is the third. */
	x11_ext_take(p, &ext_ask);
	/* The monitors are known from here, and a manager answers for itself. */
	if (!p->ewmh)
		fit_opening(a, s120);

	xcb_map_window(p->conn, p->window);
	xcb_flush(p->conn);

	if (xcb_connection_has_error(p->conn))
		return fnl_fail(FNL_ERR_NO_DISPLAY,
						"the X connection failed while opening the window");
	return FNL_OK;
}

/* Block until the window is on screen. A manager can decline to map one, so the
 * wait is bounded, and a window never mapped goes ahead at the size it asked
 * for rather than hanging fnl_init(). */
#define MAP_WAIT_SECONDS 2.0

static void wait_for_map(App *a)
{
	struct BackendState *p = a->backend_state;
	const double deadline = now_seconds() + MAP_WAIT_SECONDS;

	while (!a->configured && !xcb_connection_has_error(p->conn)) {
		/* What the handlers queued, sent before the wait, as the pump's
		 * turn does (x11_pump()). */
		xcb_flush(p->conn);
		if (dispatch_queued(a))
			continue;
		const double left = deadline - now_seconds();
		if (left <= 0.0)
			break;
		wait_fds(a, (int)(left * 1000.0) + 1);
		dispatch_read(a);
	}

	if (!a->configured) {
		fnl_log(FNL_LOG_WARN, "the window manager never mapped the window; "
							  "drawing at the size that was asked for");
		a->configured = true;
		p->publish_pending = true;
		/* And drawn: on_screen gates the frame loop, so leaving it false here
		 * would answer the warning above with a window that never renders. */
		fnl_atomic_store_bool(&p->on_screen, true);
	}

	/* Whatever state the manager mapped us in: it sets _NET_WM_STATE before the
	 * map, and that PropertyNotify may predate this client selecting for one.
	 */
	read_wm_state(a);
	publish_flush(a);
}

static FnlResult x11_open(App *a)
{
	/* libxcb before the state: without it this is a display fnl_init()
	 * steps past, and x11_close(), which calls through what this fills,
	 * finds no state and returns. */
	const char *no_xcb = x11_xcb_load();
	if (no_xcb)
		return fnl_fail(FNL_ERR_NO_DISPLAY, "%s", no_xcb);

	a->backend_state = calloc(1, sizeof *a->backend_state);
	if (!a->backend_state)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
						"out of memory for the X11 backend state");
	a->backend_state->cursor_applied = X11_CURSOR_UNKNOWN;
	a->backend_state->published_monitor = -1;
	const char *plog = getenv("FNL_X11_PRESENT_LOG");
	a->backend_state->present_log = plog && strcmp(plog, "1") == 0;
	fnl_mutex_init(&a->backend_state->sync_lock);
	fnl_mutex_init(&a->backend_state->present_lock);
	/* Not left at the calloc's zero, which is stdin: x11_close() decides
	 * whether to close these, and "never made" has to be a value that is not
	 * somebody else's descriptor. */
	a->backend_state->wake_fd = a->backend_state->epoll_fd = -1;

	/* The wakeup first: everything after this can fail, and x11_close() -- the
	 * failure path out of here -- is written to cope with any prefix. */
	FNL_TRY(loop_wake_create(&a->backend_state->wake_fd));
	FNL_TRY(create_window_x11(a));
	FNL_TRY(create_gfx_connection(a));
	FNL_TRY(loop_epoll_create(&a->backend_state->epoll_fd,
							  xcb_get_file_descriptor(a->backend_state->conn),
							  a->backend_state->wake_fd));
	wait_for_map(a);
	return FNL_OK;
}

/* Innermost first, and every step conditional: this is also the failure path
 * out of x11_open(), so it runs against every prefix of what that builds. */
static void x11_close(App *a)
{
	struct BackendState *p = a->backend_state;
	if (!p)
		return;

	/* The graphics connection first: it presents into the window destroyed
	 * below, and the other order asks the server to tear its pixmaps down
	 * against a drawable that has just been freed. The surface is gone by now;
	 * app_teardown() destroys it before closing the backend. */
	xcb_disconnect(p->gfx);
	p->gfx = NULL;

	if (p->conn && !xcb_connection_has_error(p->conn)) {
		if (p->captured)
			xcb_ungrab_pointer(p->conn, XCB_CURRENT_TIME);
		for (int i = 0; i < X11_CURSOR_COUNT; i++)
			if (p->cursors[i])
				xcb_free_cursor(p->conn, p->cursors[i]);
		if (p->blank_cursor)
			xcb_free_cursor(p->conn, p->blank_cursor);
		if (p->cursor_font)
			xcb_close_font(p->conn, p->cursor_font);
		x11_sync_close(p);
		if (p->window)
			xcb_destroy_window(p->conn, p->window);
		xcb_flush(p->conn);
	}
	/* Frees the connection whether or not it is in error, and copes with a
	 * NULL -- which is what a failed xcb_connect leaves. */
	xcb_disconnect(p->conn);

	/* The wakeup and epoll descriptors, closed unless a signal handler could
	 * still reach them through app_wakeup(): app_teardown() clears
	 * installed_signals once it has waited every handler out. */
	if (!a->installed_signals) {
		if (p->epoll_fd >= 0) close(p->epoll_fd);
		if (p->wake_fd  >= 0) close(p->wake_fd);
	}
	p->epoll_fd = p->wake_fd = -1;

	clip_in_free(&a->backend_state->clip);
	fnl_mutex_destroy(&a->backend_state->sync_lock);
	fnl_mutex_destroy(&a->backend_state->present_lock);
	free(a->backend_state);
	a->backend_state = NULL;
}

/* ---- the seam's move and resize -------------------------------------------
 *
 * From the pointer's last known root position, because _NET_WM_MOVERESIZE is
 * told where the drag started. No slop of their own: the caller has decided the
 * gesture is a drag. */
static void x11_begin_move(App *a)
{
	struct BackendState *p = a->backend_state;
	if (p->ewmh) {
		wm_moveresize(a, p->ptr_root_x, p->ptr_root_y, MOVERESIZE_MOVE,
					  XCB_BUTTON_INDEX_1);
	} else {
		/* Run by this backend, a ConfigureWindow per motion. */
		p->drag_off_x = p->ptr_root_x - p->win_x;
		p->drag_off_y = p->ptr_root_y - p->win_y;
		p->manual_drag = EDGE_NONE + 1;
	}
}

static void x11_begin_resize(App *a, Edge edge)
{
	struct BackendState *p = a->backend_state;
	if (edge <= EDGE_NONE || edge >= EDGE_COUNT)
		return;
	if (p->ewmh)
		wm_moveresize(a, p->ptr_root_x, p->ptr_root_y,
					  EDGE_CODES[edge].moveresize, XCB_BUTTON_INDEX_1);
	else
		p->manual_drag = (int)edge + 1;
}

/* The manager holds the floor, so one that moved is restated. */
static void x11_apply_size_bounds(App *a)
{
	set_size_hints(a);
}

/* xcb_connect(NULL) reads DISPLAY and has nothing to go on without it. */
static const char *x11_absent(void)
{
	const char *d = getenv("DISPLAY");
	return d && d[0] ? NULL : "no DISPLAY";
}

const FnlBackendVtbl fnl_backend_x11 = {
	.name               = "X11",
	.id                 = BACKEND_X11,
	.key                = "x11",
	.absent             = x11_absent,
	.host               = backend_noop_host,
	.shared_driver      = backend_noop_shared_driver,
	.gpu_drivers        = backend_noop_gpu_drivers,
	.gpu_driver_set     = backend_noop_gpu_driver_set,
	.gpu_driver_held    = backend_noop_gpu_driver_held,
	/* No ring: an X11 window has no separation between what it presents and
	 * what the manager positions, so the resize edges are an inner DECO_GRAB
	 * margin the hit test answers for. */
	.grab_ring          = 0,
	/* No invitation to draw: the present is the throttle. See
	 * src/core/frame.c for the late-latch signal that replaces it. */
	.invites            = false,
	.display_waits      = true,
	.ack_when_shown     = true,
	.open               = x11_open,
	.close              = x11_close,
	.pump               = x11_pump,
	.wakeup             = x11_wakeup,
	.check_error        = x11_check_error,
	.flush              = x11_flush,
	.present_pixels = backend_noop_present_pixels,
	.set_geometry       = backend_noop_set_geometry,
	.ack_configure      = x11_ack_configure,
	.ack_release        = x11_ack_release,
	.frame_request      = x11_frame_request,
	.frame_cancel       = x11_frame_cancel,
	.present_window_renew = backend_noop,
	.present_window_hold  = backend_noop,
	.frame_wait         = backend_frame_wait_shown,
	.window_on_screen   = x11_window_on_screen,
	.apply_scale        = backend_noop_apply_scale,
	.request_size       = x11_request_size,
	.nudge_size         = x11_nudge_size,
	.set_fullscreen     = x11_set_fullscreen,
	.set_maximized      = x11_set_maximized,
	.minimize           = x11_minimize,
	.modes              = backend_noop_modes,
	.outputs            = backend_published_outputs,
	.set_mode           = backend_noop_set_mode,
	.set_output         = x11_set_output,
	.begin_move         = x11_begin_move,
	.begin_resize       = x11_begin_resize,
	.apply_size_bounds  = x11_apply_size_bounds,
	.buffer_granularity = backend_noop_buffer_granularity,
	.update_scale       = x11_update_scale,
	.capture_pointer    = x11_capture,
	.release_pointer    = x11_release,
	.cursor_changed     = x11_cursor_changed,
	.text_input_changed = x11_text_input_changed,
	.clipboard_set      = x11_clipboard_set,
	.clipboard_request  = x11_clipboard_request,
	.set_title          = x11_set_title,
};

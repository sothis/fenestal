/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The testbed's own overlay: a keybindings panel, a status readout and the
 * crosshair a captured pointer gets. F5's metrics panel is the library's
 * (fnl_set_metrics_panel()), shown from the start by main.c.
 *
 * This file includes <fenestal/fenestal.h> and nothing else of Fenestal's: no
 * internal header, no App, no palette, no Vulkan. Every figure it needs it
 * asks for, and anything it cannot ask for is an API that is missing.
 *
 * The crosshair's position is this file's own state, moved by
 * FNL_EVENT_RELATIVE. Fenestal has no pointer warp -- Wayland forbids one --
 * so the application owns where its reticle is, which is also the only
 * arrangement that can put it where a click landed.
 */

#include <fenestal/fenestal.h>

#include "testbed.h"

#include <stdio.h>
#include <string.h>

/* The testbed's palette; the library ships none for a caller's own drawing. */
static const FnlColor COL_PANEL  = TESTBED_RGB(0.06f, 0.06f, 0.08f);
static const FnlColor COL_TITLE  = TESTBED_RGB(1.00f, 0.78f, 0.30f);
static const FnlColor COL_KEY    = TESTBED_RGB(0.80f, 0.85f, 0.95f);
static const FnlColor COL_DESC   = TESTBED_RGB(0.55f, 0.58f, 0.65f);
static const FnlColor COL_DIM    = TESTBED_RGB(0.45f, 0.48f, 0.55f);
static const FnlColor COL_ACTIVE = TESTBED_RGB(0.35f, 1.00f, 0.55f);

/* Points from the window edge to the panels. */
#define MARGIN 12

/* ---- state --------------------------------------------------------------- */

/* Plain fields and no lock: the events are drained inside on_overlay, so
 * everything that writes these and everything that reads them is one turn on
 * one thread. */
static struct {
	bool   show_help;       /* F1: the keybindings and the status readout */
	/* Where the crosshair is, as a *fraction* of the presentable area rather
	 * than a count of its pixels. A fraction means the same thing at any
	 * size, so a fullscreen toggle, a drag-resize, a scale change or a pick
	 * from the size list all leave the crosshair where it was relative to the
	 * picture. Pixels would want re-clamping on every FNL_EVENT_RESIZE: miss
	 * one and the crosshair is drawn outside the area and clipped away, which
	 * is a captured pointer with no cursor and no crosshair on screen. */
	double reticle_x, reticle_y;
} g_ui = { .show_help = true, .reticle_x = 0.5, .reticle_y = 0.5 };

/* Clamped here, so "inside the area" is one function's promise rather than a
 * rule the relative path and the click each have to remember. */
static void reticle_set(double x, double y)
{
	if (x < 0.0) x = 0.0;
	if (y < 0.0) y = 0.0;
	if (x > 1.0) x = 1.0;
	if (y > 1.0) y = 1.0;
	g_ui.reticle_x = x;
	g_ui.reticle_y = y;
}

/* ---- the keybindings panel ----------------------------------------------- */

static const struct {
	const char *key;
	const char *desc;
	FnlKey      held[2];  /* highlight while either is down; UNKNOWN for none */
} BINDINGS[] = {
	{ "W / Up",    "Forward",          { FNL_KEY_W,      FNL_KEY_UP     } },
	{ "S / Down",  "Back",             { FNL_KEY_S,      FNL_KEY_DOWN   } },
	{ "A / Left",  "Turn left",        { FNL_KEY_A,      FNL_KEY_LEFT   } },
	{ "D / Right", "Turn right",       { FNL_KEY_D,      FNL_KEY_RIGHT  } },
	{ "Shift",     "Run",              { FNL_KEY_LSHIFT, FNL_KEY_RSHIFT } },
	{ "Click LMB", "Capture mouse",    { FNL_KEY_UNKNOWN } },
	{ "F1",        "Toggle this help", { FNL_KEY_UNKNOWN } },
	{ "F2",        "Scene motion",     { FNL_KEY_UNKNOWN } },
	{ "F3",        "Text entry",       { FNL_KEY_UNKNOWN } },
	{ "F4 / F6",   "Copy / paste",     { FNL_KEY_UNKNOWN } },
	{ "F5",        "Toggle metrics",   { FNL_KEY_UNKNOWN } },
	{ "F9",        "Release mouse",    { FNL_KEY_UNKNOWN } },
	{ "F11",       "Fullscreen",       { FNL_KEY_UNKNOWN } },
	{ "Esc",       "Quit",             { FNL_KEY_UNKNOWN } },
};
#define BINDING_COUNT ((int)(sizeof BINDINGS / sizeof BINDINGS[0]))
#define KEY_COL_CHARS 11      /* key column width, in character cells */

#define HELP_TITLE   "Keybindings"

/* Both panels are padded by this and both are inset from the window edge by
 * MARGIN. Named once because the fit test below measures what the drawing
 * draws, and two copies of a pad is how those come apart. */
#define PANEL_PAD    10
/* Clear space between the titlebar and the top of the help panel. */
#define HELP_TOP_GAP 10

/* The status readout's rows. The panel is placed before the array of strings
 * exists; the assert in draw_status() keeps the two counts together. */
enum { STATUS_ROWS = 5 };

/* Panel geometry, in window points. Each figure is asked for twice -- once to
 * draw and once to decide whether to -- so they are functions. The help
 * panel's size takes no app: the baked faces are fixed, so what a string
 * measures to does not depend on the window. */
static int help_panel_h(void)
{
	/* The header and its gap, and the bindings. */
	return (BINDING_COUNT + 2) * fnl_text_line(FNL_TEXT_BODY) + 2 * PANEL_PAD;
}

static int help_panel_w(void)
{
	const int adv = fnl_text_advance(FNL_TEXT_BODY);

	/* The widest row is one of the two things the panel is sized to. */
	int longest = 0;
	for (int i = 0; i < BINDING_COUNT; i++) {
		int chars = KEY_COL_CHARS + (int)strlen(BINDINGS[i].desc);
		if (chars > longest)
			longest = chars;
	}

	/* Two claims on the width: the widest row, and the title. */
	const int rows_w = longest * adv;
	const int hdr_w  = fnl_text_width(HELP_TITLE, FNL_TEXT_BODY);
	return (rows_w > hdr_w ? rows_w : hdr_w) + 2 * PANEL_PAD;
}

/* Where the status panel's background begins. Anchored to the bottom of the
 * *presentable area*, not of the window: fnl_content_rect_pt() is the rectangle
 * this program is entitled to draw in, and the titlebar above it is not. */
static int status_panel_top(FnlApp *app)
{
	int cx, cy, cw, ch;
	fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);
	(void)cx; (void)cw;
	return cy + ch - MARGIN - STATUS_ROWS * fnl_text_line(FNL_TEXT_BODY)
		   - PANEL_PAD;
}

/* Whether there is room for it. The status readout does not yield to the
 * other panel but it does yield to the titlebar: nothing this program draws
 * may cover the bar, which is the only way the window can be moved, resized
 * or closed. */
static bool status_panel_fits(FnlApp *app)
{
	int cx, cy, cw, ch;
	fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);
	(void)cx; (void)cw;
	return status_panel_top(app) >= cy;
}

/* Whether the keybindings panel has room. It is the one that yields when the
 * two meet -- it is the half F1 turns off, and its background would land on
 * the status text, since every overlay rectangle is drawn before any text
 * (DESIGN *Give-way rules*). Width as well as height: a panel wider than the
 * area is clipped, and a list with its right-hand column sliced off is worse
 * than none. */
static bool help_panel_fits(FnlApp *app, bool status_shown)
{
	int cx, cy, cw, ch;
	fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);

	if (MARGIN + help_panel_w() > cx + cw)
		return false;
	/* Clear of the status readout where there is one, and of the bottom of the
	 * area where there is not -- so the panel gives way to the other panel
	 * *and* to the window, rather than to whichever happens to be checked. */
	const int limit = status_shown ? status_panel_top(app)
								   : cy + ch - MARGIN;
	return cy + HELP_TOP_GAP + help_panel_h() <= limit;
}

/* Whether the footprint -- this process's CPU, RAM and VRAM, the figures
 * the library's metrics panel shows -- is said on stderr (--footprint-log),
 * for a harness that cannot read the panel. */
static bool g_footprint_log;

void testbed_overlay_footprint_log(void)
{
	g_footprint_log = true;
}

/* The GPU memory this process holds on the GPU drawing: the scene's API's
 * figure (scene_<api>.c), or for OpenGL, which keeps none, the system's
 * (gpumem.c). */
bool testbed_vram_mib(FnlApp *app, double *mib)
{
	switch (fnl_renderer(app)) {
#ifdef SCENE_HAS_VULKAN
	case FNL_RENDERER_VULKAN: return scene_vulkan_vram(app, mib);
#endif
#ifdef SCENE_HAS_D3D12
	case FNL_RENDERER_D3D12:  return scene_d3d12_vram(app, mib);
#endif
#ifdef SCENE_HAS_METAL
	case FNL_RENDERER_METAL:  return scene_metal_vram(app, mib);
#endif
	case FNL_RENDERER_OPENGL: return testbed_gpu_mem_os(fnl_gpu_name(app), mib);
	default:                  return false;
	}
}

static void draw_help(FnlApp *app)
{
	const int line = fnl_text_line(FNL_TEXT_BODY);
	const int adv  = fnl_text_advance(FNL_TEXT_BODY);
	const int pad  = PANEL_PAD;

	const int pw = help_panel_w();
	const int ph = help_panel_h();

	/* Under the titlebar, wherever the library left off -- the content rect's
	 * origin is how this finds that without knowing whether there is a
	 * titlebar or how tall it is. */
	int cx, cy;
	fnl_content_rect_pt(app, &cx, &cy, NULL, NULL);
	int x = MARGIN, y = cy + HELP_TOP_GAP;

	fnl_overlay_rect(app, x, y, pw, ph, COL_PANEL);

	int tx = x + pad, ty = y + pad;

	fnl_overlay_text(app, tx, ty, FNL_TEXT_BODY, COL_TITLE, HELP_TITLE);
	ty += 2 * line;

	for (int i = 0; i < BINDING_COUNT; i++, ty += line) {
		/* Asked per key: the held set answers for the key named and no
		 * other, and FNL_KEY_UNKNOWN is never down. */
		bool held = fnl_key_down(app, BINDINGS[i].held[0]) ||
					fnl_key_down(app, BINDINGS[i].held[1]);
		FnlColor k = held ? COL_ACTIVE : COL_KEY;
		FnlColor d = held ? COL_ACTIVE : COL_DESC;
		fnl_overlay_text(app, tx, ty, FNL_TEXT_BODY, k, BINDINGS[i].key);
		fnl_overlay_text(app, tx + KEY_COL_CHARS * adv, ty, FNL_TEXT_BODY, d,
						 BINDINGS[i].desc);
	}
}

/* ---- the status readout -------------------------------------------------- */

static void draw_status(FnlApp *app, const FnlFrame *f)
{
	const int line = fnl_text_line(FNL_TEXT_BODY);

	char gpu[288], plat[160], mode[160], cur[48], out[80];
	snprintf(gpu, sizeof gpu, "GPU  %s", fnl_gpu_name(app));
	const char *o = fnl_output_name(app);
	snprintf(out, sizeof out, "Output  %s", o && o[0] ? o : "-");

	/* Which platform this is, the renderer on the backend: the window looks
	 * the same on each. The library publishes both halves; the phrasing is
	 * the caller's. And what that backend is itself on, where it is not the
	 * machine: a Win32 window under wine is an X11 or a Wayland one to the
	 * desktop. */
	const char *host = fnl_window_host(app);
	char how[64];
	testbed_renderer_label(app, how, sizeof how);
	snprintf(plat, sizeof plat, "Platform  %s on %s%s%s%s", how,
			 fnl_backend_name(app), host[0] ? " (" : "", host,
			 host[0] ? ")" : "");

	if (fnl_is_pointer_captured(app)) {
		/* In pixels of the area this frame has, converted from the stored
		 * fraction, so the figure is always inside the size printed on the
		 * line above it. */
		int pw, ph;
		fnl_content_size_px(app, &pw, &ph);
		snprintf(cur, sizeof cur, "Cursor  %d, %d",
				 (int)(g_ui.reticle_x * pw), (int)(g_ui.reticle_y * ph));
	}
	else
		snprintf(cur, sizeof cur, "Cursor  -   (click to capture)");

	/* The presentable area, not the buffer: the grab ring around the window is
	 * not presented. The scene's own resolution is named beside it only when
	 * a render scale makes the two differ. */
	char scene[32] = "";
	if (f->scene_w_px > 0 &&
		(f->scene_w_px != f->area_w_px || f->scene_h_px != f->area_h_px))
		snprintf(scene, sizeof scene, "Scene %dx%d   ", f->scene_w_px,
				 f->scene_h_px);

	/* What the antialiasing got, which is not always what was asked for: the
	 * frame reports the sample count the device gave. */
	char aa[32] = "";
	if (f->samples > 1)
		snprintf(aa, sizeof aa, "FSAA %dx   ", f->samples);
	else if (fnl_antialias(app) == FNL_AA_MLAA)
		snprintf(aa, sizeof aa, "MLAA   ");

	snprintf(mode, sizeof mode, "%dx%d px   %s%s%s",
			 f->area_w_px, f->area_h_px, scene, aa, fnl_present_mode(app));

	int x = MARGIN;
	/* status_panel_top() is where the *background* starts; the first row of
	 * text sits one pad below it. */
	int y = status_panel_top(app) + PANEL_PAD;

	/* On a panel of its own: the test pattern's colour bars run under this
	 * corner, and dim grey text on saturated red is not legible. */
	const char *rows[] = { gpu, plat, out, mode, cur };
	_Static_assert(sizeof rows / sizeof rows[0] == STATUS_ROWS,
				   "the status rows and STATUS_ROWS have drifted apart");
	int longest = 0;
	for (int i = 0; i < STATUS_ROWS; i++) {
		int w = fnl_text_width(rows[i], FNL_TEXT_BODY);
		if (w > longest)
			longest = w;
	}
	const int pad = PANEL_PAD;
	fnl_overlay_rect(app, x - pad, y - pad, longest + 2 * pad,
					 STATUS_ROWS * line + 2 * pad, COL_PANEL);

	for (int i = 0; i < STATUS_ROWS; i++)
		fnl_overlay_text(app, x, y + i * line, FNL_TEXT_BODY, COL_DIM, rows[i]);
}

/* ---- the crosshair ------------------------------------------------------- */

#define CURSOR_ARM   11.0f    /* points from centre to the end of an arm */
#define CURSOR_GAP    3.0f    /* points of clear space at the centre     */
#define CURSOR_THICK  1.5f

static void draw_reticle(FnlApp *app)
{
	if (!fnl_is_pointer_captured(app))
		return;

	int cx, cy, cw, ch;
	fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);
	int pw, ph;
	fnl_content_size_px(app, &pw, &ph);
	if (pw <= 0 || ph <= 0)
		return;

	const float x = (float)cx + (float)(g_ui.reticle_x * cw);
	const float y = (float)cy + (float)(g_ui.reticle_y * ch);

	/* Clipped to the area the crosshair moves in, so an arm at the very edge
	 * ends there rather than reaching over the titlebar or into the grab
	 * ring. No flush on either side: fnl_overlay_clip() issues what has been
	 * appended so far before it changes the scissor. The restore is what
	 * keeps the rest of the frame's text -- the library's caption included --
	 * out of this program's scissor. */
	fnl_overlay_clip(app, cx, cy, cw, ch);
	fnl_overlay_stroke(app, x - CURSOR_ARM, y, x - CURSOR_GAP, y,
					   CURSOR_THICK, COL_ACTIVE);
	fnl_overlay_stroke(app, x + CURSOR_GAP, y, x + CURSOR_ARM, y,
					   CURSOR_THICK, COL_ACTIVE);
	fnl_overlay_stroke(app, x, y - CURSOR_ARM, x, y - CURSOR_GAP,
					   CURSOR_THICK, COL_ACTIVE);
	fnl_overlay_stroke(app, x, y + CURSOR_GAP, x, y + CURSOR_ARM,
					   CURSOR_THICK, COL_ACTIVE);
	fnl_overlay_clip(app, 0, 0, 0, 0);
}

/* ---- what main.c calls --------------------------------------------------- */

/* The footprint line, at most twice a second (proc.c's own pace). */
static void footprint_log(FnlApp *app, double now)
{
	static double at = -1.0;
	if (at >= 0.0 && now >= at && now - at < 0.5)
		return;
	at = now;
	double cpu, ram, vram;
	testbed_proc_sample(&cpu, &ram);
	if (!testbed_vram_mib(app, &vram))
		vram = -1.0;
	const double ms = fnl_frame_gpu_ms(app);
	const int fps = fnl_fps(app);
	const double gpu = ms >= 0.0 && fps >= 0 ? ms * fps / 10.0 : -1.0;
	fprintf(stderr, "footprint: CPU %.1f %%, RAM %.0f MiB, VRAM %.0f MiB, "
			"GPU %.1f %%, on %s with %s\n", cpu, ram, vram, gpu,
			fnl_gpu_name(app), fnl_renderer_name(app));
}

/* From on_overlay, after the events. The overlay's ordering rule is this
 * file's to keep: every filled rectangle above is a background, drawn before
 * any stroke or text. */
void testbed_overlay_draw(FnlApp *app, const FnlFrame *f)
{
	bool help = false, status = false;
	if (g_ui.show_help) {
		/* Two give-ways, in order: the status readout yields to the titlebar,
		 * and the keybindings yield to whichever of the two is below them. See
		 * status_panel_fits() and help_panel_fits(). */
		status = status_panel_fits(app);
		help = help_panel_fits(app, status);
		if (help)
			draw_help(app);
		if (status)
			draw_status(app, f);
	}
	/* The footprint line wants the frame's figures measured at rest, which
	 * the library's panel does not keep (DESIGN *The metrics panel*). */
	if (g_footprint_log)
		footprint_log(app, f->time);
	fnl_set_cost_sampling(app, g_footprint_log);
	draw_reticle(app);
}

/* From the event drain in main.c. The library has already acted on whatever
 * it binds; this adds the testbed's own. */
void testbed_overlay_event(FnlApp *app, const FnlEvent *ev)
{
	switch (ev->type) {
	case FNL_EVENT_KEY:
		/* F1 toggles a panel the library cannot see, so nothing in what it
		 * compares between frames moves with the flag: the invalidate is what
		 * gets the frame re-recorded. Not on a repeat -- there are none
		 * unless text entry is on, and then a held F1 would flicker the
		 * panel. */
		if (ev->key.pressed && !ev->key.repeat && ev->key.key == FNL_KEY_F1) {
			g_ui.show_help = !g_ui.show_help;
			fnl_invalidate(app);
		}
		/* F3 switches text entry, so a harness can type at this program and
		 * still close it with Escape afterwards. Nothing is drawn for it. */
		if (ev->key.pressed && !ev->key.repeat && ev->key.key == FNL_KEY_F3)
			fnl_set_text_input(app, !fnl_is_text_input(app));
		/* The clipboard, one key each way: F4 offers a string a harness can
		 * recognise and F6 asks for whatever is there, which comes back as
		 * FNL_EVENT_CLIPBOARD. Both in answer to a key press, which is the
		 * only time Wayland allows it. */
		if (ev->key.pressed && !ev->key.repeat && ev->key.key == FNL_KEY_F4)
			fnl_clipboard_set(app, TESTBED_CLIPBOARD_TEXT);
		if (ev->key.pressed && !ev->key.repeat && ev->key.key == FNL_KEY_F6)
			fnl_clipboard_request(app);
		/* The panel also lights a row while its key is held. The held set is
		 * in what the library compares, so that needs no invalidate. */
		break;

	case FNL_EVENT_RELATIVE: {
		/* The delta is in logical points, as every pointer figure in
		 * fenestal.h is, so it is divided by the area in *points* --
		 * fnl_content_rect_pt(), the rectangle draw_reticle() places against.
		 * fnl_content_size_px() answers in pixels, and the crosshair would run
		 * slow by the surface scale. */
		int cx, cy, cw, ch;
		fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);
		(void)cx; (void)cy;
		double rx = g_ui.reticle_x, ry = g_ui.reticle_y;
		if (cw > 0) rx += ev->relative.dx / cw;
		if (ch > 0) ry += ev->relative.dy / ch;
		reticle_set(rx, ry);
		fnl_invalidate(app);
		break;
	}

	case FNL_EVENT_BUTTON:
		/* Start the reticle where the click landed. */
		if (ev->button.pressed && ev->button.button == FNL_BUTTON_LEFT) {
			int cx, cy, cw, ch;
			fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);
			if (cw > 0 && ch > 0) {
				reticle_set((ev->button.x - cx) / (double)cw,
							(ev->button.y - cy) / (double)ch);
			}
			/* The click may also have started a capture, which is what puts
			 * the crosshair on screen in the first place. */
			fnl_invalidate(app);
		}
		break;

	default:
		break;
	}
}

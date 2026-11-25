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

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* The testbed's palette; the library ships none for a caller's own drawing.
 * The braces are written out: an initialiser is no place for a function
 * call, and TESTBED_RGB is one in every build (testbed.h). */
static const FnlColor COL_PANEL  = { 0.06f, 0.06f, 0.08f, 1.0f };
static const FnlColor COL_TITLE  = { 1.00f, 0.78f, 0.30f, 1.0f };
static const FnlColor COL_KEY    = { 0.80f, 0.85f, 0.95f, 1.0f };
static const FnlColor COL_DESC   = { 0.55f, 0.58f, 0.65f, 1.0f };
static const FnlColor COL_DIM    = { 0.45f, 0.48f, 0.55f, 1.0f };
static const FnlColor COL_ACTIVE = { 0.35f, 1.00f, 0.55f, 1.0f };

/* Points from the window edge to the panels. */
#define MARGIN 12

/* ---- state --------------------------------------------------------------- */

/* Atomic fields and no lock: a window's events are drained inside its own
 * on_overlay, on its own render thread, and the panels are drawn by every
 * window's -- so what one thread writes the others read. No more than that
 * is wanted of them: a crosshair a frame behind is a crosshair, and a panel
 * flag seen one event late is the next frame's picture. */
static struct {
	_Atomic bool show_help; /* F1: the keybindings and the status readout */
	/* Where the crosshair is, as a *fraction* of the presentable area rather
	 * than a count of its pixels. A fraction means the same thing at any
	 * size, so a fullscreen toggle, a drag-resize, a scale change or a pick
	 * from the size list all leave the crosshair where it was relative to the
	 * picture. Pixels would want re-clamping on every FNL_EVENT_RESIZE: miss
	 * one and the crosshair is drawn outside the area and clipped away, which
	 * is a captured pointer with no cursor and no crosshair on screen. */
	_Atomic double reticle_x, reticle_y;
	/* O: the shapes panel -- the four filled forms, so that a round can be
	 * shown to draw them. Off by default: the testbed's own panels are what a
	 * round measures, and this is a picture of the primitives rather than a
	 * readout. */
	_Atomic bool shapes;
} g_ui = { .show_help = true, .reticle_x = 0.5, .reticle_y = 0.5 };

/* Clamped here, so "inside the area" is one function's promise rather than a
 * rule the relative path and the click each have to remember. */
static void reticle_set(double x, double y)
{
	if (x < 0.0)
		x = 0.0;
	if (y < 0.0)
		y = 0.0;
	if (x > 1.0)
		x = 1.0;
	if (y > 1.0)
		y = 1.0;
	atomic_store(&g_ui.reticle_x, x);
	atomic_store(&g_ui.reticle_y, y);
}

/* The scene's own statement of how far it moved since the frame before it,
 * in pixels of the scene (fnl_set_scene_motion_px()), so that what the
 * resolve is reprojecting by can be read off the screen. Held here and not
 * read back, because the API says what was *set* and the resolve reads what
 * the frame latched: the two differ while a change is in flight, and it is
 * the frame's half the resolve uses. g_motion is that second half, the
 * scene's clock, read from fnl_scene_motion() as the event drain passes. */
static _Atomic float g_motion_x, g_motion_y;
static _Atomic bool  g_motion;

/* The scene's motion vector: F8 steps it up by MOTION_STEP and Shift with
 * F8 steps it back, clamped to [0, MOTION_MAX] -- so a run of presses with
 * Shift returns it to zero, the still scene the clamp alone was worst at
 * (DESIGN *Antialiasing*). The vertical is half the horizontal, the way a
 * camera panning left and up would put the picture. The step is a few
 * pixels: a whole texel at a typical scene size is what one press is worth.
 * The ceiling is well under a scene's width, since a vector larger than that
 * reprojects the history off the picture entirely. */
#define MOTION_STEP 1.0f
#define MOTION_MAX  64.0f

static void motion_step(float d)
{
	float x = atomic_load(&g_motion_x) + d;
	if (x > MOTION_MAX)
		x = MOTION_MAX;
	else if (x < 0.0f)
		x = 0.0f;
	atomic_store(&g_motion_x, x);
	atomic_store(&g_motion_y, x * 0.5f);
}

/* ---- the keybindings panel ----------------------------------------------- */

static const struct {
	const char *key;
	const char *desc;
	FnlKey held[2]; /* highlight while either is down; UNKNOWN for none */
} BINDINGS[] = {
	{ "W / Up", "Forward", { FNL_KEY_W, FNL_KEY_UP } },
	{ "S / Down", "Back", { FNL_KEY_S, FNL_KEY_DOWN } },
	{ "A / Left", "Turn left", { FNL_KEY_A, FNL_KEY_LEFT } },
	{ "D / Right", "Turn right", { FNL_KEY_D, FNL_KEY_RIGHT } },
	{ "Shift", "Run", { FNL_KEY_LSHIFT, FNL_KEY_RSHIFT } },
	{ "Click LMB", "Capture mouse", { FNL_KEY_UNKNOWN } },
	{ "F1", "Toggle this help", { FNL_KEY_UNKNOWN } },
	{ "F2", "Scene motion", { FNL_KEY_UNKNOWN } },
	{ "F8", "Scene motion px", { FNL_KEY_UNKNOWN } },
	{ "F3", "Text entry", { FNL_KEY_UNKNOWN } },
	{ "F4 / F6", "Copy / paste", { FNL_KEY_UNKNOWN } },
	{ "F5", "Toggle metrics", { FNL_KEY_UNKNOWN } },
	{ "F7", "Next output", { FNL_KEY_UNKNOWN } },
	{ "I", "Picture", { FNL_KEY_UNKNOWN } },
	{ "O", "Shapes", { FNL_KEY_UNKNOWN } },
	{ "F9", "Release mouse", { FNL_KEY_UNKNOWN } },
	{ "F10", "Second window", { FNL_KEY_UNKNOWN } },
	{ "F11", "Fullscreen", { FNL_KEY_UNKNOWN } },
	{ "F12", "Scene view", { FNL_KEY_UNKNOWN } },
	{ "Esc", "Quit", { FNL_KEY_UNKNOWN } },
};
#define BINDING_COUNT ((int)(sizeof BINDINGS / sizeof BINDINGS[0]))
#define KEY_COL_CHARS 11 /* key column width, in character cells */

#define HELP_TITLE "Keybindings"

/* Both panels are padded by this and both are inset from the window edge by
 * MARGIN. Named once because the fit test below measures what the drawing
 * draws, and two copies of a pad is how those come apart. */
#define PANEL_PAD 10
/* Clear space between the titlebar and the top of the help panel. */
#define HELP_TOP_GAP 10

/* The status readout's rows. The panel is placed before the array of strings
 * exists; the assert in draw_status() keeps the two counts together. */
enum { STATUS_ROWS = 6 };

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
	(void)cx;
	(void)cw;
	return cy + ch - MARGIN - STATUS_ROWS * fnl_text_line(FNL_TEXT_BODY) -
		   PANEL_PAD;
}

/* Whether there is room for it. The status readout does not yield to the
 * other panel but it does yield to the titlebar: nothing this program draws
 * may cover the bar, which is the only way the window can be moved, resized
 * or closed. */
static bool status_panel_fits(FnlApp *app)
{
	int cx, cy, cw, ch;
	fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);
	(void)cx;
	(void)cw;
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
	const int limit = status_shown ? status_panel_top(app) : cy + ch - MARGIN;
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

/* --frame-cpu-log: the per-frame CPU figure alone, without asking for cost
 * sampling, so the panel's at-rest frames stay the panel's. */
static bool g_frame_cpu_log;

void testbed_overlay_frame_cpu_log(void)
{
	g_frame_cpu_log = true;
}

/* --reserve-log: the caller's run's place beside the library's, and the
 * width at which the output box gives way under it. */
static bool g_reserve_log;

void testbed_overlay_reserve_log(void)
{
	g_reserve_log = true;
}

void testbed_overlay_shapes_on(void)
{
	atomic_store(&g_ui.shapes, true);
}

/* --frame-stats-log: the totals fnl_frame_stats() keeps and the rows
 * fnl_frame_samples() hands over, twice a second, with the figures the
 * frame's means are kept from. The totals are since counting was turned on,
 * so the rows are summed from there too: the two must be the same account,
 * and a window at rest must keep the figures while the panel's own frames
 * are drawn and counted. */
static bool g_frame_stats_log;
/* Written by whichever window's draw turns the stats on first: atomic, since
 * two windows draw at once. */
static _Atomic bool g_stats_on;

void testbed_overlay_frame_stats_log(void)
{
	g_frame_stats_log = true;
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
	case FNL_RENDERER_D3D12: return scene_d3d12_vram(app, mib);
#endif
#ifdef SCENE_HAS_METAL
	case FNL_RENDERER_METAL: return scene_metal_vram(app, mib);
#endif
	case FNL_RENDERER_OPENGL: return testbed_gpu_mem_os(fnl_gpu_name(app), mib);
	default: return false;
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

	char gpu[288], plat[160], mode[224], cur[48], out[80];
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
				 (int)(atomic_load(&g_ui.reticle_x) * pw),
				 (int)(atomic_load(&g_ui.reticle_y) * ph));
	} else
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
	else if (fnl_antialias(app) == FNL_AA_TAAU)
		snprintf(aa, sizeof aa, "TAAU   ");

	/* Under TAAU, what the resolve is told the scene moved -- the pair, and
	 * whether the scene's own clock is running at all, which is what makes
	 * the resolve's jitter move (F2). The pair reads even while the clock is
	 * off, since a caller setting one without the other is the mistake this
	 * is meant to be seen in. */
	char mo[64] = "";
	if (fnl_antialias(app) == FNL_AA_TAAU)
		snprintf(mo, sizeof mo, "Motion %g, %g%s",
				 (double)atomic_load(&g_motion_x),
				 (double)atomic_load(&g_motion_y),
				 atomic_load(&g_motion) ? "" : "  (F2 for the jitter)");

	snprintf(mode, sizeof mode, "%dx%d px   %s%s%s%s", f->area_w_px,
			 f->area_h_px, scene, aa, mo, fnl_present_mode(app));

	/* What this window draws of the pattern, and where it sits on the canvas
	 * a join shares: the one fact an echo and a row cannot carry, and the
	 * one a harness reads to tell a mirror from a join at a glance. */
	char view[96];
	{
		TestbedViewSpec v;
		testbed_scene_view(app, f, &v);
		snprintf(view, sizeof view, "View  %s   canvas %.0fx%.0f at %.0f,%.0f",
				 testbed_view_name(), (double)v.canvas[0], (double)v.canvas[1],
				 (double)v.origin[0], (double)v.origin[1]);
	}

	int x = MARGIN;
	/* status_panel_top() is where the *background* starts; the first row of
	 * text sits one pad below it. */
	int y = status_panel_top(app) + PANEL_PAD;

	/* On a panel of its own: the test pattern's colour bars run under this
	 * corner, and dim grey text on saturated red is not legible. */
	const char *rows[] = { gpu, plat, out, mode, view, cur };
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

#define CURSOR_ARM 11.0f /* points from centre to the end of an arm */
#define CURSOR_GAP 3.0f  /* points of clear space at the centre     */
#define CURSOR_THICK 1.5f

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

	const float x = (float)cx + (float)(atomic_load(&g_ui.reticle_x) * cw);
	const float y = (float)cy + (float)(atomic_load(&g_ui.reticle_y) * ch);

	/* Clipped to the area the crosshair moves in, so an arm at the very edge
	 * ends there rather than reaching over the titlebar or into the grab
	 * ring. No flush on either side: fnl_overlay_clip() issues what has been
	 * appended so far before it changes the scissor. The restore is what
	 * keeps the rest of the frame's text -- the library's caption included --
	 * out of this program's scissor. */
	fnl_overlay_clip(app, cx, cy, cw, ch);
	fnl_overlay_stroke(app, x - CURSOR_ARM, y, x - CURSOR_GAP, y, CURSOR_THICK,
					   COL_ACTIVE);
	fnl_overlay_stroke(app, x + CURSOR_GAP, y, x + CURSOR_ARM, y, CURSOR_THICK,
					   COL_ACTIVE);
	fnl_overlay_stroke(app, x, y - CURSOR_ARM, x, y - CURSOR_GAP, CURSOR_THICK,
					   COL_ACTIVE);
	fnl_overlay_stroke(app, x, y + CURSOR_GAP, x, y + CURSOR_ARM, CURSOR_THICK,
					   COL_ACTIVE);
	fnl_overlay_clip(app, 0, 0, 0, 0);
}

/* ---- the picture --------------------------------------------------------- */

/* A caller's image, drawn before everything else this file puts up and under
 * everything the library draws after on_overlay: its metrics panel, its
 * drop-down, the veil. It is here because it is the one overlay primitive
 * whose state outlives its own draw -- a picture left standing makes every
 * rectangle and stroke flushed later read the picture's rectangle as the
 * framebuffer's, so a regression shows in the panels -- and because a round
 * reaches the image path from nothing else: no other program the harness
 * drives makes one.
 *
 * On from the start, so that every environment and every variant draws one. A
 * renderer with no image path refuses the create, and then this says nothing
 * again. I destroys it and makes it again, which is the free path a round
 * cannot otherwise reach: nothing drawn after an exit could show it. */
#define PICTURE_PX 32

/* The picture is one per window, keyed by the app: each window has its own
 * device, and an image made on one is not drawable on another. A single
 * process-wide picture -- which is what this once was -- is drawn by the
 * second window against the first's device and faults the driver, which is
 * what two windows' render threads sharing one global found. A slot is a
 * window's own, and a window's overlay runs only on its render thread, so
 * two threads never touch the same one. Sized like the scenes' table. */
#define PICTURE_WINDOWS 8
typedef struct {
	FnlApp   *app;
	FnlImage *img;
	bool      on;
} Picture;
static Picture g_pictures[PICTURE_WINDOWS];
/* Two windows' overlays can first run at the same moment, each hunting a free
 * slot; this is the one shared thing they could both take. Held only across
 * the slot lookup, never the drawing. */
static atomic_flag g_pictures_lock = ATOMIC_FLAG_INIT;

/* This window's picture, made on the first ask (I can destroy it, and the next
 * frame makes it again). NULL when every slot is taken, which is more windows
 * than the testbed draws. */
static Picture *picture_for(FnlApp *app)
{
	Picture *found = NULL, *free_slot = NULL;
	while (atomic_flag_test_and_set(&g_pictures_lock))
		;
	for (int i = 0; i < PICTURE_WINDOWS && !found; i++) {
		if (g_pictures[i].app == app)
			found = &g_pictures[i];
		else if (!g_pictures[i].app && !free_slot)
			free_slot = &g_pictures[i];
	}
	if (!found && free_slot) {
		free_slot->app = app;
		free_slot->img = NULL;
		free_slot->on  = true;
		found          = free_slot;
	}
	atomic_flag_clear(&g_pictures_lock);
	return found;
}

/* White beside black, which no part of the scene is: the harness reads a row
 * across it and counts the white, so the picture answers whether it drew and
 * whether it is still there. Pure 0 and 255 are what a texture and an sRGB
 * surface leave alone whichever way the surface is written. */
static void picture_pixels(unsigned char *px)
{
	for (int y = 0; y < PICTURE_PX; y++)
		for (int x = 0; x < PICTURE_PX; x++) {
			unsigned char *p =
				px + ((size_t)y * (size_t)PICTURE_PX + (size_t)x) * 4u;
			const unsigned char v = ((x / 8) + (y / 8)) & 1 ? 0xFF : 0x00;
			p[0]                  = v;
			p[1]                  = v;
			p[2]                  = v;
			p[3]                  = 0xFF;
		}
}

/* Centred vertically, since the middle row is the one the harness reads, its
 * left edge at 60 % of the width, and square: what is beside it is the scene
 * the row is otherwise about. */
static void picture_draw(FnlApp *app)
{
	Picture *p = picture_for(app);
	if (!p || !p->on)
		return;
	if (!p->img) {
		unsigned char px[PICTURE_PX * PICTURE_PX * 4];
		picture_pixels(px);
		p->img =
			fnl_image_create(app, PICTURE_PX, PICTURE_PX, FNL_IMAGE_RGBA8, px);
		if (!p->img) { /* a renderer that cannot draw a picture */
			p->on = false;
			return;
		}
	}
	int w_pt, h_pt;
	fnl_window_size_pt(app, &w_pt, &h_pt);
	const int side = h_pt / 4 > 16 ? h_pt / 4 : 16;
	fnl_overlay_image(app, w_pt * 6 / 10, (h_pt - side) / 2, side, side,
					  p->img);
}

/* I: this window's picture off, and its image destroyed; the next press on
 * with the image made again on this window's device. */
static void picture_toggle(FnlApp *app)
{
	Picture *p = picture_for(app);
	if (!p)
		return;
	p->on = !p->on;
	if (!p->on && p->img) {
		fnl_image_destroy(p->img);
		p->img = NULL;
	}
}

/* The window is gone: its slot must not outlive it. A closed window's app can
 * be freed and the next window handed the same address, and the slot would
 * then hand that window an image made on the destroyed device. The image is
 * the library's to free at fnl_shutdown(); this only forgets the slot. */
void testbed_overlay_picture_forget(FnlApp *app)
{
	while (atomic_flag_test_and_set(&g_pictures_lock))
		;
	for (int i = 0; i < PICTURE_WINDOWS; i++)
		if (g_pictures[i].app == app) {
			g_pictures[i].app = NULL;
			g_pictures[i].img = NULL;
			g_pictures[i].on  = true;
		}
	atomic_flag_clear(&g_pictures_lock);
}

/* ---- the shapes panel ----------------------------------------------------
 *
 * The four filled forms, drawn large enough that a picture of them says
 * whether they are there and whether the corners, the edge and the ramp are
 * where they should be. O toggles it; a harness that can read pixels reads
 * the row across the middle.
 *
 * Every one of them is a *background* in the library's sense -- a filled
 * rectangle blends over what is under it -- so what must sit on top (the
 * labels) is flushed first, the same rule the rest of this file keeps. */
static void shapes_draw(FnlApp *app)
{
	if (!atomic_load(&g_ui.shapes))
		return;

	int cx, cy, cw, ch;
	fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);

	/* A column down the left of the area, wide enough for a row of four and
	 * clear of the panels on the right. */
	const int pad  = 20;
	const int cell = ch / 6 > 40 ? ch / 6 : 40;
	const int x    = cx + pad;
	int y          = cy + pad;

	const FnlColor ink = { 0.93f, 0.95f, 0.98f, 1.0f };
	const FnlColor a1  = { 1.00f, 0.45f, 0.30f, 1.0f };
	const FnlColor a2  = { 0.25f, 0.55f, 1.00f, 1.0f };

	/* 1. The wash: a colour with alpha below one, over the panel behind it.
	 *    What an opaque write would have made a hole instead. */
	const FnlColor wash = { 1.00f, 0.45f, 0.30f, 0.35f };
	fnl_overlay_rect_blend(app, x, y, cell * 3, cell, wash);

	/* 2. The rounded corner, at a radius a third of the side so the corners
	 *    are plainly round and the middle is plainly flat. */
	fnl_overlay_rect_round(app, x, y + cell + 8, cell * 3, cell,
						   (float)cell / 3.0f, a1);

	/* 3. The disc, at a centre in points so a fraction of a pixel is
	 *    expressible. */
	fnl_overlay_circle(app, (float)x + (float)cell * 1.5f,
					   (float)(y + 2 * (cell + 8)) + (float)cell * 0.5f,
					   (float)cell * 0.5f, a2);

	/* 4. The gradient, left to right at 0 degrees: the first colour at the
	 *    left edge and the second at the right. */
	fnl_overlay_gradient(app, x, y + 3 * (cell + 8), cell * 3, cell, 0.0f, a1,
						 a2);

	/* The names, one to a row, in the ink the rest of the testbed uses. */
	const int ty = y + 4 * (cell + 8);
	fnl_overlay_text(app, x, ty, FNL_TEXT_BODY, ink, "blend");
	fnl_overlay_text(app, x, ty + fnl_text_line(FNL_TEXT_BODY), FNL_TEXT_BODY,
					 ink, "round");
	fnl_overlay_text(app, x, ty + 2 * fnl_text_line(FNL_TEXT_BODY),
					 FNL_TEXT_BODY, ink, "circle");
	fnl_overlay_text(app, x, ty + 3 * fnl_text_line(FNL_TEXT_BODY),
					 FNL_TEXT_BODY, ink, "gradient");
	(void)cw;
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
	const double ms  = fnl_frame_gpu_ms(app);
	const int fps    = fnl_fps(app);
	const double gpu = ms >= 0.0 && fps >= 0 ? ms * fps / 10.0 : -1.0;
	fprintf(stderr,
			"footprint: CPU %.1f %%, RAM %.0f MiB, VRAM %.0f MiB, "
			"frame CPU %.2f ms, GPU %.1f %%, on %s with %s\n",
			cpu, ram, vram, fnl_frame_cpu_ms(app), gpu, fnl_gpu_name(app),
			fnl_renderer_name(app));
}

/* From on_overlay, after the events. The overlay's ordering rule is this
 * file's to keep: every filled rectangle above is a background, drawn before
 * any stroke or text. */
void testbed_overlay_draw(FnlApp *app, const FnlFrame *f)
{
	bool help = false, status = false;

	/* First of all: what is drawn after it is what a push constant left
	 * standing would reach (see the picture above). */
	picture_draw(app);
	/* And the four filled forms, where O asked for them. Before the panels,
	 * for the picture's reason: a push constant left standing would reach
	 * whatever is drawn next, and the shapes' own pipelines are what a
	 * leftover would show against. */
	shapes_draw(app);
	if (atomic_load(&g_ui.show_help)) {
		/* Two give-ways, in order: the status readout yields to the titlebar,
		 * and the keybindings yield to whichever of the two is below them. See
		 * status_panel_fits() and help_panel_fits(). */
		status = status_panel_fits(app);
		help   = help_panel_fits(app, status);
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
	if (g_frame_cpu_log) {
		static double at = -1.0;
		if (at < 0.0 || f->time - at >= 0.5) {
			at = f->time;
			fprintf(stderr, "frame cpu: %.2f ms, fps %d\n",
					fnl_frame_cpu_ms(app), fnl_fps(app));
		}
	}
	if (g_reserve_log) {
		static double at = -1.0;
		if (at < 0.0 || f->time - at >= 0.5) {
			at = f->time;
			int cx, cy, cw, ch, rx, ry, rw, rh;
			bool vis;
			fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);
			(void)cx;
			(void)cy;
			(void)ch;
			/* 120 points: a caller's run wide enough to be worth asking
			 * about, and not so wide that only a large window shows it. */
			vis = fnl_titlebar_reserve(app, 120, &rx, &ry, &rw, &rh);
			if (vis)
				fprintf(stderr, "reserve: win %d -> x %d y %d %dx%d\n", cw, rx,
						ry, rw, rh);
			else
				fprintf(stderr, "reserve: win %d -> not visible\n", cw);
		}
	}
	if (g_frame_stats_log) {
		static double at = -1.0;
		static FnlFrameSample rows[1024];
		static unsigned long long n_rows, presented, rows_us, lost_all;
		if (!atomic_load(&g_stats_on)) {
			fnl_set_frame_stats(app, true);
			atomic_store(&g_stats_on, true);
		}
		if (at < 0.0 || f->time - at >= 0.5) {
			at            = f->time;
			uint64_t lost = 0;
			const int got = fnl_frame_samples(
				app, rows, (int)(sizeof rows / sizeof rows[0]), sizeof rows[0],
				&lost);
			lost_all += lost;
			for (int i = 0; i < got; i++) {
				n_rows++;
				if (rows[i].presented)
					presented++;
				if (rows[i].gpu_ms >= 0.0f)
					rows_us +=
						(unsigned long long)(rows[i].gpu_ms * 1e3f + 0.5f);
			}
			FnlFrameStats st = { .size = sizeof st };
			const bool have  = fnl_frame_stats(app, &st);
			fprintf(stderr,
					"frame stats: frames %llu presents %llu, gpu %llu us "
					"in %llu; rows %llu gpu %llu us, presented %llu, lost "
					"%llu; cpu %.2f gpu %.2f ms\n",
					have ? (unsigned long long)st.frames : 0ULL,
					have ? (unsigned long long)st.presents : 0ULL,
					have ? (unsigned long long)st.gpu_us : 0ULL,
					have ? (unsigned long long)st.gpu_samples : 0ULL, n_rows,
					rows_us, presented, lost_all, fnl_frame_cpu_ms(app),
					fnl_frame_gpu_ms(app));
		}
	}
	draw_reticle(app);
}

/* From the event drain in main.c. The library has already acted on whatever
 * it binds; this adds the testbed's own. */
/* The scene's motion, echoed when it changes: F2 is the library's binding,
 * so the testbed never sees the key itself, and a harness driving to a still
 * window has nothing else to tell it the state. -1 until the first event. */
static _Atomic int g_motion_echo = -1;

void testbed_overlay_event(FnlApp *app, const FnlEvent *ev)
{
	const bool moving = fnl_scene_motion(app);
	/* The exchange is what makes two windows' drains echo it once: the one
	 * that found the old value different printed it. */
	const int echo = moving ? 1 : 0;
	if (atomic_exchange(&g_motion_echo, echo) != echo)
		fprintf(stderr, "scene motion: %s\n", moving ? "on" : "off");
	atomic_store(&g_motion, moving);
	switch (ev->type) {
	case FNL_EVENT_KEY:
		/* F1 toggles a panel the library cannot see, so nothing in what it
		 * compares between frames moves with the flag: the invalidate is what
		 * gets the frame re-recorded. Not on a repeat -- there are none
		 * unless text entry is on, and then a held F1 would flicker the
		 * panel. */
		if (ev->key.pressed && !ev->key.repeat && ev->key.key == FNL_KEY_F1) {
			atomic_store(&g_ui.show_help, !atomic_load(&g_ui.show_help));
			fnl_invalidate(app);
		}
		/* F3 switches text entry, so a harness can type at this program and
		 * still close it with Escape afterwards. Nothing is drawn for it. */
		if (ev->key.pressed && !ev->key.repeat && ev->key.key == FNL_KEY_F3)
			fnl_set_text_input(app, !fnl_is_text_input(app));
		/* F8: the scene's motion vector, in steps of MOTION_STEP, and back
		 * again with Shift. F2 is the library's and never reaches here, so
		 * the vector and the scene's clock are two presses, as they are two
		 * calls -- and a vector under a clock that is off is a reprojection
		 * the resolve does not read, which is why the status line names both
		 * while TAAU is in force. */
		if (ev->key.pressed && !ev->key.repeat && ev->key.key == FNL_KEY_F8) {
			motion_step(fnl_key_down(app, FNL_KEY_LSHIFT) ||
							fnl_key_down(app, FNL_KEY_RSHIFT)
						? -MOTION_STEP
						: MOTION_STEP);
			const float mx = atomic_load(&g_motion_x);
			const float my = atomic_load(&g_motion_y);
			fnl_set_scene_motion_px(app, mx, my);
			fprintf(stderr, "scene motion px: %g, %g\n", (double)mx,
					(double)my);
			fnl_invalidate(app);
		}
		/* The clipboard, one key each way: F4 offers a string a harness can
		 * recognise and F6 asks for whatever is there, which comes back as
		 * FNL_EVENT_CLIPBOARD. Both in answer to a key press, which is the
		 * only time Wayland allows it. */
		if (ev->key.pressed && !ev->key.repeat && ev->key.key == FNL_KEY_F4)
			fnl_clipboard_set(app, TESTBED_CLIPBOARD_TEXT);
		if (ev->key.pressed && !ev->key.repeat && ev->key.key == FNL_KEY_F6)
			fnl_clipboard_request(app);
		/* F7 asks for the output after this one, where fullscreen hides the
		 * bar the output box is on: a harness's way to move a fullscreen
		 * window. */
		if (ev->key.pressed && !ev->key.repeat && ev->key.key == FNL_KEY_F7) {
			const int n     = fnl_output_count(app);
			const char *cur = fnl_output_name(app);
			int idx         = -1;
			for (int i = 0; i < n && idx < 0; i++) {
				const char *name = fnl_output_name_at(app, i);
				const size_t len = strlen(name);
				/* The list is by connector; the window's output is that and,
				 * on Wayland, the model after it. */
				if (len > 0 && strncmp(cur, name, len) == 0 &&
					(cur[len] == '\0' || cur[len] == ' '))
					idx = i;
			}
			if (n > 0)
				fnl_request_output(app, fnl_output_name_at(app, (idx + 1) % n));
		}
		/* I: the picture off and on, as examples/image.c binds it. The
		 * destroy is the case a round cannot otherwise reach -- the free path
		 * mid-run, not at exit -- and making it again is the create path after
		 * a device that has already uploaded one. */
		if (ev->key.pressed && !ev->key.repeat && ev->key.key == FNL_KEY_I) {
			picture_toggle(app);
			fnl_invalidate(app);
		}
		/* O: the shapes panel -- the wash, the rounded corner, the disc and
		 * the gradient. Nothing else in the tree draws them, so this is the
		 * only way a round can be shown to draw them. */
		if (ev->key.pressed && !ev->key.repeat && ev->key.key == FNL_KEY_O) {
			atomic_store(&g_ui.shapes, !atomic_load(&g_ui.shapes));
			fnl_invalidate(app);
		}
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
		(void)cx;
		(void)cy;
		double rx = atomic_load(&g_ui.reticle_x);
		double ry = atomic_load(&g_ui.reticle_y);
		if (cw > 0)
			rx += ev->relative.dx / cw;
		if (ch > 0)
			ry += ev->relative.dy / ch;
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

	default: break;
	}
}

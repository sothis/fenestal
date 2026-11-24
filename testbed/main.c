/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The testbed, as a Fenestal client: the command line, the licence text, and
 * the calls that open a window and run it. It includes exactly one header of
 * Fenestal's, and that header pulls in neither a window system nor a graphics
 * API -- which is why this file is separate from the drawing.
 *
 * Everything on screen but the titlebar is this program's, in the files
 * beside this one: the test pattern in scene_vulkan.c, and again in
 * scene_gl.c, scene_d3d12.c and scene_metal.m for the other renderers; the
 * keybindings panel, the status readout and the crosshair in overlay.c; and
 * a replacement titlebar, drawn only with --custom-titlebar, in
 * titlebar_custom.c. The library's own titlebar -- window buttons,
 * checkboxes, size drop-down -- it draws itself.
 */

#include <fenestal/fenestal.h>

/* The scenes' callbacks and the testbed's own drawing. The header names no
 * graphics API's types, so this file learns which renderer draws only from
 * fnl_renderer(), at run time. */
#include "testbed.h"
#include "bench.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <time.h>   /* nanosleep, for --fps's pacing */
#endif

#ifdef _WIN32
/* The one Windows header this file includes, for use_parent_console(). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* Set by --text-input: text entry on from the first frame, so that a harness
 * can type at this program and read back what the layout made of it. */
static bool text_input;
/* --transition=MS, in milliseconds; -1 leaves the library's own. */
static int trans_ms = -1;
/* --freeze: the scene drawn at a fixed clock while the library's own clock
 * runs, so a temporal accumulation's jitter still advances and gathers while
 * the picture holds one pose -- the only way MSAA, MLAA and TAAU frame the
 * same star to compare. Read by testbed_scene_time(). */
static bool g_freeze;
static float g_freeze_t = 0.6f; /* a pose with the wedges off the axes */
/* --fps=N: hold the window to N presents a second by sleeping out the rest of
 * each frame's budget, so a scene that would draw every refresh moves further
 * between frames -- what "30 sizes a second" means to a temporal accumulation,
 * whose clamp sees twice the displacement at 30 that it does at 60. 0 is the
 * display's own rate, the default. */
static int g_fps;
static double g_fps_next;
/* The second window on this host: opened by --second-window, or by F10, which
 * shows and hides it. g_second is the app while it is open, NULL while it is
 * not. It draws a picture of its own -- a mirror, a joined part of the first's
 * canvas, or its own scene -- and F12 cycles the three (testbed_scene_view()
 * says what each draws). The keys arrive on the render thread and window
 * creation and shutdown are the main thread's, so a key only records what was
 * asked for in g_second_want, and the run's loop below creates or destroys the
 * window on its own thread. */
static bool g_second_window; /* --second-window: open it at startup */
static FnlApp *g_second;     /* the second window's app, main thread's own */

/* What the keys have asked the loop for: -1 leave the second window as it is,
 * 0 hide it, 1 show it. Written from a key on the render thread and spent on
 * the main thread, so atomic; a press either way lands once. */
static _Atomic int g_second_want = -1;
/* Whether a second window is open, read by every window's view (which the
 * render threads compute) and written by the loop that opens and closes it.
 * An int, so a scene on a render thread can tell a joined picture from a
 * window that is alone. g_second_atomic is the second window's app itself,
 * for the same cross-thread read: g_second is the main thread's own handle to
 * it and is written only there. */
static _Atomic int g_second_open;
static _Atomic(FnlApp *) g_second_atomic;
/* How the second window is to draw, as F12 cycles it: a mirror, joined to the
 * first's canvas, or its own scene. Read by every window's scene_*.c through
 * testbed_scene_view(); written by the key and by nothing else. */
static _Atomic int g_view_mode;

/* A JOIN lays the pattern on a canvas as wide as the first window twice over,
 * so the first shows its left half and the second its right and the seam falls
 * at the first window's own width: the star sits on the seam and the sweep
 * crosses it, which is what tells a joined picture from two mirrors. The first
 * window publishes its scene size for the second to place itself against; one
 * frame stale during a resize, which a testbed does not care about. */
static _Atomic int g_join_w, g_join_h;

/* The --bench* arguments, in order, for bench.c; and the first option given
 * beside them that a benchmark row sets instead, which is refused: a record
 * names every option it ran under, and one set behind its back would not
 * be. */
static char **bench_args;
static int bench_count;
static const char *bench_clash;

/* ---- one scene per renderer ----------------------------------------------
 *
 * The build compiles a scene file for each renderer the library has and says
 * so by SCENE_HAS_<API>; the mask made from this table tells fnl_init() which
 * it may choose, and fnl_renderer() which it did. Chosen again at every
 * on_gpu_created, the first moment the answer is known. Only the render
 * thread, or fnl_run()'s before it, is ever inside these, never two at
 * once, so the choice needs no lock. */
typedef struct {
	FnlRenderer renderer;
	void (*on_gpu_created)(void *ud, FnlApp *app);
	void (*on_gpu_lost)(void *ud, FnlApp *app);
	void (*on_record)(void *ud, FnlApp *app, const FnlFrame *frame);
} Scene;

#if !defined(SCENE_HAS_VULKAN) && !defined(SCENE_HAS_GL) &&                    \
	!defined(SCENE_HAS_D3D12) && !defined(SCENE_HAS_METAL)
#error "no scene: the build defines SCENE_HAS_<API> for each one it compiles"
#endif

static const Scene SCENES[] = {
#ifdef SCENE_HAS_VULKAN
	{ FNL_RENDERER_VULKAN, scene_vulkan_on_gpu_created,
	  scene_vulkan_on_gpu_lost, scene_vulkan_on_record },
#endif
#ifdef SCENE_HAS_GL
	{ FNL_RENDERER_OPENGL, scene_gl_on_gpu_created, scene_gl_on_gpu_lost,
	  scene_gl_on_record },
#endif
#ifdef SCENE_HAS_D3D12
	{ FNL_RENDERER_D3D12, scene_d3d12_on_gpu_created, scene_d3d12_on_gpu_lost,
	  scene_d3d12_on_record },
#endif
#ifdef SCENE_HAS_METAL
	{ FNL_RENDERER_METAL, scene_metal_on_gpu_created, scene_metal_on_gpu_lost,
	  scene_metal_on_record },
#endif
};
#define SCENE_COUNT (sizeof SCENES / sizeof SCENES[0])

static const Scene *scene; /* the one drawing, or NULL */

static uint32_t scene_renderers(void)
{
	uint32_t renderers = 0;
	for (size_t i = 0; i < SCENE_COUNT; i++)
		renderers |= FNL_RENDERER_BIT(SCENES[i].renderer);
	return renderers;
}

static void scene_on_gpu_created(void *ud, FnlApp *app)
{
	const FnlRenderer r = fnl_renderer(app);
	scene               = NULL;
	for (size_t i = 0; i < SCENE_COUNT; i++)
		if (SCENES[i].renderer == r)
			scene = &SCENES[i];
	if (scene)
		scene->on_gpu_created(ud, app);
	else
		fprintf(stderr, "no scene for %s; none will be drawn\n",
				fnl_renderer_name(app));
}

static void scene_on_gpu_lost(void *ud, FnlApp *app)
{
	if (scene)
		scene->on_gpu_lost(ud, app);
	testbed_gpu_mem_os_release();
}

static void scene_on_record(void *ud, FnlApp *app, const FnlFrame *f)
{
	if (scene)
		scene->on_record(ud, app, f);
}

float testbed_scene_time(const FnlFrame *frame)
{
	return g_freeze ? g_freeze_t : frame->scene_time;
}

/* The first window publishes the scene size a join is measured against, once
 * a frame, so the second window's half starts where the first's ends. Written
 * only by the first window's own frame -- the second never overwrites it -- and
 * read by either window's view. */
static void join_publish(FnlApp *app, const FnlFrame *frame)
{
	if (app != atomic_load(&g_second_atomic)) {
		atomic_store(&g_join_w, frame->scene_w_px);
		atomic_store(&g_join_h, frame->scene_h_px);
	}
}

void testbed_scene_view(FnlApp *app, const FnlFrame *frame,
						TestbedViewSpec *v)
{
	memset(v, 0, sizeof *v);
	const TestbedView kind = (TestbedView)atomic_load(&g_view_mode);
	/* The second window is the one these kinds are about; the first is a
	 * plain mirror of the pattern whatever the second draws, unless the kind
	 * is JOIN, where both windows share one canvas. That is what lets one key
	 * make the three kinds visible: F12 says how the second draws, and the
	 * first follows only for a join. Asked of the atomic rather than g_second,
	 * which this runs beside: a render thread, where g_second is not its
	 * own. */
	const bool second = app == atomic_load(&g_second_atomic);

	v->kind    = kind;
	v->size[0] = (float)frame->scene_w_px;
	v->size[1] = (float)frame->scene_h_px;
	v->tint[0] = v->tint[1] = v->tint[2] = v->tint[3] = 1.0f;

	switch (kind) {
	case TESTBED_VIEW_JOIN: {
		/* One canvas two first-windows wide: the first window shows its
		 * left half, the second its right, so the seam falls at the first
		 * window's own width and the star sits on it. A lone first window
		 * -- no second open, or its size not yet published -- shows the
		 * whole of a canvas the width of one window, which is the pattern
		 * as a mirror. */
		const int w1   = atomic_load(&g_join_w);
		const int hh   = atomic_load(&g_join_h);
		const bool two = atomic_load(&g_second_open);
		const float cw = w1 > 0 ? (float)w1 : v->size[0];
		v->canvas[0]   = two ? cw * 2.0f : cw;
		v->canvas[1]   = hh > 0 ? (float)hh : v->size[1];
		v->origin[0]   = (two && second) ? cw : 0.0f;
		break;
	}
	case TESTBED_VIEW_OWN:
		/* A window's own canvas is its own size, so no part of it is
		 * another's; the second is additionally a different colour and a
		 * little out of phase, so echoing a row or reading a pixel tells the
		 * two apart without the title. The first keeps the pattern's own
		 * colours and clock, so a mirror of the first is exactly what the
		 * testbed drew before this was a choice. */
		v->canvas[0] = v->size[0];
		v->canvas[1] = v->size[1];
		if (second) {
			v->tint[0]   = 0.75f;
			v->tint[1]   = 0.55f;
			v->tint[2]   = 1.00f;
			v->time_bias = 3.7f;
		}
		break;
	case TESTBED_VIEW_MIRROR:
	default:
		v->kind      = TESTBED_VIEW_MIRROR;
		v->canvas[0] = v->size[0];
		v->canvas[1] = v->size[1];
		break;
	}
}

/* How the first window draws under the current kind, for the status readout:
 * a joined first window is a part of one picture and says so. */
const char *testbed_view_name(void)
{
	switch ((TestbedView)atomic_load(&g_view_mode)) {
	case TESTBED_VIEW_JOIN: return "join";
	case TESTBED_VIEW_OWN: return "own";
	case TESTBED_VIEW_MIRROR:
	default: return "mirror";
	}
}

static void print_licence(void)
{
	puts("The overlay is drawn with Fenestal Glyphs Sans and Mono, glyph");
	puts("coverage baked from Ubuntu Sans and Ubuntu Sans Mono. That baked");
	puts("data is a renamed Modified Version of the font software below, and");
	puts("travels under the same licence. The rest of this program is not");
	puts("covered by it: it is Fenestal's, under the MIT licence, with the");
	puts("other notices in LICENSE and THIRD_PARTY_NOTICES.md.");
	puts("");
	fputs(fnl_font_notice(), stdout);
	puts("");
	fputs(fnl_font_licence(), stdout);
}

static void usage(FILE *f, const char *prog)
{
	fprintf(
		f,
		"usage: %s [options]\n"
		"\n"
		"Late latching holds a frame's state until most of the display period\n"
		"it will sit through has already passed, so what reaches the screen "
		"is\n"
		"that much newer (see DESIGN.md). It is on by default and costs\n"
		"nothing measurable; the first two turn it down or off.\n"
		"\n"
		"  --no-late-latch    sample each frame's state at the top of its\n"
		"                     turn, with no delay before it\n"
		"  --late-latch=<ms>  hold the delay at <ms> instead of sizing it\n"
		"                     per frame from what the last one had spare\n"
		"\n"
		"  --scale=<n>        run at surface scale <n> (1 to 8) instead of\n"
		"                     the one the compositor asks for -- a way to see\n"
		"                     the client at a density this desktop does not\n"
		"                     have. The window keeps the size it would have\n"
		"                     had; the buffer behind it grows by <n>, so a\n"
		"                     high scale costs proportionally more memory --\n"
		"                     and is cut back to whatever the device will\n"
		"                     allocate, which the status line then reports.\n"
		"                     Without wp_viewporter a fraction rounds up, as\n"
		"                     it must for an integer buffer scale.\n"
		"\n"
		"  --gpu=<n>          run on device <n> instead of the automatic\n"
		"                     choice, one-based over the devices that can\n"
		"                     present to this window in the order the driver\n"
		"                     enumerates them -- which is not the order the\n"
		"                     automatic choice ranks them in, so 1 need not\n"
		"                     be the device it would have picked. The library\n"
		"                     logs one line per device at startup with the\n"
		"                     number to pass; the status readout names what\n"
		"                     was actually got. On a hybrid laptop the\n"
		"                     fastest GPU and the one wired to the display\n"
		"                     are different devices, and which one a window\n"
		"                     presents from is worth measuring both ways.\n"
		"  --gpu=<name>       the same by any part of the device's name,\n"
		"                     letter case aside: --gpu=intel. The number is\n"
		"                     not one device across programs on such a\n"
		"                     laptop; the name is. FNL_GPU in the\n"
		"                     environment is read the same way when the\n"
		"                     option is not given.\n"
		"  --layout=<spec>    on KMS, type through this layout instead of\n"
		"                     the console's (/etc/default/keyboard or\n"
		"                     /etc/vconsole.conf): <layout>[(<variant>)],\n"
		"                     more of them comma-separated for more\n"
		"                     groups, and XKB options after a space --\n"
		"                     --layout=de(nodeadkeys), or\n"
		"                     --layout='de,us caps:none'. Read by the KMS\n"
		"                     backend alone: under a window server the\n"
		"                     layout is the session's, as it should be.\n"
		"                     FNL_KMS_LAYOUT in the environment is the\n"
		"                     same.\n"
		"  --gpu-driver=<d>   on KMS, offer a move of an NVIDIA GPU to its\n"
		"                     other kernel driver -- nouveau or NVIDIA's own\n"
		"                     -- with the window open: the GPU box lists\n"
		"                     it as \"<GPU>, on <driver>\". <d> nouveau or\n"
		"                     nvidia also binds that one before the first\n"
		"                     frame; \"switch\" leaves it as it is. Wants\n"
		"                     tools/gpu-driver.sh installed, behind its\n"
		"                     socket or a sudoers rule (tools/drive/\n"
		"                     bare_driver.py --setup). FNL_GPU_DRIVER in\n"
		"                     the environment is the same.\n"
		"  --gpu-rank=<r>     the GPU box ranked by measured frame rate,\n"
		"                     kept in the user's cache, which orders the\n"
		"                     box and picks the GPU at start; measured at\n"
		"                     the first start and on new hardware.\n"
		"                     calibrate: measure again now; keep: use it,\n"
		"                     never measure; order: order the box by it\n"
		"                     and start as without; off: use none.\n"
		"                     FNL_GPU_RANK in the environment is the same.\n"
		"\n"
		"  --custom-titlebar  draw the titlebar in this program instead of\n"
		"                     the library's, buttons included\n"
		"  --bare-titlebar    the library's bar with none of its controls\n"
		"                     in the library: a taller bar with its own four\n"
		"                     buttons, claimed with fnl_titlebar_claim() so\n"
		"                     a press on one reaches this program while the\n"
		"                     rest of the strip still drags the window. The\n"
		"                     library's own checkboxes and size drop-down are\n"
		"                     not drawn in this mode -- they live in its bar,\n"
		"                     and there is not one.\n"
		"\n"
		"  --echo-events      print every event the library delivers, one per\n"
		"                     line, on stderr, for driving this program from\n"
		"                     a test harness. The echo is the library's own\n"
		"                     (FnlInitDesc::echo_events); setting\n"
		"                     FNL_ECHO_EVENTS=1 in the environment does the\n"
		"                     same for any program built on it.\n"
		"\n"
		"  --second-window    open a second window on the same host and run\n"
		"                     it beside the first, drawing a picture of its\n"
		"                     own. F10 shows and hides it; F12 cycles what it\n"
		"                     draws -- a mirror of this window, a joined part\n"
		"                     of one shared canvas, or its own scene. A\n"
		"                     backend that does not route a second window\n"
		"                     refuses it, and F10 then says so and nothing\n"
		"                     opens.\n"
		"\n"
		"  --footprint-log    print this process's CPU, RAM and VRAM -- the\n"
		"                     metrics panel's figures -- to stderr twice a\n"
		"                     second, \"footprint: ...\", for a harness that\n"
		"                     cannot read the screen. It asks for cost\n"
		"                     sampling, which counts the frames the panel\n"
		"                     draws at rest; --frame-cpu-log does not.\n"
		"  --frame-cpu-log    print the per-frame CPU figure\n"
		"                     (fnl_frame_cpu_ms()) to stderr twice a second,\n"
		"                     \"frame cpu: ...\", and nothing else -- the\n"
		"                     panel's own figures at rest.\n"
		"  --frame-stats-log  print the frame loop's totals and the rows they\n"
		"                     are made of, twice a second, \"frame stats:\n"
		"                     ...\", with the two figures the means are kept\n"
		"                     from; turns frame-stats counting on.\n"
		"  --reserve-log      print fnl_titlebar_reserve()'s answer for a\n"
		"                     caller's run, with the window's width, twice a\n"
		"                     second: \"reserve: ...\".\n"
		"  --shapes           the shapes panel on from the first frame, where\n"
		"                     O toggles it: the wash, the rounded rectangle,\n"
		"                     the disc and the gradient, for a round that "
		"reads\n"
		"                     pixels rather than pressing keys.\n"
		"  --freeze           draw the scene at a fixed clock while the\n"
		"                     library's clock runs, so a temporal\n"
		"                     accumulation still gathers its subpixel samples\n"
		"                     while the picture holds one pose -- what lets\n"
		"                     MSAA, MLAA and TAAU frame the same scene to\n"
		"                     compare. Press F2 for the clock.\n"
		"  --fps=N            hold the window to N presents a second,\n"
		"                     sleeping out the rest of each frame, so a\n"
		"                     moving scene advances further between frames --\n"
		"                     30 is twice the displacement per frame the\n"
		"                     temporal clamp sees at 60.\n"
		"  --width=PT         open the window at this presentable width in\n"
		"                     logical points, for capturing the titlebar\n"
		"                     where a control gives way.\n"
		"\n"
		"  --text-input       switch text entry on at startup. With\n"
		"                     --echo-events each character a key types is a\n"
		"                     line of its own, under the layout in force, and\n"
		"                     a held key repeats. Escape then no longer\n"
		"                     closes the window, which is what the mode is\n"
		"                     for, so close it from the titlebar.\n"
		"\n"
		"  --frame-times      report where each frame's wall clock went, a\n"
		"                     line each half second on stderr: the pacing,\n"
		"                     the wait on the device, the recording and the\n"
		"                     present. The number worth reading is `wait` --\n"
		"                     the CPU sitting out work the GPU already has,\n"
		"                     which is where a frame that is slow on the GPU\n"
		"                     is slow and nowhere else.\n"
		"\n"
		"  --no-vsync         draw without vsync, flat out, for measuring;\n"
		"                     FNL_VSYNC=0 in the environment does the same\n"
		"                     for any program built on the library.\n"
		"\n"
		"  --transition=MS    how long the first frames after a move to\n"
		"                     another GPU, renderer or kernel driver dissolve\n"
		"                     from the last frame before it, in milliseconds;\n"
		"                     the library's own figure is 250. 0 is a cut,\n"
		"                     for a capture taken right after a move.\n"
		"\n"
		"  --bench            the benchmark mode: a panel of its\n"
		"                     options, then a run in a window of its own,\n"
		"                     and its results, written as a record\n"
		"                     (docs/benchmark-mode.md). --bench-run starts\n"
		"                     at once, --bench-<key>=<value> sets an\n"
		"                     option, --bench-replay=<record> takes a\n"
		"                     record's, --bench-list names them all.\n"
		"\n"
		"  --licence          the Ubuntu Font Licence and copyright notice,\n"
		"                     which the baked glyphs are distributed under\n"
		"  -h, --help         this message\n",
		prog);
}

/* Fills in the descriptor the library is initialised from. Returns false when
 * the program should stop -- either because something was wrong, or because
 * --help and --licence have already done everything they were asked to. */
static bool parse_args(FnlInitDesc *d, int argc, char **argv, int *status)
{
	const char *prog = (argc > 0 && argv[0]) ? argv[0] : "fenestal-testbed";
	static const char PIN[]        = "--late-latch=";
	static const char SCALE[]      = "--scale=";
	static const char GPU[]        = "--gpu=";
	static const char DRIVER[]     = "--gpu-driver=";
	static const char RANK[]       = "--gpu-rank=";
	static const char LAYOUT[]     = "--layout=";
	static const char TRANSITION[] = "--transition=";

	*status = EXIT_SUCCESS;

	/* The options a benchmark row sets in its own way, or that its run
	 * never reads, refused beside --bench*: the run would not record them,
	 * and the second kind would be dropped without a word. */
	static const char *const CLASH[] = {
		"--no-late-latch", "--late-latch=",     "--scale=",
		"--gpu=",          "--custom-titlebar", "--text-input",
		"--frame-times",   "--no-vsync",        "--transition=",
		"--footprint-log", "--frame-cpu-log",   "--frame-stats-log",
		"--reserve-log",   "--shapes",
	};

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!strncmp(arg, "--bench", 7) && (arg[7] == '\0' || arg[7] == '-')) {
			if (!bench_args) {
				bench_args = calloc((size_t)argc, sizeof *bench_args);
				if (!bench_args) {
					*status = EXIT_FAILURE;
					return false;
				}
			}
			bench_args[bench_count++] = argv[i];
			continue;
		}
		for (size_t k = 0; k < sizeof CLASH / sizeof CLASH[0] && !bench_clash;
			 k++)
			if (!strncmp(arg, CLASH[k], strlen(CLASH[k])))
				bench_clash = arg;

		if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			usage(stdout, prog);
			return false;
		}
		/* Both spellings: the licence is British-spelled and so is the flag,
		 * but nobody should have to know that to read it. */
		if (!strcmp(arg, "--licence") || !strcmp(arg, "--license")) {
			print_licence();
			return false;
		}
		if (!strcmp(arg, "--no-late-latch")) {
			d->no_late_latch = true;
			continue;
		}
		if (!strcmp(arg, "--custom-titlebar")) {
			testbed_titlebar_custom_enable();
			continue;
		}
		if (!strcmp(arg, "--bare-titlebar")) {
			/* The library's bar with none of its own controls in it: the
			 * buttons, the drag and the edges are still the window's. */
			d->no_titlebar_controls = FNL_TITLEBAR_ALL;
			continue;
		}
		if (!strcmp(arg, "--echo-events")) {
			d->echo_events = true;
			continue;
		}
		if (!strcmp(arg, "--second-window")) {
			g_second_window = true;
			continue;
		}
		if (!strcmp(arg, "--footprint-log")) {
			testbed_overlay_footprint_log();
			continue;
		}
		if (!strcmp(arg, "--frame-cpu-log")) {
			testbed_overlay_frame_cpu_log();
			continue;
		}
		if (!strcmp(arg, "--frame-stats-log")) {
			testbed_overlay_frame_stats_log();
			continue;
		}
		if (!strcmp(arg, "--reserve-log")) {
			testbed_overlay_reserve_log();
			continue;
		}
		if (!strcmp(arg, "--shapes")) {
			testbed_overlay_shapes_on();
			continue;
		}
		if (!strcmp(arg, "--freeze")) {
			g_freeze = true;
			continue;
		}
		if (!strncmp(arg, "--fps=", 6)) {
			const char *v = arg + 6;
			char *end;
			errno = 0;
			long n = strtol(v, &end, 10);
			if (errno || !*v || *end || n < 1 || n > 1000) {
				fprintf(stderr, "--fps: expected a rate from 1 to "
								"1000; got \"%s\"\n", v);
				*status = EXIT_FAILURE;
				return false;
			}
			g_fps = (int)n;
			continue;
		}
		if (!strncmp(arg, "--width=", 8)) {
			const char *v = arg + 8;
			char *end;
			errno = 0;
			long n = strtol(v, &end, 10);
			if (errno || !*v || *end || n < 320 || n > 7680) {
				fprintf(stderr, "--width: expected logical points "
								"from 320 to 7680; got \"%s\"\n", v);
				*status = EXIT_FAILURE;
				return false;
			}
			d->width_pt = (int)n;
			continue;
		}
		if (!strcmp(arg, "--text-input")) {
			text_input = true;
			continue;
		}
		if (!strcmp(arg, "--frame-times")) {
			d->frame_times = true;
			continue;
		}
		if (!strcmp(arg, "--no-vsync")) {
			/* Read in fnl_init(), still to come: a diagnostic, with no
			 * descriptor field, as --gpu-driver's variable is. */
#if defined(_WIN32)
			_putenv("FNL_VSYNC=0");
#else
			setenv("FNL_VSYNC", "0", 1);
#endif
			continue;
		}
		if (!strncmp(arg, RANK, sizeof RANK - 1)) {
			const char *v = arg + sizeof RANK - 1;
			if (strcmp(v, "calibrate") != 0 && strcmp(v, "keep") != 0 &&
				strcmp(v, "order") != 0 && strcmp(v, "off") != 0) {
				fprintf(stderr,
						"--gpu-rank: expected calibrate, keep, order "
						"or off; got \"%s\"\n",
						v);
				*status = EXIT_FAILURE;
				return false;
			}
			/* Read in fnl_init(), as --no-vsync's variable is. */
#if defined(_WIN32)
			char put[32];
			snprintf(put, sizeof put, "FNL_GPU_RANK=%s", v);
			_putenv(put);
#else
			setenv("FNL_GPU_RANK", v, 1);
#endif
			continue;
		}
		if (!strncmp(arg, TRANSITION, sizeof TRANSITION - 1)) {
			const char *v = arg + sizeof TRANSITION - 1;
			char *end     = NULL;
			errno         = 0;
			long ms       = strtol(v, &end, 10);
			if (errno || end == v || *end || ms < 0 || ms > 5000) {
				fprintf(stderr,
						"--transition: expected a time in "
						"milliseconds, 0 to 5000; got \"%s\"\n",
						v);
				*status = EXIT_FAILURE;
				return false;
			}
			trans_ms = (int)ms;
			continue;
		}
		if (!strncmp(arg, PIN, sizeof PIN - 1)) {
			const char *v = arg + sizeof PIN - 1;
			char *end;
			errno     = 0;
			double ms = strtod(v, &end);
			/* Bounded above as well as below: the delay is a fraction of one
			 * display period, and anything approaching a second is not a
			 * mistyped frame delay but a hung render thread. */
			if (errno || !*v || *end || !(ms > 0.0) || ms >= 1000.0) {
				fprintf(stderr,
						"--late-latch: expected a delay in "
						"milliseconds, greater than 0 and under 1000; "
						"got \"%s\"\n",
						v);
				*status = EXIT_FAILURE;
				return false;
			}
			d->late_latch_fixed = ms / 1e3;
			continue;
		}
		if (!strncmp(arg, SCALE, sizeof SCALE - 1)) {
			const char *v = arg + sizeof SCALE - 1;
			char *end;
			errno    = 0;
			double s = strtod(v, &end);
			/* Bounded against nonsense, not against the device's ceiling: a
			 * buffer the device will not give is cut back to one it will.
			 * The floor is 1.0 because below it the decorations, laid out in
			 * points and rounded to whole pixels, have fewer pixels than
			 * features (DESIGN *Scale*). Downscaling is for a scene, which
			 * is what fnl_set_scene_size_px() does. */
			if (errno || !*v || *end || !(s >= 1.0) || s > 8.0) {
				fprintf(stderr,
						"--scale: expected a surface scale from 1 "
						"to 8; got \"%s\"\n",
						v);
				*status = EXIT_FAILURE;
				return false;
			}

			/* To 120ths, the unit the whole scale path works in -- rounded to
			 * nearest, so --scale=1.3 is the 156 nearest it rather than a
			 * truncation that lands a 120th low. */
			d->scale_override120 = (int)(s * 120.0 + 0.5);
			continue;
		}
		if (!strncmp(arg, DRIVER, sizeof DRIVER - 1)) {
			const char *v = arg + sizeof DRIVER - 1;
#if defined(_WIN32)
			(void)v;
			fprintf(stderr, "--gpu-driver: a Linux kernel's drivers; "
							"Windows has its own way\n");
			*status = EXIT_FAILURE;
			return false;
#else
			if (!*v) {
				fprintf(stderr, "--gpu-driver: expected nouveau, nvidia or "
								"switch\n");
				*status = EXIT_FAILURE;
				return false;
			}
			/* The library reads it in fnl_init(), which is still to come:
			 * there is no descriptor field for it while it is a test of a
			 * machine rather than a program's choice. */
			setenv("FNL_GPU_DRIVER", v, 1);
			continue;
#endif
		}
		if (!strncmp(arg, LAYOUT, sizeof LAYOUT - 1)) {
			const char *v = arg + sizeof LAYOUT - 1;
#if defined(_WIN32)
			(void)v;
			fprintf(stderr, "--layout: a Linux console's layout; Windows "
							"applies its own\n");
			*status = EXIT_FAILURE;
			return false;
#else
			if (!*v) {
				fprintf(stderr, "--layout: expected <layout>[(<variant>)]"
								"[,<more>][ <options>], as in "
								"de(nodeadkeys)\n");
				*status = EXIT_FAILURE;
				return false;
			}
			/* The KMS backend reads it in fnl_init(), as --gpu-driver's is
			 * read: a test of a machine, not a program's choice, so no
			 * descriptor field. */
			setenv("FNL_KMS_LAYOUT", v, 1);
			continue;
#endif
		}
		if (!strncmp(arg, GPU, sizeof GPU - 1)) {
			const char *v = arg + sizeof GPU - 1;
			char *end;
			errno  = 0;
			long n = strtol(v, &end, 10);
			/* Anything that is not a number through and through is a name,
			 * and so is a number no index reaches: a card is called "1050".
			 * argv outlives the library, so the pointer is kept as it is. */
			if (*v && !errno && (*end || n > 8)) {
				d->gpu_name = v;
				continue;
			}
			/* An upper bound against nonsense: nothing here knows how many
			 * devices there are, and the library answers an index past the
			 * end with a warning and the automatic choice. */
			if (errno || !*v || n < 1) {
				fprintf(stderr,
						"--gpu: expected a device number from 1 to 8 "
						"or part of a device's name; got \"%s\"\n",
						v);
				*status = EXIT_FAILURE;
				return false;
			}
			d->gpu_index = (int)n;
			continue;
		}

		fprintf(stderr, "unknown option \"%s\"\n", arg);
		usage(stderr, prog);
		*status = EXIT_FAILURE;
		return false;
	}
	if (bench_count && bench_clash) {
		fprintf(stderr,
				"%s is refused beside --bench: a benchmark takes "
				"its options as --bench-<key>, which --bench-list "
				"names\n",
				bench_clash);
		*status = EXIT_FAILURE;
		return false;
	}
	return true;
}

#define TITLE "Fenestal testbed"

/* Which backend, renderer and GPU, behind the title: one window on the
 * desk is told from another at a glance. Named after fnl_init(), since the
 * GPU is not known before it, and again at every FNL_EVENT_GPU, since a
 * move to another GPU, or another renderer's, changes two of the three. A
 * Win32 window says where it runs as well, wine or Windows itself, which
 * differ. */
static void name_window(FnlApp *app)
{
	const char *host = fnl_window_host(app);
	char where[64];
	if (!strcmp(fnl_backend_name(app), "Win32"))
		snprintf(where, sizeof where, "Win32, %s", host[0] ? host : "native");
	else
		snprintf(where, sizeof where, "%s", fnl_backend_name(app));
	/* The renderer and, where its frames go through another API's
	 * swapchain, which: "Vulkan via D3D11"; OpenGL's version. */
	char how[64];
	testbed_renderer_label(app, how, sizeof how);
	char title[192];
	snprintf(title, sizeof title, "%s (%s, %s, %s)", TITLE, where, how,
			 fnl_gpu_short_name(app));
	fnl_set_title(app, title);
}

void testbed_renderer_label(FnlApp *app, char *out, size_t size)
{
	const char *via = fnl_renderer_via(app);
	int gl          = 0;
#ifdef SCENE_HAS_GL
	gl = scene_gl_version(app);
#endif
	char version[16] = "";
	if (gl > 0)
		snprintf(version, sizeof version, " %d.%d", gl / 10, gl % 10);
	snprintf(out, size, "%s%s%s%s", fnl_renderer_name(app), version,
			 via[0] ? " via " : "", via);
}

/* Everything this program does with an event, whichever callback took it. */
static void testbed_drain(FnlApp *app)
{
	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
		/* The first window names itself after the GPU it landed on; the
		 * second keeps the title it was opened with, which is how a harness
		 * tells the two apart (_NET_WM_PID is the process's for both). */
		if (ev.type == FNL_EVENT_GPU &&
			app != atomic_load(&g_second_atomic))
			name_window(app);
		/* F10 shows and hides the second window; F12 cycles what it draws.
		 * Both are the testbed's own, and both are recorded here and spent
		 * by the run's loop: the key arrives on a render thread, and a
		 * window is opened and freed on the main thread, so neither acts
		 * where it is seen. The events still go on to the overlay, which
		 * lights the row in its panel. */
		if (ev.type == FNL_EVENT_KEY && ev.key.pressed && !ev.key.repeat) {
			if (ev.key.key == FNL_KEY_F10) {
				/* Open (or about to be) means this press hides it, and the
				 * reverse: asked of the atomics, which this render thread may
				 * read. The loop resolves the wish against the window it
				 * actually has. */
				const bool open = atomic_load(&g_second_open) ||
								  atomic_load(&g_second_want) == 1;
				atomic_store(&g_second_want, open ? 0 : 1);
			}
			if (ev.key.key == FNL_KEY_F12) {
				const int m = (atomic_load(&g_view_mode) + 1) % 3;
				atomic_store(&g_view_mode, m);
				fprintf(stderr, "scene view: %s\n", testbed_view_name());
				/* Both windows redraw with it, whichever asked. The second
				 * window is reached through the atomic, as the view is: this
				 * may be either window's render thread. */
				fnl_invalidate(app);
				FnlApp *other = atomic_load(&g_second_atomic);
				if (other && other != app)
					fnl_invalidate(other);
			}
		}
		testbed_overlay_event(app, &ev);
		testbed_titlebar_custom_event(app, &ev);
	}
}

/* The queue is drained at the top of on_record as well as in on_overlay, as
 * a program whose input moves its scene drains it (fenestal.h, on
 * fnl_poll_event): the round's keys then reach this program through both
 * callbacks, and a key lost between the two is a key the round misses. Not
 * in the benchmark mode's record, which counts its inputs from its own
 * drain. */
static void testbed_record(void *ud, FnlApp *app, const FnlFrame *f)
{
	testbed_drain(app);
	/* The first window's scene size, for the second to place a joined
	 * picture against. After the drain, so a key spent this frame is already
	 * the state this scene is drawn in. */
	join_publish(app, f);
	scene_on_record(ud, app, f);
}

/* The testbed's overlay, in the window pass the library hands it.
 *
 * Separate from on_record because the scene and the overlay are two different
 * render passes whenever a render scale is in force -- see the note on
 * on_overlay in fenestal.h.
 *
 * What arrived since on_record is drained here, ahead of the drawing: what
 * the events change is what the lines below then draw, so a click is
 * answered by the same frame rather than the one after it. Input and drawing
 * are one turn on one thread, so the state they share needs no lock. */
static void testbed_overlay(void *ud, FnlApp *app, const FnlFrame *f)
{
	(void)ud;
	testbed_drain(app);

	testbed_overlay_draw(app, f);
	testbed_titlebar_custom_draw(app, f);

	/* --fps: hold the present rate by sleeping out the rest of the frame's
	 * budget. The deadline advances by one interval each frame, and is
	 * re-anchored to now if a frame overran it, so a slow frame does not
	 * leave a burst of back-to-back presents after it. */
	if (g_fps > 0) {
		const double now  = bench_now();
		const double want = 1.0 / (double)g_fps;
		if (g_fps_next <= 0.0)
			g_fps_next = now;
		if (g_fps_next > now) {
			const double s = g_fps_next - now;
#if defined(_WIN32)
			const DWORD ms = (DWORD)(s * 1e3 + 0.5);
			Sleep(ms ? ms : 1);
#else
			struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) };
			nanosleep(&ts, NULL);
#endif
			g_fps_next += want;
		} else {
			g_fps_next = now + want;
		}
	}
}

/* The second window draws the same scene as the first (its callbacks below),
 * on its own render thread and device. Its overlay is the first's -- the
 * drain and the UI overlay -- with one thing added: it watches whether the
 * library has taken the pointer here. The library takes it on a content click
 * and gives it back on F9 without the application asking, so this only
 * reports: "second capture: 1" and ": 0" on stderr as it changes, for the
 * two-window check. */
static bool g_second_captured;

static void second_overlay(void *ud, FnlApp *app, const FnlFrame *f)
{
	testbed_overlay(ud, app, f);
	const bool c = fnl_is_pointer_captured(app);
	if (c != g_second_captured) {
		g_second_captured = c;
		fprintf(stderr, "second capture: %d\n", c ? 1 : 0);
	}
}

#ifdef _WIN32
/* ---- what this program is on Windows -------------------------------------
 *
 * Two things a graphical Win32 program says about itself in the executable,
 * which is why they are here and not in the library.
 *
 * The first: CMakeLists links this for the GUI subsystem, so Windows opens no
 * console beside the window -- and the process starts with no standard
 * streams. So take the console of whoever launched us, but only if the
 * streams are not already pointed somewhere: a shell or a harness that
 * redirects hands the child a file or a pipe, and reopening onto CONOUT$
 * would take the output away from it.
 *
 * cmd.exe does not wait for a GUI-subsystem process, so its next prompt
 * lands above this program's output. Nothing here can fix that. */
static void use_parent_console(void)
{
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
	if (h && h != INVALID_HANDLE_VALUE)
		return; /* redirected, or inherited: leave it */
	if (!AttachConsole(ATTACH_PARENT_PROCESS))
		return; /* launched from Explorer; no console */
	/* Nothing to answer a failure with, so neither is reported. */
	if (freopen("CONOUT$", "w", stdout))
		(void)freopen("CONOUT$", "w", stderr);
}

/* The second is which GPU a hybrid laptop should run this on. The library
 * already ranks a discrete device first, but on an Optimus or PowerXpress
 * machine the driver decides per application, and these two exported symbols
 * are how an executable answers for itself. They are read out of the
 * *executable's* export table, so a library cannot carry them: anyone building
 * a Fenestal program for a laptop wants these two lines in their own main.c.
 * What they move for a Vulkan program is the order of enumeration, and so
 * what --gpu=1 means: see DESIGN *The startup inventory*. */
__declspec(dllexport) unsigned long NvOptimusEnablement        = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif /* _WIN32 */

/* ---- the run, and the second window it shows and hides -------------------
 *
 * fnl_run() is fnl_start(), a loop of fnl_pump()s and fnl_stop(), and this
 * program writes the loop itself so that it can open and close the second
 * window between two turns: F10 asks for one or the other, and a window is
 * created and freed on this, the main thread, never from a callback. The
 * loop is what fenestal.h documents as fnl_run() in three. */

/* The failure the run ended on, held for main() to print after the library's
 * message has been copied past fnl_shutdown(). */
static char g_run_why[512];

/* A window beside `sibling` -- the first at startup, again after F10 hid the
 * last -- with the scene and the capture-reporting overlay. The descriptor is
 * the same every time, so a hidden-and-shown window opens where the one
 * before it did. */
static FnlResult second_open(FnlApp *sibling)
{
	if (g_second)
		return FNL_OK;
	FnlWindowDesc wd = {
		.size           = sizeof wd,
		.header_version = FNL_VERSION,
		.title          = "Second window",
		.width_pt       = 800,
		.height_pt      = 600,
		.x_pt           = 120,
		.y_pt           = 120,
	};
	const FnlCallbacks second_cb = {
		.size           = sizeof second_cb,
		.on_gpu_created = scene_on_gpu_created,
		.on_gpu_lost    = scene_on_gpu_lost,
		.on_record      = testbed_record,
		.on_overlay     = second_overlay,
	};
	FnlResult rc = fnl_window_create(sibling, &wd, &second_cb, &g_second);
	if (rc < 0)
		return rc;
	/* Read by every window's view on its render thread: a joined picture is
	 * two windows' parts of one canvas only while both are open. */
	atomic_store(&g_second_open, 1);
	atomic_store(&g_second_atomic, g_second);
	g_second_captured = false;
	fprintf(stderr, "second window: shown (F10 hides it)\n");
	return FNL_OK;
}

/* F10 hid it: close the app and free it, so the run goes on with the first
 * window, which is what a host of one window always did. Two steps, because
 * fnl_shutdown() refuses a window still running: ask it to close, let the
 * pump stop its render thread, and free it on the turn after. g_second_hiding
 * is that "on the turn after". */
static bool g_second_hiding;

static void second_hide(void)
{
	if (!g_second || g_second_hiding)
		return;
	/* Read by every window's view on its render thread: a joined picture is
	 * two windows' parts of one canvas only while both are open. */
	atomic_store(&g_second_open, 0);
	fnl_quit(g_second);
	g_second_hiding = true;
	fprintf(stderr, "second window: hidden (F10 shows it)\n");
}

/* The second half of a hide, and the give-back at the program's end: the
 * window is stopped (fnl_pump() stopped it, or fnl_stop() joined it) and its
 * app may now be freed. Idempotent over a window already gone. */
static void second_reap(void)
{
	if (!g_second)
		return;
	/* The picture slot must not outlive the window: its app is about to be
	 * freed, and the address reused for the next window, which would then
	 * draw an image made on this window's dead device. The image itself is
	 * the library's to free in fnl_shutdown(). */
	testbed_overlay_picture_forget(g_second);
	fnl_shutdown(g_second);
	atomic_store(&g_second_atomic, NULL);
	g_second          = NULL;
	g_second_hiding   = false;
	g_second_captured = false;
}

/* fnl_run()'s loop, with the second window opened and closed between turns.
 * `cb` is the first window's callbacks, as fnl_run() takes; `--second-window`
 * opened one before this, and F10 toggles it either way. */
static FnlResult run(FnlApp *app, const FnlCallbacks *cb)
{
	FnlResult rc = fnl_start(app, cb);
	if (rc < 0)
		return rc;
	for (;;) {
		/* The key's wish, spent before the turn that would draw the run with
		 * it: a window opened here draws from this turn on. A hide asks the
		 * window to close and lets the pump below stop it; the reap is after
		 * the pump, once its render thread is joined. */
		const int want = atomic_exchange(&g_second_want, -1);
		if (want == 1 && !g_second) {
			const FnlResult cr = second_open(app);
			if (cr < 0)
				fprintf(stderr, "second window: %s: %s\n",
						fnl_result_str(cr), fnl_last_error());
		} else if (want == 0 && g_second) {
			second_hide();
		}
		rc = fnl_pump(app, -1);
		/* The pump stops a window whose close was asked for at its turn's
		 * top, so a window hidden before it is stopped by the time it
		 * returns -- and only then may fnl_shutdown() free it. */
		if (g_second_hiding)
			second_reap();
		if (rc != FNL_OK)
			break;
	}
	/* Copied before fnl_stop(), and before any fnl_shutdown() a caller does:
	 * fnl_last_error() holds only until the next library call. */
	if (rc < 0)
		snprintf(g_run_why, sizeof g_run_why, "%s", fnl_last_error());
	(void)fnl_stop(app);
	/* A hide asked for on the very last turn, whose pump turn never came: the
	 * window still holds its app, and fnl_stop() above joined its thread, so
	 * it may be freed now. */
	second_reap();
	return rc == FNL_SKIPPED ? FNL_OK : rc;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
	use_parent_console();
#endif

	FnlInitDesc desc = {
		.size           = sizeof desc,
		.header_version = FNL_VERSION,
		/* Names the program, not the platform: one source builds them all. The
		 * library draws it in the titlebar and hands the same string to the
		 * window server. */
		.title = TITLE,
		/* The presentable area, in logical points -- the window comes out
		 * taller by the titlebar drawn above it. */
		.width_pt  = 1280,
		.height_pt = 720,
		/* A program may take the process's signals; a library may not assume
		 * it, so the caller asks. */
		.install_signal_handlers = true,
		/* Every renderer this program has a scene for; the library takes the
		 * first of them, in its own order, that works on this machine. */
		.renderers = scene_renderers(),
	};

	int status;
	if (!parse_args(&desc, argc, argv, &status)) {
		free(bench_args);
		return status;
	}

	/* The benchmark mode opens and closes windows of its own, each from
	 * this descriptor and the scene's callbacks. */
	if (bench_count) {
		const FnlCallbacks scene_cb = {
			.size           = sizeof scene_cb,
			.on_gpu_created = scene_on_gpu_created,
			.on_gpu_lost    = scene_on_gpu_lost,
			.on_record      = scene_on_record,
		};
		status = testbed_bench_main(&desc, &scene_cb, bench_count, bench_args);
		free(bench_args);
		return status;
	}

	FnlApp *app;
	FnlResult rc = fnl_init(&desc, &app);
	if (rc < 0) {
		fprintf(stderr, "%s: %s\n", fnl_result_str(rc), fnl_last_error());
		return EXIT_FAILURE;
	}

	name_window(app);

	/* F5's metrics panel is the library's, on its own default key; the
	 * testbed shows it from the start, as it shows F1's. */
	fnl_set_metrics_panel(app, true);

	/* The dissolve across a move to another GPU, where the command line
	 * set one; the library's own figure otherwise. */
	if (trans_ms >= 0)
		fnl_set_transition_ms(app, trans_ms);

	/* Take the titlebar, if that is what was asked for. Before fnl_run(), so
	 * the window is never seen wearing the library's bar. It keeps the
	 * presentable area asked for above: the library resizes the window by
	 * the difference between the two bars. */
	if (testbed_titlebar_custom_enabled())
		fnl_titlebar_custom(app, testbed_titlebar_custom_height(),
							testbed_titlebar_custom_min_width());

	if (text_input)
		fnl_set_text_input(app, true);

	/* The scene. Everything the library does around it -- the render scale,
	 * both antialiasing modes, the pacing, the loss recovery -- is inherited
	 * without this program knowing about any of it. */
	FnlCallbacks cb = {
		.size           = sizeof cb,
		.on_gpu_created = scene_on_gpu_created,
		.on_gpu_lost    = scene_on_gpu_lost,
		.on_record      = testbed_record,
		.on_overlay     = testbed_overlay,
	};

	/* --second-window: a window beside the first on the same host, before the
	 * run starts, so both draw from the same pump. It draws a picture of its
	 * own -- a mirror, joined to the first's canvas, or its own scene, as F12
	 * says -- on its own render thread and device. A position on the desktop
	 * is asked for, where the backend honours one, so the two do not land on
	 * top of each other. */
	if (g_second_window) {
		rc = second_open(app);
		if (rc < 0) {
			fprintf(stderr, "second window: %s: %s\n", fnl_result_str(rc),
					fnl_last_error());
			fnl_shutdown(app);
			return EXIT_FAILURE;
		}
	}

	rc = run(app, &cb);

	/* The second window first: the host goes with its last window, so the
	 * first's shutdown frees it. A window F10 hid already gave its app back
	 * from the loop, and second_reap() is idempotent over that. */
	second_reap();
	fnl_shutdown(app);

	if (rc < 0) {
		fprintf(stderr, "%s: %s\n", fnl_result_str(rc), g_run_why);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

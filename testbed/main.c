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
#include "demo.h"
#include "bench.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static int  trans_ms = -1;

/* The --bench* arguments, in order, for bench.c; and the first option given
 * beside them that a benchmark row sets instead, which is refused: a record
 * names every option it ran under, and one set behind its back would not
 * be. */
static char      **bench_args;
static int         bench_count;
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

#if !defined(SCENE_HAS_VULKAN) && !defined(SCENE_HAS_GL) && \
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

static const Scene *scene;     /* the one drawing, or NULL */

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
	scene = NULL;
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
	demo_gpu_mem_os_release();
}

static void scene_on_record(void *ud, FnlApp *app, const FnlFrame *f)
{
	if (scene)
		scene->on_record(ud, app, f);
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
	fprintf(f,
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
		"  --footprint-log    print this process's CPU, RAM and VRAM -- the\n"
		"                     metrics panel's figures -- to stderr twice a\n"
		"                     second, \"footprint: ...\", for a harness that\n"
		"                     cannot read the screen.\n"
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
	static const char PIN[]   = "--late-latch=";
	static const char SCALE[] = "--scale=";
	static const char GPU[]   = "--gpu=";
	static const char DRIVER[] = "--gpu-driver=";
	static const char RANK[] = "--gpu-rank=";
	static const char LAYOUT[] = "--layout=";
	static const char TRANSITION[] = "--transition=";

	*status = EXIT_SUCCESS;

	/* The options a benchmark row sets in its own way, or that its run
	 * never reads, refused beside --bench*: the run would not record them,
	 * and the second kind would be dropped without a word. */
	static const char *const CLASH[] = {
		"--no-late-latch", "--late-latch=", "--scale=", "--gpu=",
		"--custom-titlebar", "--text-input", "--frame-times", "--no-vsync",
		"--transition=", "--footprint-log",
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
			demo_titlebar_custom_enable();
			continue;
		}
		if (!strcmp(arg, "--echo-events")) {
			d->echo_events = true;
			continue;
		}
		if (!strcmp(arg, "--footprint-log")) {
			demo_overlay_footprint_log();
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
				fprintf(stderr, "--gpu-rank: expected calibrate, keep, order "
								"or off; got \"%s\"\n", v);
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
			char *end = NULL;
			errno = 0;
			long ms = strtol(v, &end, 10);
			if (errno || end == v || *end || ms < 0 || ms > 5000) {
				fprintf(stderr, "--transition: expected a time in "
								"milliseconds, 0 to 5000; got \"%s\"\n", v);
				*status = EXIT_FAILURE;
				return false;
			}
			trans_ms = (int)ms;
			continue;
		}
		if (!strncmp(arg, PIN, sizeof PIN - 1)) {
			const char *v = arg + sizeof PIN - 1;
			char *end;
			errno = 0;
			double ms = strtod(v, &end);
			/* Bounded above as well as below: the delay is a fraction of one
			 * display period, and anything approaching a second is not a
			 * mistyped frame delay but a hung render thread. */
			if (errno || !*v || *end || !(ms > 0.0) || ms >= 1000.0) {
				fprintf(stderr, "--late-latch: expected a delay in "
								"milliseconds, greater than 0 and under 1000; "
								"got \"%s\"\n", v);
				*status = EXIT_FAILURE;
				return false;
			}
			d->late_latch_fixed = ms / 1e3;
			continue;
		}
		if (!strncmp(arg, SCALE, sizeof SCALE - 1)) {
			const char *v = arg + sizeof SCALE - 1;
			char *end;
			errno = 0;
			double s = strtod(v, &end);
			/* Bounded against nonsense, not against the device's ceiling: a
			 * buffer the device will not give is cut back to one it will.
			 * The floor is 1.0 because below it the decorations, laid out in
			 * points and rounded to whole pixels, have fewer pixels than
			 * features (DESIGN *Scale*). Downscaling is for a scene, which
			 * is what fnl_set_scene_size_px() does. */
			if (errno || !*v || *end || !(s >= 1.0) || s > 8.0) {
				fprintf(stderr, "--scale: expected a surface scale from 1 "
								"to 8; got \"%s\"\n", v);
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
			errno = 0;
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
				fprintf(stderr, "--gpu: expected a device number from 1 to 8 "
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
		fprintf(stderr, "%s is refused beside --bench: a benchmark takes "
						"its options as --bench-<key>, which --bench-list "
						"names\n", bench_clash);
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
		snprintf(where, sizeof where, "Win32, %s",
				 host[0] ? host : "native");
	else
		snprintf(where, sizeof where, "%s", fnl_backend_name(app));
	/* The renderer and, where its frames go through another API's
	 * swapchain, which: "Vulkan via D3D11"; OpenGL's version. */
	char how[64];
	demo_renderer_label(app, how, sizeof how);
	char title[192];
	snprintf(title, sizeof title, "%s (%s, %s, %s)", TITLE, where, how,
			 fnl_gpu_short_name(app));
	fnl_set_title(app, title);
}

void demo_renderer_label(FnlApp *app, char *out, size_t size)
{
	const char *via = fnl_renderer_via(app);
	int gl = 0;
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
static void demo_drain(FnlApp *app)
{
	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
		if (ev.type == FNL_EVENT_GPU)
			name_window(app);
		demo_overlay_event(app, &ev);
		demo_titlebar_custom_event(app, &ev);
	}
}

/* The queue is drained at the top of on_record as well as in on_overlay, as
 * a program whose input moves its scene drains it (fenestal.h, on
 * fnl_poll_event): the round's keys then reach this program through both
 * callbacks, and a key lost between the two is a key the round misses. Not
 * in the benchmark mode's record, which counts its inputs from its own
 * drain. */
static void demo_record(void *ud, FnlApp *app, const FnlFrame *f)
{
	demo_drain(app);
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
static void demo_overlay(void *ud, FnlApp *app, const FnlFrame *f)
{
	(void)ud;
	demo_drain(app);

	demo_overlay_draw(app, f);
	demo_titlebar_custom_draw(app, f);
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
		return;                       /* redirected, or inherited: leave it */
	if (!AttachConsole(ATTACH_PARENT_PROCESS))
		return;                       /* launched from Explorer; no console */
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
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif  /* _WIN32 */

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
		.title          = TITLE,
		/* The presentable area, in logical points -- the window comes out
		 * taller by the titlebar drawn above it. */
		.width_pt       = 1280,
		.height_pt      = 720,
		/* A program may take the process's signals; a library may not assume
		 * it, so the caller asks. */
		.install_signal_handlers = true,
		/* Every renderer this program has a scene for; the library takes the
		 * first of them, in its own order, that works on this machine. */
		.renderers      = scene_renderers(),
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
		status = demo_bench_main(&desc, &scene_cb, bench_count, bench_args);
		free(bench_args);
		return status;
	}

	FnlApp   *app;
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
	if (demo_titlebar_custom_enabled())
		fnl_titlebar_custom(app, demo_titlebar_custom_height(),
							demo_titlebar_custom_min_width());

	if (text_input)
		fnl_set_text_input(app, true);

	/* The scene. Everything the library does around it -- the render scale,
	 * both antialiasing modes, the pacing, the loss recovery -- is inherited
	 * without this program knowing about any of it. */
	FnlCallbacks cb = {
		.size           = sizeof cb,
		.on_gpu_created = scene_on_gpu_created,
		.on_gpu_lost    = scene_on_gpu_lost,
		.on_record      = demo_record,
		.on_overlay     = demo_overlay,
	};

	rc = fnl_run(app, &cb);
	/* Copied first: fnl_last_error() holds only until the next call, and
	 * fnl_shutdown() is one. */
	char why[512] = "";
	if (rc < 0)
		snprintf(why, sizeof why, "%s", fnl_last_error());
	fnl_shutdown(app);

	if (rc < 0) {
		fprintf(stderr, "%s: %s\n", fnl_result_str(rc), why);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

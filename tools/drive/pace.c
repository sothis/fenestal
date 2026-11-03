/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * fenestal-dev pace -- one instrument, three backends: what the frame loop
 * does to a window that is being resized, measured the same way on every
 * one of them.
 *
 *   fenestal-dev pace [<sizes/s> [<seconds>]] [--settle <s>]
 *        [--no-late-latch] [--late-latch=<ms>] [--draw] [--no-vsync]
 *
 * A rate of zero leaves the window alone, which is the floor every other rate
 * is read against. --draw is the other end: no sizes, and every frame asked
 * for (fnl_invalidate(), from the same thread), so each is re-recorded and
 * presented -- which is the only thing that makes a window whose size cannot
 * change draw at all, and what times a present path whose cost is per frame
 * rather than per size: the KMS copy path's readback and flip.
 *
 * --no-vsync starts the window without vsync, by FNL_VSYNC=0, the only way
 * a window is told so: with --draw, the frame rate an environment reaches
 * flat out, which is where one that still waits on the display shows -- a
 * present mode that says "off" and is held to the refresh rate anyway.
 * There the frames ask for themselves: an uncapped loop outruns DRAW_HZ,
 * rests between two asks, and a rest restarts the frame-rate window
 * (DESIGN *At rest*), so a clock would report nothing. With vsync off only
 * --draw reads as a rate.
 *
 * Beside it, `pace: shown ...`: each frame the display stamped
 * (fnl_frame_shown()) joined on its index with the frame drawn
 * (fnl_frame_samples()), its age from the latch to the screen -- which a
 * stamp taken for the wrong frame shows as an age a few periods too long.
 *
 * The rate is the frames presented while it drives, counted by
 * fnl_frame_stats() and printed when it stops (`pace: drive ...`), and not
 * the --frame-times lines: a window that draws a size and rests before the
 * next one restarts their window at every wake and prints none while it is
 * driven, at any rate below the display's.
 *
 * The harnesses beside this one drive the *window server*, through the
 * RemoteDesktop portal, and what they time includes the portal and the
 * compositor. This drives the *library*, through the one public call every
 * backend forwards to its own pump -- fnl_request_size_pt(), which fenestal.h
 * tags [any] precisely so a caller may ask from a thread of its own -- so the
 * same program, cross-built, puts the same stream of sizes through Wayland,
 * X11 and Win32 and the numbers are comparable. A pacing fault on two
 * backends of three shows on none of them alone; see DESIGN *Frame pacing*.
 *
 * It registers no callbacks -- no on_record, no on_overlay, no scene --
 * but the one --draw --no-vsync asks with, which draws nothing. What
 * --frame-times then reports is the library's own floor -- the pacing, the
 * device waits, the titlebar's recording and the submit. The testbed answers
 * the other question, what a frame costs with an application in it, and having
 * both separates a regression in the library from one in the example.
 *
 * It includes the installed header and no other of the library's, so
 * building it is the same proof testbed carries about the link line, from a
 * program with no graphics API in it at all.
 */

#include <fenestal/fenestal.h>

#include "dev.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

/* The window this drives, in logical points. */
#define PACE_W 1280
#define PACE_H 720

/* What a size request cycles through. A request for the size the window
 * already has is one the window server may drop, so the width walks; one axis
 * is enough to force the rebuild. */
#define PACE_MIN_W 900
#define PACE_SPAN  200
#define PACE_REQ_H 620

/* ---- the system's three primitives ---------------------------------------
 *
 * A clock, a sleep and a thread. This is a caller, so src/core/thread.h is
 * out of reach; three calls written twice are smaller than exporting a seam. */

#if defined(_WIN32)

static double now_seconds(void)
{
	LARGE_INTEGER freq, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	return freq.QuadPart ? (double)now.QuadPart / (double)freq.QuadPart : 0.0;
}

static void nap(double seconds)
{
	/* Behind schedule is no wait at all. Converted, a negative is a DWORD
	 * near INFINITE: a drive that fell a millisecond behind slept for good,
	 * and its window never closed. */
	if (seconds <= 0.0)
		return;
	/* Sleep() rounds down, so a sub-millisecond request would spin at zero.
	 * Every wait here is tens of milliseconds or more. */
	DWORD ms = (DWORD)(seconds * 1e3 + 0.5);
	if (ms)
		Sleep(ms);
}

#else

static double now_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void nap(double seconds)
{
	if (seconds <= 0.0)
		return;
	struct timespec ts = {
		.tv_sec  = (time_t)seconds,
		.tv_nsec = (long)((seconds - (double)(time_t)seconds) * 1e9),
	};
	nanosleep(&ts, NULL);
}

#endif

/* ---- what the driving thread does ---------------------------------------- */

static FnlApp *g_app;
static double  g_rate   = 60.0;   /* size requests a second; 0 asks for none */
static double  g_run    = 12.0;   /* how long to drive for                   */
static double  g_settle = 4.0;    /* before it starts                        */
static bool    g_draw   = false;  /* every frame asked for, no sizes         */
static bool    g_no_vsync = false;
/* --draw --no-vsync: each frame asks for the next, once the settle is over. */
static atomic_bool g_asking;

static void ask_again(void *ud, FnlApp *app, const FnlFrame *frame)
{
	(void)ud;
	(void)frame;
	/* Drained, or a waiting event -- the focus, at the start -- would be a
	 * reason to record every turn of the settle (events_pending()). */
	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
	}
	if (atomic_load(&g_asking))
		fnl_invalidate(app);
}

/* ---- the frames shown, joined on their index ---------------------------- */

/* As many as a drive at a display's rate keeps, and then some; past it the
 * rest are counted and not kept. */
#define SHOWN_KEEP 16384

static FnlFrameSample g_drawn[SHOWN_KEEP];
static FnlFrameShown  g_shown[SHOWN_KEEP];
static int            g_drawn_n, g_shown_n;

/* Both lists drained, the rows kept where `keep` and dropped otherwise --
 * the settle's, which the drive's figures are not about. */
static void drain(bool keep)
{
	FnlFrameSample fs[128];
	FnlFrameShown  sh[128];
	int n;
	do {
		n = fnl_frame_samples(g_app, fs, 128, sizeof fs[0], NULL);
		for (int i = 0; keep && i < n && g_drawn_n < SHOWN_KEEP; i++)
			if (fs[i].presented)
				g_drawn[g_drawn_n++] = fs[i];
	} while (n == 128);
	do {
		n = fnl_frame_shown(g_app, sh, 128, sizeof sh[0], NULL);
		for (int i = 0; keep && i < n && g_shown_n < SHOWN_KEEP; i++)
			g_shown[g_shown_n++] = sh[i];
	} while (n == 128);
}

static int by_value(const void *x, const void *y)
{
	const double a = *(const double *)x, b = *(const double *)y;
	return (a > b) - (a < b);
}

/* Every shown frame's age, the latch to the screen, by one walk of the two
 * lists, both in index order; `pace: shown ...`. */
static void shown_report(void)
{
	static double age[SHOWN_KEEP];
	int n = 0, unmatched = 0, d = 0, twice = 0, early = 0;
	for (int i = 0; i < g_shown_n; i++) {
		if (i > 0 && g_shown[i].index == g_shown[i - 1].index)
			twice++;
		while (d < g_drawn_n && g_drawn[d].index < g_shown[i].index)
			d++;
		if (d < g_drawn_n && g_drawn[d].index == g_shown[i].index) {
			const double ms = (g_shown[i].time - g_drawn[d].time) * 1e3;
			age[n++] = ms;
			/* Shown before it was latched cannot be: the first few said
			 * with their neighbours' stamps, which is what tells a wrong
			 * frame from a wrong clock. */
			if (ms < 0.0 && early++ < 3)
				fprintf(stderr, "pace: shown frame %llu %.1f ms before its "
						"latch (stamps %.4f%s%.4f%s%.4f s)\n",
						(unsigned long long)g_shown[i].index, -ms,
						i > 0 ? g_shown[i - 1].time : 0.0, " / ",
						g_shown[i].time, " / ",
						i + 1 < g_shown_n ? g_shown[i + 1].time : 0.0);
		} else {
			unmatched++;
		}
	}
	if (twice)
		fprintf(stderr, "pace: shown %d frames stamped twice\n", twice);
	if (n == 0) {
		fprintf(stderr, "pace: shown none of %d presents stamped (%d "
				"stamps matched no frame)\n", g_drawn_n, unmatched);
		return;
	}
	qsort(age, (size_t)n, sizeof age[0], by_value);
	fprintf(stderr, "pace: shown %d of %d presents, latch to shown median "
			"%.1f ms, p95 %.1f, min %.1f, max %.1f (%d stamps matched no "
			"frame)\n", n, g_drawn_n, age[n / 2], age[(n * 95) / 100],
			age[0], age[n - 1], unmatched);
}

/* How often --draw asks: well past any display's rate, so a frame is always
 * owed when the last one is done, and cheap, since several asks between two
 * frames cost one re-record. */
#define DRAW_HZ 1000.0

/* Sizes at a fixed rate, then shut the window.
 *
 * The settle is not politeness: both late-latch controllers and the display
 * period estimate wait out the first sixty presents, and a run that starts
 * driving before that measures the settling rather than the steady state.
 *
 * Paced against an absolute schedule rather than by sleeping the period each
 * time round, so the rate is the rate asked for however long a request takes
 * to return -- half a millisecond on Win32, where SetWindowPos re-enters the
 * window procedure. */
static void drive(void)
{
	fnl_set_frame_stats(g_app, true);
	nap(g_settle);
	drain(false);

	FnlFrameStats s0, s1;
	memset(&s0, 0, sizeof s0);
	memset(&s1, 0, sizeof s1);
	s0.size = s1.size = sizeof s0;
	(void)fnl_frame_stats(g_app, &s0);
	const double t0 = now_seconds();
	long n = 0;

	if (g_draw && g_no_vsync) {
		atomic_store(&g_asking, true);
		fnl_invalidate(g_app);      /* the first; each frame asks after it */
		nap(g_run);
		atomic_store(&g_asking, false);
	} else if (g_draw) {
		const double period = 1.0 / DRAW_HZ;
		double due = t0;
		while (now_seconds() - t0 < g_run) {
			fnl_invalidate(g_app);
			due += period;
			nap(due - now_seconds());
		}
	} else if (g_rate > 0.0) {
		const double period = 1.0 / g_rate;
		double due = t0;
		while (now_seconds() - t0 < g_run) {
			fnl_request_size_pt(g_app,
								PACE_MIN_W + (int)(n % PACE_SPAN), PACE_REQ_H);
			n++;
			/* The library keeps 1024 of each; drained well before. */
			if (n % 16 == 0)
				drain(true);
			due += period;
			nap(due - now_seconds());
		}
	} else {
		nap(g_run);
	}

	const double dt = now_seconds() - t0;
	(void)fnl_frame_stats(g_app, &s1);
	/* A stamp comes a refresh or two after its present: waited for, so the
	 * drive's last frames are in. */
	nap(0.1);
	drain(true);
	const uint64_t shown = s1.presents - s0.presents;
	fprintf(stderr, "pace: drive %llu frames, %llu presents, %llu swapchains "
			"in %.2f s (%.1f presents/s)\n",
			(unsigned long long)(s1.frames - s0.frames),
			(unsigned long long)shown,
			(unsigned long long)(s1.rebuilds - s0.rebuilds), dt,
			dt > 0.0 ? (double)shown / dt : 0.0);
	/* On stderr with everything the library logs, so one redirect has both. */
	/* With the present mode it ended on: vsync off on a surface that offers
	 * nothing but FIFO is still FIFO, and the rate alone would not say so. */
	fprintf(stderr, "pace: %ld requests in %.2f s (%.1f/s)%s, %s\n",
			n, dt, dt > 0.0 ? (double)n / dt : 0.0,
			g_draw ? ", every frame asked for" : "",
			fnl_present_mode(g_app));
	shown_report();
	fflush(stderr);

	fnl_quit(g_app);
}

#if defined(_WIN32)
static DWORD WINAPI drive_thread(LPVOID unused)
{
	(void)unused;
	drive();
	return 0;
}
#else
static void *drive_thread(void *unused)
{
	(void)unused;
	drive();
	return NULL;
}
#endif

/* ---- arguments ----------------------------------------------------------- */

static void usage(void)
{
	fprintf(stderr,
		"fenestal-dev pace [<sizes/s> [<seconds>]] [options]\n"
		"\n"
		"  <sizes/s>          size requests a second; 0 leaves the window\n"
		"                     alone, which is the floor to read the rest\n"
		"                     against (default 60)\n"
		"  <seconds>          how long to drive for (default 12)\n"
		"  --settle <s>       quiet time before driving starts (default 4);\n"
		"                     the controllers need it, see drive()\n"
		"  --no-late-latch    the same run with the hold off, which is the\n"
		"                     floor a hold has to be read against\n"
		"  --late-latch=<ms>  pin the hold instead of letting it tune\n"
		"  --draw             no sizes: every frame asked for instead, so\n"
		"                     each is recorded and presented; the cost of a\n"
		"                     frame, where the rates are the cost of a size\n"
		"  --no-vsync         vsync off, as FNL_VSYNC=0 does for any program\n"
		"  -h, --help         this\n"
		"\n"
		"--frame-times is always on: reporting where the frame went is the\n"
		"whole of what this program is for.\n");
}

/* One number filling the whole of `v`, from `lo` -- or above it, when `open`
 * -- and under `hi`. Says what was wrong and answers false otherwise. */
static bool number(const char *what, const char *v, double lo, bool open,
				   double hi, double *out)
{
	char *end;
	errno = 0;
	const double x = strtod(v, &end);
	if (errno || !*v || *end || !(open ? x > lo : x >= lo) || !(x < hi)) {
		fprintf(stderr, "pace: %s: expected a number %s %g and under %g; "
						"got \"%s\"\n", what, open ? "above" : "from", lo,
				hi, v);
		return false;
	}
	*out = x;
	return true;
}

int pace_main(int argc, char **argv)
{
	static const char LATE[] = "--late-latch=";

	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size           = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title          = "Fenestal pace";
	desc.width_pt       = PACE_W;
	desc.height_pt      = PACE_H;
	desc.frame_times    = true;
	/* It draws no scene, so any renderer the build has will do. */
	desc.renderers      = FNL_RENDERERS_ANY;

	int positional = 0;
	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			usage();
			return EXIT_SUCCESS;
		}
		if (!strcmp(arg, "--draw")) {
			g_draw = true;
			continue;
		}
		if (!strcmp(arg, "--no-late-latch")) {
			desc.no_late_latch = true;
			continue;
		}
		if (!strcmp(arg, "--no-vsync")) {
			g_no_vsync = true;
			continue;
		}
		if (!strncmp(arg, LATE, sizeof LATE - 1)) {
			double ms;
			if (!number("--late-latch", arg + sizeof LATE - 1, 0.0, true,
						1000.0, &ms))
				return EXIT_FAILURE;
			desc.late_latch_fixed = ms * 1e-3;
			continue;
		}
		if (!strcmp(arg, "--settle") && i + 1 < argc) {
			if (!number("--settle", argv[++i], 0.0, false, 1e6, &g_settle))
				return EXIT_FAILURE;
			continue;
		}
		if (arg[0] == '-') {
			fprintf(stderr, "pace: unknown option %s\n", arg);
			usage();
			return EXIT_FAILURE;
		}
		if (positional == 0) {
			if (!number("<sizes/s>", arg, 0.0, false, 1e6, &g_rate))
				return EXIT_FAILURE;
		} else if (positional == 1) {
			if (!number("<seconds>", arg, 0.0, true, 1e6, &g_run))
				return EXIT_FAILURE;
		} else {
			fprintf(stderr, "pace: too many arguments\n");
			usage();
			return EXIT_FAILURE;
		}
		positional++;
	}

	/* Before fnl_init(), which reads it once. */
	if (g_no_vsync) {
#if defined(_WIN32)
		_putenv("FNL_VSYNC=0");
#else
		setenv("FNL_VSYNC", "0", 1);
#endif
	}

	FnlResult rc = fnl_init(&desc, &g_app);
	if (rc < 0) {
		fprintf(stderr, "pace: %s: %s\n", fnl_result_str(rc), fnl_last_error());
		return EXIT_FAILURE;
	}

	/* Which backend, renderer and GPU, behind the title: one window on the
	 * desk is told from another at a glance. Here, since the GPU is not
	 * known before fnl_init(). A Win32 window says where it runs as well,
	 * wine or Windows itself, which differ. */
	const char *host = fnl_window_host(g_app);
	char where[64];
	if (!strcmp(fnl_backend_name(g_app), "Win32"))
		snprintf(where, sizeof where, "Win32, %s",
				 host[0] ? host : "native");
	else
		snprintf(where, sizeof where, "%s", fnl_backend_name(g_app));
	const char *via = fnl_renderer_via(g_app);
	char how[48];
	snprintf(how, sizeof how, "%s%s%s", fnl_renderer_name(g_app),
			 via[0] ? " via " : "", via);
	char title[192];
	snprintf(title, sizeof title, "%s (%s, %s, %s)", desc.title, where, how,
			 fnl_gpu_short_name(g_app));
	fnl_set_title(g_app, title);

	/* Started before fnl_run(), there being no callback to start it from:
	 * a thread of the caller's own, which is what the [any] tag allows. */
#if defined(_WIN32)
	HANDLE thread = CreateThread(NULL, 0, drive_thread, NULL, 0, NULL);
	const bool started = thread != NULL;
#else
	pthread_t thread;
	const bool started = pthread_create(&thread, NULL, drive_thread, NULL) == 0;
#endif
	if (!started) {
		fprintf(stderr, "pace: could not start the driving thread\n");
		fnl_shutdown(g_app);
		return EXIT_FAILURE;
	}

	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size = sizeof cb;
	cb.on_overlay = ask_again;
	rc = fnl_run(g_app, g_draw && g_no_vsync ? &cb : NULL);

	/* Joined before the shutdown, so the thread cannot still be inside
	 * fnl_request_size_pt() on an App that has been torn down. Its last act is
	 * the fnl_quit() that ended fnl_run(), so the join does not wait. */
#if defined(_WIN32)
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
#else
	pthread_join(thread, NULL);
#endif

	/* Copied first: fnl_last_error() holds only until the next call, and
	 * fnl_shutdown() is one. */
	char why[512] = "";
	if (rc < 0)
		snprintf(why, sizeof why, "%s", fnl_last_error());
	fnl_shutdown(g_app);

	if (rc < 0) {
		fprintf(stderr, "pace: %s: %s\n", fnl_result_str(rc), why);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * fenestal-dev lifecycle -- the library called wrongly on purpose, and then
 * opened and closed many times in one process, with what each lifetime
 * leaves behind counted.
 *
 *   fenestal-dev lifecycle [<cycles>]           (default 20)
 *
 * The misuse half is what fenestal.h and app.c promise a caller who gets it
 * wrong: every public entry point takes a NULL app and does nothing, a
 * descriptor whose size no build ever produced is refused, an overlay call
 * outside on_overlay draws nothing, fnl_run() off the fnl_init() thread
 * fails, and fnl_shutdown() from a callback is refused rather than
 * deadlocking. Each is a line, so a crash names the call that made it.
 *
 * The lifecycle half runs init, a few frames and shutdown, over and over,
 * with the signal handlers installed, and compares what the process holds
 * after the last lifetime against after the first: descriptors and threads
 * here, handles and GDI/USER objects on Windows. The first lifetime is the
 * baseline and not the empty process, because a driver loads once and keeps
 * what it loaded. The terminating signals' dispositions must also be the
 * host's again once each lifetime ends.
 *
 * It also defines global functions named as the library's internals are
 * (below), so that linking it at all shows the archive exports only the API.
 *
 * Exit status 0 is everything held. A mode of fenestal-dev (dev.c), the
 * program the pace instrument is the other mode of; nothing in the build
 * runs it.
 */

#include <fenestal/fenestal.h>

#include "dev.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#endif

static int failures;

static void check(bool ok, const char *what)
{
	printf("%-60s %s\n", what, ok ? "ok" : "FAIL");
	fflush(stdout);
	if (!ok)
		failures++;
}

static void say(const char *what)
{
	printf("%-60s ", what);
	fflush(stdout);
}

/* ---- what a lifetime leaves ---------------------------------------------- */

typedef struct {
	long a, b, c;     /* fds, threads / handles, GDI, USER objects */
} Held;

#if defined(_WIN32)
static Held held(void)
{
	DWORD handles = 0;
	GetProcessHandleCount(GetCurrentProcess(), &handles);
	Held h = {
		(long)handles,
		(long)GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
		(long)GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS),
	};
	return h;
}
static const char *const HELD_NAMES[3] = { "handles", "GDI objects",
										   "USER objects" };
#else
static long count_dir(const char *path)
{
	DIR *d = opendir(path);
	if (!d)
		return -1;
	long n = 0;
	for (struct dirent *e; (e = readdir(d)) != NULL; )
		if (e->d_name[0] != '.')
			n++;
	closedir(d);
	return n;
}
/* Linux lists both under /proc. macOS has no /proc: its descriptors are
 * /dev/fd, and its threads are the task's, asked of Mach. */
#if defined(__APPLE__)
#define FD_DIR "/dev/fd"
static long count_threads(void)
{
	thread_act_array_t list;
	mach_msg_type_number_t n = 0;
	if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS)
		return -1;
	for (mach_msg_type_number_t i = 0; i < n; i++)
		mach_port_deallocate(mach_task_self(), list[i]);
	vm_deallocate(mach_task_self(), (vm_address_t)list,
				  n * sizeof list[0]);
	return (long)n;
}
#else
#define FD_DIR "/proc/self/fd"
static long count_threads(void)
{
	return count_dir("/proc/self/task");
}
#endif

static Held held(void)
{
	Held h = { count_dir(FD_DIR), count_threads(), 0 };
	return h;
}
static const char *const HELD_NAMES[3] = { "descriptors", "threads", "-" };

/* What each descriptor is, by number, so that a count that grew can say what
 * it grew by. */
#define FD_MAX 1024
static char fd_was[FD_MAX][96];

static void fds_note(char (*into)[96])
{
	DIR *d = opendir(FD_DIR);
	for (int i = 0; i < FD_MAX; i++)
		into[i][0] = '\0';
	if (!d)
		return;
	for (struct dirent *e; (e = readdir(d)) != NULL; ) {
		int fd = atoi(e->d_name);
		if (e->d_name[0] == '.' || fd < 0 || fd >= FD_MAX)
			continue;
#if defined(__APPLE__)
		/* No links to read under /dev/fd: the kernel says what a
		 * descriptor names, cut to what fits. */
		char path[1024];
		if (fcntl(fd, F_GETPATH, path) == -1)
			snprintf(path, sizeof path, "(descriptor)");
		snprintf(into[fd], sizeof into[fd], "%s", path);
#else
		char path[64];
		snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
		ssize_t n = readlink(path, into[fd], sizeof into[fd] - 1);
		into[fd][n > 0 ? n : 0] = '\0';
#endif
	}
	closedir(d);
}

static void fds_report(void)
{
	static char now[FD_MAX][96];
	fds_note(now);
	for (int i = 0; i < FD_MAX; i++)
		if (now[i][0] && strcmp(now[i], fd_was[i]) != 0)
			printf("    new since the first lifetime: fd %d -> %s\n", i,
				   now[i]);
}
#endif

/* The handler SIGINT has, as something to compare: what the host had before
 * the first fnl_init() is what every fnl_shutdown() must put back. */
static void (*sigint_handler(void))(int)
{
	void (*h)(int) = signal(SIGINT, SIG_DFL);
	signal(SIGINT, h);
	return h;
}

/* ---- callbacks ----------------------------------------------------------- */

typedef struct {
	bool on_nvidia;      /* the device was NVIDIA's (see the GDI check) */
	int  frames;
	int  quit_after;
	bool try_shutdown;   /* call fnl_shutdown() from inside, once */
	bool shutdown_tried;
	/* Asked for on the first frame, so the lifetime also builds and tears
	 * down an offscreen scene target and that mode's images. */
	FnlAntialias aa;
	bool         pin_scene;
} Run;

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	(void)frame;
	Run *r = ud;
	FnlEvent ev;
	while (fnl_poll_event(app, &ev))
		;
	if (r->frames == 0) {
		r->on_nvidia = strstr(fnl_gpu_name(app), "NVIDIA") != NULL;
		fnl_set_antialias(app, r->aa);
		if (r->pin_scene)
			fnl_set_scene_size_px(app, 320, 200);
	}
	if (r->try_shutdown && !r->shutdown_tried) {
		r->shutdown_tried = true;
		fnl_shutdown(app);       /* refused: this is the render thread */
	}
	if (++r->frames >= r->quit_after)
		fnl_quit(app);
	else
		/* on_overlay runs only when a frame is recorded, and a still window
		 * presents what it already has: asked for, or a lifetime on a
		 * quiet desktop waits for frames that are never recorded. */
		fnl_invalidate(app);
}

static FnlInitDesc desc_for(void)
{
	FnlInitDesc d;
	memset(&d, 0, sizeof d);
	d.size = sizeof d;
	d.header_version = FNL_VERSION;
	d.title = "Fenestal lifecycle";
	d.width_pt = 480;
	d.height_pt = 320;
	d.install_signal_handlers = true;
	/* It draws no scene, so any renderer the build has will do. */
	d.renderers = FNL_RENDERERS_ANY;
	return d;
}

/* Which backend, renderer and GPU, behind the title, as the testbed and
 * pace have it: one window on the desk is told from another at a
 * glance. After fnl_init(), since the GPU is not known before it. A
 * Win32 window says where it runs as well, wine or Windows itself, which
 * differ. */
static void title_for(FnlApp *app, const FnlInitDesc *d)
{
	const char *host = fnl_window_host(app);
	char where[64];
	if (!strcmp(fnl_backend_name(app), "Win32"))
		snprintf(where, sizeof where, "Win32, %s",
				 host[0] ? host : "native");
	else
		snprintf(where, sizeof where, "%s", fnl_backend_name(app));
	char title[192];
	snprintf(title, sizeof title, "%s (%s, %s, %s)", d->title, where,
			 fnl_renderer_name(app), fnl_gpu_short_name(app));
	fnl_set_title(app, title);
}

static FnlResult one_lifetime(Run *r)
{
	FnlInitDesc d = desc_for();
	FnlApp *app = NULL;
	FnlResult rc = fnl_init(&d, &app);
	if (rc < 0)
		return rc;
	title_for(app, &d);
	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size = sizeof cb;
	cb.ud = r;
	cb.on_overlay = on_overlay;
	rc = fnl_run(app, &cb);
	fnl_shutdown(app);
	return rc;
}

/* ---- misuse -------------------------------------------------------------- */

static void null_app(void)
{
	int a = 0, b = 0, c = 0, d = 0;
	char buf[16];
	FnlEvent ev;
	const FnlColor white = { 1, 1, 1, 1 };
	FnlFrameStats st;
	FnlFrameSample sample;
	FnlFrameShown shown;
	uint64_t lost;
	FnlMode mode = { 640, 480, 60000 };
	memset(&st, 0, sizeof st);
	st.size = sizeof st;

	say("every entry point with a NULL app");
	fnl_shutdown(NULL);
	fnl_quit(NULL);
	(void)fnl_poll_event(NULL, &ev);
	(void)fnl_gpu_name(NULL);
	(void)fnl_renderer(NULL);
	(void)fnl_gpu_short_name(NULL);
	fnl_set_title(NULL, "x");
	fnl_overlay_text(NULL, 0, 0, FNL_TEXT_BODY, white, "x");
	fnl_overlay_rect(NULL, 0, 0, 1, 1, white);
	fnl_overlay_stroke(NULL, 0, 0, 1, 1, 1, white);
	fnl_overlay_elbow(NULL, 0, 0, 1, 1, 2, 2, 1, white);
	fnl_overlay_box(NULL, 0, 0, 1, 1, 1, white);
	fnl_overlay_flush(NULL);
	fnl_overlay_clip(NULL, 0, 0, 1, 1);
	fnl_window_size_pt(NULL, &a, &b);
	fnl_invalidate(NULL);
	(void)fnl_titlebar_reserve(NULL, 10, &a, &b, &c, &d);
	(void)fnl_titlebar_custom(NULL, 30, 100);
	(void)fnl_titlebar_claim(NULL, 0, 0, 1, 1);
	(void)fnl_begin_move(NULL);
	(void)fnl_begin_resize(NULL, FNL_EDGE_TOP);
	(void)fnl_is_fullscreen(NULL);
	(void)fnl_is_active(NULL);
	(void)fnl_is_maximized(NULL);
	(void)fnl_is_pointer_captured(NULL);
	(void)fnl_max_samples(NULL);
	(void)fnl_offscreen_supported(NULL);
	(void)fnl_antialias(NULL);
	(void)fnl_fps(NULL);
	fnl_scene_size_px(NULL, &a, &b);
	fnl_content_size_px(NULL, &a, &b);
	fnl_content_rect_pt(NULL, &a, &b, &c, &d);
	fnl_min_content_size_pt(NULL, &a, &b);
	fnl_set_fullscreen(NULL, true);
	fnl_minimize(NULL);
	fnl_set_maximized(NULL, true);
	fnl_set_antialias(NULL, FNL_AA_NONE);
	fnl_set_cursor(NULL, FNL_CURSOR_DEFAULT);
	fnl_release_pointer(NULL);
	fnl_set_scene_size_px(NULL, 10, 10);
	(void)fnl_output_name(NULL);
	(void)fnl_present_mode(NULL);
	(void)fnl_backend_name(NULL);
	(void)fnl_window_host(NULL);
	(void)fnl_renderer_name(NULL);
	fnl_output_size_pt(NULL, &a, &b);
	(void)fnl_scale120(NULL);
	fnl_request_size_pt(NULL, 100, 100);
	(void)fnl_key_down(NULL, FNL_KEY_A);
	fnl_set_text_input(NULL, true);
	(void)fnl_is_text_input(NULL);
	(void)fnl_compose_text(NULL, buf, sizeof buf);
	fnl_set_text_rect_pt(NULL, 0, 0, 1, 1);
	(void)fnl_clipboard_set(NULL, "x");
	fnl_clipboard_request(NULL);
	(void)fnl_clipboard_text(NULL, buf, sizeof buf);
	(void)fnl_claim(NULL, 0, 0, 1, 1);
	(void)fnl_renderer_via(NULL);
	(void)fnl_gpu_count(NULL);
	(void)fnl_gpu_current(NULL);
	(void)fnl_gpu_name_at(NULL, 0);
	(void)fnl_gpu_kind_at(NULL, 0);
	(void)fnl_gpu_renderer_at(NULL, 0);
	(void)fnl_gpu_via_at(NULL, 0);
	fnl_request_gpu(NULL, 0);
	(void)fnl_frame_gpu_ms(NULL);
	(void)fnl_frame_cpu_ms(NULL);
	(void)fnl_input_latency_ms(NULL);
	fnl_set_cost_sampling(NULL, true);
	fnl_set_scene_motion(NULL, true);
	(void)fnl_scene_motion(NULL);
	fnl_set_metrics_panel(NULL, true);
	(void)fnl_metrics_panel(NULL);
	fnl_set_metrics_key(NULL, FNL_KEY_F5);
	(void)fnl_metrics_key(NULL);
	fnl_set_frame_stats(NULL, true);
	(void)fnl_frame_stats(NULL, &st);
	(void)fnl_frame_samples(NULL, &sample, 1, sizeof sample, &lost);
	(void)fnl_frame_shown(NULL, &shown, 1, sizeof shown, &lost);
	fnl_set_transition_ms(NULL, 100);
	(void)fnl_output_refresh_mhz(NULL);
	(void)fnl_mode_count(NULL);
	(void)fnl_mode_get(NULL, 0, &mode);
	(void)fnl_mode_current(NULL, &mode);
	fnl_request_mode(NULL, &mode);
	(void)fnl_output_count(NULL);
	(void)fnl_output_name_at(NULL, 0);
	fnl_request_output(NULL, "x");
	printf("ok\n");
	check(fnl_run(NULL, NULL) == FNL_ERR_INVALID_ARGUMENT,
		  "fnl_run(NULL) is an invalid argument");
}

static void bad_descriptors(void)
{
	FnlApp *app = NULL;
	FnlInitDesc d = desc_for();

	check(fnl_init(NULL, &app) < 0 && !app, "fnl_init(NULL desc) fails");
	check(fnl_init(&d, NULL) == FNL_ERR_INVALID_ARGUMENT,
		  "fnl_init(NULL out) is an invalid argument");

	d.size = 1;
	check(fnl_init(&d, &app) == FNL_ERR_INVALID_ARGUMENT && !app,
		  "a size below every version's floor is refused");
	d.size = sizeof d + 64;
	check(fnl_init(&d, &app) == FNL_ERR_VERSION_MISMATCH && !app,
		  "a size larger than this build knows is a version mismatch");
	d.size = 17;
	check(fnl_init(&d, &app) < 0 && !app,
		  "a size that is not a whole number of fields is refused");
}

#if !defined(_WIN32)
typedef struct { FnlApp *app; FnlResult rc; } OffThread;

static void *run_off_thread(void *p)
{
	OffThread *o = p;
	o->rc = fnl_run(o->app, NULL);
	return NULL;
}
#endif

static void wrong_threads(void)
{
	FnlInitDesc d = desc_for();
	FnlApp *app = NULL;
	if (fnl_init(&d, &app) < 0) {
		check(false, "fnl_init for the thread checks");
		return;
	}
	title_for(app, &d);

	const FnlColor white = { 1, 1, 1, 1 };
	say("overlay calls outside on_overlay draw nothing");
	fnl_overlay_rect(app, 0, 0, 10, 10, white);
	fnl_overlay_text(app, 0, 0, FNL_TEXT_BODY, white, "outside");
	fnl_overlay_flush(app);
	printf("ok\n");

#if !defined(_WIN32)
	OffThread o = { app, FNL_OK };
	pthread_t t;
	if (pthread_create(&t, NULL, run_off_thread, &o) == 0) {
		pthread_join(t, NULL);
		check(o.rc == FNL_ERR_INVALID_ARGUMENT,
			  "fnl_run() off the fnl_init() thread is refused");
	}
#endif

	Run r = { false, 0, 3, true, false, FNL_AA_NONE, false };
	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size = sizeof cb;
	cb.ud = &r;
	cb.on_overlay = on_overlay;
	FnlResult rc = fnl_run(app, &cb);
	check(rc == FNL_OK && r.shutdown_tried && r.frames >= 3,
		  "fnl_shutdown() from a callback is refused, and the run goes on");
	fnl_shutdown(app);
}

/* ---- names the library uses inside ---------------------------------------
 *
 * Global functions of this program's own, with names the library's own
 * internals have. fenestal::static is one object whose only globals are the
 * API, so these link; were the archive to export its internals again, this
 * program would fail to link with a duplicate definition, which is the
 * check. Called, so that nothing is discarded before the linker sees it. */
int app_event(int x);
int create_swapchain(int x);
int text_width(int x);
int render_loop(int x);
int utf8_next(int x);
int app_event(int x)        { return x + 1; }
int create_swapchain(int x) { return x + 2; }
int text_width(int x)       { return x + 3; }
int render_loop(int x)      { return x + 4; }
int utf8_next(int x)        { return x + 5; }

/* ---- main ---------------------------------------------------------------- */

int lifecycle_main(int argc, char **argv)
{
	if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		puts("fenestal-dev lifecycle [<cycles>]\n\n"
			 "  <cycles>  lifetimes in one process, at least 2 (default 20)");
		return EXIT_SUCCESS;
	}
	int cycles = argc > 1 ? atoi(argv[1]) : 20;
	if (cycles < 2)
		cycles = 2;

	check(app_event(0) + create_swapchain(0) + text_width(0) +
		  render_loop(0) + utf8_next(0) == 15,
		  "this program's app_event() and four more are its own");
	null_app();
	bad_descriptors();
	void (*host)(int) = sigint_handler();
	wrong_threads();
	check(sigint_handler() == host,
		  "SIGINT is the host's again after that lifetime");

	Held first = { 0, 0, 0 }, last = { 0, 0, 0 };
	bool nvidia = false;
	for (int i = 0; i < cycles; i++) {
		/* Each lifetime a different antialiasing mode, every other one at a
		 * pinned scene size: the offscreen targets' build and teardown are
		 * part of what a lifetime must give back. */
		static const FnlAntialias AA[3] = { FNL_AA_NONE, FNL_AA_MSAA,
											FNL_AA_MLAA };
		Run r = { false, 0, 8, false, false, AA[i % 3], (i & 1) != 0 };
		FnlResult rc = one_lifetime(&r);
		if (rc < 0 || r.frames < 3) {
			printf("lifetime %d: %s, %d frames\n", i + 1, fnl_result_str(rc),
				   r.frames);
			failures++;
			break;
		}
		nvidia = nvidia || r.on_nvidia;
		if (i == 0) {
			first = held();
#if !defined(_WIN32)
			fds_note(fd_was);
#endif
		}
		last = held();
	}
	char line[96];
	snprintf(line, sizeof line, "%d lifetimes, %s %ld -> %ld", cycles,
			 HELD_NAMES[0], first.a, last.a);
	check(last.a <= first.a, line);
#if !defined(_WIN32)
	if (last.a > first.a)
		fds_report();
#endif
	/* On Windows the second count is GDI objects, and NVIDIA's driver
	 * keeps one for every device that ever had a swapchain: measured with
	 * no fenestal in it (tools/probe/gdi.c), one a device lifetime and none
	 * for a swapchain recreated on a live device, which is what a resize
	 * makes. So on an NVIDIA device a lifetime may cost exactly one, and
	 * not one more; elsewhere, none. */
	long allowed = 0;
#if defined(_WIN32)
	if (nvidia)
		allowed = cycles - 1;
#endif
	snprintf(line, sizeof line, "%d lifetimes, %s %ld -> %ld%s", cycles,
			 HELD_NAMES[1], first.b, last.b,
			 allowed ? " (NVIDIA: one a device)" : "");
	check(last.b <= first.b + allowed, line);
#if defined(_WIN32)
	snprintf(line, sizeof line, "%d lifetimes, %s %ld -> %ld", cycles,
			 HELD_NAMES[2], first.c, last.c);
	check(last.c <= first.c, line);
#endif
	check(sigint_handler() == host,
		  "SIGINT is the host's again after every lifetime");

	printf("%s\n", failures ? "FAILED" : "all held");
	return failures ? 1 : 0;
}

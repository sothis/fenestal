/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The benchmark mode's lifetimes and its run (docs/benchmark-mode.md).
 *
 * Each window is a lifetime of its own -- fnl_init() to fnl_shutdown() --
 * because half the options are the window's at birth (its size, scale,
 * vsync, backend, renderer, GPU), and because a run that inherits a window
 * inherits its history: a GPU ranking, a pacing controller already tuned.
 * So: the setup panel in one window, each run in a fresh one, the results in
 * a third, and a repeat is a run again.
 *
 * A run's clock is a thread of this program's (pace.c's pattern): it holds
 * the settle, the warm-up, the measure and the cool-down, publishing the
 * phase in an atomic the callbacks read, and ends the window with
 * fnl_quit(). Everything measured is measured on the render thread, in the
 * callbacks, which are one thread: an interval is the time between two
 * on_overlay calls, so it is what the application sees of the frame loop.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "bench.h"
#include "testbed.h"

#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <pthread.h>
#  include <unistd.h>
#endif

/* ---- the system's primitives ----------------------------------------------
 *
 * A clock, a sleep, a thread and a lock, written twice as pace.c writes
 * them: this is a caller, and the library's shims are not its to use. */

#if defined(_WIN32)

double bench_now(void)
{
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return f.QuadPart ? (double)c.QuadPart / (double)f.QuadPart : 0.0;
}

static double utc_now(void)
{
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	const ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) |
						ft.dwLowDateTime;
	return (double)t * 1e-7 - 11644473600.0;      /* from 1601 to 1970 */
}

static void nap(double s)
{
	if (s > 0.0) {
		const DWORD ms = (DWORD)(s * 1e3 + 0.5);
		Sleep(ms ? ms : 1);
	}
}

typedef HANDLE BenchThread;
static SRWLOCK g_log_lock = SRWLOCK_INIT;
static void log_lock(void)   { AcquireSRWLockExclusive(&g_log_lock); }
static void log_unlock(void) { ReleaseSRWLockExclusive(&g_log_lock); }

static void set_env(const char *name, const char *value)
{
	char put[256];
	snprintf(put, sizeof put, "%s=%s", name, value ? value : "");
	_putenv(put);
}

static unsigned long process_id(void)
{
	return (unsigned long)GetCurrentProcessId();
}

#else

double bench_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double utc_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void nap(double s)
{
	if (s <= 0.0)
		return;
	struct timespec ts = {
		.tv_sec  = (time_t)s,
		.tv_nsec = (long)((s - (double)(time_t)s) * 1e9),
	};
	nanosleep(&ts, NULL);
}

typedef pthread_t BenchThread;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static void log_lock(void)   { pthread_mutex_lock(&g_log_lock); }
static void log_unlock(void) { pthread_mutex_unlock(&g_log_lock); }

static void set_env(const char *name, const char *value)
{
	if (value)
		setenv(name, value, 1);
	else
		unsetenv(name);
}

static unsigned long process_id(void)
{
	return (unsigned long)getpid();
}

#endif

/* ---- the run's state -------------------------------------------------------
 *
 * One run at a time. The atomics cross threads; every other field is the
 * render thread's while the window is open and the main thread's after
 * fnl_run() returns, the driving thread joined. */

typedef enum {
	PH_WAIT, PH_SETTLE, PH_WARMUP, PH_MEASURE, PH_COOLDOWN, PH_DONE,
} Phase;

static const char *const PHASE_NAMES[] = {
	"waiting", "settle", "warm-up", "measure", "cool-down", "done",
};

static struct {
	FnlApp             *app;
	const BenchConfig  *cfg;
	const FnlCallbacks *scene;
	BenchResult        *r;

	atomic_int  phase;
	atomic_bool abort;          /* the window is going, or Esc */
	/* The GPU ranking is measuring the devices: the window takes no input
	 * and its frames are the ranking's (fenestal.h, FnlInitDesc::gpu_name),
	 * so nothing counts until it is done. Read from the log, which is the
	 * one place the library says so. */
	atomic_bool calibrating;
	atomic_bool calibrated;     /* it ran while this window was open */
	atomic_bool calibrated_late;        /* and began after the settle */
	/* When the phase began and how long it is: the driving thread's,
	 * read by the render thread for the HUD and the series' clock. */
	_Atomic double phase_t0, phase_len;

	/* The render thread's. */
	double last_frame;          /* the last measured on_overlay, or -1 */
	double sample_t;            /* the last row's stamp */
	long long sample_frames, sample_inputs;
	double sample_cpu;          /* user + system seconds then */
	char   present_mode[96];
	bool   seen_measure;
	atomic_bool closed;         /* the measure's last row taken */
	bool   primed;              /* the GPU memory asked once already */
	bool   have_st0;            /* the totals read at the measure's start */
	/* A device lost in the warm-up or the measure, said invalid once one
	 * is made again: fnl_run() ends every run with an on_gpu_lost, and an
	 * aborted run's is no loss. Read once more after the join, for a loss
	 * no device came back from. Render thread's, and the closing one
	 * fnl_run()'s after the join. */
	bool   lost_pending;
	/* The library's rows handed over so far, lost ones counted: a row's
	 * place in FnlFrameStats::frames, which says whether the measure's two
	 * readings of the totals counted it (drain_frames()). */
	unsigned long long rows_seen;
	double measure_t0;          /* its first frame, the series' zero */
	double record_t0;
	int    samples;

	/* The log, while a run's window is open. */
	bool   logging;
} R;

static void add_reason(char *list, size_t n, const char *why)
{
	if (strstr(list, why))
		return;                 /* said once */
	const size_t len = strlen(list);
	snprintf(list + len, n - len, "%s%s", len ? "; " : "", why);
}

static void invalid(const char *why)
{
	add_reason(R.r->invalid, sizeof R.r->invalid, why);
}

static void note(const char *why)
{
	add_reason(R.r->notes, sizeof R.r->notes, why);
}

/* ---- the log ---------------------------------------------------------------
 *
 * Every line goes on to stderr as the library's own sink writes it, built
 * whole and flushed; while a run's window is open it is kept for the record
 * as well. */

#define LOG_KEEP_MAX (256 * 1024)

static void bench_log(void *ud, FnlLogLevel lvl, const char *msg)
{
	(void)ud;
	(void)lvl;
	char line[2048];
	const int n = snprintf(line, sizeof line, "%s\n", msg);
	if (n > 0)
		fwrite(line, 1, (size_t)n < sizeof line ? (size_t)n
												: sizeof line - 1, stderr);
	fflush(stderr);

	/* A calibration ends on one of two lines: "calibrated on" once it has
	 * figures to keep, or "the window left", which is all it says when the
	 * window moved before the first entry was measured (rank.c, finish()). */
	if (strstr(msg, "GPU rank: calibrating")) {
		atomic_store(&R.calibrating, true);
		atomic_store(&R.calibrated, true);
	} else if (strstr(msg, "GPU rank:") &&
			   (strstr(msg, "calibrated on") ||
				strstr(msg, "the window left"))) {
		atomic_store(&R.calibrating, false);
	}

	log_lock();
	if (R.logging && R.r && n > 0) {
		BenchResult *r = R.r;
		const size_t len = (size_t)n < sizeof line ? (size_t)n
												   : sizeof line - 1;
		if (r->log_len + len < LOG_KEEP_MAX) {
			char *p = realloc(r->log, r->log_len + len + 1);
			if (p) {
				memcpy(p + r->log_len, line, len);
				r->log_len += len;
				p[r->log_len] = '\0';
				r->log = p;
			}
		}
	}
	log_unlock();
}

/* ---- what is measured ---------------------------------------------------- */

/* A getter's "not yet" (-1) as NaN, which the record writes as null. */
static double known(double v)
{
	return v < 0.0 ? BENCH_NAN : v;
}

/* The platform, read once a frame is being drawn for the measure, or after
 * the loop for a run that never reached it. `driver` reads the renderer's
 * interop entries (scene_*_driver()), so only from a callback, while the
 * device is the caller's: not after fnl_run() has delivered its last
 * on_gpu_lost. */
static void read_platform(FnlApp *app, const FnlFrame *f, bool driver)
{
	BenchResult *r = R.r;
	snprintf(r->backend, sizeof r->backend, "%s", fnl_backend_name(app));
	snprintf(r->renderer, sizeof r->renderer, "%s", fnl_renderer_name(app));
	snprintf(r->gpu, sizeof r->gpu, "%s", fnl_gpu_name(app));
	snprintf(r->gpu_short, sizeof r->gpu_short, "%s",
			 fnl_gpu_short_name(app));
	snprintf(r->present_mode, sizeof r->present_mode, "%s",
			 fnl_present_mode(app));
	snprintf(r->window_host, sizeof r->window_host, "%s",
			 fnl_window_host(app));
	const char *o = fnl_output_name(app);
	snprintf(r->output, sizeof r->output, "%s", o ? o : "");
	fnl_output_size_pt(app, &r->output_w_pt, &r->output_h_pt);
	r->refresh_mhz = fnl_output_refresh_mhz(app);
	r->scale120 = fnl_scale120(app);
	r->area_w_px = f->area_w_px;
	r->area_h_px = f->area_h_px;
	fnl_window_size_pt(app, &r->window_w_pt, &r->window_h_pt);
	r->samples = f->samples;
	r->fullscreen = fnl_is_fullscreen(app);
	r->scene_motion = fnl_scene_motion(app);

	TestbedDriver d;
	memset(&d, 0, sizeof d);
	bool known = false;
	switch (driver ? fnl_renderer(app) : FNL_RENDERER_DEFAULT) {
#ifdef SCENE_HAS_VULKAN
	case FNL_RENDERER_VULKAN: known = scene_vulkan_driver(app, &d); break;
#endif
#ifdef SCENE_HAS_GL
	case FNL_RENDERER_OPENGL: known = scene_gl_driver(app, &d);     break;
#endif
#ifdef SCENE_HAS_D3D12
	case FNL_RENDERER_D3D12:  known = scene_d3d12_driver(app, &d);  break;
#endif
#ifdef SCENE_HAS_METAL
	case FNL_RENDERER_METAL:  known = scene_metal_driver(app, &d);  break;
#endif
	default:                                                         break;
	}
	if (known) {
		snprintf(r->driver_name, sizeof r->driver_name, "%s", d.name);
		snprintf(r->driver_info, sizeof r->driver_info, "%s", d.info);
		snprintf(r->api_version, sizeof r->api_version, "%s", d.api);
		r->vendor_id = d.vendor_id;
		r->device_id = d.device_id;
	}
}

/* The frame loop's totals now, into `out`; false where none are kept. */
static bool stats_now(FnlApp *app, FnlFrameStats *out)
{
	memset(out, 0, sizeof *out);
	out->size = sizeof *out;
	return fnl_frame_stats(app, out);
}

/* One row of the series, every interval-ms while measuring. */
static void take_sample(FnlApp *app, double now)
{
	BenchResult *r = R.r;
	const double dt = now - R.sample_t;
	if (dt <= 0.0)
		return;
	const double interval = bench_opt_int(R.cfg, OPT_INTERVAL_MS) * 1e-3;
	if (dt > 3.0 * interval)
		invalid("a gap of more than three intervals between two samples");

	const long long frames = r->frames - R.sample_frames;
	const long long inputs = r->inputs - R.sample_inputs;
	const double rate = (double)frames / dt;
	const int fps = fnl_fps(app);
	const double lib_fps = fps >= 0 ? (double)fps : BENCH_NAN;
	const double gpu = known(fnl_frame_gpu_ms(app));
	const double cpu = known(fnl_frame_cpu_ms(app));
	/* The getter keeps its last mean while no input arrives, so it counts
	 * only for an interval that had some. */
	const double input = inputs > 0 ? known(fnl_input_latency_ms(app))
									: BENCH_NAN;
	double user, sys, pct = BENCH_NAN;
	if (testbed_proc_times(&user, &sys)) {
		if (R.sample_cpu >= 0.0)
			pct = 100.0 * (user + sys - R.sample_cpu) / dt;
		R.sample_cpu = user + sys;
	}
	const double rss = known(testbed_proc_rss_mib());
	double vram;
	if (!testbed_vram_mib(app, &vram))
		vram = BENCH_NAN;

	bench_series_add(&r->fps, lib_fps);
	bench_series_add(&r->gpu_ms, gpu);
	bench_series_add(&r->cpu_ms, cpu);
	bench_series_add(&r->input_ms, input);
	bench_series_add(&r->cpu_pct, pct);
	bench_series_add(&r->rss_mib, rss);
	bench_series_add(&r->vram_mib, vram);
	const double row[BENCH_SERIES_COLUMNS] = {
		now - R.measure_t0, (double)frames, rate, lib_fps, gpu, cpu, input,
		pct, rss, vram,
	};
	for (int i = 0; i < BENCH_SERIES_COLUMNS; i++)
		bench_series_add(&r->rows, row[i]);

	if (strcmp(fnl_present_mode(app), R.present_mode) != 0)
		invalid("the present mode changed");

	R.sample_t = now;
	R.sample_frames = r->frames;
	R.sample_inputs = r->inputs;
	R.samples++;
}

/* The library's rows for the frames drawn since the last call, kept where
 * the measure's totals counted them: after the first reading and up to the
 * second (FnlFrameStats::frames), which is where their means are the
 * totals'. A row arrives at the next frame's call at the soonest, and one
 * the GPU timed a few frames later still, with its time
 * (fnl_frame_samples()), so the rows are placed by their count and not by
 * when they came; the last come after the window's loop has ended. */
static void drain_frames(FnlApp *app)
{
	const BenchResult *r = R.r;
	FnlFrameSample buf[128];
	int n;
	do {
		uint64_t lost;
		n = fnl_frame_samples(app, buf, 128, sizeof buf[0], &lost);
		/* The oldest go first, so the lost are ahead of these. */
		const unsigned long long first = R.rows_seen + lost + 1;
		R.rows_seen += lost + (unsigned long long)n;
		if (!R.have_st0)
			continue;
		const unsigned long long lo = r->st0.frames;
		const unsigned long long hi = r->stats ? r->st1.frames : ULLONG_MAX;
		/* The lost that fall inside the measure: [first - lost, first)
		 * against (lo, hi]. */
		const unsigned long long l0 = first - lost > lo + 1 ? first - lost
															: lo + 1;
		const unsigned long long l1 = first - 1 < hi ? first - 1 : hi;
		if (lost && l1 >= l0)
			R.r->lframes_lost += l1 - l0 + 1;
		for (int i = 0; i < n; i++) {
			const unsigned long long at = first + (unsigned long long)i;
			if (at <= lo || at > hi)
				continue;
			const FnlFrameSample *s = &buf[i];
			const double row[BENCH_LFRAME_COLUMNS] = {
				(double)s->index, s->time, (double)s->pace_ms,
				(double)s->fence_ms, (double)s->acquire_ms,
				(double)s->record_ms, (double)s->submit_ms,
				(double)s->present_ms, (double)s->rebuild_ms,
				(double)s->draw_ms, s->presented ? 1.0 : 0.0,
				s->gpu_ms >= 0.0f ? (double)s->gpu_ms : BENCH_NAN,
			};
			for (int k = 0; k < BENCH_LFRAME_COLUMNS; k++)
				bench_series_add(&R.r->lframes, row[k]);
		}
	} while (n == 128);
}

/* The frames the display has said it showed since the last call, kept or
 * thrown away as drain_frames() does. */
static void drain_shown(FnlApp *app, bool keep)
{
	FnlFrameShown buf[64];
	int n;
	do {
		uint64_t lost;
		n = fnl_frame_shown(app, buf, 64, sizeof buf[0], &lost);
		if (!keep)
			continue;
		R.r->shown_lost += lost;
		for (int i = 0; i < n; i++) {
			bench_series_add(&R.r->shown, (double)buf[i].index);
			bench_series_add(&R.r->shown, buf[i].time);
			bench_series_add(&R.r->shown, buf[i].vblank ? 1.0 : 0.0);
		}
	} while (n == 64);
}

/* ---- the callbacks a run's window gets ----------------------------------- */

static void run_gpu_created(void *ud, FnlApp *app)
{
	if (R.lost_pending) {
		invalid("the GPU was lost");
		R.lost_pending = false;
	}
	R.scene->on_gpu_created(ud, app);
}

static void run_gpu_lost(void *ud, FnlApp *app)
{
	const int ph = atomic_load(&R.phase);
	R.lost_pending = ph == PH_WARMUP || ph == PH_MEASURE;
	R.scene->on_gpu_lost(ud, app);
}

static void run_record(void *ud, FnlApp *app, const FnlFrame *f)
{
	const double t0 = bench_now();
	R.scene->on_record(ud, app, f);
	if (atomic_load(&R.phase) == PH_MEASURE)
		bench_series_add(&R.r->record_ms, (bench_now() - t0) * 1e3);
}

static bool is_input(FnlEventType t)
{
	return t == FNL_EVENT_KEY || t == FNL_EVENT_BUTTON ||
		   t == FNL_EVENT_MOTION || t == FNL_EVENT_SCROLL ||
		   t == FNL_EVENT_RELATIVE || t == FNL_EVENT_TEXT;
}

/* What the events say about the run: inputs counted, and anything that
 * changes what is being measured under it named as a disqualifier. */
static void run_event(FnlApp *app, const FnlEvent *ev, int ph,
					  uint64_t frame)
{
	BenchResult *r = R.r;
	const bool counting = ph == PH_WARMUP || ph == PH_MEASURE;
	if (ph == PH_MEASURE) {
		r->events++;
		if (is_input(ev->type)) {
			r->inputs++;
			/* The frame this drain is for shows what the input did; when
			 * the library read it, and when it happened. */
			bench_series_add(&r->inputs_at, (double)frame);
			bench_series_add(&r->inputs_at, ev->time);
			bench_series_add(&r->inputs_at,
							 ev->time - (double)ev->input_age);
		}
	}
	switch (ev->type) {
	case FNL_EVENT_KEY:
		if (ev->key.pressed && ev->key.key == FNL_KEY_ESCAPE) {
			atomic_store(&R.abort, true);
			fnl_quit(app);
		}
		/* The library's own binding, acted on before the event comes:
		 * the scene's clock started or stopped changes whether a frame
		 * records again, the option scene-motion holds for the run -- from
		 * before the warm-up too, whose state the measure inherits. */
		if (ph <= PH_MEASURE && ev->key.pressed &&
			ev->key.key == FNL_KEY_F2)
			invalid("the scene's motion was toggled (F2)");
		break;
	case FNL_EVENT_FOCUS:
		if (counting && !ev->focus.focused)
			invalid("the window lost the focus");
		break;
	case FNL_EVENT_RESIZE:
		if (counting)
			invalid("the window was resized");
		break;
	case FNL_EVENT_SCALE:
		if (counting)
			invalid("the scale changed");
		break;
	case FNL_EVENT_WINDOW:
		if (counting)
			invalid("the window's state changed");
		break;
	case FNL_EVENT_OUTPUT:
		if (counting)
			invalid("the window moved to another output");
		break;
	case FNL_EVENT_GPU:
		if (counting)
			invalid("the window moved to another GPU");
		break;
	case FNL_EVENT_LOST:
		invalid("events were lost");
		break;
	default:
		break;
	}
}

/* The one line over the scene while a run goes on, so a hand at the machine
 * can see what it is waiting for. Constant in cost: one rectangle and one
 * string a frame, whatever it says. */
static void draw_hud(FnlApp *app, int ph)
{
	int cx, cy, cw, ch;
	fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);
	char text[160];
	double left = atomic_load(&R.phase_t0) + atomic_load(&R.phase_len)
				- bench_now();
	if (left < 0.0)
		left = 0.0;
	if (atomic_load(&R.calibrating))
		snprintf(text, sizeof text, "bench: waiting for the GPU ranking");
	else
		snprintf(text, sizeof text, "bench: %s, %.0f s left, run %d of %d"
				 "   Esc aborts", PHASE_NAMES[ph], ceil(left),
				 R.r->repeat + 1, R.r->repeats);
	const int pad = 6;
	const int w = fnl_text_width(text, FNL_TEXT_BODY) + 2 * pad;
	const int h = fnl_text_line(FNL_TEXT_BODY) + 2 * pad;
	fnl_overlay_rect(app, cx + 12, cy + 12, w, h,
					 (FnlColor){ 0.06f, 0.06f, 0.08f, 0.85f });
	fnl_overlay_text(app, cx + 12 + pad, cy + 12 + pad, FNL_TEXT_BODY,
					 TESTBED_RGB(0.80f, 0.85f, 0.95f), text);
}

static void run_overlay(void *ud, FnlApp *app, const FnlFrame *f)
{
	(void)ud;
	const double now = bench_now();
	const int ph = atomic_load(&R.phase);
	BenchResult *r = R.r;

	FnlEvent ev;
	while (fnl_poll_event(app, &ev))
		run_event(app, &ev, ph, f->index);

	/* The GPU memory's first answer, asked before anything counts: for
	 * OpenGL it is the system's (gpumem.c), and opening NVML for it took
	 * 20 ms of one frame on NVIDIA, a 50 ms interval in the measure. */
	if ((ph == PH_SETTLE || ph == PH_WARMUP) && !R.primed) {
		double vram;
		(void)testbed_vram_mib(app, &vram);
		R.primed = true;
	}

	if (ph == PH_MEASURE) {
		if (!R.seen_measure) {
			R.seen_measure = true;
			read_platform(app, f, true);
			snprintf(R.present_mode, sizeof R.present_mode, "%s",
					 fnl_present_mode(app));
			R.have_st0 = stats_now(app, &r->st0);
			drain_frames(app);
			drain_shown(app, false);
			R.sample_t = now;
			R.measure_t0 = now;
			R.sample_frames = 0;
			R.sample_inputs = 0;
			double user, sys;
			R.sample_cpu = testbed_proc_times(&user, &sys) ? user + sys : -1.0;
		} else {
			bench_series_add(&r->interval_ms, (now - R.last_frame) * 1e3);
			drain_frames(app);
			drain_shown(app, true);
		}
		R.last_frame = now;
		r->frames++;
		const double interval = bench_opt_int(R.cfg, OPT_INTERVAL_MS) * 1e-3;
		if (now - R.sample_t >= interval)
			take_sample(app, now);
	} else if (R.seen_measure && !atomic_load(&R.closed)) {
		/* The first frame after the measure: the part interval it ended
		 * in is a row too, its frames all counted by then. */
		if (now - R.sample_t > 0.05)
			take_sample(app, now);
		r->stats = R.have_st0 && stats_now(app, &r->st1);
		drain_frames(app);
		drain_shown(app, true);
		atomic_store(&R.closed, true);
	} else if (r->stats && R.rows_seen < r->st1.frames) {
		/* The measure's rows that waited for their GPU time. */
		drain_frames(app);
	}

	if (bench_opt_bool(R.cfg, OPT_HUD) && ph != PH_DONE)
		draw_hud(app, ph);

	/* Every frame asked for: from in here, "the frame after this one
	 * differs" (fnl_invalidate()), so the window draws as fast as its
	 * pacing lets it and never rests. */
	if (bench_opt_int(R.cfg, OPT_REDRAW) == REDRAW_EVERY_FRAME &&
		ph != PH_DONE)
		fnl_invalidate(app);

	if (ph == PH_MEASURE)
		bench_series_add(&r->overlay_ms, (bench_now() - now) * 1e3);
}

/* ---- the driving thread -------------------------------------------------- */

/* Sleeps `s` seconds in short naps, so an abort is heard within one. False
 * when the run was aborted meanwhile. */
static bool hold(Phase ph, double s)
{
	atomic_store(&R.phase_len, s);
	atomic_store(&R.phase_t0, bench_now());
	atomic_store(&R.phase, (int)ph);
	while (!atomic_load(&R.abort)) {
		/* The GPU ranking starts measuring once the window is up, which
		 * can be inside the settle: the settle then starts again after
		 * it, and anywhere later it is a run measured beside it. */
		if (atomic_load(&R.calibrating)) {
			if (ph != PH_SETTLE) {
				atomic_store(&R.calibrated_late, true);
			} else {
				while (atomic_load(&R.calibrating) &&
					   !atomic_load(&R.abort))
					nap(0.1);
				atomic_store(&R.phase_t0, bench_now());
			}
		}
		const double left = atomic_load(&R.phase_t0) + s - bench_now();
		if (left <= 0.0)
			return true;
		nap(left < 0.02 ? left : 0.02);
	}
	return false;
}

static void drive(void)
{
	const BenchConfig *c = R.cfg;
	bool ok = hold(PH_SETTLE, bench_opt_int(c, OPT_SETTLE_S));
	/* The load from the warm-up on, so the measure starts under it
	 * already settled. */
	if (ok) {
		bench_load_start(c, &R.r->cond);
		ok = hold(PH_WARMUP, bench_opt_int(c, OPT_WARMUP_S));
	}
	if (ok) {
		/* The process's CPU over the measure, from its two edges. */
		double u0, s0, u1, s1;
		const bool had = testbed_proc_times(&u0, &s0);
		const double t0 = bench_now();
		/* The two edges on stderr, for a harness whose hands work the
		 * window while it measures (tools/drive/bench.py --hands), each
		 * flushed: Windows' C runtime buffers a stderr that is a pipe, and
		 * the line then came home after the measure it announced. */
		fprintf(stderr, "bench: measuring, %d s\n",
				bench_opt_int(c, OPT_MEASURE_S));
		fflush(stderr);
		ok = hold(PH_MEASURE, bench_opt_int(c, OPT_MEASURE_S));
		fprintf(stderr, "bench: measured\n");
		fflush(stderr);
		const double dt = bench_now() - t0;
		R.r->measure_s = dt;
		if (had && testbed_proc_times(&u1, &s1) && dt > 0.0) {
			R.r->user_s = u1 - u0;
			R.r->sys_s = s1 - s0;
			R.r->cpu_pct_mean = 100.0 * (u1 - u0 + s1 - s0) / dt;
		}
	}
	if (ok)
		ok = hold(PH_COOLDOWN, bench_opt_int(c, OPT_COOLDOWN_S));
	/* A frame after the measure takes its last row; at rest one comes
	 * within the cost sampling's fifth of a second. */
	const double tail = bench_now() + 0.5;
	while (ok && !atomic_load(&R.closed) && bench_now() < tail)
		nap(0.01);
	bench_load_stop(&R.r->cond);
	atomic_store(&R.phase, (int)PH_DONE);
	if (!ok)
		atomic_store(&R.abort, true);
	fnl_quit(R.app);
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

/* ---- a run's lifetime ---------------------------------------------------- */

/* What the environment said before any run changed it, so that "auto"
 * puts it back. */
static struct {
	char *backend, *renderer, *vsync;
} g_env;

static char *dup_env(const char *name)
{
	const char *v = getenv(name);
	if (!v)
		return NULL;
	const size_t n = strlen(v) + 1;
	char *p = malloc(n);
	if (p)
		memcpy(p, v, n);
	return p;
}

static void env_for_run(const BenchConfig *c)
{
	if (bench_opt_int(c, OPT_BACKEND) == BACKEND_AUTO)
		set_env("FNL_BACKEND", g_env.backend);
	else
		set_env("FNL_BACKEND", bench_opt_enum_name(c, OPT_BACKEND));
	if (bench_opt_int(c, OPT_RENDERER) == RENDERER_AUTO)
		set_env("FNL_RENDERER", g_env.renderer);
	else
		set_env("FNL_RENDERER", bench_opt_enum_name(c, OPT_RENDERER));
	set_env("FNL_VSYNC", bench_opt_bool(c, OPT_VSYNC) ? g_env.vsync : "0");
}

static void new_id(char out[17])
{
	static unsigned long long counter;
	unsigned long long x = (unsigned long long)(utc_now() * 1e6) ^
						   ((unsigned long long)process_id() << 40) ^
						   (++counter * 0x9e3779b97f4a7c15ull);
	/* splitmix64's finaliser: neighbouring inputs, unrelated outputs */
	x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
	x ^= x >> 27; x *= 0x94d049bb133111ebull;
	x ^= x >> 31;
	snprintf(out, 17, "%016llx", x);
}

/* The window's name, as main.c's name_window() makes it, with the mode. */
static void name_bench_window(FnlApp *app, const char *what)
{
	char title[192], how[64];
	testbed_renderer_label(app, how, sizeof how);
	snprintf(title, sizeof title, "Fenestal testbed: %s (%s, %s, %s)", what,
			 fnl_backend_name(app), how, fnl_gpu_short_name(app));
	fnl_set_title(app, title);
}

static FnlAntialias antialias_of(int aa)
{
	return aa == AA_MSAA ? FNL_AA_MSAA : aa == AA_MLAA ? FNL_AA_MLAA
														: FNL_AA_NONE;
}

/* The mode a run asks for before its first frame: WxH, or WxH@Hz, the
 * size's fastest mode where no refresh is named. KMS is the backend that
 * acts on it, its window being the output; a window server's set_mode
 * does nothing, so one option may be passed to every run of a sweep. */
static void request_mode(FnlApp *app, const char *text)
{
	int w = 0, h = 0;
	double hz = 0.0;
	if (sscanf(text, "%dx%d@%lf", &w, &h, &hz) < 2 || w <= 0 || h <= 0) {
		fprintf(stderr, "bench: mode %s is not WxH or WxH@Hz\n", text);
		return;
	}
	FnlMode m = { w, h, hz > 0.0 ? (unsigned)(hz * 1000.0 + 0.5) : 0 };

	if (m.refresh_mhz == 0) {
		FnlMode got;
		/* Largest first and, within a size, fastest first. */
		for (int i = 0; fnl_mode_get(app, i, &got); i++) {
			if (got.w_px == w && got.h_px == h) {
				m = got;
				break;
			}
		}
		if (m.refresh_mhz == 0) {
			fprintf(stderr, "bench: the output has no %dx%d mode\n", w, h);
			return;
		}
	}
	fnl_request_mode(app, &m);
}

/* One run, in a window of its own; false when the window would not open. */
static bool run_once(const FnlInitDesc *base, const FnlCallbacks *scene,
					 const BenchConfig *c, BenchResult *r)
{
	env_for_run(c);
	FnlInitDesc d = *base;
	d.title     = "Fenestal testbed: benchmark";
	d.width_pt  = bench_opt_int(c, OPT_WIDTH_PT);
	d.height_pt = bench_opt_int(c, OPT_HEIGHT_PT);
	d.scale_override120 = bench_opt_scale120(c);
	d.no_late_latch = bench_opt_int(c, OPT_LATE_LATCH) == LATE_OFF;
	d.late_latch_fixed = bench_opt_int(c, OPT_LATE_LATCH) == LATE_FIXED
					   ? bench_opt_num(c, OPT_LATE_LATCH_MS) * 1e-3 : 0.0;
	d.gpu_name = bench_opt_text(c, OPT_GPU)[0] ? bench_opt_text(c, OPT_GPU)
											   : NULL;
	d.frame_times = bench_opt_bool(c, OPT_FRAME_TIMES);
	d.log = bench_log;
	d.log_ud = NULL;

	memset(&R, 0, sizeof R);
	R.cfg = c;
	R.scene = scene;
	R.r = r;
	R.last_frame = -1.0;
	R.sample_cpu = -1.0;
	atomic_store(&R.phase, (int)PH_WAIT);
	log_lock();
	R.logging = true;
	log_unlock();

	r->user_s = r->sys_s = r->cpu_pct_mean = BENCH_NAN;
	r->cond.process_nice = r->cond.load_nice = INT_MIN;
	r->cond.load_cpu_pct = BENCH_NAN;
	/* Before fnl_init(), so every thread it starts inherits them. */
	bench_process_apply(c, &r->cond);
	r->started_utc = utc_now();
	FnlApp *app;
	const FnlResult rc = fnl_init(&d, &app);
	if (rc < 0) {
		fprintf(stderr, "bench: %s: %s\n", fnl_result_str(rc),
				fnl_last_error());
		log_lock();
		R.logging = false;
		log_unlock();
		bench_process_restore(&r->cond);
		return false;
	}
	R.app = app;
	/* No metrics panel in a measured run: one shown by a stray key would
	 * be drawing the run did not record. */
	fnl_set_metrics_key(app, FNL_KEY_UNKNOWN);
	name_bench_window(app, "benchmark");

	/* The RUN rows, before the first frame: [any] calls, so from here. */
	if (bench_opt_int(c, OPT_TRANSITION_MS) >= 0)
		fnl_set_transition_ms(app, bench_opt_int(c, OPT_TRANSITION_MS));
	fnl_set_cost_sampling(app, bench_opt_bool(c, OPT_COST_SAMPLING));
	fnl_set_scene_motion(app, bench_opt_bool(c, OPT_SCENE_MOTION));
	/* Counted from the start, and read at the measure's two ends. */
	fnl_set_frame_stats(app, true);
	if (bench_opt_int(c, OPT_ANTIALIAS) != AA_DEFAULT)
		fnl_set_antialias(app, antialias_of(bench_opt_int(c, OPT_ANTIALIAS)));
	if (bench_opt_bool(c, OPT_FULLSCREEN))
		fnl_set_fullscreen(app, true);
	if (bench_opt_text(c, OPT_MODE)[0])
		request_mode(app, bench_opt_text(c, OPT_MODE));

	FnlCallbacks cb = *scene;
	cb.on_gpu_created = run_gpu_created;
	cb.on_gpu_lost    = run_gpu_lost;
	cb.on_record      = run_record;
	cb.on_overlay     = run_overlay;

#if defined(_WIN32)
	BenchThread t = CreateThread(NULL, 0, drive_thread, NULL, 0, NULL);
	const bool started = t != NULL;
#else
	BenchThread t;
	const bool started = pthread_create(&t, NULL, drive_thread, NULL) == 0;
#endif
	FnlResult run_rc = FNL_OK;
	if (started)
		run_rc = fnl_run(app, &cb);
	else
		fprintf(stderr, "bench: could not start the driving thread\n");

	/* The window is going: closed by hand, by Esc, or by the driver. */
	const bool finished = atomic_load(&R.phase) == PH_DONE &&
						  !atomic_load(&R.abort);
	atomic_store(&R.abort, true);
	if (started) {
#if defined(_WIN32)
		WaitForSingleObject(t, INFINITE);
		CloseHandle(t);
#else
		pthread_join(t, NULL);
#endif
	}
	/* The measure's last rows, which the end of the loop let go. */
	if (R.have_st0)
		drain_frames(app);
	if (!R.seen_measure) {
		FnlFrame f;
		memset(&f, 0, sizeof f);
		fnl_content_size_px(app, &f.area_w_px, &f.area_h_px);
		/* A run that never reached its measure: no driver, whose
		 * device the loop's end has handed back. */
		read_platform(app, &f, false);
	}
	r->peak_rss_mib = testbed_proc_peak_rss_mib();
	r->threads = testbed_proc_threads();
	fnl_shutdown(app);
	log_lock();
	R.logging = false;
	log_unlock();
	bench_process_restore(&r->cond);
	r->finished_utc = utc_now();
	/* What the system refused of the conditions asked. */
	for (const char *p = r->cond.notes; *p;) {
		const char *e = strstr(p, "; ");
		const size_t len = e ? (size_t)(e - p) : strlen(p);
		char one[256];
		snprintf(one, sizeof one, "%.*s", (int)(len < 255 ? len : 255), p);
		note(one);
		p += len;
		if (*p)
			p += 2;
	}

	if (run_rc < 0)
		invalid("the frame loop ended with an error");
	/* A loss the device never came back from: no on_gpu_created said it,
	 * and fnl_run() owed it no closing on_gpu_lost. A finished run's
	 * closing one is at PH_DONE and leaves the flag clear. */
	if (R.lost_pending && finished)
		invalid("the GPU was lost");
	if (finished && r->frames == 0)
		invalid("no frame was drawn while measuring");
	if (finished && r->rows.n == 0)
		invalid("no sample was taken");
	if (atomic_load(&R.calibrated_late))
		invalid("the GPU ranking was measured during the run");
	r->status = !finished ? "aborted" : r->invalid[0] ? "invalid"
													 : "complete";
	if (atomic_load(&R.calibrated))
		note("the GPU ranking was measured first, before the settle");
	if (r->inputs == 0)
		note("no input while measuring, so no input latency");
	if (!r->vram_mib.n || bench_dist(&r->vram_mib).n == 0)
		note("the GPU memory is not known here");
	{
		/* A series keeps its first values and drops the rest, so a long
		 * measure would otherwise describe its start as if it were the
		 * whole. */
		const BenchSeries *all[] = {
			&r->interval_ms, &r->record_ms, &r->overlay_ms, &r->fps,
			&r->gpu_ms, &r->cpu_ms, &r->input_ms, &r->cpu_pct, &r->rss_mib,
			&r->vram_mib, &r->rows, &r->lframes, &r->shown, &r->inputs_at,
		};
		for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
			if (all[i]->capped) {
				note("a series stopped keeping values (its cap, or out "
					 "of memory), so its figures cover only the start of "
					 "the measure");
				break;
			}
	}
	{
		const bool vsync = bench_opt_bool(c, OPT_VSYNC);
		const char *pm = r->present_mode;
		const bool says_off = strstr(pm, "IMMEDIATE") || strstr(pm, "MAILBOX")
						   || strstr(pm, "off") || strstr(pm, "interval 0");
		if (!vsync && !says_off)
			note("vsync was asked off and the present mode is still "
				 "synchronised");
	}
	return true;
}

/* ---- a panel's lifetime -------------------------------------------------- */

static BenchAction panel_once(const FnlInitDesc *base,
							  const FnlCallbacks *scene, BenchConfig *c,
							  const BenchResult *result, const char *written)
{
	/* The library's own defaults, but for the backend and renderer the
	 * run will use, so the panel is shown where the run will be. */
	env_for_run(c);
	set_env("FNL_VSYNC", g_env.vsync);
	FnlInitDesc d = *base;
	d.title = "Fenestal testbed: benchmark";
	d.log = bench_log;
	d.log_ud = NULL;
	FnlApp *app;
	const FnlResult rc = fnl_init(&d, &app);
	if (rc < 0) {
		fprintf(stderr, "bench: %s: %s\n", fnl_result_str(rc),
				fnl_last_error());
		return PANEL_QUIT;
	}
	/* F5 starts the run here (bench_panel.c), so it is not the library's
	 * metrics key as well. */
	fnl_set_metrics_key(app, FNL_KEY_UNKNOWN);
	name_bench_window(app, result ? "results" : "benchmark setup");
	BenchPanel p;
	memset(&p, 0, sizeof p);
	p.config = c;
	p.result = result;
	p.written = written;
	p.hover = -1;
	p.hover_button = -1;
	FnlCallbacks cb = *scene;
	cb.ud = &p;
	cb.on_overlay = bench_panel_overlay;
	fnl_run(app, &cb);
	fnl_shutdown(app);
	/* A window closed without a decision is a quit. */
	return p.action == PANEL_NONE ? PANEL_QUIT : p.action;
}

/* ---- the entry ----------------------------------------------------------- */

typedef struct {
	BenchConfig *c;
	bool ok;
} Replay;

static bool replay_member(void *ud, const char *key, const char *value)
{
	Replay *rp = ud;
	for (int i = 0; i < OPT_COUNT; i++) {
		if (strcmp(BENCH_OPTIONS[i].key, key) != 0)
			continue;
		/* A number is written whole when it is, so an INT reads back. */
		if (!bench_opt_set(rp->c, (BenchOptId)i, value)) {
			rp->ok = false;
			return true;
		}
		/* A record holds every option, so one at its default is not one
		 * the run was given: left unset, as the run had it, or the panel
		 * shows it as chosen and a priority at its default is asked of a
		 * process already nicer, which the system refuses. */
		const BenchOption *o = &BENCH_OPTIONS[i];
		if (o->kind == BENCH_TEXT ? !strcmp(rp->c->text[i], o->def_text)
								  : rp->c->num[i] == o->def)
			rp->c->set[i] = false;
		return true;
	}
	/* A key this program does not have: a newer record's, which this
	 * reader ignores as the schema says. */
	fprintf(stderr, "--bench-replay: no option \"%s\" here; ignored\n", key);
	return true;
}

int testbed_bench_main(const FnlInitDesc *base, const FnlCallbacks *scene,
					   int argc, char **args)
{
	static const char REPLAY[] = "--bench-replay=";
	BenchConfig *c = malloc(sizeof *c);
	if (!c)
		return 1;
	bench_opt_defaults(c);
	bool setup = false, run = false, list = false;

	/* The replay first, wherever it was given: the command line wins
	 * over it. */
	for (int i = 0; i < argc; i++) {
		if (strncmp(args[i], REPLAY, sizeof REPLAY - 1) != 0)
			continue;
		Replay rp = { c, true };
		if (!bench_json_options(args[i] + sizeof REPLAY - 1, replay_member,
								&rp) || !rp.ok) {
			free(c);
			return 1;
		}
	}
	for (int i = 0; i < argc; i++) {
		const char *a = args[i];
		if (!strcmp(a, "--bench")) {
			setup = true;
			continue;
		}
		if (!strcmp(a, "--bench-run")) {
			run = true;
			continue;
		}
		if (!strcmp(a, "--bench-list")) {
			list = true;
			continue;
		}
		if (!strncmp(a, REPLAY, sizeof REPLAY - 1)) {
			run = run || !setup;
			continue;
		}
		bool known;
		if (!bench_opt_arg(c, a, &known)) {
			if (!known)
				fprintf(stderr, "unknown option \"%s\"; --bench-list names "
						"the benchmark's\n", a);
			free(c);
			return 1;
		}
	}
	if (list) {
		bench_opt_list(stdout, c);
		free(c);
		return 0;
	}
	/* Options alone are a setup panel with them filled in. */
	if (!run)
		setup = true;

	g_env.backend  = dup_env("FNL_BACKEND");
	g_env.renderer = dup_env("FNL_RENDERER");
	g_env.vsync    = dup_env("FNL_VSYNC");

	BenchResult *r = calloc(1, sizeof *r);
	int status = r ? 0 : 1;
	char written[1024] = "";
	bool have_result = false;
	BenchAction next = setup ? PANEL_BACK : PANEL_START;
	while (r && next != PANEL_QUIT) {
		if (next == PANEL_BACK) {
			next = panel_once(base, scene, c, NULL, "");
			continue;
		}
		/* PANEL_START or PANEL_RERUN: the runs of one series. */
		const int repeats = bench_opt_int(c, OPT_REPEATS);
		char series[17];
		new_id(series);
		/* The status is the worst of every series the program ran, not the
		 * last one's: a caller reads one exit for all of them. 1 is a
		 * failure -- a run that could not start, or a record not kept --
		 * and outranks 2, a run that did not complete. */
		bool stopped = false;
		for (int k = 0; k < repeats; k++) {
			/* Whatever the last run kept, a failed one's log lines too:
			 * safe on a result never filled. */
			bench_result_free(r);
			memset(r, 0, sizeof *r);
			new_id(r->id);
			snprintf(r->series, sizeof r->series, "%s", series);
			r->repeat = k;
			r->repeats = repeats;
			if (!run_once(base, scene, c, r)) {
				status = 1;
				stopped = true;
				have_result = false;
				break;
			}
			have_result = true;
			if (!bench_record_write(c, r, written, sizeof written))
				status = 1;
			if (strcmp(r->status, "complete") != 0 && status != 1)
				status = 2;
			if (!strcmp(r->status, "aborted"))
				break;
		}
		if (bench_opt_bool(c, OPT_QUIT) || stopped)
			break;
		next = panel_once(base, scene, c, have_result ? r : NULL, written);
	}

	if (r) {
		bench_result_free(r);
		free(r);
	}
	set_env("FNL_BACKEND", g_env.backend);
	set_env("FNL_RENDERER", g_env.renderer);
	set_env("FNL_VSYNC", g_env.vsync);
	free(g_env.backend);
	free(g_env.renderer);
	free(g_env.vsync);
	free(c);
	return status;
}

/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The benchmark's record, `fenestal-bench/1`: one JSON document per run,
 * written to a file of its own, said in one line on stderr, and on stderr
 * whole between marker lines when asked (--bench-emit), which is how a
 * record comes home from a machine this one only has a shell on.
 *
 * Keys end in their unit (_ms, _s, _fps, _pct, _mib, _hz, _n). A metric is
 * {"value": x} or a distribution {n, mean, min, p50, p95, p99, max}, and
 * unknown or not applicable is null, never 0. The version is additive: a
 * renamed key is a new key and a reader ignores keys it does not know.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "bench.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <direct.h>
#else
#  include <sys/stat.h>
#endif

#define SCHEMA "fenestal-bench/1"

/* The library's rows, one a drawn frame (fnl_frame_samples()). */
static const char *const LFRAME_NAMES[BENCH_LFRAME_COLUMNS] = {
	"index_n", "t_s", "pace_ms", "fence_ms", "acquire_ms", "record_ms",
	"submit_ms", "present_ms", "rebuild_ms", "draw_ms", "presented_n",
	"gpu_ms",
};

/* The series' columns, one row an interval; bench.c fills them in this
 * order. */
static const char *const SERIES_NAMES[BENCH_SERIES_COLUMNS] = {
	"t_s", "frames_n", "rate_fps", "library_fps", "gpu_ms", "cpu_ms",
	"input_ms", "cpu_pct", "rss_mib", "vram_mib",
};

/* ---- where it goes ------------------------------------------------------- */

static void utc_text(double t, char *out, size_t n, bool for_file)
{
	const time_t s = (time_t)t;
	struct tm tm;
#if defined(_WIN32)
	const bool got = gmtime_s(&tm, &s) == 0;
#else
	const bool got = gmtime_r(&s, &tm) != NULL;
#endif
	if (!got) {
		snprintf(out, n, "unknown");
		return;
	}
	const int ms = (int)((t - floor(t)) * 1000.0);
	if (for_file)
		snprintf(out, n, "%04d%02d%02dT%02d%02d%02dZ", tm.tm_year + 1900,
				 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	else
		snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
				 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
				 tm.tm_min, tm.tm_sec, ms);
}

/* Lower case, letters and digits, a dash for any run of the rest: a name
 * that goes into a file name on every system. */
static void slug(const char *s, char *out, size_t n)
{
	size_t k = 0;
	bool dash = false;
	for (; *s && k + 1 < n; s++) {
		char ch = *s;
		if (ch >= 'A' && ch <= 'Z')
			ch = (char)(ch - 'A' + 'a');
		if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
			if (dash && k > 0 && k + 2 < n)
				out[k++] = '-';
			dash = false;
			out[k++] = ch;
		} else {
			dash = true;
		}
	}
	out[k] = '\0';
	if (!k)
		snprintf(out, n, "none");
}

static bool make_dir(const char *path)
{
#if defined(_WIN32)
	return _mkdir(path) == 0 || errno == EEXIST;
#else
	return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

/* Every directory down to `path`, itself included. */
static bool make_dirs(const char *path)
{
	char p[1024];
	snprintf(p, sizeof p, "%s", path);
	for (char *s = p + 1; *s; s++) {
		if (*s != '/' && *s != '\\')
			continue;
		const char was = *s;
		*s = '\0';
		if (!(s > p && s[-1] == ':'))       /* a drive's root */
			make_dir(p);
		*s = was;
	}
	return make_dir(p);
}

/* The user's state directory's, as the ledger's is on this system:
 * $XDG_STATE_HOME or ~/.local/state on Linux, Application Support on
 * macOS, %LOCALAPPDATA% on Windows. */
static bool default_dir(char *out, size_t n)
{
#if defined(_WIN32)
	const char *base = getenv("LOCALAPPDATA");
	if (!base || !base[0])
		return false;
	snprintf(out, n, "%s\\fenestal\\bench", base);
#elif defined(__APPLE__)
	const char *home = getenv("HOME");
	if (!home || !home[0])
		return false;
	snprintf(out, n, "%s/Library/Application Support/fenestal/bench", home);
#else
	const char *xdg = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");
	if (xdg && xdg[0])
		snprintf(out, n, "%s/fenestal/bench", xdg);
	else if (home && home[0])
		snprintf(out, n, "%s/.local/state/fenestal/bench", home);
	else
		return false;
#endif
	return true;
}

/* ---- what goes in it ----------------------------------------------------- */

static void value(BenchJson *j, const char *key, double v)
{
	if (!isfinite(v)) {
		bench_json_null(j, key);
		return;
	}
	bench_json_obj(j, key);
	bench_json_num(j, "value", v);
	bench_json_end_obj(j);
}

static void dist(BenchJson *j, const char *key, const BenchSeries *s)
{
	const BenchDist d = bench_dist(s);
	if (!d.n) {
		bench_json_null(j, key);
		return;
	}
	bench_json_obj(j, key);
	bench_json_int(j, "n", (long long)d.n);
	bench_json_num(j, "mean", d.mean);
	bench_json_num(j, "min", d.min);
	bench_json_num(j, "p50", d.p50);
	bench_json_num(j, "p95", d.p95);
	bench_json_num(j, "p99", d.p99);
	bench_json_num(j, "max", d.max);
	bench_json_end_obj(j);
}

/* A list kept as "; "-separated text, as an array of strings. */
static void reasons(BenchJson *j, const char *key, const char *list)
{
	bench_json_arr(j, key);
	const char *p = list;
	while (*p) {
		const char *e = strstr(p, "; ");
		const size_t len = e ? (size_t)(e - p) : strlen(p);
		char one[256];
		snprintf(one, sizeof one, "%.*s", (int)(len < 255 ? len : 255), p);
		bench_json_str(j, NULL, one);
		p += len;
		if (*p)
			p += 2;
	}
	bench_json_end_arr(j);
}

/* What the environment said to the library, since a run set some of it
 * and a harness may have set the rest. */
static void environment(BenchJson *j)
{
	static const char *const VARS[] = {
		"FNL_BACKEND", "FNL_RENDERER", "FNL_GPU", "FNL_VSYNC",
		"FNL_GPU_RANK", "FNL_GPU_DRIVER", "FNL_KMS_LAYOUT",
		"MESA_VK_WSI_DEBUG", "VK_INSTANCE_LAYERS", "DRI_PRIME",
	};
	bench_json_obj(j, "env");
	for (size_t i = 0; i < sizeof VARS / sizeof VARS[0]; i++) {
		const char *v = getenv(VARS[i]);
		if (v)
			bench_json_str(j, VARS[i], v);
		else
			bench_json_null(j, VARS[i]);
	}
	bench_json_end_obj(j);
}

static void options(BenchJson *j, const BenchConfig *c)
{
	bench_json_obj(j, "options");
	for (int i = 0; i < OPT_COUNT; i++) {
		const BenchOption *o = &BENCH_OPTIONS[i];
		switch (o->kind) {
		case BENCH_BOOL:
			bench_json_bool(j, o->key, bench_opt_bool(c, (BenchOptId)i));
			break;
		case BENCH_INT:
		case BENCH_NUM:
			bench_json_num(j, o->key, bench_opt_num(c, (BenchOptId)i));
			break;
		case BENCH_ENUM:
			bench_json_str(j, o->key, bench_opt_enum_name(c, (BenchOptId)i));
			break;
		case BENCH_TEXT:
			bench_json_str(j, o->key, bench_opt_text(c, (BenchOptId)i));
			break;
		}
	}
	bench_json_end_obj(j);
}

/* ---- the frames the display showed (fnl_frame_shown()) -------------------
 *
 * Joined on the frame's index with the library's own rows and with the
 * inputs: both are in index order, as the shown frames are, so each join
 * is one walk. */

#define SHOWN_COLS 3

/* The time the display showed frame `index`, or NaN; `at` is where the
 * walk got to, moved past every row before it. */
static double shown_at(const BenchSeries *s, size_t *at, double index)
{
	while (*at + SHOWN_COLS <= s->n && s->v[*at] < index)
		*at += SHOWN_COLS;
	if (*at + SHOWN_COLS <= s->n && s->v[*at] == index)
		return s->v[*at + 1];
	return BENCH_NAN;
}

static void shown_frame(BenchJson *j, const BenchResult *r)
{
	const size_t n = r->shown.n / SHOWN_COLS;
	if (!n) {
		/* Not heard on this platform, which is not none shown. */
		bench_json_null(j, "shown_n");
		bench_json_null(j, "shown_interval_ms");
		bench_json_null(j, "latch_to_shown_ms");
		return;
	}
	value(j, "shown_n", (double)n);
	BenchSeries gaps = { 0 }, ages = { 0 };
	for (size_t i = 1; i < n; i++)
		bench_series_add(&gaps, (r->shown.v[i * SHOWN_COLS + 1] -
								 r->shown.v[(i - 1) * SHOWN_COLS + 1]) * 1e3);
	/* A frame's age on the screen: from when its state was latched (the
	 * library's row's time) to when the display showed it. */
	size_t at = 0;
	for (size_t k = 0; k + BENCH_LFRAME_COLUMNS <= r->lframes.n;
		 k += BENCH_LFRAME_COLUMNS) {
		const double t = shown_at(&r->shown, &at, r->lframes.v[k]);
		if (isfinite(t))
			bench_series_add(&ages, (t - r->lframes.v[k + 1]) * 1e3);
	}
	dist(j, "shown_interval_ms", &gaps);
	dist(j, "latch_to_shown_ms", &ages);
	bench_series_free(&gaps);
	bench_series_free(&ages);
}

/* From when the library read an input (FnlEvent::time), and from when it
 * happened (FnlEvent::input_age, the device's own stamp where the library
 * shares its clock), to when the frame that drained it was shown. */
static void shown_input(BenchJson *j, const BenchResult *r)
{
	if (!r->shown.n) {
		bench_json_null(j, "read_to_shown_ms");
		bench_json_null(j, "to_shown_ms");
		return;
	}
	BenchSeries read = { 0 }, born = { 0 };
	size_t at = 0;
	for (size_t k = 0; k + 3 <= r->inputs_at.n; k += 3) {
		const double t = shown_at(&r->shown, &at, r->inputs_at.v[k]);
		if (!isfinite(t))
			continue;
		bench_series_add(&read, (t - r->inputs_at.v[k + 1]) * 1e3);
		bench_series_add(&born, (t - r->inputs_at.v[k + 2]) * 1e3);
	}
	dist(j, "read_to_shown_ms", &read);
	dist(j, "to_shown_ms", &born);
	bench_series_free(&read);
	bench_series_free(&born);
}

/* What the run got of the machine (bench_load.c), as read back. */
static void conditions(BenchJson *j, const BenchConditions *k)
{
	bench_json_obj(j, "conditions");
	bench_json_str(j, "process_affinity", k->process_affinity[0]
										  ? k->process_affinity : NULL);
	if (k->process_nice != INT_MIN)
		bench_json_int(j, "process_nice", k->process_nice);
	else
		bench_json_null(j, "process_nice");
	bench_json_int(j, "load_threads_n", k->load_threads);
	bench_json_num(j, "load_duty_pct", k->load_threads ? k->load_duty_pct
													   : BENCH_NAN);
	bench_json_str(j, "load_affinity", k->load_threads &&
				   k->load_affinity[0] ? k->load_affinity : NULL);
	if (k->load_threads && k->load_nice != INT_MIN)
		bench_json_int(j, "load_nice", k->load_nice);
	else
		bench_json_null(j, "load_nice");
	bench_json_num(j, "load_cpu_pct", k->load_cpu_pct);
	bench_json_end_obj(j);
}

/* The frame loop's own account of the measure (fnl_frame_stats()): each
 * phase's mean per frame drawn, from the totals at its two ends. */
static void library(BenchJson *j, const BenchResult *r)
{
	const FnlFrameStats *a = &r->st0, *b = &r->st1;
	if (!r->stats || b->frames < a->frames) {
		bench_json_null(j, "library");
		return;
	}
	const double n = (double)(b->frames - a->frames);
	const double per = n > 0 ? 1e-3 / n : BENCH_NAN;
	bench_json_obj(j, "library");
	value(j, "frames_n", n);
	value(j, "presents_n", (double)(b->presents - a->presents));
	value(j, "rebuilds_n", (double)(b->rebuilds - a->rebuilds));
	value(j, "pace_ms", (double)(b->pace_us - a->pace_us) * per);
	value(j, "fence_ms", (double)(b->fence_us - a->fence_us) * per);
	value(j, "acquire_ms", (double)(b->acquire_us - a->acquire_us) * per);
	value(j, "record_ms", (double)(b->record_us - a->record_us) * per);
	value(j, "submit_ms", (double)(b->submit_us - a->submit_us) * per);
	value(j, "present_ms", (double)(b->present_us - a->present_us) * per);
	value(j, "rebuild_ms", (double)(b->rebuild_us - a->rebuild_us) * per);
	value(j, "draw_ms", (double)(b->draw_us - a->draw_us) * per);
	const double gn = (double)(b->gpu_samples - a->gpu_samples);
	value(j, "gpu_ms", gn > 0 ? (double)(b->gpu_us - a->gpu_us) / 1e3 / gn
							  : BENCH_NAN);
	value(j, "gpu_samples_n", gn);
	value(j, "gpu_idle_pct", n > 0 ? 100.0 * (double)(b->gpu_idle -
													  a->gpu_idle) / n
								   : BENCH_NAN);
	/* Cycles are counted where the window server invites frames alone,
	 * so none elsewhere is "not known", not "none missed". */
	const double cycles = (double)(b->cycles - a->cycles);
	value(j, "cycles_n", cycles > 0 ? cycles : BENCH_NAN);
	value(j, "missed_n", cycles > 0 ? (double)(b->missed - a->missed)
									: BENCH_NAN);
	value(j, "period_ms", b->period_ms > 0 ? b->period_ms : BENCH_NAN);
	value(j, "hold_ms", b->hold_ms);
	/* Each phase across the frames, from the library's own rows: where a
	 * slow frame's time went, which the means above cannot say. */
	static const int SPREAD[] = { 2, 4, 7, 9, 11 };  /* pace, acquire, ... */
	for (size_t i = 0; i < sizeof SPREAD / sizeof SPREAD[0]; i++) {
		BenchSeries col = { 0 };
		/* Whole rows alone, as the other readers take them: a series cut
		 * at BENCH_SAMPLES_MAX ends inside one. */
		for (size_t k = 0; k + BENCH_LFRAME_COLUMNS <= r->lframes.n;
			 k += BENCH_LFRAME_COLUMNS)
			bench_series_add(&col, r->lframes.v[k + (size_t)SPREAD[i]]);
		char key[48];
		snprintf(key, sizeof key, "frame_%s", LFRAME_NAMES[SPREAD[i]]);
		dist(j, key, &col);
		bench_series_free(&col);
	}
	value(j, "frames_lost_n", (double)r->lframes_lost);
	bench_json_end_obj(j);
}

static void document(BenchJson *j, const BenchConfig *c,
					 const BenchResult *r)
{
	char t0[96], t1[96];
	utc_text(r->started_utc, t0, sizeof t0, false);
	utc_text(r->finished_utc, t1, sizeof t1, false);
	const uint32_t v = fnl_version();
	char ver[24];
	snprintf(ver, sizeof ver, "%u.%u.%u", (unsigned)(v >> 16),
			 (unsigned)((v >> 8) & 0xff), (unsigned)(v & 0xff));

	bench_json_obj(j, NULL);
	bench_json_str(j, "schema", SCHEMA);

	bench_json_obj(j, "run");
	bench_json_str(j, "id", r->id);
	bench_json_str(j, "series", r->series);
	bench_json_int(j, "repeat", r->repeat);
	bench_json_int(j, "repeats_n", r->repeats);
	bench_json_str(j, "started_utc", t0);
	bench_json_str(j, "finished_utc", t1);
	bench_json_str(j, "label", bench_opt_text(c, OPT_LABEL));
	bench_json_str(j, "program", "fenestal-testbed");
	bench_json_str(j, "library_version", ver);
	bench_json_str(j, "status", r->status);
	reasons(j, "invalid_reasons", r->invalid);
	reasons(j, "notes", r->notes);
	bench_json_end_obj(j);

	char host[128], os[256], cpu[128];
	int cpus;
	double mem;
	testbed_machine(host, sizeof host, os, sizeof os, cpu, sizeof cpu, &cpus,
					&mem);
	bench_json_obj(j, "machine");
	bench_json_str(j, "host", host[0] ? host : NULL);
	bench_json_str(j, "os", os[0] ? os : NULL);
	bench_json_str(j, "cpu", cpu[0] ? cpu : NULL);
	if (cpus > 0)
		bench_json_int(j, "cpus_n", cpus);
	else
		bench_json_null(j, "cpus_n");
	bench_json_num(j, "memory_mib", mem > 0 ? floor(mem) : BENCH_NAN);
	bench_json_end_obj(j);

	bench_json_obj(j, "platform");
	bench_json_str(j, "backend", r->backend);
	bench_json_str(j, "renderer", r->renderer);
	bench_json_str(j, "gpu", r->gpu);
	bench_json_str(j, "gpu_short", r->gpu_short);
	bench_json_str(j, "window_host", r->window_host[0] ? r->window_host
														: NULL);
	bench_json_str(j, "present_mode", r->present_mode);
	bench_json_str(j, "output", r->output[0] ? r->output : NULL);
	bench_json_int(j, "output_w_pt", r->output_w_pt);
	bench_json_int(j, "output_h_pt", r->output_h_pt);
	bench_json_num(j, "refresh_hz", r->refresh_mhz ? r->refresh_mhz / 1000.0
												   : BENCH_NAN);
	bench_json_int(j, "scale_120", r->scale120);
	bench_json_int(j, "window_w_pt", r->window_w_pt);
	bench_json_int(j, "window_h_pt", r->window_h_pt);
	bench_json_int(j, "area_w_px", r->area_w_px);
	bench_json_int(j, "area_h_px", r->area_h_px);
	bench_json_int(j, "samples_n", r->samples);
	bench_json_bool(j, "fullscreen", r->fullscreen);
	bench_json_bool(j, "scene_motion", r->scene_motion);
	bench_json_obj(j, "driver");
	bench_json_str(j, "name", r->driver_name[0] ? r->driver_name : NULL);
	bench_json_str(j, "info", r->driver_info[0] ? r->driver_info : NULL);
	bench_json_str(j, "api", r->api_version[0] ? r->api_version : NULL);
	if (r->vendor_id) {
		char id[16];
		snprintf(id, sizeof id, "%04x", r->vendor_id);
		bench_json_str(j, "vendor_id", id);
		snprintf(id, sizeof id, "%04x", r->device_id);
		bench_json_str(j, "device_id", id);
	} else {
		bench_json_null(j, "vendor_id");
		bench_json_null(j, "device_id");
	}
	bench_json_end_obj(j);
	environment(j);
	bench_json_end_obj(j);

	options(j, c);
	conditions(j, &r->cond);

	bench_json_obj(j, "metrics");
	bench_json_obj(j, "frame");
	value(j, "measure_s", r->measure_s);
	value(j, "frames_n", (double)r->frames);
	value(j, "rate_fps", r->measure_s > 0 ? (double)r->frames / r->measure_s
										  : BENCH_NAN);
	dist(j, "interval_ms", &r->interval_ms);
	value(j, "low_1pct_fps", bench_low_fps(&r->interval_ms, 0.01));
	value(j, "low_01pct_fps", bench_low_fps(&r->interval_ms, 0.001));
	dist(j, "library_fps", &r->fps);
	dist(j, "gpu_ms", &r->gpu_ms);
	dist(j, "cpu_ms", &r->cpu_ms);
	dist(j, "record_ms", &r->record_ms);
	dist(j, "overlay_ms", &r->overlay_ms);
	shown_frame(j, r);
	bench_json_end_obj(j);
	bench_json_obj(j, "input");
	dist(j, "latency_ms", &r->input_ms);
	shown_input(j, r);
	value(j, "events_n", (double)r->events);
	value(j, "inputs_n", (double)r->inputs);
	bench_json_end_obj(j);
	bench_json_obj(j, "process");
	value(j, "cpu_pct", r->cpu_pct_mean);
	value(j, "user_s", r->user_s);
	value(j, "sys_s", r->sys_s);
	dist(j, "rss_mib", &r->rss_mib);
	value(j, "peak_rss_mib", r->peak_rss_mib >= 0 ? r->peak_rss_mib
												  : BENCH_NAN);
	value(j, "threads_n", r->threads >= 0 ? (double)r->threads : BENCH_NAN);
	bench_json_end_obj(j);
	bench_json_obj(j, "gpu");
	dist(j, "vram_mib", &r->vram_mib);
	bench_json_end_obj(j);
	library(j, r);
	bench_json_end_obj(j);

	bench_json_obj(j, "series");
	bench_json_arr(j, "columns");
	for (int i = 0; i < BENCH_SERIES_COLUMNS; i++)
		bench_json_str(j, NULL, SERIES_NAMES[i]);
	bench_json_end_arr(j);
	bench_json_arr(j, "rows");
	for (size_t i = 0; i + BENCH_SERIES_COLUMNS <= r->rows.n;
		 i += BENCH_SERIES_COLUMNS)
		bench_json_row(j, r->rows.v + i, BENCH_SERIES_COLUMNS);
	bench_json_end_arr(j);
	bench_json_end_obj(j);

	if (bench_opt_bool(c, OPT_FRAMES)) {
		bench_json_obj(j, "frames");
		bench_json_arr(j, "columns");
		bench_json_str(j, NULL, "interval_ms");
		bench_json_str(j, NULL, "record_ms");
		bench_json_str(j, NULL, "overlay_ms");
		bench_json_end_arr(j);
		bench_json_arr(j, "rows");
		size_t n = r->interval_ms.n;
		if (r->overlay_ms.n > n) n = r->overlay_ms.n;
		for (size_t i = 0; i < n; i++) {
			const double row[3] = {
				i < r->interval_ms.n ? r->interval_ms.v[i] : BENCH_NAN,
				i < r->record_ms.n   ? r->record_ms.v[i]   : BENCH_NAN,
				i < r->overlay_ms.n  ? r->overlay_ms.v[i]  : BENCH_NAN,
			};
			bench_json_row(j, row, 3);
		}
		bench_json_end_arr(j);
		bench_json_end_obj(j);
	} else {
		bench_json_null(j, "frames");
	}

	if (bench_opt_bool(c, OPT_FRAMES) && r->lframes.n) {
		bench_json_obj(j, "library_frames");
		bench_json_arr(j, "columns");
		for (int i = 0; i < BENCH_LFRAME_COLUMNS; i++)
			bench_json_str(j, NULL, LFRAME_NAMES[i]);
		bench_json_end_arr(j);
		bench_json_arr(j, "rows");
		for (size_t i = 0; i + BENCH_LFRAME_COLUMNS <= r->lframes.n;
			 i += BENCH_LFRAME_COLUMNS)
			bench_json_row(j, r->lframes.v + i, BENCH_LFRAME_COLUMNS);
		bench_json_end_arr(j);
		bench_json_end_obj(j);
	} else {
		bench_json_null(j, "library_frames");
	}

	if (bench_opt_bool(c, OPT_FRAMES) && r->shown.n) {
		static const char *const NAMES[SHOWN_COLS] = {
			"index_n", "t_s", "vblank_n",
		};
		bench_json_obj(j, "shown_frames");
		bench_json_arr(j, "columns");
		for (int i = 0; i < SHOWN_COLS; i++)
			bench_json_str(j, NULL, NAMES[i]);
		bench_json_end_arr(j);
		bench_json_arr(j, "rows");
		for (size_t i = 0; i + SHOWN_COLS <= r->shown.n; i += SHOWN_COLS)
			bench_json_row(j, r->shown.v + i, SHOWN_COLS);
		bench_json_end_arr(j);
		bench_json_end_obj(j);
	} else {
		bench_json_null(j, "shown_frames");
	}

	if (bench_opt_bool(c, OPT_LIBRARY_LOG) && r->log) {
		bench_json_arr(j, "library_log");
		const char *p = r->log;
		while (*p) {
			const char *e = strchr(p, '\n');
			const size_t len = e ? (size_t)(e - p) : strlen(p);
			char line[1024];
			snprintf(line, sizeof line, "%.*s",
					 (int)(len < sizeof line - 1 ? len : sizeof line - 1), p);
			bench_json_str(j, NULL, line);
			p += len;
			if (*p)
				p++;
		}
		bench_json_end_arr(j);
	} else {
		bench_json_null(j, "library_log");
	}
	bench_json_end_obj(j);
}

/* ---- writing it ---------------------------------------------------------- */

bool bench_record_write(const BenchConfig *c, const BenchResult *r,
						char *path, size_t n)
{
	path[0] = '\0';
	BenchJson j;
	bench_json_init(&j);
	document(&j, c, r);
	if (j.failed) {
		fprintf(stderr, "bench: out of memory writing the record\n");
		bench_json_free(&j);
		return false;
	}

	char dir[768];
	const char *out = bench_opt_text(c, OPT_OUT);
	if (out[0])
		snprintf(dir, sizeof dir, "%s", out);
	else if (!default_dir(dir, sizeof dir))
		snprintf(dir, sizeof dir, ".");

	char stamp[96], host[128], os[8], cpu[8], gpu[64], label[64];
	int cpus;
	double mem;
	utc_text(r->started_utc, stamp, sizeof stamp, true);
	testbed_machine(host, sizeof host, os, sizeof os, cpu, sizeof cpu, &cpus,
					&mem);
	slug(host, host, sizeof host);
	slug(r->gpu_short, gpu, sizeof gpu);
	slug(bench_opt_text(c, OPT_LABEL), label, sizeof label);
	char be[32], re[32];
	slug(r->backend, be, sizeof be);
	slug(r->renderer, re, sizeof re);

	bool ok = make_dirs(dir);
	if (ok) {
		snprintf(path, n, "%s/bench-%s-%s-%s-%s-%s%s%s-%s.json", dir, stamp,
				 host, be, re, gpu,
				 bench_opt_text(c, OPT_LABEL)[0] ? "-" : "",
				 bench_opt_text(c, OPT_LABEL)[0] ? label : "", r->id);
		FILE *f = fopen(path, "wb");
		ok = f && fwrite(j.buf, 1, j.len, f) == j.len;
		if (f) {
			fputc('\n', f);
			if (fclose(f) != 0)
				ok = false;
		}
	}
	if (!ok) {
		fprintf(stderr, "bench: could not write the record under %s (%s)\n",
				dir, strerror(errno));
		path[0] = '\0';
	}

	const BenchDist iv = bench_dist(&r->interval_ms);
	fprintf(stderr, "bench: %s, %.1f s, %.1f fps, interval p50 %.2f ms "
			"p99 %.2f ms, run %d of %d -> %s\n", r->status, r->measure_s,
			r->measure_s > 0 ? (double)r->frames / r->measure_s : 0.0,
			iv.n ? iv.p50 : 0.0, iv.n ? iv.p99 : 0.0, r->repeat + 1,
			r->repeats, path[0] ? path : "nowhere");
	if (r->invalid[0])
		fprintf(stderr, "bench: invalid: %s\n", r->invalid);
	if (bench_opt_bool(c, OPT_EMIT)) {
		fprintf(stderr, "==== %s begin %s ====\n", SCHEMA, r->id);
		fwrite(j.buf, 1, j.len, stderr);
		fprintf(stderr, "\n==== %s end %s ====\n", SCHEMA, r->id);
	}
	fflush(stderr);
	bench_json_free(&j);
	return ok;
}

void bench_result_free(BenchResult *r)
{
	BenchSeries *all[] = {
		&r->interval_ms, &r->record_ms, &r->overlay_ms, &r->fps, &r->gpu_ms,
		&r->cpu_ms, &r->input_ms, &r->cpu_pct, &r->rss_mib, &r->vram_mib,
		&r->rows, &r->lframes, &r->shown, &r->inputs_at,
	};
	for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
		bench_series_free(all[i]);
	free(r->log);
	r->log = NULL;
	r->log_len = 0;
}

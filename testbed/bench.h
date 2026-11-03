/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The testbed's benchmark mode (docs/benchmark-mode.md): a run that measures
 * what a frame costs with an application in it, under options every one of
 * which is recorded, and writes one `fenestal-bench/1` document per run.
 *
 * Files: bench.c the lifetimes, the run and its driving thread;
 * bench_options.c the option table, which the command line, the panel, the
 * record and a replay all read; bench_panel.c the setup and results panels;
 * bench_stats.c the distributions; bench_json.c the writer and the replay's
 * reader; bench_record.c the document; bench_load.c the machine's
 * conditions. Like the rest of the testbed they include
 * <fenestal/fenestal.h> and nothing else of the library's.
 */

#ifndef TESTBED_BENCH_H
#define TESTBED_BENCH_H

#include <fenestal/fenestal.h>

#include <stdbool.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>

/* "Not known", which the record writes as null: NAN is a float in C, and
 * one promoted in a conditional is a warning in this tree. */
#define BENCH_NAN ((double)NAN)

/* ---- bench.c: the entry from main.c --------------------------------------
 *
 * `base` is the descriptor main.c built, `scene` its scene's callbacks (no
 * on_overlay); `args` the `--bench*` arguments, in order. Returns the
 * process's exit status: 0 when every run was complete, 1 for a usage error
 * or a window that would not open, 2 when a run was invalid or aborted. */
int demo_bench_main(const FnlInitDesc *base, const FnlCallbacks *scene,
					int argc, char **args);

/* The clock every bench stamp is on: monotonic seconds. */
double bench_now(void);

/* ---- bench_options.c: one table, four readers ---------------------------- */

typedef enum {
	BENCH_BOOL, BENCH_INT, BENCH_NUM, BENCH_ENUM, BENCH_TEXT,
} BenchKind;

/* When a row takes effect: at fnl_init() (a new lifetime), at the run's
 * start inside its lifetime, or only in the record. */
typedef enum { BENCH_INIT, BENCH_RUN, BENCH_RECORD } BenchWhen;

typedef enum {
	BENCH_PAGE_RUN, BENCH_PAGE_WINDOW, BENCH_PAGE_DEVICE, BENCH_PAGE_LOAD,
	BENCH_PAGE_RECORD, BENCH_PAGE_COUNT
} BenchPage;

/* The rows, in the table's order. Appended, as the record's keys are. */
typedef enum {
	OPT_LABEL, OPT_REDRAW, OPT_SETTLE_S, OPT_WARMUP_S, OPT_MEASURE_S,
	OPT_COOLDOWN_S, OPT_REPEATS, OPT_INTERVAL_MS, OPT_FRAMES, OPT_HUD,
	OPT_WIDTH_PT, OPT_HEIGHT_PT, OPT_FULLSCREEN, OPT_SCALE, OPT_VSYNC,
	OPT_LATE_LATCH, OPT_LATE_LATCH_MS, OPT_ANTIALIAS,
	OPT_BACKEND, OPT_RENDERER, OPT_GPU, OPT_TRANSITION_MS, OPT_COST_SAMPLING,
	OPT_FRAME_TIMES,
	OPT_OUT, OPT_EMIT, OPT_QUIT, OPT_LIBRARY_LOG, OPT_NOTES,
	OPT_SCENE_MOTION,
	OPT_LOAD_THREADS, OPT_LOAD_DUTY, OPT_LOAD_AFFINITY, OPT_LOAD_PRIORITY,
	OPT_PROCESS_AFFINITY, OPT_PRIORITY,
	OPT_COUNT
} BenchOptId;

/* The enumerations' values, as their rows name them. */
enum { REDRAW_EVERY_FRAME, REDRAW_AT_REST };
enum { LATE_AUTO, LATE_OFF, LATE_FIXED };
enum { AA_DEFAULT, AA_NONE, AA_MSAA, AA_MLAA };
enum { BACKEND_AUTO };     /* then the names, BENCH_BACKENDS */
enum { RENDERER_AUTO };    /* then the names, BENCH_RENDERERS */
enum { SCALE_DESKTOP };    /* then the factors, bench_opt_scale120() */

#define BENCH_TEXT_MAX 256

typedef struct {
	double num[OPT_COUNT];               /* BOOL, INT, NUM, ENUM's index */
	char   text[OPT_COUNT][BENCH_TEXT_MAX];
	bool   set[OPT_COUNT];               /* given, not defaulted */
} BenchConfig;

typedef struct {
	const char *key;        /* --bench-<key>=, and the record's */
	const char *label;      /* the panel's */
	BenchKind   kind;
	double      min, max, step, def;
	const char *const *names;            /* ENUM's, NULL-terminated */
	const char *def_text;
	BenchWhen   when;
	BenchPage   page;
	const char *help;
} BenchOption;

extern const BenchOption BENCH_OPTIONS[OPT_COUNT];
extern const char *const BENCH_PAGE_NAMES[BENCH_PAGE_COUNT];

void bench_opt_defaults(BenchConfig *c);
/* "--bench-<key>=<value>", or "--bench-<key>" for a BOOL's true. False,
 * with a line on stderr, for a key not in the table or a value out of
 * its range; `known` false when the key is not a row at all. */
bool bench_opt_arg(BenchConfig *c, const char *arg, bool *known);
/* One row from its value as text: the command line's and a replay's. */
bool bench_opt_set(BenchConfig *c, BenchOptId id, const char *value);
/* The panel's step: a BOOL flips, an INT or NUM moves by its step, an
 * ENUM to the next name; `dir` is +1 or -1. Clamped, never wrapped but
 * for an ENUM and a BOOL. */
void bench_opt_step(BenchConfig *c, BenchOptId id, int dir);
/* The value as the panel and --bench-list print it. */
void bench_opt_format(const BenchConfig *c, BenchOptId id, char *out,
					  size_t n);
void bench_opt_list(FILE *f, const BenchConfig *c);

int         bench_opt_int(const BenchConfig *c, BenchOptId id);
double      bench_opt_num(const BenchConfig *c, BenchOptId id);
bool        bench_opt_bool(const BenchConfig *c, BenchOptId id);
const char *bench_opt_text(const BenchConfig *c, BenchOptId id);
const char *bench_opt_enum_name(const BenchConfig *c, BenchOptId id);
/* The surface scale the row asks for, in 120ths; 0 for the desktop's. */
int         bench_opt_scale120(const BenchConfig *c);

/* ---- bench_json.c: the writer, and the replay's reader -------------------
 *
 * The writer builds into a growing buffer, so a document can go to a file
 * and to stderr alike; NaN and the infinities are written as null, which is
 * what JSON has for them. Keys are written as given and must need no
 * escaping. */
typedef struct {
	char  *buf;
	size_t len, cap;
	int    depth;
	bool   first;           /* nothing yet at this depth */
	bool   failed;          /* an allocation failed: the document is lost */
} BenchJson;

void bench_json_init(BenchJson *j);
void bench_json_free(BenchJson *j);
void bench_json_obj(BenchJson *j, const char *key);    /* key NULL in arrays */
void bench_json_arr(BenchJson *j, const char *key);
void bench_json_end_obj(BenchJson *j);
void bench_json_end_arr(BenchJson *j);
void bench_json_str(BenchJson *j, const char *key, const char *v);
void bench_json_num(BenchJson *j, const char *key, double v);
void bench_json_int(BenchJson *j, const char *key, long long v);
void bench_json_bool(BenchJson *j, const char *key, bool v);
void bench_json_null(BenchJson *j, const char *key);
/* A row of numbers on one line, for a series: NaN as null. */
void bench_json_row(BenchJson *j, const double *v, int n);

/* The document at `path`'s "options" object, each member handed to `fn` as
 * its key and its value as text (a string's unescaped, a number's or a
 * bool's as written). False, with a line on stderr, when the file cannot be
 * read or has no such object. */
bool bench_json_options(const char *path,
						bool (*fn)(void *ud, const char *key,
								   const char *value),
						void *ud);

/* ---- bench_stats.c: what a series of values comes to --------------------- */

typedef struct {
	double *v;
	size_t  n, cap;
	bool    capped;         /* values were dropped past BENCH_SAMPLES_MAX */
} BenchSeries;

#define BENCH_SAMPLES_MAX ((size_t)1 << 20)

void bench_series_add(BenchSeries *s, double v);
void bench_series_free(BenchSeries *s);

/* The seven figures a distribution is recorded as; n 0 where there was
 * nothing, and every other figure NaN then. Sorts a copy. */
typedef struct {
	size_t n;
	double mean, min, p50, p95, p99, max;
} BenchDist;

BenchDist bench_dist(const BenchSeries *s);
/* 1000 over the mean of the slowest ceil(n * frac) intervals, in ms: the
 * "1 % low" frame rate for frac 0.01; NaN with no intervals. */
double bench_low_fps(const BenchSeries *intervals_ms, double frac);

/* ---- bench_load.c: the machine's conditions ------------------------------
 *
 * What a run got of what it asked: CPU lists as "0-3,6", "" where every
 * CPU or unknown; a nice INT_MIN where unknown. Notes, "; "-separated, say
 * what the system refused. */
typedef struct {
	char   process_affinity[128];
	int    process_nice;
	int    load_threads, load_duty_pct;
	char   load_affinity[128];
	int    load_nice;
	double load_cpu_pct;        /* per load thread, of one core; NaN none */
	char   notes[384];
} BenchConditions;

/* Whether `spec` is a CPU list this system can name; "" is. */
bool bench_cpu_list_valid(const char *spec);
/* Before fnl_init(), on the main thread: the process's CPUs and nice, as
 * the options ask, and what it then has. Put back by the restore, after
 * fnl_shutdown(). */
void bench_process_apply(const BenchConfig *c, BenchConditions *out);
/* A refusal to put the nice (the priority class on Windows) back is
 * added to `out`'s notes; the CPUs are put back without a word. */
void bench_process_restore(BenchConditions *out);
/* The load threads, from the warm-up to the end of the cool-down. */
void bench_load_start(const BenchConfig *c, BenchConditions *out);
void bench_load_stop(BenchConditions *out);

/* ---- bench_record.c: the document ---------------------------------------- */

/* One measured run, as bench.c gathers it; the record writes it. */
typedef struct {
	/* run */
	char   id[17];
	char   series[17];
	int    repeat, repeats;
	double started_utc, finished_utc;    /* seconds since 1970 */
	const char *status;                  /* complete, aborted, invalid */
	char   invalid[512];                 /* reasons, "; "-separated */
	char   notes[512];                   /* notes, the same */
	/* platform, read before the window closed */
	char   backend[32], renderer[32], gpu[256], gpu_short[128];
	char   present_mode[96], window_host[64], output[64];
	int    output_w_pt, output_h_pt, scale120;
	unsigned refresh_mhz;       /* fnl_output_refresh_mhz(), 0 unknown */
	int    area_w_px, area_h_px, window_w_pt, window_h_pt, samples;
	bool   fullscreen, scene_motion;
	BenchConditions cond;
	/* The GPU driver, as the renderer's API names it; "" unknown. */
	char   driver_name[256], driver_info[256], api_version[160];
	unsigned vendor_id, device_id;
	/* The frame loop's totals at the measure's two ends
	 * (fnl_frame_stats()); `stats` false where either was not read. */
	bool          stats;
	FnlFrameStats st0, st1;
	/* The frame loop's own rows for the measure's frames
	 * (fnl_frame_samples()), BENCH_LFRAME_COLUMNS values each, and how
	 * many the library dropped before they were read. */
	BenchSeries   lframes;
	unsigned long long lframes_lost;
	/* The frames the display says it showed (fnl_frame_shown()): index,
	 * time and whether on a vblank, three values a row; and each input
	 * taken while measuring, as the frame that drained it, when it was
	 * read and when it happened (FnlEvent::input_age), three a row.
	 * Joined in the record. */
	BenchSeries   shown, inputs_at;
	unsigned long long shown_lost;
	/* metrics */
	double measure_s;                    /* measured, not asked */
	long long frames;                    /* on_overlay calls while measuring */
	long long events, inputs;
	BenchSeries interval_ms, record_ms, overlay_ms;
	BenchSeries fps, gpu_ms, cpu_ms, input_ms, cpu_pct, rss_mib, vram_mib;
	double user_s, sys_s, cpu_pct_mean;  /* over the measure */
	double peak_rss_mib;
	int    threads;
	/* series: one row per interval */
	BenchSeries rows;                    /* SERIES_COLUMNS values a row */
	/* the library's log while the window was open */
	char  *log;
	size_t log_len;
} BenchResult;

#define BENCH_LFRAME_COLUMNS 12  /* see bench_record.c's LFRAME_NAMES */
#define BENCH_SERIES_COLUMNS 10  /* see bench_record.c's SERIES_NAMES */

/* Writes the document to its file and says where on stderr, one line,
 * and the whole of it on stderr between marker lines with `emit`. The
 * file's path is put in `path`; false when it could not be written. */
bool bench_record_write(const BenchConfig *c, const BenchResult *r,
						char *path, size_t n);
void bench_result_free(BenchResult *r);

/* ---- bench_panel.c: the setup and the results ---------------------------- */

typedef enum {
	PANEL_NONE, PANEL_START, PANEL_QUIT, PANEL_RERUN, PANEL_BACK,
} BenchAction;

/* The panel's on_overlay, for a lifetime that shows it: drains the events,
 * draws the panel, claims its controls, and on a decision stores it and
 * calls fnl_quit(). `result` NULL shows the setup; otherwise the last
 * run's summary with what was written. */
typedef struct {
	BenchConfig       *config;
	const BenchResult *result;
	const char        *written;          /* the file, or "" */
	BenchAction        action;
	/* the panel's own state */
	int  page, row, hover, hover_button;
	bool editing;
	char edit[BENCH_TEXT_MAX];
} BenchPanel;

void bench_panel_overlay(void *ud, FnlApp *app, const FnlFrame *frame);

/* ---- proc.c: this process and this machine, for the record --------------- */

/* User and system CPU seconds; false where the system will not say. */
bool   demo_proc_times(double *user_s, double *sys_s);
/* The most resident memory this process has had, in MiB; -1 unknown. */
double demo_proc_peak_rss_mib(void);
/* Threads in this process; -1 unknown. */
int    demo_proc_threads(void);
/* The machine: host name, OS and its version, CPU model, logical CPUs,
 * memory in MiB; "" and -1 for what the system will not say. */
void   demo_machine(char *host, size_t host_n, char *os, size_t os_n,
					char *cpu, size_t cpu_n, int *cpus, double *mem_mib);

#endif /* TESTBED_BENCH_H */

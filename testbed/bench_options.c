/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The benchmark's options: one table, read by the command line
 * (--bench-<key>=<value>), the setup panel, the record's "options" object
 * and a replay of a record. A row added here is in all four at once, which
 * is the point of having one table; --bench-list prints it.
 */

#include "bench.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *const REDRAW[] = { "every-frame", "at-rest", NULL };
static const char *const LATE[]   = { "auto", "off", "fixed", NULL };
static const char *const AA[]     = { "default", "none", "msaa", "mlaa",
									  NULL };
static const char *const SCALES[] = { "desktop", "1", "1.25", "1.5", "2",
									  "3", NULL };
/* As FNL_BACKEND and FNL_RENDERER name them, after "auto", which leaves
 * whatever the environment says standing. */
static const char *const BACKENDS[]  = { "auto", "wayland", "x11", "kms",
										 "win32", "cocoa", NULL };
static const char *const RENDERERS[] = { "auto", "vulkan", "gl", "d3d12",
										 "metal", NULL };

const char *const BENCH_PAGE_NAMES[BENCH_PAGE_COUNT] = {
	"Run", "Window", "Device", "Load", "Record",
};

#define B(key, label, def, when, page, help) \
	{ key, label, BENCH_BOOL, 0, 1, 1, def, NULL, NULL, when, page, help }
#define I(key, label, lo, hi, st, def, when, page, help) \
	{ key, label, BENCH_INT, lo, hi, st, def, NULL, NULL, when, page, help }
#define N(key, label, lo, hi, st, def, when, page, help) \
	{ key, label, BENCH_NUM, lo, hi, st, def, NULL, NULL, when, page, help }
#define E(key, label, names, when, page, help) \
	{ key, label, BENCH_ENUM, 0, 0, 1, 0, names, NULL, when, page, help }
#define T(key, label, def, when, page, help) \
	{ key, label, BENCH_TEXT, 0, 0, 0, 0, NULL, def, when, page, help }

const BenchOption BENCH_OPTIONS[OPT_COUNT] = {
	[OPT_LABEL] = T("label", "Label", "", BENCH_RECORD, BENCH_PAGE_RUN,
		"a name for the run, in the record and its file name"),
	[OPT_REDRAW] = E("redraw", "Redraw", REDRAW, BENCH_RUN, BENCH_PAGE_RUN,
		"every-frame asks for each frame, so the rate is the window's; "
		"at-rest asks for none and reads the cost sampling's 5 Hz"),
	[OPT_SETTLE_S] = I("settle-s", "Settle", 0, 60, 1, 4, BENCH_RUN,
		BENCH_PAGE_RUN, "seconds after the window opens before anything "
		"counts: the pacing's controllers wait out sixty presents"),
	[OPT_WARMUP_S] = I("warmup-s", "Warm-up", 0, 60, 1, 2, BENCH_RUN,
		BENCH_PAGE_RUN, "seconds drawn as measured, and not counted"),
	[OPT_MEASURE_S] = I("measure-s", "Measure", 1, 3600, 1, 20, BENCH_RUN,
		BENCH_PAGE_RUN, "seconds measured"),
	[OPT_COOLDOWN_S] = I("cooldown-s", "Cool-down", 0, 60, 1, 0, BENCH_RUN,
		BENCH_PAGE_RUN, "seconds drawn after the measure, not counted"),
	[OPT_REPEATS] = I("repeats", "Repeats", 1, 100, 1, 1, BENCH_RUN,
		BENCH_PAGE_RUN, "runs, each a window of its own, one series"),
	[OPT_INTERVAL_MS] = I("interval-ms", "Sample every", 100, 10000, 100,
		1000, BENCH_RUN, BENCH_PAGE_RUN,
		"milliseconds between two rows of the series"),
	[OPT_FRAMES] = B("frames", "Keep frames", 0, BENCH_RECORD,
		BENCH_PAGE_RUN, "every frame's interval and callback times in the "
		"record, not only their distributions"),
	[OPT_HUD] = B("hud", "Progress line", 1, BENCH_RUN, BENCH_PAGE_RUN,
		"one line of text over the scene while the run goes on"),

	[OPT_WIDTH_PT] = I("width-pt", "Width", 320, 7680, 40, 1280, BENCH_INIT,
		BENCH_PAGE_WINDOW, "presentable area, logical points"),
	[OPT_HEIGHT_PT] = I("height-pt", "Height", 240, 4320, 40, 720,
		BENCH_INIT, BENCH_PAGE_WINDOW, "presentable area, logical points"),
	[OPT_FULLSCREEN] = B("fullscreen", "Fullscreen", 0, BENCH_INIT,
		BENCH_PAGE_WINDOW, "asked for before the first frame"),
	[OPT_SCALE] = E("scale", "Scale", SCALES, BENCH_INIT, BENCH_PAGE_WINDOW,
		"surface scale; desktop is the window server's"),
	[OPT_VSYNC] = B("vsync", "Vsync", 1, BENCH_INIT, BENCH_PAGE_WINDOW,
		"off is FNL_VSYNC=0"),
	[OPT_LATE_LATCH] = E("late-latch", "Late latch", LATE, BENCH_INIT,
		BENCH_PAGE_WINDOW, "the hold before a frame's state is sampled: "
		"self-tuned, off, or fixed at late-latch-ms"),
	[OPT_LATE_LATCH_MS] = N("late-latch-ms", "Late latch ms", 0.5, 50, 0.5,
		4, BENCH_INIT, BENCH_PAGE_WINDOW, "the fixed hold"),
	[OPT_ANTIALIAS] = E("antialias", "Antialias", AA, BENCH_RUN,
		BENCH_PAGE_WINDOW, "default leaves the library's"),

	[OPT_BACKEND] = E("backend", "Backend", BACKENDS, BENCH_INIT,
		BENCH_PAGE_DEVICE, "auto leaves FNL_BACKEND as it is"),
	[OPT_RENDERER] = E("renderer", "Renderer", RENDERERS, BENCH_INIT,
		BENCH_PAGE_DEVICE, "auto leaves FNL_RENDERER as it is"),
	[OPT_GPU] = T("gpu", "GPU", "", BENCH_INIT, BENCH_PAGE_DEVICE,
		"part of the device's name; empty leaves FNL_GPU as it is"),
	[OPT_TRANSITION_MS] = I("transition-ms", "Transition ms", -1, 5000, 50,
		-1, BENCH_RUN, BENCH_PAGE_DEVICE,
		"the dissolve across a GPU move; -1 the library's"),
	[OPT_COST_SAMPLING] = B("cost-sampling", "Cost sampling", 1, BENCH_RUN,
		BENCH_PAGE_DEVICE, "keep the GPU and CPU figures measured at rest"),
	[OPT_FRAME_TIMES] = B("frame-times", "Frame times", 0, BENCH_INIT,
		BENCH_PAGE_DEVICE, "the library's --frame-times lines, in the log"),

	[OPT_OUT] = T("out", "Directory", "", BENCH_RECORD, BENCH_PAGE_RECORD,
		"where the record goes; empty is the user's state directory"),
	[OPT_EMIT] = B("emit", "Emit on stderr", 0, BENCH_RECORD,
		BENCH_PAGE_RECORD, "the whole record on stderr as well, between "
		"marker lines, for a harness on another machine"),
	[OPT_QUIT] = B("quit", "Quit after", 0, BENCH_RECORD, BENCH_PAGE_RECORD,
		"exit after the runs instead of showing the results"),
	[OPT_LIBRARY_LOG] = B("library-log", "Keep the log", 1, BENCH_RECORD,
		BENCH_PAGE_RECORD, "the library's log lines in the record"),
	[OPT_NOTES] = T("notes", "Notes", "", BENCH_RECORD, BENCH_PAGE_RECORD,
		"free text, in the record"),
	[OPT_SCENE_MOTION] = B("scene-motion", "Scene motion", 0, BENCH_RUN,
		BENCH_PAGE_WINDOW, "the scene's clock running, as F2 does: every "
		"frame records again (fnl_set_scene_motion())"),

	[OPT_LOAD_THREADS] = I("load-threads", "Load threads", 0, 64, 1, 0,
		BENCH_RUN, BENCH_PAGE_LOAD, "threads of background CPU load, from "
		"the warm-up to the end of the cool-down"),
	[OPT_LOAD_DUTY] = I("load-duty", "Load duty %", 10, 100, 10, 100,
		BENCH_RUN, BENCH_PAGE_LOAD, "how much of each 10 ms slot a load "
		"thread is busy"),
	[OPT_LOAD_AFFINITY] = T("load-affinity", "Load CPUs", "", BENCH_RUN,
		BENCH_PAGE_LOAD, "the CPUs the load runs on, as 0-3,6; empty is "
		"any (Linux, Windows)"),
	[OPT_LOAD_PRIORITY] = I("load-priority", "Load nice", -20, 19, 1, 0,
		BENCH_RUN, BENCH_PAGE_LOAD, "the load threads' nice, where set; "
		"below the process's wants a privilege (Linux; Windows' nearest "
		"thread level)"),
	[OPT_PROCESS_AFFINITY] = T("process-affinity", "Process CPUs", "",
		BENCH_INIT, BENCH_PAGE_LOAD, "the CPUs every thread of the program "
		"runs on, set before the window opens; empty is any (Linux, "
		"Windows)"),
	[OPT_PRIORITY] = I("priority", "Process nice", -20, 19, 1, 0,
		BENCH_INIT, BENCH_PAGE_LOAD, "the program's nice, where set, before "
		"the window opens; lower wants a privilege (Linux, macOS; "
		"Windows' nearest priority class)"),
};

#undef B
#undef I
#undef N
#undef E
#undef T

static int names_count(const char *const *names)
{
	int n = 0;
	while (names[n])
		n++;
	return n;
}

void bench_opt_defaults(BenchConfig *c)
{
	memset(c, 0, sizeof *c);
	for (int i = 0; i < OPT_COUNT; i++) {
		const BenchOption *o = &BENCH_OPTIONS[i];
		c->num[i] = o->def;
		if (o->kind == BENCH_TEXT)
			snprintf(c->text[i], sizeof c->text[i], "%s", o->def_text);
	}
}

static bool parse_bool(const char *v, double *out)
{
	if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "on") ||
		!strcmp(v, "yes")) {
		*out = 1;
		return true;
	}
	if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off") ||
		!strcmp(v, "no")) {
		*out = 0;
		return true;
	}
	return false;
}

bool bench_opt_set(BenchConfig *c, BenchOptId id, const char *v)
{
	const BenchOption *o = &BENCH_OPTIONS[id];
	double x = 0;
	switch (o->kind) {
	case BENCH_BOOL:
		if (!parse_bool(v, &x)) {
			fprintf(stderr, "--bench-%s: expected on or off; got \"%s\"\n",
					o->key, v);
			return false;
		}
		break;
	case BENCH_INT:
	case BENCH_NUM: {
		char *end;
		errno = 0;
		x = strtod(v, &end);
		if (errno || !*v || *end || !(x >= o->min && x <= o->max) ||
			(o->kind == BENCH_INT && x != floor(x))) {
			fprintf(stderr, "--bench-%s: expected %s from %g to %g; got "
					"\"%s\"\n", o->key,
					o->kind == BENCH_INT ? "a whole number" : "a number",
					o->min, o->max, v);
			return false;
		}
		break;
	}
	case BENCH_ENUM: {
		const int n = names_count(o->names);
		int k = 0;
		while (k < n && strcmp(o->names[k], v) != 0)
			k++;
		if (k == n) {
			fprintf(stderr, "--bench-%s: expected one of", o->key);
			for (int i = 0; i < n; i++)
				fprintf(stderr, "%s %s", i ? "," : "", o->names[i]);
			fprintf(stderr, "; got \"%s\"\n", v);
			return false;
		}
		x = k;
		break;
	}
	case BENCH_TEXT:
		if (strlen(v) >= BENCH_TEXT_MAX) {
			fprintf(stderr, "--bench-%s: longer than %d bytes\n", o->key,
					BENCH_TEXT_MAX - 1);
			return false;
		}
		if ((id == OPT_LOAD_AFFINITY || id == OPT_PROCESS_AFFINITY) &&
			!bench_cpu_list_valid(v)) {
			fprintf(stderr, "--bench-%s: expected CPUs as 0-3,6; got "
					"\"%s\"\n", o->key, v);
			return false;
		}
		snprintf(c->text[id], sizeof c->text[id], "%s", v);
		c->set[id] = true;
		return true;
	}
	c->num[id] = x;
	c->set[id] = true;
	return true;
}

bool bench_opt_arg(BenchConfig *c, const char *arg, bool *known)
{
	static const char PREFIX[] = "--bench-";
	*known = false;
	if (strncmp(arg, PREFIX, sizeof PREFIX - 1) != 0)
		return false;
	const char *key = arg + sizeof PREFIX - 1;
	const char *eq = strchr(key, '=');
	const size_t klen = eq ? (size_t)(eq - key) : strlen(key);
	for (int i = 0; i < OPT_COUNT; i++) {
		const BenchOption *o = &BENCH_OPTIONS[i];
		if (strlen(o->key) != klen || strncmp(o->key, key, klen) != 0)
			continue;
		*known = true;
		if (!eq) {
			if (o->kind != BENCH_BOOL) {
				fprintf(stderr, "--bench-%s wants a value: "
						"--bench-%s=<value>\n", o->key, o->key);
				return false;
			}
			return bench_opt_set(c, (BenchOptId)i, "on");
		}
		return bench_opt_set(c, (BenchOptId)i, eq + 1);
	}
	return false;
}

void bench_opt_step(BenchConfig *c, BenchOptId id, int dir)
{
	const BenchOption *o = &BENCH_OPTIONS[id];
	double x = c->num[id];
	switch (o->kind) {
	case BENCH_BOOL:
		x = x != 0 ? 0 : 1;
		break;
	case BENCH_INT:
	case BENCH_NUM:
		x += dir * o->step;
		if (x < o->min) x = o->min;
		if (x > o->max) x = o->max;
		break;
	case BENCH_ENUM: {
		const int n = names_count(o->names);
		x = (double)((((int)x + dir) % n + n) % n);
		break;
	}
	case BENCH_TEXT:
		return;
	}
	c->num[id] = x;
	c->set[id] = true;
}

void bench_opt_format(const BenchConfig *c, BenchOptId id, char *out,
					  size_t n)
{
	const BenchOption *o = &BENCH_OPTIONS[id];
	switch (o->kind) {
	case BENCH_BOOL:
		snprintf(out, n, "%s", c->num[id] != 0 ? "on" : "off");
		break;
	case BENCH_INT:
		snprintf(out, n, "%d", (int)c->num[id]);
		break;
	case BENCH_NUM:
		snprintf(out, n, "%g", c->num[id]);
		break;
	case BENCH_ENUM:
		snprintf(out, n, "%s", o->names[(int)c->num[id]]);
		break;
	case BENCH_TEXT:
		snprintf(out, n, "%s", c->text[id]);
		break;
	}
}

void bench_opt_list(FILE *f, const BenchConfig *c)
{
	static const char *const WHEN[] = { "init", "run", "record" };
	for (int p = 0; p < BENCH_PAGE_COUNT; p++) {
		fprintf(f, "%s\n", BENCH_PAGE_NAMES[p]);
		for (int i = 0; i < OPT_COUNT; i++) {
			const BenchOption *o = &BENCH_OPTIONS[i];
			if ((int)o->page != p)
				continue;
			char v[BENCH_TEXT_MAX];
			bench_opt_format(c, (BenchOptId)i, v, sizeof v);
			fprintf(f, "  --bench-%-15s %-12s %-6s %s", o->key,
					v[0] ? v : "\"\"", WHEN[o->when], o->help);
			if (o->kind == BENCH_ENUM) {
				fprintf(f, " (");
				for (int k = 0; o->names[k]; k++)
					fprintf(f, "%s%s", k ? ", " : "", o->names[k]);
				fprintf(f, ")");
			} else if (o->kind == BENCH_INT || o->kind == BENCH_NUM) {
				fprintf(f, " (%g to %g)", o->min, o->max);
			}
			fprintf(f, "\n");
		}
	}
}

int bench_opt_int(const BenchConfig *c, BenchOptId id)
{
	return (int)c->num[id];
}

double bench_opt_num(const BenchConfig *c, BenchOptId id)
{
	return c->num[id];
}

bool bench_opt_bool(const BenchConfig *c, BenchOptId id)
{
	return c->num[id] != 0;
}

const char *bench_opt_text(const BenchConfig *c, BenchOptId id)
{
	return c->text[id];
}

const char *bench_opt_enum_name(const BenchConfig *c, BenchOptId id)
{
	return BENCH_OPTIONS[id].names[(int)c->num[id]];
}

int bench_opt_scale120(const BenchConfig *c)
{
	const int k = (int)c->num[OPT_SCALE];
	if (k == SCALE_DESKTOP)
		return 0;
	return (int)(atof(SCALES[k]) * 120.0 + 0.5);
}

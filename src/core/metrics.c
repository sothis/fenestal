/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The metrics panel (fnl_set_metrics_panel()): the library's own readout of
 * what it measures, on every renderer, shown and hidden by a key of the
 * caller's choosing (fnl_set_metrics_key(), F5 until set).
 *
 * The frame's figures are the draw state's -- fps, the two costs and the
 * input latency, which the frame loop already publishes for the public
 * getters. The process's are sampled here, on the render thread and only
 * while the panel is shown, the way each system keeps them: getrusage() and
 * /proc/self/statm on Linux, getrusage() and the task's basic info on
 * macOS, GetProcessTimes() and K32GetProcessMemoryInfo() -- kernel32's, so
 * the link line does not grow -- on Windows; and the GPU memory is the
 * renderer's (FnlRendererVtbl::gpu_memory). They are operating-system
 * questions and not a window server's, so they are answered here behind
 * #ifdefs rather than through the backend table, as signals are in app.c.
 *
 * The panel reads every figure from the draw state it is drawn for, so what
 * it shows is what the frame latched, and a figure that moves re-records
 * the frame as fps does. Shown, it keeps no frames drawn at rest beyond
 * the ones its own process figures move, and those are kept out of the
 * frame's figures (App::cost_counts): a panel that drew frames to measure
 * would be measuring itself. DESIGN *The metrics panel*.
 */

#include "core/app.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <psapi.h>
#else
#  include <sys/resource.h>
#  include <time.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <mach/mach.h>
#  endif
#endif

/* ---- this process ------------------------------------------------------- */

/* Seconds of CPU this process has used, and seconds of a clock that only
 * goes forward; false where the system will not say. */
static bool proc_times(double *cpu, double *wall)
{
#if defined(_WIN32)
	FILETIME created, ended, kernel, user;
	if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel,
						 &user))
		return false;
	const ULONGLONG k = ((ULONGLONG)kernel.dwHighDateTime << 32) |
						kernel.dwLowDateTime;
	const ULONGLONG u = ((ULONGLONG)user.dwHighDateTime << 32) |
						user.dwLowDateTime;
	*cpu = (double)(k + u) * 1e-7;          /* 100 ns units */
#else
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) != 0)
		return false;
	*cpu = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec * 1e-6 +
		   (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec * 1e-6;
#endif
	*wall = now_seconds();
	return true;
}

/* The resident set, in MiB; negative where the system will not say. */
static double proc_rss_mib(void)
{
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS pmc;
	if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
		return -1.0;
	return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
#elif defined(__APPLE__)
	mach_task_basic_info_data_t info;
	mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
				  (task_info_t)&info, &n) != KERN_SUCCESS)
		return -1.0;
	return (double)info.resident_size / (1024.0 * 1024.0);
#else
	FILE *f = fopen("/proc/self/statm", "r");
	if (!f)
		return -1.0;
	long size = 0, resident = 0;
	const int got = fscanf(f, "%ld %ld", &size, &resident);
	fclose(f);
	const long page = sysconf(_SC_PAGESIZE);
	if (got != 2 || page <= 0)
		return -1.0;
	return (double)resident * (double)page / (1024.0 * 1024.0);
#endif
}

/* A figure as the draw state carries it: whole units, METRICS_UNKNOWN for
 * none, and clamped below it so a real figure is never read as none. */
static uint32_t met_u32(double v)
{
	if (!(v >= 0.0))
		return METRICS_UNKNOWN;
	if (v >= (double)(METRICS_UNKNOWN - 1u))
		return METRICS_UNKNOWN - 1u;
	return (uint32_t)(v + 0.5);
}

void app_metrics_sample(App *a, double now)
{
	if (!fnl_atomic_load_bool(&a->metrics_shown)) {
		/* So a panel shown again measures its two shares from then, not
		 * over however long it was hidden, and shows dashes rather than
		 * the figures it was hidden with until its first sample: that is
		 * taken after the frame that shows it, where the window draws. */
		a->met_at = 0.0;
		a->met_cpu_s = -1.0;
		a->met_gpu_at = 0.0;
		a->met_cpu_pm = a->met_ram_mib = a->met_vram_mib = METRICS_UNKNOWN;
		a->met_gpu_pm = METRICS_UNKNOWN;
		return;
	}
	if (a->met_at > 0.0 && now >= a->met_at &&
		now - a->met_at < METRICS_WINDOW)
		return;
	a->met_at = now;

	/* The share of one core since the last sample: several busy threads
	 * go past 100 %. Unknown on the first, which has nothing to measure
	 * from, and on the one after a reading the system refused, which
	 * leaves none to measure from either. */
	double cpu, wall;
	if (proc_times(&cpu, &wall)) {
		a->met_cpu_pm = a->met_cpu_s >= 0.0 && wall > a->met_wall_s
			? met_u32(1000.0 * (cpu - a->met_cpu_s) / (wall - a->met_wall_s))
			: METRICS_UNKNOWN;
		a->met_cpu_s = cpu;
		a->met_wall_s = wall;
	} else {
		a->met_cpu_pm = METRICS_UNKNOWN;
		a->met_cpu_s = -1.0;
	}

	/* The share of the GPU's time the window's frames took since the last
	 * sample, every frame read in it -- the ones drawn at rest only to
	 * show the figures too, which fnl_frame_gpu_ms() leaves out: those
	 * are what the window takes of the GPU at rest, and a share from the
	 * frame's figures (its GPU time by fnl_fps()) would stand at the last
	 * moving frames' however long the window rests. Unknown on the first,
	 * as the CPU's is, and after a move to another device
	 * (app_frame_cost_reset()). */
	a->met_gpu_pm = a->met_gpu_at > 0.0 && now > a->met_gpu_at
		? met_u32(1000.0 * (a->met_gpu_s - a->met_gpu_base) /
				  (now - a->met_gpu_at))
		: METRICS_UNKNOWN;
	a->met_gpu_base = a->met_gpu_s;
	a->met_gpu_at = now;
	a->met_ram_mib = met_u32(proc_rss_mib());
	double vram = -1.0;
	if (!RENDERER(a)->gpu_memory || !RENDERER(a)->gpu_memory(a, &vram))
		vram = -1.0;
	a->met_vram_mib = met_u32(vram);
}

/* ---- the panel --------------------------------------------------------------
 *
 * The grid, in character cells of the monospaced body face: two columns of
 * MET_ROWS rows, each row a label, a figure right-aligned in a field of its
 * own and a unit, so a column's figures end on one edge whatever they read,
 * and every field as wide as the widest it can hold, so the panel does not
 * breathe as the figures move. */
#define MET_TITLE     "Metrics"
#define MET_FPS_MAX   99999
#define MET_FPS_TEXT  "99999 FPS"
#define MET_LABEL     6       /* "Input " */
#define MET_FIGURE    7       /* "9999.99", "99999.9", "9999999" */
#define MET_UNIT      4       /* " fps", " MiB" */
#define MET_COLUMN    (MET_LABEL + MET_FIGURE + MET_UNIT)
#define MET_GUTTER    3
#define MET_ROWS      4
#define MET_PAD       10      /* the panel's padding, points */
#define MET_MARGIN    12      /* from the presentable area's edges */

static int met_panel_w(void)
{
	const int adv = text_advance(TEXT_BODY);
	const int head = text_width(MET_TITLE, TEXT_BODY) + 2 * adv +
					 text_width(MET_FPS_TEXT, TEXT_LARGE);
	const int grid = (2 * MET_COLUMN + MET_GUTTER) * adv;
	return (head > grid ? head : grid) + 2 * MET_PAD;
}

static int met_panel_h(void)
{
	/* The header and its gap, the column headings, the rows, and the
	 * caption half a line below them. */
	const int line = FONT_LINE_PT[TEXT_BODY];
	return (3 + MET_ROWS + 1) * line + line / 2 + 2 * MET_PAD;
}

/* A figure right-aligned in MET_FIGURE cells, clamped to the largest that
 * fits there at its decimals (0 to 2), or a dash where there is none. */
static void met_figure(char out[16], double v, int decimals)
{
	static const double TOP[] = { 9999999.0, 99999.9, 9999.99 };
	if (v < 0.0)
		snprintf(out, 16, "%*s", MET_FIGURE, "-");
	else
		snprintf(out, 16, "%*.*f", MET_FIGURE, decimals,
				 v > TOP[decimals] ? TOP[decimals] : v);
}

/* One row of a column from its left edge: the label grey, the figure bright
 * -- in `fc`, which says something only for the frame's halves and the rate
 * -- and the unit dimmer; a figure not known is a dash as dim as its
 * unit. */
static void met_row(App *a, int x, int y, const char *label, double v,
					int decimals, const float *fc, const char *unit)
{
	const int adv = text_advance(TEXT_BODY);
	char fig[16];
	met_figure(fig, v, decimals);
	const int fx = x + MET_LABEL * adv;
	text_draw(a, x, y, TEXT_BODY, a->pal[COL_ICON_OFF], label);
	text_draw(a, fx, y, TEXT_BODY, v < 0.0 ? a->pal[COL_CTL_DIM] : fc, fig);
	text_draw(a, fx + (MET_FIGURE + 1) * adv, y, TEXT_BODY,
			  a->pal[COL_CTL_DIM], unit);
}

/* A draw-state figure in its unit: microseconds as milliseconds, tenths
 * as whole ones; negative for none. */
static double met_value(uint32_t v, uint32_t unknown, double scale)
{
	return v == unknown ? -1.0 : (double)v * scale;
}

FnlResult app_metrics_draw(App *a, const FnlDrawState *ds, int w, int h)
{
	if (!ds->metrics)
		return FNL_OK;
	const int pw = met_panel_w(), ph = met_panel_h();
	/* The presentable area's top right, under whatever bar there is; and
	 * nowhere where the window cannot hold it whole. */
	const int x = w - MET_MARGIN - pw, y = ds->bar_h + MET_MARGIN;
	if (x < MET_MARGIN || y + ph > h - MET_MARGIN)
		return FNL_OK;

	/* Everything appended so far issued first, so the panel's background
	 * covers the caller's glyphs and strokes as well as its rectangles. */
	overlay_flush_rects(a);
	overlay_flush_shapes(a);
	const FnlResult rc = text_end(a);

	const int line = FONT_LINE_PT[TEXT_BODY];
	const int adv = text_advance(TEXT_BODY);
	const float *edge = a->pal[COL_LINE_ON];
	overlay_rect(a, x, y, pw, ph, a->pal[COL_CTL_BOX]);
	overlay_rect(a, x, y, pw, 1, edge);
	overlay_rect(a, x, y + ph - 1, pw, 1, edge);
	overlay_rect(a, x, y, 1, ph, edge);
	overlay_rect(a, x + pw - 1, y, 1, ph, edge);

	/* The counter at the header's right edge, clamped to the width kept
	 * for it, and the title on its baseline: the two sizes share the row
	 * at different cap heights, and text_draw() takes a glyph box's top. */
	int ty = y + MET_PAD;
	int fps = ds->fps;
	if (fps < 0)
		fps = 0;
	if (fps > MET_FPS_MAX)
		fps = MET_FPS_MAX;
	char fps_text[16];
	snprintf(fps_text, sizeof fps_text, "%d FPS", fps);
	text_draw(a, x + pw - MET_PAD - text_width(fps_text, TEXT_LARGE), ty,
			  TEXT_LARGE, a->pal[COL_ICON_ON], fps_text);
	text_draw(a, x + MET_PAD,
			  ty + text_height(TEXT_LARGE) - text_height(TEXT_BODY),
			  TEXT_BODY, a->pal[COL_MET_HEAD], MET_TITLE);
	ty += 2 * line;

	/* The second column held to the right edge, so where the header is
	 * the wider the gutter takes the difference. */
	const int left = x + MET_PAD;
	const int right = x + pw - MET_PAD - MET_COLUMN * adv;
	text_draw(a, left, ty, TEXT_BODY, a->pal[COL_MET_HEAD], "Per frame");
	text_draw(a, right, ty, TEXT_BODY, a->pal[COL_MET_HEAD], "Process");
	ty += line;

	/* What a frame costs: the GPU's time on it and the render thread's,
	 * the slower red and the faster green, the rate the slower allows
	 * yellow; then how long input takes to reach the caller. */
	const double gpu = met_value(ds->cost_gpu_us, COST_UNKNOWN, 1e-3);
	const double cpu = met_value(ds->cost_cpu_us, COST_UNKNOWN, 1e-3);
	const bool both = gpu >= 0.0 && cpu >= 0.0;
	const bool gpu_slow = gpu >= cpu;
	const double slower = gpu_slow ? gpu : cpu;
	const float *plain = a->pal[COL_ICON_ON];
	const float *slow = a->pal[COL_MET_SLOW], *fast = a->pal[COL_CTL_LIVE];
	met_row(a, left, ty, "GPU", gpu, 2,
			!both ? plain : gpu_slow ? slow : fast, "ms");
	met_row(a, left, ty + line, "CPU", cpu, 2,
			!both ? plain : gpu_slow ? fast : slow, "ms");
	met_row(a, left, ty + 2 * line, "Max",
			both && slower > 0.0 ? 1000.0 / slower : -1.0, 0,
			a->pal[COL_MET_RATE], "fps");
	met_row(a, left, ty + 3 * line, "Input",
			met_value(ds->input_lat_us, COST_UNKNOWN, 1e-3), 2, plain, "ms");

	/* What this process takes of the machine. The GPU row is the share of
	 * the GPU's time this program's frames take -- the GPU's time on the
	 * frames read over the sample's window by that window's length, so 100
	 * is the GPU busy with them throughout and other programs' work is not
	 * counted; a dash on a device that times no frame. */
	met_row(a, right, ty, "CPU",
			met_value(ds->met_cpu_pm, METRICS_UNKNOWN, 0.1), 1, plain, "%");
	met_row(a, right, ty + line, "RAM",
			met_value(ds->met_ram_mib, METRICS_UNKNOWN, 1.0), 0, plain,
			"MiB");
	met_row(a, right, ty + 2 * line, "VRAM",
			met_value(ds->met_vram_mib, METRICS_UNKNOWN, 1.0), 0, plain,
			"MiB");
	met_row(a, right, ty + 3 * line, "GPU",
			gpu >= 0.0 ? met_value(ds->met_gpu_pm, METRICS_UNKNOWN, 0.1)
					   : -1.0, 1, plain, "%");
	ty += MET_ROWS * line + line / 2;

	/* The GPU every GPU figure above is on, cut to the panel's width. */
	const int room = (pw - 2 * MET_PAD) / adv;
	char name[64];
	snprintf(name, sizeof name, "%.*s",
			 room > 0 ? (room < 63 ? room : 63) : 0, fnl_gpu_short_name(a));
	text_draw(a, left, ty, TEXT_BODY, a->pal[COL_CTL_DIM], name);
	return rc;
}

/* ---- the public knobs --------------------------------------------------- */

void fnl_set_metrics_panel(FnlApp *app, bool shown)
{
	App *a = (App *)app;
	if (!a)
		return;
	fnl_atomic_store_bool(&a->metrics_shown, shown);
	/* A window at rest draws nothing until something wakes it. */
	fnl_invalidate(app);
}

bool fnl_metrics_panel(const FnlApp *app)
{
	const App *a = (const App *)app;
	return a && fnl_atomic_load_bool(&a->metrics_shown);
}

/* A value outside FnlKey -- a cast, or a key from a newer header -- is
 * ignored and not stored: no press would ever match it, which would turn
 * the key off as FNL_KEY_UNKNOWN does but say nothing of it. */
void fnl_set_metrics_key(FnlApp *app, FnlKey key)
{
	App *a = (App *)app;
	if (a && (key == FNL_KEY_UNKNOWN || echo_key_known(key)))
		fnl_atomic_store_u32(&a->metrics_key, (uint32_t)key);
}

FnlKey fnl_metrics_key(const FnlApp *app)
{
	const App *a = (const App *)app;
	return a ? (FnlKey)fnl_atomic_load_u32(&a->metrics_key)
			 : FNL_KEY_UNKNOWN;
}

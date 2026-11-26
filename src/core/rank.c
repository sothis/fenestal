/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The GPU list ranked by measured frame rate (DESIGN *Ranked by measured
 * frame rate*): every entry -- each GPU with each renderer, and on KMS
 * with FNL_GPU_DRIVER each kernel driver too -- measured on each display
 * the window has been on and kept, and the list ordered by the figures for
 * the display it is on, the fastest first and the window opened there.
 *
 * Measured, not guessed: which of a GPU's renderers is faster, and by how
 * much a driver is, is the program's scene's and the machine's -- on
 * schenker NVK at a GTX 1050 Ti's boot clocks against NVIDIA's own driver
 * -- and no property a driver reports says it; and a display's size and
 * rate are part of what a frame costs. The figure is the frame's cost,
 * 1000 / the larger of the GPU's and the CPU's milliseconds
 * (app_frame_cost()), which the library measures at every frame anyway:
 * with vsync on, so nothing on screen changes for it, and with the
 * program's own scene at the window's size, moving. A frame rate counted
 * would be the display's.
 *
 * Only what is not known is measured: once the list is whole, and again
 * whenever the window is on another display or the list has an entry it
 * had not, the entries with no figure on this display -- all of them on a
 * display new to the program, a GPU's on a GPU new to it -- and only once
 * the window has stayed on that display for RANK_QUIET (quiet()). A
 * figure for a GPU or a display not here now is kept, and so is an entry
 * that would not draw, at 0, so that neither is measured again when it
 * comes back.
 * Each entry in turn is asked for as fnl_request_gpu() asks, the entries
 * on the driver the GPU is on first and then the moves, the likeliest to
 * win first within each (order_plan()); drawn until its figure holds
 * still over two spans -- the means converge and the clocks rise -- and
 * then sampled until its mean does, each at least CAL_SETTLE_MIN and
 * CAL_SAMPLE_MIN and at most CAL_SETTLE and CAL_SAMPLE; one settled under
 * half of the best measured here is kept at that and not sampled.
 * Meanwhile the window says so over all it draws, with the
 * time left (app_rank_draw()), and takes no input (app_rank_holds_input()):
 * a click would move it off the entry measured.
 *
 * FNL_GPU_RANK: unset, the above; =calibrate, every entry on this display
 * measured again; =keep, a ranking used and never measured; =order, the
 * list ordered by it and nothing else -- the window opens where the pick
 * puts it, and an index is the starting renderer's own; =off, none read.
 * The harness runs with it off, but for a round on a kernel driver, which
 * keeps or orders.
 */
#include "core/app.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#else
#include <unistd.h>
#endif
#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#define CAL_SETTLE 1.5     /* seconds drawn at most before sampling */
#define CAL_SAMPLE 2.0     /* seconds sampled at most */
#define CAL_SETTLE_MIN 0.5 /* drawn at least, before a figure is believed */
#define CAL_SAMPLE_MIN 0.5 /* and sampled at least */
#define CAL_BIN 0.25       /* the span the figure is watched over */
#define CAL_STEADY 0.05    /* two spans this close: the clocks have risen */
#define CAL_CONVERGE 0.03  /* the running mean moving less: measured */
#define CAL_OUTCLASS 0.5   /* settled under this of the best: not sampled */
#define CAL_SWITCH 0.2     /* what a switch is guessed at, before one ran */
#define CAL_MOVE 1.8       /* and a move to another kernel driver, more */
#define CAL_LIMIT 30.0     /* an entry not drawn on in this is left out */
#define RANK_QUIET 1.0     /* seconds on one output, unmoved, to act */
#define CAL_PANEL_W 640    /* the panel's least width, points, padding out */
/* The panel's own padding and the gap inside its figures, which plan() and
 * the draw must agree on: they are what turns a measured line into a width. */
#define CAL_PAD 20
#define CAL_GAP 12
/* Its three fixed lines. Here rather than in the draw because plan() measures
 * them once for the whole calibration. */
static const char CAL_HEAD[] = "Calibrating the GPUs";
static const char CAL_UNIT[] = "s left";
static const char CAL_NOTE[] = "Input is off until it ends; the fastest is "
							   "chosen then.";

enum { CAL_OFF, CAL_NEXT, CAL_LANDING, CAL_SETTLING, CAL_SAMPLING };
enum { RANK_AUTO, RANK_CALIBRATE, RANK_KEEP, RANK_ORDER, RANK_OFF };

/* ---- where the ranking is kept ------------------------------------------ */

static void program_name(char *out, size_t size)
{
	char path[512] = "";
#if defined(_WIN32)
	if (_pgmptr)
		snprintf(path, sizeof path, "%s", _pgmptr);
#elif defined(__APPLE__)
	snprintf(path, sizeof path, "%s", getprogname());
#elif defined(__FreeBSD__)
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
	size_t len = sizeof path;
	if (sysctl(mib, 4, path, &len, NULL, 0) != 0)
		path[0] = '\0';
#else
	const ssize_t k     = readlink("/proc/self/exe", path, sizeof path - 1);
	path[k > 0 ? k : 0] = '\0';
#endif
	const char *base = path;
	for (const char *p = path; *p; p++)
		if (*p == '/' || *p == '\\')
			base = p + 1;
	snprintf(out, size, "%s", base[0] ? base : "program");
	const size_t n = strlen(out);
	if (n > 4 && strcmp(out + n - 4, ".exe") == 0)
		out[n - 4] = '\0';
	/* A file name: anything but a word character is a dash. */
	for (char *p = out; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			  (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
			*p = '-';
}

static int make_dir(const char *path)
{
#if defined(_WIN32)
	return _mkdir(path);
#else
	return mkdir(path, 0755);
#endif
}

/* The user's cache directory and fenestal's in it, made where it is not;
 * false where there is none to be had. */
static bool cache_dir(char *out, size_t size)
{
	const char *base;
	char made[512];
#if defined(_WIN32)
	base = getenv("LOCALAPPDATA");
	if (!base || !base[0])
		return false;
	snprintf(made, sizeof made, "%s", base);
#elif defined(__APPLE__)
	base = getenv("HOME");
	if (!base || !base[0])
		return false;
	snprintf(made, sizeof made, "%s/Library/Caches", base);
#else
	base = getenv("XDG_CACHE_HOME");
	if (base && base[0] == '/') {
		snprintf(made, sizeof made, "%s", base);
	} else {
		base = getenv("HOME");
		if (!base || !base[0])
			return false;
		snprintf(made, sizeof made, "%s/.cache", base);
	}
	/* Either may be missing yet, $XDG_CACHE_HOME as well: the XDG spec has
	 * a writer make it, as the pipeline cache's does (pipeline.c). */
	if (make_dir(made) != 0 && errno != EEXIST)
		return false;
#endif
	const int n = snprintf(out, size, "%s/fenestal", made);
	if (n < 0 || (size_t)n >= size)
		return false;
	return make_dir(out) == 0 || errno == EEXIST;
}

/* What a figure in the file means, raised whenever that changes: a file
 * of another version is read as nothing measured, and every entry is
 * measured again, since its figures would stand against new ones that are
 * not alike. 2: the GPU half with the copy path's readback on every
 * renderer and no snapshot's copy, the CPU half the render thread's CPU
 * time to the present's return (DESIGN *What a frame costs, compared*). */
#define RANK_VERSION "v2"
#define RANK_HEAD "# fenestal GPU ranking " RANK_VERSION ":"

/* The ranking file: one line a figure, "<fps>\t<display>\t<key>", 0 for an
 * entry that would not draw there, after a line saying what it is and its
 * version. A line of another shape is skipped, which is how a file from
 * before the displays is read: as nothing measured. */
static void rank_load(App *a)
{
	FILE *f = fopen(a->rank_path, "r");
	if (!f)
		return;
	char line[RANK_DISPLAY_MAX + RANK_ID_MAX + 64];
	if (!fgets(line, sizeof line, f) ||
		strncmp(line, RANK_HEAD, strlen(RANK_HEAD)) != 0) {
		fclose(f);
		fnl_log(FNL_LOG_INFO,
				"GPU rank: %s is from before the figures "
				"meant what they mean now (" RANK_VERSION "); every entry "
				"is measured again",
				a->rank_path);
		return;
	}
	int n = 0;
	while (n < RANK_MAX && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '#')
			continue;
		char *t1 = strchr(line, '\t');
		char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
		if (!t2)
			continue;
		*t1 = *t2 = '\0';
		char *end;
		const double fps = strtod(line, &end);
		/* A figure, and a finite one: strtod() takes "inf", which would
		 * rank first for good. */
		if (end == line || *end || !(fps >= 0.0 && fps < 1e9) || !t1[1] ||
			!t2[1])
			continue;
		/* A way ranked apart, from before a GPU was one key: never asked
		 * for again, and not to be opened on. Files of that time hold it
		 * in either case ("-D3D11|" as well), so both are dropped. */
		if (strstr(t2 + 1, "-d3d11|") || strstr(t2 + 1, "-D3D11|"))
			continue;
		/* A renderer no build has any more: its entries would otherwise
		 * stay in the file for good, and rank first on a figure nothing
		 * can match again. */
		if (strncmp(t2 + 1, "gl|", 3) == 0)
			continue;
		RankRow *r = &a->rank[n++];
		snprintf(r->display, sizeof r->display, "%s", t1 + 1);
		snprintf(r->id, sizeof r->id, "%s", t2 + 1);
		r->fps = fps;
	}
	fclose(f);
	a->rank_n = n;
}

/* Written beside the file and renamed over it, under a name of this
 * process's own: two instances of a program share the file, and with one
 * name between them one's rename could move the other's half-written
 * file into place, or find its own gone. */
static bool rank_save(const App *a)
{
	char tmp[sizeof a->rank_path + 32];
#if defined(_WIN32)
	const long pid = (long)_getpid();
#else
	const long pid = (long)getpid();
#endif
	snprintf(tmp, sizeof tmp, "%s.%ld.new", a->rank_path, pid);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return false;
	const time_t now   = time(NULL);
	char when[32]      = "";
	const struct tm *t = gmtime(&now);
	if (t)
		strftime(when, sizeof when, "%Y-%m-%d %H:%M UTC", t);
	fprintf(f,
			RANK_HEAD " %s, last measured %s; <fps> "
					  "<display> <entry>, 0 where it would not draw\n",
			BACKEND(a)->name, when);
	for (int i = 0; i < a->rank_n; i++)
		fprintf(f, "%.1f\t%s\t%s\n", a->rank[i].fps, a->rank[i].display,
				a->rank[i].id);
	/* A write that failed midway -- a full disk -- sets the stream's error
	 * and may leave fclose() nothing to fail on: renamed, the short file
	 * would stand for the whole ranking. */
	const bool wrote = !ferror(f);
	/* The stream error's own errno, read before fclose() runs: a buffered
	 * write that set the stream error need not have set errno, and a
	 * subsequent successful fclose() leaves whatever errno last held --
	 * the callers log strerror(errno), which would then read "Success" or
	 * an unrelated call's error. */
	const int e    = wrote ? 0 : errno;
	const bool ok  = fclose(f) == 0 && wrote;
	if (!ok) {
		(void)remove(tmp);
		/* A stream error that left no errno is still an error: say so
		 * rather than let the caller print "Success" over the empty zero. */
		errno = wrote ? errno : (e ? e : EIO);
		return false;
	}
#if defined(_WIN32)
	/* Windows' rename will not replace a file; POSIX's replaces it in one
	 * step, which a remove first would give up. */
	(void)remove(a->rank_path);
#endif
	if (rename(tmp, a->rank_path) == 0)
		return true;
	/* A rename that failed leaves the whole temporary behind, once per run:
	 * the ranking's own name is what the next start would read. */
	(void)remove(tmp);
	return false;
}

/* ---- the display, and the figures for it -------------------------------- */

/* How much of an output's name names the display (display_now()): a name
 * published for a status line adds the model in brackets, and on Wayland
 * the other outputs a window straddles after " + ". */
static size_t output_name_len(const char *name)
{
	size_t len      = strlen(name);
	const char *cut = strstr(name, " + ");
	if (cut && (size_t)(cut - name) < len)
		len = (size_t)(cut - name);
	/* Not a count, though: " (2)" is how Cocoa tells two monitors of one
	 * model apart (screens_named()), and is the display's own name. */
	for (cut = strstr(name, " ("); cut && (size_t)(cut - name) < len;
		 cut = strstr(cut + 2, " (")) {
		const char *q = cut + 2;
		while (*q >= '0' && *q <= '9')
			q++;
		if (q > cut + 2 && *q == ')')
			continue;
		len = (size_t)(cut - name);
		break;
	}
	return len;
}

/* The display the window is on, as figures are kept for it: the output as
 * the backend names it and the mode it shows -- "HDMI-A-1 3840x2160@30" --
 * since a frame's cost is the size's, and a monitor swapped on a cable is
 * the same connector at another mode. The backend's mode where it says one
 * (KMS), and otherwise the output's size and rate as published, the size
 * in points made pixels. "-" for an output it names none. */
static void display_now(App *a, char *out, size_t size)
{
	char name[OUTPUT_NAME_MAX];
	fnl_mutex_lock(&a->lock);
	snprintf(name, sizeof name, "%s", a->want_output[0] ? a->want_output : "-");
	const int w_pt = a->want_out_w_pt, h_pt = a->want_out_h_pt;
	const int mode_w = a->want_out_mode_w, mode_h = a->want_out_mode_h;
	/* The pump's scale, which the points were published at; the drawn one
	 * (fnl_scale120()) is a frame behind, and on a scale change it would key
	 * the display at the old density's pixels -- a display nobody has, and
	 * one the calibration reads as the window having moved. */
	const int s120_pt = want_s120(a);
	fnl_mutex_unlock(&a->lock);
	FnlMode modes[OUTPUT_MODES_MAX];
	int cur     = -1;
	const int n = BACKEND(a)->modes(a, modes, OUTPUT_MODES_MAX, &cur);
	/* Where the backend says the mode, the output's name from the same
	 * selection: the name published lags a switch between outputs, and
	 * the old output's name with the new one's mode is a display nobody
	 * has (KMS's sel_want, which both of these read). */
	char outs[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	int out_cur = -1;
	const int k =
		n > 0 ? BACKEND(a)->outputs(a, outs, OUTPUTS_MAX, &out_cur) : 0;
	if (k > 0 && out_cur >= 0 && out_cur < k)
		snprintf(name, sizeof name, "%s", outs[out_cur]);
	/* The output followed and nothing else. A straddle is no display of
	 * its own, or every crossing would be a calibration, and a monitor
	 * swapped at the same mode keeps the figures, as on KMS. */
	name[output_name_len(name)] = '\0';
	int w, h;
	unsigned int mhz;
	if (n > 0 && cur >= 0 && cur < n) {
		w   = modes[cur].w_px;
		h   = modes[cur].h_px;
		mhz = modes[cur].refresh_mhz;
	} else if (mode_w > 0 && mode_h > 0) {
		/* The output's own mode, where the backend publishes one apart from
		 * the room: Wayland's room is the compositor's bounds for the
		 * window, another output's while it straddles two, and the mode
		 * itself after a move until bounds come -- three names for two
		 * monitors, each measured. */
		w   = mode_w;
		h   = mode_h;
		mhz = fnl_atomic_load_u32(&a->out_refresh_mhz);
	} else {
		w   = w_pt * s120_pt / 120;
		h   = h_pt * s120_pt / 120;
		mhz = fnl_atomic_load_u32(&a->out_refresh_mhz);
	}
	snprintf(out, size, "%s %dx%d@%u", name, w, h, (mhz + 500) / 1000);
	/* A rate not said yet -- X11 reads it a pump turn after the window
	 * opens, after the ranking has picked the GPU, and so does Win32 under
	 * wine -- is the one kept for this output at this size. Keyed @0, the
	 * start would be ordered by figures that are not this display's, and
	 * the rate's arrival would read as a move to another display. */
	char pre[RANK_DISPLAY_MAX];
	snprintf(pre, sizeof pre, "%s %dx%d@", name, w, h);
	const size_t len = strlen(pre);
	if (mhz == 0) {
		/* Under the lock, as the rated lookup below: the lister thread
		 * reads these names too (gpu_merge()), and one reader outside the
		 * lock is the one a later cross-thread writer would tear. */
		char kept[RANK_DISPLAY_MAX] = "";
		fnl_mutex_lock(&a->lock);
		for (int i = 0; i < a->rank_n; i++)
			if (strncmp(a->rank[i].display, pre, len) == 0) {
				snprintf(kept, sizeof kept, "%s", a->rank[i].display);
				break;
			}
		fnl_mutex_unlock(&a->lock);
		if (kept[0])
			snprintf(out, size, "%s", kept);
		return;
	}
	/* And the other way round: figures kept @0, where the first start on
	 * this output knew no rate, are this rate's once it is known and none
	 * are kept for it -- or the output is measured again under a second
	 * name (wine's DISPLAY1, @0 and @60, 2026-10-31). Kept so in the file
	 * where the ranking writes one. */
	/* A display name with no room for its "0" was never kept @0 either:
	 * the kept names are cut at the same size. */
	if (len + 2 > RANK_DISPLAY_MAX)
		return;
	char unrated[RANK_DISPLAY_MAX];
	memcpy(unrated, pre, len);
	unrated[len]     = '0';
	unrated[len + 1] = '\0';
	/* Under the lock: the lister thread orders the list by these names
	 * (gpu_merge()). */
	bool rated = false, renamed = false;
	fnl_mutex_lock(&a->lock);
	for (int i = 0; i < a->rank_n && !rated; i++)
		rated = strcmp(a->rank[i].display, out) == 0;
	for (int i = 0; i < a->rank_n && !rated; i++)
		if (strcmp(a->rank[i].display, unrated) == 0) {
			snprintf(a->rank[i].display, sizeof a->rank[i].display, "%s", out);
			renamed = true;
		}
	fnl_mutex_unlock(&a->lock);
	if (renamed) {
		fnl_log(FNL_LOG_INFO,
				"GPU rank: the figures for %s are %s's, its "
				"rate now known",
				unrated, out);
		if ((a->rank_mode == RANK_AUTO || a->rank_mode == RANK_CALIBRATE) &&
			!rank_save(a))
			fnl_log(FNL_LOG_WARN, "GPU rank: %s could not be written (%s)",
					a->rank_path, strerror(errno));
	}
}

/* The kept figure for entry `id` on `display`, -1 for none. */
static double figure(const App *a, const char *display, const char *id)
{
	for (int i = 0; i < a->rank_n; i++)
		if (strcmp(a->rank[i].id, id) == 0 &&
			strcmp(a->rank[i].display, display) == 0)
			return a->rank[i].fps;
	return -1.0;
}

/* What orders entry `id` on `display`: its figure there, or where none is
 * kept yet -- a display the window has not been measured on -- its best
 * on any other, so a new display starts from what is known. */
static double order_figure(const App *a, const char *display, const char *id)
{
	const double here = figure(a, display, id);
	if (here >= 0.0)
		return here;
	double best = 0.0;
	for (int i = 0; i < a->rank_n; i++)
		if (strcmp(a->rank[i].id, id) == 0 && a->rank[i].fps > best)
			best = a->rank[i].fps;
	return best;
}

/* ---- opening on the ranked first ---------------------------------------- */

static FnlRenderer renderer_of_key(const char *id, size_t len)
{
	for (int r = 1; r < 16; r++) {
		const char *k = app_renderer_key((FnlRenderer)r);
		if (strncmp(k, id, len) == 0 && k[len] == '\0')
			return (FnlRenderer)r;
	}
	return FNL_RENDERER_DEFAULT;
}

/* Whether this run can make key `id` (app_gpu_entry_id()): one naming a
 * kernel driver only with a move offered, and onto one of the GPU's two;
 * and then not the GPU's key with none, which a run without the moves
 * kept -- known by the same GPU kept with a driver as well. A ranking
 * measured with the moves and read without them otherwise opens on a
 * name no renderer lists. */
static bool makeable(const App *a, const char *id, bool offered,
					 const GpuDriver *g)
{
	const char *bar = strchr(id, '|');
	if (!bar)
		return false;
	const char *name = bar + 1;
	const char *d    = app_gpu_driver_of(name);
	if (d)
		return offered &&
			   (strcmp(d, g->bound) == 0 || strcmp(d, g->other) == 0);
	if (!offered)
		return true;
	const size_t len = strlen(name);
	for (int i = 0; i < a->rank_n; i++) {
		const char *b  = strchr(a->rank[i].id, '|');
		const char *kd = b ? app_gpu_driver_of(b + 1) : NULL;
		if (kd && (size_t)(kd - 4 - (b + 1)) == len &&
			strncmp(b + 1, name, len) == 0)
			return false;
	}
	return true;
}

/* The k-th best entry known that this run can make, from 1, on the
 * display the window opened on as order_figure() orders them, into `out`;
 * false where there are fewer with a figure. */
static bool ranked_id(App *a, const char *display, int k, char *out,
					  size_t size)
{
	GpuDriver g;
	const bool offered = app_gpu_driver_offered(a, &g);
	char seen[RANK_MAX][RANK_ID_MAX];
	double fps[RANK_MAX];
	int n = 0;
	for (int i = 0; i < a->rank_n; i++) {
		bool dup = false;
		for (int j = 0; j < n && !dup; j++)
			dup = strcmp(seen[j], a->rank[i].id) == 0;
		if (dup || !makeable(a, a->rank[i].id, offered, &g))
			continue;
		const double f = order_figure(a, display, a->rank[i].id);
		if (!(f > 0.0))
			continue;
		int j = n++;
		while (j > 0 && fps[j - 1] < f) {
			memcpy(seen[j], seen[j - 1], sizeof seen[j]);
			fps[j] = fps[j - 1];
			j--;
		}
		snprintf(seen[j], sizeof seen[j], "%s", a->rank[i].id);
		fps[j] = f;
	}
	if (k > n)
		return false;
	snprintf(out, size, "%s", seen[k - 1]);
	return true;
}

/* Ranked entry `id` made what fnl_init() opens on: its renderer tried
 * first, its GPU asked for by the name its key holds, and its kernel
 * driver bound before any renderer where it names one the GPU is not on.
 * A software rasteriser's key names no GPU, and is left to the move once
 * the list is whole. */
static void open_on(App *a, int k, const char *id)
{
	const char *bar = strchr(id, '|');
	if (!bar)
		return;
	a->renderer_first = renderer_of_key(id, (size_t)(bar - id));
	char name[RANK_ID_MAX];
	snprintf(name, sizeof name, "%s", bar + 1);
	/* "<GPU> on <driver>", for the GPU that can move: ranked_id() gives
	 * one only with the move offered, onto one of its drivers. */
	const char *d = app_gpu_driver_of(name);
	if (d) {
		char driver[GPU_DRIVER_NAME_MAX];
		snprintf(driver, sizeof driver, "%s", d);
		name[d - 4 - name] = '\0';
		app_gpu_driver_prefer(a, driver);
	}
	if (strcmp(name, "software") != 0)
		snprintf(a->gpu_want, sizeof a->gpu_want, "%.*s",
				 (int)sizeof a->gpu_want - 1, name);
	fnl_log(FNL_LOG_INFO, "GPU rank: opening on rank %d, %s", k, id);
}

void app_rank_start(App *a)
{
	a->cal.state    = CAL_OFF;
	a->cal.at       = 0.0;
	a->rank_target  = 0;
	a->rank_land[0] = '\0';
	/* What the caller asked for, before the ranking or a move spends it:
	 * a name stands for the window's life, and an index counts the ranked
	 * list whenever the window is aimed again (aim()). */
	a->rank_asked_name  = a->gpu_want[0] != '\0';
	a->rank_asked_index = a->gpu_index;
	a->rank_dirty       = true;
	a->rank_display[0]  = '\0';
	fnl_atomic_store_bool(&a->calibrating, false);
	const char *v = getenv("FNL_GPU_RANK");
	a->rank_mode  = !v || !v[0]                   ? RANK_AUTO
					: strcmp(v, "calibrate") == 0 ? RANK_CALIBRATE
					: strcmp(v, "keep") == 0      ? RANK_KEEP
					: strcmp(v, "order") == 0     ? RANK_ORDER
					: strcmp(v, "off") == 0       ? RANK_OFF
												  : RANK_AUTO;
	/* FnlInitDesc::no_gpu_ranking means what unset now does not: nothing
	 * measured with the window up, for a program a person starts to use. The
	 * environment still decides where it speaks. */
	if (a->rank_no_measure && (!v || !v[0]))
		a->rank_mode = RANK_OFF;
	if (v && v[0] && a->rank_mode == RANK_AUTO)
		fnl_log(FNL_LOG_WARN,
				"FNL_GPU_RANK is '%s': calibrate, keep, "
				"order, off, or unset",
				v);
	/* A second window reads the ranking the first kept -- the figures are
	 * the program's -- and opens on its first entry, but measures and writes
	 * nothing: a calibration moves the window between GPUs, which a host of
	 * several cannot do, and the file is the calibration's to write.
	 * RANK_KEEP is that read: it aims once (open_on()), neither calibrates
	 * nor saves, and names no later aim -- a move would be refused while two
	 * are open. RANK_ORDER's explicit "order alone" is taken as given, and a
	 * host whose first window asked for no ranking (RANK_OFF) has none to
	 * read. */
	if (a->rank_read_only) {
		if (a->host->rank_off || a->rank_mode == RANK_OFF)
			a->rank_mode = RANK_OFF;
		else if (a->rank_mode != RANK_ORDER)
			a->rank_mode = RANK_KEEP;
	} else {
		a->host->rank_off = a->rank_mode == RANK_OFF;
	}
	if (a->rank_mode == RANK_OFF)
		return;
	char dir[512], prog[96];
	if (!cache_dir(dir, sizeof dir)) {
		fnl_log(FNL_LOG_INFO, "GPU rank: no cache directory, so no ranking "
							  "is kept");
		a->rank_mode = RANK_OFF;
		return;
	}
	program_name(prog, sizeof prog);
	snprintf(a->rank_path, sizeof a->rank_path, "%s/gpu-rank-%s-%s.txt", dir,
			 prog, BACKEND(a)->key);
	rank_load(a);
	if (a->rank_n == 0)
		return;
	char display[RANK_DISPLAY_MAX];
	display_now(a, display, sizeof display);
	fnl_log(FNL_LOG_INFO,
			"GPU rank: %d figures kept, from %s; the window "
			"on %s%s",
			a->rank_n, a->rank_path, display,
			a->rank_mode == RANK_ORDER ? ", the list ordered alone" : "");
	if (a->rank_mode == RANK_ORDER)
		return;
	/* A name asked for stands; an index counts the ranked list; nothing
	 * asked for is the first of it. Opened on there where the key says
	 * enough (open_on()); and moved to once every renderer has listed its
	 * GPUs where the pick went elsewhere, which a click would do. */
	if (a->gpu_want[0])
		return;
	a->rank_target = a->gpu_index > 0 ? a->gpu_index : 1;
	a->gpu_index   = 0;
	char id[RANK_ID_MAX];
	if (ranked_id(a, display, a->rank_target, id, sizeof id))
		open_on(a, a->rank_target, id);
	/* A second window's open is its one aim: opening on the ranked entry is
	 * the read, and a move to it afterwards would be refused while two
	 * windows are open (and is not wanted once the first has gone). */
	if (a->rank_read_only)
		a->rank_target = 0;
}

/* ---- the order ---------------------------------------------------------- */

/* A GPU listed both ways (GpuEntry::via) is one key: the ranking orders
 * GPUs and renderers, and leaves the way to the renderer. Ranked apart,
 * the way through DXGI lost every time to the copy it costs, and a program
 * that ranked was moved onto the GPU's own swapchain, which a frame's cost
 * does not show is a refresh or three later on screen and, on Intel, 33 to
 * 44 fps through a drag (DESIGN *Vulkan through DXGI*). */
void app_gpu_entry_id(const App *a, const GpuEntry *e, char *out, size_t size)
{
	char cut[GPU_NAME_MAX];
	/* A renderer's software rasteriser by its kind: a renderer may list Mesa's
	 * as "Mesa software rasteriser" and name it "llvmpipe (...)" once it
	 * draws, and there is one a renderer. */
	if (e->kind == FNL_GPU_KIND_SOFTWARE)
		snprintf(out, size, "%s|software", app_renderer_key(e->renderer));
	else if (e->driver[0] && a->gpu_driver_gpu[0])
		snprintf(out, size, "%s|%s on %s", app_renderer_key(e->renderer),
				 app_gpu_short(cut, sizeof cut, a->gpu_driver_gpu), e->driver);
	else
		snprintf(out, size, "%s|%s", app_renderer_key(e->renderer),
				 app_gpu_short(cut, sizeof cut, e->name));
}

void app_rank_sort(const App *a, GpuEntry *list, int n)
{
	/* The render thread's last word on the display, under the lock this
	 * is called with (gpu_merge()); "" before it has one, which orders by
	 * each entry's best anywhere. */
	char display[RANK_DISPLAY_MAX];
	snprintf(display, sizeof display, "%s", a->rank_display);
	for (int i = 0; i < n; i++) {
		char id[RANK_ID_MAX];
		app_gpu_entry_id(a, &list[i], id, sizeof id);
		list[i].fps = (float)order_figure(a, display, id);
	}
	if (a->rank_n == 0)
		return;
	/* Insertion, stable: the unmeasured keep the list's own order after
	 * the measured, and so does a tie -- but for a GPU listed both ways,
	 * one figure, whose renderer's own way goes first: where the ranking
	 * moves the window (app_rank_tick()). */
	for (int i = 1; i < n; i++) {
		const GpuEntry e = list[i];
		int j            = i;
		while (j > 0 && (list[j - 1].fps < e.fps ||
						 (list[j - 1].fps == e.fps && e.way_default &&
						  list[j - 1].renderer == e.renderer &&
						  strcmp(list[j - 1].name, e.name) == 0))) {
			list[j] = list[j - 1];
			j--;
		}
		list[j] = e;
	}
}

/* ---- the calibration, and the move to the ranked entry ------------------ */

bool app_rank_holds_input(const App *a)
{
	return fnl_atomic_load_bool(&a->calibrating);
}

/* Whether the list is whole and nothing is moving: every renderer has
 * listed its GPUs since the last move to another kernel driver, and no
 * request, swap or move is under way. */
static bool settled(App *a)
{
	if (!a->renderer || !a->renderer_ready || a->mover_started ||
		a->gpu_cross_landing || a->gpu_switching || a->renderer_next ||
		!a->gpu_others_listed ||
		(a->lister_started && !fnl_atomic_load_bool(&a->lister_done)))
		return false;
	fnl_mutex_lock(&a->lock);
	const bool idle = a->gpu_req == 0;
	fnl_mutex_unlock(&a->lock);
	return idle;
}

/* The list's entry keyed `id` -- of a GPU listed both ways, the way its
 * renderer takes by itself (GpuEntry::way_default) -- and the one in use;
 * -1 for none. */
static int entry_of(App *a, const char *id, int *cur)
{
	int found = -1;
	fnl_mutex_lock(&a->lock);
	for (int i = 0; i < a->gpu_list_n; i++) {
		char k[RANK_ID_MAX];
		app_gpu_entry_id(a, &a->gpu_list[i], k, sizeof k);
		if (strcmp(k, id) == 0 && (found < 0 || a->gpu_list[i].way_default))
			found = i;
	}
	*cur = a->gpu_list_cur;
	fnl_mutex_unlock(&a->lock);
	return found;
}

/* Entry `i` asked for, as fnl_request_gpu() asks. */
static void request(App *a, int i)
{
	fnl_mutex_lock(&a->lock);
	if (i >= 0 && i < a->gpu_list_n && i != a->gpu_list_cur) {
		a->gpu_req       = i + 1;
		a->gpu_req_entry = a->gpu_list[i];
	}
	fnl_mutex_unlock(&a->lock);
}

/* Whether key `id` is a software rasteriser's, which keys by its kind
 * (app_gpu_entry_id()): "vulkan|software". */
static bool software_id(const char *id)
{
	const char *bar = strchr(id, '|');
	return bar && strcmp(bar + 1, "software") == 0;
}

/* The plan reordered within each group, the likely winner first: hardware
 * before software, and by what each is known to do here or elsewhere
 * (order_figure()). Measured first, it is the best known by the time the
 * others are drawn, and one far behind it is not sampled (CAL_OUTCLASS).
 * The groups keep their order -- the moves to another kernel driver last
 * -- so the GPU still moves once and back once. */
static void order_plan(App *a, const char *display)
{
	double key[GPU_LIST_MAX * 2];
	for (int j = 0; j < a->cal.n; j++)
		key[j] = software_id(a->cal.ids[j])
					 ? -1.0
					 : order_figure(a, display, a->cal.ids[j]) + 1.0;
	/* Insertion sort, stable, within each run of one group. */
	for (int j = 1; j < a->cal.n; j++) {
		for (int k = j; k > 0 && a->cal.group[k - 1] == a->cal.group[k] &&
						key[k - 1] < key[k];
			 k--) {
			char id[RANK_ID_MAX];
			memcpy(id, a->cal.ids[k], sizeof id);
			memcpy(a->cal.ids[k], a->cal.ids[k - 1], sizeof id);
			memcpy(a->cal.ids[k - 1], id, sizeof id);
			const double t = key[k];
			key[k]         = key[k - 1];
			key[k - 1]     = t;
		}
	}
}

/* What to visit on `display`: every entry with no figure kept there, or
 * every entry where `all` -- those on the kernel driver the GPU is on
 * first and the moves to another after them, so that the GPU moves once
 * and the entries there are drawn on before it moves back. A figure kept
 * for an entry or a display not here now is left as it is. */
/* rank.c's own: how an entry's key reads as words, defined far below. */
static void entry_words(const char *id, char *out, size_t size);

/* The panel's width for the plan just made: the widest line any of its
 * entries will show, the seconds at their widest, and never less than
 * CAL_PANEL_W -- a panel sized to the line on screen would change width with
 * every entry's name. Asked once, here, where the ids are set; the draw used
 * to walk every entry again on every frame of the calibration. */
static int panel_width(const App *a)
{
	int pw             = CAL_PANEL_W;
	const int fixed[3] = { text_width(CAL_HEAD, TEXT_UI),
						   text_width("88888", TEXT_LARGE) + CAL_GAP +
							   text_width(CAL_UNIT, TEXT_UI),
						   text_width(CAL_NOTE, TEXT_UI) };
	for (int k = 0; k < 3; k++)
		if (fixed[k] > pw)
			pw = fixed[k];
	for (int j = 0; j < a->cal.n; j++) {
		char one[RANK_ID_MAX + 64], words[RANK_ID_MAX + 16];
		entry_words(a->cal.ids[j], words, sizeof words);
		snprintf(one, sizeof one, "%d of %d: %s", a->cal.n, a->cal.n, words);
		const int ew = text_width(one, TEXT_UI);
		if (ew > pw)
			pw = ew;
	}
	return pw + 2 * CAL_PAD;
}

static void plan(App *a, const char *display, bool all)
{
	int n = 0;
	fnl_mutex_lock(&a->lock);
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < a->gpu_list_n; i++) {
			const GpuEntry *e = &a->gpu_list[i];
			if ((e->driver_move != 0) != (pass == 1))
				continue;
			char id[RANK_ID_MAX];
			app_gpu_entry_id(a, e, id, sizeof id);
			if (!all && figure(a, display, id) >= 0.0)
				continue;
			/* A GPU listed both ways is one key, measured once. */
			bool planned = false;
			for (int j = 0; j < n && !planned; j++)
				planned = strcmp(a->cal.ids[j], id) == 0;
			if (planned)
				continue;
			snprintf(a->cal.ids[n], sizeof a->cal.ids[n], "%s", id);
			a->cal.group[n] = (unsigned char)pass;
			a->cal.fps[n++] = 0.0;
		}
	fnl_mutex_unlock(&a->lock);
	a->cal.n       = n;
	a->cal.i       = 0;
	a->cal.panel_w = panel_width(a);
	snprintf(a->cal.display, sizeof a->cal.display, "%s", display);
	order_plan(a, display);
}

/* The calibration under way: the window takes no input until it ends,
 * and its scene moves, as it is used -- what the switch costs a frame in
 * flight. The scene's clock runs off App::calibrating itself
 * (draw_state_update()), so the caller's own setting is never touched.
 * The entry in use is noted, for a GPU the caller named to come back to
 * (aim()). */
static void begin(App *a, const char *why)
{
	a->cal.started = a->cal.entry_at = now_seconds();
	a->cal.drew_at                  = a->cal.started;
	a->cal.seen_presents            = a->presents;
	a->cal.dur_sum[0] = a->cal.dur_sum[1] = 0.0;
	a->cal.dur_k[0] = a->cal.dur_k[1] = 0;
	a->cal.back[0]                    = '\0';
	fnl_mutex_lock(&a->lock);
	if (a->gpu_list_cur >= 0 && a->gpu_list_cur < a->gpu_list_n)
		app_gpu_entry_id(a, &a->gpu_list[a->gpu_list_cur], a->cal.back,
						 sizeof a->cal.back);
	fnl_mutex_unlock(&a->lock);
	fnl_atomic_store_bool(&a->calibrating, true);
	fnl_log(FNL_LOG_INFO,
			"GPU rank: calibrating %d entries on %s, %.1f to "
			"%.1f s each, the window taking no input: %s",
			a->cal.n, a->cal.display, CAL_SETTLE_MIN + CAL_SAMPLE_MIN,
			CAL_SETTLE + CAL_SAMPLE, why);
}

/* How many kept figures a display has. A row is evicted with the fewest,
 * so the last figure of a display taken away goes before any part of one
 * still known by several. */
static int display_figures(const App *a, const char *display)
	FNL_REQUIRES(a->lock)
{
	int n = 0;
	for (int i = 0; i < a->rank_n; i++)
		if (strcmp(a->rank[i].display, display) == 0)
			n++;
	return n;
}

/* A figure for an entry on a display into the kept ones, in its place
 * where one was. A new one when the table is full evicts rather than
 * discarding: the row whose display has the fewest kept figures, so a
 * display taken away keeps its figures (DESIGN *Ranked by measured frame
 * rate*) until the room is wanted, and then the smallest set goes first --
 * which is why a lone stale key never pushes out a live display's. */
static void keep(App *a, const char *display, const char *id, double fps)
{
	fnl_mutex_lock(&a->lock);
	int at = -1;
	for (int i = 0; i < a->rank_n && at < 0; i++)
		if (strcmp(a->rank[i].id, id) == 0 &&
			strcmp(a->rank[i].display, display) == 0)
			at = i;
	if (at < 0 && a->rank_n < RANK_MAX)
		at = a->rank_n++;
	if (at < 0 && a->rank_n == RANK_MAX) {
		/* Full: the row of a display other than this one with the fewest
		 * figures, the oldest of equals taking it. */
		int fewest = -1;
		for (int i = 0; i < a->rank_n; i++) {
			if (strcmp(a->rank[i].display, display) == 0)
				continue;
			const int n = display_figures(a, a->rank[i].display);
			if (fewest < 0 || n < fewest) {
				fewest = n;
				at     = i;
			}
		}
	}
	if (at >= 0) {
		RankRow *r = &a->rank[at];
		snprintf(r->display, sizeof r->display, "%s", display);
		snprintf(r->id, sizeof r->id, "%s", id);
		r->fps = fps;
	}
	fnl_mutex_unlock(&a->lock);
	if (at < 0)
		fnl_log(FNL_LOG_WARN,
				"GPU rank: %d figures kept already, every one on %s; "
				"%s on %s is not",
				RANK_MAX, display, id, display);
}

/* Where the window goes once the ranking has said something new -- a
 * calibration ended, or the window on another display: back to the entry
 * it was on where the caller named a GPU, whose name stands
 * (app_rank_start()) though the calibration's moves spent it; the
 * caller's index into the ranked list where it gave one; and its first
 * entry where it asked for neither. `back` is the entry to come back to,
 * "" where there is none to (a display moved to with no calibration). */
static void aim(App *a, const char *back)
{
	if (a->rank_asked_name) {
		snprintf(a->rank_land, sizeof a->rank_land, "%s", back);
		return;
	}
	a->rank_target = a->rank_asked_index > 0 ? a->rank_asked_index : 1;
}

static void finish(App *a)
{
	a->cal.state = CAL_OFF;
	fnl_atomic_store_bool(&a->calibrating, false);
	/* Ended before an entry was measured -- the window gone to another
	 * display at once, a second window opened, nothing drawn: nothing to
	 * keep, say or rank. But the first entry may already have been asked
	 * for, so the GPU it left is still gone back to. */
	if (a->cal.n == 0) {
		aim(a, a->cal.back);
		return;
	}
	for (int i = 0; i < a->cal.n; i++)
		keep(a, a->cal.display, a->cal.ids[i], a->cal.fps[i]);
	fnl_log(FNL_LOG_INFO, "GPU rank: %d entries calibrated on %s in %.0f s",
			a->cal.n, a->cal.display, now_seconds() - a->cal.started);
	if (rank_save(a))
		fnl_log(FNL_LOG_INFO, "GPU rank: kept in %s", a->rank_path);
	else
		fnl_log(FNL_LOG_WARN, "GPU rank: %s could not be written (%s)",
				a->rank_path, strerror(errno));
	app_gpu_list_merge(a);
	char id[RANK_ID_MAX];
	for (int k = 1; ranked_id(a, a->cal.display, k, id, sizeof id); k++)
		fnl_log(FNL_LOG_INFO, "GPU rank %d on %s: %.0f fps, %s", k,
				a->cal.display, order_figure(a, a->cal.display, id), id);
	aim(a, a->cal.back);
}

/* Entering a state (defined below), which measured() uses to begin the next
 * entry's wait afresh. */
static void enter(App *a, int state, double now);

/* One entry's figure, sampled each frame from the means the frame cost
 * keeps (app_frame_cost()). */
static void sample(App *a)
{
	if (a->gpu_cost > 0.0) {
		a->cal.gpu_sum += a->gpu_cost;
		a->cal.gpu_k++;
	}
	const double cpu = app_cpu_cost(a);
	if (cpu > 0.0) {
		a->cal.cpu_sum += cpu;
		a->cal.cpu_k++;
	}
}

/* A frame's rate from its GPU and CPU seconds: the slower half's, 0 where
 * neither is known. */
static double rate_of(double gpu, double cpu)
{
	const double worst = gpu > cpu ? gpu : cpu;
	return worst > 0.0 ? 1.0 / worst : 0.0;
}

/* The entry measured, or left out with `why`: kept at 0 then, so that it
 * is not tried again at every start, as an entry never measured is. */
static void measured(App *a, const char *why)
{
	const double gpu = a->cal.gpu_k ? a->cal.gpu_sum / a->cal.gpu_k : 0.0;
	const double cpu = a->cal.cpu_k ? a->cal.cpu_sum / a->cal.cpu_k : 0.0;
	const double fps = rate_of(gpu, cpu);
	const char *id   = a->cal.ids[a->cal.i];
	const int g      = a->cal.group[a->cal.i];
	a->cal.dur_sum[g] += now_seconds() - a->cal.entry_at;
	a->cal.dur_k[g]++;
	if (why || !(fps > 0.0)) {
		fnl_log(FNL_LOG_WARN, "GPU rank: %s left out: %s", id,
				why ? why : "no frame cost measured");
	} else {
		a->cal.fps[a->cal.i] = fps;
		fnl_log(FNL_LOG_INFO,
				"GPU rank: %s: %.0f fps (GPU %.2f ms, CPU "
				"%.2f ms) in %.1f s",
				id, fps, gpu * 1e3, cpu * 1e3, now_seconds() - a->cal.entry_at);
	}
	a->cal.i++;
	enter(a, CAL_NEXT, now_seconds());
}

/* The best figure on the display measured so far: this calibration's, and
 * those kept for the entries it is not measuring again. 0 for none. */
static double best_here(const App *a)
{
	double best = 0.0;
	for (int j = 0; j < a->cal.i; j++)
		if (a->cal.fps[j] > best)
			best = a->cal.fps[j];
	for (int r = 0; r < a->rank_n; r++) {
		if (strcmp(a->rank[r].display, a->cal.display) != 0 ||
			!(a->rank[r].fps > best))
			continue;
		bool planned = false;
		for (int j = 0; j < a->cal.n && !planned; j++)
			planned = strcmp(a->cal.ids[j], a->rank[r].id) == 0;
		if (!planned)
			best = a->rank[r].fps;
	}
	return best;
}

/* One turn's frame cost into the span under way, and the span's figure
 * once CAL_BIN has passed -- the span then begun again -- or 0 while it
 * is still filling. The same costs sample() takes. */
static double bin_tick(App *a, double now)
{
	if (a->gpu_cost > 0.0) {
		a->cal.bin_gpu += a->gpu_cost;
		a->cal.bin_gk++;
	}
	const double cpu = app_cpu_cost(a);
	if (cpu > 0.0) {
		a->cal.bin_cpu += cpu;
		a->cal.bin_ck++;
	}
	if (now - a->cal.bin_at < CAL_BIN)
		return 0.0;
	a->cal.span_gpu = a->cal.bin_gk ? a->cal.bin_gpu / a->cal.bin_gk : 0.0;
	a->cal.span_cpu = a->cal.bin_ck ? a->cal.bin_cpu / a->cal.bin_ck : 0.0;
	a->cal.bin_gpu = a->cal.bin_cpu = 0.0;
	a->cal.bin_gk = a->cal.bin_ck = 0;
	a->cal.bin_at                 = now;
	return rate_of(a->cal.span_gpu, a->cal.span_cpu);
}

static void enter(App *a, int state, double now)
{
	a->cal.state = state;
	a->cal.at    = now;
	/* Each of the two watched states starts its spans afresh. */
	a->cal.bin_gpu = a->cal.bin_cpu = 0.0;
	a->cal.bin_gk = a->cal.bin_ck = 0;
	a->cal.bin_at                 = now;
	a->cal.bin_last = a->cal.mean_last = 0.0;
}

static double time_left(const App *a, double now);

/* Whether to calibrate, asked once the list is whole and again whenever
 * the window is on another display or the list has entries it had not.
 *
 * Only where the caller asked for it: an entry with no figure for this
 * display used to be measured as soon as the window was up, which is some
 * four seconds an entry with the window taking no input meanwhile -- not
 * what a program somebody opened to use should do on its first start. The
 * figures are used where they are kept, the window opens on the automatic
 * pick where they are not, and the box's last row measures on demand
 * (app_rank_recalibrate()). */
static void check_missing(App *a, const char *display)
{
	a->rank_dirty = false;
	if (a->rank_mode != RANK_CALIBRATE)
		return;
	/* A calibration moves the window between GPUs, which gpu_switch_owed()
	 * refuses while a second window is open or opening. Asked again when the
	 * host is one window: `rank_dirty` stays set, and the ask is an atomic
	 * load until then. */
	if (fnl_atomic_load_u32(&a->host->window_count) > 1 ||
		fnl_atomic_load_u32(&a->host->opening) > 0) {
		a->rank_dirty = true;
		return;
	}
	/* The caller's display, the one its rank_display and list hash stand
	 * for -- derived once a turn, not again here, so a publish landing
	 * between the two cannot key the calibration to one display and the
	 * tick to another. No output yet -- on Wayland none is known until the
	 * window is shown -- is no display to measure: asked again once it
	 * enters one. */
	if (!display[0] || strncmp(display, "- ", 2) == 0)
		return;
	const bool all = !a->rank_forced;
	a->rank_forced = true;
	plan(a, display, all);
	if (a->cal.n == 0)
		return;
	begin(a, all ? "FNL_GPU_RANK=calibrate"
				 : "entries with no figure on this display");
	enter(a, CAL_NEXT, now_seconds());
}

bool app_rank_recalibrates(const App *a)
{
	/* Not with a second window open or opening: the moves a calibration
	 * needs are refused then (gpu_switch_owed()), and its window would take
	 * no input while it waited. The box's row is dim on the same answer. */
	return (a->rank_mode == RANK_AUTO || a->rank_mode == RANK_CALIBRATE) &&
		   fnl_atomic_load_u32(&a->host->window_count) <= 1 &&
		   fnl_atomic_load_u32(&a->host->opening) == 0;
}

void app_rank_recalibrate(App *a)
{
	if (!app_rank_recalibrates(a) || app_rank_holds_input(a))
		return;
	fnl_atomic_store_bool(&a->rank_recal_owed, true);
	fnl_mutex_lock(&a->lock);
	a->want_user_gen++;
	fnl_cond_signal(&a->cond);
	fnl_mutex_unlock(&a->lock);
}

/* A calibration asked for from the GPU box: the figures of every display
 * connected dropped -- this one's, at any mode, and the others', which
 * are measured when the window is there -- and every entry here measured
 * now. A display not connected keeps its own. */
static void recalibrate(App *a, const char *display)
{
	char names[OUTPUTS_MAX][OUTPUT_NAME_MAX];
	int cur;
	int k = BACKEND(a)->outputs(a, names, OUTPUTS_MAX, &cur);
	if (k < 0)
		k = 0;
	/* A key is "<name> WxH@R" and a name may hold spaces (Cocoa's): the
	 * name is what comes before the last. */
	const char *sp    = strrchr(display, ' ');
	const size_t here = sp ? (size_t)(sp - display) : strlen(display);
	fnl_mutex_lock(&a->lock);
	int n = 0;
	for (int i = 0; i < a->rank_n; i++) {
		const char *d    = a->rank[i].display;
		sp               = strrchr(d, ' ');
		const size_t len = sp ? (size_t)(sp - d) : strlen(d);
		bool drop        = len == here && strncmp(d, display, here) == 0;
		for (int j = 0; j < k && !drop; j++)
			drop = output_name_len(names[j]) == len &&
				   strncmp(d, names[j], len) == 0;
		if (!drop)
			a->rank[n++] = a->rank[i];
	}
	const int dropped = a->rank_n - n;
	a->rank_n         = n;
	fnl_mutex_unlock(&a->lock);
	fnl_log(FNL_LOG_INFO,
			"GPU rank: %d figures of the displays connected "
			"dropped, asked for from the GPU box",
			dropped);
	plan(a, display, true);
	if (a->cal.n == 0)
		return;
	begin(a, "asked for from the GPU box");
	enter(a, CAL_NEXT, now_seconds());
}

/* The list's entries, by key, as a sum of their hashes: what changes when
 * an entry joins the list or leaves it, in any order. */
static unsigned long list_hash(App *a)
{
	unsigned long sum = 0;
	fnl_mutex_lock(&a->lock);
	for (int i = 0; i < a->gpu_list_n; i++) {
		char id[RANK_ID_MAX];
		app_gpu_entry_id(a, &a->gpu_list[i], id, sizeof id);
		unsigned long h = 5381;
		for (const char *p = id; *p; p++)
			h = h * 33 + (unsigned char)*p;
		sum += h;
	}
	fnl_mutex_unlock(&a->lock);
	return sum;
}

/* Whether the window has stayed put on one output for RANK_QUIET: the same
 * display, the same size, and on that output alone. Asked every turn, so
 * the time counts from the last change. What waits on it is what a window
 * still being carried or stretched across monitors should not set off --
 * a calibration of the output it is passing over, or a move to another
 * GPU in the middle of the drag: on Wayland, which says when a window
 * straddles two, the calibration that began as a resize reached the
 * second monitor and another when the window was there whole. */
static bool quiet(App *a, const char *display, double now)
{
	fnl_mutex_lock(&a->lock);
	const bool straddles = strstr(a->want_output, " + ") != NULL;
	const int w = a->want_w, h = a->want_h;
	fnl_mutex_unlock(&a->lock);
	if (strcmp(display, a->rank_seen) != 0 || w != a->rank_seen_w ||
		h != a->rank_seen_h) {
		snprintf(a->rank_seen, sizeof a->rank_seen, "%s", display);
		a->rank_seen_w  = w;
		a->rank_seen_h  = h;
		a->rank_seen_at = now;
	}
	return !straddles && now - a->rank_seen_at >= RANK_QUIET;
}

void app_rank_tick(App *a)
{
	const double now = now_seconds();
	const uint32_t gen = fnl_atomic_load_u32(&a->display_gen);
	/* A calibration under way when a window opens on the host ends here,
	 * with what it measured: its moves between GPUs are refused from now on
	 * (gpu_switch_owed()), and its window would take no input while it
	 * waited for one. Asked again when the host is one window. */
	if (a->cal.state != CAL_OFF &&
		fnl_atomic_load_u32(&a->host->window_count) > 1) {
		fnl_log(FNL_LOG_INFO,
				"GPU rank: calibration ended, a second window is open");
		a->cal.n = a->cal.i;
		finish(a);
		a->rank_dirty = true;
		return;
	}
	if (a->cal.state == CAL_OFF) {
		if (a->rank_mode == RANK_OFF || !settled(a))
			return;
		/* The window on another display: the list ordered for it, and
		 * measured there where nothing is kept. A list with entries it
		 * had not -- a GPU added -- is asked about the same way. Two
		 * stamps gate the derivation: the display's, which moves on every
		 * publish that can change the display (a mode switch that keeps
		 * the size moves it too), and the list's, which moves wherever the
		 * list gained or lost an entry with no display fact to say so. A
		 * turn that changed neither costs a compare instead of two round
		 * trips through the backend. */
		char display[RANK_DISPLAY_MAX];
		const uint32_t lgen = fnl_atomic_load_u32(&a->gpu_list_gen);
		if (a->rank_gen_seen != gen || a->rank_list_gen_seen != lgen) {
			/* Both stamps before the derivation: a publish that lands
			 * between them and it is seen on the next turn, where a flag
			 * read and cleared would lose it. */
			a->rank_gen_seen      = gen;
			a->rank_list_gen_seen = lgen;
			display_now(a, display, sizeof display);
			const unsigned long hash = list_hash(a);
			const bool moved = a->rank_display[0] &&
							   strcmp(display, a->rank_display) != 0;
			if (moved || hash != a->rank_list_hash) {
				/* Moved to another display, not opened on one: to its first
				 * entry, or the caller's index, as the start would be -- once
				 * anything missing there is measured, after which the
				 * calibration sends it anyway; left where it is on a GPU the
				 * caller named. Not where the ranking is only kept, as a
				 * harness round's is, whose own moves between displays are
				 * checked as such. */
				/* An aim the calibration just set is not overwritten: the
				 * move to it waits until the window is settled here. */
				if (moved && a->rank_mode == RANK_AUTO && !a->rank_land[0] &&
					a->rank_target == 0)
					aim(a, "");
				fnl_mutex_lock(&a->lock);
				snprintf(a->rank_display, sizeof a->rank_display, "%s",
						 display);
				fnl_mutex_unlock(&a->lock);
				a->rank_list_hash = hash;
				a->rank_dirty     = true;
				/* The list is merged already, and sorted for the display it
				 * stands on when it has not moved: a merge always sorts for
				 * the current one, so only a move asks for it again. */
				if (moved)
					app_gpu_list_merge(a);
				/* Ordered for this display now: its numbers are said again. */
				if (moved && a->rank_n > 0)
					app_gpu_list_say(a);
			}
		} else {
			/* The display the last derivation settled on: the recalibration
			 * below is the only reader that runs with the gate closed. */
			fnl_mutex_lock(&a->lock);
			snprintf(display, sizeof display, "%s", a->rank_display);
			fnl_mutex_unlock(&a->lock);
		}
		if (fnl_atomic_load_bool(&a->rank_recal_owed) &&
			fnl_atomic_load_u32(&a->host->window_count) <= 1 &&
			fnl_atomic_load_u32(&a->host->opening) == 0) {
			fnl_atomic_store_bool(&a->rank_recal_owed, false);
			recalibrate(a, display);
			if (fnl_atomic_load_bool(&a->calibrating))
				return;
		}
		/* Ordered for the display at once, above; measured there, and moved
		 * to its first entry, once the window has stayed on it. A
		 * recalibration asked for by hand does not wait. */
		if (!quiet(a, display, now))
			return;
		if (a->rank_dirty) {
			check_missing(a, display);
			if (fnl_atomic_load_bool(&a->calibrating))
				return;
		}
		/* A move is refused while another window is open (gpu_switch_owed()
		 * clears the request), so an aim is kept until the host is one
		 * window rather than spent on a request that cannot be made. */
		if (fnl_atomic_load_u32(&a->host->window_count) > 1)
			return;
		/* Back to the entry a named GPU was on before the calibration:
		 * by its key, wherever the ranking has put it in the list. */
		if (a->rank_land[0]) {
			int cur;
			const int at = entry_of(a, a->rank_land, &cur);
			if (at < 0)
				fnl_log(FNL_LOG_WARN,
						"GPU rank: %s, the GPU named, is no "
						"longer in the list",
						a->rank_land);
			else if (at != cur)
				request(a, at);
			a->rank_land[0] = '\0';
			return;
		}
		if (a->rank_target == 0)
			return;
		/* The list is ordered by the ranking: its k-th entry is the k-th
		 * ranked, where so many are measured -- of the named renderer's
		 * entries alone where one was named, since a move to another
		 * renderer's would draw with one that was not asked for. */
		const int want = a->rank_target;
		int k          = want - 1;
		a->rank_target = 0;
		fnl_mutex_lock(&a->lock);
		if (a->renderer_named != FNL_RENDERER_DEFAULT) {
			int seen = 0;
			k        = -1;
			for (int i = 0; i < a->gpu_list_n && k < 0; i++)
				if (a->gpu_list[i].renderer == a->renderer_named &&
					++seen == want)
					k = i;
		}
		const bool ranked =
			k >= 0 && k < a->gpu_list_n && a->gpu_list[k].fps > 0.0f;
		fnl_mutex_unlock(&a->lock);
		if (ranked)
			request(a, k);
		else
			fnl_log(FNL_LOG_WARN, "GPU rank: no entry %d%s measured here", want,
					a->renderer_named != FNL_RENDERER_DEFAULT
						? " of the named renderer's"
						: "");
		return;
	}
	/* The window taken to another display meanwhile -- its own unplugged,
	 * which moves it (kms.c's outputs_rescan()) -- ends the calibration
	 * with what was measured where it was, and the display it is on now
	 * is asked about afresh. A figure measured after would be the new
	 * display's under the old one's name. */
	if (a->rank_gen_seen != gen) {
		char display[RANK_DISPLAY_MAX];
		display_now(a, display, sizeof display);
		if (strcmp(display, a->cal.display) != 0) {
			fnl_log(FNL_LOG_INFO,
					"GPU rank: the window left %s for %s with %d "
					"of %d entries measured; those kept",
					a->cal.display, display, a->cal.i, a->cal.n);
			a->cal.n = a->cal.i;
			finish(a);
			/* And to the first entry on the display it is on now, once
			 * settled there, not the one it was measuring when it moved --
			 * or back to the GPU named, or the index asked for (aim()). */
			aim(a, a->cal.back);
			return;
		}
		/* The display the calibration is on: the stamp is consumed, so the
		 * CAL_OFF path does not derive it again. A change is left
		 * unconsumed, for that path to set rank_dirty, aim and say. */
		a->rank_gen_seen = gen;
	}
	/* Only a turn that presented a frame may take a figure or advance a
	 * span: a covered, minimized or uninvited window presents none, and
	 * its means are the last ones taken -- read again every turn they
	 * would keep a figure the entry never produced, and the wall clock
	 * alone would keep a zero for one it never drew. And a calibration
	 * that cannot draw for CAL_LIMIT ends with what it measured, rather
	 * than holding the window's input for a frame nobody shows. */
	const uint64_t presents = a->presents;
	const bool drew         = presents != a->cal.seen_presents;
	a->cal.seen_presents    = presents;
	if (drew)
		a->cal.drew_at = now;
	if (now - a->cal.drew_at > CAL_LIMIT) {
		fnl_log(FNL_LOG_WARN,
				"GPU rank: the window drew nothing for %.0f s; the "
				"calibration ends with %d of %d entries measured",
				CAL_LIMIT, a->cal.i, a->cal.n);
		a->cal.n = a->cal.i;
		finish(a);
		return;
	}
	/* Drawn throughout, whether the scene moves or not: the cost is a
	 * frame's, and the banner says the time left. */
	fnl_mutex_lock(&a->lock);
	a->want_user_gen++;
	fnl_mutex_unlock(&a->lock);
	int cur, at;
	switch (a->cal.state) {
	case CAL_NEXT:
		if (a->cal.i >= a->cal.n) {
			finish(a);
			return;
		}
		if (now - a->cal.at > CAL_LIMIT) {
			/* The list never settled -- a listing or a move that hangs:
			 * the calibration ends with what it measured rather than
			 * waiting for it with the window's input held. */
			fnl_log(FNL_LOG_WARN,
					"GPU rank: the list did not settle in %.0f s; the "
					"calibration ends with %d of %d entries measured",
					CAL_LIMIT, a->cal.i, a->cal.n);
			a->cal.n = a->cal.i;
			finish(a);
			return;
		}
		if (!settled(a))
			return;
		a->cal.entry_at = now;
		/* What the panel says against what it comes to: the elapsed
		 * time and the estimate at each entry, and the whole at the end
		 * (finish()). */
		fnl_log(FNL_LOG_INFO,
				"GPU rank: entry %d of %d at %.1f s, %.0f s "
				"left by the estimate",
				a->cal.i + 1, a->cal.n, now - a->cal.started,
				time_left(a, now));
		at = entry_of(a, a->cal.ids[a->cal.i], &cur);
		if (at < 0) {
			measured(a, "not in the list");
			return;
		}
		if (at != cur)
			request(a, at);
		enter(a, CAL_LANDING, now);
		return;
	case CAL_LANDING:
		if (now - a->cal.at > CAL_LIMIT) {
			measured(a, "not drawn on");
			return;
		}
		if (!settled(a))
			return;
		at = entry_of(a, a->cal.ids[a->cal.i], &cur);
		if (at >= 0 && at == cur) {
			enter(a, CAL_SETTLING, now);
		} else if (now - a->cal.at > 1.0) {
			/* The move ended elsewhere: the library went back. */
			measured(a, "would not draw here");
		}
		return;
	case CAL_SETTLING: {
		if (!drew)
			return;
		/* Settled when two spans in a row agree -- the clocks have risen and
		 * the means caught up -- after CAL_SETTLE_MIN, or at CAL_SETTLE
		 * whatever they say: a GPU that ramps slowly gets the time it
		 * always had. */
		const double fps = bin_tick(a, now);
		const double in  = now - a->cal.at;
		bool steady      = false;
		if (fps > 0.0) {
			steady =
				in >= CAL_SETTLE_MIN && a->cal.bin_last > 0.0 &&
				fabs(fps - a->cal.bin_last) <= CAL_STEADY * a->cal.bin_last;
			a->cal.bin_last = fps;
		}
		if (!steady && in < CAL_SETTLE)
			return;
		/* Settled far behind the best measured here: its figure decides
		 * nothing at the top of the list, so the steady span's is kept and
		 * the entry not sampled -- a figure that may be low, for an entry
		 * that loses either way. */
		const double best = best_here(a);
		if (steady && best > 0.0 && fps < CAL_OUTCLASS * best) {
			fnl_log(FNL_LOG_INFO,
					"GPU rank: %s settled under half of the "
					"best here (%.0f fps); not sampled",
					a->cal.ids[a->cal.i], best);
			a->cal.gpu_sum = a->cal.span_gpu;
			a->cal.cpu_sum = a->cal.span_cpu;
			a->cal.gpu_k   = a->cal.span_gpu > 0.0;
			a->cal.cpu_k   = a->cal.span_cpu > 0.0;
			measured(a, NULL);
			return;
		}
		a->cal.gpu_sum = a->cal.cpu_sum = 0.0;
		a->cal.gpu_k = a->cal.cpu_k = 0;
		enter(a, CAL_SAMPLING, now);
		return;
	}
	case CAL_SAMPLING: {
		if (!drew)
			return;
		at = entry_of(a, a->cal.ids[a->cal.i], &cur);
		if (at < 0 || at != cur) {
			measured(a, "moved off while measured");
			return;
		}
		sample(a);
		/* Measured once the running mean moves less than CAL_CONVERGE in a
		 * span, after CAL_SAMPLE_MIN, or at CAL_SAMPLE whatever it does. */
		if (now - a->cal.at >= CAL_SAMPLE) {
			measured(a, NULL);
			return;
		}
		if (now - a->cal.bin_at < CAL_BIN)
			return;
		a->cal.bin_at = now;
		const double mean =
			rate_of(a->cal.gpu_k ? a->cal.gpu_sum / a->cal.gpu_k : 0.0,
					a->cal.cpu_k ? a->cal.cpu_sum / a->cal.cpu_k : 0.0);
		if (now - a->cal.at >= CAL_SAMPLE_MIN && a->cal.mean_last > 0.0 &&
			fabs(mean - a->cal.mean_last) <= CAL_CONVERGE * a->cal.mean_last)
			measured(a, NULL);
		else
			a->cal.mean_last = mean;
		return;
	}
	default: return;
	}
}

/* ---- what the window says meanwhile ------------------------------------- */

static void entry_words(const char *id, char *out, size_t size)
{
	const char *bar = strchr(id, '|');
	if (!bar) {
		snprintf(out, size, "%s", id);
		return;
	}
	const FnlRenderer r = renderer_of_key(id, (size_t)(bar - id));
	snprintf(out, size, "%s, %s", app_renderer_name(r),
			 strcmp(bar + 1, "software") == 0 ? "software rasteriser"
											  : bar + 1);
}

/* The time the calibration has left: each entry still to measure at what
 * those measured took, or at its settle, sample and a switch before one
 * has been, less what the one under way has had. */
static double time_left(const App *a, double now)
{
	/* Each group at what its entries took, or before one has been
	 * measured at a guess: a settle and a sample at their least with a
	 * span more of each, which is what a steady GPU takes, and a switch;
	 * and for the entries after a move to another kernel driver CAL_MOVE
	 * more -- the move, and on NVK a slower build (5.7 and 4.7 s against
	 * 3.7 on schenker, 2026-10-26, when every entry took the whole settle
	 * and sample). */
	double each[2];
	each[0]     = a->cal.dur_k[0] ? a->cal.dur_sum[0] / a->cal.dur_k[0]
								  : CAL_SETTLE_MIN + CAL_SAMPLE_MIN +
									2.0 * CAL_BIN + CAL_SWITCH;
	each[1]     = a->cal.dur_k[1] ? a->cal.dur_sum[1] / a->cal.dur_k[1]
								  : each[0] + CAL_MOVE;
	double left = 0.0;
	for (int j = a->cal.i; j < a->cal.n; j++)
		left += each[a->cal.group[j]];
	if (a->cal.i < a->cal.n) {
		const double cur = each[a->cal.group[a->cal.i]];
		double into      = now - a->cal.entry_at;
		if (into > cur * 0.95)
			into = cur * 0.95;
		left -= into;
	}
	return left > 1.0 ? left : 1.0;
}

FnlResult app_rank_draw(App *a, int w, int h)
{
	if (!fnl_atomic_load_bool(&a->calibrating))
		return FNL_OK;
	const double now = now_seconds();
	/* The seconds left in the large size, baked for every code point the
	 * faces carry as every size is (tools/mkfont.c), and the word beside
	 * them in the proportional one. */
	const char *head = CAL_HEAD;
	const char *unit = CAL_UNIT;
	char secs[16], entry[RANK_ID_MAX + 64], what[RANK_ID_MAX + 16];
	snprintf(secs, sizeof secs, "%.0f", time_left(a, now));
	const int i = a->cal.i < a->cal.n ? a->cal.i : a->cal.n - 1;
	entry_words(i >= 0 ? a->cal.ids[i] : "", what, sizeof what);
	snprintf(entry, sizeof entry, "%d of %d: %s", i + 1, a->cal.n, what);
	const char *note = CAL_NOTE;

	/* Centred over everything, the bar included, and one width for the
	 * whole calibration: the widest line any of its entries will show, the
	 * seconds at their widest, and never less than CAL_PANEL_W -- a panel
	 * sized to the line on screen would change width with every entry's
	 * name. Narrower than the window only where that is narrow. */
	const int pad = CAL_PAD, gap = CAL_GAP, bar_h = 8;
	const int lh = text_height(TEXT_LARGE), uh = text_height(TEXT_UI);
	const int sw = text_width(secs, TEXT_LARGE);
	/* Frozen when the plan was made; the clamp is this frame's, since the
	 * window may have narrowed since. */
	int pw = a->cal.panel_w > 0 ? a->cal.panel_w : CAL_PANEL_W;
	if (pw > w - 16)
		pw = w - 16;
	const int ph = 2 * pad + uh + gap + lh + gap + bar_h + gap + uh + gap + uh;
	const int x = (w - pw) / 2, y = (h - ph) / 2;

	overlay_flush_rects(a);
	overlay_flush_shapes(a);
	const FnlResult rc = text_end(a);

	const float *line = a->pal[COL_LINE_ON];
	overlay_rect(a, x, y, pw, ph, a->pal[COL_CTL_BOX]);
	overlay_rect(a, x, y, pw, 1, line);
	overlay_rect(a, x, y + ph - 1, pw, 1, line);
	overlay_rect(a, x, y, 1, ph, line);
	overlay_rect(a, x + pw - 1, y, 1, ph, line);

	int ty = y + pad;
	text_draw(a, x + pad, ty, TEXT_UI, a->pal[COL_CTL_LIVE], head);
	ty += uh + gap;
	/* The unit on the digits' baseline: both sit on their cap heights. */
	text_draw(a, x + pad, ty, TEXT_LARGE, a->pal[COL_ICON_ON], secs);
	text_draw(a, x + pad + sw + gap, ty + lh - uh, TEXT_UI, a->pal[COL_ICON_ON],
			  unit);
	ty += lh + gap;
	/* How far it has come, by time: what the figure above says. */
	const double total = (now - a->cal.started) + time_left(a, now);
	const double part  = total > 0.0 ? (now - a->cal.started) / total : 0.0;
	const int track    = pw - 2 * pad;
	overlay_rect(a, x + pad, ty, track, bar_h, a->pal[COL_CTL_DIM]);
	overlay_rect(a, x + pad, ty, (int)(track * part), bar_h,
				 a->pal[COL_CTL_LIVE]);
	ty += bar_h + gap;
	text_draw(a, x + pad, ty, TEXT_UI, a->pal[COL_ICON_ON], entry);
	ty += uh + gap;
	text_draw(a, x + pad, ty, TEXT_UI, a->pal[COL_CTL_DIM], note);
	return rc;
}

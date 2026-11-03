/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What a frame's two cost figures are made of: FNL_COST_PARTS=1 beside
 * --frame-times. The measurement docs/comparable-frame-costs.md made the
 * renderers' spans one on, and kept as the way to ask why an entry ranks
 * where it does: these lines say which part of each half is what, and
 * which threads do the work.
 *
 *   frame parts: cpu ...      the render thread's CPU time over the two
 *                             spans the frame line gives in wall time --
 *                             record+submit (late_cost's) and present --
 *                             so a wait that spins shows as CPU
 *   frame parts: gpu ...      one GPU span's mean and its parts, as the
 *                             renderer stamps them (parts_gpu())
 *   frame threads ...         Linux: every thread's CPU per frame, the
 *                             busiest first, the library's two named --
 *                             which of a driver's threads exist and how
 *                             much each does
 *
 * Read from the environment once, at fnl_init(), as FNL_VSYNC is. The
 * marks cost two clock reads each, the thread list a read of /proc once a
 * line, on the render thread and outside both marked spans.
 */

#include "core/app.h"

#if defined(__linux__)
#include <dirent.h>
#include <unistd.h>
#endif

/* Threads remembered between lines, and how many a line names. */
#define PARTS_THREADS_MAX  256
#define PARTS_THREADS_SAID 10

/* The kernel's ceiling on pid_max (PID_MAX_LIMIT, 2^22). Bounding a /proc
 * name by it is what lets gcc see that a thread id fits a name's 16 bytes;
 * a larger one is no task of this process. */
#define PARTS_TID_MAX 4194304L

typedef struct PartsThread {
	long     tid;
	uint64_t ns;        /* on the CPU, as schedstat last said */
	uint64_t delta;     /* since the line before */
	bool     seen;
	bool     fresh;     /* first seen since the line before */
	char     name[16];
} PartsThread;

struct PartsState {
	/* The frame being drawn: where its marks fell, wall and CPU. */
	double      rec_wall, rec_cpu, pre_wall, pre_cpu;
	bool        rec_set, pre_set;

	/* The line's window. */
	double      rec_wall_sum, rec_cpu_sum, pre_wall_sum, pre_cpu_sum;
	/* The CPU sums are of the frames whose CPU time was known, and are
	 * averaged over those alone. */
	int         frames, cpu_frames;

	const char *const *gpu_names;
	int         gpu_n;
	double      gpu_sum[PARTS_GPU_MAX];
	int         gpu_samples;

	/* The display-period samples of the line's window (parts_period()):
	 * how many, how many taken, and the sums of their parts. */
	int         per_n, per_taken;
	double      per_sum[4];

	long        render_tid;
	PartsThread th[PARTS_THREADS_MAX];
	int         th_n;
	bool        th_primed;
};

void parts_open(App *a)
{
	const char *v = getenv("FNL_COST_PARTS");
	if (!v || !v[0] || (v[0] == '0' && !v[1]))
		return;
	if (!a->frame_times) {
		fnl_log(FNL_LOG_INFO, "FNL_COST_PARTS: nothing without --frame-times "
							  "(FnlInitDesc::frame_times)");
		return;
	}
	a->parts = calloc(1, sizeof *a->parts);
	if (!a->parts) {
		fnl_log(FNL_LOG_WARN, "FNL_COST_PARTS: out of memory; off");
		return;
	}
	fnl_log(FNL_LOG_INFO, "frame parts: on (FNL_COST_PARTS)");
}

void parts_close(App *a)
{
	free(a->parts);
	a->parts = NULL;
}

#if defined(__linux__)

/* The calling thread's id, from what /proc/thread-self points at
 * ("<pid>/task/<tid>"): gettid() wants _GNU_SOURCE, which app.h does not
 * set. */
static long own_tid(void)
{
	char buf[64];
	const ssize_t n = readlink("/proc/thread-self", buf, sizeof buf - 1);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	const char *slash = strrchr(buf, '/');
	return slash ? strtol(slash + 1, NULL, 10) : 0;
}

static bool read_line(const char *path, char *out, size_t size)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	const bool ok = fgets(out, (int)size, f) != NULL;
	fclose(f);
	if (ok)
		out[strcspn(out, "\n")] = '\0';
	return ok;
}

static PartsThread *thread_find(struct PartsState *p, long tid)
{
	for (int i = 0; i < p->th_n; i++)
		if (p->th[i].tid == tid)
			return &p->th[i];
	return NULL;
}

/* Every thread's time on the CPU into the table, each one's delta since
 * the last scan beside it; a thread gone since is dropped. A new one's
 * delta is all it has done, which is all within the window but for one
 * that started before the first scan, and that one primes. */
static void threads_scan(struct PartsState *p)
{
	DIR *d = opendir("/proc/self/task");
	if (!d)
		return;
	for (int i = 0; i < p->th_n; i++)
		p->th[i].seen = false;
	const struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		char *end;
		const long tid = strtol(e->d_name, &end, 10);
		if (end == e->d_name || *end || tid <= 0 || tid > PARTS_TID_MAX)
			continue;
		char path[64], line[64];
		snprintf(path, sizeof path, "/proc/self/task/%ld/schedstat", tid);
		if (!read_line(path, line, sizeof line))
			continue;
		const uint64_t ns = strtoull(line, NULL, 10);
		PartsThread *t = thread_find(p, tid);
		if (!t) {
			if (p->th_n == PARTS_THREADS_MAX)
				continue;
			t = &p->th[p->th_n++];
			memset(t, 0, sizeof *t);
			t->tid = tid;
			t->fresh = p->th_primed;
			t->delta = p->th_primed ? ns : 0;
			snprintf(path, sizeof path, "/proc/self/task/%ld/comm", tid);
			if (!read_line(path, t->name, sizeof t->name))
				snprintf(t->name, sizeof t->name, "%ld", tid);
		} else {
			t->fresh = false;
			t->delta = ns >= t->ns ? ns - t->ns : 0;
		}
		t->ns = ns;
		t->seen = true;
	}
	closedir(d);
	int k = 0;
	for (int i = 0; i < p->th_n; i++)
		if (p->th[i].seen)
			p->th[k++] = p->th[i];
	p->th_n = k;
	p->th_primed = true;
}

/* The busiest threads of the window, the library's two by role: `render`
 * this one, `main` the process's first, which is the window's thread in
 * every example. A thread that never named itself has the program's name,
 * and is `unnamed:<tid>` here: a driver's, mostly, since the library's
 * others (the GPU lister, the mover, Vulkan's reaper) sleep while frames
 * are drawn. */
static void threads_log(struct PartsState *p, int frames)
{
	threads_scan(p);
	if (frames <= 0)
		return;
	char own[16];
	if (!read_line("/proc/self/comm", own, sizeof own))
		own[0] = '\0';
	int order[PARTS_THREADS_MAX];
	int n = 0;
	for (int i = 0; i < p->th_n; i++) {
		if (!p->th[i].delta)
			continue;
		int j = n++;
		while (j > 0 && p->th[order[j - 1]].delta < p->th[i].delta) {
			order[j] = order[j - 1];
			j--;
		}
		order[j] = i;
	}
	const long pid = (long)getpid();
	char buf[400];
	size_t at = 0;
	for (int k = 0; k < n && k < PARTS_THREADS_SAID; k++) {
		const PartsThread *t = &p->th[order[k]];
		char unnamed[32];
		snprintf(unnamed, sizeof unnamed, "unnamed:%ld", t->tid);
		const char *name = t->tid == p->render_tid  ? "render"
						 : t->tid == pid            ? "main"
						 : !strcmp(t->name, own)    ? unnamed
						 : t->name;
		const int w = snprintf(buf + at, sizeof buf - at, "%s%s%s %.3f",
							   k ? ", " : "", name, t->fresh ? "+" : "",
							   (double)t->delta * 1e-6 / frames);
		if (w < 0 || (size_t)w >= sizeof buf - at)
			break;
		at += (size_t)w;
	}
	fnl_log(FNL_LOG_INFO, "frame threads, ms a frame (+ new): %s; "
			"%d threads, %d busy", at ? buf : "none", p->th_n, n);
}

#else

static long own_tid(void)
{
	return 0;
}

static void threads_scan(struct PartsState *p)
{
	(void)p;
}

static void threads_log(struct PartsState *p, int frames)
{
	(void)p;
	(void)frames;
}

#endif

void parts_mark(App *a, FrameMark m, double cpu)
{
	struct PartsState *p = a->parts;
	if (!p)
		return;
	const double wall = now_seconds();
	switch (m) {
	case FRAME_MARK_REC:
		/* Again where the frame waited after its first: the span starts
		 * past every wait, as late_cost's does. */
		if (!p->render_tid)
			p->render_tid = own_tid();
		p->rec_wall = wall;
		p->rec_cpu = cpu;
		p->rec_set = true;
		p->pre_set = false;
		break;
	case FRAME_MARK_PRESENT:
		if (!p->rec_set)
			break;
		p->pre_wall = wall;
		p->pre_cpu = cpu;
		p->pre_set = true;
		break;
	case FRAME_MARK_PRESENTED:
		if (!p->rec_set || !p->pre_set)
			break;
		p->rec_wall_sum += p->pre_wall - p->rec_wall;
		p->pre_wall_sum += wall - p->pre_wall;
		if (cpu >= 0.0 && p->rec_cpu >= 0.0) {
			p->rec_cpu_sum += p->pre_cpu - p->rec_cpu;
			p->pre_cpu_sum += cpu - p->pre_cpu;
			p->cpu_frames++;
		}
		p->frames++;
		p->rec_set = p->pre_set = false;
		break;
	}
}

void parts_gpu(App *a, const char *const *names, const double *seconds,
			   int n)
{
	struct PartsState *p = a->parts;
	if (!p || n <= 0)
		return;
	if (n > PARTS_GPU_MAX)
		n = PARTS_GPU_MAX;
	if (names != p->gpu_names || n != p->gpu_n) {
		p->gpu_names = names;
		p->gpu_n = n;
		memset(p->gpu_sum, 0, sizeof p->gpu_sum);
		p->gpu_samples = 0;
	}
	for (int i = 0; i < n; i++)
		p->gpu_sum[i] += seconds[i];
	p->gpu_samples++;
}

void parts_period(App *a, const double s[4], bool taken)
{
	struct PartsState *p = a->parts;
	if (!p)
		return;
	p->per_n++;
	p->per_taken += taken;
	for (int i = 0; i < 4; i++)
		p->per_sum[i] += s[i];
}

static void sums_clear(struct PartsState *p)
{
	p->per_n = p->per_taken = 0;
	memset(p->per_sum, 0, sizeof p->per_sum);
	p->rec_wall_sum = p->rec_cpu_sum = 0.0;
	p->pre_wall_sum = p->pre_cpu_sum = 0.0;
	p->frames = p->cpu_frames = 0;
	memset(p->gpu_sum, 0, sizeof p->gpu_sum);
	p->gpu_samples = 0;
}

void parts_log(App *a)
{
	struct PartsState *p = a->parts;
	if (!p)
		return;
	const double n = p->frames;
	if (p->frames > 0) {
		const double c = p->cpu_frames;
		if (p->cpu_frames > 0)
			fnl_log(FNL_LOG_INFO, "frame parts: cpu record+submit %.3f ms "
					"(%.3f on the CPU), present %.3f ms (%.3f on the CPU), "
					"%d frames",
					p->rec_wall_sum * 1e3 / n, p->rec_cpu_sum * 1e3 / c,
					p->pre_wall_sum * 1e3 / n, p->pre_cpu_sum * 1e3 / c,
					p->frames);
		else
			fnl_log(FNL_LOG_INFO, "frame parts: cpu record+submit %.3f ms, "
					"present %.3f ms (CPU time not known here), %d frames",
					p->rec_wall_sum * 1e3 / n, p->pre_wall_sum * 1e3 / n,
					p->frames);
	}
	if (p->gpu_samples > 0) {
		const double g = p->gpu_samples;
		char buf[300];
		size_t at = 0;
		for (int i = 1; i < p->gpu_n; i++) {
			const int w = snprintf(buf + at, sizeof buf - at, "%s%s %.3f",
								   i > 1 ? ", " : "", p->gpu_names[i],
								   p->gpu_sum[i] * 1e3 / g);
			if (w < 0 || (size_t)w >= sizeof buf - at)
				break;
			at += (size_t)w;
		}
		fnl_log(FNL_LOG_INFO, "frame parts: gpu %s %.3f ms%s%s, %d samples",
				p->gpu_names[0], p->gpu_sum[0] * 1e3 / g,
				at ? ": " : "", at ? buf : "", p->gpu_samples);
	}
	if (p->per_n > 0) {
		const double k = 1e3 / p->per_n;
		fnl_log(FNL_LOG_INFO, "frame parts: period samples %d, %d taken, "
				"mean %.2f ms = held %.2f + device %.2f + present %.2f + "
				"cost %.2f", p->per_n, p->per_taken,
				(p->per_sum[0] + p->per_sum[1] + p->per_sum[2] +
				 p->per_sum[3]) * k, p->per_sum[0] * k, p->per_sum[1] * k,
				p->per_sum[2] * k, p->per_sum[3] * k);
	}
	threads_log(p, p->frames);
	sums_clear(p);
}

/* A window that drew again after a rest: the rest's time is nobody's
 * frame, so the thread figures are taken again from here. */
void parts_reset(App *a)
{
	struct PartsState *p = a->parts;
	if (!p)
		return;
	sums_clear(p);
	p->rec_set = p->pre_set = false;
	threads_scan(p);
}

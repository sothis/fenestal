/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * fenestal-dev open -- what it costs to get a window to its first frame.
 *
 *   fenestal-dev open [<lifetimes>]            (default 5)
 *
 * Each lifetime is fnl_init(), a run that stops once the second frame is
 * being drawn -- the first has been recorded, submitted and presented by
 * then -- and fnl_shutdown(), each timed. The first lifetime in a process is
 * the cold one, the one a program's user waits for: the loaders and drivers
 * are read from disk by it and kept by the process after it. The later
 * ones are what a GPU move or a device rebuild pays, and the figure to read
 * a change in the open path against when the machine's file cache is warm
 * for both.
 *
 * Beside the pace instrument and for the same reason: the same program,
 * cross-built, times the same path on every backend and renderer, so a
 * cost one platform alone pays shows against the rest. A consumer of the
 * installed header alone.
 */

#include <fenestal/fenestal.h>

#include "dev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* One clock for every system: C11's, which the UCRT, glibc and macOS give at
 * better than a microsecond. Wall time, but a lifetime is 0.1 s to 2 s. */
static double now_ms(void)
{
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	return 1e3 * (double)ts.tv_sec + 1e-6 * (double)ts.tv_nsec;
}

#define OPEN_MAX 64

typedef struct {
	int    frames;
	double first_at, second_at; /* on_overlay's first and second call */
} Lifetime;

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	Lifetime *l = ud;
	(void)frame;
	if (l->frames == 0)
		l->first_at = now_ms();
	else if (l->frames == 1)
		l->second_at = now_ms();
	/* The frame after the first is what shows the first one is out. */
	if (++l->frames >= 2) {
		fnl_quit(app);
		return;
	}
	fnl_invalidate(app);
}

static int cmp(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static double median(double *v, int n)
{
	qsort(v, (size_t)n, sizeof *v, cmp);
	return v[n / 2];
}

int open_main(int argc, char **argv)
{
	if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		puts("fenestal-dev open [<lifetimes>]\n\n"
			 "  <lifetimes>  windows opened and closed in one process, "
			 "1 to 64 (default 5)");
		return EXIT_SUCCESS;
	}
	int n = argc > 1 ? atoi(argv[1]) : 5;
	if (n < 1 || n > OPEN_MAX)
		n = 5;

	double init[OPEN_MAX], frame[OPEN_MAX], total[OPEN_MAX], shut[OPEN_MAX];
	int done = 0;
	for (; done < n; done++) {
		FnlInitDesc d;
		memset(&d, 0, sizeof d);
		d.size           = sizeof d;
		d.header_version = FNL_VERSION;
		d.title          = "Fenestal open";
		d.width_pt       = 1280;
		d.height_pt      = 720;
		d.renderers      = FNL_RENDERERS_ANY;

		Lifetime l;
		memset(&l, 0, sizeof l);
		FnlApp *app = NULL;
		const double t0 = now_ms();
		if (fnl_init(&d, &app) < 0) {
			fprintf(stderr, "open: fnl_init failed\n");
			return EXIT_FAILURE;
		}
		const double t1 = now_ms();
		FnlCallbacks cb;
		memset(&cb, 0, sizeof cb);
		cb.size       = sizeof cb;
		cb.ud         = &l;
		cb.on_overlay = on_overlay;
		const FnlResult rc = fnl_run(app, &cb);
		const double t2 = now_ms();
		fnl_shutdown(app);
		const double t3 = now_ms();
		if (rc < 0 || l.frames < 2) {
			fprintf(stderr, "open: the run drew %d frames\n", l.frames);
			return EXIT_FAILURE;
		}
		init[done]  = t1 - t0;
		frame[done] = l.second_at - t1;
		total[done] = l.second_at - t0;
		shut[done]  = t3 - t2;
		printf("open: lifetime %d: init %.1f ms, first frame %.1f ms "
			   "later (%.1f from the call), shutdown %.1f ms\n",
			   done + 1, init[done], frame[done], total[done], shut[done]);
		fflush(stdout);
	}
	/* The cold open is the program's own; the rest are one measure. */
	printf("open: cold %.1f ms (init %.1f + frame %.1f)", total[0], init[0],
		   frame[0]);
	if (n > 1) {
		const double ti = median(init + 1, n - 1);
		const double tf = median(frame + 1, n - 1);
		const double tt = median(total + 1, n - 1);
		const double ts = median(shut + 1, n - 1);
		printf("; warm median %.1f ms (init %.1f + frame %.1f), shutdown "
			   "%.1f ms", tt, ti, tf, ts);
	}
	putchar('\n');
	return EXIT_SUCCESS;
}

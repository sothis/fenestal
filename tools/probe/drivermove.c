/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * A window moved to the GPU's other kernel driver and back, with no hand
 * at the machine: FNL_GPU_DRIVER's list entry, "<GPU>, on <driver>",
 * asked for through fnl_request_gpu() three seconds in, and again once the
 * window is drawn on the new driver, then the program quits. Each step
 * prints the list, the GPU drawn on, how long the move took, the frames
 * drawn meanwhile and the longest the window went without one (DESIGN
 * *Another kernel driver, with the window open*, *Drawn across the move*).
 *
 *   cc -std=gnu11 -O2 -Iinclude tools/probe/drivermove.c \
 *      build/linux/release/libfenestal.a -lm -lpthread -ldl -o drivermove
 *   FNL_BACKEND=kms FNL_GPU_DRIVER=switch ./drivermove [moves] [gl]
 *                                                    [land-gl]
 *
 * The list has a move entry for each renderer; the one asked for is
 * Vulkan's, or with `land-gl` OpenGL's, which lands on NVIDIA's own OpenGL
 * or on Zink's on nouveau. With `gl`, before each move the window is put
 * on OpenGL on the NVIDIA GPU, by the OpenGL entry that names it, so every
 * move starts from OpenGL, and OpenGL's entries are seen listed again
 * after each.
 *
 * Exit status 0 when every move landed on another driver's GPU, 1 when one
 * did not or the window stopped drawing for MOVE_LIMIT.
 */
#include <fenestal/fenestal.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTLE      3.0     /* seconds drawn before each move */
#define MOVE_LIMIT  30.0    /* a move not done in this is a failure */

typedef struct {
	int    moves, done, frames, gl_switches;
	bool   printed, moving, failed, gl, to_gl;
	FnlRenderer land;
	double asked_at, settled_at, last_frame, gap;
	char   before[256], target[256];
} Probe;

static void print_list(FnlApp *app, double t)
{
	const int now = fnl_gpu_current(app);
	printf("%7.3f  drawn with %s on \"%s\", %d entries:\n", t,
		   fnl_renderer_name(app), fnl_gpu_name(app), fnl_gpu_count(app));
	for (int i = 0; i < fnl_gpu_count(app); i++)
		printf("           %c %d %s\n", i == now ? '*' : ' ', i,
			   fnl_gpu_name_at(app, i));
	fflush(stdout);
}

/* The OpenGL entry that names the NVIDIA GPU, or -1. */
static int gl_entry(FnlApp *app)
{
	for (int i = 0; i < fnl_gpu_count(app); i++)
		if (fnl_gpu_renderer_at(app, i) == FNL_RENDERER_OPENGL &&
			strstr(fnl_gpu_name_at(app, i), "NVIDIA") &&
			!strstr(fnl_gpu_name_at(app, i), ", on "))
			return i;
	return -1;
}

static bool on_gl_nvidia(FnlApp *app)
{
	return strcmp(fnl_renderer_name(app), "OpenGL") == 0 &&
		   strstr(fnl_gpu_name(app), "NVIDIA") != NULL;
}

static int move_entry(FnlApp *app, FnlRenderer land)
{
	for (int i = 0; i < fnl_gpu_count(app); i++)
		if (fnl_gpu_renderer_at(app, i) == land &&
				strstr(fnl_gpu_name_at(app, i), ", on "))
			return i;
	return -1;
}

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	Probe *p = ud;
	const double t = frame->time;
	if (p->last_frame > 0.0 && t - p->last_frame > p->gap)
		p->gap = t - p->last_frame;
	p->last_frame = t;
	fnl_invalidate(app);

	FnlEvent ev;
	while (fnl_poll_event(app, &ev))
		if (ev.type == FNL_EVENT_GPU)
			print_list(app, t);
	if (!p->printed) {
		p->printed = true;
		p->settled_at = t;
		print_list(app, t);
	}

	if (p->to_gl) {
		if (on_gl_nvidia(app)) {
			printf("%7.3f  on OpenGL in %.2f s: \"%s\"\n", t,
				   t - p->asked_at, fnl_gpu_name(app));
			fflush(stdout);
			p->to_gl = false;
			p->gl_switches++;
			p->settled_at = t;
		} else if (t - p->asked_at > MOVE_LIMIT) {
			printf("%7.3f  not on OpenGL on the NVIDIA GPU after %.0f s\n",
				   t, MOVE_LIMIT);
			p->failed = true;
			fnl_quit(app);
		}
		return;
	}
	if (p->moving) {
		/* Landed: a frame drawn on the GPU the entry named, under a name
		 * other than before -- Zink's holds it within its own. The GPU
		 * drawn on while the move is made has a name of its own, and is
		 * no landing. */
		const char *now = fnl_gpu_name(app);
		p->frames++;
		if (strstr(now, p->target) && strcmp(now, p->before) != 0) {
			printf("%7.3f  move %d done in %.2f s, %d frames meanwhile, "
				   "longest without one %.2f s: \"%s\" -> \"%s\"\n", t,
				   p->done + 1, t - p->asked_at, p->frames, p->gap,
				   p->before, now);
			fflush(stdout);
			p->moving = false;
			p->done++;
			p->settled_at = t;
		} else if (t - p->asked_at > MOVE_LIMIT) {
			printf("%7.3f  move %d: still on \"%s\" after %.0f s, %d "
				   "frames drawn meanwhile, the longest without one %.2f s\n",
				   t, p->done + 1, p->before, MOVE_LIMIT, p->frames, p->gap);
			p->failed = true;
			fnl_quit(app);
		}
		return;
	}
	if (t - p->settled_at < SETTLE)
		return;
	if (p->done == p->moves) {
		fnl_quit(app);
		return;
	}
	/* OpenGL first, once its entries are listed again after a move. */
	if (p->gl && !on_gl_nvidia(app)) {
		const int k = gl_entry(app);
		if (k < 0) {
			if (t - p->settled_at > MOVE_LIMIT) {
				printf("%7.3f  no OpenGL entry for the NVIDIA GPU after "
					   "%.0f s\n", t, MOVE_LIMIT);
				p->failed = true;
				fnl_quit(app);
			}
			return;
		}
		printf("%7.3f  asking for %d: %s (OpenGL)\n", t, k,
			   fnl_gpu_name_at(app, k));
		fflush(stdout);
		p->asked_at = t;
		p->to_gl = true;
		fnl_request_gpu(app, k);
		return;
	}
	const int i = move_entry(app, p->land);
	if (i < 0) {
		printf("%7.3f  no \"<GPU>, on <driver>\" entry: FNL_GPU_DRIVER "
			   "unset, or not the KMS backend\n", t);
		p->failed = true;
		fnl_quit(app);
		return;
	}
	snprintf(p->before, sizeof p->before, "%s", fnl_gpu_name(app));
	const char *entry = fnl_gpu_name_at(app, i);
	/* An entry named by the GPU's address lands on the GPU named NVIDIA,
	 * as app_gpu_driver_owe() asks for it. */
	if (strncmp(entry, "NVIDIA GPU ", 11) == 0)
		snprintf(p->target, sizeof p->target, "NVIDIA");
	else
		snprintf(p->target, sizeof p->target, "%.*s",
				 (int)(strstr(entry, ", on ") - entry), entry);
	printf("%7.3f  asking for %d: %s\n", t, i, fnl_gpu_name_at(app, i));
	fflush(stdout);
	p->asked_at = t;
	p->gap = 0.0;
	p->frames = 0;
	p->moving = true;
	fnl_request_gpu(app, i);
}

int main(int argc, char **argv)
{
	/* A line at a time into a pipe as well, so that these lines and the
	 * library's, on stderr, read in the order they were said. */
	setvbuf(stdout, NULL, _IOLBF, 0);
	Probe p;
	memset(&p, 0, sizeof p);
	p.moves = argc > 1 ? atoi(argv[1]) : 2;
	p.land = FNL_RENDERER_VULKAN;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "gl") == 0)
			p.gl = true;
		else if (strcmp(argv[i], "land-gl") == 0)
			p.land = FNL_RENDERER_OPENGL;
	}

	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title = "drivermove";
	desc.renderers = FNL_RENDERERS_ANY;
	FnlApp *app;
	if (fnl_init(&desc, &app) < 0) {
		fprintf(stderr, "fnl_init: %s\n", fnl_last_error());
		return 1;
	}
	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size = sizeof cb;
	cb.ud = &p;
	cb.on_overlay = on_overlay;
	const FnlResult rc = fnl_run(app, &cb);
	fnl_shutdown(app);
	printf("%d moves of %d, %d onto OpenGL\n", p.done, p.moves,
		   p.gl_switches);
	return rc < 0 || p.failed || p.done < p.moves;
}

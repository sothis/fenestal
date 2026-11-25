/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * A loop of the caller's own: fnl_start(), fnl_pump() inside this program's
 * while, fnl_stop(). What fnl_run() does, done here -- so a program that
 * already pumps something (a toolkit's loop, an engine's, a socket, a timer)
 * keeps its loop and still has the window.
 *
 * The pump's block is bounded, which is the whole point of doing it this way:
 * this program's own five seconds run when they are due, with nothing
 * happening in the window at all.
 *
 * Shows: fnl_start fnl_pump fnl_stop fnl_init fnl_shutdown
 *        fnl_content_rect_pt fnl_overlay_text fnl_error_copy fnl_last_error
 */

#include <fenestal/fenestal.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
	int turns; /* turns of the caller's loop */
	double started;
} State;

/* A clock that does not go backwards, which is what the loop below measures
 * against. Windows has no clock_gettime, and no library a consumer of this
 * one links for it -- core/thread.h says why the library carries no pthread
 * either -- so its own high-resolution counter is used there. */
static double now(void)
{
#ifdef _WIN32
	LARGE_INTEGER freq, count;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&count);
	return (double)count.QuadPart / (double)freq.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	State *s = ud;
	(void)frame;
	const FnlColor ink = { 0.95f, 0.95f, 0.90f, 1.0f };
	char line[128];
	int cx, cy, cw, ch;

	fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);
	snprintf(line, sizeof line, "the caller's loop: %d turns, %.1f s", s->turns,
			 now() - s->started);
	fnl_overlay_text(app, cx + 16, cy + 16, FNL_TEXT_BODY, ink, line);
}

int main(void)
{
	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size           = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title          = "embed";
	/* No scene of its own, so any renderer the build has will do. */
	desc.renderers = FNL_RENDERERS_ANY;

	FnlApp *app;
	if (fnl_init(&desc, &app) < 0) {
		fprintf(stderr, "fnl_init: %s\n", fnl_last_error());
		return 1;
	}

	State s = { 0, now() };
	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size       = sizeof cb;
	cb.ud         = &s;
	cb.on_overlay = on_overlay;

	if (fnl_start(app, &cb) < 0) {
		fprintf(stderr, "fnl_start: %s\n", fnl_last_error());
		fnl_shutdown(app);
		return 1;
	}

	/* This program's own loop. The pump is asked back every tenth of a
	 * second so the five seconds are honoured with nothing happening in the
	 * window; FNL_SKIPPED is the window closing -- the close button, or a
	 * fnl_quit() from anywhere. A negative result is the connection's, and
	 * the loop's end says which it was. */
	FnlResult prc;
	while ((prc = fnl_pump(app, 100)) == FNL_OK && now() - s.started < 5.0)
		s.turns++;

	/* Copied before fnl_stop() and fnl_shutdown(): fnl_last_error() holds
	 * its text only until the next call on the thread. */
	char why[256] = "";
	if (prc < 0)
		fnl_error_copy(why, sizeof why);
	fnl_stop(app);
	fnl_shutdown(app);
	printf("%d turns of the caller's loop\n", s.turns);
	if (prc < 0) {
		fprintf(stderr, "fnl_pump: %s: %s\n", fnl_result_str(prc), why);
		return 1;
	}
	return 0;
}

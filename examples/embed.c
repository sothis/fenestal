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
 *        fnl_content_rect_pt fnl_overlay_text fnl_text_line fnl_last_error
 */

#include <fenestal/fenestal.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
	int    turns;          /* turns of the caller's loop */
	double started;
} State;

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	State *s = ud;
	(void)frame;
	const FnlColor ink = { 0.95f, 0.95f, 0.90f, 1.0f };
	char line[128];
	int cx, cy, cw, ch;

	fnl_content_rect_pt(app, &cx, &cy, &cw, &ch);
	snprintf(line, sizeof line, "the caller's loop: %d turns, %.1f s",
			 s->turns, now() - s->started);
	fnl_overlay_text(app, cx + 16, cy + 16, FNL_TEXT_BODY, ink, line);
}

int main(void)
{
	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title = "embed";
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
	cb.size = sizeof cb;
	cb.ud = &s;
	cb.on_overlay = on_overlay;

	if (fnl_start(app, &cb) < 0) {
		fprintf(stderr, "fnl_start: %s\n", fnl_last_error());
		fnl_shutdown(app);
		return 1;
	}

	/* This program's own loop. The pump is asked back every tenth of a
	 * second so the five seconds are honoured with nothing happening in the
	 * window; FNL_SKIPPED is the window closing -- the close button, or a
	 * fnl_quit() from anywhere. A negative result is the connection's. */
	while (fnl_pump(app, 100) == FNL_OK && now() - s.started < 5.0)
		s.turns++;

	fnl_stop(app);
	fnl_shutdown(app);
	printf("%d turns of the caller's loop\n", s.turns);
	return 0;
}

/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * Input as a queue: drained once a frame, keys by position, and the
 * library's own notifications in the same stream.
 *
 * Shows: fnl_poll_event fnl_key_down fnl_invalidate fnl_quit fnl_text_line
 *        fnl_set_fullscreen fnl_is_fullscreen fnl_overlay_flush
 *        fnl_overlay_text_in fnl_overlay_rect_blend fnl_output_name
 *        fnl_key_name fnl_key_from_name fnl_is_shown fnl_set_log_level
 *        fnl_window_size_pt fnl_post
 */

#include <fenestal/fenestal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A thread of the caller's own, which the library has no API for: pthreads
 * on POSIX, CreateThread on Windows. Neither is a library a consumer of this
 * one links -- core/thread.h says why the library carries no pthread -- so
 * the two are spelled out rather than reached through one. */
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct {
	double x, y; /* where the pointer was last seen, in points */
	int clicks;
	double scrolled; /* in notches: a wheel click is 1.0 */
	char output[64]; /* the monitor, since FNL_EVENT_OUTPUT said */
	char last[32];   /* the last key, by the name fnl_key_name() gives */
	FnlKey quit;     /* FNL_QUIT_KEY, by name, or Q */
	int ticks;       /* what the worker thread has posted */
} State;

/* A thread of the caller's, reaching the drawing's state through a post: the
 * count is written on the render thread, in tick(), and nothing here needs a
 * lock of its own. */
struct Ticker {
	FnlApp *app;
	State *s;
	volatile int run;
};

static void tick(void *ud)
{
	((State *)ud)->ticks++;
}

/* What the thread does, written once: only the entry point's signature is
 * each platform's own. */
static void ticker_run(struct Ticker *t)
{
	while (t->run) {
#ifdef _WIN32
		Sleep(500);
#else
		const struct timespec half = { 0, 500000000 };
		nanosleep(&half, NULL);
#endif
		fnl_post(t->app, tick, t->s);
	}
}

#ifdef _WIN32
static DWORD WINAPI ticker_thread(LPVOID ud)
{
	ticker_run(ud);
	return 0;
}
#else
static void *ticker_thread(void *ud)
{
	ticker_run(ud);
	return NULL;
}
#endif

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	State *s = ud;
	(void)frame;

	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
		switch (ev.type) {
		case FNL_EVENT_KEY:
			/* A position, not a character: W is forward on AZERTY too, and the
			 * name is the one a help line prints -- the same table the echo of
			 * events uses. */
			snprintf(s->last, sizeof s->last, "%s %s", fnl_key_name(ev.key.key),
					 ev.key.pressed ? "down" : "up");
			if (ev.key.pressed && !ev.key.repeat && ev.key.key == FNL_KEY_F)
				fnl_set_fullscreen(app, !fnl_is_fullscreen(app));
			/* The quit key read by name, which is what a config file holds. */
			if (ev.key.pressed && ev.key.key == s->quit)
				fnl_quit(app);
			fnl_invalidate(app);
			break;
		case FNL_EVENT_MOTION:
			s->x = ev.motion.x;
			s->y = ev.motion.y;
			fnl_invalidate(app); /* what we draw moved */
			break;
		case FNL_EVENT_BUTTON:
			/* The library's own count: 1 on a lone press, 2 on the second press
			 * of a double-click, without a clock here. A release carries 0, so
			 * the figure a press set stands until the next one. */
			if (ev.button.pressed && ev.button.button == FNL_BUTTON_LEFT &&
				ev.button.clicks > 0)
				s->clicks = ev.button.clicks;
			fnl_invalidate(app);
			break;
		case FNL_EVENT_SCROLL:
			s->scrolled += ev.scroll.dy;
			fnl_invalidate(app);
			break;
		case FNL_EVENT_RESIZE:
			/* Posted after the fact, once, when the area actually changed. */
			break;
		case FNL_EVENT_OUTPUT:
			/* Another monitor, or this one resized; the name is asked. */
			snprintf(s->output, sizeof s->output, "%s", fnl_output_name(app));
			fnl_invalidate(app);
			break;
		default: break; /* a newer library may add types */
		}
	}

	const FnlColor ink = { 0.9f, 0.9f, 0.9f, 1.0f };
	char line[128];
	snprintf(line, sizeof line,
			 "pointer %.0f,%.0f  clicks %d  scrolled "
			 "%.1f  %s  %s  %s",
			 s->x, s->y, s->clicks, s->scrolled,
			 fnl_key_down(app, FNL_KEY_LSHIFT) ? "shift" : "", s->last,
			 fnl_is_shown(app) ? "shown" : "parked");
	{
		char posted[32];
		snprintf(posted, sizeof posted, "  posted %d", s->ticks);
		strncat(line, posted, sizeof line - strlen(line) - 1);
	}
	fnl_overlay_text(app, 16, 48, FNL_TEXT_BODY, ink, line);
	if (s->output[0])
		fnl_overlay_text(app, 16, 48 + fnl_text_line(FNL_TEXT_BODY),
						 FNL_TEXT_BODY, ink, s->output);

	/* A line placed by its right edge instead of its left: the count of
	 * clicks, against the window's right margin, so figures of any width
	 * line up -- fnl_overlay_text() with a subtraction at every call site
	 * otherwise, and the subtraction is wrong the moment a figure changes. */
	{
		char count[32];
		int w_pt, h_pt;
		snprintf(count, sizeof count, "%d clicks", s->clicks);
		fnl_window_size_pt(app, &w_pt, &h_pt);
		fnl_overlay_text_in(app, 16, 48 + 2 * fnl_text_line(FNL_TEXT_BODY),
							w_pt - 32, FNL_TEXT_ALIGN_RIGHT, FNL_TEXT_BODY, ink,
							count);
		/* A wash under the line, so a colour with alpha is a wash and not
		 * the hole an opaque write makes: the library blends this one over
		 * what is under it, as fnl_overlay_rect() cannot. Flushed, so the
		 * line's glyphs land on top of it. */
		const FnlColor wash = { 1.0f, 1.0f, 1.0f, 0.15f };
		fnl_overlay_rect_blend(app, 16, 48 + 2 * fnl_text_line(FNL_TEXT_BODY),
							   w_pt - 32, fnl_text_line(FNL_TEXT_BODY), wash);
		fnl_overlay_flush(app);
	}

	/* A marker over the text, so drawn after a flush: without it the
	 * rectangle would go under, every filled rectangle being a background. */
	fnl_overlay_flush(app);
	const FnlColor mark = { 1.0f, 0.4f, 0.2f, 1.0f };
	fnl_overlay_rect(app, (int)s->x - 2, (int)s->y - 2, 4, 4, mark);
}

int main(void)
{
	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size           = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title          = "events";
	/* No scene of its own, so any renderer the build has will do. */
	desc.renderers = FNL_RENDERERS_ANY;

	FnlApp *app;
	if (fnl_init(&desc, &app) < 0)
		return 1;
	/* Its own lines are the output, so the library's INFO ones are dropped. */
	fnl_set_log_level(FNL_LOG_WARN);
	const char *want = getenv("FNL_QUIT_KEY");
	State s          = {
        0, 0, 0, 0.0, "", "", fnl_key_from_name(want && want[0] ? want : "q"), 0
	};
	struct Ticker t = { app, &s, 1 };
#ifdef _WIN32
	HANDLE th         = CreateThread(NULL, 0, ticker_thread, &t, 0, NULL);
	const int started = th != NULL;
#else
	pthread_t th;
	const int started = pthread_create(&th, NULL, ticker_thread, &t) == 0;
#endif

	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size            = sizeof cb;
	cb.ud              = &s;
	cb.on_overlay      = on_overlay;
	const FnlResult rc = fnl_run(app, &cb);
	/* The window is gone, so nothing may post to it: stop the thread before
	 * the App it names is shut down. */
	t.run = 0;
	if (started) {
#ifdef _WIN32
		WaitForSingleObject(th, INFINITE);
		CloseHandle(th);
#else
		pthread_join(th, NULL);
#endif
	}
	fnl_shutdown(app);
	return rc < 0;
}

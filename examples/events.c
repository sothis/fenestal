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
 *        fnl_output_name
 */

#include <fenestal/fenestal.h>

#include <stdio.h>
#include <string.h>

typedef struct {
	double x, y;            /* where the pointer was last seen, in points */
	int    clicks;
	double scrolled;        /* in notches: a wheel click is 1.0 */
	char   output[64];      /* the monitor, since FNL_EVENT_OUTPUT said */
} State;

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	State *s = ud;
	(void)frame;

	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
		switch (ev.type) {
		case FNL_EVENT_KEY:
			/* A position, not a character: W is forward on AZERTY too. */
			if (ev.key.pressed && !ev.key.repeat &&
				ev.key.key == FNL_KEY_F)
				fnl_set_fullscreen(app, !fnl_is_fullscreen(app));
			if (ev.key.pressed && ev.key.key == FNL_KEY_Q)
				fnl_quit(app);
			break;
		case FNL_EVENT_MOTION:
			s->x = ev.motion.x;
			s->y = ev.motion.y;
			fnl_invalidate(app);        /* what we draw moved */
			break;
		case FNL_EVENT_BUTTON:
			if (ev.button.pressed && ev.button.button == FNL_BUTTON_LEFT)
				s->clicks++;
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
			snprintf(s->output, sizeof s->output, "%s",
					 fnl_output_name(app));
			fnl_invalidate(app);
			break;
		default:
			break;                      /* a newer library may add types */
		}
	}

	const FnlColor ink = { 0.9f, 0.9f, 0.9f, 1.0f };
	char line[128];
	snprintf(line, sizeof line, "pointer %.0f,%.0f  clicks %d  scrolled "
			 "%.1f  %s", s->x, s->y, s->clicks, s->scrolled,
			 fnl_key_down(app, FNL_KEY_LSHIFT) ? "shift" : "");
	fnl_overlay_text(app, 16, 48, FNL_TEXT_BODY, ink, line);
	if (s->output[0])
		fnl_overlay_text(app, 16, 48 + fnl_text_line(FNL_TEXT_BODY),
						 FNL_TEXT_BODY, ink, s->output);

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
	desc.size = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title = "events";
	/* No scene of its own, so any renderer the build has will do. */
	desc.renderers = FNL_RENDERERS_ANY;

	FnlApp *app;
	if (fnl_init(&desc, &app) < 0)
		return 1;
	State s = { 0, 0, 0, 0.0, "" };
	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size = sizeof cb;
	cb.ud = &s;
	cb.on_overlay = on_overlay;
	const FnlResult rc = fnl_run(app, &cb);
	fnl_shutdown(app);
	return rc < 0;
}

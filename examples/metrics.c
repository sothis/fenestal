/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * The library's metrics panel, shown from the start and moved off F5, which
 * this program keeps for itself: F7 shows and hides the panel instead, and
 * F5 says on stderr whether it is up.
 *
 * Shows: fnl_set_metrics_panel fnl_metrics_panel fnl_set_metrics_key
 *        fnl_metrics_key
 */

#include <fenestal/fenestal.h>

#include <stdio.h>
#include <string.h>

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	(void)ud;
	(void)frame;
	FnlEvent ev;
	while (fnl_poll_event(app, &ev))
		if (ev.type == FNL_EVENT_KEY && ev.key.pressed && !ev.key.repeat &&
			ev.key.key == FNL_KEY_F5)
			fprintf(stderr, "the panel is %s; F7 %s it\n",
					fnl_metrics_panel(app) ? "up" : "down",
					fnl_metrics_panel(app) ? "hides" : "shows");
}

int main(void)
{
	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size           = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title          = "metrics";
	desc.renderers      = FNL_RENDERERS_ANY;

	FnlApp *app;
	FnlResult rc = fnl_init(&desc, &app);
	if (rc < 0) {
		fprintf(stderr, "%s: %s\n", fnl_result_str(rc), fnl_last_error());
		return 1;
	}

	/* Bound before the window runs, so no press reaches the old key. */
	fnl_set_metrics_key(app, FNL_KEY_F7);
	if (fnl_metrics_key(app) != FNL_KEY_F7)
		fprintf(stderr, "the panel stayed on another key\n");
	fnl_set_metrics_panel(app, true);

	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size       = sizeof cb;
	cb.on_overlay = on_overlay;
	rc = fnl_run(app, &cb);
	fnl_shutdown(app);
	return rc < 0;
}

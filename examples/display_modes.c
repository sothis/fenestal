/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * The display mode and the output: on KMS the window is a whole monitor in
 * one of its modes, and these choose both. Under a window server the mode
 * list is empty and Up and Down do nothing -- the server sizes the window
 * there -- and the outputs are its monitors.
 *
 * Up and Down step through the modes of the monitor the window is on; Tab
 * moves the window to the next monitor. The list is printed when it
 * changes, which is what FNL_EVENT_OUTPUT and FNL_EVENT_RESIZE say.
 *
 * Shows: fnl_mode_count fnl_mode_get fnl_mode_current fnl_request_mode
 *        fnl_output_count fnl_output_name_at fnl_request_output
 */

#include <fenestal/fenestal.h>

#include <stdio.h>
#include <string.h>

/* The mode one step along the list from the one in force: the list is
 * largest first, so Down is smaller. */
static void step_mode(FnlApp *app, int by)
{
	FnlMode now, m;
	if (!fnl_mode_current(app, &now))
		return;
	const int n = fnl_mode_count(app);
	for (int i = 0; i < n; i++) {
		if (!fnl_mode_get(app, i, &m) || m.w_px != now.w_px ||
			m.h_px != now.h_px || m.refresh_mhz != now.refresh_mhz)
			continue;
		if (i + by >= 0 && i + by < n && fnl_mode_get(app, i + by, &m))
			fnl_request_mode(app, &m);
		return;
	}
}

/* The output after the one the window is on, round to the first. */
static void next_output(FnlApp *app)
{
	const int n = fnl_output_count(app);
	char on[160];   /* the library's own bound on a name; a cut one never
					 * matches the list */
	snprintf(on, sizeof on, "%s", fnl_output_name(app));
	for (int i = 0; i < n; i++)
		if (strcmp(fnl_output_name_at(app, i), on) == 0) {
			fnl_request_output(app, fnl_output_name_at(app, (i + 1) % n));
			return;
		}
}

static void print_list(FnlApp *app)
{
	FnlMode now = { 0, 0, 0 }, m;
	fnl_mode_current(app, &now);
	printf("on %s, %d outputs:", fnl_output_name(app),
		   fnl_output_count(app));
	for (int i = 0; i < fnl_output_count(app); i++)
		printf(" %s", fnl_output_name_at(app, i));
	printf("\n");
	for (int i = 0; fnl_mode_get(app, i, &m); i++)
		printf("  %c %dx%d @ %u.%03u Hz\n",
			   m.w_px == now.w_px && m.h_px == now.h_px &&
					   m.refresh_mhz == now.refresh_mhz ? '*' : ' ',
			   m.w_px, m.h_px, m.refresh_mhz / 1000, m.refresh_mhz % 1000);
}

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	(void)ud; (void)frame;
	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
		if (ev.type == FNL_EVENT_OUTPUT || ev.type == FNL_EVENT_RESIZE)
			print_list(app);
		if (ev.type != FNL_EVENT_KEY || !ev.key.pressed || ev.key.repeat)
			continue;
		if (ev.key.key == FNL_KEY_UP)
			step_mode(app, -1);
		else if (ev.key.key == FNL_KEY_DOWN)
			step_mode(app, 1);
		else if (ev.key.key == FNL_KEY_TAB)
			next_output(app);
	}
}

int main(void)
{
	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title = "display modes";
	/* No scene of its own, so any renderer the build has will do. */
	desc.renderers = FNL_RENDERERS_ANY;

	FnlApp *app;
	if (fnl_init(&desc, &app) < 0)
		return 1;
	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size = sizeof cb;
	cb.on_overlay = on_overlay;
	const FnlResult rc = fnl_run(app, &cb);
	fnl_shutdown(app);
	return rc < 0;
}

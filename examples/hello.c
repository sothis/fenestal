/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * A window with a line of text in it, and every failure reported.
 *
 * Shows: fnl_version fnl_init fnl_run fnl_shutdown fnl_result_str
 *        fnl_last_error fnl_overlay_text fnl_overlay_rect fnl_text_width
 *        fnl_text_line fnl_window_size_pt fnl_font_notice fnl_font_licence
 */

#include <fenestal/fenestal.h>

#include <stdio.h>
#include <string.h>

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	(void)ud;
	(void)frame;
	const char *msg = "Hello from Fenestal";
	const FnlColor panel = { 0.10f, 0.10f, 0.12f, 0.85f };
	const FnlColor ink   = { 0.95f, 0.95f, 0.90f, 1.00f };

	/* Centred, in logical points: the same layout at every display scale.
	 * The rectangle is a background, as every filled rectangle must be. */
	int w_pt, h_pt;
	fnl_window_size_pt(app, &w_pt, &h_pt);
	const int tw = fnl_text_width(msg, FNL_TEXT_LARGE);
	const int th = fnl_text_line(FNL_TEXT_LARGE);
	const int x = (w_pt - tw) / 2, y = (h_pt - th) / 2;
	fnl_overlay_rect(app, x - 12, y - 8, tw + 24, th + 16, panel);
	fnl_overlay_text(app, x, y, FNL_TEXT_LARGE, ink, msg);
}

int main(int argc, char **argv)
{
	/* The baked font's licence travels with every binary that ships it. */
	if (argc > 1 && !strcmp(argv[1], "--licence")) {
		printf("%s\n\n%s\n", fnl_font_notice(), fnl_font_licence());
		return 0;
	}

	/* A shared library may be newer than the header this was built with;
	 * the major has to match -- and before 1.0 the minor as well -- and
	 * fnl_init() checks it too. */
	unsigned shift = FNL_VERSION_MAJOR == 0 ? 8 : 16;
	if ((fnl_version() >> shift) != ((uint32_t)FNL_VERSION >> shift)) {
		fprintf(stderr, "libfenestal is another release\n");
		return 1;
	}

	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size           = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title          = "hello";
	desc.width_pt       = 640;
	desc.height_pt      = 360;
	/* No scene of its own, so any renderer the build has will do. */
	desc.renderers      = FNL_RENDERERS_ANY;

	FnlApp *app;
	FnlResult rc = fnl_init(&desc, &app);
	if (rc < 0) {
		fprintf(stderr, "fnl_init: %s: %s\n", fnl_result_str(rc),
				fnl_last_error());
		return 1;
	}

	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size       = sizeof cb;
	cb.on_overlay = on_overlay;
	rc = fnl_run(app, &cb);
	if (rc < 0)
		fprintf(stderr, "fnl_run: %s: %s\n", fnl_result_str(rc),
				fnl_last_error());
	fnl_shutdown(app);
	return rc < 0;
}

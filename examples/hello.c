/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * A window with a line of text in it, and every failure reported.
 *
 * Shows: fnl_version fnl_init fnl_run fnl_shutdown fnl_result_str
 *        fnl_last_error fnl_error_copy fnl_overlay_text fnl_overlay_rect
 *        fnl_text_width fnl_text_wrap
 *        fnl_text_line fnl_window_size_pt fnl_font_notice fnl_font_licence
 */

#include <fenestal/fenestal.h>

#include <stdio.h>
#include <string.h>

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	(void)ud;
	(void)frame;
	const char *msg = "Hello from Fenestal, wrapped into the window it is "
					  "drawn in however narrow that is";
	const FnlColor panel = { 0.10f, 0.10f, 0.12f, 0.85f };
	const FnlColor ink   = { 0.95f, 0.95f, 0.90f, 1.00f };

	/* Wrapped, in logical points: the same layout at every display scale.
	 * The rectangle is a background, as every filled rectangle must be.
	 * fnl_text_wrap() puts '\n' between the lines and overlay text draws one
	 * line per call, so each line is drawn on its own. */
	int w_pt, h_pt;
	fnl_window_size_pt(app, &w_pt, &h_pt);
	const int margin = 24;
	char text[512];
	const int lines = fnl_text_wrap(text, sizeof text, msg,
									w_pt - 2 * margin, FNL_TEXT_LARGE);
	const int line_h = fnl_text_line(FNL_TEXT_LARGE);
	const int tw = w_pt - 2 * margin;
	const int th = line_h * (lines > 0 ? lines : 1);
	const int x = margin, y = (h_pt - th) / 2;
	fnl_overlay_rect(app, x - 12, y - 8, tw + 24, th + 16, panel);
	char *line = text;
	for (int i = 0; i < lines; i++) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		fnl_overlay_text(app, x, y + i * line_h, FNL_TEXT_LARGE, ink, line);
		if (!nl)
			break;
		line = nl + 1;
	}
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
		/* Copied: fnl_last_error()'s text is valid only until the next
		 * Fenestal call on this thread, and a log line goes somewhere
		 * that may outlive it. */
		char why[256];
		fnl_error_copy(why, sizeof why);
		fprintf(stderr, "fnl_init: %s: %s\n", fnl_result_str(rc), why);
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

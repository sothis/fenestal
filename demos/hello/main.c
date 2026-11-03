/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * The smallest Fenestal program: a window and a black scene.
 *
 * No callbacks at all. The library clears the scene to opaque black on every
 * renderer and draws its own titlebar over it; Escape quits.
 */

#include <fenestal/fenestal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size           = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title          = "fenestal-demo-hello";
	desc.width_pt       = 960;
	desc.height_pt      = 540;
	/* No scene of its own, so any renderer the build has will do. */
	desc.renderers      = FNL_RENDERERS_ANY;

	FnlApp *app;
	FnlResult rc = fnl_init(&desc, &app);
	if (rc < 0) {
		fprintf(stderr, "fnl_init: %s: %s\n", fnl_result_str(rc),
				fnl_last_error());
		return EXIT_FAILURE;
	}

	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size = sizeof cb;
	rc = fnl_run(app, &cb);
	if (rc < 0)
		fprintf(stderr, "fnl_run: %s: %s\n", fnl_result_str(rc),
				fnl_last_error());
	fnl_shutdown(app);
	return rc < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

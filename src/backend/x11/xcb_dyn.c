/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The pointers xcb_dyn.h declares, and the load that fills them.
 */

#include "core/app.h"

#include "backend/x11/xcb_dyn.h"
#include "core/dynlib.h"

#define X11_XCB_DEFINE(f) __typeof__(fnl_##f) fnl_##f = NULL;
X11_XCB_FUNCS(X11_XCB_DEFINE)
#undef X11_XCB_DEFINE

const char *x11_xcb_load(void)
{
	/* 0 untried, 1 loaded, -1 refused with `why`. Only fnl_init()'s thread
	 * comes here, in x11_open(). */
	static int state;
	static char why[256];
	if (state)
		return state > 0 ? NULL : why;

	static const char *const NAMES[] = { "libxcb.so.1", NULL };
	char err[192];
	void *lib = fnl_lib_open(NAMES, err, sizeof err);
	if (!lib) {
		snprintf(why, sizeof why, "no libxcb.so.1: %s", err);
		state = -1;
		return why;
	}

	/* Every row, or the backend is refused: a libxcb older than one of
	 * them is turned down here, by name, rather than at the first call
	 * through a NULL. */
	FnlLibFn sym;
#define X11_XCB_RESOLVE(f)                                              \
	sym = fnl_lib_sym(lib, #f);                                         \
	if (!sym) {                                                         \
		snprintf(why, sizeof why, "no libxcb.so.1: it has no %s", #f);  \
		state = -1;                                                     \
		return why;                                                     \
	}                                                                   \
	fnl_##f = (__typeof__(fnl_##f))sym;
	X11_XCB_FUNCS(X11_XCB_RESOLVE)
#undef X11_XCB_RESOLVE

	state = 1;
	return NULL;
}

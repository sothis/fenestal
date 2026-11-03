/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * libwayland-client's pointers, and the loader that fills them
 * (wl_dyn.h). One soname: libwayland-client has kept .so.0 since 1.0, and
 * wl_proxy_marshal_flags, the newest entry here, is 1.20's -- a library
 * older than that is refused by name rather than half used.
 */

#include "core/app.h"

#include "backend/wayland/wl_dyn.h"

#include "core/dynlib.h"

#define WL_DYN_DEFINE(f) __typeof__(fnl_##f) fnl_##f;
WL_DYN_FUNCS(WL_DYN_DEFINE)
#undef WL_DYN_DEFINE

#define WL_LIB_NAME "libwayland-client.so.0"

/* [main] fnl_init()'s thread, and open() is its only caller, so a plain
 * static remembers the answer: no two opens run at once. */
const char *wl_dyn_load(void)
{
	static int  state;      /* 0 untried, 1 resolved, -1 refused */
	static char reason[256];
	if (state)
		return state > 0 ? NULL : reason;

	static const char *const NAMES[] = { WL_LIB_NAME, NULL };
	char why[200];
	void *lib = fnl_lib_open(NAMES, why, sizeof why);
	if (!lib) {
		snprintf(reason, sizeof reason, "no " WL_LIB_NAME ": %s", why);
		state = -1;
		return reason;
	}

	/* The first entry missing is the one named; the pointers already
	 * filled are never called, since the answer stays no. */
	state = 1;
#define WL_DYN_RESOLVE(f)                                              \
	if (state > 0 &&                                                   \
		!(fnl_##f = (__typeof__(fnl_##f))fnl_lib_sym(lib, #f))) {      \
		snprintf(reason, sizeof reason, WL_LIB_NAME " has no %s", #f); \
		state = -1;                                                    \
	}
	WL_DYN_FUNCS(WL_DYN_RESOLVE)
#undef WL_DYN_RESOLVE
	return state > 0 ? NULL : reason;
}

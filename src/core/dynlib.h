/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * A runtime opened at run time rather than linked: dlopen on POSIX,
 * LoadLibraryEx on Windows. What the system does not always have -- the
 * Vulkan loader, EGL, libwayland-client, libxcb -- is reached this way, so
 * that a machine missing one still loads the program and steps past the
 * backend or the renderer that wanted it (DESIGN *One build per operating
 * system*, *The link line*).
 *
 * **A library once opened is never closed**, with one exception. Mesa's
 * drivers, which every one of these loads beneath it, have left
 * thread-locals and exit handlers that ran after the library would have
 * been unloaded, and the cost of keeping one mapped is its pages. Each
 * loader opens once per process and remembers the answer, failure
 * included. The exception is EGL where a GPU may be moved to another
 * kernel driver (src/renderer/gl/egl_dyn.c), which is closed because
 * glvnd kept open holds that driver; measured to close cleanly there.
 */

#ifndef FNL_DYNLIB_H
#define FNL_DYNLIB_H

#include "core/compiler.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* What a symbol is handed back as. Any function pointer converts to and
 * from this one without -Wcast-function-type, which exempts it. */
typedef void (*FnlLibFn)(void);

/* Each name in `names` in turn, up to a NULL; the first that opens is the
 * handle. NULL when none does, with the loader's own reason for the first
 * one in `why` -- "libvulkan.so.1: cannot open shared object file" -- for
 * the line that says what was passed over: the first is the runtime's
 * proper name, and the rest are fallbacks a reader would not look for.
 *
 * Windows searches the program's directory and System32, never the current
 * directory, where a DLL planted beside a document would be loaded in the
 * runtime's place. */
static inline void *fnl_lib_open(const char *const *names, char *why,
								 size_t why_n)
{
	if (why_n)
		why[0] = '\0';
	for (size_t i = 0; names[i]; i++) {
#if defined(_WIN32)
		HMODULE h = LoadLibraryExA(names[i], NULL,
								   LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		if (h)
			return (void *)h;
		if (why_n && !why[0])
			snprintf(why, why_n, "%s: not found (error %lu)", names[i],
					 (unsigned long)GetLastError());
#else
		void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
		if (h)
			return h;
		const char *e = dlerror();
		if (why_n && !why[0])
			snprintf(why, why_n, "%s", e ? e : names[i]);
#endif
	}
	return NULL;
}

/* Closed: egl_dyn.c's alone (above). */
static inline void fnl_lib_close(void *lib)
{
#if defined(_WIN32)
	FreeLibrary((HMODULE)lib);
#else
	dlclose(lib);
#endif
}

/* NULL where the library has no such symbol. */
static inline FnlLibFn fnl_lib_sym(void *lib, const char *name)
{
#if defined(_WIN32)
	return (FnlLibFn)GetProcAddress((HMODULE)lib, name);
#else
	/* POSIX hands back an object pointer, which C does not convert to a
	 * function pointer; dlsym's own specification makes the two the same
	 * size and representation, and the copy says so without a cast. */
	_Static_assert(sizeof(void *) == sizeof(FnlLibFn),
				   "dlsym's result must fit a function pointer");
	void *p = dlsym(lib, name);
	FnlLibFn f;
	memcpy(&f, &p, sizeof f);
	return f;
#endif
}

#endif /* FNL_DYNLIB_H */

/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The GL entry points, loaded through the bridge into the window's table
 * (gl_funcs.h says why a table, what its three lists are, and why it is the
 * window's), and the thread's binding to that window's table. A function the
 * driver does not have fails the load, by name, where the context's version
 * says it is core: a missing one is a driver that does not offer what it
 * claimed.
 */

#include "core/app.h"

#include "renderer/gl/gl_internal.h"

/* The window this thread last bound, and what gl_internal.h's `gl` and
 * gl_profile41 name: a thread builds or draws for one window at a time, and a
 * table is the window's (gl_funcs.h). */
static FNL_THREAD_LOCAL struct RendererState *gl_thread_win;

void gl_bind(App *a)
{
	gl_thread_win = a && a->renderer_state ? GLS(a) : NULL;
}

struct RendererState *gl_bound_state(void)
{
	/* Every GL call is made on a thread that built, entered, destroyed or
	 * freed an image of a window -- gl_load(), create_device_objects(),
	 * destroy_device_objects(), thread_enter() and gl_image_free() bind
	 * there -- so an unbound one is this renderer's fault, not the
	 * caller's. */
	FNL_ASSERT(gl_thread_win);
	return gl_thread_win;
}

GlFuncs *gl_bound_funcs(void)
{
	return &gl_bound_state()->funcs;
}

bool gl_bound_profile41(void)
{
	return gl_thread_win && gl_thread_win->funcs_41;
}

enum { CORE, GL45, OPT };

static const struct {
	const char   *name;
	unsigned char list;
} ENTRIES[] = {
#define FNL_GL_CORE_ENTRY(type, name) { "gl" #name, CORE },
#define FNL_GL_45_ENTRY(type, name)   { "gl" #name, GL45 },
#define FNL_GL_OPT_ENTRY(type, name)  { "gl" #name, OPT },
	FNL_GL_CORE(FNL_GL_CORE_ENTRY)
	FNL_GL_45(FNL_GL_45_ENTRY)
	FNL_GL_OPT(FNL_GL_OPT_ENTRY)
#undef FNL_GL_CORE_ENTRY
#undef FNL_GL_45_ENTRY
#undef FNL_GL_OPT_ENTRY
};

/* Whether the context lists `ext`, asked one name at a time as a core
 * context must. */
static bool has_ext(const GlFuncs *t, const char *ext)
{
	GLint n = 0;
	t->GetIntegerv(GL_NUM_EXTENSIONS, &n);
	for (GLint i = 0; i < n; i++) {
		const char *e = (const char *)t->GetStringi(GL_EXTENSIONS, (GLuint)i);
		if (e && !strcmp(e, ext))
			return true;
	}
	return false;
}

/* An optional entry on a 4.1 context: by its name where the extension that
 * adds it unsuffixed is listed, by `arb` where that one is, NULL where
 * neither -- asked by extension and not by the pointer, since Mesa's EGL
 * hands out a stub for any name. */
static FnlGlProc optional(const GlFuncs *t, const char *name,
						  const char *khr, const char *arb, const char *arb_ext)
{
	if (has_ext(t, khr))
		return gl_bridge_proc(name);
	if (arb && has_ext(t, arb_ext))
		return gl_bridge_proc(arb);
	return NULL;
}

FnlResult gl_load(App *a)
{
	/* Gathered into an array and copied into the table whole: memcpy and
	 * not a store through a cast, which the table's own member types would
	 * not alias. A load that fails part-way leaves the last good table in
	 * place for whatever still holds a context of the driver it came
	 * from. */
	enum { N = sizeof ENTRIES / sizeof ENTRIES[0] };
	FnlGlProc procs[N];
	GlFuncs next;
	_Static_assert(sizeof next == sizeof procs,
				   "GlFuncs holds one function pointer per name");
	for (size_t i = 0; i < N; i++) {
		procs[i] = ENTRIES[i].list == CORE ? gl_bridge_proc(ENTRIES[i].name)
										   : NULL;
		if (ENTRIES[i].list == CORE && !procs[i])
			return fnl_fail(FNL_ERR_NO_DEVICE,
							"the OpenGL driver has no %s", ENTRIES[i].name);
	}
	memcpy(&next, procs, sizeof next);

	/* 4.5 is asked for at the context's creation, and 4.1 where that was
	 * refused (egl.c); a driver may also hand back a lower one rather than
	 * refuse, and GL_MAJOR_VERSION is the answer. */
	GLint major = 0, minor = 0;
	next.GetIntegerv(GL_MAJOR_VERSION, &major);
	next.GetIntegerv(GL_MINOR_VERSION, &minor);
	if (major < 4 || (major == 4 && minor < 1))
		return fnl_fail(FNL_ERR_NO_DEVICE,
						"OpenGL %d.%d; this renderer needs 4.1 core",
						(int)major, (int)minor);
	/* The site draws a 4.5 context through the 4.1 profile, which is how
	 * the profile is driven where every device has 4.5. */
	const bool v41 = (major == 4 && minor < 5) || FNL_FAULT("gl-41");

	if (!v41) {
		for (size_t i = 0; i < N; i++) {
			if (ENTRIES[i].list == CORE)
				continue;
			procs[i] = gl_bridge_proc(ENTRIES[i].name);
			if (!procs[i])
				return fnl_fail(FNL_ERR_NO_DEVICE,
								"the OpenGL driver has no %s",
								ENTRIES[i].name);
		}
		memcpy(&next, procs, sizeof next);
	} else {
		gl_compat_install(&next, GLS(a));
		next.GetGraphicsResetStatus = (PFNGLGETGRAPHICSRESETSTATUSPROC)
			optional(&next, "glGetGraphicsResetStatus", "GL_KHR_robustness",
					 "glGetGraphicsResetStatusARB", "GL_ARB_robustness");
		next.DebugMessageCallback = (PFNGLDEBUGMESSAGECALLBACKPROC)
			optional(&next, "glDebugMessageCallback", "GL_KHR_debug", NULL,
					 NULL);
	}
	GLS(a)->funcs    = next;
	GLS(a)->funcs_41 = v41;
	gl_bind(a);
	return FNL_OK;
}

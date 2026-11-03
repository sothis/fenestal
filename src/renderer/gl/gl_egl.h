/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What egl.c asks of the window server's few lines of EGL, egl_<backend>.c:
 * the native display, the visual a configuration must match, and the native
 * window. With the loader's egl_dyn.h, the one header in this directory
 * beside the bridges that may name EGL; nothing outside them includes it.
 */

#ifndef FNL_GL_EGL_H
#define FNL_GL_EGL_H

/* EGL's headers, and every EGL call made through the loader's pointers
 * rather than the linked library. */
#include "renderer/gl/egl_dyn.h"

/* The platform eglGetPlatformDisplay is asked for, the native display and
 * its attributes, EGL_NONE-terminated. */
typedef struct {
	EGLenum   platform;
	void     *display;
	EGLAttrib attrs[5];
} EglNative;

/* What one window server's egl_<backend>.c answers: a table, because a
 * Linux build has one per backend and learns which at open (DESIGN *One
 * build per operating system*). egl.c finds it by FnlBackendVtbl::id. */
typedef struct EglBridge {
	BackendId backend;
	/* No window: the context is current on no surface, and the frame is
	 * read back and handed to the backend (KMS, egl_kms.c). */
	bool surfaceless;
	/* False where the window server has no display to offer, with the
	 * reason recorded. */
	FNL_MUST_USE bool (*display)(App *a, EglNative *out);
	/* The native visual a configuration must have, where the window was
	 * made with one before any configuration existed; 0 for any. */
	EGLint (*visual)(const App *a);
	/* The alpha bits a configuration must have: 0 where the window's visual
	 * decides, 8 where the buffer itself carries the window's
	 * transparency. */
	EGLint (*alpha)(const App *a);
	/* What paces a vsynced frame where the swap is never asked to wait:
	 * the window server's frame callback, or a compositor that shows what
	 * is presented at its own refresh with the loop spacing the frames.
	 * NULL where the swap is the throttle, under an interval of 1. A name
	 * for the status line, "frame callback (vsync)". */
	const char *(*pacer)(const App *a);
	/* What eglCreatePlatformWindowSurface takes as the native window, and
	 * the end of whatever was made for it. NULL from the first is a
	 * failure, with the reason recorded. */
	void *(*window)(App *a);
	void  (*window_close)(App *a);
	/* The native window's size in pixels, at every rebuild; nothing where
	 * the window server sizes the window itself. */
	void  (*window_resize)(App *a, int w, int h);
	/* A surfaceless bridge's way onto the output with no readback, where
	 * it has one (KMS): `scan_begin` blits the window's image -- `fbo`,
	 * `w` by `h` -- into the buffer the next flip shows, true where it
	 * did or where this frame shows nothing; false sends the frame the
	 * copy path's way. `scan_show` flips it on, false where there was
	 * nothing to flip; `scan_close` lets go of every buffer it holds, with
	 * the context still current, and `scan_forget` of what is not the
	 * context's, with none current. NULL where there is none. */
	bool  (*scan_begin)(App *a, unsigned fbo, int w, int h);
	bool  (*scan_show)(App *a);
	void  (*scan_close)(App *a);
	void  (*scan_forget)(App *a);
} EglBridge;

extern const EglBridge egl_bridge_wayland;
extern const EglBridge egl_bridge_x11;
extern const EglBridge egl_bridge_kms;

#endif /* FNL_GL_EGL_H */

/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * EGL, opened at run time rather than linked (DESIGN *One build per
 * operating system*, *The link line*): a machine without libEGL.so.1 still
 * loads the program, and the OpenGL renderer fails its start so that
 * fnl_init() steps past it.
 *
 * Every EGL function the renderer's Linux side calls by name is a pointer
 * here, filled by egl_dyn_load(), and each name is redirected to its pointer
 * below, so a call site reads as plain EGL. The redirect must be seen before
 * any EGL call: gl_egl.h includes this header, and every file that calls EGL
 * includes that. Extension functions are not listed; they are reached
 * through eglGetProcAddress, which is.
 *
 * Included after "core/app.h", which every .c names first.
 */

#ifndef FNL_EGL_DYN_H
#define FNL_EGL_DYN_H

/* An eglplatform.h older than Khronos's current one includes <X11/Xlib.h>
 * unless told not to, and Xlib is what the X11 backend exists to do
 * without. */
#define EGL_NO_X11
#define MESA_EGL_NO_X11_HEADERS

/* With the prototypes, which are what each pointer takes its type from. */
#include <EGL/egl.h>
#include <EGL/eglext.h>

/* Each function called by name. eglGetPlatformDisplay and
 * eglCreatePlatformWindowSurface are EGL 1.5's, which the renderer needs
 * anyway (egl.c), so a library without them is a library that cannot serve
 * it. */
#define FNL_EGL_FUNCS(X)                                                      \
	X(eglBindAPI)                                                             \
	X(eglChooseConfig)                                                        \
	X(eglCreateContext)                                                       \
	X(eglCreatePlatformWindowSurface)                                         \
	X(eglDestroyContext)                                                      \
	X(eglDestroySurface)                                                      \
	X(eglGetConfigAttrib)                                                     \
	X(eglGetError)                                                            \
	X(eglGetPlatformDisplay)                                                  \
	X(eglGetProcAddress)                                                      \
	X(eglInitialize)                                                          \
	X(eglMakeCurrent)                                                         \
	X(eglQueryString)                                                         \
	X(eglReleaseThread)                                                       \
	X(eglSwapBuffers)                                                         \
	X(eglSwapInterval)                                                        \
	X(eglTerminate)

/* Declared before the redirect, while each name is still the prototype's. */
#define FNL_EGL_DECLARE(f) extern __typeof__(f) *fnl_##f;
FNL_EGL_FUNCS(FNL_EGL_DECLARE)
#undef FNL_EGL_DECLARE

/* The redirect, one line per entry of the list and in its order. A macro
 * argument is expanded before it is substituted unless it is the operand
 * of # or ##, so a body over FNL_EGL_FUNCS names its argument only as #f or
 * fnl_##f from here on. */
#define eglBindAPI                     (*fnl_eglBindAPI)
#define eglChooseConfig                (*fnl_eglChooseConfig)
#define eglCreateContext               (*fnl_eglCreateContext)
#define eglCreatePlatformWindowSurface (*fnl_eglCreatePlatformWindowSurface)
#define eglDestroyContext              (*fnl_eglDestroyContext)
#define eglDestroySurface              (*fnl_eglDestroySurface)
#define eglGetConfigAttrib             (*fnl_eglGetConfigAttrib)
#define eglGetError                    (*fnl_eglGetError)
#define eglGetPlatformDisplay          (*fnl_eglGetPlatformDisplay)
#define eglGetProcAddress              (*fnl_eglGetProcAddress)
#define eglInitialize                  (*fnl_eglInitialize)
#define eglMakeCurrent                 (*fnl_eglMakeCurrent)
#define eglQueryString                 (*fnl_eglQueryString)
#define eglReleaseThread               (*fnl_eglReleaseThread)
#define eglSwapBuffers                 (*fnl_eglSwapBuffers)
#define eglSwapInterval                (*fnl_eglSwapInterval)
#define eglTerminate                   (*fnl_eglTerminate)

/* A hold on libEGL.so.1, opened and every pointer filled where no hold
 * has it open. FNL_ERR_NO_DEVICE, recorded with the reason, where the
 * library or any entry of the list is missing -- remembered, and no hold
 * taken -- and then no pointer may be called. From any thread. */
FNL_MUST_USE FnlResult egl_dyn_load(void);
/* A hold taken only where one already has the library open: false, and no
 * hold, where none has, so a caller off the render thread pins what the
 * renderer opened and never opens it again itself. From any thread. */
FNL_MUST_USE bool egl_dyn_hold_open(void);
/* The hold given back. With `close`, the last one closes the library and
 * nulls every pointer (egl_dyn.c says when); without, it stays open as
 * every runtime does (core/dynlib.h). Every EGL object made under the
 * holds is gone by then, and each thread that called EGL has released
 * it (eglReleaseThread). */
void egl_dyn_release(bool close);

#endif /* FNL_EGL_DYN_H */

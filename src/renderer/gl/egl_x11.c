/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The X11-to-EGL bridge's half, and the sibling of the Vulkan one in
 * src/renderer/vulkan/surface_x11.c: the same connection, window and visual.
 *
 * Through EGL_EXT_platform_xcb and so through core xcb, the connection the
 * backend already has, where GLX would bring Xlib onto the link line.
 * BackendState::gfx and never ::conn, for the Vulkan bridge's reason: the
 * driver blocks in the connection it is handed, and the pump's must have one
 * reader -- see x11_state.h.
 */

#include "core/app.h"

#include "renderer/gl/gl_internal.h"
#include "renderer/gl/gl_egl.h"

#include "backend/x11/x11_state.h"

static bool native_display(App *a, EglNative *out)
{
	*out = (EglNative){
		.platform = EGL_PLATFORM_XCB_EXT,
		.display  = a->backend_state->gfx,
		.attrs    = { EGL_PLATFORM_XCB_SCREEN_EXT,
					  (EGLAttrib)a->backend_state->screen_num, EGL_NONE },
	};
	return true;
}

/* The window is made with the root visual before there is any
 * configuration (x11.c), so the configuration follows the window. */
static EGLint native_visual(const App *a)
{
	return (EGLint)a->backend_state->screen->root_visual;
}

/* The visual decides, and the root visual has no alpha: X11's window has no
 * transparent grab ring. */
static EGLint native_alpha(const App *a)
{
	(void)a;
	return 0;
}

/* Under a compositor -- XWayland's, or a desktop's on Xorg -- what is
 * presented is shown at the compositor's refresh and does not tear, and
 * Mesa's swap under an interval of 1 only adds its wait: one present in
 * flight, each held for the last one's completion, which during a resize
 * comes late and costs a whole refresh (DESIGN *X11*). The loop spaces the
 * frames instead, at the output's refresh. With nobody compositing the swap
 * is the throttle, but for a resize (frame.c). */
static const char *native_pacer(const App *a)
{
	return a->backend_state->composited &&
		   fnl_atomic_load_u32(&a->present_mode) == 1u
			   ? "paced by the loop (vsync)" : NULL;
}

/* EGL_EXT_platform_xcb takes a pointer to the window's id. */
static void *native_window(App *a)
{
	return &a->backend_state->window;
}

static void native_window_close(App *a)
{
	(void)a;
}

/* The X server sizes the window, and EGL follows it. */
static void native_window_resize(App *a, int w, int h)
{
	(void)a;
	(void)w;
	(void)h;
}

const EglBridge egl_bridge_x11 = {
	.backend       = BACKEND_X11,
	.surfaceless   = false,
	.display       = native_display,
	.visual        = native_visual,
	.alpha         = native_alpha,
	.pacer         = native_pacer,
	.window        = native_window,
	.window_close  = native_window_close,
	.window_resize = native_window_resize,
};

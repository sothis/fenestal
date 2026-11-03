/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Wayland-to-EGL bridge's half, and the sibling of the Vulkan one in
 * src/renderer/vulkan/surface_wayland.c: the same display and wl_surface.
 *
 * The native window is a wl_egl_window, libwayland-egl's, which is the
 * runtime of this target as libwayland-client is of every Wayland one. It
 * has a size of its own, unlike an X11 window's, so every rebuild hands it
 * the new extent (native_window_resize()); Mesa takes it up at the next
 * buffer it draws into, which is the blit at the end of that frame.
 *
 * libwayland-egl is opened at run time, as libwayland-client is
 * (backend/wayland/wl_dyn.h), and by the same shape: the prototypes
 * first, then a pointer for each and its name a macro onto it.
 */

#include "core/app.h"

#include "renderer/gl/gl_internal.h"
#include "renderer/gl/gl_egl.h"

#include "backend/wayland/wayland_state.h"

#include "core/dynlib.h"

/* The core header alone: <wayland-egl.h> adds <wayland-client.h>, which
 * wayland_state.h has already included through the redirect. After the EGL
 * headers, since it defines WL_EGL_PLATFORM, which would change their
 * native types. */
#include <wayland-egl-core.h>

#define WL_EGL_FUNCS(X)          \
	X(wl_egl_window_create)      \
	X(wl_egl_window_destroy)     \
	X(wl_egl_window_resize)

/* Before the #defines, the one body that may name the function bare. */
#define WL_EGL_DECLARE(f) static __typeof__(f) *fnl_##f;
WL_EGL_FUNCS(WL_EGL_DECLARE)
#undef WL_EGL_DECLARE

/* In the list's order. */
#define wl_egl_window_create  (*fnl_wl_egl_window_create)
#define wl_egl_window_destroy (*fnl_wl_egl_window_destroy)
#define wl_egl_window_resize  (*fnl_wl_egl_window_resize)

#define WL_EGL_LIB_NAME "libwayland-egl.so.1"

/* NULL once every entry is resolved; otherwise why not, the same on every
 * call. Opened when the first window is wanted and never closed. A plain
 * static remembers: the window is made at startup on the main thread and
 * at a recovery on the render thread, which starts after the first. */
static const char *wl_egl_load(void)
{
	static int  state;      /* 0 untried, 1 resolved, -1 refused */
	static char reason[256];
	if (state)
		return state > 0 ? NULL : reason;

	static const char *const NAMES[] = { WL_EGL_LIB_NAME, NULL };
	char why[200];
	void *lib = fnl_lib_open(NAMES, why, sizeof why);
	if (!lib) {
		snprintf(reason, sizeof reason, "no " WL_EGL_LIB_NAME ": %s", why);
		state = -1;
		return reason;
	}

	state = 1;
#define WL_EGL_RESOLVE(f)                                                  \
	if (state > 0 &&                                                       \
		!(fnl_##f = (__typeof__(fnl_##f))fnl_lib_sym(lib, #f))) {          \
		snprintf(reason, sizeof reason, WL_EGL_LIB_NAME " has no %s", #f); \
		state = -1;                                                        \
	}
	WL_EGL_FUNCS(WL_EGL_RESOLVE)
#undef WL_EGL_RESOLVE
	return state > 0 ? NULL : reason;
}

static bool native_display(App *a, EglNative *out)
{
	*out = (EglNative){
		.platform = EGL_PLATFORM_WAYLAND_KHR,
		.display  = a->backend_state->display,
		.attrs    = { EGL_NONE },
	};
	return true;
}

/* A Wayland surface has no visual; the configuration decides the format. */
static EGLint native_visual(const App *a)
{
	(void)a;
	return 0;
}

/* The grab ring is transparent, so the buffer has alpha, pre-multiplied as
 * Wayland's ARGB is; fullscreen drops it by EGL_EXT_present_opaque instead
 * (egl.c). DESIGN *Composite alpha*. */
static EGLint native_alpha(const App *a)
{
	(void)a;
	return 8;
}

/* Mesa's swap under an interval of 1 waits for a frame callback of its own,
 * with no timeout, and a hidden window is sent none: the render thread would
 * stop there for as long as the window stays hidden. The library's frame
 * callback, which does time out, is the throttle here, as it is for Vulkan
 * on this window server (DESIGN *Late latching*). Without vsync nothing
 * paces, and the interval is 0 either way. */
static const char *native_pacer(const App *a)
{
	return fnl_atomic_load_u32(&a->present_mode) == 1u
			   ? "frame callback (vsync)" : NULL;
}

/* At the extent of the last build, or 1x1 before the first: the size is
 * replaced at every rebuild, and wl_egl_window_create refuses zero. */
static void *native_window(App *a)
{
	/* The renderer cannot draw on this backend without it; the backend
	 * itself is open and fine. */
	const char *missing = wl_egl_load();
	if (missing) {
		const FnlResult rc = fnl_fail(FNL_ERR_NO_DEVICE, "%s", missing);
		(void)rc;
		return NULL;
	}
	const int w = a->extent.w > 0 ? a->extent.w : 1;
	const int h = a->extent.h > 0 ? a->extent.h : 1;
	struct wl_egl_window *win =
		wl_egl_window_create(a->backend_state->present, w, h);
	if (!win) {
		/* Recorded, and the caller reads the NULL: a cast to void would not
		 * quiet gcc's unused-result warning. */
		const FnlResult rc = fnl_fail(FNL_ERR_SYSTEM,
									  "wl_egl_window_create failed");
		(void)rc;
		return NULL;
	}
	GLS(a)->bridge_native = win;
	return win;
}

static void native_window_resize(App *a, int w, int h)
{
	if (GLS(a)->bridge_native)
		wl_egl_window_resize(GLS(a)->bridge_native, w, h, 0, 0);
}

static void native_window_close(App *a)
{
	if (!GLS(a)->bridge_native)
		return;
	wl_egl_window_destroy(GLS(a)->bridge_native);
	GLS(a)->bridge_native = NULL;
}

const EglBridge egl_bridge_wayland = {
	.backend       = BACKEND_WAYLAND,
	.surfaceless   = false,
	.display       = native_display,
	.visual        = native_visual,
	.alpha         = native_alpha,
	.pacer         = native_pacer,
	.window        = native_window,
	.window_close  = native_window_close,
	.window_resize = native_window_resize,
};

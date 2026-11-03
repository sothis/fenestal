/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The EGL bridge: display, configuration, window surface and context, for
 * every window server that reaches OpenGL through EGL. What differs between
 * them -- the native display, the visual, the native window -- is the few
 * lines of egl_<backend>.c (gl_egl.h).
 *
 * Desktop OpenGL and not GLES: eglBindAPI(EGL_OPENGL_API), which is per
 * thread, so it is said again before every context is made -- at startup on
 * the main thread and at a recovery on the render thread.
 */

#include "core/app.h"

#include "renderer/gl/gl_internal.h"
#include "renderer/gl/gl_egl.h"

/* EGL_EXT_device_query_name's, where the headers predate it. */
#ifndef EGL_RENDERER_EXT
#define EGL_RENDERER_EXT 0x335F
#endif

/* One per backend this build has; see EglBridge. Made from FNL_BACKENDS
 * as the backend list is, so the backend that opened has one. */
#define BRIDGE_ENTRY(key) &egl_bridge_##key,
static const EglBridge *const BRIDGES[] = { FNL_BACKENDS(BRIDGE_ENTRY) };
#undef BRIDGE_ENTRY

static const EglBridge *native(const App *a)
{
	size_t i = 0;
	while (i + 1 < sizeof BRIDGES / sizeof BRIDGES[0] &&
		   BRIDGES[i]->backend != BACKEND(a)->id)
		i++;
	FNL_ASSERT(BRIDGES[i]->backend == BACKEND(a)->id);
	return BRIDGES[i];
}

static const char *egl_error_name(EGLint e)
{
	switch (e) {
	case EGL_SUCCESS:             return "EGL_SUCCESS";
	case EGL_NOT_INITIALIZED:     return "EGL_NOT_INITIALIZED";
	case EGL_BAD_ACCESS:          return "EGL_BAD_ACCESS";
	case EGL_BAD_ALLOC:           return "EGL_BAD_ALLOC";
	case EGL_BAD_ATTRIBUTE:       return "EGL_BAD_ATTRIBUTE";
	case EGL_BAD_CONFIG:          return "EGL_BAD_CONFIG";
	case EGL_BAD_CONTEXT:         return "EGL_BAD_CONTEXT";
	case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
	case EGL_BAD_DISPLAY:         return "EGL_BAD_DISPLAY";
	case EGL_BAD_MATCH:           return "EGL_BAD_MATCH";
	case EGL_BAD_NATIVE_PIXMAP:   return "EGL_BAD_NATIVE_PIXMAP";
	case EGL_BAD_NATIVE_WINDOW:   return "EGL_BAD_NATIVE_WINDOW";
	case EGL_BAD_PARAMETER:       return "EGL_BAD_PARAMETER";
	case EGL_BAD_SURFACE:         return "EGL_BAD_SURFACE";
	case EGL_CONTEXT_LOST:        return "EGL_CONTEXT_LOST";
	default:                      return "an unknown EGL error";
	}
}

/* EGL_NOT_INITIALIZED on a display that was: terminated under the renderer
 * (RendererState::display_gone), which the recovery answers by opening it
 * again. */
static void note_terminated(App *a, EGLint e)
{
	if (e == EGL_NOT_INITIALIZED && GLS(a)->bridge_display)
		GLS(a)->display_gone = true;
}

static FnlResult egl_fail(App *a, FnlResult code, const char *what)
{
	const EGLint e = eglGetError();
	note_terminated(a, e);
	return fnl_fail(code, "%s failed: %s", what, egl_error_name(e));
}

/* The configuration: 8-bit RGB, renderable by desktop GL, for a window, with
 * the window's visual where it already has one and alpha where the buffer
 * carries the window's transparency. No depth or multisampling: nothing is
 * drawn into the window's own framebuffer but the finished frame, blitted
 * (target.c). */
static FnlResult choose_config(App *a, EGLDisplay dpy)
{
	const EGLint want[] = {
		/* A surfaceless bridge makes no window; a pbuffer configuration
		 * is what Mesa's surfaceless display offers. */
		EGL_SURFACE_TYPE,    native(a)->surfaceless ? EGL_PBUFFER_BIT
													: EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE,   8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE,  8,
		EGL_ALPHA_SIZE, native(a)->alpha(a),
		EGL_NONE,
	};
	EGLint n = 0;
	if (!eglChooseConfig(dpy, want, NULL, 0, &n) || n <= 0)
		return fnl_fail(FNL_ERR_NO_DEVICE,
						"EGL offers no configuration for an OpenGL window");
	EGLConfig *all = calloc((size_t)n, sizeof *all);
	if (!all)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
						"out of memory listing EGL configurations");
	if (!eglChooseConfig(dpy, want, all, n, &n)) {
		free(all);
		return egl_fail(a, FNL_ERR_NO_DEVICE, "eglChooseConfig");
	}
	/* The order is EGL's, best first by its own sort; the first that matches
	 * the window's visual, else the first. */
	const EGLint visual = native(a)->visual(a);
	EGLConfig pick = all[0];
	if (visual)
		for (EGLint i = 0; i < n; i++) {
			EGLint v = 0;
			if (eglGetConfigAttrib(dpy, all[i], EGL_NATIVE_VISUAL_ID, &v) &&
				v == visual) {
				pick = all[i];
				break;
			}
		}
	free(all);
	GLS(a)->bridge_config = pick;
	return FNL_OK;
}

/* A whole name in EGL's space-separated list, not a prefix of a longer
 * one. */
static bool has_extension(const char *list, const char *name)
{
	const size_t n = strlen(name);
	for (const char *p = list; p && (p = strstr(p, name)) != NULL; p += n)
		if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0'))
			return true;
	return false;
}

/* ---- which GPU ------------------------------------------------------------
 *
 * EGL lists its devices (EGL_EXT_device_enumeration), and a display can be
 * asked for on one: on the device itself where there is no window
 * (EGL_EXT_platform_device, KMS), and on the window server with the device
 * named (EGL_EXT_explicit_device). Not every driver takes the name: on X11
 * Mesa's and NVIDIA's both draw on the server's GPU whatever is named, and
 * on Wayland NVIDIA's takes it and Mesa's does not (schenker, Mesa 26.2.5,
 * NVIDIA 580, tools/probe/eglexplicit.c). So a device is listed where a
 * display opened for this window on it says it is on it
 * (EGL_DEVICE_EXT of the display), and nowhere else; the window server's
 * own choice is the display with none named. */

/* The device a display is on, where EGL says; EGL_NO_DEVICE_EXT where it
 * does not. */
static EGLDeviceEXT display_device(EGLDisplay dpy)
{
	PFNEGLQUERYDISPLAYATTRIBEXTPROC query = (PFNEGLQUERYDISPLAYATTRIBEXTPROC)
		eglGetProcAddress("eglQueryDisplayAttribEXT");
	EGLAttrib dev = 0;
	if (query && query(dpy, EGL_DEVICE_EXT, &dev))
		return (EGLDeviceEXT)dev;
	return EGL_NO_DEVICE_EXT;
}

/* This window's display on `dev`, or the window server's choice for
 * EGL_NO_DEVICE_EXT. Not initialised. */
static EGLDisplay window_display(App *a, const EglNative *nat,
								 EGLDeviceEXT dev)
{
	if (dev == EGL_NO_DEVICE_EXT)
		return eglGetPlatformDisplay(nat->platform, nat->display,
									 nat->attrs);
	if (native(a)->surfaceless)
		return eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, dev, NULL);
	EGLAttrib attrs[8];
	size_t n = 0;
	while (n < 5 && nat->attrs[n] != EGL_NONE) {
		attrs[n] = nat->attrs[n];
		n++;
	}
	attrs[n++] = EGL_DEVICE_EXT;
	attrs[n++] = (EGLAttrib)dev;
	attrs[n] = EGL_NONE;
	return eglGetPlatformDisplay(nat->platform, nat->display, attrs);
}

/* Whether `dpy` makes a 4.5 core context, asked with no configuration
 * (EGL_KHR_no_config_context, which Mesa's and NVIDIA's both have); yes
 * where that cannot be asked, and the context made later says. Made and
 * destroyed and never current: which context is current on this thread is
 * not the list's to change. 4.5 and not 4.1, although the renderer draws
 * with either: a device with 4.1 alone is drawn on where it is the window
 * server's own and nothing listed has 4.5, and is offered as a move only
 * once a move onto one has been seen to land. */
static bool makes_gl45(EGLDisplay dpy)
{
	if (!has_extension(eglQueryString(dpy, EGL_EXTENSIONS),
					   "EGL_KHR_no_config_context") ||
		!eglBindAPI(EGL_OPENGL_API))
		return true;
	const EGLint attrs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 4,
		EGL_CONTEXT_MINOR_VERSION, 5,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
		EGL_NONE,
	};
	EGLContext c = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
									attrs);
	if (c == EGL_NO_CONTEXT)
		return false;
	eglDestroyContext(dpy, c);
	return true;
}

/* Every device a display for this window can be opened on and draw with a
 * 4.5 core context, into `l` -- each opened and closed again, a driver
 * loaded, which the renderer pays once a life (gl_bridge_open()). A device
 * with no name to give and no software flag is a driver's view of a card
 * another drives (Mesa's of NVIDIA's, under NVIDIA's own module) and is
 * passed over without opening it. One that opens and makes no 4.5 context
 * is left out with a line: listed, a move to it fails and comes back
 * (nouveau on GM107 and GP107, Mesa 26.2.5: EGL_BAD_MATCH). */
static void list_devices(App *a, const EglNative *nat, GlDevices *l)
{
	l->n = 0;
	l->refused_n = 0;
	const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	if (!has_extension(client, "EGL_EXT_device_enumeration") ||
		!has_extension(client, "EGL_EXT_device_query") ||
		!has_extension(client, native(a)->surfaceless
								   ? "EGL_EXT_platform_device"
								   : "EGL_EXT_explicit_device"))
		return;
	PFNEGLQUERYDEVICESEXTPROC query_devices = (PFNEGLQUERYDEVICESEXTPROC)
		eglGetProcAddress("eglQueryDevicesEXT");
	PFNEGLQUERYDEVICESTRINGEXTPROC device_string =
		(PFNEGLQUERYDEVICESTRINGEXTPROC)
			eglGetProcAddress("eglQueryDeviceStringEXT");
	EGLDeviceEXT devs[16];
	EGLint n = 0;
	if (!query_devices || !device_string || !query_devices(16, devs, &n))
		return;
	for (EGLint i = 0; i < n && l->n + l->refused_n < GPU_LIST_MAX;
		 i++) {
		const char *ext = device_string(devs[i], EGL_EXTENSIONS);
		const bool soft = has_extension(ext, "EGL_MESA_device_software");
		const char *name = has_extension(ext, "EGL_EXT_device_query_name")
							   ? device_string(devs[i], EGL_RENDERER_EXT)
							   : NULL;
		if (!soft && !name)
			continue;
		EGLDisplay dpy = window_display(a, nat, devs[i]);
		EGLint major, minor;
		if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor))
			continue;
		const bool on_it = display_device(dpy) == devs[i];
		const bool gl45 = on_it && makes_gl45(dpy);
		eglTerminate(dpy);
		/* Asked for one device, the display is on another: Mesa's Wayland
		 * and X11 platforms (26.0 and 26.2) put a display asked of the
		 * software device on the hardware one, so it is not this device's
		 * to list. KMS's surfaceless display takes the device asked. */
		if (!on_it) {
			fnl_log(FNL_LOG_INFO, "GPU left out: %s, whose display for this "
					"window EGL opens on another device",
					name ? name : "a software device");
			continue;
		}
		if (!gl45) {
			l->refused[l->refused_n++] = devs[i];
			fnl_log(FNL_LOG_INFO, "GPU left out: %s, which makes no OpenGL "
					"4.5 core context", name ? name : "a software device");
			continue;
		}
		const int k = l->n++;
		l->dev[k] = devs[i];
		l->soft[k] = soft;
		snprintf(l->name[k], sizeof l->name[k], "%s",
				 name ? name : "Mesa software rasteriser");
	}
}

/* The device asked for (FnlInitDesc::gpu_index and ::gpu_name, FNL_GPU),
 * counted and named over the list; EGL_NO_DEVICE_EXT for the window
 * server's choice, and for an ask the list has no answer to, which is
 * said. */
static EGLDeviceEXT wanted_device(App *a)
{
	const struct RendererState *g = GLS(a);
	if (a->gpu_want[0]) {
		for (int i = 0; i < g->devs.n; i++)
			if (app_name_has(g->devs.name[i], a->gpu_want))
				return g->devs.dev[i];
		fnl_log(FNL_LOG_WARN, "no GPU named \"%s\" that OpenGL can draw "
				"this window on; the window server's is used", a->gpu_want);
	} else if (a->gpu_index > 0) {
		if (a->gpu_index <= g->devs.n)
			return g->devs.dev[a->gpu_index - 1];
		/* Where EGL lists none the list is the one GPU in use, the window
		 * server's (gl_bridge_gpus()), and 1 names it. */
		if (g->devs.n > 0 || a->gpu_index != 1)
			fnl_log(FNL_LOG_WARN, "no GPU %d that OpenGL can draw this "
					"window on; the window server's is used", a->gpu_index);
	}
	return EGL_NO_DEVICE_EXT;
}

int gl_bridge_gpus(App *a, GpuEntry *out, int *cur)
{
	const struct RendererState *g = GLS(a);
	for (int i = 0; i < g->devs.n; i++) {
		snprintf(out[i].name, sizeof out[i].name, "%s", g->devs.name[i]);
		out[i].kind = g->devs.soft[i] ? FNL_GPU_KIND_SOFTWARE
									 : FNL_GPU_KIND_UNKNOWN;
		out[i].renderer = FNL_RENDERER_OPENGL;
	}
	*cur = g->dev_cur;
	return g->devs.n;
}

/* Whether the display is on a device list_devices() left out. */
static bool on_refused(const struct RendererState *g, EGLDisplay dpy)
{
	const EGLDeviceEXT on = display_device(dpy);
	for (int i = 0; i < g->devs.refused_n; i++)
		if (g->devs.refused[i] == on)
			return true;
	return false;
}

/* The one GPU the window server's display draws on, where EGL lists none:
 * named as GL names it, by a 4.5 context made current on no surface
 * (EGL_KHR_surfaceless_context) -- this thread has no context of the
 * renderer's, which is another's while this is asked. 0 where there is no
 * 4.5 to be had here. */
static int server_gpu(const EglNative *nat, GpuEntry *out)
{
	EGLDisplay dpy = eglGetPlatformDisplay(nat->platform, nat->display,
										   nat->attrs);
	EGLint major, minor;
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor))
		return 0;
	int n = 0;
	const EGLint attrs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 4,
		EGL_CONTEXT_MINOR_VERSION, 5,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
		EGL_NONE,
	};
	EGLContext c = EGL_NO_CONTEXT;
	if (has_extension(eglQueryString(dpy, EGL_EXTENSIONS),
					  "EGL_KHR_no_config_context") &&
		eglBindAPI(EGL_OPENGL_API))
		c = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
	if (c != EGL_NO_CONTEXT &&
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
		typedef const unsigned char *(*GetString)(unsigned);
		const GetString get = (GetString)eglGetProcAddress("glGetString");
		const char *r = get ? (const char *)get(GL_RENDERER) : NULL;
		snprintf(out->name, sizeof out->name, "%s", r ? r : "(unnamed)");
		out->kind = r && gl_renderer_is_software(r) ? FNL_GPU_KIND_SOFTWARE
													: FNL_GPU_KIND_UNKNOWN;
		out->renderer = FNL_RENDERER_OPENGL;
		n = 1;
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	}
	if (c != EGL_NO_CONTEXT)
		eglDestroyContext(dpy, c);
	eglTerminate(dpy);
	eglReleaseThread();
	return n;
}

/* The library held for the renderer's instance, and for a listing while
 * another renderer draws. Given back closes it with the last hold where a
 * GPU may be moved to another kernel driver (egl_dyn.c), so that nothing
 * of NVIDIA's driver stays in the process between OpenGL's spells. */
FnlResult gl_bridge_hold(App *a)
{
	(void)a;
	return egl_dyn_load();
}

void gl_bridge_let_go(App *a)
{
	egl_dyn_release(a->gpu_drivers_on);
}

bool gl_bridge_hold_open(App *a)
{
	(void)a;
	return egl_dyn_hold_open();
}

int gl_bridge_list_gpus(App *a, GpuEntry *out, int max)
{
	if (max < 1 || gl_bridge_hold(a) < 0)
		return 0;
	int n = 0;
	EglNative nat;
	if (native(a)->display(a, &nat)) {
		GlDevices l;
		list_devices(a, &nat, &l);
		for (; n < l.n && n < max; n++) {
			snprintf(out[n].name, sizeof out[n].name, "%s", l.name[n]);
			out[n].kind = l.soft[n] ? FNL_GPU_KIND_SOFTWARE
									: FNL_GPU_KIND_UNKNOWN;
			out[n].renderer = FNL_RENDERER_OPENGL;
		}
		if (n == 0)
			n = server_gpu(&nat, out);
	}
	/* This thread's state handed back before the hold: the library may be
	 * closed with it. */
	eglReleaseThread();
	gl_bridge_let_go(a);
	return n;
}

/* Under the instance's hold on the library (gl_bridge_hold()), which a
 * machine without it fails with FNL_ERR_NO_DEVICE, which fnl_init() steps
 * past. Called again at a move to another GPU, after gl_bridge_close(),
 * with the choice the move made. */
FnlResult gl_bridge_open(App *a)
{
	EglNative nat;
	if (!native(a)->display(a, &nat))
		return FNL_ERR_NO_DISPLAY;
	struct RendererState *g = GLS(a);
	if (!g->dev_listed) {
		g->dev_listed = true;
		list_devices(a, &nat, &g->devs);
	}
	const EGLDeviceEXT dev = wanted_device(a);
	EGLDisplay dpy = window_display(a, &nat, dev);
	if (dpy == EGL_NO_DISPLAY)
		return egl_fail(a, FNL_ERR_NO_DEVICE, "eglGetPlatformDisplay");
	EGLint major = 0, minor = 0;
	if (!eglInitialize(dpy, &major, &minor))
		return egl_fail(a, FNL_ERR_NO_DEVICE, "eglInitialize");
	/* EGL's own choice, made where no device is named, gives way to the
	 * first listed GPU that is not software, else to the first, where it
	 * is one the list left out: Mesa's surfaceless display (KMS) takes the
	 * first render node, which on a laptop can be the discrete GPU's, and
	 * under nouveau that one has no 4.5. Where nothing is listed the
	 * context made later fails and says so. */
	if (dev == EGL_NO_DEVICE_EXT && g->devs.n > 0 && on_refused(g, dpy)) {
		int k = 0;
		for (int i = g->devs.n - 1; i >= 0; i--)
			if (!g->devs.soft[i])
				k = i;
		fnl_log(FNL_LOG_INFO, "EGL's own GPU makes no OpenGL 4.5 core "
				"context; drawing on %s", g->devs.name[k]);
		eglTerminate(dpy);
		dpy = window_display(a, &nat, g->devs.dev[k]);
		if (dpy == EGL_NO_DISPLAY)
			return egl_fail(a, FNL_ERR_NO_DEVICE, "eglGetPlatformDisplay");
		if (!eglInitialize(dpy, &major, &minor))
			return egl_fail(a, FNL_ERR_NO_DEVICE, "eglInitialize");
	}
	g->bridge_display = dpy;
	const EGLDeviceEXT on = display_device(dpy);
	g->dev_cur = -1;
	for (int i = 0; i < g->devs.n; i++)
		if (g->devs.dev[i] == on)
			g->dev_cur = i;
	/* eglGetPlatformDisplay and eglCreatePlatformWindowSurface, both used
	 * here, are EGL 1.5's. */
	if (major < 1 || (major == 1 && minor < 5))
		return fnl_fail(FNL_ERR_NO_DEVICE, "EGL %d.%d; this renderer needs 1.5",
						(int)major, (int)minor);
	const char *vendor = eglQueryString(dpy, EGL_VENDOR);
	fnl_log(FNL_LOG_INFO, "EGL %d.%d, %s", (int)major, (int)minor,
			vendor ? vendor : "(no vendor)");
	GLS(a)->can_opaque = native(a)->alpha(a) > 0 &&
						 has_extension(eglQueryString(dpy, EGL_EXTENSIONS),
									   "EGL_EXT_present_opaque");
	return choose_config(a, dpy);
}

void gl_bridge_close(App *a)
{
	if (!GLS(a)->bridge_display)
		return;
	eglBindAPI(EGL_OPENGL_API);
	eglMakeCurrent(GLS(a)->bridge_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
				   EGL_NO_CONTEXT);
	eglTerminate(GLS(a)->bridge_display);
	eglReleaseThread();
	GLS(a)->bridge_display = NULL;
	GLS(a)->bridge_config  = NULL;
}

/* Linear: the frame reaches the window by a blit that copies its sRGB-encoded
 * bytes as they are (target.c), so the surface is never asked to encode.
 * Opaque as RendererState::opaque says, where that can be said at all. */
static EGLSurface make_surface(App *a)
{
	EGLAttrib attrs[3] = { EGL_NONE, EGL_NONE, EGL_NONE };
	if (GLS(a)->can_opaque && GLS(a)->opaque) {
		attrs[0] = EGL_PRESENT_OPAQUE_EXT;
		attrs[1] = EGL_TRUE;
	}
	return eglCreatePlatformWindowSurface(GLS(a)->bridge_display,
										  GLS(a)->bridge_config,
										  GLS(a)->bridge_window, attrs);
}

/* Not current on this thread any longer, or the destroy is deferred to the
 * moment it stops being so -- which a recovery never reaches. */
static void destroy_surface(App *a)
{
	if (!GLS(a)->bridge_surface)
		return;
	eglBindAPI(EGL_OPENGL_API);
	eglMakeCurrent(GLS(a)->bridge_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
				   EGL_NO_CONTEXT);
	eglDestroySurface(GLS(a)->bridge_display, GLS(a)->bridge_surface);
	GLS(a)->bridge_surface = NULL;
}

/* With the opacity the last surface had: a surface made again after a loss
 * is under the grab ring the rebuild that follows still compares against. */
FnlResult gl_bridge_surface(App *a)
{
	/* Nothing to make: the context goes current on EGL_NO_SURFACE, which
	 * bridge_surface left null is. */
	if (native(a)->surfaceless) {
		GLS(a)->interval = -1;
		return FNL_OK;
	}
	GLS(a)->bridge_window = native(a)->window(a);
	if (!GLS(a)->bridge_window)
		return FNL_ERR_SYSTEM;
	EGLSurface s = make_surface(a);
	if (s == EGL_NO_SURFACE) {
		const FnlResult rc = egl_fail(a, FNL_ERR_SYSTEM,
									  "eglCreatePlatformWindowSurface");
		native(a)->window_close(a);
		GLS(a)->bridge_window = NULL;
		return rc;
	}
	GLS(a)->bridge_surface = s;
	GLS(a)->interval = -1;        /* the next rebuild sets one */
	return FNL_OK;
}

/* The native window too when there is no surface: a surface made again over
 * it may have failed and left it. */
void gl_bridge_surface_close(App *a)
{
	destroy_surface(a);
	native(a)->window_close(a);
	GLS(a)->bridge_window = NULL;
}

/* The surface is gone and the context is current nowhere. It is made current
 * with no surface -- EGL 1.5's, which every 4.5 context takes -- so that the
 * drain, the caller's on_gpu_lost and the teardown run as after any loss.
 * Only where even that is refused does `loaded` go, as in gl_thread_enter():
 * no GL call until the recovery, which forgets the context's names. */
static void lose_surface(App *a)
{
	a->surface_lost = true;
	if (!eglMakeCurrent(GLS(a)->bridge_display, EGL_NO_SURFACE,
						EGL_NO_SURFACE, GLS(a)->bridge_context))
		GLS(a)->loaded = false;
}

/* The native window keeps its size and its place in the window server's
 * scene; only the EGL surface over it is made again, with the context made
 * current on the new one. A failure is the surface's loss, so the recovery
 * makes both again. */
FnlResult gl_bridge_resize(App *a, int w, int h, bool opaque)
{
	native(a)->window_resize(a, w, h);
	if (!GLS(a)->can_opaque || GLS(a)->opaque == opaque)
		return FNL_OK;

	destroy_surface(a);
	GLS(a)->opaque = opaque;
	EGLSurface s = make_surface(a);
	if (opaque && FNL_FAULT("gl-opaque-refused") && s != EGL_NO_SURFACE) {
		eglDestroySurface(GLS(a)->bridge_display, s);
		lose_surface(a);
		GLS(a)->can_opaque = false;
		GLS(a)->opaque = false;
		return fnl_fail(FNL_ERR_SYSTEM, "the opaque EGL surface was "
						"refused (FNL_FAULT)");
	}
	if (s == EGL_NO_SURFACE) {
		const char *what = opaque
						 ? "an opaque eglCreatePlatformWindowSurface"
						 : "eglCreatePlatformWindowSurface";
		const FnlResult rc = egl_fail(a, FNL_ERR_SYSTEM, what);
		/* An opaque one refused is never asked for again: the recovery's
		 * would be refused the same way, every turn. The window keeps its
		 * alpha, which inside it is 1 in every pixel and costs only the
		 * scanout plane. A plain one refused is the surface's loss alone. */
		if (opaque) {
			GLS(a)->can_opaque = false;
			GLS(a)->opaque = false;
		}
		lose_surface(a);
		return rc;
	}
	GLS(a)->bridge_surface = s;
	if (!eglMakeCurrent(GLS(a)->bridge_display, s, s,
						GLS(a)->bridge_context)) {
		const FnlResult rc = egl_fail(a, FNL_ERR_SYSTEM, "eglMakeCurrent");
		lose_surface(a);
		return rc;
	}
	/* A new surface has the default interval, whatever the old one had. */
	GLS(a)->interval = -1;
	return FNL_OK;
}

static EGLContext make_context(App *a, int minor, bool robust)
{
	EGLint attrs[16];
	int n = 0;
	attrs[n++] = EGL_CONTEXT_MAJOR_VERSION;
	attrs[n++] = 4;
	attrs[n++] = EGL_CONTEXT_MINOR_VERSION;
	attrs[n++] = minor;
	attrs[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK;
	attrs[n++] = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT;
	if (robust) {
		attrs[n++] = EGL_CONTEXT_OPENGL_ROBUST_ACCESS;
		attrs[n++] = EGL_TRUE;
		attrs[n++] = EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY;
		attrs[n++] = EGL_LOSE_CONTEXT_ON_RESET;
	}
#ifndef NDEBUG
	/* A Debug tree's GL errors reach the log (device.c), as a Vulkan
	 * round's reach it through the validation layer. */
	attrs[n++] = EGL_CONTEXT_OPENGL_DEBUG;
	attrs[n++] = EGL_TRUE;
#endif
	attrs[n] = EGL_NONE;
	return eglCreateContext(GLS(a)->bridge_display, GLS(a)->bridge_config,
							EGL_NO_CONTEXT, attrs);
}

FnlResult gl_bridge_context(App *a)
{
	if (!eglBindAPI(EGL_OPENGL_API))
		return egl_fail(a, FNL_ERR_NO_DEVICE, "eglBindAPI(EGL_OPENGL_API)");
	/* Robust where the driver will, since that is what reports a reset; a
	 * driver that will not still draws, and a reset there is undefined, as
	 * GL without it has always had it. */
	/* 4.5 core, and 4.1 core where the device makes no 4.5: the renderer
	 * draws with either (gl_load(), DESIGN *OpenGL 4.1*), and a scene of
	 * the caller's is told which (fnl_gl_version()). */
	static const int MINORS[] = { 5, 1 };
	EGLContext c = EGL_NO_CONTEXT;
	EGLint err = EGL_SUCCESS;
	for (size_t i = 0; i < 2 && c == EGL_NO_CONTEXT; i++) {
		GLS(a)->robust = true;
		c = make_context(a, MINORS[i], true);
		if (c == EGL_NO_CONTEXT) {
			GLS(a)->robust = false;
			c = make_context(a, MINORS[i], false);
		}
		if (c == EGL_NO_CONTEXT && i == 0)
			err = eglGetError();
		else if (c != EGL_NO_CONTEXT && i == 1)
			fnl_log(FNL_LOG_INFO, "no OpenGL 4.5 core context here (%s); "
					"drawing with 4.1 core", egl_error_name(err));
	}
	/* No other fallback here: gl_bridge_open() has already passed EGL's own
	 * choice over for a listed device where that made no 4.5 context. Where
	 * there was none to pass to, Mesa is the one EGL with a software
	 * device, and on X11 and Wayland it ignores EGL_DEVICE_EXT (26.0.8,
	 * egl_dri2.c's dri2_setup_device()): only LIBGL_ALWAYS_SOFTWARE, in the
	 * environment, switches a display to it, and a library does not write
	 * the caller's environment. So the reason says how. */
	if (c == EGL_NO_CONTEXT)
		return fnl_fail(FNL_ERR_NO_DEVICE,
						"eglCreateContext for OpenGL 4.5 and 4.1 core "
						"failed: %s -- this device has no OpenGL 4.1; "
						"LIBGL_ALWAYS_SOFTWARE=1 draws with Mesa's software "
						"rasteriser instead",
						egl_error_name(eglGetError()));
	GLS(a)->bridge_context = c;
	if (!eglMakeCurrent(GLS(a)->bridge_display, GLS(a)->bridge_surface,
						GLS(a)->bridge_surface, c))
		return egl_fail(a, FNL_ERR_SYSTEM, "eglMakeCurrent");
	if (!GLS(a)->robust)
		fnl_log(FNL_LOG_WARN, "the OpenGL context has no reset notification; "
				"a lost device will not be noticed");
	return FNL_OK;
}

void gl_bridge_context_close(App *a)
{
	if (!GLS(a)->bridge_context)
		return;
	eglBindAPI(EGL_OPENGL_API);
	eglMakeCurrent(GLS(a)->bridge_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
				   EGL_NO_CONTEXT);
	eglDestroyContext(GLS(a)->bridge_display, GLS(a)->bridge_context);
	GLS(a)->bridge_context = NULL;
}

/* The API is bound first, every time: it is per thread, a thread that never
 * bound one is GLES's, and eglMakeCurrent(EGL_NO_CONTEXT) releases only the
 * bound API's context -- so on the render thread, unbound, the release would
 * leave this one current. Leaving also hands back EGL's per-thread state,
 * which a thread about to end would otherwise keep. */
bool gl_bridge_current(App *a, bool on)
{
	if (!GLS(a)->bridge_display)
		return true;
	eglBindAPI(EGL_OPENGL_API);
	if (on && GLS(a)->bridge_context) {
		if (eglMakeCurrent(GLS(a)->bridge_display, GLS(a)->bridge_surface,
						   GLS(a)->bridge_surface, GLS(a)->bridge_context))
			return true;
		const EGLint e = eglGetError();
		note_terminated(a, e);
		fnl_log(FNL_LOG_ERROR, "eglMakeCurrent failed on the %s thread: "
				"%s%s", app_thread() == APP_THREAD_RENDER ? "render" : "main",
				egl_error_name(e), e == EGL_NOT_INITIALIZED
				? " -- the display was terminated under the renderer, as "
				  "NVIDIA's Vulkan driver does to it at an instance's end"
				: "");
		return false;
	}
	eglMakeCurrent(GLS(a)->bridge_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
				   EGL_NO_CONTEXT);
	eglReleaseThread();
	return true;
}

const char *gl_bridge_pacer(const App *a)
{
	return native(a)->pacer(a);
}

/* EGL says nothing of a covered window. */
bool gl_bridge_occluded(const App *a)
{
	(void)a;
	return false;
}

bool gl_bridge_surfaceless(const App *a)
{
	return native(a)->surfaceless;
}

bool gl_bridge_scan_begin(App *a, unsigned fbo, int w, int h)
{
	const EglBridge *b = native(a);
	return b->scan_begin && b->scan_begin(a, fbo, w, h);
}

bool gl_bridge_scan_show(App *a)
{
	const EglBridge *b = native(a);
	return b->scan_show && b->scan_show(a);
}

void gl_bridge_scan_close(App *a)
{
	const EglBridge *b = native(a);
	if (b->scan_close)
		b->scan_close(a);
}

void gl_bridge_scan_forget(App *a)
{
	const EglBridge *b = native(a);
	if (b->scan_forget)
		b->scan_forget(a);
}

bool gl_bridge_interval(App *a, int n)
{
	/* No surface to give one: the backend's flip paces. */
	if (native(a)->surfaceless)
		return true;
	if (eglSwapInterval(GLS(a)->bridge_display, n))
		return true;
	fnl_log(FNL_LOG_WARN, "eglSwapInterval(%d) failed: %s", n,
			egl_error_name(eglGetError()));
	return false;
}

GlSwap gl_bridge_swap(App *a)
{
	if (eglSwapBuffers(GLS(a)->bridge_display, GLS(a)->bridge_surface))
		return GL_SWAP_OK;
	const EGLint e = eglGetError();
	switch (e) {
	case EGL_CONTEXT_LOST:
		return GL_SWAP_DEVICE_LOST;
	case EGL_BAD_SURFACE:
	case EGL_BAD_NATIVE_WINDOW:
	case EGL_BAD_CURRENT_SURFACE:
		return GL_SWAP_SURFACE_LOST;
	default:
		fnl_log(FNL_LOG_WARN, "eglSwapBuffers failed: %s",
				egl_error_name(e));
		return GL_SWAP_FAILED;
	}
}

FnlGlProc gl_bridge_proc(const char *name)
{
	return (FnlGlProc)eglGetProcAddress(name);
}

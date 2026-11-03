/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Whether EGL draws a window on a GPU it is told (EGL_EXT_explicit_device),
 * asked of a machine on X11 or on Wayland: for each device EGL lists, a
 * display on the window server with EGL_DEVICE_EXT naming that device, a
 * window surface, a 4.5 core context, a clear and a swap -- and the renderer
 * the context names, which says whether the device was taken or ignored.
 *
 *   cc -O2 tools/probe/eglexplicit.c -o eglexplicit \
 *      -lEGL -lxcb -lwayland-client -lwayland-egl
 *   DISPLAY=:9 ./eglexplicit x11
 *   WAYLAND_DISPLAY=wayland-fenestal ./eglexplicit wayland
 *
 * The Wayland surface has no role: EGL makes a surface on it and swaps,
 * which is all that is asked; nothing is shown. Written for the OpenGL
 * renderer's GPU list (DESIGN *Moving to another GPU*).
 */

#define _POSIX_C_SOURCE 200809L

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <xcb/xcb.h>

#include <stdio.h>
#include <string.h>

#define GL_RENDERER_ENUM 0x1F01
#define GL_COLOR_BUFFER  0x00004000

typedef const unsigned char *(*GetStringFn)(unsigned int);
typedef void (*ClearFn)(unsigned int);

static struct wl_compositor *compositor;

static void reg_global(void *ud, struct wl_registry *r, uint32_t name,
					   const char *iface, uint32_t ver)
{
	(void)ud;
	(void)ver;
	if (strcmp(iface, "wl_compositor") == 0)
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
}

static void reg_remove(void *ud, struct wl_registry *r, uint32_t name)
{
	(void)ud;
	(void)r;
	(void)name;
}

static const struct wl_registry_listener REG = { reg_global, reg_remove };

int main(int argc, char **argv)
{
	const bool wayland = argc > 1 && strcmp(argv[1], "wayland") == 0;
	void *native_display = NULL;
	void *native_window = NULL;
	EGLenum platform;
	EGLAttrib extra[3] = { EGL_NONE, EGL_NONE, EGL_NONE };
	xcb_window_t xwin = 0;

	if (wayland) {
		struct wl_display *d = wl_display_connect(NULL);
		if (!d) {
			printf("no Wayland display\n");
			return 1;
		}
		struct wl_registry *r = wl_display_get_registry(d);
		wl_registry_add_listener(r, &REG, NULL);
		wl_display_roundtrip(d);
		if (!compositor) {
			printf("no wl_compositor\n");
			return 1;
		}
		struct wl_surface *s = wl_compositor_create_surface(compositor);
		native_display = d;
		native_window = wl_egl_window_create(s, 256, 256);
		platform = EGL_PLATFORM_WAYLAND_KHR;
	} else {
		int screen = 0;
		xcb_connection_t *c = xcb_connect(NULL, &screen);
		if (xcb_connection_has_error(c)) {
			printf("no X display\n");
			return 1;
		}
		xcb_screen_iterator_t it = xcb_setup_roots_iterator(
			xcb_get_setup(c));
		for (int i = 0; i < screen; i++)
			xcb_screen_next(&it);
		xwin = xcb_generate_id(c);
		xcb_create_window(c, XCB_COPY_FROM_PARENT, xwin, it.data->root, 0, 0,
						  256, 256, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
						  it.data->root_visual, 0, NULL);
		xcb_map_window(c, xwin);
		xcb_flush(c);
		native_display = c;
		native_window = &xwin;
		platform = EGL_PLATFORM_XCB_EXT;
		extra[0] = EGL_PLATFORM_XCB_SCREEN_EXT;
		extra[1] = screen;
	}

	PFNEGLQUERYDEVICESEXTPROC query_devices = (PFNEGLQUERYDEVICESEXTPROC)
		eglGetProcAddress("eglQueryDevicesEXT");
	PFNEGLQUERYDEVICESTRINGEXTPROC device_string =
		(PFNEGLQUERYDEVICESTRINGEXTPROC)
			eglGetProcAddress("eglQueryDeviceStringEXT");
	EGLDeviceEXT devs[16];
	EGLint n = 0;
	if (!query_devices || !query_devices(16, devs, &n))
		return 1;
	/* -1 first: no device named, the window server's own choice. */
	for (EGLint i = -1; i < n; i++) {
		EGLAttrib attrs[5];
		int k = 0;
		if (extra[0] != EGL_NONE) {
			attrs[k++] = extra[0];
			attrs[k++] = extra[1];
		}
		if (i >= 0) {
			attrs[k++] = EGL_DEVICE_EXT;
			attrs[k++] = (EGLAttrib)devs[i];
		}
		attrs[k] = EGL_NONE;
		const char *file = i >= 0 ? device_string(devs[i],
												  EGL_DRM_DEVICE_FILE_EXT)
								  : "(none named)";
		printf("device %d (%s):", (int)i, file ? file : "-");
		EGLDisplay d = eglGetPlatformDisplay(platform, native_display, attrs);
		EGLint maj, min;
		if (d == EGL_NO_DISPLAY || !eglInitialize(d, &maj, &min)) {
			printf(" no display 0x%x\n", eglGetError());
			continue;
		}
		const EGLint want[] = {
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
			EGL_NONE,
		};
		EGLConfig cfg;
		EGLint nc = 0;
		if (!eglChooseConfig(d, want, &cfg, 1, &nc) || nc < 1) {
			printf(" %s: no config\n", eglQueryString(d, EGL_VENDOR));
			eglTerminate(d);
			continue;
		}
		EGLSurface s = eglCreatePlatformWindowSurface(d, cfg, native_window,
													  NULL);
		eglBindAPI(EGL_OPENGL_API);
		const EGLint ca[] = {
			EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
			EGL_CONTEXT_OPENGL_PROFILE_MASK,
			EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE,
		};
		EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ca);
		if (s == EGL_NO_SURFACE || c == EGL_NO_CONTEXT ||
			!eglMakeCurrent(d, s, s, c)) {
			printf(" %s: surface %p context %p: 0x%x\n",
				   eglQueryString(d, EGL_VENDOR), (void *)s, (void *)c,
				   eglGetError());
			eglTerminate(d);
			continue;
		}
		GetStringFn get = (GetStringFn)eglGetProcAddress("glGetString");
		ClearFn clear = (ClearFn)eglGetProcAddress("glClear");
		clear(GL_COLOR_BUFFER);
		const EGLBoolean swapped = eglSwapBuffers(d, s);
		printf(" %s: %s, swap %s\n", eglQueryString(d, EGL_VENDOR),
			   (const char *)get(GL_RENDERER_ENUM),
			   swapped ? "ok" : "failed");
		eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroyContext(d, c);
		eglDestroySurface(d, s);
		eglTerminate(d);
	}
	return 0;
}

/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What EGL offers for choosing a GPU, asked of a machine: the client
 * extensions, every device EGL lists with its own extensions and DRM node,
 * and for each a display opened on that device alone
 * (EGL_PLATFORM_DEVICE_EXT) with a 4.5 core context made current on no
 * surface, and the renderer it names. No window: it runs from ssh.
 *
 *   cc -O2 tools/probe/egldevices.c -o egldevices -lEGL
 *   ./egldevices
 *
 * Written for the OpenGL renderer's GPU list (DESIGN *Moving to another
 * GPU*): which devices a display can be opened on, and what each is called.
 */

#define _POSIX_C_SOURCE 200809L

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <stdio.h>
#include <string.h>

#ifndef EGL_RENDERER_EXT
#define EGL_RENDERER_EXT 0x335F
#endif
#define GL_RENDERER_ENUM 0x1F01

typedef const unsigned char *(*GetStringFn)(unsigned int);

int main(void)
{
	const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
	printf("client extensions: %s\n", client ? client : "(none)");

	PFNEGLQUERYDEVICESEXTPROC query_devices = (PFNEGLQUERYDEVICESEXTPROC)
		eglGetProcAddress("eglQueryDevicesEXT");
	PFNEGLQUERYDEVICESTRINGEXTPROC device_string =
		(PFNEGLQUERYDEVICESTRINGEXTPROC)
			eglGetProcAddress("eglQueryDeviceStringEXT");
	PFNEGLGETPLATFORMDISPLAYEXTPROC platform_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)
			eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!query_devices || !device_string || !platform_display) {
		printf("no device enumeration\n");
		return 1;
	}
	EGLDeviceEXT devs[16];
	EGLint n = 0;
	if (!query_devices(16, devs, &n)) {
		printf("eglQueryDevicesEXT failed: 0x%x\n", eglGetError());
		return 1;
	}
	printf("%d devices\n", n);
	for (EGLint i = 0; i < n; i++) {
		const char *ext = device_string(devs[i], EGL_EXTENSIONS);
		const char *file = ext && strstr(ext, "EGL_EXT_device_drm")
							   ? device_string(devs[i],
											   EGL_DRM_DEVICE_FILE_EXT)
							   : NULL;
		const char *name = ext && strstr(ext, "EGL_EXT_device_query_name")
							   ? device_string(devs[i], EGL_RENDERER_EXT)
							   : NULL;
		printf("device %d: %s\n  file %s\n  name %s\n", i,
			   ext ? ext : "(no extensions)", file ? file : "-",
			   name ? name : "-");

		EGLDisplay d = platform_display(EGL_PLATFORM_DEVICE_EXT, devs[i],
										NULL);
		EGLint major = 0, minor = 0;
		if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) {
			printf("  no display: 0x%x\n", eglGetError());
			continue;
		}
		printf("  display EGL %d.%d, vendor %s\n", major, minor,
			   eglQueryString(d, EGL_VENDOR));
		eglBindAPI(EGL_OPENGL_API);
		const EGLint attrs[] = {
			EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
			EGL_CONTEXT_OPENGL_PROFILE_MASK,
			EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE,
		};
		EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT,
										attrs);
		if (c == EGL_NO_CONTEXT ||
			!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
			printf("  no 4.5 context: 0x%x\n", eglGetError());
		} else {
			GetStringFn get = (GetStringFn)eglGetProcAddress("glGetString");
			printf("  GL_RENDERER %s\n",
				   get ? (const char *)get(GL_RENDERER_ENUM) : "?");
			eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			eglDestroyContext(d, c);
		}
		eglTerminate(d);
	}
	return 0;
}

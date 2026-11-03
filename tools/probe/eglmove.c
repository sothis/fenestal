/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What OpenGL leaves of NVIDIA's driver in a process, and whether a GPU
 * moved to its other kernel driver is then found: one process, a list of
 * steps run in order, and after each the EGL and NVIDIA libraries mapped
 * and every GPU node open. Written for the OpenGL renderer's part in a move
 * (DESIGN *Another kernel driver, with the window open*, the paragraph on
 * glvnd); the moves are asked of the root helper's socket, as the library
 * asks them, so the machine needs bare_driver.py --setup.
 *
 *   cc -std=gnu11 -O2 tools/probe/eglmove.c -o eglmove -ldl -lpthread
 *   ./eglmove mode=glvnd query gl:nvidia unload move:nouveau vk
 *
 *   mode=glvnd|mesa   how the gl steps after it reach EGL: glvnd's
 *                     libEGL.so.1, or Mesa's vendor library driven
 *                     through __egl_Main with no glvnd at all
 *   zink              MESA_LOADER_DRIVER_OVERRIDE=zink from here on
 *   query             eglQueryDevicesEXT, each device printed
 *   gl:intel|nvidia|surfaceless
 *                     a 4.5 core context on that GPU's EGL device, the
 *                     first EGL lists for its nodes (or on Mesa's
 *                     surfaceless display, with no device query), current
 *                     on no surface, its renderer printed, then torn down
 *                     and the thread released
 *   unload            glvnd's libEGL.so.1 closed
 *   hold:intel|nvidia a display and a 4.5 core context on that GPU, made
 *                     current here and released, and kept open
 *   cur               the held context made current on this thread and
 *                     released again, the error printed where it is not
 *   thread:cur        the same on a thread of its own
 *   unhold            the held context and display destroyed
 *   vk                a Vulkan instance, its GPUs printed, destroyed
 *   move:nvidia|nouveau
 *                     the NVIDIA GPU moved, asked of
 *                     /run/fenestal-gpu-driver.sock
 *
 * The GPUs are found by vendor in sysfs: the first display device of
 * NVIDIA's (0x10de) and of Intel's (0x8086).
 */

#define _GNU_SOURCE

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glvnd/libeglabi.h>
#include <vulkan/vulkan.h>

#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/run/fenestal-gpu-driver.sock"
#define GL_RENDERER_ENUM 0x1F01
#define GL_VERSION_ENUM  0x1F02
#define EGL_RENDERER_EXT_ENUM 0x335F

static char nvidia_pci[32];
static char intel_pci[32];

static int mesa_mode;
static void *glvnd;
static __EGLapiImports imports;
static int mesa_ready;

/* ---- the callbacks a vendor is handed: only Mesa's dispatch stubs read
 * them, and nothing here calls a stub. */
static void x_thread_init(void) {}
static EGLenum x_api(void) { return EGL_NONE; }
static __EGLvendorInfo *x_vendor(void) { return NULL; }
static EGLContext x_ctx(void) { return EGL_NO_CONTEXT; }
static EGLDisplay x_dpy(void) { return EGL_NO_DISPLAY; }
static EGLSurface x_surf(EGLint rd) { (void)rd; return EGL_NO_SURFACE; }
static __eglMustCastToProperFunctionPointerType
x_fetch(__EGLvendorInfo *v, int i) { (void)v; (void)i; return NULL; }
static void x_err(EGLint e) { (void)e; }
static EGLBoolean x_last(__EGLvendorInfo *v) { (void)v; return EGL_TRUE; }
static __EGLvendorInfo *x_vdpy(EGLDisplay d) { (void)d; return NULL; }
static __EGLvendorInfo *x_vdev(EGLDeviceEXT d) { (void)d; return NULL; }
static EGLBoolean x_setdev(EGLDeviceEXT d, __EGLvendorInfo *v)
{
	(void)d;
	(void)v;
	return EGL_TRUE;
}

static const __EGLapiExports EXPORTS = {
	.threadInit = x_thread_init,       .getCurrentApi = x_api,
	.getCurrentVendor = x_vendor,      .getCurrentContext = x_ctx,
	.getCurrentDisplay = x_dpy,        .getCurrentSurface = x_surf,
	.fetchDispatchEntry = x_fetch,     .setEGLError = x_err,
	.setLastVendor = x_last,           .getVendorFromDisplay = x_vdpy,
	.getVendorFromDevice = x_vdev,     .setVendorForDevice = x_setdev,
};
static char vendor_cookie[64];

/* An EGL or GL entry point, through whichever EGL the mode names. */
static void *proc(const char *name)
{
	if (mesa_mode) {
		if (!mesa_ready) {
			void *m = dlopen("libEGL_mesa.so.0", RTLD_NOW | RTLD_LOCAL);
			if (!m) {
				printf("  no libEGL_mesa.so.0: %s\n", dlerror());
				exit(2);
			}
			typedef EGLBoolean (*MainFn)(uint32_t, const __EGLapiExports *,
										 __EGLvendorInfo *,
										 __EGLapiImports *);
			MainFn main_fn = (MainFn)dlsym(m, "__egl_Main");
			if (!main_fn ||
				!main_fn(EGL_VENDOR_ABI_VERSION, &EXPORTS,
						 (__EGLvendorInfo *)vendor_cookie, &imports)) {
				printf("  __egl_Main refused\n");
				exit(2);
			}
			mesa_ready = 1;
			printf("  Mesa's vendor library driven directly\n");
		}
		return imports.getProcAddress(name);
	}
	if (!glvnd) {
		glvnd = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
		if (!glvnd) {
			printf("  no libEGL.so.1\n");
			exit(2);
		}
	}
	void *p = dlsym(glvnd, name);
	if (!p) {
		PFNEGLGETPROCADDRESSPROC gpa =
			(PFNEGLGETPROCADDRESSPROC)dlsym(glvnd, "eglGetProcAddress");
		p = (void *)gpa(name);
	}
	return p;
}

#define P(type, name) ((type)proc(#name))

/* The first display device of `vendor` in sysfs, its address into `out`. */
static void find_pci(const char *vendor, char *out, size_t size)
{
	DIR *d = opendir("/sys/bus/pci/devices");
	const struct dirent *e;
	char best[32] = "";
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char path[300], v[16] = "", c[16] = "";
		snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/vendor",
				 e->d_name);
		FILE *f = fopen(path, "r");
		if (f) {
			if (!fgets(v, sizeof v, f))
				v[0] = 0;
			fclose(f);
		}
		snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/class",
				 e->d_name);
		f = fopen(path, "r");
		if (f) {
			if (!fgets(c, sizeof c, f))
				c[0] = 0;
			fclose(f);
		}
		if (strncmp(v, vendor, strlen(vendor)) == 0 &&
			strncmp(c, "0x03", 4) == 0 &&
			(!best[0] || strcmp(e->d_name, best) < 0))
			snprintf(best, sizeof best, "%.31s", e->d_name);
	}
	if (d)
		closedir(d);
	snprintf(out, size, "%s", best);
}

static void state(void)
{
	static const char *const LIBS[] = {
		"libEGL.so.1", "libEGL_mesa", "libEGL_nvidia", "libnvidia-eglcore",
		"libnvidia-glcore", "libGLX_nvidia", "libvulkan.so", "libgallium",
	};
	enum { N = sizeof LIBS / sizeof LIBS[0] };
	char line[1024];
	int seen[N] = {0};
	FILE *f = fopen("/proc/self/maps", "r");
	while (f && fgets(line, sizeof line, f))
		for (size_t i = 0; i < N; i++)
			if (strstr(line, LIBS[i]))
				seen[i] = 1;
	if (f)
		fclose(f);
	printf("  mapped:");
	for (size_t i = 0; i < N; i++)
		if (seen[i])
			printf(" %s", LIBS[i]);
	printf("\n  open:");
	DIR *d = opendir("/proc/self/fd");
	const struct dirent *e;
	int any = 0;
	while (d && (e = readdir(d))) {
		char p[300], t[PATH_MAX];
		snprintf(p, sizeof p, "/proc/self/fd/%s", e->d_name);
		const ssize_t n = readlink(p, t, sizeof t - 1);
		if (n <= 0)
			continue;
		t[n] = 0;
		if (!strncmp(t, "/dev/nvidia", 11) || !strncmp(t, "/dev/dri", 8)) {
			printf(" %s", t);
			any = 1;
		}
	}
	if (d)
		closedir(d);
	printf("%s\n", any ? "" : " nothing of a GPU");
}

static void node_of(const char *pci, const char *kind, char *out, size_t n)
{
	char link[128];
	snprintf(link, sizeof link, "/dev/dri/by-path/pci-%s-%s", pci, kind);
	if (!realpath(link, out))
		snprintf(out, n, "(none)");
}

/* The first EGL device on `which` GPU's nodes, the devices printed where
 * `which` is NULL. Mesa lists a nameless device for a card NVIDIA's module
 * drives, after NVIDIA's own, which is why the first is taken. */
static EGLDeviceEXT find_device(const char *which)
{
	PFNEGLQUERYDEVICESEXTPROC qd =
		P(PFNEGLQUERYDEVICESEXTPROC, eglQueryDevicesEXT);
	PFNEGLQUERYDEVICESTRINGEXTPROC ds =
		P(PFNEGLQUERYDEVICESTRINGEXTPROC, eglQueryDeviceStringEXT);
	EGLDeviceEXT devs[16];
	EGLint n = 0;
	if (!qd || !ds || !qd(16, devs, &n)) {
		printf("  no device query\n");
		return 0;
	}
	const char *pci = which && !strcmp(which, "intel") ? intel_pci
													   : nvidia_pci;
	char card[PATH_MAX], render[PATH_MAX];
	node_of(pci, "card", card, sizeof card);
	node_of(pci, "render", render, sizeof render);
	EGLDeviceEXT hit = 0;
	for (EGLint i = 0; i < n; i++) {
		const char *ext = ds(devs[i], EGL_EXTENSIONS);
		const char *file = ext && strstr(ext, "EGL_EXT_device_drm")
							   ? ds(devs[i], EGL_DRM_DEVICE_FILE_EXT)
							   : NULL;
		const char *rn = ext && strstr(ext, "EGL_EXT_device_drm_render_node")
							 ? ds(devs[i], EGL_DRM_RENDER_NODE_FILE_EXT)
							 : NULL;
		const char *name = ext && strstr(ext, "EGL_EXT_device_query_name")
							   ? ds(devs[i], EGL_RENDERER_EXT_ENUM)
							   : NULL;
		if (!which)
			printf("  device %d: %s, %s, %s\n", i,
				   name ? name : "(no name)", file ? file : "-",
				   rn ? rn : "-");
		if (which && !hit &&
			((file && !strcmp(file, card)) || (rn && !strcmp(rn, render))))
			hit = devs[i];
	}
	return hit;
}

static void gl(const char *which)
{
	PFNEGLGETPLATFORMDISPLAYPROC gpd =
		P(PFNEGLGETPLATFORMDISPLAYPROC, eglGetPlatformDisplay);
	EGLDisplay dpy;
	if (!strcmp(which, "surfaceless")) {
		dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	} else {
		const EGLDeviceEXT dev = find_device(which);
		if (!dev) {
			printf("  no EGL device for %s\n", which);
			return;
		}
		dpy = gpd(EGL_PLATFORM_DEVICE_EXT, dev, NULL);
	}
	EGLint ma, mi;
	if (dpy == EGL_NO_DISPLAY ||
		!P(PFNEGLINITIALIZEPROC, eglInitialize)(dpy, &ma, &mi)) {
		printf("  no display\n");
		return;
	}
	printf("  EGL %d.%d, %s\n", ma, mi,
		   P(PFNEGLQUERYSTRINGPROC, eglQueryString)(dpy, EGL_VENDOR));
	P(PFNEGLBINDAPIPROC, eglBindAPI)(EGL_OPENGL_API);
	const EGLint attrs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
		EGL_NONE,
	};
	EGLContext c = P(PFNEGLCREATECONTEXTPROC, eglCreateContext)(
		dpy, (EGLConfig)0, EGL_NO_CONTEXT, attrs);
	if (c == EGL_NO_CONTEXT) {
		printf("  no 4.5 core context (0x%x)\n",
			   P(PFNEGLGETERRORPROC, eglGetError)());
	} else {
		PFNEGLMAKECURRENTPROC mc = P(PFNEGLMAKECURRENTPROC, eglMakeCurrent);
		if (mc(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
			typedef const char *(*GetStringFn)(unsigned);
			GetStringFn gs = P(GetStringFn, glGetString);
			printf("  GL: %s | %s\n", gs(GL_RENDERER_ENUM),
				   gs(GL_VERSION_ENUM));
			mc(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		} else {
			printf("  not made current\n");
		}
		P(PFNEGLDESTROYCONTEXTPROC, eglDestroyContext)(dpy, c);
	}
	P(PFNEGLTERMINATEPROC, eglTerminate)(dpy);
	P(PFNEGLRELEASETHREADPROC, eglReleaseThread)();
}

/* A display and a context kept across steps, for a context made current
 * on a thread that lived before the library was opened again (cur,
 * thread:cur). */
static EGLDisplay held_dpy = EGL_NO_DISPLAY;
static EGLContext held_ctx = EGL_NO_CONTEXT;

static void cur(void)
{
	if (held_ctx == EGL_NO_CONTEXT) {
		printf("  nothing held\n");
		return;
	}
	P(PFNEGLBINDAPIPROC, eglBindAPI)(EGL_OPENGL_API);
	PFNEGLMAKECURRENTPROC mc = P(PFNEGLMAKECURRENTPROC, eglMakeCurrent);
	if (mc(held_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, held_ctx)) {
		typedef const char *(*GetStringFn)(unsigned);
		printf("  current on thread %lx: %s\n", (unsigned long)pthread_self(),
			   P(GetStringFn, glGetString)(GL_RENDERER_ENUM));
		mc(held_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	} else {
		printf("  not made current on thread %lx: 0x%x\n",
			   (unsigned long)pthread_self(),
			   P(PFNEGLGETERRORPROC, eglGetError)());
	}
	P(PFNEGLRELEASETHREADPROC, eglReleaseThread)();
}

static void *cur_thread(void *arg)
{
	(void)arg;
	cur();
	return NULL;
}

static void hold(const char *which)
{
	PFNEGLGETPLATFORMDISPLAYPROC gpd =
		P(PFNEGLGETPLATFORMDISPLAYPROC, eglGetPlatformDisplay);
	const EGLDeviceEXT dev = find_device(which);
	if (!dev) {
		printf("  no EGL device for %s\n", which);
		return;
	}
	held_dpy = gpd(EGL_PLATFORM_DEVICE_EXT, dev, NULL);
	EGLint ma, mi;
	if (held_dpy == EGL_NO_DISPLAY ||
		!P(PFNEGLINITIALIZEPROC, eglInitialize)(held_dpy, &ma, &mi)) {
		printf("  no display\n");
		held_dpy = EGL_NO_DISPLAY;
		return;
	}
	printf("  EGL %d.%d, %s\n", ma, mi,
		   P(PFNEGLQUERYSTRINGPROC, eglQueryString)(held_dpy, EGL_VENDOR));
	P(PFNEGLBINDAPIPROC, eglBindAPI)(EGL_OPENGL_API);
	const EGLint attrs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
		EGL_NONE,
	};
	held_ctx = P(PFNEGLCREATECONTEXTPROC, eglCreateContext)(
		held_dpy, (EGLConfig)0, EGL_NO_CONTEXT, attrs);
	if (held_ctx == EGL_NO_CONTEXT) {
		printf("  no 4.5 core context (0x%x)\n",
			   P(PFNEGLGETERRORPROC, eglGetError)());
		return;
	}
	cur();
}

static void unhold(void)
{
	if (held_ctx != EGL_NO_CONTEXT)
		P(PFNEGLDESTROYCONTEXTPROC, eglDestroyContext)(held_dpy, held_ctx);
	if (held_dpy != EGL_NO_DISPLAY)
		P(PFNEGLTERMINATEPROC, eglTerminate)(held_dpy);
	P(PFNEGLRELEASETHREADPROC, eglReleaseThread)();
	held_ctx = EGL_NO_CONTEXT;
	held_dpy = EGL_NO_DISPLAY;
}

static void vk(void)
{
	static void *lib;
	if (!lib)
		lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		printf("  no libvulkan.so.1\n");
		return;
	}
	PFN_vkGetInstanceProcAddr gipa =
		(PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	PFN_vkCreateInstance ci =
		(PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
	};
	VkInstance inst;
	if (ci(&info, NULL, &inst)) {
		printf("  no instance\n");
		return;
	}
	PFN_vkEnumeratePhysicalDevices ep = (PFN_vkEnumeratePhysicalDevices)
		gipa(inst, "vkEnumeratePhysicalDevices");
	PFN_vkGetPhysicalDeviceProperties gp = (PFN_vkGetPhysicalDeviceProperties)
		gipa(inst, "vkGetPhysicalDeviceProperties");
	uint32_t n = 8;
	VkPhysicalDevice pd[8];
	ep(inst, &n, pd);
	for (uint32_t i = 0; i < n; i++) {
		VkPhysicalDeviceProperties p;
		gp(pd[i], &p);
		printf("  vulkan: %s\n", p.deviceName);
	}
	printf("  with the instance open:\n");
	state();
	((PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance"))(inst, NULL);
}

static void move(const char *driver)
{
	const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", SOCKET_PATH);
	if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
		perror("  " SOCKET_PATH);
		if (s >= 0)
			close(s);
		return;
	}
	char req[64];
	const int len = snprintf(req, sizeof req, "%s %s\n", driver, nvidia_pci);
	if (send(s, req, (size_t)len, MSG_NOSIGNAL) != len)
		perror("  send");
	char buf[4096];
	ssize_t r;
	while ((r = read(s, buf, sizeof buf - 1)) > 0) {
		buf[r] = 0;
		for (char *ln = strtok(buf, "\n"); ln; ln = strtok(NULL, "\n"))
			printf("  helper: %s\n", ln);
	}
	close(s);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	find_pci("0x10de", nvidia_pci, sizeof nvidia_pci);
	find_pci("0x8086", intel_pci, sizeof intel_pci);
	printf("NVIDIA %s, Intel %s\n", nvidia_pci[0] ? nvidia_pci : "none",
		   intel_pci[0] ? intel_pci : "none");
	for (int i = 1; i < argc; i++) {
		const char *s = argv[i];
		printf("== %s\n", s);
		if (!strcmp(s, "mode=mesa")) {
			mesa_mode = 1;
		} else if (!strcmp(s, "mode=glvnd")) {
			mesa_mode = 0;
		} else if (!strcmp(s, "zink")) {
			setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
		} else if (!strcmp(s, "query")) {
			find_device(NULL);
		} else if (!strncmp(s, "gl:", 3)) {
			gl(s + 3);
		} else if (!strcmp(s, "unload")) {
			printf("  dlclose: %d\n", glvnd ? dlclose(glvnd) : -1);
			glvnd = NULL;
		} else if (!strncmp(s, "hold:", 5)) {
			hold(s + 5);
		} else if (!strcmp(s, "cur")) {
			cur();
		} else if (!strcmp(s, "thread:cur")) {
			pthread_t t;
			if (pthread_create(&t, NULL, cur_thread, NULL) == 0)
				pthread_join(t, NULL);
		} else if (!strcmp(s, "unhold")) {
			unhold();
		} else if (!strcmp(s, "vk")) {
			vk();
		} else if (!strncmp(s, "move:", 5)) {
			move(s + 5);
		} else {
			printf("  no such step\n");
			continue;
		}
		state();
	}
	return 0;
}

/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The EGL loader: libEGL.so.1 opened and every pointer of egl_dyn.h filled
 * from it, under holds -- the OpenGL renderer's instance has one, and each
 * listing of its GPUs another.
 *
 * The name with its soname's version, as the runtime package installs it:
 * a bare libEGL.so is the development package's link, absent where nothing
 * is built. A library of EGL 1.4, without eglGetPlatformDisplay and
 * eglCreatePlatformWindowSurface, is refused here by the missing name, as
 * egl.c would refuse it by its version.
 *
 * Closed with the last hold where an NVIDIA GPU may be moved to another
 * kernel driver (FNL_GPU_DRIVER), and nowhere else: the one runtime the
 * library closes (core/dynlib.h). glvnd loads NVIDIA's EGL beside Mesa's,
 * and with it loaded NVIDIA's driver in this process is fixed as it was:
 * on NVIDIA's module it holds the GPU (/dev/nvidia0 and nvidiactl, from the
 * first device query, eglTerminate() or not), and loaded with the GPU on
 * nouveau it holds nvidiactl and leaves NVIDIA's Vulkan driver blind to
 * the GPU once it is moved -- that driver finds its GPU through glvnd's
 * vendors. glvnd closed takes every vendor with it and lets go of all of
 * it; opened again, it starts afresh, as each Vulkan instance's does (the
 * loader closes NVIDIA's ICD and glvnd beneath it at vkDestroyInstance).
 * Measured on schenker (glvnd 1.7.0, Mesa 26.2.5, NVIDIA 580.196,
 * tools/probe/eglmove.c): NVIDIA's own OpenGL drawn, glvnd closed, the GPU
 * moved to nouveau and back, and NVIDIA's OpenGL and Vulkan both found it
 * after. glvnd's destructor drops its thread key and every thread's state,
 * and Mesa's EGL keeps its per-thread state in a plain thread-local, so a
 * thread that called EGL and ends after the close runs no code of either.
 *
 * What is closed is what holds the driver, and not what took the static
 * TLS block: a library whose thread-locals live there (DF_STATIC_TLS --
 * libGLdispatch, Mesa's EGL and gallium) gets its room from a surplus
 * glibc reuses only from the end, and the Vulkan loader's own loads of
 * NVIDIA's ICD in between leave each room freed in the middle. On
 * schenker, ten moves from OpenGL in -- two closings each -- there was no
 * room left: glvnd then loaded no vendor that needed it, and OpenGL listed
 * one GPU, then none. The libraries EGL's own spell brought in are kept
 * (pin_static_tls(): those with static TLS mapped at the close and not at
 * the open), each loaded once for the process's life, and none of them
 * holds a GPU: Mesa's lets go at eglTerminate(). Twenty moves from OpenGL
 * passed with the surplus made large instead
 * (GLIBC_TUNABLES=glibc.rtld.optional_static_tls), which a library cannot
 * set for itself: glibc reads it at the process's start; and with the
 * libraries kept.
 *
 * Not kept: what another loader has mapped, which is the Vulkan loader's
 * drivers and layers. NVIDIA's ICD (libGLX_nvidia.so.0) has static TLS
 * too, and is mapped at a close whenever Vulkan draws while OpenGL's GPUs
 * are listed; kept, it would stay for the process's life, where the move
 * rests on each Vulkan instance starting NVIDIA's userspace afresh. The
 * loader's drivers load and unload as one, so they take their room at the
 * surplus's end and give it back there: twenty moves from OpenGL passed
 * with them left out.
 */

/* dl_iterate_phdr(), RTLD_NOLOAD and RTLD_NODELETE are glibc's, asked for
 * ahead of app.h's narrower pin; this file is Linux's alone, as the OpenGL
 * renderer is. */
#define _GNU_SOURCE

#include "core/app.h"

#include "core/dynlib.h"
#include "renderer/gl/egl_dyn.h"

#include <dlfcn.h>
#include <link.h>
#include <pthread.h>

#define FNL_EGL_DEFINE(f) __typeof__(fnl_##f) fnl_##f = NULL;
FNL_EGL_FUNCS(FNL_EGL_DEFINE)
#undef FNL_EGL_DEFINE

/* The library while a hold has it open; how many holds; and a load that
 * failed, with why, which every later load answers with: a library or a
 * name missing once is missing for the process's life. Under a lock of
 * their own, since each App's fnl_init() runs on a thread of the caller's
 * choosing and the listing runs on one of the library's. */
static pthread_mutex_t load_lock = PTHREAD_MUTEX_INITIALIZER;
static void           *load_lib;
static int             load_holds;
static bool            load_failed;
static char            load_why[256];

/* The libraries mapped now whose thread-locals are in the static TLS
 * block, by path: gathered at the open (`at_open`) and at the close, and
 * what the close has beyond the open is kept loaded for the process's
 * life (above). Gathered first and pinned after: dlopen() is not to be
 * called from inside the walk. */
#define PIN_MAX 32

typedef struct {
	char names[PIN_MAX][512];
	int  n;
} Pins;

static Pins at_open;

static int pin_gather(struct dl_phdr_info *info, size_t size, void *data)
{
	(void)size;
	Pins *p = data;
	if (!info->dlpi_name || !info->dlpi_name[0] || p->n == PIN_MAX)
		return 0;
	bool tls = false;
	const ElfW(Dyn) *dyn = NULL;
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
		if (ph->p_type == PT_TLS && ph->p_memsz > 0)
			tls = true;
		else if (ph->p_type == PT_DYNAMIC)
			dyn = (const ElfW(Dyn) *)(info->dlpi_addr + ph->p_vaddr);
	}
	if (!tls || !dyn)
		return 0;
	for (; dyn->d_tag != DT_NULL; dyn++)
		if (dyn->d_tag == DT_FLAGS && (dyn->d_un.d_val & DF_STATIC_TLS)) {
			snprintf(p->names[p->n++], sizeof p->names[0], "%s",
					 info->dlpi_name);
			break;
		}
	return 0;
}

static void load(void)
{
	at_open.n = 0;
	dl_iterate_phdr(pin_gather, &at_open);
	static const char *const NAMES[] = { "libEGL.so.1", NULL };
	char err[200];
	void *lib = fnl_lib_open(NAMES, err, sizeof err);
	if (!lib) {
		snprintf(load_why, sizeof load_why, "no libEGL.so.1: %s", err);
		load_failed = true;
		return;
	}
	/* A name missing leaves the library open, as it was before there were
	 * holds: nothing of it was called. */
#define FNL_EGL_RESOLVE(f)                                                    \
	fnl_##f = (__typeof__(fnl_##f))fnl_lib_sym(lib, #f);                      \
	if (!fnl_##f) {                                                           \
		snprintf(load_why, sizeof load_why, "libEGL.so.1 has no %s", #f);     \
		load_failed = true;                                                   \
		return;                                                               \
	}
	FNL_EGL_FUNCS(FNL_EGL_RESOLVE)
#undef FNL_EGL_RESOLVE
	load_lib = lib;
}

FnlResult egl_dyn_load(void)
{
	pthread_mutex_lock(&load_lock);
	if (!load_lib && !load_failed)
		load();
	const bool ok = load_lib != NULL;
	if (ok)
		load_holds++;
	pthread_mutex_unlock(&load_lock);
	if (!ok)
		return fnl_fail(FNL_ERR_NO_DEVICE, "%s", load_why);
	return FNL_OK;
}

bool egl_dyn_hold_open(void)
{
	pthread_mutex_lock(&load_lock);
	const bool ok = load_lib != NULL && load_holds > 0;
	if (ok)
		load_holds++;
	pthread_mutex_unlock(&load_lock);
	return ok;
}

static bool pinned_at_open(const char *name)
{
	for (int i = 0; i < at_open.n; i++)
		if (strcmp(at_open.names[i], name) == 0)
			return true;
	return false;
}

/* RTLD_NOLOAD names one already loaded and RTLD_NODELETE keeps it; the
 * handle is never closed, which a kept library has no use for. */
static void pin_static_tls(void)
{
	static Pins p;
	p.n = 0;
	dl_iterate_phdr(pin_gather, &p);
	for (int i = 0; i < p.n; i++)
		if (!pinned_at_open(p.names[i]))
			(void)dlopen(p.names[i],
						 RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE);
}

void egl_dyn_release(bool close)
{
	pthread_mutex_lock(&load_lock);
	FNL_ASSERT(load_holds > 0);
	if (load_holds > 0)
		load_holds--;
	if (load_holds == 0 && close && load_lib) {
#define FNL_EGL_FORGET(f) fnl_##f = NULL;
		FNL_EGL_FUNCS(FNL_EGL_FORGET)
#undef FNL_EGL_FORGET
		pin_static_tls();
		fnl_lib_close(load_lib);
		load_lib = NULL;
	}
	pthread_mutex_unlock(&load_lock);
}

/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Vulkan loader, opened at run time, and every pointer of vk_fns.h
 * filled from it. The library is never closed (core/dynlib.h).
 *
 * Only vkGetInstanceProcAddr is taken by name from the library; everything
 * else is asked of it, as the specification has an application do. That is
 * also what lets the Mac go without a loader: MoltenVK exports
 * vkGetInstanceProcAddr itself, and is found beside the program, where
 * build.sh puts it. A loader beside it comes first: a tree has none, and
 * one is put there for a checker round, to bring the validation layer
 * (tools/vulkan_macos.py), MoltenVK then its driver by its manifest.
 *
 * The name with its soname's version first, as the runtime package
 * installs it; a bare libvulkan.so is the development package's link, kept
 * as a last try for a loader installed by hand.
 */

#include "core/app.h"

#include "core/dynlib.h"
#include "renderer/vulkan/vk_internal.h"

#define X_DEFINE(f) __typeof__(fnl_##f) fnl_##f;
__typeof__(fnl_vkGetInstanceProcAddr) fnl_vkGetInstanceProcAddr;
VK_GLOBAL_FNS(X_DEFINE)
VK_INSTANCE_FNS(X_DEFINE)
#undef X_DEFINE

static const char *const LOADER_NAMES[] = {
#if defined(_WIN32)
	"vulkan-1.dll",
#elif defined(__APPLE__)
	"@executable_path/libvulkan.1.dylib",
	"@executable_path/libMoltenVK.dylib",
	"libvulkan.1.dylib",
	"libMoltenVK.dylib",
#else
	"libvulkan.so.1",
	"libvulkan.so",
#endif
	NULL,
};

/* From make_instance(), on three threads: fnl_init()'s and the render
 * thread's (renderer_swap()) for the renderer's own instance, and the
 * listing thread's for vk_list_gpus()'s while another renderer draws. A
 * plain static remembers the answer, refusal included, because no two run
 * at once: the first two are one thread at a time, and the listing makes
 * its instance only while Vulkan is not drawing and publishes Vulkan's
 * entries, which a move to Vulkan is made from, only once that instance
 * is gone (vk_list_gpus()). */
FnlResult vk_load_global(void)
{
	static int  state;      /* 0 untried, 1 loaded, -1 refused */
	static char reason[256];
	/* A refusal is said the first time and only recorded after: every
	 * later ask would say the same line again. */
	if (state)
		return state > 0 ? FNL_OK
						 : fnl_fail_quiet(FNL_ERR_NO_DEVICE, "%s", reason);

	char why[200];
	void *lib = fnl_lib_open(LOADER_NAMES, why, sizeof why);
	state = -1;
	if (!lib) {
		snprintf(reason, sizeof reason, "no Vulkan loader: %s", why);
		return fnl_fail(FNL_ERR_NO_DEVICE, "%s", reason);
	}
	fnl_vkGetInstanceProcAddr = (__typeof__(fnl_vkGetInstanceProcAddr))
		fnl_lib_sym(lib, "vkGetInstanceProcAddr");
	if (!fnl_vkGetInstanceProcAddr) {
		snprintf(reason, sizeof reason, "no Vulkan loader: the library "
				 "found exports no vkGetInstanceProcAddr");
		return fnl_fail(FNL_ERR_NO_DEVICE, "%s", reason);
	}
	/* The first entry missing is the one named; the pointers already
	 * filled are never called, since the answer stays no. */
#define X_GLOBAL(f)                                                           \
	fnl_##f = (__typeof__(fnl_##f))vkGetInstanceProcAddr(NULL, #f);           \
	if (!fnl_##f) {                                                           \
		snprintf(reason, sizeof reason, "no Vulkan loader: the library "      \
				 "has no %s", #f);                                            \
		return fnl_fail(FNL_ERR_NO_DEVICE, "%s", reason);                     \
	}
	VK_GLOBAL_FNS(X_GLOBAL)
#undef X_GLOBAL
	state = 1;
	return FNL_OK;
}

/* Asked of the instance every time, never remembered: each instance's
 * answers are its own, and the device's entries come back as the loader's
 * trampolines, which find each device's driver through the handle. */
FnlResult vk_load_instance(VkInstance instance)
{
#define X_INSTANCE(f)                                                         \
	fnl_##f = (__typeof__(fnl_##f))vkGetInstanceProcAddr(instance, #f);       \
	if (!fnl_##f)                                                             \
		return fnl_fail(FNL_ERR_NO_DEVICE, "the Vulkan loader has no %s",     \
						#f);
	VK_INSTANCE_FNS(X_INSTANCE)
#undef X_INSTANCE
	return FNL_OK;
}

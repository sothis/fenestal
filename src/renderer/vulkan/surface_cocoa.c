/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Cocoa-to-Vulkan bridge, the sibling of surface_x11.c and
 * surface_win32.c beside it: the same two functions over the one handle a
 * Mac has, the CAMetalLayer that backs the window's view. Vulkan on a Mac is
 * MoltenVK, on Metal, and VK_EXT_metal_surface is how it is handed the
 * layer.
 *
 * C, where the rest of the Cocoa backend is Objective-C: the layer crosses
 * cocoa_state.h as a `void *`, and vulkan_metal.h spells CAMetalLayer as
 * void outside Objective-C.
 */

#define VK_USE_PLATFORM_METAL_EXT

#include "core/app.h"

#include "renderer/vulkan/vk_internal.h"

#include "backend/cocoa/cocoa_state.h"

#include <vulkan/vulkan_metal.h>

#include <mach/mach_time.h>

/* The surface's, required; and what VK_EXT_present_timing wants of the
 * instance, asked for where offered (driver_stamps()), whose bit in
 * RendererState::backend_ext_on is EXT_CAPS2. */
#define EXT_CAPS2 0x1u

static const char *const *instance_extensions(uint32_t *count,
											  uint32_t *required)
{
	static const char *const EXTS[] = {
		VK_EXT_METAL_SURFACE_EXTENSION_NAME,
		VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
	};
	*required = 1;
	*count = 2;
	return EXTS;
}

/* Built into a local and assigned only on success, for surface_x11.c's
 * reason. The layer is the backend's, made with the view, and outlives
 * every surface made on it. */
static FnlResult create_surface(App *a)
{
	VkMetalSurfaceCreateInfoEXT ci = {
		.sType  = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
		.pLayer = a->backend_state->layer,
	};
	VkSurfaceKHR s = VK_NULL_HANDLE;
	/* Asked of the instance: vk_fns.h sees no window server's header. */
	PFN_vkCreateMetalSurfaceEXT make = (PFN_vkCreateMetalSurfaceEXT)
		vkGetInstanceProcAddr(VK(a)->instance, "vkCreateMetalSurfaceEXT");
	if (!make)
		return fnl_fail(FNL_ERR_NO_DEVICE, "the Vulkan loader has no "
						"vkCreateMetalSurfaceEXT");
	VK_TRY(make(VK(a)->instance, &ci, NULL, &s));
	VK(a)->vk_surface = s;
	return FNL_OK;
}

/* Nothing of the Cocoa backend's says when a frame was shown -- Metal's
 * renderer reads its drawables, which MoltenVK keeps -- so the driver is
 * asked, by VK_EXT_present_timing wherever the instance has what that
 * rests on, and else by VK_GOOGLE_display_timing, which needs none of it
 * (create_device()). */
static bool driver_stamps(App *a)
{
	return (VK(a)->backend_ext_on & EXT_CAPS2) != 0;
}

/* MoltenVK's present time is the Metal drawable's presentedTime in
 * nanoseconds, mach_absolute_time()'s clock, which stops while the machine
 * sleeps where CLOCK_MONOTONIC does not; moved across by the two read
 * together. */
static double driver_clock(App *a, uint64_t ns)
{
	(void)a;
	mach_timebase_info_data_t tb;
	if (mach_timebase_info(&tb) != KERN_SUCCESS || !tb.denom)
		return -1.0;
	const double up = (double)mach_absolute_time() * tb.numer / tb.denom *
					  1e-9;
	return (double)ns * 1e-9 + (now_seconds() - up);
}

/* Covered: AppKit's occlusion state, which the backend keeps where the
 * render thread may read it. MoltenVK's layer, like Metal's, still hands
 * out drawables there that the window server never shows, or none for a
 * second at a time once they are spent (metal/surface_cocoa.m). */
static bool occluded(const App *a)
{
	return a->backend_state &&
		   fnl_atomic_load_bool(&a->backend_state->occluded);
}

const VkBridge vk_bridge_cocoa = {
	.backend            = BACKEND_COCOA,
	.instance_extensions = instance_extensions,
	.create_surface      = create_surface,
	.driver_stamps       = driver_stamps,
	.driver_clock        = driver_clock,
	.occluded            = occluded,
};

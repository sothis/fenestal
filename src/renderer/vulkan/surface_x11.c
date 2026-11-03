/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The X11-to-Vulkan bridge, and the sibling of surface_wayland.c and
 * surface_win32.c beside it: the same two functions over different handles.
 *
 * BackendState::gfx and never ::conn. Mesa's WSI blocks in the connection it
 * is handed, and the pump's must have one reader -- see x11_state.h.
 */

#define VK_USE_PLATFORM_XCB_KHR

#include "core/app.h"

#include "renderer/vulkan/vk_internal.h"

#include "backend/x11/x11_state.h"

#include <vulkan/vulkan_xcb.h>

static const char *const *instance_extensions(uint32_t *count,
											  uint32_t *required)
{
	static const char *const EXTS[] = { VK_KHR_XCB_SURFACE_EXTENSION_NAME };
	*count = *required = 1;
	return EXTS;
}

/* Built into a local and assigned only on success. Vulkan leaves an output
 * handle undefined on failure, and recover_from_loss() comes back to this
 * builder after destroying the surface: a handle left holding whatever the
 * driver wrote would be destroyed a second time. */
static FnlResult create_surface(App *a)
{
	VkXcbSurfaceCreateInfoKHR ci = {
		.sType      = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
		.connection = a->backend_state->gfx,
		.window     = a->backend_state->window,
	};
	VkSurfaceKHR s = VK_NULL_HANDLE;
	/* Asked of the instance: vk_fns.h sees no window server's header. */
	PFN_vkCreateXcbSurfaceKHR make = (PFN_vkCreateXcbSurfaceKHR)
		vkGetInstanceProcAddr(VK(a)->instance, "vkCreateXcbSurfaceKHR");
	if (!make)
		return fnl_fail(FNL_ERR_NO_DEVICE, "the Vulkan loader has no "
						"vkCreateXcbSurfaceKHR");
	VK_TRY(make(VK(a)->instance, &ci, NULL, &s));
	VK(a)->vk_surface = s;
	return FNL_OK;
}

/* Of the window's own visual, the root's (egl_x11.c says why). */
static bool presents(App *a, VkInstance inst, VkPhysicalDevice dev,
					 uint32_t family)
{
	PFN_vkGetPhysicalDeviceXcbPresentationSupportKHR ask =
		(PFN_vkGetPhysicalDeviceXcbPresentationSupportKHR)
			vkGetInstanceProcAddr(
				inst,
				"vkGetPhysicalDeviceXcbPresentationSupportKHR");
	return ask && ask(dev, family, a->backend_state->gfx,
					  a->backend_state->screen->root_visual);
}

const VkBridge vk_bridge_x11 = {
	.backend            = BACKEND_X11,
	.instance_extensions = instance_extensions,
	.create_surface      = create_surface,
	.presents            = presents,
};

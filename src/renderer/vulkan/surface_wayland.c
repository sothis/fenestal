/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Wayland-to-Vulkan bridge: the one file that sees <wayland-client.h> and
 * <vulkan/vulkan.h> at once, and the only one that defines
 * VK_USE_PLATFORM_WAYLAND_KHR. The surface belongs to the renderer and dies
 * with it; recovery from a lost one remakes it while the window stays
 * mapped.
 */

#define VK_USE_PLATFORM_WAYLAND_KHR

#include "core/app.h"

#include "renderer/vulkan/vk_internal.h"

#include "backend/wayland/wayland_state.h"

#include <vulkan/vulkan_wayland.h>

static const char *const *instance_extensions(uint32_t *count,
											  uint32_t *required)
{
	static const char *const EXTS[] = { VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME };
	*count = *required = 1;
	return EXTS;
}

/* Built into a local and assigned only on success: Vulkan leaves the handle
 * undefined on failure, and recover_from_loss() retries through here. */
static FnlResult create_surface(App *a)
{
	VkWaylandSurfaceCreateInfoKHR ci = {
		.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
		.display = a->backend_state->display,
		.surface = a->backend_state->present,
	};
	VkSurfaceKHR s = VK_NULL_HANDLE;
	/* Asked of the instance: vk_fns.h sees no window server's header. */
	PFN_vkCreateWaylandSurfaceKHR make = (PFN_vkCreateWaylandSurfaceKHR)
		vkGetInstanceProcAddr(VK(a)->instance, "vkCreateWaylandSurfaceKHR");
	if (!make)
		return fnl_fail(FNL_ERR_NO_DEVICE, "the Vulkan loader has no "
						"vkCreateWaylandSurfaceKHR");
	VK_TRY(make(VK(a)->instance, &ci, NULL, &s));
	VK(a)->vk_surface = s;
	return FNL_OK;
}

static bool presents(App *a, VkInstance inst, VkPhysicalDevice dev,
					 uint32_t family)
{
	PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR ask =
		(PFN_vkGetPhysicalDeviceWaylandPresentationSupportKHR)
			vkGetInstanceProcAddr(
				inst,
				"vkGetPhysicalDeviceWaylandPresentationSupportKHR");
	return ask && ask(dev, family, a->backend_state->display);
}

const VkBridge vk_bridge_wayland = {
	.backend            = BACKEND_WAYLAND,
	.instance_extensions = instance_extensions,
	.create_surface      = create_surface,
	.presents            = presents,
};

/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Win32-to-Vulkan bridge: the one file that sees both this window server
 * and the graphics API.
 */

#define VK_USE_PLATFORM_WIN32_KHR

#include "core/app.h"

#include "renderer/vulkan/vk_internal.h"

#include "backend/win32/win32_state.h"

#include <vulkan/vulkan_win32.h>

/* The surface's, required; and the one that tells a device's LUID and what
 * it imports, asked for where offered (present_dxgi.c). */
static const char *const *instance_extensions(uint32_t *count,
											  uint32_t *required)
{
	static const char *const EXTS[] = {
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
		"VK_KHR_external_memory_capabilities",
	};
	*required = 1;
	*count = 2;
	return EXTS;
}

/* Built into a local and assigned only on success: Vulkan leaves the output
 * handle undefined on failure, and recover_from_loss() comes back to retry. */
static FnlResult create_surface(App *a)
{
	VkWin32SurfaceCreateInfoKHR ci = {
		.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
		.hinstance = a->backend_state->instance,
		.hwnd      = win32_present_hwnd(a->backend_state),
	};
	VkSurfaceKHR s = VK_NULL_HANDLE;
	/* Asked of the instance: vk_fns.h sees no window server's header. */
	PFN_vkCreateWin32SurfaceKHR make = (PFN_vkCreateWin32SurfaceKHR)
		vkGetInstanceProcAddr(VK(a)->instance, "vkCreateWin32SurfaceKHR");
	if (!make)
		return fnl_fail(FNL_ERR_NO_DEVICE, "the Vulkan loader has no "
						"vkCreateWin32SurfaceKHR");
	VK_TRY(make(VK(a)->instance, &ci, NULL, &s));
	VK(a)->vk_surface = s;
	return FNL_OK;
}

static bool presents(App *a, VkInstance inst, VkPhysicalDevice dev,
					 uint32_t family)
{
	(void)a;
	PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR ask =
		(PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR)
			vkGetInstanceProcAddr(
				inst,
				"vkGetPhysicalDeviceWin32PresentationSupportKHR");
	return ask && ask(dev, family);
}

const VkBridge vk_bridge_win32 = {
	.backend            = BACKEND_WIN32,
	.instance_extensions = instance_extensions,
	.create_surface      = create_surface,
	.presents            = presents,
	.own_images          = vk_dxgi_own_images,
	.submit_chain        = vk_dxgi_submit_chain,
	.present_own         = vk_dxgi_present,
	.own_end             = vk_dxgi_end,
	.device_extensions   = vk_dxgi_device_extensions,
	.choose_present      = vk_dxgi_choose,
	.shares              = vk_dxgi_shares,
};

/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* Mesa's display WSI, alone: the leak lsan.supp keys on libvulkan_broadcom
 * for, with nothing of fenestal in it. A display plane surface on the
 * device's first display, its formats asked for, and then `rounds` times a
 * swapchain made on it, destroyed, and the formats asked for again.
 *
 * wsi_display_swapchain_destroy() zeroes the connector's crtc_id as its
 * last swapchain goes, and the next query's wsi_display_setup_crtc() then
 * assigns connector->formats again without freeing the list it had, so
 * under ASan each round should leave one list -- the primary plane's
 * formats, 4 bytes each -- reported at exit. No mode is set and nothing is
 * presented, so no master is needed: run it with no window server up.
 *
 * Built here, for this desktop's processor or the Pi's, and run on a bare
 * machine by tools/probe/bare_run.py --kms:
 *
 *   gcc -std=c11 -g -O1 -fsanitize=address -Wall -Wextra \
 *       -o build/wsidisplay tools/probe/wsidisplay.c -lvulkan
 *   aarch64-linux-gnu-gcc -std=c11 -g -O1 -fsanitize=address -Wall \
 *       -Wextra -o build/wsidisplay-arm64 tools/probe/wsidisplay.c \
 *       -Lbuild/linux-arm64/release/vulkan-stub -lvulkan
 *
 *   wsidisplay [device-name-part [rounds]]   defaults: the first GPU, 3
 *
 * With the driver kept loaded (LD_PRELOAD, after ASan's runtime), the
 * report names its module rather than an unknown one. */
#define VK_USE_PLATFORM_DISPLAY_KHR
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call)                                                       \
	do {                                                                  \
		VkResult r_ = (call);                                             \
		if (r_ != VK_SUCCESS) {                                           \
			fprintf(stderr, "wsidisplay: %s: %d\n", #call, (int)r_);      \
			exit(1);                                                      \
		}                                                                 \
	} while (0)

/* The device whose name has `want` in it, or the first that is not a CPU
 * rasteriser. */
static VkPhysicalDevice pick_device(VkInstance inst, const char *want)
{
	uint32_t n = 0;
	CHECK(vkEnumeratePhysicalDevices(inst, &n, NULL));
	VkPhysicalDevice *all = calloc(n ? n : 1, sizeof *all);
	if (!all)
		exit(1);
	CHECK(vkEnumeratePhysicalDevices(inst, &n, all));
	VkPhysicalDevice got = VK_NULL_HANDLE;
	for (uint32_t i = 0; i < n && !got; i++) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(all[i], &p);
		if (want ? strstr(p.deviceName, want) != NULL
				 : p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
			got = all[i];
			printf("device: %s\n", p.deviceName);
		}
	}
	free(all);
	if (!got) {
		fprintf(stderr, "wsidisplay: no device %s\n", want ? want : "");
		exit(1);
	}
	return got;
}

/* A surface on the device's first display, at its first mode, on a plane
 * that can show it; the mode's size into *extent. */
static VkSurfaceKHR display_surface(VkInstance inst, VkPhysicalDevice dev,
									VkExtent2D *extent)
{
	/* Through the instance, as fenestal reaches it: the ARM tree's stub
	 * loader has only the entry points fenestal's sources name. */
	PFN_vkGetPhysicalDeviceDisplayPropertiesKHR props =
		(PFN_vkGetPhysicalDeviceDisplayPropertiesKHR)vkGetInstanceProcAddr(
			inst, "vkGetPhysicalDeviceDisplayPropertiesKHR");
	if (!props)
		exit(1);
	uint32_t n = 1;
	VkDisplayPropertiesKHR disp;
	VkResult r = props(dev, &n, &disp);
	if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0) {
		fprintf(stderr, "wsidisplay: the device lists no display -- is a "
				"window server holding it?\n");
		exit(1);
	}
	printf("display: %s\n", disp.displayName ? disp.displayName : "?");

	uint32_t m = 1;
	VkDisplayModePropertiesKHR mode;
	r = vkGetDisplayModePropertiesKHR(dev, disp.display, &m, &mode);
	if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || m == 0) {
		fprintf(stderr, "wsidisplay: the display has no mode\n");
		exit(1);
	}

	uint32_t np = 0;
	CHECK(vkGetPhysicalDeviceDisplayPlanePropertiesKHR(dev, &np, NULL));
	VkDisplayPlanePropertiesKHR *planes = calloc(np ? np : 1,
												 sizeof *planes);
	if (!planes)
		exit(1);
	CHECK(vkGetPhysicalDeviceDisplayPlanePropertiesKHR(dev, &np, planes));
	uint32_t plane = UINT32_MAX, stack = 0;
	for (uint32_t i = 0; i < np && plane == UINT32_MAX; i++) {
		uint32_t nd = 0;
		CHECK(vkGetDisplayPlaneSupportedDisplaysKHR(dev, i, &nd, NULL));
		VkDisplayKHR *ds = calloc(nd ? nd : 1, sizeof *ds);
		if (!ds)
			exit(1);
		CHECK(vkGetDisplayPlaneSupportedDisplaysKHR(dev, i, &nd, ds));
		for (uint32_t j = 0; j < nd; j++)
			if (ds[j] == disp.display) {
				plane = i;
				stack = planes[i].currentStackIndex;
				break;
			}
		free(ds);
	}
	free(planes);
	if (plane == UINT32_MAX) {
		fprintf(stderr, "wsidisplay: no plane shows the display\n");
		exit(1);
	}

	*extent = mode.parameters.visibleRegion;
	VkDisplaySurfaceCreateInfoKHR ci = {
		.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
		.displayMode = mode.displayMode,
		.planeIndex = plane,
		.planeStackIndex = stack,
		.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
		.imageExtent = *extent,
	};
	VkSurfaceKHR s;
	CHECK(vkCreateDisplayPlaneSurfaceKHR(inst, &ci, NULL, &s));
	return s;
}

/* The surface's formats, asked for as an application asks: the count,
 * then the list. The first is what the swapchain is made with. */
static VkSurfaceFormatKHR formats(VkPhysicalDevice dev, VkSurfaceKHR s)
{
	uint32_t n = 0;
	CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(dev, s, &n, NULL));
	VkSurfaceFormatKHR *f = calloc(n ? n : 1, sizeof *f);
	if (!f || n == 0) {
		fprintf(stderr, "wsidisplay: the surface has no format\n");
		exit(1);
	}
	CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(dev, s, &n, f));
	VkSurfaceFormatKHR first = f[0];
	free(f);
	return first;
}

int main(int argc, char **argv)
{
	const char *want = argc > 1 && argv[1][0] ? argv[1] : NULL;
	const int rounds = argc > 2 ? atoi(argv[2]) : 3;

	const char *iext[] = { VK_KHR_SURFACE_EXTENSION_NAME,
						   VK_KHR_DISPLAY_EXTENSION_NAME };
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "wsidisplay",
		.apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
		.enabledExtensionCount = 2,
		.ppEnabledExtensionNames = iext,
	};
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	VkPhysicalDevice dev = pick_device(inst, want);

	VkExtent2D extent;
	VkSurfaceKHR surf = display_surface(inst, dev, &extent);

	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(dev, &nq, NULL);
	uint32_t family = UINT32_MAX;
	for (uint32_t i = 0; i < nq && family == UINT32_MAX; i++) {
		VkBool32 ok = VK_FALSE;
		CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surf, &ok));
		if (ok)
			family = i;
	}
	if (family == UINT32_MAX) {
		fprintf(stderr, "wsidisplay: no queue presents to the surface\n");
		return 1;
	}
	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = family,
		.queueCount = 1,
		.pQueuePriorities = &prio,
	};
	const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci,
		.enabledExtensionCount = 1,
		.ppEnabledExtensionNames = dext,
	};
	VkDevice device;
	CHECK(vkCreateDevice(dev, &dci, NULL, &device));

	VkSurfaceFormatKHR fmt = formats(dev, surf);
	for (int i = 0; i < rounds; i++) {
		VkSurfaceCapabilitiesKHR caps;
		CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev, surf, &caps));
		VkSwapchainCreateInfoKHR sci = {
			.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			.surface = surf,
			.minImageCount = caps.minImageCount,
			.imageFormat = fmt.format,
			.imageColorSpace = fmt.colorSpace,
			.imageExtent = extent,
			.imageArrayLayers = 1,
			.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
			.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			.presentMode = VK_PRESENT_MODE_FIFO_KHR,
			.clipped = VK_TRUE,
		};
		VkSwapchainKHR chain;
		CHECK(vkCreateSwapchainKHR(device, &sci, NULL, &chain));
		vkDestroySwapchainKHR(device, chain, NULL);
		fmt = formats(dev, surf);
		printf("round %d: swapchain made and destroyed, formats asked "
			   "again\n", i + 1);
	}

	vkDestroyDevice(device, NULL);
	vkDestroySurfaceKHR(inst, surf, NULL);
	vkDestroyInstance(inst, NULL);
	printf("done; under ASan, a leak per round is Mesa's\n");
	return 0;
}

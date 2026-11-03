/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* wine keeping a destroyed Vulkan surface's frame on the window, over a
 * child that presents after it, with nothing of fenestal in it:
 * docs/wine-parked-surface.md's reproducer. A window is cleared red for a
 * second through a swapchain of its own, the swapchain and the surface
 * destroyed, and then a child window filling the client area is cleared
 * green through a new one until the time is up. On Windows the window
 * shows green; under wine 11.20's Wayland driver, by reading the source,
 * it goes on showing red: win32u parks the destroyed surface's client
 * surface for the window's next one (use_window_client_surface(FALSE)),
 * attached, and winewayland puts the child's subsurface directly above
 * the window's own wl_surface, which is under the parked one.
 *
 *   parkedchild [child|self] [seconds]
 *       child (the default) presents the second colour into the child;
 *       self into the window again, which reuses the parked surface and
 *       is the control: green there too. seconds is 10 by default.
 *
 * Each phase prints a line with its time, for a capture to be matched
 * against. Built by hand, run under wine with DISPLAY unset:
 *
 *   R=build/win64/release
 *   x86_64-w64-mingw32-gcc -std=gnu11 -O2 -Wall -Wextra -mconsole \
 *       -I$R/vulkan-include -o parkedchild.exe tools/probe/parkedchild.c \
 *       $R/libvulkan-1.a -luser32
 *
 * Written from the measurements in DESIGN *Under wine on Wayland the
 * child is there from the start* and not yet built or run. */
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VkInstance vi;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static VkCommandBuffer cb;
static VkSemaphore acquired, done;
static VkFence fence;
static PFN_vkCmdClearColorImage clear;
static DWORD t0;

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	return DefWindowProcW(h, m, w, l);
}

static void pump(void)
{
	MSG msg;
	while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}

static void say(const char *what)
{
	printf("%6.3f s  %s\n", (GetTickCount() - t0) / 1000.0, what);
	fflush(stdout);
}

/* The device, on the first GPU whose queue family 0 presents to `hwnd`. */
static int device_for(HWND hwnd, VkSurfaceKHR surf)
{
	uint32_t n = 8;
	VkPhysicalDevice all[8];
	vkEnumeratePhysicalDevices(vi, &n, all);
	for (uint32_t d = 0; d < n && !pd; d++) {
		VkBool32 ok = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(all[d], 0, surf, &ok);
		if (ok)
			pd = all[d];
	}
	if (!pd)
		return 0;
	VkPhysicalDeviceProperties pp;
	vkGetPhysicalDeviceProperties(pd, &pp);
	printf("device: %s\n", pp.deviceName);
	(void)hwnd;

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
	const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
		.enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
	if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS)
		return 0;
	vkGetDeviceQueue(dev, 0, 0, &queue);

	VkCommandPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
	vkCreateCommandPool(dev, &pci, NULL, &pool);
	VkCommandBufferAllocateInfo cai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1 };
	vkAllocateCommandBuffers(dev, &cai, &cb);
	VkSemaphoreCreateInfo sei = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	vkCreateSemaphore(dev, &sei, NULL, &acquired);
	vkCreateSemaphore(dev, &sei, NULL, &done);
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	vkCreateFence(dev, &fci, NULL, &fence);

	/* Fetched: the cross tree's import library holds only the entry
	 * points the library itself calls. */
	PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)
		(void (*)(void))GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),
									   "vkGetDeviceProcAddr");
	clear = (PFN_vkCmdClearColorImage)gdpa(dev, "vkCmdClearColorImage");
	return clear != NULL;
}

/* A surface and a swapchain on `hwnd`, cleared to `rgb` frame after frame
 * for `ms`, then both destroyed: the destroy is what wine parks. */
static int present(HWND hwnd, const float rgb[3], DWORD ms)
{
	VkWin32SurfaceCreateInfoKHR wci = {
		.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
		.hinstance = GetModuleHandleW(NULL), .hwnd = hwnd };
	VkSurfaceKHR surf;
	if (vkCreateWin32SurfaceKHR(vi, &wci, NULL, &surf) != VK_SUCCESS)
		return 0;
	if (!dev && !device_for(hwnd, surf))
		return 0;

	VkSurfaceCapabilitiesKHR caps;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
	VkExtent2D extent = caps.currentExtent;
	if (extent.width == UINT32_MAX) {
		RECT rc;
		GetClientRect(hwnd, &rc);
		extent.width = (uint32_t)(rc.right - rc.left);
		extent.height = (uint32_t)(rc.bottom - rc.top);
	}
	uint32_t fn = 1;
	VkSurfaceFormatKHR fmt;
	vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &fn, &fmt);
	VkSwapchainCreateInfoKHR sci = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = surf,
		.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount,
		.imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
		.imageExtent = extent, .imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.preTransform = caps.currentTransform,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
	VkSwapchainKHR sc;
	if (vkCreateSwapchainKHR(dev, &sci, NULL, &sc) != VK_SUCCESS) {
		vkDestroySurfaceKHR(vi, surf, NULL);
		return 0;
	}
	uint32_t ni = 8;
	VkImage imgs[8];
	vkGetSwapchainImagesKHR(dev, sc, &ni, imgs);

	const DWORD end = GetTickCount() + ms;
	while (GetTickCount() < end) {
		uint32_t idx;
		if (vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acquired,
								  VK_NULL_HANDLE, &idx) < 0)
			break;
		VkCommandBufferBeginInfo bi = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		vkBeginCommandBuffer(cb, &bi);
		VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageMemoryBarrier b = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = imgs[idx], .subresourceRange = r };
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
							 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
							 NULL, 1, &b);
		VkClearColorValue c = { .float32 = { rgb[0], rgb[1], rgb[2], 1.0f } };
		clear(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = 0;
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
							 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
							 0, NULL, 1, &b);
		vkEndCommandBuffer(cb);
		VkPipelineStageFlags w = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
							.waitSemaphoreCount = 1,
							.pWaitSemaphores = &acquired,
							.pWaitDstStageMask = &w,
							.commandBufferCount = 1, .pCommandBuffers = &cb,
							.signalSemaphoreCount = 1,
							.pSignalSemaphores = &done };
		vkQueueSubmit(queue, 1, &si, fence);
		VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
								.waitSemaphoreCount = 1,
								.pWaitSemaphores = &done,
								.swapchainCount = 1, .pSwapchains = &sc,
								.pImageIndices = &idx };
		vkQueuePresentKHR(queue, &pi);
		vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
		vkResetFences(dev, 1, &fence);
		pump();
	}
	vkDeviceWaitIdle(dev);
	vkDestroySwapchainKHR(dev, sc, NULL);
	vkDestroySurfaceKHR(vi, surf, NULL);
	return 1;
}

int main(int argc, char **argv)
{
	const int self = argc > 1 && !strcmp(argv[1], "self");
	const DWORD seconds = argc > 2 ? (DWORD)atoi(argv[2]) : 10;
	static const float RED[3] = { 1.0f, 0.0f, 0.0f };
	static const float GREEN[3] = { 0.0f, 1.0f, 0.0f };

	HINSTANCE inst = GetModuleHandleW(NULL);
	WNDCLASSEXW wc = { .cbSize = sizeof wc, .lpfnWndProc = proc,
					   .hInstance = inst, .lpszClassName = L"parkedchild",
					   .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW) };
	RegisterClassExW(&wc);
	HWND top = CreateWindowExW(0, L"parkedchild", L"parkedchild",
							   WS_OVERLAPPEDWINDOW, 100, 100, 640, 400, NULL,
							   NULL, inst, NULL);
	ShowWindow(top, SW_SHOW);
	pump();

	const char *ext[] = { VK_KHR_SURFACE_EXTENSION_NAME,
						  VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
	VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
							 .apiVersion = VK_API_VERSION_1_0 };
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &ai, .enabledExtensionCount = 2,
		.ppEnabledExtensionNames = ext };
	if (vkCreateInstance(&ici, NULL, &vi) != VK_SUCCESS) {
		puts("no Vulkan instance");
		return 1;
	}

	t0 = GetTickCount();
	say("window: red, 1 s");
	if (!present(top, RED, 1000)) {
		puts("the window's swapchain failed");
		return 1;
	}
	say("window: swapchain and surface destroyed");

	HWND target = top;
	if (!self) {
		RECT rc;
		GetClientRect(top, &rc);
		target = CreateWindowExW(0, L"parkedchild", NULL,
								 WS_CHILD | WS_VISIBLE, 0, 0,
								 rc.right - rc.left, rc.bottom - rc.top, top,
								 NULL, inst, NULL);
		pump();
		say("child: made over the whole client area");
	}
	say(self ? "window again: green; expected on screen: green"
			 : "child: green; expected on screen: green");
	if (!present(target, GREEN, seconds * 1000)) {
		puts("the second swapchain failed");
		return 1;
	}
	say("done");

	vkDestroyCommandPool(dev, pool, NULL);
	vkDestroyFence(dev, fence, NULL);
	vkDestroySemaphore(dev, done, NULL);
	vkDestroySemaphore(dev, acquired, NULL);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(vi, NULL);
	if (target != top)
		DestroyWindow(target);
	DestroyWindow(top);
	return 0;
}

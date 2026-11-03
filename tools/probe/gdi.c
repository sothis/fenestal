/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* Where a lifetime's GDI object comes from. fenestal-dev lifecycle on Windows
 * itself found GDI objects one higher after each init/run/shutdown -- handles
 * and USER objects held -- and the library creates none of its own. So each
 * stage of a lifetime is repeated here alone, and the count read after:
 *
 *   window     a class and a window, shown, pumped and destroyed
 *   surface    the same with a Vulkan instance and a surface on it
 *   inventory  the same, asking every device's every queue family whether it
 *              can present there -- what fenestal's startup inventory asks
 *   swapchain  the same with a device and a swapchain on that
 *   present    the same with frames cleared and presented
 *   hidden     the swapchain stage, made while the window is not yet shown
 *              and shown only after -- the order fnl_init() and fnl_run()
 *              have
 *
 *   R=../../build/win64/release
 *   x86_64-w64-mingw32-gcc -O2 -I$R/vulkan-include -o gdi.exe gdi.c \
 *       $R/libvulkan-1.a -luser32 -lgdi32
 *
 * Run on the desktop (tools/probe/native_run.py). GDI_DEVICE=<n> picks the
 * device, GDI_RECREATE=<n> recreates the swapchain that often within one
 * lifetime. What it found on the first laptop: on the GTX 1050 Ti each
 * lifetime past the swapchain stage keeps one GDI object and fifty
 * recreations keep none; on the HD 630 nothing is kept. DESIGN *Measured
 * on Windows itself*. */
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CYCLES 10

static int printed;

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	return DefWindowProcW(h, m, w, l);
}

static DWORD gdi(void)
{
	return GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
}

static void pump(int ms)
{
	DWORD end = GetTickCount() + (DWORD)ms;
	while (GetTickCount() < end) {
		MSG msg;
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		Sleep(5);
	}
}

/* One lifetime, as far as `depth` goes; a `hidden` one makes its window
 * shown only once the swapchain exists. */
static int lifetime(int depth, int hidden)
{
	HINSTANCE inst = GetModuleHandleW(NULL);
	WNDCLASSEXW wc = { .cbSize = sizeof wc, .lpfnWndProc = proc,
					   .hInstance = inst, .lpszClassName = L"fnlgdi",
					   .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW) };
	if (!RegisterClassExW(&wc))
		return 1;
	HWND hwnd = CreateWindowExW(0, L"fnlgdi", L"gdi", WS_OVERLAPPEDWINDOW,
								100, 100, 640, 400, NULL, NULL, inst, NULL);
	if (!hidden) {
		ShowWindow(hwnd, SW_SHOW);
		pump(150);
	}

	VkInstance vi = VK_NULL_HANDLE;
	VkSurfaceKHR surf = VK_NULL_HANDLE;
	VkDevice dev = VK_NULL_HANDLE;
	VkSwapchainKHR sc = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkSemaphore acquired = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	int rc = 0;

	if (depth >= 1) {
		const char *ext[] = { VK_KHR_SURFACE_EXTENSION_NAME,
							  VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
		VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
								 .apiVersion = VK_API_VERSION_1_0 };
		VkInstanceCreateInfo ici = {
			.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.pApplicationInfo = &ai, .enabledExtensionCount = 2,
			.ppEnabledExtensionNames = ext };
		if (vkCreateInstance(&ici, NULL, &vi) != VK_SUCCESS)
			{ rc = 2; goto out; }
		VkWin32SurfaceCreateInfoKHR sci = {
			.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
			.hinstance = inst, .hwnd = hwnd };
		if (vkCreateWin32SurfaceKHR(vi, &sci, NULL, &surf) != VK_SUCCESS)
			{ rc = 3; goto out; }
	}
	if (depth >= 2) {
		uint32_t n = 8, nq = 16;
		VkPhysicalDevice all[8];
		VkQueueFamilyProperties qf[16];
		vkEnumeratePhysicalDevices(vi, &n, all);
		for (uint32_t d = 0; d < n; d++) {
			nq = 16;
			vkGetPhysicalDeviceQueueFamilyProperties(all[d], &nq, qf);
			for (uint32_t q = 0; q < nq; q++) {
				VkBool32 can = VK_FALSE;
				vkGetPhysicalDeviceSurfaceSupportKHR(all[d], q, surf, &can);
			}
		}
	}
	if (depth >= 3) {
		uint32_t n = 8;
		VkPhysicalDevice pds[8];
		vkEnumeratePhysicalDevices(vi, &n, pds);
		/* GDI_DEVICE picks one: on a hybrid laptop which GPU presents is the
		 * question, the discrete one going out through the other. */
		const char *pick = getenv("GDI_DEVICE");
		uint32_t k = pick ? (uint32_t)atoi(pick) : 0;
		VkPhysicalDevice pd = pds[k < n ? k : 0];
		if (depth == 3 && !hidden && !printed) {
			VkPhysicalDeviceProperties pp;
			vkGetPhysicalDeviceProperties(pd, &pp);
			printf("device: %s\n", pp.deviceName);
			printed = 1;
		}
		VkBool32 ok = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surf, &ok);
		if (!ok)
			{ rc = 4; goto out; }
		float prio = 1.0f;
		VkDeviceQueueCreateInfo qci = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = 0, .queueCount = 1,
			.pQueuePriorities = &prio };
		const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
		VkDeviceCreateInfo dci = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
			.enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
		if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS)
			{ rc = 5; goto out; }
		VkSurfaceCapabilitiesKHR caps;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
		uint32_t fn = 1;
		VkSurfaceFormatKHR fmt;
		vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &fn, &fmt);
		VkSwapchainCreateInfoKHR sci = {
			.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			.surface = surf,
			.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount,
			.imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
			.imageExtent = caps.currentExtent, .imageArrayLayers = 1,
			.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
						  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			.preTransform = caps.currentTransform,
			.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			.presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
		if (vkCreateSwapchainKHR(dev, &sci, NULL, &sc) != VK_SUCCESS)
			{ rc = 6; goto out; }
		/* GDI_RECREATE: that many more, each from the last, as a resize
		 * makes them -- the question being whether a resize costs one. */
		const char *rec = getenv("GDI_RECREATE");
		const int more = rec ? atoi(rec) : 0;
		const DWORD g0 = gdi();
		for (int i = 0; i < more; i++) {
			sci.oldSwapchain = sc;
			VkSwapchainKHR next;
			if (vkCreateSwapchainKHR(dev, &sci, NULL, &next) != VK_SUCCESS)
				{ rc = 7; goto out; }
			vkDestroySwapchainKHR(dev, sc, NULL);
			sc = next;
		}
		if (more && depth == 3 && !hidden)
			printf("  %d recreations in one lifetime: GDI %lu -> %lu\n",
				   more, g0, gdi());
		if (hidden) {
			ShowWindow(hwnd, SW_SHOW);
			pump(150);
		}
	}
	if (depth >= 4) {
		/* A few frames, each cleared and moved to present: the transfer
		 * the window's own presents would be. */
		VkQueue q;
		vkGetDeviceQueue(dev, 0, 0, &q);
		VkCommandPoolCreateInfo pci = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
		vkCreateCommandPool(dev, &pci, NULL, &pool);
		VkCommandBuffer cb;
		VkCommandBufferAllocateInfo cai = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1 };
		vkAllocateCommandBuffers(dev, &cai, &cb);
		/* Fetched: the import library is made of the entry points the
		 * library itself calls, and it clears nothing this way. */
		PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)
			(void (*)(void))GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"),
										   "vkGetDeviceProcAddr");
		PFN_vkCmdClearColorImage clear = (PFN_vkCmdClearColorImage)
			gdpa(dev, "vkCmdClearColorImage");
		VkSemaphoreCreateInfo sei = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		vkCreateSemaphore(dev, &sei, NULL, &acquired);
		vkCreateSemaphore(dev, &sei, NULL, &done);
		VkFenceCreateInfo fci = {
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		vkCreateFence(dev, &fci, NULL, &fence);
		uint32_t ni = 0;
		vkGetSwapchainImagesKHR(dev, sc, &ni, NULL);
		VkImage imgs[8];
		if (ni > 8) ni = 8;
		vkGetSwapchainImagesKHR(dev, sc, &ni, imgs);
		for (int f = 0; f < 6; f++) {
			uint32_t idx;
			vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acquired,
								  VK_NULL_HANDLE, &idx);
			VkCommandBufferBeginInfo bi = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			vkBeginCommandBuffer(cb, &bi);
			VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT,
										  0, 1, 0, 1 };
			VkImageMemoryBarrier b = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				.srcAccessMask = 0,
				.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = imgs[idx], .subresourceRange = r };
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
								 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
								 0, NULL, 1, &b);
			VkClearColorValue c = { .float32 = { 0.2f, 0.3f, 0.4f, 1.0f } };
			clear(cb, imgs[idx],
								 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1,
								 &r);
			b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.dstAccessMask = 0;
			b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
								 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
								 NULL, 0, NULL, 1, &b);
			vkEndCommandBuffer(cb);
			VkPipelineStageFlags w = VK_PIPELINE_STAGE_TRANSFER_BIT;
			VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
								.waitSemaphoreCount = 1,
								.pWaitSemaphores = &acquired,
								.pWaitDstStageMask = &w,
								.commandBufferCount = 1,
								.pCommandBuffers = &cb,
								.signalSemaphoreCount = 1,
								.pSignalSemaphores = &done };
			vkQueueSubmit(q, 1, &si, fence);
			VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
									.waitSemaphoreCount = 1,
									.pWaitSemaphores = &done,
									.swapchainCount = 1, .pSwapchains = &sc,
									.pImageIndices = &idx };
			vkQueuePresentKHR(q, &pi);
			vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
			vkResetFences(dev, 1, &fence);
			pump(20);
		}
		vkDeviceWaitIdle(dev);
	}

out:
	if (fence) vkDestroyFence(dev, fence, NULL);
	if (done) vkDestroySemaphore(dev, done, NULL);
	if (acquired) vkDestroySemaphore(dev, acquired, NULL);
	if (pool) vkDestroyCommandPool(dev, pool, NULL);
	if (sc) vkDestroySwapchainKHR(dev, sc, NULL);
	if (dev) vkDestroyDevice(dev, NULL);
	if (surf) vkDestroySurfaceKHR(vi, surf, NULL);
	if (vi) vkDestroyInstance(vi, NULL);
	DestroyWindow(hwnd);
	pump(100);
	UnregisterClassW(L"fnlgdi", inst);
	return rc;
}

int main(void)
{
	static const char *const NAME[] = { "window", "surface", "inventory",
										"swapchain", "present", "hidden" };
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	for (int stage = 0; stage < 6; stage++) {
		const int depth = stage < 5 ? stage : 3, hidden = stage == 5;
		/* One first, for what a driver loads once and keeps. */
		int rc = lifetime(depth, hidden);
		const DWORD before = gdi();
		for (int i = 0; i < CYCLES && !rc; i++)
			rc = lifetime(depth, hidden);
		printf("%-10s %2d lifetimes: GDI %lu -> %lu%s\n", NAME[stage], CYCLES,
			   before, gdi(), rc ? " (stopped: a call failed)" : "");
		fflush(stdout);
	}
	return 0;
}

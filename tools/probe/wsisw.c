/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* Mesa's X11 software swapchain, alone: the two faults docs/mesa-x11-sw.md
 * reports, each with nothing of fenestal in it. Run on lavapipe.
 *
 *   wsisw pad      with MESA_VK_WSI_DEBUG=noshm: a window PAD_W pixels wide
 *                  on a green background, an image cleared red at that
 *                  width, the window widened by PAD_GROW before the image
 *                  is presented, and the new strip read back. Mesa's
 *                  PutImage is the row pitch wide, so the first columns of
 *                  the strip are the image's padding and not the green.
 *   wsisw shm [n]  with MIT-SHM: a small swapchain rebuilt n times (200),
 *                  and this process's segments still alive in
 *                  /proc/sysvipc/shm counted every 25. Each rebuild should
 *                  leave none; the server keeps them attached instead.
 *
 * Built here, run on the thinkpad by tools/probe/bare_run.py:
 *
 *   gcc -O2 -Wall -Wextra -o build/wsisw tools/probe/wsisw.c \
 *       -lvulkan -lxcb */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xcb/xcb.h>
#define VK_USE_PLATFORM_XCB_KHR
#include <vulkan/vulkan.h>

#define PAD_W    1001
#define PAD_H    200
#define PAD_GROW 32
#define SHM_W    64
#define SHM_H    64

#define CK(x) do { VkResult r_ = (x); if (r_ < 0) { \
	fprintf(stderr, "%s: %d\n", #x, r_); exit(1); } } while (0)

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_window_t win;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static VkSurfaceKHR surf;
static VkCommandPool pool;

static void sleep_ms(long ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&t, NULL);
}

static void open_window(int w, int h, uint32_t background)
{
	int scr;
	conn = xcb_connect(NULL, &scr);
	if (xcb_connection_has_error(conn)) {
		fprintf(stderr, "no X server\n");
		exit(1);
	}
	screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
	win = xcb_generate_id(conn);
	const uint32_t v[2] = { background, XCB_EVENT_MASK_EXPOSURE };
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, 0, 0,
					  (uint16_t)w, (uint16_t)h, 0,
					  XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
					  XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, v);
	xcb_map_window(conn, win);
	xcb_flush(conn);
	xcb_generic_event_t *e;
	while ((e = xcb_wait_for_event(conn))) {
		const bool exposed = (e->response_type & 0x7f) == XCB_EXPOSE;
		free(e);
		if (exposed)
			break;
	}
}

static void open_vulkan(void)
{
	const char *ext[] = { VK_KHR_SURFACE_EXTENSION_NAME,
						  VK_KHR_XCB_SURFACE_EXTENSION_NAME };
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
							  .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
		.enabledExtensionCount = 2, .ppEnabledExtensionNames = ext };
	CK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t n = 1;
	VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd);
	if (r < 0 || n == 0) {
		fprintf(stderr, "no Vulkan device\n");
		exit(1);
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	fprintf(stderr, "device: %s, Vulkan %u.%u, driver %#x\n",
			props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
			VK_VERSION_MINOR(props.apiVersion), props.driverVersion);

	VkXcbSurfaceCreateInfoKHR sci = {
		.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
		.connection = conn, .window = win };
	CK(vkCreateXcbSurfaceKHR(inst, &sci, NULL, &surf));
	VkBool32 ok = VK_FALSE;
	CK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surf, &ok));
	if (!ok) {
		fprintf(stderr, "queue family 0 cannot present\n");
		exit(1);
	}
	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
	const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
		.enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &q);
	VkCommandPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
	CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
}

static VkSwapchainKHR make_swapchain(int w, int h, VkSwapchainKHR old)
{
	VkSurfaceCapabilitiesKHR caps;
	CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
	VkSwapchainCreateInfoKHR ci = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = surf,
		.minImageCount = caps.minImageCount,
		.imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
		.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
		.imageExtent = { (uint32_t)w, (uint32_t)h },
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
					  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR,
		.clipped = VK_TRUE,
		.oldSwapchain = old };
	VkSwapchainKHR sc;
	CK(vkCreateSwapchainKHR(dev, &ci, NULL, &sc));
	return sc;
}

/* Acquire an image and clear it to `rgba`, waited for; its index. */
static uint32_t acquire_cleared(VkSwapchainKHR sc, const float rgba[4])
{
	VkFence f;
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	CK(vkCreateFence(dev, &fci, NULL, &f));
	uint32_t idx;
	CK(vkAcquireNextImageKHR(dev, sc, UINT64_MAX, VK_NULL_HANDLE, f, &idx));
	CK(vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX));
	vkDestroyFence(dev, f, NULL);

	uint32_t n = 0;
	CK(vkGetSwapchainImagesKHR(dev, sc, &n, NULL));
	VkImage *img = calloc(n, sizeof *img);
	CK(vkGetSwapchainImagesKHR(dev, sc, &n, img));

	VkCommandBuffer cb;
	VkCommandBufferAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1 };
	CK(vkAllocateCommandBuffers(dev, &ai, &cb));
	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CK(vkBeginCommandBuffer(cb, &bi));
	const VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
										  0, 1 };
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img[idx], .subresourceRange = all };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
						 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
						 1, &b);
	VkClearColorValue c;
	memcpy(c.float32, rgba, sizeof c.float32);
	vkCmdClearColorImage(cb, img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						 &c, 1, &all);
	b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask = 0;
	b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
						 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
						 NULL, 1, &b);
	CK(vkEndCommandBuffer(cb));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
						.commandBufferCount = 1, .pCommandBuffers = &cb };
	CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(q));
	vkFreeCommandBuffers(dev, pool, 1, &cb);
	free(img);
	return idx;
}

static VkResult present(VkSwapchainKHR sc, uint32_t idx)
{
	VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
							.swapchainCount = 1, .pSwapchains = &sc,
							.pImageIndices = &idx };
	return vkQueuePresentKHR(q, &pi);
}

static int run_pad(void)
{
	open_window(PAD_W, PAD_H, 0x00ff00);
	open_vulkan();
	VkSwapchainKHR sc = make_swapchain(PAD_W, PAD_H, VK_NULL_HANDLE);
	const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
	const uint32_t idx = acquire_cleared(sc, red);

	/* Wider before the present, as a window being dragged is: the strip is
	 * the server's green until something draws into it. */
	const uint32_t wv[1] = { PAD_W + PAD_GROW };
	xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_WIDTH, wv);
	free(xcb_get_geometry_reply(conn, xcb_get_geometry(conn, win), NULL));

	const VkResult pr = present(sc, idx);
	fprintf(stderr, "present after the widening: %d\n", pr);
	sleep_ms(300);                  /* Mesa's thread sends the image */
	CK(vkDeviceWaitIdle(dev));

	xcb_get_image_reply_t *im = xcb_get_image_reply(conn,
		xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, win, PAD_W - 2,
					  PAD_H / 2, PAD_GROW + 2, 1, ~0u), NULL);
	if (!im) {
		fprintf(stderr, "no image back\n");
		return 1;
	}
	const uint32_t *px = (const uint32_t *)xcb_get_image_data(im);
	int not_green = 0;
	fprintf(stderr, "columns %d.. of a window widened from %d to %d, after a "
			"present of the %d-wide image:\n", PAD_W - 2, PAD_W,
			PAD_W + PAD_GROW, PAD_W);
	for (int i = 0; i < PAD_GROW + 2; i++) {
		const uint32_t p = px[i] & 0xffffff;
		const char *what = p == 0xff0000 ? "image" : p == 0x00ff00
						 ? "background" : "padding";
		if (i >= 2 && p != 0x00ff00)
			not_green++;
		fprintf(stderr, "  x=%d %06x %s\n", PAD_W - 2 + i, p, what);
	}
	fprintf(stderr, "%d of the %d new columns were written over\n", not_green,
			PAD_GROW);
	free(im);
	vkDestroySwapchainKHR(dev, sc, NULL);
	return not_green ? 2 : 0;
}

/* Segments this process created that are still in the system: detached by
 * the client and marked for removal, they live while the server holds
 * them. */
static int my_segments(void)
{
	FILE *f = fopen("/proc/sysvipc/shm", "r");
	if (!f)
		return -1;
	char line[512];
	int n = 0;
	const long me = (long)getpid();
	if (!fgets(line, sizeof line, f)) {
		fclose(f);
		return -1;
	}
	while (fgets(line, sizeof line, f)) {
		long key, id, perms, size, cpid;
		if (sscanf(line, "%ld %ld %lo %ld %ld", &key, &id, &perms, &size,
				   &cpid) == 5 && cpid == me)
			n++;
	}
	fclose(f);
	return n;
}

static int run_shm(int rebuilds)
{
	open_window(SHM_W, SHM_H, 0);
	open_vulkan();
	VkSwapchainKHR sc = make_swapchain(SHM_W, SHM_H, VK_NULL_HANDLE);
	uint32_t images = 0;
	CK(vkGetSwapchainImagesKHR(dev, sc, &images, NULL));
	fprintf(stderr, "%u images a swapchain; segments alive after each 25 "
			"rebuilds:\n", images);
	fprintf(stderr, "  0: %d\n", my_segments());
	for (int i = 1; i <= rebuilds; i++) {
		VkSwapchainKHR next = make_swapchain(SHM_W, SHM_H, sc);
		vkDestroySwapchainKHR(dev, sc, NULL);
		sc = next;
		if (i % 25 == 0)
			fprintf(stderr, "  %d: %d\n", i, my_segments());
		fflush(stderr);
	}
	const int left = my_segments();
	vkDestroySwapchainKHR(dev, sc, NULL);
	fprintf(stderr, "and %d after the last swapchain is destroyed\n",
			my_segments());
	return left > (int)images ? 2 : 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "pad"))
		return run_pad();
	if (argc >= 2 && !strcmp(argv[1], "shm"))
		return run_shm(argc >= 3 ? atoi(argv[2]) : 200);
	fprintf(stderr, "usage: wsisw pad | wsisw shm [rebuilds]\n");
	return 1;
}

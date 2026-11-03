/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* A frame presented through Mesa's Vulkan WSI on weston, timed against
 * the GPU's runtime PM, with nothing of fenestal in it: whose the frames
 * are that a weston round on NVK leaves unpresented until the GPU resumes
 * (DESIGN *The startup inventory*, TODO). One window, FIFO, each frame a
 * vkCmdClearColorImage; a round idles `idle` seconds, presents one frame
 * and watches it for `watch` seconds, logging on one clock (ms since the
 * round began, its idle included)
 *
 *   submit     the frame's vkQueueSubmit
 *   fence      the submit's fence signalled: exported once as a sync_file
 *              and poll()ed beside the Wayland fd, so watching it is no
 *              ioctl -- each nouveau ioctl resumes the GPU and restarts its
 *              autosuspend timer, and a status query reads the fence's
 *              seqno, which would hide a fence that only a wake signals
 *   presented  weston's wp_presentation_feedback for the frame's commit
 *   discarded
 *   pm         a change in the device's power/runtime_status
 *
 * A frame not presented by the end of the watch is then followed by a
 * submit of an empty command buffer, the GPU's next work, and watched
 * again.
 *
 *   heldpresent [device-name-part [rounds [idle [watch]]]]
 *               defaults: NVK, 3, 8, 20
 *   HELDPRESENT_PCI=0000:01:00.0  the device whose runtime_status is read
 *   HELDPRESENT_BUSY=1            no idle between rounds: the control
 *   HELDPRESENT_RESIZE=1          the swapchain made anew at another size
 *                                 before each round, as at a restore
 *   HELDPRESENT_FRAMES=n          n frames a round, each watched (1)
 *   HELDPRESENT_STATES=1          each round maximizes or restores first:
 *                                 the configure awaited, the swapchain made
 *                                 at its size, the ack just before the
 *                                 present, as a toolkit does
 *   HELDPRESENT_AT=suspending     end the idle the moment runtime_status
 *                                 reads `suspending` (within `idle`)
 *
 * Exit status 0 is every frame presented within its watch, 1 one that
 * was not, 2 anything else. Run on weston with WAYLAND_DISPLAY set.
 *
 * Built, and run on a bare machine's weston, by tools/probe/heldpresent.sh:
 *
 *   FNL_DRIVE_BARE=schenker tools/probe/heldpresent.sh 120 \
 *       --env HELDPRESENT_PCI=0000:01:00.0 NVK 3 8 20 */
#define _GNU_SOURCE
#include <poll.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>

#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define WIN_W 640
#define WIN_H 480

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wp_presentation *presentation;
static bool configured;
static bool auto_ack = true;
static uint32_t cfg_serial;
static int32_t cfg_w, cfg_h;
static bool cfg_max;

static double t0;
/* Each frame's feedback, 0 waiting, 1 presented, 2 discarded: its own
 * slot, so a held frame's late answer cannot land on a later frame. */
static int fb_state[4096];
static int fb_next;
static VkSwapchainCreateInfoKHR sci;
static VkSwapchainKHR sc;
static VkImage imgs[8];

/* The swapchain at w x h, made anew from the old one if there is one. */
static void make_swapchain(VkDevice dev, uint32_t w, uint32_t h);
static char pm_path[256];
static char pm_last[32];

static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void
die(const char *what)
{
	fprintf(stderr, "heldpresent: %s\n", what);
	exit(2);
}

#define CHECK(x)                                                              \
	do {                                                                      \
		VkResult r_ = (x);                                                    \
		if (r_ != VK_SUCCESS) {                                               \
			fprintf(stderr, "heldpresent: %s: %d\n", #x, r_);                 \
			exit(2);                                                          \
		}                                                                     \
	} while (0)

/* Logs the device's runtime_status when it changed since the last call. */
static void
pm_poll(void)
{
	char s[32] = "?";
	FILE *f = fopen(pm_path, "r");
	if (f) {
		if (!fgets(s, sizeof s, f))
			strcpy(s, "?");
		fclose(f);
	}
	s[strcspn(s, "\n")] = 0;
	if (strcmp(s, pm_last)) {
		printf("  %9.1f  pm %s\n", now_ms() - t0, s);
		strcpy(pm_last, s);
	}
}

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name,
				const char *iface, uint32_t version)
{
	(void)data;
	(void)version;
	if (!strcmp(iface, wl_compositor_interface.name))
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, xdg_wm_base_interface.name))
		wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
	else if (!strcmp(iface, wp_presentation_interface.name))
		presentation = wl_registry_bind(reg, name,
										&wp_presentation_interface, 1);
}

static void
registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data;
	(void)reg;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void
wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void
xs_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	(void)data;
	if (auto_ack)
		xdg_surface_ack_configure(xs, serial);
	cfg_serial = serial;
	configured = true;
}

static const struct xdg_surface_listener xs_listener = {
	.configure = xs_configure,
};

static void
top_configure(void *data, struct xdg_toplevel *top, int32_t w, int32_t h,
			  struct wl_array *states)
{
	(void)data;
	(void)top;
	cfg_w = w;
	cfg_h = h;
	cfg_max = false;
	uint32_t *st;
	wl_array_for_each(st, states) if (*st == XDG_TOPLEVEL_STATE_MAXIMIZED)
		cfg_max = true;
}

static void
top_close(void *data, struct xdg_toplevel *top)
{
	(void)data;
	(void)top;
}

static const struct xdg_toplevel_listener top_listener = {
	.configure = top_configure,
	.close = top_close,
};

/* The frame's feedback: 0 waiting, 1 presented, 2 discarded. */
static void
fb_sync_output(void *data, struct wp_presentation_feedback *fb,
			   struct wl_output *out)
{
	(void)data;
	(void)fb;
	(void)out;
}

static void
fb_presented(void *data, struct wp_presentation_feedback *fb, uint32_t hi,
			 uint32_t lo, uint32_t nsec, uint32_t refresh, uint32_t seq_hi,
			 uint32_t seq_lo, uint32_t flags)
{
	(void)hi;
	(void)lo;
	(void)nsec;
	(void)refresh;
	(void)seq_hi;
	(void)seq_lo;
	(void)flags;
	*(int *)data = 1;
	wp_presentation_feedback_destroy(fb);
}

static void
fb_discarded(void *data, struct wp_presentation_feedback *fb)
{
	*(int *)data = 2;
	wp_presentation_feedback_destroy(fb);
}

static const struct wp_presentation_feedback_listener fb_listener = {
	.sync_output = fb_sync_output,
	.presented = fb_presented,
	.discarded = fb_discarded,
};

/* Dispatches Wayland events for up to `ms`, without blocking longer;
 * with `*sync` a sync_file, also says when it signals, then closes it and
 * sets it to -1. */
static void
pump_fd(int ms, int *sync)
{
	while (wl_display_prepare_read(dpy) != 0)
		wl_display_dispatch_pending(dpy);
	wl_display_flush(dpy);
	struct pollfd p[2] = { { wl_display_get_fd(dpy), POLLIN, 0 },
						   { sync ? *sync : -1, POLLIN, 0 } };
	int n = poll(p, 2, ms);
	if (n > 0 && (p[0].revents & POLLIN))
		wl_display_read_events(dpy);
	else
		wl_display_cancel_read(dpy);
	if (n > 0 && sync && *sync >= 0 && p[1].revents) {
		printf("  %9.1f  fence\n", now_ms() - t0);
		close(*sync);
		*sync = -1;
	}
	wl_display_dispatch_pending(dpy);
}

static void
pump(int ms)
{
	pump_fd(ms, NULL);
}

/* Watches the frame's fence (a sync_file, -1 once signalled) and feedback
 * for `ms`; true when presented. */
static bool
watch(int *sync, int *fb, double ms)
{
	double end = now_ms() + ms;
	int seen_fb = *fb;
	while (now_ms() < end) {
		pump_fd(1, sync);
		pm_poll();
		if (*fb != seen_fb) {
			printf("  %9.1f  %s\n", now_ms() - t0,
				   *fb == 1 ? "presented" : "discarded");
			seen_fb = *fb;
		}
		if (*sync < 0 && *fb)
			return *fb == 1;
		if (wl_display_get_error(dpy))
			die("Wayland connection lost");
	}
	return false;
}

static void
make_swapchain(VkDevice dev, uint32_t w, uint32_t h)
{
	VkSwapchainKHR old = sc;
	if (old)
		vkDeviceWaitIdle(dev);
	sci.imageExtent = (VkExtent2D){ w, h };
	sci.oldSwapchain = old;
	VkResult r = vkCreateSwapchainKHR(dev, &sci, NULL, &sc);
	if (r != VK_SUCCESS) {
		fprintf(stderr, "heldpresent: vkCreateSwapchainKHR: %d\n", r);
		exit(2);
	}
	if (old)
		vkDestroySwapchainKHR(dev, old, NULL);
	uint32_t ni = 8;
	vkGetSwapchainImagesKHR(dev, sc, &ni, imgs);
}

int
main(int argc, char **argv)
{
	const char *want = argc > 1 ? argv[1] : "NVK";
	int rounds = argc > 2 ? atoi(argv[2]) : 3;
	double idle_s = argc > 3 ? atof(argv[3]) : 8;
	double watch_s = argc > 4 ? atof(argv[4]) : 20;
	bool busy = getenv("HELDPRESENT_BUSY") != NULL;
	bool resize = getenv("HELDPRESENT_RESIZE") != NULL;
	const char *frames_env = getenv("HELDPRESENT_FRAMES");
	int frames = frames_env ? atoi(frames_env) : 1;
	const char *at = getenv("HELDPRESENT_AT");
	bool states = getenv("HELDPRESENT_STATES") != NULL;
	const char *pci = getenv("HELDPRESENT_PCI");
	snprintf(pm_path, sizeof pm_path,
			 "/sys/bus/pci/devices/%s/power/runtime_status",
			 pci ? pci : "0000:01:00.0");
	setvbuf(stdout, NULL, _IOLBF, 0);

	dpy = wl_display_connect(NULL);
	if (!dpy)
		die("no Wayland display (WAYLAND_DISPLAY)");
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!compositor || !wm_base || !presentation)
		die("no wl_compositor, xdg_wm_base or wp_presentation");
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	struct wl_surface *surf = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surf);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(top, &top_listener, NULL);
	xdg_toplevel_set_title(top, "heldpresent");
	wl_surface_commit(surf);
	while (!configured)
		if (wl_display_dispatch(dpy) < 0)
			die("Wayland connection lost");

	const char *iext[] = { VK_KHR_SURFACE_EXTENSION_NAME,
						   VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME };
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
							  .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
		.enabledExtensionCount = 2,
		.ppEnabledExtensionNames = iext,
	};
	VkInstance inst;
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	VkWaylandSurfaceCreateInfoKHR wci = {
		.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
		.display = dpy,
		.surface = surf,
	};
	VkSurfaceKHR vsurf;
	CHECK(vkCreateWaylandSurfaceKHR(inst, &wci, NULL, &vsurf));

	uint32_t n = 8;
	VkPhysicalDevice pds[8];
	CHECK(vkEnumeratePhysicalDevices(inst, &n, pds));
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	uint32_t qf = 0;
	for (uint32_t i = 0; i < n && !pd; i++) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(pds[i], &p);
		if (!strstr(p.deviceName, want))
			continue;
		uint32_t nq = 16;
		VkQueueFamilyProperties qp[16];
		vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
		for (uint32_t q = 0; q < nq; q++) {
			VkBool32 ok = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, vsurf, &ok);
			if (ok && (qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
				pd = pds[i];
				qf = q;
				printf("device: %s (driver %u.%u.%u)\n", p.deviceName,
					   VK_VERSION_MAJOR(p.driverVersion),
					   VK_VERSION_MINOR(p.driverVersion),
					   VK_VERSION_PATCH(p.driverVersion));
				break;
			}
		}
	}
	if (!pd)
		die("no device by that name that presents to this surface");

	float prio = 1;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = qf,
		.queueCount = 1,
		.pQueuePriorities = &prio,
	};
	const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
						   VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME };
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci,
		.enabledExtensionCount = 2,
		.ppEnabledExtensionNames = dext,
	};
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue queue;
	vkGetDeviceQueue(dev, qf, 0, &queue);

	VkSurfaceCapabilitiesKHR caps;
	CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, vsurf, &caps));
	uint32_t nf = 32;
	VkSurfaceFormatKHR fmts[32];
	CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, vsurf, &nf, fmts));
	sci = (VkSwapchainCreateInfoKHR){
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = vsurf,
		.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount,
		.imageFormat = fmts[0].format,
		.imageColorSpace = fmts[0].colorSpace,
		.imageExtent = { WIN_W, WIN_H },
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR,
		.clipped = VK_TRUE,
	};
	if (caps.maxImageCount && sci.minImageCount > caps.maxImageCount)
		sci.minImageCount = caps.maxImageCount;
	make_swapchain(dev, WIN_W, WIN_H);

	VkCommandPoolCreateInfo pci_ = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = qf,
	};
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &pci_, NULL, &pool));
	VkCommandBufferAllocateInfo cai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cmd;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cmd));
	VkSemaphoreCreateInfo semi = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};
	VkSemaphore acquired, rendered;
	CHECK(vkCreateSemaphore(dev, &semi, NULL, &acquired));
	CHECK(vkCreateSemaphore(dev, &semi, NULL, &rendered));
	VkExportFenceCreateInfo efci = {
		.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
							  .pNext = &efci };
	VkFence fence;
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));
	PFN_vkGetFenceFdKHR get_fence_fd =
		(PFN_vkGetFenceFdKHR)vkGetDeviceProcAddr(dev, "vkGetFenceFdKHR");
	if (!get_fence_fd)
		die("no vkGetFenceFdKHR");
	/* The GPU's next work after a frame not presented: empty, but a real
	 * submission to the kernel, which a vkQueueSubmit of nothing is not. */
	VkCommandBuffer nop;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &nop));
	VkCommandBufferBeginInfo nbi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	};
	CHECK(vkBeginCommandBuffer(nop, &nbi));
	CHECK(vkEndCommandBuffer(nop));

	int missed = 0;
	/* Round 0 shows the window; it is not counted. */
	for (int r = 0; r <= rounds; r++) {
		if (r > 0)
			printf("round %d%s:\n", r, busy ? " (busy)" : "");
		t0 = now_ms();
		if (r > 0 && !busy) {
			double end = now_ms() + idle_s * 1e3;
			while (now_ms() < end) {
				pump(at ? 1 : 10);
				pm_poll();
				if (at && !strcmp(pm_last, at))
					break;
			}
		}
		pm_poll();
		if (r > 0 && resize) {
			make_swapchain(dev, r % 2 ? 800 : WIN_W, r % 2 ? 600 : WIN_H);
			printf("  %9.1f  swapchain made at %ux%u\n", now_ms() - t0,
				   sci.imageExtent.width, sci.imageExtent.height);
		}
		bool ack = false;
		if (r > 0 && states) {
			bool to_max = r % 2;
			auto_ack = false;
			configured = false;
			if (to_max)
				xdg_toplevel_set_maximized(top);
			else
				xdg_toplevel_unset_maximized(top);
			while (!(configured && cfg_max == to_max))
				pump(10);
			uint32_t w = cfg_w > 0 ? (uint32_t)cfg_w : WIN_W;
			uint32_t h = cfg_h > 0 ? (uint32_t)cfg_h : WIN_H;
			printf("  %9.1f  configure %s %ux%u\n", now_ms() - t0,
				   to_max ? "maximized" : "floating", w, h);
			make_swapchain(dev, w, h);
			printf("  %9.1f  swapchain made at %ux%u\n", now_ms() - t0, w, h);
			ack = true;
		}
		bool presented = true;
		for (int f = 0; f < frames; f++) {
		uint32_t idx;
		VkResult ar = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acquired,
											VK_NULL_HANDLE, &idx);
		if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR)
			CHECK(ar);
		VkCommandBufferBeginInfo cbi = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		CHECK(vkResetCommandBuffer(cmd, 0));
		CHECK(vkBeginCommandBuffer(cmd, &cbi));
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
										  0, 1 };
		VkImageMemoryBarrier to_dst = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = imgs[idx],
			.subresourceRange = range,
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
							 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
							 NULL, 1, &to_dst);
		VkClearColorValue colour = { .float32 = {
			(r % 3 == 0), (r % 3 == 1), (r % 3 == 2), 1 } };
		vkCmdClearColorImage(cmd, imgs[idx],
							 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1,
							 &range);
		VkImageMemoryBarrier to_present = to_dst;
		to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		to_present.dstAccessMask = 0;
		to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
							 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
							 0, NULL, 1, &to_present);
		CHECK(vkEndCommandBuffer(cmd));
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo si = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &acquired,
			.pWaitDstStageMask = &stage,
			.commandBufferCount = 1,
			.pCommandBuffers = &cmd,
			.signalSemaphoreCount = 1,
			.pSignalSemaphores = &rendered,
		};
		CHECK(vkResetFences(dev, 1, &fence));
		if (r > 0)
			printf("  %9.1f  submit, frame %d\n", now_ms() - t0, f);
		CHECK(vkQueueSubmit(queue, 1, &si, fence));
		/* One ioctl, now, while the submit has the GPU awake anyway; the
		 * export leaves the fence unsignalled for the next frame's reset. */
		VkFenceGetFdInfoKHR gfi = {
			.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
			.fence = fence,
			.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
		};
		int sync = -1;
		CHECK(get_fence_fd(dev, &gfi, &sync));
		if (sync < 0)
			printf("  %9.1f  fence (signalled at export)\n", now_ms() - t0);
		/* The feedback goes on the next commit, which is the present's. */
		int *fb = &fb_state[fb_next++ % 4096];
		*fb = 0;
		wp_presentation_feedback_add_listener(
			wp_presentation_feedback(presentation, surf), &fb_listener, fb);
		VkPresentInfoKHR pi = {
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &rendered,
			.swapchainCount = 1,
			.pSwapchains = &sc,
			.pImageIndices = &idx,
		};
		if (ack) {
			xdg_surface_ack_configure(xs, cfg_serial);
			ack = false;
			auto_ack = true;
		}
		VkResult pr = vkQueuePresentKHR(queue, &pi);
		if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR)
			CHECK(pr);
		if (r > 0)
			printf("  %9.1f  frame %d presented to WSI\n", now_ms() - t0, f);
		if (watch(&sync, fb, watch_s * 1e3))
			continue;
		if (r == 0)
			die("the first frame was never presented");
		presented = false;
		printf("  %9.1f  not presented; an empty command buffer submitted\n",
			   now_ms() - t0);
		VkSubmitInfo nsi = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
							 .commandBufferCount = 1,
							 .pCommandBuffers = &nop };
		CHECK(vkQueueSubmit(queue, 1, &nsi, VK_NULL_HANDLE));
		watch(&sync, fb, watch_s * 1e3);
		if (sync >= 0)
			close(sync);
		}
		if (!presented)
			missed++;
	}
	vkDeviceWaitIdle(dev);
	printf("%d of %d rounds with a frame not presented within %.0f s\n",
		   missed, rounds, watch_s);
	return missed ? 1 : 0;
}

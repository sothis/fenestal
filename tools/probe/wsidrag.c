/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* A window resized as the stress round's drags resize it, and a swapchain
 * rebuilt the way src/renderer/vulkan/swapchain.c rebuilds it, with nothing
 * else of fenestal: whether a driver's X11 WSI can be left answering every
 * acquire on a new swapchain with VK_ERROR_OUT_OF_DATE_KHR, its extent the
 * surface's own, and what then gets an image out of it.
 *
 * NVIDIA's 580.178.04 on schenker's GTX 1050 Ti (PRIME onto the Intel
 * GPU's screen, Xorg with no window manager) did that to fenestal-testbed in
 * 7 of 13 rounds: the X server, currentExtent and the swapchain all said
 * 1392x860, and acquire said out of date on each of 21 rebuilds in 0.4 s,
 * for good.
 *
 *   wsidrag [rounds] [escapes] [hold-ms]
 *
 * A thread of its own resizes the window on the program's connection, as
 * the library's main thread does for a drag on an unmanaged window: one
 * grow of 160x120 in 12 steps, then eight drags of 40 each way in 6 steps
 * a quarter of a second apart, every step 16 ms; then the window is held
 * for a second and put back. That is a round (default 20).
 *
 * The surface is on a connection of its own, which nothing reads but the
 * driver, as the library's is (BackendState::gfx); the window is black
 * with north-west bit gravity, as x11.c makes it.
 *
 * The swapchain side is the library's: an sRGB format where there is one,
 * images cleared by a render pass and used for nothing else, two frames in
 * flight, the extent
 * the size the last ConfigureNotify gave and a rebuild as soon as it
 * changes, the old swapchain passed as oldSwapchain and
 * kept until two further frames are submitted (four at most, all destroyed
 * at once when there is no room), and the first rebuild of a run preceded
 * by a drained queue and 20 ms. It is *stuck* after STUCK_AFTER rebuilds in
 * a row whose first acquire was out of date. The resizing then stops and
 * the escapes are tried in order, TRIES rebuilds each, until an image is
 * acquired -- a comma list, by default every one in the order below:
 *
 *   again    the library's rebuild, more of it: the control
 *   events   a round trip to the server and 100 ms, then the library's
 *   idle     the device idle and every retired swapchain destroyed first
 *   fresh    as idle, the live one destroyed too and no oldSwapchain
 *   surface  as fresh, and a new VkSurfaceKHR on the same window
 *   rest     as surface, with half a second between, nothing alive
 *   connection  as surface, on a new X connection
 *   touch    the window configured at the size it has, then the library's
 *   wiggle   a pixel wider and straight back, then the library's
 *   nudge    the window a pixel wider, then the library's rebuild
 *
 * Exit 0 is never stuck, 2 stuck and freed every time, 3 stuck with
 * nothing that freed it. hold-ms (0) is slept before each acquire, as
 * the library's hold_wait() sleeps out its frame. FNL_GPU picks the
 * device as the library's does by vendor (nvidia, intel, amd), or else by
 * a piece of its name.
 *
 * WSIDRAG_RATE=60 asks the rate instead: the window's size changed that
 * many times a second for WSIDRAG_SECONDS (10), between 900x600 and
 * 1060x720 and back, and the frames and sizes counted. WSIDRAG_MODE says
 * how the two sides go together:
 *
 *   configure  rebuilt at each ConfigureNotify's size: the library
 *   ood        rebuilt only when an acquire or a present says out of
 *              date or suboptimal, at the surface's currentExtent
 *   held       as configure, and the next size applied only once a frame
 *              at the last one has been presented, or 250 ms on:
 *              Win32's size_await() for a window with no manager
 *
 * Built here, run on the bare machine by tools/probe/bare_run.py:
 *
 *   gcc -O2 -Wall -Wextra -pthread -o build/wsidrag tools/probe/wsidrag.c \
 *       -lvulkan -lxcb
 *   tools/probe/bare_run.py build/wsidrag 300 --env FNL_GPU=nvidia 20 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>
#define VK_USE_PLATFORM_XCB_KHR
#include <vulkan/vulkan.h>

#define BASE_W       900
#define BASE_H       600
#define STEP_MS      16
#define SETTLE_MS    250
#define HOLD_MS      1000
#define FRAMES       2          /* MAX_FRAMES_IN_FLIGHT */
#define MAX_RETIRED  4
#define MAX_IMAGES   8
#define STUCK_AFTER  40
#define TRIES        10
#define ACQUIRE_NS   100000000ull
#define REST_AFTER   0.25       /* SWAP_REST_AFTER */
#define REST_S       0.020      /* SWAP_REST */

#define CK(x) do { VkResult r_ = (x); if (r_ < 0) { \
	fprintf(stderr, "%s: %d\n", #x, r_); exit(1); } } while (0)

static xcb_connection_t *conn;
static xcb_connection_t *gfx;   /* the surface's, as BackendState::gfx */
static xcb_screen_t *screen;
static xcb_window_t win;
static VkInstance inst;
static VkPhysicalDevice pd;
static uint32_t qfam;
static VkDevice dev;
static VkQueue q;
static VkSurfaceKHR surf;
static VkSurfaceFormatKHR fmt;
static VkCommandPool pool;
static VkRenderPass pass;

/* Written by the resizer, read by the render loop. */
static atomic_bool hold, quit, resizer_done;
static atomic_int cfg_w = BASE_W, cfg_h = BASE_H;

/* The rate test's (WSIDRAG_RATE): the mode, and the size the render loop
 * last presented a frame at, which the held resizer waits for. */
typedef enum { MODE_CONFIGURE, MODE_OOD, MODE_HELD } Mode;
static Mode mode = MODE_CONFIGURE;
static atomic_int shown_w, shown_h;
static atomic_uint sizes_applied, sizes_shown, held_out;

typedef struct {
	VkSwapchainKHR sc;
	uint32_t n;
	VkImage img[MAX_IMAGES];
	VkSemaphore done[MAX_IMAGES];
	VkImageView view[MAX_IMAGES];
	VkFramebuffer fb[MAX_IMAGES];
	VkExtent2D ext;
	int countdown;
} Chain;

static Chain cur;
static Chain retired[MAX_RETIRED];
static int nretired;
static double rebuilt_at = -1.0;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void sleep_s(double s)
{
	struct timespec t = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
	nanosleep(&t, NULL);
}

/* ---- the window and its resizer ---------------------------------------- */

static void open_window(void)
{
	int scr;
	conn = xcb_connect(NULL, &scr);
	if (xcb_connection_has_error(conn)) {
		fprintf(stderr, "no X server\n");
		exit(1);
	}
	screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
	win = xcb_generate_id(conn);
	const uint32_t v[3] = { screen->black_pixel, XCB_GRAVITY_NORTH_WEST,
							XCB_EVENT_MASK_EXPOSURE |
							XCB_EVENT_MASK_STRUCTURE_NOTIFY };
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, 100, 100,
					  BASE_W, BASE_H, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
					  screen->root_visual,
					  XCB_CW_BACK_PIXEL | XCB_CW_BIT_GRAVITY |
					  XCB_CW_EVENT_MASK, v);
	static const char title[] = "wsidrag";
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME,
						XCB_ATOM_STRING, 8, sizeof title - 1, title);
	xcb_map_window(conn, win);
	xcb_flush(conn);
	xcb_generic_event_t *e;
	while ((e = xcb_wait_for_event(conn))) {
		const bool exposed = (e->response_type & 0x7f) == XCB_EXPOSE;
		free(e);
		if (exposed)
			break;
	}
	gfx = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(gfx)) {
		fprintf(stderr, "no second connection\n");
		exit(1);
	}
}

/* What the library's pump does between configures: every event read, the
 * last ConfigureNotify's size kept. */
static void drain(void)
{
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(conn))) {
		if ((e->response_type & 0x7f) == XCB_CONFIGURE_NOTIFY) {
			const xcb_configure_notify_event_t *c = (void *)e;
			atomic_store(&cfg_w, c->width);
			atomic_store(&cfg_h, c->height);
		}
		free(e);
	}
	/* What the driver decoded and nobody wants, as gfx_drain(): queued
	 * only, so this is never a second reader of that socket. */
	while ((e = xcb_poll_for_queued_event(gfx)))
		free(e);
}

/* Sleep in steps, the events drained and the hold honoured: once the render
 * loop is stuck the window stays the size it was stuck at, as the testbed's
 * did. */
static void pause_ms(int ms)
{
	const double end = now() + ms * 1e-3;
	while ((now() < end || atomic_load(&hold)) && !atomic_load(&quit)) {
		drain();
		sleep_s(0.004);
	}
}

/* All four, as x11.c configures an unmanaged window it is dragging. */
static void resize(int w, int h)
{
	const uint32_t v[4] = { 100, 100, (uint32_t)w, (uint32_t)h };
	xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
						 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
	xcb_flush(conn);
}

static void drag(int *w, int *h, int dw, int dh, int steps)
{
	const int w0 = *w, h0 = *h;
	for (int i = 1; i <= steps && !atomic_load(&quit); i++) {
		*w = w0 + dw * i / steps;
		*h = h0 + dh * i / steps;
		resize(*w, *h);
		pause_ms(STEP_MS);
	}
}

static void *resizer(void *arg)
{
	const int rounds = *(const int *)arg;
	for (int r = 0; r < rounds && !atomic_load(&quit); r++) {
		int w = BASE_W, h = BASE_H;
		drag(&w, &h, 160, 120, 12);
		pause_ms(SETTLE_MS);
		for (int i = 0; i < 8 && !atomic_load(&quit); i++) {
			const int d = i % 2 == 0 ? 40 : -40;
			drag(&w, &h, d, d, 6);
			pause_ms(SETTLE_MS);
		}
		pause_ms(HOLD_MS);
		resize(BASE_W, BASE_H);
		pause_ms(HOLD_MS / 2);
		fprintf(stderr, "round %d of %d done\n", r + 1, rounds);
	}
	atomic_store(&resizer_done, true);
	return NULL;
}

/* The rate test's resizer: a size every 1/rate s along a triangle, held
 * back in MODE_HELD until a frame at the last size applied has been
 * presented -- the latest size then, those between never the window's. */
typedef struct { double rate, seconds; } RateArg;

static void *rate_resizer(void *arg)
{
	const RateArg *ra = arg;
	const double t0 = now(), dt = 1.0 / ra->rate;
	int aw = BASE_W, ah = BASE_H;
	for (unsigned i = 1; !atomic_load(&quit); i++) {
		const double due = t0 + i * dt;
		if (due - t0 > ra->seconds)
			break;
		while (now() < due && !atomic_load(&quit)) {
			drain();
			sleep_s(0.001);
		}
		const double held_from = now();
		while (mode == MODE_HELD && !atomic_load(&quit) &&
			   (atomic_load(&shown_w) != aw || atomic_load(&shown_h) != ah)) {
			if (now() - held_from > 0.25) {
				atomic_fetch_add(&held_out, 1);
				break;
			}
			drain();
			sleep_s(0.0005);
		}
		/* Where the triangle stands now, not where step i was due: a held
		 * resizer skips the sizes it waited through. */
		const unsigned k = (unsigned)((now() - t0) / dt) % 40;
		const unsigned s = k < 20 ? k : 40 - k;
		const int w = BASE_W + (int)s * 8, h = BASE_H + (int)s * 6;
		if (w == aw && h == ah)
			continue;
		resize(w, h);
		aw = w;
		ah = h;
		atomic_fetch_add(&sizes_applied, 1);
	}
	pause_ms(100);
	atomic_store(&resizer_done, true);
	return NULL;
}

/* ---- Vulkan ------------------------------------------------------------- */

static bool gpu_wanted(const VkPhysicalDeviceProperties *p, const char *want)
{
	static const struct { const char *name; uint32_t vendor; } VENDORS[] = {
		{ "nvidia", 0x10de }, { "intel", 0x8086 }, { "amd", 0x1002 },
	};
	for (size_t i = 0; i < sizeof VENDORS / sizeof VENDORS[0]; i++)
		if (!strcmp(want, VENDORS[i].name))
			return p->vendorID == VENDORS[i].vendor;
	char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
	for (size_t i = 0; i < sizeof name; i++)
		name[i] = (char)tolower((unsigned char)p->deviceName[i]);
	return strstr(name, want) != NULL;
}

static void make_surface(void)
{
	VkXcbSurfaceCreateInfoKHR sci = {
		.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
		.connection = gfx, .window = win };
	CK(vkCreateXcbSurfaceKHR(inst, &sci, NULL, &surf));
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
	make_surface();

	VkPhysicalDevice all[8];
	uint32_t n = 8;
	VkResult r = vkEnumeratePhysicalDevices(inst, &n, all);
	if (r < 0 || n == 0) {
		fprintf(stderr, "no Vulkan device\n");
		exit(1);
	}
	const char *want = getenv("FNL_GPU");
	VkPhysicalDeviceProperties props;
	bool found = false;
	for (uint32_t i = 0; i < n && !found; i++) {
		vkGetPhysicalDeviceProperties(all[i], &props);
		if (want && *want && !gpu_wanted(&props, want))
			continue;
		uint32_t nf = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(all[i], &nf, NULL);
		VkQueueFamilyProperties qf[16];
		if (nf > 16)
			nf = 16;
		vkGetPhysicalDeviceQueueFamilyProperties(all[i], &nf, qf);
		for (uint32_t f = 0; f < nf && !found; f++) {
			VkBool32 ok = VK_FALSE;
			if (!(qf[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) ||
				vkGetPhysicalDeviceSurfaceSupportKHR(all[i], f, surf, &ok) < 0
				|| !ok)
				continue;
			pd = all[i];
			qfam = f;
			found = true;
		}
	}
	if (!found) {
		fprintf(stderr, "no device%s%s can present to this window\n",
				want ? " named " : "", want ? want : "");
		exit(1);
	}
	const uint32_t dv = props.driverVersion;
	if (props.vendorID == 0x10de)
		fprintf(stderr, "device: %s, driver %u.%u.%u\n", props.deviceName,
				dv >> 22, (dv >> 14) & 0xff, (dv >> 6) & 0xff);
	else
		fprintf(stderr, "device: %s, driver %#x\n", props.deviceName, dv);

	uint32_t nfmt = 0;
	CK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nfmt, NULL));
	VkSurfaceFormatKHR fmts[64];
	if (nfmt > 64)
		nfmt = 64;
	CK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nfmt, fmts));
	/* The library's preference, in its order (SRGB_FORMATS, device.c). */
	static const VkFormat SRGB[] = { VK_FORMAT_B8G8R8A8_SRGB,
									 VK_FORMAT_R8G8B8A8_SRGB,
									 VK_FORMAT_A8B8G8R8_SRGB_PACK32 };
	fmt = fmts[0];
	bool srgb = false;
	for (size_t w = 0; w < sizeof SRGB / sizeof SRGB[0] && !srgb; w++)
		for (uint32_t i = 0; i < nfmt && !srgb; i++)
			if (fmts[i].format == SRGB[w] &&
				fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
				fmt = fmts[i];
				srgb = true;
			}
	fprintf(stderr, "format %d\n", fmt.format);

	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
	const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
		.enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, qfam, 0, &q);
	VkCommandPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = qfam };
	CK(vkCreateCommandPool(dev, &pci, NULL, &pool));

	VkAttachmentDescription at = {
		.format = fmt.format, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
	VkAttachmentReference ref = {
		0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &ref };
	/* The acquire semaphore is waited at this stage: the layout change
	 * waits for it too. */
	VkSubpassDependency dep = {
		.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
	VkRenderPassCreateInfo rci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &at,
		.subpassCount = 1, .pSubpasses = &sub,
		.dependencyCount = 1, .pDependencies = &dep };
	CK(vkCreateRenderPass(dev, &rci, NULL, &pass));
}

static void chain_destroy(Chain *c)
{
	for (uint32_t i = 0; i < c->n; i++) {
		vkDestroyFramebuffer(dev, c->fb[i], NULL);
		vkDestroyImageView(dev, c->view[i], NULL);
		vkDestroySemaphore(dev, c->done[i], NULL);
	}
	if (c->sc)
		vkDestroySwapchainKHR(dev, c->sc, NULL);
	memset(c, 0, sizeof *c);
}

static void retired_destroy_all(void)
{
	if (nretired == 0)
		return;
	vkQueueWaitIdle(q);
	for (int i = 0; i < nretired; i++)
		chain_destroy(&retired[i]);
	nretired = 0;
}

/* After each submit: a retired swapchain two frames old is idle. One drain
 * for every entry that expires, as vk_retire_tick() does. */
static void retire_tick(void)
{
	bool drained = false;
	for (int i = nretired - 1; i >= 0; i--)
		if (--retired[i].countdown <= 0) {
			if (!drained)
				vkQueueWaitIdle(q);
			drained = true;
			chain_destroy(&retired[i]);
			retired[i] = retired[--nretired];
		}
}

/* In order of how much each tears down from IDLE on: rebuild() tests
 * `how >=` against IDLE, FRESH and SURFACE. */
typedef enum {
	AGAIN, EVENTS, TOUCH, WIGGLE, NUDGE, IDLE, FRESH, SURFACE, REST,
	CONNECTION, NESCAPES
} Escape;
static const char *const ESCAPE_NAMES[NESCAPES] = {
	"again", "events", "touch", "wiggle", "nudge", "idle", "fresh",
	"surface", "rest", "connection",
};

/* The swapchain made again, the library's way unless `how` says otherwise.
 * True when the extent is also the surface's currentExtent, as it was in
 * every rebuild of the testbed's stuck loop. */
static bool rebuild(Escape how)
{
	if (how == EVENTS) {
		free(xcb_get_geometry_reply(conn, xcb_get_geometry(conn, win), NULL));
		sleep_s(0.1);
	}
	/* The window configured at the size it has, which the server answers
	 * with a ConfigureNotify all the same. */
	if (how == TOUCH) {
		xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(conn,
			xcb_get_geometry(conn, win), NULL);
		if (g) {
			const uint32_t v[4] = { (uint32_t)g->x, (uint32_t)g->y,
									g->width, g->height };
			const uint16_t all = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
								 XCB_CONFIG_WINDOW_WIDTH |
								 XCB_CONFIG_WINDOW_HEIGHT;
			xcb_configure_window(conn, win, all, v);
			xcb_flush(conn);
			free(g);
		}
		free(xcb_get_geometry_reply(conn, xcb_get_geometry(conn, win), NULL));
		sleep_s(0.05);
	}
	/* A pixel wider and straight back, both sent before either is read. */
	if (how == WIGGLE) {
		const uint32_t w = (uint32_t)atomic_load(&cfg_w);
		const uint32_t v1[1] = { w + 1 }, v0[1] = { w };
		xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_WIDTH, v1);
		xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_WIDTH, v0);
		xcb_flush(conn);
		free(xcb_get_geometry_reply(conn, xcb_get_geometry(conn, win), NULL));
		sleep_s(0.05);
	}
	/* A pixel wider, and its ConfigureNotify read by the resizer's drain
	 * before the size is taken. */
	if (how == NUDGE) {
		const uint32_t v[1] = { (uint32_t)atomic_load(&cfg_w) + 1 };
		xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_WIDTH, v);
		xcb_flush(conn);
		free(xcb_get_geometry_reply(conn, xcb_get_geometry(conn, win), NULL));
		sleep_s(0.05);
	}
	if (how >= IDLE) {
		vkDeviceWaitIdle(dev);
		retired_destroy_all();
	}
	if (how >= FRESH && cur.sc)
		chain_destroy(&cur);
	if (how >= SURFACE) {
		vkDestroySurfaceKHR(inst, surf, NULL);
		if (how == REST)
			sleep_s(0.5);
		if (how == CONNECTION) {
			xcb_disconnect(gfx);
			gfx = xcb_connect(NULL, NULL);
			if (xcb_connection_has_error(gfx)) {
				fprintf(stderr, "no new connection\n");
				exit(1);
			}
		}
		make_surface();
		VkBool32 ok = VK_FALSE;
		CK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, qfam, surf, &ok));
	}
	if (nretired == MAX_RETIRED)
		retired_destroy_all();
	const double t = now();
	if (cur.sc && t - rebuilt_at > REST_AFTER) {
		vkQueueWaitIdle(q);
		sleep_s(REST_S);
	}
	rebuilt_at = now();

	VkSurfaceCapabilitiesKHR caps;
	CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
	/* The library's draw size, which is the last ConfigureNotify's, and not
	 * currentExtent (choose_extent()): the two disagree mid-drag. MODE_OOD
	 * takes currentExtent, as the TODO's alternative does. */
	VkExtent2D e = { (uint32_t)atomic_load(&cfg_w),
					 (uint32_t)atomic_load(&cfg_h) };
	if (mode == MODE_OOD && caps.currentExtent.width != 0xFFFFFFFFu)
		e = caps.currentExtent;
	const bool own = e.width == caps.currentExtent.width &&
					 e.height == caps.currentExtent.height;
	uint32_t want = caps.minImageCount < 2 ? 2 : caps.minImageCount;
	if (caps.maxImageCount > 0 && want > caps.maxImageCount)
		want = caps.maxImageCount;
	VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (!(caps.supportedCompositeAlpha & alpha))
		alpha = (VkCompositeAlphaFlagBitsKHR)
				(caps.supportedCompositeAlpha &
				 -caps.supportedCompositeAlpha);
	VkSwapchainCreateInfoKHR ci = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = surf,
		.minImageCount = want,
		.imageFormat = fmt.format,
		.imageColorSpace = fmt.colorSpace,
		.imageExtent = e,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = caps.currentTransform,
		.compositeAlpha = alpha,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR,
		.clipped = VK_TRUE,
		.oldSwapchain = cur.sc };
	Chain next = { .ext = e };
	CK(vkCreateSwapchainKHR(dev, &ci, NULL, &next.sc));
	if (cur.sc) {
		cur.countdown = FRAMES;
		retired[nretired++] = cur;
	}
	CK(vkGetSwapchainImagesKHR(dev, next.sc, &next.n, NULL));
	if (next.n > MAX_IMAGES) {
		fprintf(stderr, "%u images, more than %d\n", next.n, MAX_IMAGES);
		exit(1);
	}
	CK(vkGetSwapchainImagesKHR(dev, next.sc, &next.n, next.img));
	VkSemaphoreCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	for (uint32_t i = 0; i < next.n; i++) {
		CK(vkCreateSemaphore(dev, &sci, NULL, &next.done[i]));
		VkImageViewCreateInfo vci = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = next.img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = fmt.format,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
		CK(vkCreateImageView(dev, &vci, NULL, &next.view[i]));
		VkFramebufferCreateInfo fci = {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = pass, .attachmentCount = 1,
			.pAttachments = &next.view[i], .width = e.width,
			.height = e.height, .layers = 1 };
		CK(vkCreateFramebuffer(dev, &fci, NULL, &next.fb[i]));
	}
	cur = next;
	return own;
}

static void record_clear(VkCommandBuffer cb, uint32_t img, unsigned frame)
{
	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	CK(vkBeginCommandBuffer(cb, &bi));
	const float g = (float)(frame % 64) / 63.0f;
	VkClearValue c = { .color = { .float32 = { 0.1f, g, 0.4f, 1.0f } } };
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = pass, .framebuffer = cur.fb[img],
		.renderArea = { { 0, 0 }, cur.ext },
		.clearValueCount = 1, .pClearValues = &c };
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(cb);
	CK(vkEndCommandBuffer(cb));
}

/* What the server says the window is, beside what the swapchain was made
 * at: they agreed in the testbed's stuck loop. */
static void say_geometry(void)
{
	xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(conn,
		xcb_get_geometry(conn, win), NULL);
	fprintf(stderr, "  the server says %dx%d, the swapchain is %ux%u\n",
			g ? g->width : -1, g ? g->height : -1, cur.ext.width,
			cur.ext.height);
	free(g);
}

static int parse_escapes(const char *s, Escape *out)
{
	int n = 0;
	char buf[128];
	snprintf(buf, sizeof buf, "%s", s);
	for (char *tok = strtok(buf, ","); tok && n < 16;
		 tok = strtok(NULL, ",")) {
		int k = 0;
		while (k < NESCAPES && strcmp(tok, ESCAPE_NAMES[k]))
			k++;
		if (k == NESCAPES) {
			fprintf(stderr, "no escape named %s\n", tok);
			exit(1);
		}
		out[n++] = (Escape)k;
	}
	return n;
}

int main(int argc, char **argv)
{
	int rounds = argc >= 2 ? atoi(argv[1]) : 20;
	Escape order[16];
	const int norder = parse_escapes(argc >= 3 ? argv[2]
									 : "again,events,idle,fresh,surface,rest,"
									   "connection,touch,wiggle,nudge",
									 order);
	const int hold_ms = argc >= 4 ? atoi(argv[3]) : 0;
	if (rounds < 1 || norder < 1 || hold_ms < 0) {
		fprintf(stderr, "usage: wsidrag [rounds] [escape,...] [hold-ms]\n");
		return 1;
	}
	const char *rate_s = getenv("WSIDRAG_RATE");
	const char *secs_s = getenv("WSIDRAG_SECONDS");
	const char *mode_s = getenv("WSIDRAG_MODE");
	RateArg ra = { rate_s ? atof(rate_s) : 0.0, secs_s ? atof(secs_s) : 10.0 };
	if (mode_s && !strcmp(mode_s, "ood"))
		mode = MODE_OOD;
	else if (mode_s && !strcmp(mode_s, "held"))
		mode = MODE_HELD;
	else if (mode_s && strcmp(mode_s, "configure")) {
		fprintf(stderr, "no mode named %s\n", mode_s);
		return 1;
	}
	open_window();
	open_vulkan();

	VkFence fence[FRAMES];
	VkSemaphore acquired[FRAMES];
	VkCommandBuffer cb[FRAMES];
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
							  .flags = VK_FENCE_CREATE_SIGNALED_BIT };
	VkSemaphoreCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkCommandBufferAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = FRAMES };
	CK(vkAllocateCommandBuffers(dev, &ai, cb));
	for (int i = 0; i < FRAMES; i++) {
		CK(vkCreateFence(dev, &fci, NULL, &fence[i]));
		CK(vkCreateSemaphore(dev, &sci, NULL, &acquired[i]));
	}
	rebuild(AGAIN);

	pthread_t th;
	const double t_start = now();
	if (ra.rate > 0.0)
		pthread_create(&th, NULL, rate_resizer, &ra);
	else
		pthread_create(&th, NULL, resizer, &rounds);

	unsigned frames = 0, rebuilds = 0, ood_acquire = 0, ood_present = 0;
	unsigned suboptimal = 0, timeouts = 0, stuck = 0, unfreed = 0;
	unsigned freed_by[NESCAPES] = { 0 };
	int run = 0, longest = 0;   /* rebuilds in a row out of date at once */
	int esc = -1, tries = 0;    /* the escape being tried, while stuck */
	bool rebuild_due = false, fresh = true, own = true;
	unsigned f = 0;
	double stuck_at = 0.0;

	while (!atomic_load(&resizer_done) || esc >= 0) {
		/* A size published is a rebuild before any present says so, as
		 * the library's frame policy rebuilds on a new draw size. */
		if (mode != MODE_OOD &&
			((uint32_t)atomic_load(&cfg_w) != cur.ext.width ||
			 (uint32_t)atomic_load(&cfg_h) != cur.ext.height))
			rebuild_due = true;
		if (rebuild_due) {
			if (esc >= 0 && ++tries > TRIES) {
				fprintf(stderr, "  %s: still out of date after %d\n",
						ESCAPE_NAMES[order[esc]], TRIES);
				tries = 1;
				if (++esc == norder) {
					fprintf(stderr, "  nothing freed it\n");
					unfreed++;
					break;
				}
			}
			own = rebuild(esc >= 0 ? order[esc] : AGAIN);
			rebuilds++;
			rebuild_due = false;
			fresh = true;
		}
		CK(vkWaitForFences(dev, 1, &fence[f], VK_TRUE, UINT64_MAX));
		if (hold_ms)
			sleep_s(hold_ms * 1e-3);
		uint32_t img;
		const VkResult r = vkAcquireNextImageKHR(dev, cur.sc, ACQUIRE_NS,
												 acquired[f], VK_NULL_HANDLE,
												 &img);
		if (r == VK_TIMEOUT || r == VK_NOT_READY) {
			timeouts++;
			continue;
		}
		if (r == VK_ERROR_OUT_OF_DATE_KHR) {
			ood_acquire++;
			rebuild_due = true;
			if (!fresh || !own) {
				run = 0;
				continue;
			}
			if (++run > longest)
				longest = run;
			if (run == STUCK_AFTER && esc < 0) {
				stuck++;
				stuck_at = now();
				atomic_store(&hold, true);
				fprintf(stderr, "stuck: %d rebuilds in a row out of date at "
						"once, %u frames in\n", run, frames);
				say_geometry();
				esc = 0;
				tries = 0;
			}
			continue;
		}
		if (r < 0)
			CK(r);
		if (r == VK_SUBOPTIMAL_KHR) {
			suboptimal++;
			rebuild_due = true;
		}
		if (esc >= 0) {
			fprintf(stderr, "  freed by %s, rebuild %d of it, %.2f s on\n",
					ESCAPE_NAMES[order[esc]], tries,
					now() - stuck_at);
			freed_by[order[esc]]++;
			esc = -1;
			atomic_store(&hold, false);
		}
		run = 0;
		fresh = false;

		CK(vkResetFences(dev, 1, &fence[f]));
		record_clear(cb[f], img, frames);
		const VkPipelineStageFlags stage =
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo si = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = 1, .pWaitSemaphores = &acquired[f],
			.pWaitDstStageMask = &stage,
			.commandBufferCount = 1, .pCommandBuffers = &cb[f],
			.signalSemaphoreCount = 1, .pSignalSemaphores = &cur.done[img] };
		CK(vkQueueSubmit(q, 1, &si, fence[f]));
		retire_tick();
		VkPresentInfoKHR pi = {
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.waitSemaphoreCount = 1, .pWaitSemaphores = &cur.done[img],
			.swapchainCount = 1, .pSwapchains = &cur.sc,
			.pImageIndices = &img };
		const VkResult pr = vkQueuePresentKHR(q, &pi);
		if (pr == VK_ERROR_OUT_OF_DATE_KHR) {
			ood_present++;
			rebuild_due = true;
		} else if (pr == VK_SUBOPTIMAL_KHR) {
			suboptimal++;
			rebuild_due = true;
		} else if (pr < 0) {
			CK(pr);
		}
		if (pr >= 0) {
			if ((int)cur.ext.width != atomic_load(&shown_w) ||
				(int)cur.ext.height != atomic_load(&shown_h))
				atomic_fetch_add(&sizes_shown, 1);
			atomic_store(&shown_w, (int)cur.ext.width);
			atomic_store(&shown_h, (int)cur.ext.height);
		}
		frames++;
		f = (f + 1) % FRAMES;
	}

	atomic_store(&quit, true);
	atomic_store(&hold, false);
	pthread_join(th, NULL);
	vkDeviceWaitIdle(dev);

	fprintf(stderr, "%u frames, %u rebuilds; out of date %u at acquire and "
			"%u at present, suboptimal %u, timed out %u\n", frames, rebuilds,
			ood_acquire, ood_present, suboptimal, timeouts);
	fprintf(stderr, "longest run out of date at once: %d; stuck %u times",
			longest, stuck);
	for (int k = 0; k < NESCAPES; k++)
		if (freed_by[k])
			fprintf(stderr, ", freed by %s %u", ESCAPE_NAMES[k], freed_by[k]);
	if (unfreed)
		fprintf(stderr, ", not freed %u", unfreed);
	fprintf(stderr, "\n");
	if (ra.rate > 0.0) {
		const double t = now() - t_start;
		fprintf(stderr, "rate %s at %.0f sizes a second asked: %.1f frames "
				"a second, %.1f sizes applied a second, %.1f sizes shown a "
				"second%s\n", mode == MODE_OOD ? "ood" : mode == MODE_HELD
				? "held" : "configure", ra.rate, frames / t,
				atomic_load(&sizes_applied) / t,
				atomic_load(&sizes_shown) / t,
				atomic_load(&held_out) ? ", some held out" : "");
		if (atomic_load(&held_out))
			fprintf(stderr, "  held out %u times\n", atomic_load(&held_out));
	}
	return unfreed ? 3 : stuck ? 2 : 0;
}

/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The device: instance, physical and logical device, what the surface resolved
 * to, and the one pair of functions that builds and tears down everything
 * below the instance, for startup and loss recovery alike. Then the renderer
 * seam's table and the public Vulkan interop.
 */

#include "core/app.h"

#include <fenestal/fnl_vulkan.h>

#include "renderer/vulkan/vk_internal.h"

#include "core/dynlib.h"

/* ---- failure + allocation helpers -------------------------------------- */

/* Stringify through the enum name itself, so a typo cannot make the message
 * disagree with the constant it reports. */
#define VK_RESULT_CASE(x) case x: return #x

const char *vk_result_str(VkResult r)
{
	switch (r) {
	VK_RESULT_CASE(VK_SUCCESS);
	VK_RESULT_CASE(VK_NOT_READY);
	VK_RESULT_CASE(VK_TIMEOUT);
	VK_RESULT_CASE(VK_INCOMPLETE);
	VK_RESULT_CASE(VK_SUBOPTIMAL_KHR);
	VK_RESULT_CASE(VK_ERROR_OUT_OF_HOST_MEMORY);
	VK_RESULT_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY);
	VK_RESULT_CASE(VK_ERROR_INITIALIZATION_FAILED);
	VK_RESULT_CASE(VK_ERROR_DEVICE_LOST);
	VK_RESULT_CASE(VK_ERROR_MEMORY_MAP_FAILED);
	VK_RESULT_CASE(VK_ERROR_LAYER_NOT_PRESENT);
	VK_RESULT_CASE(VK_ERROR_EXTENSION_NOT_PRESENT);
	VK_RESULT_CASE(VK_ERROR_FEATURE_NOT_PRESENT);
	VK_RESULT_CASE(VK_ERROR_INCOMPATIBLE_DRIVER);
	VK_RESULT_CASE(VK_ERROR_FORMAT_NOT_SUPPORTED);
	VK_RESULT_CASE(VK_ERROR_UNKNOWN);
	VK_RESULT_CASE(VK_ERROR_SURFACE_LOST_KHR);
	VK_RESULT_CASE(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR);
	VK_RESULT_CASE(VK_ERROR_OUT_OF_DATE_KHR);
	default: return "unknown VkResult";
	}
}

#undef VK_RESULT_CASE

/* Both losses are recoverable and rebuild through one path (recover_from_loss
 * below): the format is resolved from the surface and the render pass and
 * pipelines are built on it, so a lost surface takes the device with it. */
bool vk_lost(App *a, VkResult r)
{
	if (r == VK_ERROR_DEVICE_LOST) {
		a->device_lost = true;
		return true;
	}
	if (r == VK_ERROR_SURFACE_LOST_KHR) {
		a->surface_lost = true;
		return true;
	}
	return false;
}

/* Memory exhaustion and loss get codes of their own because a caller can act
 * on them; everything else is FNL_ERR_SYSTEM with the detail in the message.
 * VK_SUCCESS records nothing, which is what lets this wrap a call. */
FnlResult fnl_vk_result(const char *expr, const char *file, int line,
						VkResult r)
{
	FnlResult code;

	if (r == VK_SUCCESS)
		return FNL_OK;

	switch (r) {
	case VK_ERROR_OUT_OF_HOST_MEMORY:
	case VK_ERROR_OUT_OF_DEVICE_MEMORY:
		code = FNL_ERR_OUT_OF_MEMORY;
		break;
	case VK_ERROR_DEVICE_LOST:
	case VK_ERROR_SURFACE_LOST_KHR:
		code = FNL_ERR_DEVICE_LOST;
		break;
	default:
		code = FNL_ERR_SYSTEM;
		break;
	}
	return fnl_fail(code, "%s:%d: %s\n  returned %s (%d)", file, line, expr,
					vk_result_str(r), (int)r);
}

/* For Vulkan's enumerate-then-fill queries. A zero request is rounded up, so
 * NULL always means failure and never "you asked for nothing". */
static void *xmalloc(size_t n)
{
	return malloc(n ? n : 1);
}

static FnlResult out_of_memory(const char *what)
{
	return fnl_fail(FNL_ERR_OUT_OF_MEMORY, "out of memory allocating %s", what);
}

/* The instance extensions the loader offers, asked once an instance: each
 * asking opens every driver's library, and NVIDIA's, with its kernel
 * module not loaded -- the GPU on nouveau -- tries to load it and waits
 * about 2 s each time (580.196, schenker), which a dozen askings made a
 * move to nouveau's GPU of 30 s. A list it cannot give is empty, which
 * reads as offering nothing and leaves every optional extension off. */
typedef struct {
	VkExtensionProperties *props;
	uint32_t               n;
} InstanceExts;

static InstanceExts instance_exts(void)
{
	InstanceExts e = { NULL, 0 };
	uint32_t n = 0;
	if (vkEnumerateInstanceExtensionProperties(NULL, &n, NULL) != VK_SUCCESS
		|| n == 0)
		return e;
	e.props = calloc(n, sizeof *e.props);
	if (!e.props)
		return e;
	if (vkEnumerateInstanceExtensionProperties(NULL, &n, e.props) >= 0)
		e.n = n;
	return e;
}

/* The drivers an instance made across a move to another kernel driver is
 * made of (App::gpu_cross, DESIGN *Drawn across the move*): Mesa's for
 * Intel's GPUs, the older ones', AMD's and its CPU rasteriser's, each of
 * which opens no GPU of another vendor's. Opened by the library itself and
 * handed to the loader in its exclusive mode
 * (VK_LUNARG_direct_driver_loading), so that no other driver is loaded --
 * NVK and NVIDIA's would open the GPU being moved. By the names the
 * drivers are installed under on the library path, Debian's and Arch's
 * alike, which saves reading their manifests. Opened once, on the render
 * thread, and never closed (core/dynlib.h). */
static const char *const CROSS_LIBS[] = {
	"libvulkan_intel.so", "libvulkan_intel_hasvk.so", "libvulkan_radeon.so",
	"libvulkan_lvp.so",
};
#define CROSS_MAX (sizeof CROSS_LIBS / sizeof CROSS_LIBS[0])

static PFN_vkGetInstanceProcAddrLUNARG cross_gipa[CROSS_MAX];
static uint32_t cross_n;

static uint32_t cross_drivers(void)
{
	static bool opened;
	if (opened)
		return cross_n;
	opened = true;
	for (size_t i = 0; i < CROSS_MAX; i++) {
		const char *names[] = { CROSS_LIBS[i], NULL };
		char why[256];
		void *lib = fnl_lib_open(names, why, sizeof why);
		const FnlLibFn f = lib ? fnl_lib_sym(lib, "vk_icdGetInstanceProcAddr")
							   : NULL;
		if (f)
			cross_gipa[cross_n++] = (PFN_vkGetInstanceProcAddrLUNARG)f;
	}
	return cross_n;
}

/* What those drivers offer between them, asked of each directly: the
 * loader's own asking would load every driver installed. The loader's
 * extension is not among them and is asked for on its own. */
static InstanceExts cross_exts(void)
{
	InstanceExts e = { NULL, 0 };
	for (uint32_t d = 0; d < cross_drivers(); d++) {
		const PFN_vkEnumerateInstanceExtensionProperties get =
			(PFN_vkEnumerateInstanceExtensionProperties)cross_gipa[d](
				VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
		uint32_t n = 0;
		/* A count no driver comes near, and a size that cannot wrap. */
		if (!get || get(NULL, &n, NULL) != VK_SUCCESS || n == 0 ||
			n > 1024)
			continue;
		VkExtensionProperties *more =
			realloc(e.props, (e.n + n) * sizeof *e.props);
		if (!more)
			break;
		e.props = more;
		if (get(NULL, &n, e.props + e.n) >= 0)
			e.n += n;
	}
	return e;
}

static bool instance_has(const InstanceExts *e, const char *name)
{
	for (uint32_t i = 0; i < e->n; i++)
		if (strcmp(e->props[i].extensionName, name) == 0)
			return true;
	return false;
}

/* One per backend this build has; see VkBridge. */
#define BRIDGE_ENTRY(key) &vk_bridge_##key,
static const VkBridge *const BRIDGES[] = { FNL_BACKENDS(BRIDGE_ENTRY) };
#undef BRIDGE_ENTRY

static FnlResult make_instance(App *a, struct RendererState *g)
{
	for (size_t i = 0; i < sizeof BRIDGES / sizeof BRIDGES[0]; i++)
		if (BRIDGES[i]->backend == BACKEND(a)->id)
			g->bridge = BRIDGES[i];
	if (!g->bridge)
		return fnl_fail(FNL_ERR_UNSUPPORTED, "the Vulkan renderer has no "
						"bridge to %s in this build", BACKEND(a)->name);
	FNL_TRY(vk_load_global());

	/* The library's name, not the caller's: a driver matches its application
	 * profiles against this string, and FnlInitDesc::title is user-facing
	 * prose. */
	VkApplicationInfo app_info = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "fenestal",
		.apiVersion = VK_API_VERSION_1_0,
	};
	/* The surface extensions are the backend's: this file must not know
	 * which window server it presents to. */
	uint32_t n_backend = 0, required = 0;
	const char *const *backend =
		g->bridge->instance_extensions(&n_backend, &required);
	const char *exts[VK_BACKEND_EXTENSIONS_MAX + 4];
	uint32_t n = 0;
	exts[n++] = VK_KHR_SURFACE_EXTENSION_NAME;
	/* Across a move, of the drivers that open nothing of the GPU moved
	 * alone (cross_drivers()); a loader too old for that refuses the
	 * extension, and the move is then made with the frames paused. */
	const bool cross = a->gpu_cross;
	VkDirectDriverLoadingInfoLUNARG direct[CROSS_MAX];
	VkDirectDriverLoadingListLUNARG direct_list = {
		.sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG,
		.mode = VK_DIRECT_DRIVER_LOADING_MODE_EXCLUSIVE_LUNARG,
		.pDrivers = direct,
	};
	if (cross) {
		if (cross_drivers() == 0)
			return fnl_fail(FNL_ERR_NO_DEVICE, "none of Mesa's Intel, AMD "
							"or CPU Vulkan drivers is installed");
		for (uint32_t i = 0; i < cross_n; i++)
			direct[i] = (VkDirectDriverLoadingInfoLUNARG){
				.sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_INFO_LUNARG,
				.pfnGetInstanceProcAddr = cross_gipa[i],
			};
		direct_list.driverCount = cross_n;
		exts[n++] = VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME;
	}
	/* The instance is 1.0, where a device's extended features are asked
	 * through this extension: present wait's, here (create_device()).
	 * Optional; without it present wait is simply not asked for. */
	InstanceExts offered = cross ? cross_exts() : instance_exts();
	const bool props2 = instance_has(
		&offered, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
	if (props2)
		exts[n++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
	/* A driver that implements Vulkan on another API -- MoltenVK, on Metal
	 * -- is listed by the loader only to an instance that says it will take
	 * one, and none is to be had on a Mac otherwise. Asked for wherever the
	 * loader offers it, which names no system: no Linux or Windows driver
	 * is one, and the list there is what it was. */
	const bool portability =
		instance_has(&offered, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
	if (portability)
		exts[n++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
	FNL_ASSERT(n_backend <= VK_BACKEND_EXTENSIONS_MAX);
	if (n_backend > VK_BACKEND_EXTENSIONS_MAX)
		n_backend = VK_BACKEND_EXTENSIONS_MAX;
	/* A required one is asked for whether or not the loader lists it, so
	 * that its absence is the instance's refusal and says which. */
	g->backend_ext_on = 0;
	for (uint32_t i = 0; i < n_backend; i++)
		if (i < required || instance_has(&offered, backend[i])) {
			exts[n++] = backend[i];
			if (i >= required)
				g->backend_ext_on |= 1u << (i - required);
		}
	free(offered.props);
	VkInstanceCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pNext = cross ? &direct_list : NULL,
		.flags = portability
					 ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0,
		.pApplicationInfo = &app_info,
		.enabledExtensionCount = n,
		.ppEnabledExtensionNames = exts,
	};
	/* Khronos' validation layer in this instance alone, where
	 * FNL_VK_VALIDATION=1 asks: what a checker round claims is this
	 * library's Vulkan, and a caller's scene's, which is drawn on this
	 * instance. The loader's VK_LOADER_LAYERS_ENABLE puts the layer in every
	 * instance of the process -- Zink's and vkd3d-proton's, whose calls are
	 * Mesa's and wine's -- and a lock-order inversion of the layer's own,
	 * in Zink's instance, failed every ThreadSanitizer round that drew with
	 * OpenGL on NVK. Named, not looked for first: a loader without it
	 * refuses the instance, which is made again without it, and said. */
	static const char *const VALIDATION[] = { "VK_LAYER_KHRONOS_validation" };
	const char *val = getenv("FNL_VK_VALIDATION");
	if (val && val[0] == '1' && !val[1]) {
		ci.enabledLayerCount = 1;
		ci.ppEnabledLayerNames = VALIDATION;
	}
	/* A loader with no driver beneath it answers this, the usual way a
	 * machine says it has no Vulkan: no device, which fnl_init() steps
	 * past, where any other refusal is a system error. No driver is also
	 * one whose own libraries will not load -- Mesa's link
	 * libwayland-client and libxcb -- so the line does not say none is
	 * installed. Said once a process: the GPU listing asks too. */
	VkResult made = vkCreateInstance(&ci, NULL, &g->instance);
	if (made == VK_ERROR_LAYER_NOT_PRESENT && ci.enabledLayerCount) {
		fnl_log(FNL_LOG_WARN, "FNL_VK_VALIDATION: the loader has no "
				"VK_LAYER_KHRONOS_validation; the instance is made "
				"without it");
		ci.enabledLayerCount = 0;
		ci.ppEnabledLayerNames = NULL;
		made = vkCreateInstance(&ci, NULL, &g->instance);
	}
	if (made == VK_ERROR_INCOMPATIBLE_DRIVER) {
		static FnlAtomicBool said;
		if (fnl_atomic_load_bool(&said))
			return fnl_fail_quiet(FNL_ERR_NO_DEVICE, "no Vulkan driver "
								  "would load: vkCreateInstance returned "
								  "VK_ERROR_INCOMPATIBLE_DRIVER");
		fnl_atomic_store_bool(&said, true);
		return fnl_fail(FNL_ERR_NO_DEVICE, "no Vulkan driver would load: "
						"vkCreateInstance returned "
						"VK_ERROR_INCOMPATIBLE_DRIVER");
	}
	FNL_TRY(fnl_vk_result("vkCreateInstance", __FILE__, __LINE__, made));
	/* On a refusal vkDestroyInstance is this instance's or NULL (vk_load.c
	 * asks for it first), never an earlier instance's. */
	const FnlResult loaded = vk_load_instance(g->instance);
	if (loaded < 0) {
		if (fnl_vkDestroyInstance)
			vkDestroyInstance(g->instance, NULL);
		g->instance = VK_NULL_HANDLE;
		return loaded;
	}
	g->get_features2 = props2
		? (PFN_vkGetPhysicalDeviceFeatures2KHR)vkGetInstanceProcAddr(
			  g->instance, "vkGetPhysicalDeviceFeatures2KHR")
		: NULL;
	g->get_memory2 = props2
		? (PFN_vkGetPhysicalDeviceMemoryProperties2KHR)vkGetInstanceProcAddr(
			  g->instance, "vkGetPhysicalDeviceMemoryProperties2KHR")
		: NULL;
	return FNL_OK;
}

/* The seam's first entry, so the renderer's state is born here: zeroed, which
 * is every handle VK_NULL_HANDLE. app.c calls destroy_instance() only once
 * this has succeeded, so a failure frees the state itself; make_instance()
 * leaves no instance behind a failure of its own, destroying the one it
 * made where the loader then refuses an entry. */
static FnlResult create_instance(App *a)
{
	FNL_ASSERT(!a->renderer_state);
	a->renderer_state = calloc(1, sizeof *a->renderer_state);
	if (!a->renderer_state)
		return out_of_memory("the Vulkan renderer's state");
	FnlResult rc = make_instance(a, a->renderer_state);
	if (rc < 0) {
		free(a->renderer_state);
		a->renderer_state = NULL;
	}
	return rc;
}

/* The seam's bridge entry: this backend's. */
static FnlResult create_surface(App *a)
{
	return VK(a)->bridge->create_surface(a);
}

/* Steps of two, so that a device passed over (device_passed_over()) ranks
 * between an integrated GPU and a virtual one. */
static int device_type_rank(VkPhysicalDeviceType t)
{
	switch (t) {
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 8;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 6;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 4;
	case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 2;
	default:                                     return 0;
	}
}

/* For the startup inventory: a window rendering on a software device should
 * be readable off a log. */
static const char *device_type_name(VkPhysicalDeviceType t)
{
	switch (t) {
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
	case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "software";
	default:                                     return "other";
	}
}

/* One queue family that can both render and present keeps the whole engine
 * single-queue; a family split would need ownership transfers. */
int vk_find_present_queue(VkPhysicalDevice dev, VkSurfaceKHR surface)
{
	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, NULL);
	VkQueueFamilyProperties *qprops = xmalloc(qn * sizeof(*qprops));
	if (!qprops)
		return -1;      /* reads as "this device has no present queue", which
						 * is the right answer to give a device we cannot ask */
	vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, qprops);
	int found = -1;
	for (uint32_t q = 0; q < qn; q++) {
		VkBool32 present = surface == VK_NULL_HANDLE;
		if (surface != VK_NULL_HANDLE)
			vkGetPhysicalDeviceSurfaceSupportKHR(dev, q, surface, &present);
		if ((qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
			found = (int)q;
			break;
		}
	}
	free(qprops);
	return found;
}

/* What fnl_gpu_kind_at() says of a device of this type. */
static FnlGpuKind device_kind(VkPhysicalDeviceType t)
{
	switch (t) {
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return FNL_GPU_KIND_INTEGRATED;
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return FNL_GPU_KIND_DISCRETE;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return FNL_GPU_KIND_VIRTUAL;
	case VK_PHYSICAL_DEVICE_TYPE_CPU:            return FNL_GPU_KIND_SOFTWARE;
	default:                                     return FNL_GPU_KIND_UNKNOWN;
	}
}

/* NVK, Mesa's NVIDIA driver, on a GPU older than Turing: below an integrated
 * GPU in the automatic choice, and chosen only by name or index. Before
 * Turing there is no GSP firmware for the kernel to reclock with, so the GPU
 * runs at its boot clocks, and on Pascal (a GTX 1050 Ti, kernel 7.2.9)
 * nouveau's runtime suspend garbles a tiled image left in VRAM, which is
 * why a device chosen by name anyway keeps the glyph atlas out of it
 * (DESIGN *The startup inventory*). Told by NVK's own
 * name for its devices, "(NVK GP107)", and by the PCI ID: Turing's start
 * at 0x1e00. */
static bool device_passed_over(const VkPhysicalDeviceProperties *p)
{
	return p->vendorID == 0x10de && p->deviceID < 0x1e00 &&
		   app_name_has(p->deviceName, "(NVK ");
}

static FnlResult pick_device(App *a)
{
	uint32_t n = 0;
	VK_TRY(vkEnumeratePhysicalDevices(VK(a)->instance, &n, NULL));
	if (n == 0)
		return fnl_fail(FNL_ERR_NO_DEVICE, "no Vulkan physical devices found");
	VkPhysicalDevice *devs = xmalloc(n * sizeof(*devs));
	if (!devs)
		return out_of_memory("the physical device list");
	/* Not VK_TRY: `devs` is owned from here, and the second enumerate can fail
	 * on its own (a device removed between the two calls). */
	FnlResult rc = VKR(vkEnumeratePhysicalDevices(VK(a)->instance, &n, devs));
	if (rc < 0) {
		free(devs);
		return rc;
	}

	VkPhysicalDevice best = VK_NULL_HANDLE;
	int best_q = -1, best_rank = -1;
	/* FnlInitDesc::gpu_index counts the devices listed -- those that can
	 * present to the surface, or those the bridge says can draw the window
	 * (VkBridge::can_draw) -- so it indexes the list the log prints. A
	 * name is asked of the same list, and the first device carrying it is
	 * the one: two cards of one model are told apart by the index. */
	VkPhysicalDevice chosen = VK_NULL_HANDLE;
	int chosen_q = -1, seen = 0;
	const bool by_name = a->gpu_want[0] != '\0';
	/* The same list, for fnl_gpu_count() and the titlebar: entry i is
	 * gpu_index i + 1. A device that can present another way
	 * (VkBridge::shares) is two entries, the plain one and then that way,
	 * and an index names one of them. */
	GpuEntry *list = VK(a)->pick_list;
	int best_i = -1, chosen_i = -1;
	VK(a)->pick_via = -1;

	for (uint32_t i = 0; i < n; i++) {
		/* A surface made on one device's display is that device's alone,
		 * whatever the others say they can present to. */
		const int q = VK(a)->surface_phys != VK_NULL_HANDLE &&
							  devs[i] != VK(a)->surface_phys
						  ? -1
						  : vk_find_present_queue(devs[i], VK(a)->vk_surface);
		/* Listed by the bridge's word where it has one (VkBridge::
		 * can_draw), though this surface may not be the device's; chosen
		 * only where the surface is. */
		const bool listed = VK(a)->bridge->can_draw
								? VK(a)->bridge->can_draw(a, devs[i])
								: q >= 0;
		if (!listed)
			continue;
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(devs[i], &props);
		/* gpu_index is 0 for automatic and seen starts at 1, so the automatic
		 * case never matches and needs no branch of its own. */
		seen++;
		if (q >= 0 &&
			(by_name ? chosen == VK_NULL_HANDLE &&
						   app_name_has(props.deviceName, a->gpu_want)
					 : seen == a->gpu_index)) {
			chosen = devs[i];
			chosen_q = q;
			chosen_i = seen - 1;
		}
		if (seen <= GPU_LIST_MAX) {
			GpuEntry *e = &list[seen - 1];
			memset(e, 0, sizeof *e);
			snprintf(e->name, sizeof e->name, "%s", props.deviceName);
			e->kind = device_kind(props.deviceType);
			e->renderer = FNL_RENDERER_VULKAN;
		}
		if (!a->gpu_cross)
			app_gpu_inventory(a, seen - 1, props.deviceName,
							  device_type_name(props.deviceType));
		const int plain = seen - 1;
		if (VK(a)->bridge->shares &&
			VK(a)->bridge->shares(VK(a)->instance, devs[i])) {
			seen++;
			if (q >= 0 && !by_name && seen == a->gpu_index) {
				chosen = devs[i];
				chosen_q = q;
				chosen_i = seen - 1;
				VK(a)->pick_via = 1;
			}
			if (seen <= GPU_LIST_MAX) {
				list[seen - 1] = list[plain];
				snprintf(list[seen - 1].via, sizeof list[seen - 1].via,
						 "D3D11");
				/* The way vk_dxgi_choose() takes unasked: through DXGI
				 * wherever the device shares, unless FNL_VK_DXGI=0. */
				const bool via = vk_dxgi_env() != 0;
				list[seen - 1].way_default = via;
				if (plain < GPU_LIST_MAX)
					list[plain].way_default = !via;
			}
			if (!a->gpu_cross)
				app_gpu_inventory(a, seen - 1, props.deviceName,
								  device_type_name(props.deviceType));
		}
		if (chosen_i == plain && !by_name)
			VK(a)->pick_via = 0;
		if (q < 0)
			continue;

		int rank = device_type_rank(props.deviceType);
		if (device_passed_over(&props) &&
			props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			rank = device_type_rank(VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) - 1;
			fnl_log(FNL_LOG_INFO, "%s is ranked below an integrated one: "
					"NVK on a GPU before Turing runs at its boot clocks; "
					"FNL_GPU names it to choose it anyway",
					props.deviceName);
		}
		if (rank > best_rank) {
			best = devs[i];
			best_q = q;
			best_rank = rank;
			best_i = plain;
		}
	}
	free(devs);

	if (best == VK_NULL_HANDLE)
		return fnl_fail(FNL_ERR_NO_DEVICE,
						"no Vulkan device can present to this surface");

	/* An index past the end degrades to the automatic choice, with one line
	 * so the caller can tell. */
	if (by_name && chosen == VK_NULL_HANDLE)
		fnl_log(FNL_LOG_WARN,
				"no GPU named \"%s\" among the %d that can present to this "
				"surface", a->gpu_want, seen);
	else if (a->gpu_index > 0 && chosen == VK_NULL_HANDLE)
		fnl_log(FNL_LOG_WARN,
				"no GPU %d: %d device%s can present to this surface",
				a->gpu_index, seen, seen == 1 ? "" : "s");
	if (chosen != VK_NULL_HANDLE) {
		best = chosen;
		best_q = chosen_q;
		best_i = chosen_i;
	}
	/* Across a move the instance has a few of the drivers and lists a
	 * few of the GPUs: the list and the inventory stay as they were for
	 * the second or two until the window is on the GPU moved. */
	VK(a)->pick_publish = !a->gpu_cross;
	VK(a)->pick_n = seen < GPU_LIST_MAX ? seen : GPU_LIST_MAX;
	VK(a)->pick_cur = best_i;

	VK(a)->phys = best;
	VK(a)->queue_family = (uint32_t)best_q;

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(VK(a)->phys, &props);
	VK(a)->phys_software = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
	VK(a)->phys_vram_unkept = device_passed_over(&props);
	/* Under the lock: recovery re-picks on the render thread while
	 * fnl_gpu_name() may be reading from another. */
	fnl_mutex_lock(&a->lock);
	snprintf(a->gpu_name, sizeof a->gpu_name, "%s", props.deviceName);
	fnl_mutex_unlock(&a->lock);

	/* What FSAA multisamples at: the best of 4x and 2x that colour and depth
	 * both offer, since they share a framebuffer. VK_SAMPLE_COUNT_1_BIT means
	 * neither, which fnl_max_samples() reports. */
	VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts &
								props.limits.framebufferDepthSampleCounts;
	if (FNL_FAULT("no-msaa"))
		counts = VK_SAMPLE_COUNT_1_BIT;
	fnl_atomic_store_u32(&a->msaa_samples,
		(counts & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT
	  : (counts & VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT
										 : VK_SAMPLE_COUNT_1_BIT);
	return FNL_OK;
}

/* Whether pick_device() would list `dev`, asked with no surface: the
 * bridge's can_draw where it has one, else whether a graphics queue family
 * presents to the window server (VkBridge::presents), else whether there
 * is a graphics queue at all. */
static bool listed_without_surface(App *a, const struct RendererState *g,
								   VkPhysicalDevice dev)
{
	const VkBridge *b = g->bridge;
	if (b->can_draw)
		return b->can_draw(a, dev);
	if (!b->presents)
		return vk_find_present_queue(dev, VK_NULL_HANDLE) >= 0;
	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, NULL);
	VkQueueFamilyProperties *qprops = xmalloc(qn * sizeof *qprops);
	if (!qprops)
		return false;
	vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, qprops);
	bool yes = false;
	for (uint32_t q = 0; q < qn && !yes; q++)
		yes = (qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
			  b->presents(a, g->instance, dev, q);
	free(qprops);
	return yes;
}

/* The seam's list_gpus, on a thread of its own while another renderer
 * draws (app_gpu_list_others()): an instance made in a state of this
 * renderer's own, never App::renderer_state, which is the other one's and
 * in use. The instance-level entry points it loads are this renderer's
 * alone, unused while another draws, and loaded again by the next
 * instance; the list is published only once this one is destroyed, so no
 * move to Vulkan can start meanwhile. */
static int vk_list_gpus(App *a, GpuEntry *out, int max)
{
	struct RendererState *g = calloc(1, sizeof *g);
	if (!g)
		return 0;
	int n = 0;
	VkPhysicalDevice devs[16];
	uint32_t c = 16;
	if (make_instance(a, g) == FNL_OK) {
		if (vkEnumeratePhysicalDevices(g->instance, &c, devs) < 0)
			c = 0;
		for (uint32_t i = 0; i < c && n < max; i++) {
			if (!listed_without_surface(a, g, devs[i]))
				continue;
			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(devs[i], &props);
			memset(&out[n], 0, sizeof out[n]);
			snprintf(out[n].name, sizeof out[n].name, "%s",
					 props.deviceName);
			out[n].kind = device_kind(props.deviceType);
			out[n].renderer = FNL_RENDERER_VULKAN;
			n++;
			/* As pick_device() lists it: the same GPU again, that way, and
			 * of the two the one vk_dxgi_choose() takes unasked marked so,
			 * which is what the ranking moves to. */
			if (g->bridge->shares && n < max &&
				g->bridge->shares(g->instance, devs[i])) {
				const bool via = vk_dxgi_env() != 0;
				out[n - 1].way_default = !via;
				out[n] = out[n - 1];
				snprintf(out[n].via, sizeof out[n].via, "D3D11");
				out[n].way_default = via;
				n++;
			}
		}
		vkDestroyInstance(g->instance, NULL);
	}
	free(g);
	return n;
}

/* Whether the device offers an extension, asked of the driver: a device
 * extension the device lacks fails vkCreateDevice outright. */
static bool device_has_extension(VkPhysicalDevice phys, const char *name)
{
	uint32_t n = 0;
	if (vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL) !=
		VK_SUCCESS || n == 0)
		return false;
	VkExtensionProperties *props = xmalloc(n * sizeof *props);
	if (!props)
		return false;
	bool found = false;
	if (vkEnumerateDeviceExtensionProperties(phys, NULL, &n, props) ==
		VK_SUCCESS)
		for (uint32_t i = 0; i < n && !found; i++)
			found = strcmp(props[i].extensionName, name) == 0;
	free(props);
	return found;
}

#ifdef VK_EXT_present_timing
/* Whether the device reads CLOCK_MONOTONIC beside its own clocks, which
 * a present stage's own clock is calibrated onto (vk_timing_calibrate()):
 * a domain the device does not list is not to be asked for. */
static bool calibrates_monotonic(VkInstance inst, VkPhysicalDevice phys)
{
	PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR ask =
		(PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)
			vkGetInstanceProcAddr(
				inst, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR");
	VkTimeDomainKHR dom[8];
	uint32_t n = 8;
	if (!ask)
		return false;
	const VkResult r = ask(phys, &n, dom);
	if (r != VK_SUCCESS && r != VK_INCOMPLETE)
		return false;
	for (uint32_t i = 0; i < n && i < 8; i++)
		if (dom[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR)
			return true;
	return false;
}
#endif

static FnlResult create_device(App *a)
{
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = VK(a)->queue_family,
		.queueCount = 1,
		.pQueuePriorities = &prio,
	};

	/* Dual-source blending is what gives text a weight per colour channel (see
	 * shaders/text.frag). Optional, so asked for rather than assumed: text.c
	 * builds the grayscale variant without it. */
	VkPhysicalDeviceFeatures have;
	vkGetPhysicalDeviceFeatures(VK(a)->phys, &have);
	VK(a)->dual_src_blend = have.dualSrcBlend && !FNL_FAULT("no-dual-src");
	VkPhysicalDeviceFeatures want = { .dualSrcBlend = VK(a)->dual_src_blend };

	/* Present wait, where both extensions and both features are there:
	 * optional, and only the X11 resize handshake uses it. */
	VkPhysicalDevicePresentWaitFeaturesKHR wait_have = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
	};
	VkPhysicalDevicePresentIdFeaturesKHR id_have = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
		.pNext = &wait_have,
	};
	VkPhysicalDeviceFeatures2 have2 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &id_have,
	};
	const bool wait_exts =
		VK(a)->get_features2 &&
		device_has_extension(VK(a)->phys, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
		device_has_extension(VK(a)->phys, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
	if (wait_exts)
		VK(a)->get_features2(VK(a)->phys, &have2);
	VK(a)->present_wait = wait_exts && id_have.presentId &&
						  wait_have.presentWait;
	VkPhysicalDevicePresentWaitFeaturesKHR wait_want = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
		.presentWait = VK_TRUE,
	};
	VkPhysicalDevicePresentIdFeaturesKHR id_want = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
		.pNext = &wait_want,
		.presentId = VK_TRUE,
	};

	const char *exts[16];
	uint32_t n_exts = 0;
	exts[n_exts++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	VK(a)->dxgi_exts = false;
	if (VK(a)->bridge->device_extensions) {
		uint32_t k = 0;
		const char *const *more = VK(a)->bridge->device_extensions(&k);
		uint32_t on = 0;
		for (uint32_t i = 0; i < k && n_exts < 10; i++)
			if (device_has_extension(VK(a)->phys, more[i])) {
				exts[n_exts++] = more[i];
				on++;
			}
		VK(a)->dxgi_exts = k > 0 && on == k;
	}
	if (VK(a)->present_wait) {
		exts[n_exts++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
		exts[n_exts++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
	}
	/* The driver's own present stamps, where the bridge asks for them:
	 * the extension and the three it depends on, its feature, and the
	 * present ids of present_id2's, which its results are keyed by.
	 * Optional; without them no frame of the display path is stamped. */
	VK(a)->present_timing = VK(a)->timing_calibrates = false;
	/* Wanted where the bridge has the entry at all; asked of
	 * VK_EXT_present_timing only where it says the instance can, since
	 * VK_GOOGLE_display_timing, the fallback, rests on nothing of it. */
	const bool stamps_wanted = VK(a)->bridge->driver_stamps != NULL;
	const bool stamps_asked = stamps_wanted &&
							  VK(a)->bridge->driver_stamps(a);
#ifdef VK_EXT_present_timing
	VkPhysicalDevicePresentTimingFeaturesEXT timing_have = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
	};
	VkPhysicalDevicePresentId2FeaturesKHR id2_have = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR,
		.pNext = &timing_have,
	};
	VkPhysicalDeviceFeatures2 have3 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &id2_have,
	};
	if (VK(a)->get_features2 && stamps_asked &&
		device_has_extension(VK(a)->phys,
							 VK_EXT_PRESENT_TIMING_EXTENSION_NAME) &&
		device_has_extension(VK(a)->phys,
							 VK_KHR_PRESENT_ID_2_EXTENSION_NAME) &&
		device_has_extension(VK(a)->phys,
							 VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) &&
		!FNL_FAULT("no-present-timing")) {
		VK(a)->get_features2(VK(a)->phys, &have3);
		VK(a)->present_timing = id2_have.presentId2 &&
								timing_have.presentTiming;
	}
	VkPhysicalDevicePresentTimingFeaturesEXT timing_want = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,
		.pNext = VK(a)->present_wait ? (void *)&id_want : NULL,
		.presentTiming = VK_TRUE,
	};
	VkPhysicalDevicePresentId2FeaturesKHR id2_want = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR,
		.pNext = &timing_want,
		.presentId2 = VK_TRUE,
	};
	if (VK(a)->present_timing) {
		exts[n_exts++] = VK_KHR_PRESENT_ID_2_EXTENSION_NAME;
		exts[n_exts++] = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
		exts[n_exts++] = VK_EXT_PRESENT_TIMING_EXTENSION_NAME;
	}
#endif
	/* Else VK_GOOGLE_display_timing, its older sibling, which names no
	 * clock: where the bridge says whose its times are (MoltenVK's). Its
	 * present ids are its own, carried in the same struct as its times. */
	VK(a)->google_timing = !VK(a)->present_timing && stamps_wanted &&
						   VK(a)->bridge->driver_clock &&
						   device_has_extension(
							   VK(a)->phys,
							   VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME) &&
						   !FNL_FAULT("no-present-timing");
	if (VK(a)->google_timing)
		exts[n_exts++] = VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME;
	/* A device that implements only part of Vulkan says so with this, and
	 * the specification has it enabled wherever it is offered: MoltenVK's.
	 * The name is spelt out because the header declares it only under
	 * VK_ENABLE_BETA_EXTENSIONS. */
	static const char PORTABILITY_SUBSET[] = "VK_KHR_portability_subset";
	if (device_has_extension(VK(a)->phys, PORTABILITY_SUBSET))
		exts[n_exts++] = PORTABILITY_SUBSET;
	const void *features_next = VK(a)->present_wait ? &id_want : NULL;
#ifdef VK_EXT_present_timing
	if (VK(a)->present_timing)
		features_next = &id2_want;
#endif
	VkDeviceCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = features_next,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci,
		.enabledExtensionCount = n_exts,
		.ppEnabledExtensionNames = exts,
		.pEnabledFeatures = &want,
	};
	VK_TRY(vkCreateDevice(VK(a)->phys, &ci, NULL, &VK(a)->device));
	VK(a)->memory_budget = VK(a)->get_memory2 &&
		device_has_extension(VK(a)->phys, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
	VK(a)->wait_for_present = NULL;
	if (VK(a)->present_wait)
		VK(a)->wait_for_present = (PFN_vkWaitForPresentKHR)
			vkGetDeviceProcAddr(VK(a)->device, "vkWaitForPresentKHR");
	VK(a)->present_wait = VK(a)->wait_for_present != NULL;
#ifdef VK_EXT_present_timing
	if (VK(a)->present_timing) {
		VK(a)->timing_queue_size = (PFN_vkSetSwapchainPresentTimingQueueSizeEXT)
			vkGetDeviceProcAddr(VK(a)->device,
								"vkSetSwapchainPresentTimingQueueSizeEXT");
		VK(a)->timing_domains = (PFN_vkGetSwapchainTimeDomainPropertiesEXT)
			vkGetDeviceProcAddr(VK(a)->device,
								"vkGetSwapchainTimeDomainPropertiesEXT");
		VK(a)->timing_past = (PFN_vkGetPastPresentationTimingEXT)
			vkGetDeviceProcAddr(VK(a)->device,
								"vkGetPastPresentationTimingEXT");
		VK(a)->surface_caps2 = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
			vkGetInstanceProcAddr(VK(a)->instance,
								  "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
		VK(a)->calibrated = (PFN_vkGetCalibratedTimestampsKHR)
			vkGetDeviceProcAddr(VK(a)->device, "vkGetCalibratedTimestampsKHR");
		VK(a)->present_timing = VK(a)->timing_queue_size &&
								VK(a)->timing_domains && VK(a)->timing_past &&
								VK(a)->surface_caps2 && VK(a)->calibrated;
		VK(a)->timing_calibrates =
			calibrates_monotonic(VK(a)->instance, VK(a)->phys);
	}
#endif
	VK(a)->google_past = NULL;
	if (VK(a)->google_timing)
		VK(a)->google_past = (PFN_vkGetPastPresentationTimingGOOGLE)
			vkGetDeviceProcAddr(VK(a)->device,
								"vkGetPastPresentationTimingGOOGLE");
	VK(a)->google_timing = VK(a)->google_past != NULL;
	VK(a)->timing_why[0] = '\0';
	if (stamps_asked && !VK(a)->present_timing && !VK(a)->google_timing)
		snprintf(VK(a)->timing_why, sizeof VK(a)->timing_why, "the device "
				 "has neither VK_EXT_present_timing nor "
				 "VK_GOOGLE_display_timing");
	vkGetDeviceQueue(VK(a)->device, VK(a)->queue_family, 0, &VK(a)->queue);
	vkGetPhysicalDeviceMemoryProperties(VK(a)->phys, &VK(a)->mem_props);
	return FNL_OK;
}

/* What the vsync toggle switches to: MAILBOX when available (uncapped,
 * tear-free), else IMMEDIATE (uncapped, tears), else FIFO -- nowhere to go. */
static FnlResult choose_present_modes(App *a)
{
	/* No surface, nothing to ask: nothing is presented, and the backend's
	 * flip is what paces the frames (RendererState::surfaceless). */
	if (VK(a)->surfaceless) {
		VK(a)->mode_tearing = VK_PRESENT_MODE_IMMEDIATE_KHR;
		return FNL_OK;
	}
	/* Checked: this runs on the recovery path as well as at startup, where the
	 * surface may have gone again. */
	uint32_t n = 0;
	VK_TRY(vkGetPhysicalDeviceSurfacePresentModesKHR(VK(a)->phys,
													 VK(a)->vk_surface,
													 &n, NULL));
	VkPresentModeKHR *modes = xmalloc(n * sizeof(*modes));
	if (!modes)
		return out_of_memory("the present mode list");
	/* Owned from here, so no VK_TRY. */
	FnlResult rc = VKR(vkGetPhysicalDeviceSurfacePresentModesKHR(
		VK(a)->phys, VK(a)->vk_surface, &n, modes));
	if (rc < 0) {
		free(modes);
		return rc;
	}

	bool has_mailbox = false, has_immediate = false;
	for (uint32_t i = 0; i < n; i++) {
		if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
			has_mailbox = true;
		else if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
			has_immediate = true;
	}
	free(modes);

	VK(a)->mode_tearing = has_mailbox   ? VK_PRESENT_MODE_MAILBOX_KHR
					: has_immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR
									: VK_PRESENT_MODE_FIFO_KHR;
	return FNL_OK;
}

static const char *present_mode_name(VkPresentModeKHR m)
{
	switch (m) {
	case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "IMMEDIATE";
	case VK_PRESENT_MODE_MAILBOX_KHR:      return "MAILBOX";
	case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO";
	case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO RELAXED";
	default:                               return "UNKNOWN";
	}
}

/* Every _SRGB surface format this renderer takes, in preference order. One
 * table for the choice and for srgb_surface: a format chosen from one list
 * and not recognised by another would quietly take the UNORM colour path. */
static const VkFormat SRGB_FORMATS[] = {
	VK_FORMAT_B8G8R8A8_SRGB,
	VK_FORMAT_R8G8B8A8_SRGB,
	VK_FORMAT_A8B8G8R8_SRGB_PACK32,
};

static bool is_srgb_format(VkFormat f)
{
	for (size_t i = 0; i < sizeof SRGB_FORMATS / sizeof SRGB_FORMATS[0]; i++)
		if (f == SRGB_FORMATS[i])
			return true;
	return false;
}

/* The window's format where there is no surface to offer one: the first of
 * SRGB_FORMATS, blue first as the backend's scanout buffers are, or under
 * FNL_FAULT("unorm-surface") its UNORM twin. Every device takes both as a
 * colour attachment and a copy's source (the spec's required formats).
 * Through DXGI the twin too: the Direct3D 11 textures are UNORM, the one
 * import vk_dxgi_shares() asks about and that was measured (DESIGN *Vulkan
 * through DXGI*). */
static void surfaceless_format(App *a)
{
	VK(a)->surf_format = (VkSurfaceFormatKHR){
		FNL_FAULT("unorm-surface") || VK(a)->dxgi ? VK_FORMAT_B8G8R8A8_UNORM
												  : VK_FORMAT_B8G8R8A8_SRGB,
		VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
	};
}

static FnlResult query_surface_info(App *a)
{
	if (VK(a)->surfaceless) {
		surfaceless_format(a);
		FNL_TRY(choose_present_modes(a));
		vk_set_present_mode(a, VK_PRESENT_MODE_FIFO_KHR);
		a->srgb_surface = is_srgb_format(VK(a)->surf_format.format);
		app_palette_init(a);
		return FNL_OK;
	}
	/* Capabilities are re-queried per build in create_swapchain, since they
	 * change at runtime; the format and the present modes do not. */
	uint32_t fn = 0;
	VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(VK(a)->phys, VK(a)->vk_surface,
												  &fn, NULL));
	if (fn == 0)
		return fnl_fail(FNL_ERR_NO_DEVICE,
						"the surface exposes no Vulkan formats");
	VkSurfaceFormatKHR *formats = xmalloc(fn * sizeof(*formats));
	if (!formats)
		return out_of_memory("the surface format list");
	/* Owned from here, so no VK_TRY. */
	FnlResult fr = VKR(vkGetPhysicalDeviceSurfaceFormatsKHR(VK(a)->phys,
															VK(a)->vk_surface,
															&fn, formats));
	if (fr < 0) {
		free(formats);
		return fr;
	}

	/* The first _SRGB format on offer, else the surface's first. */
	VK(a)->surf_format = formats[0];
	for (size_t w = 0; w < sizeof SRGB_FORMATS / sizeof SRGB_FORMATS[0]; w++) {
		bool done = false;
		for (uint32_t i = 0; i < fn && !done; i++)
			if (formats[i].format == SRGB_FORMATS[w] &&
				formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
				VK(a)->surf_format = formats[i];
				done = true;
			}
		if (done)
			break;
	}
	/* The UNORM colour path, where the surface offers one to take. */
	if (FNL_FAULT("unorm-surface"))
		for (uint32_t i = 0; i < fn; i++)
			if (!is_srgb_format(formats[i].format)) {
				VK(a)->surf_format = formats[i];
				break;
			}
	free(formats);

	FNL_TRY(choose_present_modes(a));
	/* Vsynced until create_swapchain() adopts what the caller asked for. */
	vk_set_present_mode(a, VK_PRESENT_MODE_FIFO_KHR);
	a->srgb_surface = is_srgb_format(VK(a)->surf_format.format);
	/* The palette is converted for this surface's blend space. */
	app_palette_init(a);
	return FNL_OK;
}

static FnlResult create_command_pool(App *a)
{
	VkCommandPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = VK(a)->queue_family,
	};
	VK_TRY(vkCreateCommandPool(VK(a)->device, &pci, NULL, &VK(a)->cmd_pool));
	return FNL_OK;
}

static FnlResult create_sync(App *a)
{
	VkSemaphoreCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkFenceCreateInfo fci = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT,   /* first wait must not block */
	};
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		VK_TRY(vkCreateSemaphore(VK(a)->device, &sci, NULL,
								 &VK(a)->img_available[i]));
		VK_TRY(vkCreateFence(VK(a)->device, &fci, NULL, &VK(a)->in_flight[i]));
	}
	return FNL_OK;
}

/* ---- device lifetime --------------------------------------------------- */

/* The timestamps fnl_frame_gpu_ms() is read from, where the queue keeps
 * time (timestampValidBits) -- every Vulkan driver measured does. Without
 * them the figure is said to be unknown, and nothing else changes. */
static void create_timestamps(App *a)
{
	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(VK(a)->phys, &qn, NULL);
	VkQueueFamilyProperties *q = xmalloc(qn * sizeof *q);
	if (!q)
		return;
	vkGetPhysicalDeviceQueueFamilyProperties(VK(a)->phys, &qn, q);
	const uint32_t bits = VK(a)->queue_family < qn
						? q[VK(a)->queue_family].timestampValidBits : 0;
	free(q);
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(VK(a)->phys, &props);
	if (bits == 0 || !(props.limits.timestampPeriod > 0.0f))
		return;
	const VkQueryPoolCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.queryType = VK_QUERY_TYPE_TIMESTAMP,
		.queryCount = 2 * OVERLAY_MAX_IMAGES,
	};
	if (vkCreateQueryPool(VK(a)->device, &ci, NULL, &VK(a)->ts_pool) !=
			VK_SUCCESS) {
		VK(a)->ts_pool = VK_NULL_HANDLE;
		return;
	}
	VK(a)->ts_ns = (double)props.limits.timestampPeriod;
	VK(a)->ts_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
	if (a->parts) {
		const VkQueryPoolCreateInfo pci = {
			.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			.queryType = VK_QUERY_TYPE_TIMESTAMP,
			.queryCount = 4 * OVERLAY_MAX_IMAGES,
		};
		if (vkCreateQueryPool(VK(a)->device, &pci, NULL,
							  &VK(a)->ts_parts) != VK_SUCCESS)
			VK(a)->ts_parts = VK_NULL_HANDLE;
	}
}

int vk_dxgi_env(void)
{
	const char *v = getenv("FNL_VK_DXGI");
	return v && (v[0] == '0' || v[0] == '1') ? v[0] - '0' : -1;
}

/* Everything below the VkInstance, in dependency order. Startup and loss
 * recovery both go through here, so they cannot build different sets. */
static FnlResult create_device_objects(App *a)
{
	FNL_TRY(pick_device(a));
	FNL_TRY(create_device(a));
	if (VK(a)->bridge->choose_present)
		FNL_TRY(VK(a)->bridge->choose_present(a));
	/* The entry in use is the one the frames now go by: the one beside a
	 * plain entry where they go through DXGI, and back where they do not. */
	int cur = VK(a)->pick_cur;
	if (cur >= 0 && cur < VK(a)->pick_n) {
		const bool via = VK(a)->pick_list[cur].via[0] != '\0';
		if (VK(a)->dxgi && !via && cur + 1 < VK(a)->pick_n &&
			VK(a)->pick_list[cur + 1].via[0])
			cur++;
		else if (!VK(a)->dxgi && via)
			cur--;
	}
	fnl_mutex_lock(&a->lock);
	snprintf(a->gpu_via, sizeof a->gpu_via, "%s",
			 VK(a)->dxgi ? "D3D11" : "");
	fnl_mutex_unlock(&a->lock);
	/* Said only where there was a choice -- the inventory is one line on
	 * most machines, and that line is the answer -- and once the way is
	 * chosen: a device with two entries names the one in use. */
	if (VK(a)->pick_n > 1) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(VK(a)->phys, &props);
		fnl_log(FNL_LOG_INFO, "GPU in use: %s%s", props.deviceName,
				VK(a)->dxgi ? " (via D3D11)" : "");
	}
	if (VK(a)->pick_publish)
		app_gpu_list_publish(a, VK(a)->pick_list, VK(a)->pick_n, cur);
	FNL_TRY(query_surface_info(a));

	FNL_TRY(create_command_pool(a));
	create_timestamps(a);
	FNL_TRY(create_render_pass(a));
	FNL_TRY(create_pipeline_cache(a));
	FNL_TRY(create_text_pipeline(a));
	FNL_TRY(create_overlay_pipelines(a));
	/* After the render pass: the scene's passes reuse its depth format. The
	 * images come and go with the scene resolution (see scene_set_size). */
	FNL_TRY(create_scene_objects(a));

	/* No unwinding on failure, deliberately: the caller pairs this with
	 * destroy_device_objects(), which is safe on any prefix because every
	 * handle is VK_NULL_HANDLE until the call that fills it succeeded. */
	FNL_TRY(create_swapchain(a, VK_NULL_HANDLE));
	FNL_TRY(create_sync(a));
	return FNL_OK;
}

/* The reverse, and safe on a lost device: destroying objects stays legal, and
 * the idle waits report the loss rather than blocking, so their results are
 * ignored. `save_cache` is false when the device is gone -- the read-back
 * fails. */
static void destroy_device_objects(App *a, bool save_cache)
{
	if (VK(a)->device == VK_NULL_HANDLE)
		return;

	/* A device the drain finds lost has no cache worth keeping, whatever
	 * the caller expected of it. */
	if (vkDeviceWaitIdle(VK(a)->device) == VK_ERROR_DEVICE_LOST)
		save_cache = false;
	vkDestroyQueryPool(VK(a)->device, VK(a)->ts_pool, NULL);
	VK(a)->ts_pool = VK_NULL_HANDLE;
	vkDestroyQueryPool(VK(a)->device, VK(a)->ts_parts, NULL);
	VK(a)->ts_parts = VK_NULL_HANDLE;
	memset(VK(a)->ts_unread, 0, sizeof VK(a)->ts_unread);

	shown_stop(a);
	reap_stop(a);
	free_all_retired(a);
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		vkDestroySemaphore(VK(a)->device, VK(a)->img_available[i], NULL);
		vkDestroyFence(VK(a)->device, VK(a)->in_flight[i], NULL);
		VK(a)->img_available[i] = VK_NULL_HANDLE;
		VK(a)->in_flight[i] = VK_NULL_HANDLE;
	}

	destroy_swapchain(a);
	/* What the frames went through, which is the device's: a move to
	 * another GPU rebuilds the device and keeps the surface, and the next
	 * device chooses again (VkBridge::choose_present). */
	if (VK(a)->bridge->own_end)
		VK(a)->bridge->own_end(a);
	destroy_instance_buffers(a, OVERLAY_MAX_IMAGES, VK(a)->readback,
							 &VK(a)->readback_mem, VK(a)->readback_map);
	VK(a)->readback_bytes = 0;
	destroy_instance_buffers(a, 1, VK(a)->snap_buf, &VK(a)->snap_mem,
							 VK(a)->snap_map);
	VK(a)->snap_bytes = 0;
	vkDestroyCommandPool(VK(a)->device, VK(a)->cmd_pool, NULL);
	if (save_cache)
		save_pipeline_cache(a);
	vkDestroyPipelineCache(VK(a)->device, VK(a)->pipeline_cache, NULL);
	destroy_overlay_pipelines(a);
	destroy_text_pipeline(a);
	destroy_scene_objects(a);
	vkDestroyRenderPass(VK(a)->device, VK(a)->render_pass, NULL);
	vkDestroyDevice(VK(a)->device, NULL);

	VK(a)->device         = VK_NULL_HANDLE;
	VK(a)->queue          = VK_NULL_HANDLE;
	VK(a)->cmd_pool       = VK_NULL_HANDLE;
	VK(a)->pipeline_cache = VK_NULL_HANDLE;
	VK(a)->render_pass    = VK_NULL_HANDLE;
	VK(a)->swapchain      = VK_NULL_HANDLE;
	VK(a)->image_count    = 0;

	/* Per-frame bookkeeping that named the objects just destroyed. */
	a->frame   = 0;
	a->resized = false;
	/* The scene image went too, and a recovery onto the same window size
	 * rebuilds nothing else, so the loop must be told nothing is asked for. */
	a->draw_scene_w = a->draw_scene_h = SCENE_NATIVE;
}

/* Rebuild everything below the instance. The VkInstance and the native window
 * survive either loss; the physical device is re-picked, since a loss can mean
 * the GPU itself went.
 *
 * Every way out must leave a loss flag raised unless the device is up: the
 * loop's only question is lost(), asked again the moment this returns. A
 * device loss keeps device_lost to the last line; a surface loss hands over to
 * it below. DESIGN *Device and surface loss*. */
static void recover_from_loss(App *a)
{
	/* Spent here: a retry after this pass failed is a loss like any other,
	 * said as one and paced as one. */
	const bool remake = a->surface_remake;
	a->surface_remake = false;
	/* A move to another GPU was said by the loop (gpu_switch_owed()). */
	if (remake)
		fnl_log(FNL_LOG_INFO, "the surface is made again for the output's "
				"new mode");
	else if (!a->gpu_switching)
		fnl_log(FNL_LOG_ERROR, "%s lost -- rebuilding",
				a->surface_lost ? "surface" : "device");

	/* The gap's picture under the swapchain before it goes
	 * (FnlBackendVtbl::present_window_hold). */
	BACKEND(a)->present_window_hold(a);
	/* A move or a surface made again leaves the device alive, so the
	 * pipelines it compiled are kept in its own cache file; a move to
	 * another kernel driver is a renderer swap (renderer_swap()) and never
	 * comes here. No read-back from a lost one. */
	destroy_device_objects(a, (remake || a->gpu_switching) &&
							  !a->device_lost);

	/* After the swapchain that was using it, never before. */
	if (a->surface_lost) {
		vkDestroySurfaceKHR(VK(a)->instance, VK(a)->vk_surface, NULL);
		VK(a)->vk_surface = VK_NULL_HANDLE;
		/* A move to another GPU presents into a window of its own, which a
		 * driver before has not claimed (FnlBackendVtbl::
		 * present_window_renew). */
		if (a->gpu_switching)
			BACKEND(a)->present_window_renew(a);
		if (FNL_FAULT("create-surface") || create_surface(a) < 0)
			return;        /* still gone; the loop retries and rechecks quit */
		/* The surface is back and the device is not, so the second flag takes
		 * over for the rest of the rebuild. */
		a->surface_lost = false;
		a->device_lost  = true;
	}

	/* A driver mid-reset can refuse to open the device for a moment. A
	 * surface made again on request, or a move to another GPU, has no reset
	 * behind it. */
	if (!remake && !a->gpu_switching)
		fnl_sleep_seconds(0.2);

	/* Neither pick_device() nor query_surface_info() can report a lost
	 * surface -- they would read it as "no device" and "no formats" -- so ask
	 * something that can first. */
	/* Not on a move, which lost nothing, and whose surface may be another
	 * device's display that the device in VK(a)->phys cannot be asked
	 * about (KMS); nor where there is no surface to have lost. */
	VkSurfaceCapabilitiesKHR caps;
	if (!a->gpu_switching && VK(a)->vk_surface != VK_NULL_HANDLE &&
		vk_lost(a, vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
					   VK(a)->phys, VK(a)->vk_surface, &caps)))
		return;            /* still gone; the loop retries and rechecks quit */

	if (FNL_FAULT("recover-objects") || create_device_objects(a) < 0)
		return;            /* a flag is still raised; the loop comes back */

	/* A fresh img_cmd_gen is all zeroes and record_gen is never zero, so
	 * every command buffer of the new swapchain records afresh. */
	a->record_gen++;
	a->device_lost = false;

	fnl_log(FNL_LOG_INFO, "rebuilt on %s", a->gpu_name);
}

/* ---- the renderer seam ----------------------------------------------------
 *
 * This file's half of renderer.h. The adapters exist where the internal
 * signature and the seam's differ. */

/* The seam's last entry, so the renderer's state dies here. */
static void vk_destroy_instance(App *a)
{
	if (!a->renderer_state)
		return;
	/* A display the bridge acquired is handed back, which is also where the
	 * driver lets go of the descriptor it was lent (VK_EXT_direct_mode_
	 * display); the entry is looked up because only that bridge enables
	 * the extension. */
	if (VK(a)->instance && VK(a)->vk_display != VK_NULL_HANDLE) {
		PFN_vkReleaseDisplayEXT release = (PFN_vkReleaseDisplayEXT)
			vkGetInstanceProcAddr(VK(a)->instance, "vkReleaseDisplayEXT");
		if (release)
			release(VK(a)->surface_phys, VK(a)->vk_display);
	}
	if (VK(a)->instance)
		vkDestroyInstance(VK(a)->instance, NULL);
	free(a->renderer_state);
	a->renderer_state = NULL;
	/* Written by this renderer alone (create_device_objects()), and ""
	 * for every other: a move to Direct3D 12 would say "D3D11" on. */
	fnl_mutex_lock(&a->lock);
	a->gpu_via[0] = '\0';
	fnl_mutex_unlock(&a->lock);
}

static void vk_destroy_surface(App *a)
{
	if (VK(a)->dxgi && VK(a)->bridge->own_end)
		VK(a)->bridge->own_end(a);
	if (VK(a)->instance && VK(a)->vk_surface) {
		vkDestroySurfaceKHR(VK(a)->instance, VK(a)->vk_surface, NULL);
		VK(a)->vk_surface = VK_NULL_HANDLE;
	}
}

static void vk_wait_idle(App *a)
{
	if (VK(a)->device)
		vkDeviceWaitIdle(VK(a)->device);
}

static bool vk_is_lost(const App *a)
{
	return a->device_lost || a->surface_lost;
}

/* A display surface names its mode (VkDisplaySurfaceCreateInfoKHR), so
 * another mode is another surface, through the loss path: the format the
 * render pass and pipelines are built on is the surface's, which is why a
 * lost surface takes the device with it. A headless surface -- the copy
 * path -- takes any extent, and the swapchain follows the size published
 * with the mode as it follows any other. */
static void vk_remake_surface(App *a)
{
	if (VK(a)->present_copy || a->surface_lost)
		return;
	a->surface_lost = true;
	a->surface_remake = true;
}

static const char *vk_device_name(const App *app)
{
	/* A per-thread snapshot under App::lock, as fnl_output_name() takes:
	 * pick_device() rewrites the buffer from the render thread on recovery.
	 * The const is cast away for the lock, as api_state.c does. */
	static FNL_THREAD_LOCAL char snap[GPU_NAME_MAX];
	App *a = (App *)app;

	fnl_mutex_lock(&a->lock);
	memcpy(snap, a->gpu_name, sizeof snap);
	fnl_mutex_unlock(&a->lock);
	snap[sizeof snap - 1] = '\0';
	return snap[0] ? snap : "(no device)";
}

static const char *vk_current_present_mode(const App *a)
{
	/* The answer is a literal in .rodata, so only the enum crosses threads --
	 * which is why that field is atomic. */
	return present_mode_name(vk_present_mode(a));
}

/* VK_EXT_memory_budget's heap usage, this process's, over the device-local
 * heaps -- on an integrated GPU the system's memory, which is what it draws
 * from. False where the device has no budget to read. */
static bool vk_gpu_memory(App *a, double *mib)
{
	if (VK(a)->phys == VK_NULL_HANDLE || !VK(a)->memory_budget)
		return false;
	VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
	};
	VkPhysicalDeviceMemoryProperties2KHR props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2_KHR,
		.pNext = &budget,
	};
	VK(a)->get_memory2(VK(a)->phys, &props);
	VkDeviceSize used = 0;
	for (uint32_t i = 0; i < props.memoryProperties.memoryHeapCount; i++)
		if (props.memoryProperties.memoryHeaps[i].flags &
			VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
			used += budget.heapUsage[i];
	*mib = (double)used / (1024.0 * 1024.0);
	return true;
}

/* Vulkan's WSI reports no occlusion -- VK_SUBOPTIMAL_KHR is about the
 * surface's properties, not about what sits on the window -- so the window
 * server is asked, where it says (VkBridge::occluded: a Mac's), and the
 * answer is otherwise no. Wayland's frame invitation covers it anyway; on
 * Win32 a covered window still renders, and only a minimized one is
 * parked, by the backend half. */
static bool vk_occluded(App *a)
{
	const VkBridge *b = a->renderer_state ? VK(a)->bridge : NULL;
	return b && b->occluded && b->occluded(a);
}

const FnlRendererVtbl fnl_renderer_vulkan = {
	.name                  = "Vulkan",
	.api                   = FNL_RENDERER_VULKAN,
	.key                   = "vulkan",
	.draws_overlay         = true,
	/* The passes and the fetch are built unconditionally. What a device may
	 * still refuse is the memory for a target, which is scene_set_size()'s
	 * answer and not this one. */
	.offscreen_scene       = true,
	.create_instance       = create_instance,
	.destroy_instance      = vk_destroy_instance,
	.create_surface        = create_surface,
	.destroy_surface       = vk_destroy_surface,
	.list_gpus             = vk_list_gpus,
	/* Any thread may use a VkDevice; the queue is the caller's to keep to
	 * one at a time, which the frame loop does. */
	.thread_enter          = renderer_noop,
	.thread_leave          = renderer_noop,
	.create_device_objects = create_device_objects,
	.destroy_device_objects = destroy_device_objects,
	.recreate_swapchain    = recreate_swapchain,
	.present_mode_matches  = vk_present_mode_matches,
	.retire_tick           = vk_retire_tick,
	.scale_changed         = text_set_scale,
	.set_scene_size        = scene_set_size,
	.draw_frame            = draw_frame,
	.wait_idle             = vk_wait_idle,
	.lost                  = vk_is_lost,
	.recover               = recover_from_loss,
	.remake_surface        = vk_remake_surface,
	.occluded              = vk_occluded,
	.max_samples           = vk_max_samples,
	.active_samples        = vk_active_samples,
	.gpu_time_poll         = vk_gpu_time_poll,
	.device_name           = vk_device_name,
	.gpu_memory            = vk_gpu_memory,
	.present_mode_name     = vk_current_present_mode,
	.overlay_flush_rects   = vk_overlay_flush_rects,
	.overlay_flush_shapes  = vk_overlay_flush_shapes,
	.overlay_scissor       = vk_overlay_scissor,
	.text_end              = vk_text_end,
	.overlay_cover         = vk_overlay_cover,
	.cover_release         = vk_cover_release,
};

/* ---- the public Vulkan interop --------------------------------------------
 *
 * include/fenestal/fnl_vulkan.h: what device the caller is on, what a
 * pipeline must be compatible with, and where to record.
 *
 * Each refuses an App that another renderer draws: a build may have several,
 * and every one's interop is exported. Each reads the renderer's state,
 * which a loss or a move frees on the render thread, so each is that
 * thread's alone, says so in the header, and refuses any other thread
 * (app_renderer_state_ours()). */

bool fnl_vulkan_gpu(App *a, FnlVulkanGpu *out)
{
	if (!a || !out || !app_renderer_state_ours(a) ||
		renderer_any(a) != &fnl_renderer_vulkan ||
		!a->renderer_state || !VK(a)->device)
		return false;
	*out = (FnlVulkanGpu){
		.instance        = VK(a)->instance,
		.physical_device = VK(a)->phys,
		.device          = VK(a)->device,
		.queue           = VK(a)->queue,
		.queue_family    = VK(a)->queue_family,
		.pipeline_cache  = VK(a)->pipeline_cache,
		.get_instance_proc_addr = fnl_vkGetInstanceProcAddr,
	};
	return true;
}

bool fnl_vulkan_scene_layout(App *a, FnlSceneTarget t,
							 FnlVulkanSceneLayout *out)
{
	if (!a || !out || !app_renderer_state_ours(a) ||
		renderer_any(a) != &fnl_renderer_vulkan ||
		!a->renderer_state || t < 0 || t >= FNL_TARGET_COUNT)
		return false;

	VkRenderPass          pass;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

	switch (t) {
	case FNL_TARGET_DIRECT:    pass = VK(a)->render_pass;   break;
	case FNL_TARGET_OFFSCREEN: pass = VK(a)->scene_pass;    break;
	case FNL_TARGET_OFFSCREEN_MS:
		pass = VK(a)->scene_pass_ms;
		samples = scene_samples(a);
		break;
	default: return false;
	}
	/* Null means this window cannot use that target: the multisampled pass
	 * is not built on a device offering no sample count, and the frame never
	 * selects it. */
	if (pass == VK_NULL_HANDLE)
		return false;

	*out = (FnlVulkanSceneLayout){
		.render_pass  = pass,
		.subpass      = 0,
		.samples      = samples,
		.color_format = VK(a)->surf_format.format,
		.depth_format = VK(a)->depth_format,
	};
	return true;
}

bool fnl_vulkan_scene_cmd(App *a, VkCommandBuffer *out)
{
	/* Null outside on_record: a handle used a frame later would record into a
	 * buffer that may be executing, so a stale one is refused rather than
	 * handed back. The thread first: from another, the state read may be
	 * one a move to another renderer is freeing. */
	if (!a || !out || app_thread() != APP_THREAD_RENDER ||
		renderer_any(a) != &fnl_renderer_vulkan ||
		!a->renderer_state || VK(a)->scene_cb == VK_NULL_HANDLE)
		return false;
	*out = VK(a)->scene_cb;
	return true;
}

bool fnl_vulkan_scene_rect(App *a, VkRect2D *out)
{
	/* Refused where fnl_vulkan_scene_cmd() is, for the same reasons: the
	 * rectangle is this recording's, and the state it is read from is the
	 * render thread's. */
	if (!a || !out || app_thread() != APP_THREAD_RENDER ||
		renderer_any(a) != &fnl_renderer_vulkan ||
		!a->renderer_state || VK(a)->scene_cb == VK_NULL_HANDLE)
		return false;
	*out = VK(a)->scene_rect;
	return true;
}

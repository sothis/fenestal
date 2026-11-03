/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The KMS-to-Vulkan bridge: a display plane surface on the output the
 * backend holds. The sibling of surface_wayland.c and the rest, with more to
 * do, because there is no window: the display is found by the connector the
 * backend chose, acquired with the DRM node it holds as master, and given a
 * mode and a plane before there is a surface to make.
 *
 * VK_EXT_acquire_drm_display is the way in, and not the driver's own list of
 * displays: a driver opens the node for itself too, and Mesa's WSI gives up
 * its own descriptor when it is not master -- which it is not, since the
 * backend opened first. So the display goes to the descriptor that is, and
 * the driver needs no privilege of its own.
 *
 * Where no device will take the display -- lavapipe, which has no display
 * at all -- the frames go a second way: a headless surface, every image
 * read back, and the pixels flipped onto the output by the backend
 * (RendererState::present_copy, DESIGN *KMS: no window server*).
 */

#include "core/app.h"

#include "renderer/vulkan/vk_internal.h"
#include "backend/kms/kms_state.h"

#include <fcntl.h>
#include <unistd.h>

/* Two ways onto the output, and none required, since either will do: the
 * display itself, through the three below -- direct mode is what acquiring
 * rests on, and what releases -- or a headless surface whose frames are
 * copied on (RendererState::present_copy). The bits are
 * RendererState::backend_ext_on's. The last is what VK_EXT_present_timing
 * wants of the instance, for the display path's stamps (driver_stamps()).
 */
#define EXT_DISPLAY_ALL 0x7u
#define EXT_HEADLESS    0x8u
#define EXT_CAPS2       0x10u

static const char *const *instance_extensions(uint32_t *count,
											  uint32_t *required)
{
	static const char *const EXTS[] = {
		VK_KHR_DISPLAY_EXTENSION_NAME,
		VK_EXT_DIRECT_MODE_DISPLAY_EXTENSION_NAME,
		VK_EXT_ACQUIRE_DRM_DISPLAY_EXTENSION_NAME,
		VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME,
		VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
	};
	*count = (uint32_t)(sizeof EXTS / sizeof EXTS[0]);
	*required = 0;
	return EXTS;
}

/* Hardware before a CPU rasteriser: a software device takes any descriptor
 * it is lent, so on a machine with both it would win by coming first. */
static bool device_is_cpu(VkPhysicalDevice d)
{
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(d, &props);
	return props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
}

/* Whether `dev` can draw this window: any device with a graphics queue
 * can. One that takes the output's display draws to it; one that will not
 * draws by the copy path, on a headless surface where it presents to one
 * and otherwise into images of the renderer's own with no surface at all
 * (RendererState::surfaceless) -- NVIDIA's own driver beside an Intel
 * panel, which names no display here and has no headless surface. The
 * list pick_device() shows and the index wanted_device() reads are both of
 * these, in the enumeration's order. */
static bool can_draw(App *a, VkPhysicalDevice dev)
{
	(void)a;
	return vk_find_present_queue(dev, VK_NULL_HANDLE) >= 0;
}

/* The device the caller asked for, where one was: the first carrying the
 * name, as pick_device() reads a name -- two cards of one model are told
 * apart by the index -- or the one in the index's place in the list
 * (can_draw()). VK_NULL_HANDLE for the automatic choice, or for a name or
 * an index that finds nothing. `*by_index` says which was asked. */
static VkPhysicalDevice wanted_device(App *a, const VkPhysicalDevice *devs,
									  uint32_t n, bool *by_index)
{
	*by_index = !a->gpu_want[0] && a->gpu_index > 0;
	for (uint32_t i = 0, k = 0; i < n; i++) {
		if (a->gpu_want[0]) {
			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(devs[i], &props);
			if (app_name_has(props.deviceName, a->gpu_want))
				return devs[i];
		} else if (a->gpu_index > 0 && can_draw(a, devs[i]) &&
				   (int)++k == a->gpu_index) {
			return devs[i];
		}
	}
	return VK_NULL_HANDLE;
}

/* The display behind the backend's connector, on the first device that
 * will take it, acquired with a duplicate of the backend's descriptor: one
 * open file, so one master, and the driver may close its copy when it
 * releases the display without closing the backend's.
 *
 * A device chosen by name is the only one asked, where one carries the
 * name: a surface on a device's display is that device's alone, so the
 * display taken by another would leave the name nothing to choose. When
 * the named one will not take it -- NVK on a GPU with no panel -- the frames
 * go by the copy path, whose headless surface every device presents to,
 * and pick_device() finds it there. An index is read as the list counts:
 * the devices that can draw this window, the one holding the display and
 * the rest, which reach the output by the copy path (can_draw()), in the
 * enumeration's order (wanted_device()). An index past the end chooses
 * nothing, and the display goes as it would with none. An index beside a
 * name is the name's, as pick_device() reads them.
 *
 * False with the reason in `why`, and nothing recorded: a refusal is the
 * copy path's cue where there is one, and create_surface() decides whether
 * it is a failure. */
static bool acquire_display(App *a, char *why, size_t size)
{
	struct BackendState *p = a->backend_state;
	const int sel = kms_sel_to_show(p);
	const char *output = kms_name(p, sel);
	if (FNL_FAULT("kms-no-display")) {
		snprintf(why, size, "FNL_FAULT: no device takes %s", output);
		return false;
	}
	if ((VK(a)->backend_ext_on & EXT_DISPLAY_ALL) != EXT_DISPLAY_ALL) {
		snprintf(why, size, "the Vulkan loader has no "
				 "VK_EXT_acquire_drm_display");
		return false;
	}
	PFN_vkGetDrmDisplayEXT get_display = (PFN_vkGetDrmDisplayEXT)
		vkGetInstanceProcAddr(VK(a)->instance, "vkGetDrmDisplayEXT");
	PFN_vkAcquireDrmDisplayEXT acquire = (PFN_vkAcquireDrmDisplayEXT)
		vkGetInstanceProcAddr(VK(a)->instance, "vkAcquireDrmDisplayEXT");
	uint32_t n = 0;
	VkPhysicalDevice *devs = NULL;
	if (get_display && acquire &&
		vkEnumeratePhysicalDevices(VK(a)->instance, &n, NULL) == VK_SUCCESS &&
		n)
		devs = calloc(n, sizeof *devs);
	if (!devs ||
		vkEnumeratePhysicalDevices(VK(a)->instance, &n, devs) < VK_SUCCESS) {
		free(devs);
		snprintf(why, size, "no Vulkan device to ask");
		return false;
	}
	/* The last refusal, and whose: a device that will not name the
	 * display is told apart from one that names it and will not let go of
	 * it, which is the difference between no support and a busy output. */
	VkResult last = VK_SUCCESS;
	const char *refused = "vkGetDrmDisplayEXT";
	char by[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = "no device";
	bool by_index;
	const VkPhysicalDevice only = wanted_device(a, devs, n, &by_index);
	const VkPhysicalDevice named = by_index ? VK_NULL_HANDLE : only;
	const VkPhysicalDevice indexed = by_index ? only : VK_NULL_HANDLE;
	for (int pass = 0; pass < 2; pass++)
		for (uint32_t i = 0; i < n; i++) {
			if (device_is_cpu(devs[i]) != (pass == 1))
				continue;
			if (only != VK_NULL_HANDLE && devs[i] != only)
				continue;
			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(devs[i], &props);
			snprintf(by, sizeof by, "%s", props.deviceName);
			VkDisplayKHR d = VK_NULL_HANDLE;
			last = get_display(devs[i], p->card_fd, kms_connector(p, sel),
							   &d);
			if (last != VK_SUCCESS || d == VK_NULL_HANDLE) {
				refused = "vkGetDrmDisplayEXT";
				continue;
			}
			const int lent = fcntl(p->card_fd, F_DUPFD_CLOEXEC, 3);
			if (lent < 0) {
				refused = "dup of the DRM descriptor";
				last = VK_ERROR_INITIALIZATION_FAILED;
				continue;
			}
			last = acquire(devs[i], lent, d);
			if (last != VK_SUCCESS) {
				close(lent);
				refused = "vkAcquireDrmDisplayEXT";
				continue;
			}
			VK(a)->surface_phys = devs[i];
			VK(a)->vk_display = d;
			p->disp_out = KMS_SEL_OUT(sel);
			free(devs);
			return true;
		}
	free(devs);
	if (named)
		snprintf(why, size, "%s, the GPU named \"%s\", will not take %s: %s "
				 "returned %s (%d)", by, a->gpu_want, output, refused,
				 vk_result_str(last), (int)last);
	else if (indexed != VK_NULL_HANDLE)
		snprintf(why, size, "%s, GPU %d by index, will not take %s: %s "
				 "returned %s (%d)", by, a->gpu_index, output, refused,
				 vk_result_str(last), (int)last);
	else
		snprintf(why, size, "no Vulkan device would take %s: %s on %s "
				 "returned %s (%d)", output, refused, by,
				 vk_result_str(last), (int)last);
	return false;
}

/* Move the display the device holds to output `out`'s, on the same device:
 * the old one released -- which is also where the driver lets go of the
 * descriptor it was lent -- and the new one found by its connector and
 * acquired with a fresh duplicate. On a refusal the old one is taken back,
 * so the device still has a display to make a surface on, and false. */
static bool take_display(App *a, struct BackendState *p, int out)
{
	PFN_vkGetDrmDisplayEXT get_display = (PFN_vkGetDrmDisplayEXT)
		vkGetInstanceProcAddr(VK(a)->instance, "vkGetDrmDisplayEXT");
	PFN_vkAcquireDrmDisplayEXT acquire = (PFN_vkAcquireDrmDisplayEXT)
		vkGetInstanceProcAddr(VK(a)->instance, "vkAcquireDrmDisplayEXT");
	PFN_vkReleaseDisplayEXT release = (PFN_vkReleaseDisplayEXT)
		vkGetInstanceProcAddr(VK(a)->instance, "vkReleaseDisplayEXT");
	if (!get_display || !acquire || !release)
		return false;
	const int was = p->disp_out;
	for (int pass = 0; pass < 2; pass++) {
		const int to = pass == 0 ? out : was;
		if (to < 0)
			break;
		if (VK(a)->vk_display != VK_NULL_HANDLE) {
			release(VK(a)->surface_phys, VK(a)->vk_display);
			VK(a)->vk_display = VK_NULL_HANDLE;
			p->disp_out = -1;
		}
		VkDisplayKHR d = VK_NULL_HANDLE;
		VkResult r = get_display(VK(a)->surface_phys, p->card_fd,
								 p->outputs[to].connector_id, &d);
		const int lent = r == VK_SUCCESS && d != VK_NULL_HANDLE
							 ? fcntl(p->card_fd, F_DUPFD_CLOEXEC, 3) : -1;
		if (lent >= 0) {
			r = acquire(VK(a)->surface_phys, lent, d);
			if (r == VK_SUCCESS) {
				VK(a)->vk_display = d;
				p->disp_out = to;
				return to == out;
			}
			close(lent);
		}
		if (pass == 0)
			fnl_log(FNL_LOG_WARN, "KMS: %s will not be taken by %s: %s "
					"(%d)", p->outputs[out].name, a->gpu_name,
					vk_result_str(r), (int)r);
	}
	return false;
}

/* The copy path's surface: a headless one, which every device presents to
 * and nothing shows. The frames are read back and handed to the backend
 * (RendererState::present_copy), so the swapchain, its rebuilds and its
 * losses are the same code as on every other backend. */
static FnlResult create_headless(App *a)
{
	PFN_vkCreateHeadlessSurfaceEXT make = (PFN_vkCreateHeadlessSurfaceEXT)
		vkGetInstanceProcAddr(VK(a)->instance, "vkCreateHeadlessSurfaceEXT");
	if (!make)
		return fnl_fail(FNL_ERR_NO_DEVICE, "the Vulkan loader has no "
						"VK_EXT_headless_surface");
	const VkHeadlessSurfaceCreateInfoEXT ci = {
		.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT,
	};
	VkSurfaceKHR s = VK_NULL_HANDLE;
	VK_TRY(make(VK(a)->instance, &ci, NULL, &s));
	VK(a)->vk_surface = s;
	return FNL_OK;
}

/* The copy path's way: a headless surface where the device that will draw
 * presents to one -- the device asked for, or with none asked any device,
 * which pick_device() then chooses among -- and otherwise no surface at all
 * (RendererState::surfaceless), whose images are the renderer's own. The
 * headless surface first because it is the swapchain every other backend
 * has, the one the rest of this renderer was measured on. */
static FnlResult copy_surface(App *a)
{
	VK(a)->surfaceless = false;
	if ((VK(a)->backend_ext_on & EXT_HEADLESS) &&
		!FNL_FAULT("kms-no-headless")) {
		FNL_TRY(create_headless(a));
		uint32_t n = 0;
		VkPhysicalDevice devs[16];
		if (vkEnumeratePhysicalDevices(VK(a)->instance, &n, NULL) !=
				VK_SUCCESS)
			n = 0;
		if (n > 16)
			n = 16;
		if (n && vkEnumeratePhysicalDevices(VK(a)->instance, &n, devs) <
					 VK_SUCCESS)
			n = 0;
		bool by_index;
		const VkPhysicalDevice want = wanted_device(a, devs, n, &by_index);
		for (uint32_t i = 0; i < n; i++)
			if ((want == VK_NULL_HANDLE || devs[i] == want) &&
				vk_find_present_queue(devs[i], VK(a)->vk_surface) >= 0)
				return FNL_OK;
		vkDestroySurfaceKHR(VK(a)->instance, VK(a)->vk_surface, NULL);
		VK(a)->vk_surface = VK_NULL_HANDLE;
	}
	fnl_log(FNL_LOG_INFO, "KMS: no headless surface for the GPU that will "
			"draw; it draws into images of its own");
	VK(a)->surfaceless = true;
	return FNL_OK;
}

/* VK_KHR_display's entries, asked of the instance where they are used and
 * not linked through vk_fns.h: the loader hands them only to an instance
 * that enabled the extension, which this bridge asks for and can go without.
 * A display is held only where it is enabled, so none missing is expected;
 * one missing is still a refusal rather than a call through NULL. */
typedef struct {
	PFN_vkGetDisplayModePropertiesKHR                modes;
	PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR planes;
	PFN_vkGetDisplayPlaneSupportedDisplaysKHR        plane_displays;
	PFN_vkGetDisplayPlaneCapabilitiesKHR             plane_caps;
	PFN_vkCreateDisplayPlaneSurfaceKHR               make;
} DisplayFns;

static FnlResult display_fns(App *a, DisplayFns *d)
{
#define ASK(f) ((PFN_##f)vkGetInstanceProcAddr(VK(a)->instance, #f))
	d->modes          = ASK(vkGetDisplayModePropertiesKHR);
	d->planes         = ASK(vkGetPhysicalDeviceDisplayPlanePropertiesKHR);
	d->plane_displays = ASK(vkGetDisplayPlaneSupportedDisplaysKHR);
	d->plane_caps     = ASK(vkGetDisplayPlaneCapabilitiesKHR);
	d->make           = ASK(vkCreateDisplayPlaneSurfaceKHR);
#undef ASK
	if (!d->modes || !d->planes || !d->plane_displays || !d->plane_caps ||
		!d->make)
		return fnl_fail(FNL_ERR_NO_DEVICE, "the Vulkan loader has no "
						"VK_KHR_display");
	return FNL_OK;
}

/* The mode a selection names, as Vulkan lists it on the display held: the
 * same size, and of those the refresh nearest. The backend publishes that
 * size, and a surface of another would be a window that is not the size it
 * says. */
static FnlResult pick_mode(App *a, const DisplayFns *d, int which,
						   VkDisplayModeKHR *out)
{
	const struct BackendState *p = a->backend_state;
	int want_w, want_h;
	unsigned int want_mhz;
	kms_mode_size(p, which, &want_w, &want_h, &want_mhz);
	uint32_t n = 0;
	VK_TRY(d->modes(VK(a)->surface_phys, VK(a)->vk_display, &n, NULL));
	VkDisplayModePropertiesKHR *modes = n ? calloc(n, sizeof *modes) : NULL;
	if (!modes)
		return fnl_fail(FNL_ERR_NO_DISPLAY, "%s lists no modes to Vulkan",
						kms_name(p, which));
	FnlResult rc = VKR(d->modes(VK(a)->surface_phys, VK(a)->vk_display, &n,
								modes));
	int best = -1;
	unsigned int best_off = 0;
	for (uint32_t i = 0; rc == FNL_OK && i < n; i++) {
		const VkDisplayModeParametersKHR *m = &modes[i].parameters;
		if ((int)m->visibleRegion.width != want_w ||
			(int)m->visibleRegion.height != want_h)
			continue;
		const unsigned int off = m->refreshRate > want_mhz
									 ? m->refreshRate - want_mhz
									 : want_mhz - m->refreshRate;
		if (best < 0 || off < best_off) {
			best = (int)i;
			best_off = off;
		}
	}
	if (rc == FNL_OK && best < 0)
		rc = fnl_fail(FNL_ERR_NO_DISPLAY, "Vulkan lists no %dx%d mode for %s",
					  want_w, want_h, kms_name(p, which));
	if (rc == FNL_OK)
		*out = modes[best].displayMode;
	free(modes);
	return rc;
}

/* A plane that can show this display and is not showing another. */
static FnlResult pick_plane(App *a, const DisplayFns *d, uint32_t *plane,
							uint32_t *stack)
{
	uint32_t n = 0;
	VK_TRY(d->planes(VK(a)->surface_phys, &n, NULL));
	VkDisplayPlanePropertiesKHR *props = n ? calloc(n, sizeof *props) : NULL;
	if (!props)
		return fnl_fail(FNL_ERR_NO_DISPLAY, "the device lists no planes");
	FnlResult rc = VKR(d->planes(VK(a)->surface_phys, &n, props));
	bool found = false;
	for (uint32_t i = 0; rc == FNL_OK && !found && i < n; i++) {
		if (props[i].currentDisplay != VK_NULL_HANDLE &&
			props[i].currentDisplay != VK(a)->vk_display)
			continue;
		uint32_t dn = 0;
		if (d->plane_displays(VK(a)->surface_phys, i, &dn, NULL) !=
				VK_SUCCESS || !dn)
			continue;
		VkDisplayKHR *ds = calloc(dn, sizeof *ds);
		if (!ds)
			break;
		if (d->plane_displays(VK(a)->surface_phys, i, &dn, ds) ==
				VK_SUCCESS)
			for (uint32_t j = 0; j < dn; j++)
				if (ds[j] == VK(a)->vk_display) {
					*plane = i;
					*stack = props[i].currentStackIndex;
					found = true;
					break;
				}
		free(ds);
	}
	free(props);
	if (rc == FNL_OK && !found)
		rc = fnl_fail(
			FNL_ERR_NO_DISPLAY, "no display plane can show %s",
			a->backend_state->outputs[a->backend_state->disp_out].name);
	return rc;
}

/* A move to another GPU (fnl_request_gpu()) decides the way onto the output
 * afresh: the display the old device holds is let go -- which is also where
 * its driver lets go of the descriptor it was lent -- so that the new one
 * can take it, and where it will not, the frames go by the copy path. The
 * backend has been told already (FnlBackendVtbl::present_window_renew), so
 * a copy path after a display sets its mode again. */
static void let_display_go(App *a)
{
	struct BackendState *p = a->backend_state;
	if (VK(a)->vk_display != VK_NULL_HANDLE) {
		PFN_vkReleaseDisplayEXT release = (PFN_vkReleaseDisplayEXT)
			vkGetInstanceProcAddr(VK(a)->instance, "vkReleaseDisplayEXT");
		if (release)
			release(VK(a)->surface_phys, VK(a)->vk_display);
		VK(a)->vk_display = VK_NULL_HANDLE;
		p->disp_out = -1;
	}
	VK(a)->surface_phys = VK_NULL_HANDLE;
	VK(a)->present_copy = false;
	VK(a)->surfaceless = false;
}

/* Built into a local and assigned only on success: recover_from_loss()
 * comes back through here, and the display it acquired the first time is
 * still acquired. */
static FnlResult create_surface(App *a)
{
	if (a->gpu_switching)
		let_display_go(a);
	/* Across a move to another kernel driver the renderer drawn meanwhile
	 * takes the copy path whatever it could take: the backend's own pair
	 * then stays on the output from the last frame before the move to the
	 * first on the GPU moved, which lands by the copy path too, so no
	 * display changes hands for the landing, no output is taken back with
	 * a black frame and no mode is set in between -- the frames freeze
	 * for the landing's build and nothing goes dark. A second or two of
	 * the copy's cost, on the GPU with the panel. DESIGN *The veil*. */
	if (a->gpu_cross && !VK(a)->present_copy) {
		fnl_log(FNL_LOG_INFO, "KMS: drawn across the move by the copy path "
				"onto %s, so the output is not handed over for the landing",
				kms_name(a->backend_state, kms_sel_to_show(a->backend_state)));
		VK(a)->present_copy = true;
	}
	/* Decided at the first surface and at a move: a rebuild after a loss
	 * keeps whichever way the frames reached the output. */
	if (!VK(a)->present_copy && VK(a)->vk_display == VK_NULL_HANDLE) {
		char why[256];
		if (!acquire_display(a, why, sizeof why)) {
			fnl_log(FNL_LOG_INFO, "KMS: %s; drawing offscreen and copying "
					"each frame onto %s", why,
					kms_name(a->backend_state,
							 kms_sel_to_show(a->backend_state)));
			VK(a)->present_copy = true;
		}
	}
	if (VK(a)->present_copy)
		return copy_surface(a);
	DisplayFns d;
	FNL_TRY(display_fns(a, &d));

	/* Which selection. The surface made last named one, and presents since
	 * mean it showed. None, twice running, means it would not -- the
	 * swapchain on it lost its surface before a frame was out, which is how
	 * a display that refuses the mode at the first flip answers -- and the
	 * last that did is made instead, as it is where Vulkan has no such mode
	 * at all, and where the device will not take that output's display.
	 * Twice, because once is what schenker's HDMI output does at the first
	 * present after it is lit, at a mode it then shows. */
	struct BackendState *p = a->backend_state;
	if (p->surf_sel >= 0 && a->presents != p->surf_presents)
		kms_sel_shown(a, p->surf_sel);
	int show = kms_sel_to_show(p);
	const bool again = show == p->surf_sel &&
					   a->presents == p->surf_presents;
	p->surf_unshown = again ? p->surf_unshown + 1 : 0;
	const bool unshown = p->surf_unshown >= 2;
	if (show != (int)fnl_atomic_load_u32(&p->sel_good) &&
		(unshown || FNL_FAULT("kms-mode-refused"))) {
		kms_sel_refused(a, show, false);
		show = kms_sel_to_show(p);
	}

	/* Another output: its display in place of the one held, and the one
	 * left goes dark. Where the device will not take it, the output is the
	 * one that would not show. */
	const int left = p->disp_out;
	if (KMS_SEL_OUT(show) != p->disp_out &&
		!take_display(a, p, KMS_SEL_OUT(show))) {
		kms_sel_refused(a, show, true);
		show = kms_sel_to_show(p);
	}

	VkDisplayModeKHR mode = VK_NULL_HANDLE;
	uint32_t plane = 0, stack = 0;
	FnlResult picked = KMS_SEL_OUT(show) == p->disp_out
		? pick_mode(a, &d, show, &mode)
		: fnl_fail(FNL_ERR_NO_DISPLAY, "the device holds no display for %s",
				   kms_name(p, show));
	if (picked < 0 && show != (int)fnl_atomic_load_u32(&p->sel_good)) {
		kms_sel_refused(a, show, false);
		show = kms_sel_to_show(p);
		if (KMS_SEL_OUT(show) == p->disp_out ||
			take_display(a, p, KMS_SEL_OUT(show)))
			picked = pick_mode(a, &d, show, &mode);
	}
	FNL_TRY(picked);
	VK(a)->swapchain_refusal_lost =
		show != (int)fnl_atomic_load_u32(&p->sel_good);
	if (left >= 0 && left != p->disp_out)
		kms_output_off(a, left, show, false);
	FNL_TRY(pick_plane(a, &d, &plane, &stack));

	VkDisplayPlaneCapabilitiesKHR caps;
	VK_TRY(d.plane_caps(VK(a)->surface_phys, mode, plane, &caps));
	/* Opaque where the plane offers it, which the primary plane always
	 * does; the scene's alpha is not meant to show anything through. */
	VkDisplayPlaneAlphaFlagBitsKHR alpha =
		VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	if (!(caps.supportedAlpha & alpha)) {
		const VkDisplayPlaneAlphaFlagBitsKHR others[] = {
			VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR,
			VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR,
			VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR,
		};
		for (size_t i = 0; i < sizeof others / sizeof others[0]; i++)
			if (caps.supportedAlpha & others[i]) {
				alpha = others[i];
				break;
			}
	}

	int show_w, show_h;
	unsigned int show_mhz;
	kms_mode_size(p, show, &show_w, &show_h, &show_mhz);
	VkDisplaySurfaceCreateInfoKHR ci = {
		.sType           = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
		.displayMode     = mode,
		.planeIndex      = plane,
		.planeStackIndex = stack,
		.transform       = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.globalAlpha     = 1.0f,
		.alphaMode       = alpha,
		.imageExtent     = { (uint32_t)show_w, (uint32_t)show_h },
	};
	VkSurfaceKHR s = VK_NULL_HANDLE;
	VK_TRY(d.make(VK(a)->instance, &ci, NULL, &s));
	VK(a)->vk_surface = s;
	p->surf_sel = show;
	p->surf_presents = a->presents;
	return FNL_OK;
}

/* The display path's images are what the kernel scans out, so before their
 * swapchain goes the backend puts a frame of its own on the output
 * (kms_scanout_take()); the copy path's are the backend's already. */
static void scanout_take(App *a)
{
	if (VK(a)->vk_display != VK_NULL_HANDLE && !VK(a)->present_copy)
		kms_scanout_take(a);
}

/* The display path's flips are the driver's, which the backend never
 * sees, so the driver is asked when each frame was shown; the copy path's
 * are the backend's, which stamps them itself (kms.c). */
static bool driver_stamps(App *a)
{
	return (VK(a)->backend_ext_on & EXT_CAPS2) && !VK(a)->present_copy;
}

const VkBridge vk_bridge_kms = {
	.backend            = BACKEND_KMS,
	.instance_extensions = instance_extensions,
	.create_surface      = create_surface,
	.can_draw            = can_draw,
	.scanout_take        = scanout_take,
	.driver_stamps       = driver_stamps,
};

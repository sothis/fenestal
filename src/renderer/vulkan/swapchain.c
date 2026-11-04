/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The swapchain and everything that exists once per image of it -- views,
 * framebuffers, semaphores, command buffers -- with the window's render pass
 * and its depth attachment, and the retiring of all of it across a rebuild.
 */

#include "core/app.h"

#include "renderer/vulkan/vk_internal.h"

#include <limits.h>

/* ---- the window's pass -------------------------------------------------- */

/* The widest depth format the device takes as an optimal-tiling attachment.
 * Probed: the spec guarantees D16_UNORM, and only one of D32_SFLOAT and
 * X8_D24_UNORM_PACK32, not either individually. */
static VkFormat choose_depth_format(App *a)
{
	static const VkFormat WANT[] = {
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_X8_D24_UNORM_PACK32,
		VK_FORMAT_D16_UNORM,          /* mandatory, so the loop always ends */
	};
	for (size_t i = 0; i < sizeof WANT / sizeof WANT[0]; i++) {
		VkFormatProperties p;
		vkGetPhysicalDeviceFormatProperties(VK(a)->phys, WANT[i], &p);
		if (p.optimalTilingFeatures &
			VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
			return WANT[i];
	}
	/* Undefined is not a depth format, so it can mean "none worked". */
	return VK_FORMAT_UNDEFINED;
}

FnlResult create_render_pass(App *a)
{
	VK(a)->depth_format = choose_depth_format(a);
	if (VK(a)->depth_format == VK_FORMAT_UNDEFINED)
		return fnl_fail(FNL_ERR_NO_DEVICE, "no depth format supports being an "
										   "attachment on this device");

	const PassDesc d = {
		.samples = VK_SAMPLE_COUNT_1_BIT, .depth = true, .clear = true };
	VK(a)->render_pass = make_render_pass(a, &d);
	/* The call that failed is already recorded. */
	return VK(a)->render_pass != VK_NULL_HANDLE ? FNL_OK : FNL_ERR_SYSTEM;
}

/* ---- building one -------------------------------------------------------- */

/* The buffer's size in pixels: the window and its grab ring at the scale in
 * force, fitted to what the surface will take. */
static VkExtent2D choose_extent(App *a, const VkSurfaceCapabilitiesKHR *caps)
{
	/* Where the window is the output, a surface that names its extent is a
	 * display mode's, and takes that size and no other: anv refuses a
	 * swapchain of any other (VK_ERROR_INITIALIZATION_FAILED), which the
	 * rebuild after a mode switch would otherwise be, at the size last
	 * drawn, for as long as it retried. A headless surface names none. */
	if (BACKEND(a)->is_output && caps->currentExtent.width != UINT32_MAX)
		return caps->currentExtent;

	/* Capped at INT_MAX: a limit past it is no limit, and the extent is an
	 * int everywhere else. */
	const Extent lo = { (int)caps->minImageExtent.width,
						(int)caps->minImageExtent.height };
	const uint32_t max_w = caps->maxImageExtent.width;
	const uint32_t max_h = caps->maxImageExtent.height;
	const Extent hi = {
		max_w > INT_MAX ? INT_MAX : (int)max_w,
		max_h > INT_MAX ? INT_MAX : (int)max_h,
	};
	const Extent e = app_buffer_extent(a, lo, hi);
	return (VkExtent2D){ (uint32_t)e.w, (uint32_t)e.h };
}

/* Decided per build: opaque when there is no grab ring (fullscreen), which
 * keeps the surface eligible for a scanout plane; pre-multiplied when the
 * transparent ring is there. DESIGN *Composite alpha*. */
static VkCompositeAlphaFlagBitsKHR
choose_composite_alpha(const App *a, const VkSurfaceCapabilitiesKHR *caps)
{
	const VkCompositeAlphaFlagsKHR have = caps->supportedCompositeAlpha;

	if (a->draw_margin == 0 && (have & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
		return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	return (have & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
			   ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR
		 : (have & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
			   ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
			   : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

/* Every image's frame begun again: called before anything can fail, so a
 * stale generation or fence never sits beside new framebuffers. Generation
 * zero is never current, so every buffer records on first use.
 *
 * The fences are seeded with the previous frame slot's, not null: the
 * per-image instance buffers are device-scoped, so a retired swapchain's
 * pending command buffers still read them, and draw_frame's own wait
 * covers only one slot. Null on the first swapchain. DESIGN *Per-image
 * instance buffers*. */
static void image_frames_reset(App *a)
{
	VkFence seed = VK(a)->in_flight[(a->frame + MAX_FRAMES_IN_FLIGHT - 1) %
								MAX_FRAMES_IN_FLIGHT];
	for (uint32_t i = 0; i < OVERLAY_MAX_IMAGES; i++) {
		VK(a)->images_in_flight[i] = seed;
		VK(a)->img_cmd_gen[i] = 0;
	}
}

/* A command buffer per image, recorded over that image's framebuffer. */
static FnlResult image_cmds_make(App *a)
{
	VkCommandBufferAllocateInfo cai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = VK(a)->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = VK(a)->image_count,
	};
	VK_TRY(vkAllocateCommandBuffers(VK(a)->device, &cai, VK(a)->img_cmd));
	return FNL_OK;
}

/* What exists once per image of VK(a)->swapchain. Every array is null on entry
 * and stays null past whatever fails here, which destroy_swapchain() relies
 * on. */
static FnlResult create_image_objects(App *a)
{
	image_frames_reset(a);

	uint32_t n = 0;
	VkImage images[OVERLAY_MAX_IMAGES];
	if (VK(a)->surfaceless) {
		/* The renderer's own, made with their views (own_images_make()). */
		n = VK(a)->image_count;
		for (uint32_t i = 0; i < n; i++)
			images[i] = VK(a)->own[i].image;
	} else {
		VK_TRY(vkGetSwapchainImagesKHR(VK(a)->device, VK(a)->swapchain, &n,
									   NULL));
		/* Refused rather than wrapped, and before image_count is set: the
		 * arrays and the overlay's instance buffers are all sized by this
		 * ceiling, and two images on one buffer is tearing that only shows
		 * under load. */
		if (n > OVERLAY_MAX_IMAGES)
			return fnl_fail(FNL_ERR_SYSTEM,
							"the surface wants %u swapchain images; "
							"OVERLAY_MAX_IMAGES is %d",
							n, OVERLAY_MAX_IMAGES);
		VK_TRY(vkGetSwapchainImagesKHR(VK(a)->device, VK(a)->swapchain, &n,
									   images));
	}
	VK(a)->image_count = n;
	/* The copy path reads the images themselves; the swapchain's or the
	 * renderer's. */
	memcpy(VK(a)->sc_images, images, n * sizeof images[0]);

	if (!VK(a)->surfaceless)
		for (uint32_t i = 0; i < n; i++)
			VK_TRY(make_image_view(a, images[i], VK(a)->surf_format.format,
								   VK_IMAGE_ASPECT_COLOR_BIT,
								   &VK(a)->views[i]));

	/* One render-finished semaphore per *image*, not per frame in flight: the
	 * present waits on it and we cannot know which frame slot last drew it. */
	VkSemaphoreCreateInfo sem = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	for (uint32_t i = 0; i < n; i++)
		VK_TRY(vkCreateSemaphore(VK(a)->device, &sem, NULL,
								 &VK(a)->render_done[i]));
	return image_cmds_make(a);
}

/* One framebuffer per image, every one of them over the same depth view. */
static FnlResult create_framebuffers(App *a)
{
	if (FNL_FAULT("framebuffers"))
		return fnl_fail(FNL_ERR_SYSTEM, "FNL_FAULT: create_framebuffers");
	for (uint32_t i = 0; i < VK(a)->image_count; i++) {
		VkImageView attachments[2] = { VK(a)->views[i], VK(a)->depth.view };
		VK_TRY(make_framebuffer(a, VK(a)->render_pass, 2, attachments,
								(uint32_t)a->extent.w, (uint32_t)a->extent.h,
								&VK(a)->framebuffers[i]));
	}
	return FNL_OK;
}

/* The copy path's buffers, one per image, large enough for this extent:
 * made the first time, and made again only for a larger one. Again after a
 * drain, since a retired swapchain's command buffers may still be copying
 * into the old ones; the window on a bare display keeps its size, so that is
 * a rebuild nobody sees twice. */
static FnlResult ensure_readback(App *a)
{
	const VkDeviceSize need = (VkDeviceSize)a->extent.w * 4u *
							  (VkDeviceSize)a->extent.h;
	if (!VK(a)->present_copy || need <= VK(a)->readback_bytes)
		return FNL_OK;
	if (VK(a)->readback_bytes) {
		const VkResult wr = vkDeviceWaitIdle(VK(a)->device);
		if (vk_lost(a, wr))
			return FNL_ERR_DEVICE_LOST;
		VK_TRY(wr);
		destroy_instance_buffers(a, OVERLAY_MAX_IMAGES, VK(a)->readback,
								 &VK(a)->readback_mem, VK(a)->readback_map);
		VK(a)->readback_bytes = 0;
	}
	FNL_TRY(create_instance_buffers(a, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
									VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
									OVERLAY_MAX_IMAGES, VK(a)->readback,
									&VK(a)->readback_mem, VK(a)->readback_map));
	VK(a)->readback_bytes = need;
	return FNL_OK;
}

/* Never while the reaper is destroying one; see *the reaper*. One test of
 * reap_started decides both the lock and the unlock, so the pair cannot
 * disagree -- and -Wthread-safety can see that it does not. */
static VkResult create_swapchain_serialised(App *a,
											const VkSwapchainCreateInfoKHR *ci,
											VkSwapchainKHR *sc)
{
	if (!VK(a)->reap_started)
		return vkCreateSwapchainKHR(VK(a)->device, ci, NULL, sc);
	fnl_mutex_lock(&VK(a)->reap_serial);
	VkResult r = vkCreateSwapchainKHR(VK(a)->device, ci, NULL, sc);
	fnl_mutex_unlock(&VK(a)->reap_serial);
	return r;
}

bool vk_timing_calibrate(App *a)
{
#ifdef VK_EXT_present_timing
	const VkSwapchainCalibratedTimestampInfoEXT sci = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT,
		.swapchain = VK(a)->swapchain,
		.presentStage = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT,
		.timeDomainId = VK(a)->timing_domain,
	};
	const VkCalibratedTimestampInfoKHR info[2] = {
		{ .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,
		  .pNext = &sci,
		  .timeDomain = VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT },
		{ .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,
		  .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR },
	};
	uint64_t t[2], dev = 0;
	VK(a)->timing_cal_at = now_seconds();
	if (VK(a)->calibrated(VK(a)->device, 2, info, t, &dev) != VK_SUCCESS)
		return false;
	VK(a)->timing_offset_ns = (int64_t)(t[1] - t[0]);
	return true;
#else
	(void)a;
	return false;
#endif
}

/* Whether this surface's swapchain can be made with the driver's present
 * stamps: the surface reports when its first pixel goes out, which is the
 * moment KMS's own flip event stamps. */
static bool timing_surface(App *a)
{
#ifdef VK_EXT_present_timing
	if (!VK(a)->present_timing || !VK(a)->bridge->driver_stamps ||
		!VK(a)->bridge->driver_stamps(a))
		return false;
	VkPresentTimingSurfaceCapabilitiesEXT tc = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT,
	};
	VkSurfaceCapabilities2KHR c2 = {
		.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
		.pNext = &tc,
	};
	const VkPhysicalDeviceSurfaceInfo2KHR si = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
		.surface = VK(a)->vk_surface,
	};
	return VK(a)->surface_caps2(VK(a)->phys, &si, &c2) == VK_SUCCESS &&
		   tc.presentTimingSupported &&
		   (tc.presentStageQueries &
			VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
#else
	(void)a;
	return false;
#endif
}

/* The new swapchain's timing queue, and the id of its CLOCK_MONOTONIC
 * domain, now_seconds()'s clock, so no stamp is converted. False where
 * either is not had, with why in `why`: the frames then go unstamped. */
static bool timing_start(App *a, VkSwapchainKHR sc, char *why, size_t n)
{
#ifdef VK_EXT_present_timing
	const VkResult q = VK(a)->timing_queue_size(VK(a)->device, sc,
												TIMING_QUEUE);
	if (q != VK_SUCCESS) {
		snprintf(why, n, "the timing queue was refused (%d)", (int)q);
		return false;
	}
	VkTimeDomainKHR dom[8];
	uint64_t ids[8];
	VkSwapchainTimeDomainPropertiesEXT tp = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT,
		.timeDomainCount = 8,
		.pTimeDomains = dom,
		.pTimeDomainIds = ids,
	};
	uint64_t counter = 0;
	const VkResult r = VK(a)->timing_domains(VK(a)->device, sc, &tp,
											 &counter);
	if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
		snprintf(why, n, "its time domains were not said (%d)", (int)r);
		return false;
	}
	/* CLOCK_MONOTONIC as it is, where the swapchain has it; else the
	 * present stage's own clock, calibrated onto it. */
	size_t k = (size_t)snprintf(why, n, "none of its time domains is "
								"CLOCK_MONOTONIC or its own:");
	int local = -1;
	for (uint32_t i = 0; i < tp.timeDomainCount && i < 8; i++) {
		if (dom[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR) {
			VK(a)->timing_domain = ids[i];
			VK(a)->timing_local = false;
			VK(a)->timing_offset_ns = 0;
			return true;
		}
		if (dom[i] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT && local < 0)
			local = (int)i;
		if (k < n)
			k += (size_t)snprintf(why + k, n - k, " %d", (int)dom[i]);
	}
	if (local < 0)
		return false;
	VK(a)->timing_domain = ids[local];
	VK(a)->timing_local = true;
	if (!VK(a)->timing_calibrates || !vk_timing_calibrate(a)) {
		snprintf(why, n, "its own clock would not be read beside "
				 "CLOCK_MONOTONIC");
		return false;
	}
	return true;
#else
	(void)a; (void)sc;
	snprintf(why, n, "no VK_EXT_present_timing in these headers");
	return false;
#endif
}

/* The swapchain itself, on the surface: everything create_swapchain()
 * decided handed in. `scale` chains one-to-one presentation from the
 * image's top-left, so an image larger than the window stays valid. */
static FnlResult swapchain_make(App *a, const VkSurfaceCapabilitiesKHR *caps,
								VkExtent2D e, VkPresentModeKHR mode,
								uint32_t want, VkSwapchainKHR old_swapchain,
								bool scale)
{
	/* Copied from: for the copy path's readback of every frame, and for
	 * the snapshot a move takes of one (App::snap_owed) wherever the
	 * surface allows it, which every surface measured does. One that does
	 * not gives no snapshot, and the move fades through black. */
	VK(a)->snap_usage =
		VK(a)->present_copy ||
		(caps->supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
	VkSwapchainCreateInfoKHR ci = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = VK(a)->vk_surface,
		.minImageCount = want,
		.imageFormat = VK(a)->surf_format.format,
		.imageColorSpace = VK(a)->surf_format.colorSpace,
		.imageExtent = e,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
					  (VK(a)->snap_usage ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT
										 : 0u),
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = caps->currentTransform,
		.compositeAlpha = choose_composite_alpha(a, caps),
		.presentMode = mode,
		.clipped = VK_TRUE,
		.oldSwapchain = old_swapchain,
	};
	/* Nothing else sets pNext. The struct lives until the create returns. */
	const VkSwapchainPresentScalingCreateInfoKHR scaling = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_KHR,
		.scalingBehavior = VK_PRESENT_SCALING_ONE_TO_ONE_BIT_KHR,
		.presentGravityX = VK_PRESENT_GRAVITY_MIN_BIT_KHR,
		.presentGravityY = VK_PRESENT_GRAVITY_MIN_BIT_KHR,
	};
	if (scale)
		ci.pNext = &scaling;
	const bool timed = timing_surface(a);
#ifdef VK_EXT_present_timing
	if (timed)
		ci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
#endif
	/* With create_swapchain()'s capability query, one of the two calls that
	 * can report a lost device or surface before any drawing, so it
	 * classifies rather than fails. Built into a local first: a half-dead
	 * handle left in RendererState::swapchain would have the teardown
	 * destroy what the retired entry still owns. */
	VkSwapchainKHR sc = VK_NULL_HANDLE;
	VkResult cr = create_swapchain_serialised(a, &ci, &sc);
	if (VK(a)->swapchain_refusal_lost && cr == VK_SUCCESS &&
		FNL_FAULT("kms-swapchain-refused")) {
		vkDestroySwapchainKHR(VK(a)->device, sc, NULL);
		sc = VK_NULL_HANDLE;
		cr = VK_ERROR_INITIALIZATION_FAILED;
	}
	/* A display surface's swapchain is test-committed image by image
	 * (Mesa's wsi_display_surface_create_swapchain()), so a mode the
	 * kernel will not show is refused here and never reaches a present. */
	if (VK(a)->swapchain_refusal_lost && cr == VK_ERROR_INITIALIZATION_FAILED) {
		fnl_log(FNL_LOG_WARN, "the display refused a swapchain on its "
				"mode; the surface is made again");
		cr = VK_ERROR_SURFACE_LOST_KHR;
	}
	if (vk_lost(a, cr)) {
		VK(a)->swapchain   = VK_NULL_HANDLE;
		VK(a)->image_count = 0;
		return FNL_ERR_DEVICE_LOST;
	}
	VK_TRY(cr);
	VK(a)->swapchain = sc;
	/* Its presents are numbered from 1 again (App::present_chain). */
	a->present_chain++;
	VK(a)->timing_owed = 0;
	if (VK(a)->present_timing) {
		char why[sizeof VK(a)->timing_why] = "the surface reports no first "
											 "pixel out";
		VK(a)->timing_on = timed && timing_start(a, sc, why, sizeof why);
		snprintf(VK(a)->timing_why, sizeof VK(a)->timing_why, "%s",
				 VK(a)->timing_on ? "" : why);
	} else {
		VK(a)->timing_on = false;
	}
	VK(a)->swap_spent  = false;
	VK(a)->swap_fresh  = true;
	/* A deliberately larger image is not the surface's extent, so a run
	 * of those being out of date does not ask for the nudge: that nudge
	 * is for a swapchain made at the window's own size. */
	VK(a)->swap_agreed = e.width == caps->currentExtent.width &&
						 e.height == caps->currentExtent.height;
	if (scale && !VK(a)->scale_said) {
		fnl_log(FNL_LOG_INFO, "Vulkan: the swapchain presents one to one "
				"from its corner, so a resize keeps it");
		VK(a)->scale_said = true;
	}
	return FNL_OK;
}

/* The surfaceless window's images (RendererState::surfaceless): `n` colour
 * images of the renderer's at the window's size, drawn into and copied out
 * of. The views go to views[], as a swapchain image's do, so that
 * create_image_objects() and the teardown treat both alike; own[] keeps the
 * image and its memory. */
static FnlResult own_images_make(App *a, VkExtent2D e, uint32_t n)
{
	if (VK(a)->dxgi)
		return VK(a)->bridge->own_images(a, e, n);
	VK(a)->swapchain = VK_NULL_HANDLE;
	VK(a)->own_next = 0;
	VK(a)->snap_usage = true;          /* made with TRANSFER_SRC below */
	for (uint32_t i = 0; i < n; i++) {
		TargetImage *t = &VK(a)->own[i];
		if (!create_target_image(a, e.width, e.height,
								 VK(a)->surf_format.format,
								 VK_SAMPLE_COUNT_1_BIT,
								 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
									 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
								 false, t))
			return fnl_fail(FNL_ERR_SYSTEM, "could not create the window's "
							"%ux%u image %u", e.width, e.height, i);
		VK(a)->views[i] = t->view;
		t->view = VK_NULL_HANDLE;
		/* Counted as made, so a teardown after a later one fails frees
		 * this one's view. */
		VK(a)->image_count = i + 1;
	}
	VK(a)->swap_fresh  = true;
	VK(a)->swap_agreed = true;
	return FNL_OK;
}

/* What a surface would answer, where there is none: any size the device
 * makes an image of, the window's own (choose_extent()), and two images --
 * one drawn while the other is read back. */
static void surfaceless_caps(App *a, VkSurfaceCapabilitiesKHR *caps)
{
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(VK(a)->phys, &props);
	const uint32_t max = props.limits.maxImageDimension2D;
	*caps = (VkSurfaceCapabilitiesKHR){
		.minImageCount  = 2,
		.maxImageCount  = 2,
		.currentExtent  = { UINT32_MAX, UINT32_MAX },
		.minImageExtent = { 1, 1 },
		.maxImageExtent = { max, max },
		.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
	};
}

/* The window pass's depth image, kept across sizes while the extent fits
 * inside it and grown a DEPTH_STEP at a time when it does not. One that
 * only covers the window is made again at every size, and allocating it is
 * a third of what a size costs the Pi 4 on weston (6 of 21 ms, DESIGN *At
 * rest*). A framebuffer may be smaller than its attachments, and the pass
 * clears depth over its render area alone, so the larger image is the same
 * attachment.
 *
 * Every framebuffer since the image was made names it, retired ones
 * included, so one grown out of goes with the newest retired entry, the
 * last of them to expire; with none it is destroyed after a drain. */
#define DEPTH_STEP 256u

static uint32_t depth_side(uint32_t want, uint32_t max)
{
	uint32_t side = (want + DEPTH_STEP - 1) / DEPTH_STEP * DEPTH_STEP;
	return side > max && max >= want ? max : side;
}

static FnlResult depth_ensure(App *a, VkExtent2D e,
							  const VkSurfaceCapabilitiesKHR *caps)
{
	if (VK(a)->depth.image != VK_NULL_HANDLE &&
		e.width <= VK(a)->depth_w && e.height <= VK(a)->depth_h)
		return FNL_OK;

	if (VK(a)->depth.image != VK_NULL_HANDLE) {
		RetiredSwapchain *r = VK(a)->retired_count > 0
			? &VK(a)->retired[VK(a)->retired_count - 1] : NULL;
		if (r && r->depth.image == VK_NULL_HANDLE) {
			r->depth = VK(a)->depth;
		} else {
			const VkResult wr = vkQueueWaitIdle(VK(a)->queue);
			if (vk_lost(a, wr))
				return FNL_ERR_DEVICE_LOST;
			VK_TRY(wr);
			destroy_target_image(a, &VK(a)->depth);
		}
		memset(&VK(a)->depth, 0, sizeof VK(a)->depth);
	}

	/* Transient where the device has such memory: the pass clears it and
	 * never stores it, and its external dependency orders each frame's
	 * clear after the last frame's depth writes (see make_render_pass). */
	const uint32_t w = depth_side(e.width, caps->maxImageExtent.width);
	const uint32_t h = depth_side(e.height, caps->maxImageExtent.height);
	if (!create_target_image(a, w, h, VK(a)->depth_format,
							 VK_SAMPLE_COUNT_1_BIT,
							 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, true,
							 &VK(a)->depth)) {
		VK(a)->depth_w = VK(a)->depth_h = 0;
		return fnl_fail(FNL_ERR_SYSTEM,
						"could not create the %ux%u depth attachment", w, h);
	}
	VK(a)->depth_w = w;
	VK(a)->depth_h = h;
	return FNL_OK;
}

/* A swapchain's images where the window can show part of them
 * (App::buffer_crops, or DXGI): app_buffer_room()'s, so the sizes of a
 * drag fit inside (resize_in_place()); elsewhere the window's size.
 * X11's present scaling is scale_extent()'s, not this: it needs the
 * present mode's gravity, which a size alone does not say. */
static VkExtent2D buffer_extent(App *a, VkExtent2D e,
								const VkSurfaceCapabilitiesKHR *caps)
{
	if (!VK(a)->dxgi &&
		(!a->buffer_crops || VK(a)->surfaceless || VK(a)->present_copy))
		return e;
	const Extent max = {
		caps->maxImageExtent.width > INT_MAX
			? INT_MAX : (int)caps->maxImageExtent.width,
		caps->maxImageExtent.height > INT_MAX
			? INT_MAX : (int)caps->maxImageExtent.height };
	const Extent r = app_buffer_room(a, (Extent){ (int)e.width,
												  (int)e.height }, max);
	return (VkExtent2D){ (uint32_t)r.w, (uint32_t)r.h };
}

/* Inside the range a scaled image must lie in. A max of zero is no range:
 * chaining the scaling struct outside it makes the create fail. */
static bool extent_scaled(VkExtent2D e,
						  const VkSurfacePresentScalingCapabilitiesKHR *sc)
{
	return sc->maxScaledImageExtent.width > 0 &&
		   sc->maxScaledImageExtent.height > 0 &&
		   e.width >= sc->minScaledImageExtent.width &&
		   e.height >= sc->minScaledImageExtent.height &&
		   e.width <= sc->maxScaledImageExtent.width &&
		   e.height <= sc->maxScaledImageExtent.height;
}

/* One to one from the top-left, for this present mode. False where the
 * query fails or a bit is missing; present_scale stays, because another
 * mode may have them. */
static bool scale_caps(App *a, VkPresentModeKHR mode,
					   VkSurfacePresentScalingCapabilitiesKHR *out)
{
	VkSurfacePresentModeKHR pm = {
		.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR,
		.presentMode = mode,
	};
	const VkPhysicalDeviceSurfaceInfo2KHR si = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
		.pNext = &pm,
		.surface = VK(a)->vk_surface,
	};
	VkSurfacePresentScalingCapabilitiesKHR sc = {
		.sType =
			VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR,
	};
	VkSurfaceCapabilities2KHR c2 = {
		.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
		.pNext = &sc,
	};
	if (!VK(a)->surface_caps2 ||
		VK(a)->surface_caps2(VK(a)->phys, &si, &c2) != VK_SUCCESS)
		return false;
	*out = sc;
	return (sc.supportedPresentScaling &
			VK_PRESENT_SCALING_ONE_TO_ONE_BIT_KHR) != 0 &&
		   (sc.supportedPresentGravityX &
			VK_PRESENT_GRAVITY_MIN_BIT_KHR) != 0 &&
		   (sc.supportedPresentGravityY &
			VK_PRESENT_GRAVITY_MIN_BIT_KHR) != 0;
}

/* `b` is what buffer_extent() chose. Where this mode can present one to
 * one from the corner, the image is app_buffer_room()'s when that fits
 * the scaled range, and otherwise the window's when the window itself
 * fits -- chained either way, so a size that changes under an in-flight
 * present is not by itself out of date. A room the range cannot hold is
 * not chained: room_side() returns the window when the scaled max is
 * smaller than it, and that window may lie outside the range. */
static VkExtent2D scale_extent(App *a, VkExtent2D e, VkExtent2D b,
							   const VkSurfaceCapabilitiesKHR *caps,
							   VkPresentModeKHR mode, bool *scale)
{
	*scale = false;
	if (!VK(a)->present_scale || !VK(a)->surface_caps2 || VK(a)->dxgi ||
		a->buffer_crops || VK(a)->surfaceless || VK(a)->present_copy)
		return b;
	VkSurfacePresentScalingCapabilitiesKHR sc;
	if (!scale_caps(a, mode, &sc))
		return b;
	uint32_t mx = caps->maxImageExtent.width;
	uint32_t my = caps->maxImageExtent.height;
	if (sc.maxScaledImageExtent.width < mx)
		mx = sc.maxScaledImageExtent.width;
	if (sc.maxScaledImageExtent.height < my)
		my = sc.maxScaledImageExtent.height;
	const Extent lim = {
		mx > (uint32_t)INT_MAX ? INT_MAX : (int)mx,
		my > (uint32_t)INT_MAX ? INT_MAX : (int)my,
	};
	const Extent room = app_buffer_room(a, (Extent){ (int)e.width,
													 (int)e.height }, lim);
	const VkExtent2D rb = { (uint32_t)room.w, (uint32_t)room.h };
	if (extent_scaled(rb, &sc)) {
		*scale = true;
		return rb;
	}
	if (extent_scaled(e, &sc))
		*scale = true;
	return b;
}

FnlResult create_swapchain(App *a, VkSwapchainKHR old_swapchain)
{
	/* The swapchain a wait names is not to be retired under it. */
	shown_idle(a);
	/* Re-query caps every build: currentTransform and the image-extent /
	 * image-count limits can change at runtime (output rotation, hotplug). */
	VkSurfaceCapabilitiesKHR caps;
	VkResult capr = VK_SUCCESS;
	if (VK(a)->surfaceless)
		surfaceless_caps(a, &caps);
	else
		capr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VK(a)->phys,
														 VK(a)->vk_surface,
														 &caps);
	if (vk_lost(a, capr)) {
		VK(a)->swapchain   = VK_NULL_HANDLE;
		VK(a)->image_count = 0;
		return FNL_ERR_DEVICE_LOST;
	}
	VK_TRY(capr);

	/* The ceiling the scale is resolved against, re-published every build. The
	 * pointer thread cuts the scale to fit it (clamp_device_scale120 in
	 * wayland.c), so the clamp in choose_extent() is only a backstop. */
	fnl_atomic_store_u32(&a->max_extent_w, caps.maxImageExtent.width);
	fnl_atomic_store_u32(&a->max_extent_h, caps.maxImageExtent.height);

	const VkExtent2D e = choose_extent(a, &caps);
	a->extent = (Extent){ (int)e.width, (int)e.height };
	/* The one place the present mode changes, because this is where a
	 * swapchain is made with it; the loop only asks whether it matches (see
	 * vk_present_mode_matches). Read into a local once: a field another thread
	 * observes is not a scratch variable. The scaling query is per mode, so
	 * the image size is decided after it. */
	const VkPresentModeKHR mode =
		vk_wanted_present_mode(a, fnl_atomic_load_bool(&a->vsync));
	vk_set_present_mode(a, mode);
	bool scale = false;
	const VkExtent2D b = scale_extent(a, e, buffer_extent(a, e, &caps),
									  &caps, mode, &scale);
	VK(a)->sc_extent = b;
	a->buffer = b.width != e.width || b.height != e.height
		? (Extent){ (int)b.width, (int)b.height } : (Extent){ 0, 0 };

	uint32_t want = caps.minImageCount;
	if (mode == VK_PRESENT_MODE_MAILBOX_KHR && want < 3)
		want = 3;
	if (want < 2) want = 2;
	if (caps.maxImageCount > 0 && want > caps.maxImageCount)
		want = caps.maxImageCount;

	if (VK(a)->surfaceless)
		FNL_TRY(own_images_make(a, b, want));
	else
		FNL_TRY(swapchain_make(a, &caps, b, mode, want, old_swapchain,
							   scale));

	FNL_TRY(create_image_objects(a));
	FNL_TRY(ensure_readback(a));

	FNL_TRY(depth_ensure(a, e, &caps));
	return create_framebuffers(a);
}

/* ---- retiring and destroying -------------------------------------------- */

/* Move the live swapchain into `r`, leaving every live handle null. Handed
 * over, not shared: a create_swapchain() that then fails part-way must not
 * leave this side naming what the entry owns, or the loop's retry would
 * retire it a second time and destroy it twice.
 *
 * images_in_flight and img_cmd_gen stay behind. They describe frames, not
 * swapchain objects, and create_swapchain() resets both. So does the depth
 * image, which outlives a swapchain (depth_ensure()). */
static void swapchain_hand_over(App *a, RetiredSwapchain *r)
{
	r->swapchain = VK(a)->swapchain;
	memcpy(r->views, VK(a)->views, sizeof r->views);
	memcpy(r->framebuffers, VK(a)->framebuffers, sizeof r->framebuffers);
	memcpy(r->render_done, VK(a)->render_done, sizeof r->render_done);
	memcpy(r->img_cmd, VK(a)->img_cmd, sizeof r->img_cmd);
	memset(&r->depth, 0, sizeof r->depth);
	memcpy(r->own, VK(a)->own, sizeof r->own);
	r->image_count = VK(a)->image_count;
	r->countdown   = MAX_FRAMES_IN_FLIGHT;

	/* All-zero is VK_NULL_HANDLE in both of a handle's representations. */
	VK(a)->swapchain = VK_NULL_HANDLE;
	memset(VK(a)->views, 0, sizeof VK(a)->views);
	memset(VK(a)->framebuffers, 0, sizeof VK(a)->framebuffers);
	memset(VK(a)->render_done, 0, sizeof VK(a)->render_done);
	memset(VK(a)->img_cmd, 0, sizeof VK(a)->img_cmd);
	memset(VK(a)->own, 0, sizeof VK(a)->own);
	VK(a)->image_count = 0;
}

/* ---- the reaper -----------------------------------------------------------
 *
 * vkDestroySwapchainKHR on a thread of its own, where it is slow.
 *
 * On Intel's Windows driver the call takes 8 to 10 ms, once per resize and
 * on the thread that draws: a third of what a new size costs there and a
 * frame that is late by it every time. Everything else a retired swapchain
 * owns still goes on the frame's thread -- the views, the framebuffers, the
 * semaphores, and the command buffers, whose pool is that thread's alone.
 * The swapchain handle is the one thing Vulkan asks to be synchronised for
 * this call, and nobody else has it by then.
 *
 * Only where a destroy has been *measured* slow (REAP_SLOW): elsewhere the
 * call is a tenth of a millisecond and a thread would be a third party in
 * the driver for nothing. A queue that is full is a destroy made in line.
 *
 * **Never beside vkCreateSwapchainKHR** (RendererState::reap_serial).
 * Vulkan allows the two at once -- they name different swapchains -- and
 * that driver does not survive it: four runs of twelve ended in a corrupted
 * heap or an access violation inside it, and none of fourteen with the two
 * kept apart. What is left to gain is the destroy done while a frame is
 * drawn, which is the resize slower than the rebuild: 34 to 44 fps and 42
 * to 53 on the two devices at thirty sizes a second, nothing at sixty. */
#define REAP_SLOW 0.002

static void *reap_main(void *arg)
{
	App *a = arg;

	fnl_mutex_lock(&VK(a)->reap_lock);
	for (;;) {
		while (VK(a)->reap_n == 0 && !VK(a)->reap_quit)
			fnl_cond_wait_for(&VK(a)->reap_cond, &VK(a)->reap_lock, 1.0);
		if (VK(a)->reap_n == 0)
			break;
		const VkSwapchainKHR sc = VK(a)->reap_q[--VK(a)->reap_n];
		VK(a)->reap_busy = true;
		fnl_mutex_unlock(&VK(a)->reap_lock);

		fnl_mutex_lock(&VK(a)->reap_serial);
		vkDestroySwapchainKHR(VK(a)->device, sc, NULL);
		fnl_mutex_unlock(&VK(a)->reap_serial);

		fnl_mutex_lock(&VK(a)->reap_lock);
		VK(a)->reap_busy = false;
	}
	fnl_mutex_unlock(&VK(a)->reap_lock);
	return NULL;
}

/* Give the entry's swapchain to the reaper. False is "destroy it yourself":
 * not wanted here, no thread to be had, or the queue is full. */
static bool reap_hand_over(App *a, RetiredSwapchain *r)
{
	if (!VK(a)->reap_async)
		return false;
	if (!VK(a)->reap_started) {
		fnl_mutex_init(&VK(a)->reap_lock);
		fnl_mutex_init(&VK(a)->reap_serial);
		fnl_cond_init(&VK(a)->reap_cond);
		VK(a)->reap_n = 0;
		VK(a)->reap_busy = VK(a)->reap_quit = false;
		if (!fnl_thread_start(&VK(a)->reap_thread, reap_main, a)) {
			fnl_cond_destroy(&VK(a)->reap_cond);
			fnl_mutex_destroy(&VK(a)->reap_lock);
			fnl_mutex_destroy(&VK(a)->reap_serial);
			VK(a)->reap_async = false;
			return false;
		}
		VK(a)->reap_started = true;
	}

	bool taken = false;
	fnl_mutex_lock(&VK(a)->reap_lock);
	if (VK(a)->reap_n < REAP_MAX) {
		VK(a)->reap_q[VK(a)->reap_n++] = r->swapchain;
		taken = true;
	}
	fnl_mutex_unlock(&VK(a)->reap_lock);
	if (taken) {
		fnl_cond_signal(&VK(a)->reap_cond);
		r->swapchain = VK_NULL_HANDLE;
	}
	return taken;
}

/* Ahead of anything that ends the device: every swapchain handed over is
 * destroyed and the thread is gone when this returns. A later resize on a
 * device that came back starts another. */
void reap_stop(App *a)
{
	if (!VK(a)->reap_started)
		return;
	fnl_mutex_lock(&VK(a)->reap_lock);
	VK(a)->reap_quit = true;
	fnl_mutex_unlock(&VK(a)->reap_lock);
	fnl_cond_signal(&VK(a)->reap_cond);
	fnl_thread_join(VK(a)->reap_thread);
	fnl_cond_destroy(&VK(a)->reap_cond);
	fnl_mutex_destroy(&VK(a)->reap_lock);
	fnl_mutex_destroy(&VK(a)->reap_serial);
	VK(a)->reap_started = false;
}

/* Queue idle, not the frame fences, is what the caller owes before this:
 * render_done[] is waited on by vkQueuePresentKHR, which has no fence in
 * Vulkan 1.0 (VUID-vkDestroySemaphore-semaphore-05149).
 *
 * Safe on a half-built entry: whatever was not made is null, and destroying
 * null does nothing. The command buffers are the exception --
 * vkFreeCommandBuffers with a count of zero is invalid usage -- and they are
 * allocated all at once, so the first answers for the rest. */
static void retired_destroy(App *a, RetiredSwapchain *r, bool may_defer)
{
	for (uint32_t i = 0; i < r->image_count; i++) {
		vkDestroyFramebuffer(VK(a)->device, r->framebuffers[i], NULL);
		vkDestroyImageView(VK(a)->device, r->views[i], NULL);
		vkDestroySemaphore(VK(a)->device, r->render_done[i], NULL);
	}
	destroy_target_image(a, &r->depth);
	/* After their views, above. */
	for (uint32_t i = 0; i < OVERLAY_MAX_IMAGES; i++)
		destroy_target_image(a, &r->own[i]);
	if (r->image_count && r->img_cmd[0] != VK_NULL_HANDLE)
		vkFreeCommandBuffers(VK(a)->device, VK(a)->cmd_pool, r->image_count,
							 r->img_cmd);
	if (may_defer && r->swapchain != VK_NULL_HANDLE && reap_hand_over(a, r))
		return;
	const double t0 = now_seconds();
	vkDestroySwapchainKHR(VK(a)->device, r->swapchain, NULL);
	/* Asked of the one retired mid-run alone: at teardown nobody is waiting
	 * for the frame this would have held up. */
	if (may_defer && r->swapchain != VK_NULL_HANDLE &&
		now_seconds() - t0 > REAP_SLOW)
		VK(a)->reap_async = true;
}

/* The live swapchain, at teardown. The caller has idled the device. */
void destroy_swapchain(App *a)
{
	RetiredSwapchain r;

	shown_idle(a);

	if (VK(a)->bridge->scanout_take && VK(a)->swapchain != VK_NULL_HANDLE)
		VK(a)->bridge->scanout_take(a);
	swapchain_hand_over(a, &r);
	a->buffer = (Extent){ 0, 0 };
	VK(a)->sc_extent = (VkExtent2D){ 0, 0 };
	/* The depth image too, at teardown: the device is idle. */
	r.depth = VK(a)->depth;
	memset(&VK(a)->depth, 0, sizeof VK(a)->depth);
	VK(a)->depth_w = VK(a)->depth_h = 0;
	retired_destroy(a, &r, false);
	/* The fences these named go with the device. */
	memset(VK(a)->images_in_flight, 0, sizeof VK(a)->images_in_flight);
	memset(VK(a)->img_cmd_gen, 0, sizeof VK(a)->img_cmd_gen);
}

void free_all_retired(App *a)
{
	if (VK(a)->retired_count == 0)
		return;
	vkQueueWaitIdle(VK(a)->queue);          /* one drain covers every entry */
	for (int i = 0; i < VK(a)->retired_count; i++)
		retired_destroy(a, &VK(a)->retired[i], false);
	VK(a)->retired_count = 0;
}

/* How long since the last rebuild for the next one to wait, and for how
 * long: a quarter of a second is past any run of rebuilds, and 20 ms is a
 * refresh at 50 Hz. */
#define SWAP_REST_AFTER 0.25
#define SWAP_REST       0.020

/* A new size inside the live swapchain's images, where the window shows
 * part of them (App::buffer): the framebuffers and the command buffers
 * recorded over them are made again, and the swapchain stays. A swapchain
 * made at each size is its images allocated and cleared by the driver --
 * 14 ms of the Pi 4's 16.7 under weston, which halved the frame rate of a
 * drag (DESIGN *A swapchain larger than the window*). What is replaced is
 * retired as a swapchain is, in an entry with no swapchain of its own.
 * Not where app_buffer_room_fits() says the images no longer serve, nor
 * once the swapchain has been said suboptimal, nor over one whose build
 * stopped part-way: its last framebuffer, the build's last step, is
 * missing, and so may be its views, semaphores or command buffers. 1 when
 * done, 0 when a swapchain is wanted, -1 on a failure. */
static int resize_in_place(App *a)
{
	if (a->buffer.w == 0 || VK(a)->image_count == 0 ||
		VK(a)->framebuffers[VK(a)->image_count - 1] == VK_NULL_HANDLE ||
		(VK(a)->swapchain == VK_NULL_HANDLE && !VK(a)->dxgi) ||
		VK(a)->swap_spent ||
		!vk_present_mode_matches(a, fnl_atomic_load_bool(&a->vsync)))
		return 0;
	/* A failure is asked again, and classified, by the full rebuild. */
	VkSurfaceCapabilitiesKHR caps;
	if (VK(a)->surfaceless)
		surfaceless_caps(a, &caps);
	else if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VK(a)->phys,
													   VK(a)->vk_surface,
													   &caps) != VK_SUCCESS)
		return 0;
	const VkExtent2D e = choose_extent(a, &caps);
	const VkExtent2D b = VK(a)->sc_extent;
	if (!app_buffer_room_fits(a, (Extent){ (int)e.width, (int)e.height },
							  (Extent){ (int)b.width, (int)b.height }))
		return 0;

	if (VK(a)->retired_count == MAX_RETIRED)
		free_all_retired(a);
	RetiredSwapchain *r = &VK(a)->retired[VK(a)->retired_count++];
	memset(r, 0, sizeof *r);
	memcpy(r->framebuffers, VK(a)->framebuffers, sizeof r->framebuffers);
	memcpy(r->img_cmd, VK(a)->img_cmd, sizeof r->img_cmd);
	r->image_count = VK(a)->image_count;
	r->countdown = MAX_FRAMES_IN_FLIGHT;
	memset(VK(a)->framebuffers, 0, sizeof VK(a)->framebuffers);
	memset(VK(a)->img_cmd, 0, sizeof VK(a)->img_cmd);

	a->extent = (Extent){ (int)e.width, (int)e.height };
	image_frames_reset(a);
	if (image_cmds_make(a) < 0 || depth_ensure(a, e, &caps) < 0 ||
		create_framebuffers(a) < 0)
		return -1;
	return 1;
}

/* Build a new swapchain, keeping the old one (and everything derived from it)
 * alive until enough frames have been submitted that it is provably idle.
 * Destroying it immediately would race presents still in flight. */
bool recreate_swapchain(App *a)
{
	const int in_place = resize_in_place(a);
	if (in_place != 0)
		return in_place > 0;

	if (VK(a)->retired_count == MAX_RETIRED)
		free_all_retired(a);

	/* The first of a run of rebuilds lets the swapchain come to rest
	 * before it is replaced: the queue drained, and one refresh for its
	 * last present to be shown. Intel's Windows driver otherwise does not
	 * always come back from the create or the destroy that follows -- one
	 * start in twenty, the window stopped for good -- and with this none
	 * of ninety did. Resting before every one was tried too and was no
	 * better. Dated from the last *rebuild* and not from the swapchain's
	 * age: the one a window opens with is as new as any, and a window
	 * moved to a monitor of another scale as it opened stopped all the
	 * same. */
	const double t_now = now_seconds();
	if (VK(a)->swapchain != VK_NULL_HANDLE &&
		t_now - VK(a)->swap_rebuilt_at > SWAP_REST_AFTER) {
		vkQueueWaitIdle(VK(a)->queue);
		fnl_sleep_seconds(SWAP_REST);
	}
	VK(a)->swap_rebuilt_at = now_seconds();

	RetiredSwapchain *r = &VK(a)->retired[VK(a)->retired_count++];
	swapchain_hand_over(a, r);

	/* `old_swapchain` comes from the entry: the live handle is null by now. */
	return create_swapchain(a, r->swapchain) >= 0;
}

bool vk_present_mode_matches(const App *a, bool vsync)
{
	return vk_present_mode(a) == vk_wanted_present_mode(a, vsync);
}

/* Each retired swapchain is freed once MAX_FRAMES_IN_FLIGHT further frames
 * have been submitted: every submit is queue-ordered after the retired
 * swapchain's last present and waits the frame's fence, so its command buffers
 * are idle. Freed as each expires rather than batched; DESIGN *Per-image
 * instance buffers* has the measurement. */
void vk_retire_tick(App *a)
{
	bool drained = false;   /* one drain covers every entry, as in
							 * free_all_retired -- not one apiece */
	for (int i = VK(a)->retired_count - 1; i >= 0; i--)
		if (--VK(a)->retired[i].countdown == 0) {
			if (!drained) {
				vkQueueWaitIdle(VK(a)->queue);
				drained = true;
			}
			retired_destroy(a, &VK(a)->retired[i], true);
			VK(a)->retired[i] = VK(a)->retired[--VK(a)->retired_count];
		}
}

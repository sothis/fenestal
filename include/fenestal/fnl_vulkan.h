/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Fenestal's Vulkan interop.
 *
 * Present only in a build configured with the Vulkan renderer, and included
 * by nothing in fenestal.h, so the portable header does not drag
 * <vulkan/vulkan.h> into every consumer.
 *
 * Fenestal owns the window, the device, the swapchain, the present schedule,
 * the render scale, the three antialiasing modes and the text overlay; the scene
 * is yours. Everything up to "here is a command buffer, inside a pass, with
 * the viewport set" is portable, and what you record into it is not.
 */

#ifndef FENESTAL_VULKAN_H
#define FENESTAL_VULKAN_H

#include <fenestal/fenestal.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a scene needs of Vulkan beyond what the library enables for itself:
 * handed to fnl_init() as FnlInitDesc::vulkan_setup, with `size` set to
 * sizeof (FnlVulkanSetup) and everything you do not need left zero or NULL.
 * The library builds the instance and the device from it, and again from it
 * after every device loss, so it and everything it points at must stay valid
 * until fnl_shutdown().
 *
 *   api_version    the Vulkan version to ask for, as VK_MAKE_API_VERSION
 *                  builds it. 0 is the highest the loader offers, up to
 *                  1.3. A version the loader or the physical device lacks
 *                  is FNL_ERR_UNSUPPORTED rather than a lower one.
 *   *_extensions   names added to the library's own, at most
 *                  FNL_VULKAN_SETUP_EXTENSIONS_MAX of each. One the loader
 *                  or the device does not offer fails the window's creation
 *                  and the log names it: a scene that needs it has nothing
 *                  to draw without it.
 *   instance_pnext a structure chain for VkInstanceCreateInfo, such as
 *                  VkValidationFeaturesEXT.
 *   features       core features to enable, beside the library's own. One
 *                  the device lacks fails with its name.
 *   feature_chain  feature structures of the 1.1 to 1.3 promotions and the
 *                  extensions, for VkDeviceCreateInfo::pNext -- timeline
 *                  semaphores, dynamic rendering, buffer device address.
 *                  Not a VkPhysicalDeviceFeatures2, which a device made with
 *                  `features` cannot also take: that is refused. A feature
 *                  the device lacks is the driver's refusal of the device,
 *                  logged with its result.
 *
 * The library's own device extensions are VK_KHR_swapchain and what it
 * finds offered of present wait and present id, present timing and
 * calibrated timestamps, swapchain maintenance 1, memory budget and the
 * portability subset; its core feature is dual-source blending. Names of
 * these in a list are harmless. A scene that relies on none of this passes
 * no setup, and is made at the highest version the loader has, which is
 * what api_version above says it got. */
#define FNL_VULKAN_SETUP_EXTENSIONS_MAX 32
typedef struct FnlVulkanSetup {
	size_t       size;
	uint32_t     api_version;
	uint32_t     instance_extension_count;
	const char *const *instance_extensions;
	const void  *instance_pnext;
	uint32_t     device_extension_count;
	const char *const *device_extensions;
	const VkPhysicalDeviceFeatures *features;
	const void  *feature_chain;
} FnlVulkanSetup;

/* Everything needed to allocate, upload and submit alongside Fenestal. The
 * queue is shared, not handed over: submissions on it are ordered against the
 * frame's own. Submitting on `queue` directly is safe only while no other
 * window's render thread can submit; fnl_vulkan_queue_submit() takes the lock
 * the library's own submits take, and is the call to make otherwise.
 *
 * Valid for one device generation -- that is, from an on_gpu_created to the
 * next on_gpu_lost. Every handle here changes across a loss.
 *
 * get_instance_proc_addr is the loader the library opened (MoltenVK's own
 * on a Mac). Ask it, with `instance`, for every entry point the scene
 * calls -- the device's included -- and link no loader: a program that
 * links one does not start on a machine without it, where the library
 * would have stepped past Vulkan to another renderer. Define
 * VK_NO_PROTOTYPES before this header and a call not asked for is a
 * compile error rather than a link. Ask again after every on_gpu_created,
 * since an entry is the instance's. */
typedef struct {
	VkInstance       instance;
	VkPhysicalDevice physical_device;
	VkDevice         device;
	VkQueue          queue;
	uint32_t         queue_family;
	VkPipelineCache  pipeline_cache;   /* shared: your pipelines warm it */
	PFN_vkGetInstanceProcAddr get_instance_proc_addr;
	/* The Vulkan version the device was made for: the lesser of the
	 * instance's and the physical device's, as VK_MAKE_API_VERSION builds
	 * it. What the core entry points and features of that version -- the
	 * 1.1 to 1.3 promotions -- are to be asked for against. */
	uint32_t         api_version;
} FnlVulkanGpu;

/* Read from the renderer's state, so valid on the render thread alone:
 * call it from a callback -- on_gpu_created, on_record or on_overlay -- as
 * the examples do. The thread that called fnl_init() counts as that thread
 * while there is no other: before fnl_run() starts it, where the first
 * on_gpu_created runs, and once fnl_run() has joined it, where the last
 * on_gpu_lost runs. Anywhere else a loss or a move to another renderer may
 * free what it reads, so anywhere else it returns false and writes
 * nothing. False too where Vulkan does not draw this window. */
FNL_API bool fnl_vulkan_gpu(FnlApp *app, FnlVulkanGpu *out);

/* These two return Vulkan's own VkResult, not an FnlResult: they stand where
 * vkQueueSubmit and vkQueueWaitIdle would, and what they say is those calls'.
 * Do not pass it to fnl_result_str().
 *
 * Submit on the shared queue, and wait for it to go idle, under the lock the
 * library takes for its own submits and presents. The queue is one for every
 * window on the host, so with two windows open another window's render thread
 * may be submitting while a scene's own does: a submit on the raw queue then
 * races it, and these do not. Called as fnl_vulkan_gpu() is -- on_gpu_created,
 * on_record or on_overlay of the window whose queue it is -- and VK_ERROR_
 * INITIALIZATION_FAILED where the window is not drawn by Vulkan now. */
FNL_API VkResult fnl_vulkan_queue_submit(FnlApp *app, uint32_t count,
										 const VkSubmitInfo *submits,
										 VkFence fence);
FNL_API VkResult fnl_vulkan_queue_wait_idle(FnlApp *app);

/* What a scene pipeline must be built compatible with.
 *
 * Compatibility is the spec's: the three targets differ in their subpass
 * dependencies, so a pipeline built for one is not usable in another even
 * though the attachments match. Ask for each target you might see and build
 * one apiece.
 *
 * False when this window cannot use that target: FNL_TARGET_OFFSCREEN_MS on a
 * device that offers no sample count worth having, for instance. */
typedef struct {
	VkRenderPass          render_pass;
	uint32_t              subpass;
	VkSampleCountFlagBits samples;
	/* The colour attachment's format: an _SRGB one where the hardware
	 * encodes linear light as it is written, and R16G16B16A16_SFLOAT where
	 * HDR's target holds it as written. Either way a shader writes linear
	 * values with no encoding of its own; see FnlOutputColor. */
	VkFormat              color_format;
	VkFormat              depth_format;
} FnlVulkanSceneLayout;

/* On the render thread alone, as fnl_vulkan_gpu(): from on_gpu_created,
 * on_record or on_overlay. False, writing nothing, on any thread
 * fnl_vulkan_gpu() refuses. */
FNL_API bool fnl_vulkan_scene_layout(FnlApp *app, FnlSceneTarget target,
									 FnlVulkanSceneLayout *out);

/* The command buffer for this frame's scene. Call it only from inside
 * on_record, on the thread that runs it; anywhere else it returns false, and
 * the handle must not be kept past the callback.
 *
 * Vulkan keeps its recordings. Each swapchain image has a command buffer of
 * its own, recorded the first time the image is drawn after something
 * moved and submitted again, unchanged, on every frame after that until
 * something moves again. So on_record runs at most once per image for each
 * change, as each comes round, and not at all on a frame that only
 * resubmits. What a draw needs goes into the recording -- push constants, a
 * descriptor chosen there -- since memory written from the host later is
 * read by every recording still being submitted.
 *
 * On return the pass is already begun and the viewport and scissor are set to
 * the rectangle the frame is being drawn into -- the presentable area under
 * FNL_TARGET_DIRECT, and the offscreen image's own extent otherwise. The
 * overlay recorded afterwards resets both, so changing them affects only your
 * own later draws.
 *
 * Do not begin or end a render pass, and do not end the command buffer. */
FNL_API bool fnl_vulkan_scene_cmd(FnlApp *app,
								  VkCommandBuffer *out);     /* on_record */

/* The rectangle the viewport and scissor are set to for this frame's scene,
 * in the target's pixels: the presentable area under FNL_TARGET_DIRECT,
 * offset past the titlebar and the border, and the offscreen image's whole
 * extent otherwise -- fnl_d3d12_scene()'s rect. A clear of your own wants
 * it: vkCmdClearAttachments takes no viewport, and under FNL_TARGET_DIRECT
 * the attachment is the whole window. Refused, with false, wherever
 * fnl_vulkan_scene_cmd() is. */
FNL_API bool fnl_vulkan_scene_rect(FnlApp *app,
								   VkRect2D *out);           /* on_record */

#ifdef __cplusplus
}
#endif

#endif /* FENESTAL_VULKAN_H */

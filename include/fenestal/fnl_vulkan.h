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
 * the render scale, both antialiasing modes and the text overlay; the scene
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

/* Everything needed to allocate, upload and submit alongside Fenestal. The
 * queue is shared, not handed over: submissions on it are ordered against the
 * frame's own, and submitting from another thread without external
 * synchronisation is the same race it would be anywhere.
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

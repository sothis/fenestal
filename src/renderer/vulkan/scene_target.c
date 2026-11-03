/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The scene's offscreen target: the passes it is rendered and filtered in, the
 * fetch that puts it on the window, and the images behind them, which come and
 * go with the scene's resolution and with the antialiasing mode.
 *
 * The scene's own pipelines are the caller's, built against
 * fnl_vulkan_scene_layout(); what is built here is the frame's.
 */

#include "core/app.h"

#include "renderer/vulkan/vk_internal.h"

#include "fullscreen.vert.h"
#include "blit.frag.h"
#include "mlaa.frag.h"

/* ---- built once with the device ----------------------------------------- */

FnlResult create_scene_objects(App *a)
{
	/* The scene's pass twice -- single-sampled, writing the image the fetch
	 * reads, and at the device's sample count, resolving into it -- then
	 * MLAA's: no depth because a filter has no geometry, no clear because it
	 * writes every pixel. */
	const VkSampleCountFlagBits ms = scene_samples(a);
	const PassDesc scene = { .samples = VK_SAMPLE_COUNT_1_BIT,
							 .depth = true, .clear = true, .sampled = true };
	const PassDesc scene_ms = { .samples = ms,
								.depth = true, .clear = true, .sampled = true };
	const PassDesc post = { .samples = VK_SAMPLE_COUNT_1_BIT,
							.sampled = true };
	VK(a)->scene_pass = make_render_pass(a, &scene);
	if (ms != VK_SAMPLE_COUNT_1_BIT)
		VK(a)->scene_pass_ms = make_render_pass(a, &scene_ms);
	VK(a)->post_pass = make_render_pass(a, &post);
	/* Before the pipelines, one of which is built against post_pass. The
	 * multisampled pass is the one that may be absent: without it the device
	 * simply has no FSAA. */
	if (VK(a)->scene_pass == VK_NULL_HANDLE ||
		VK(a)->post_pass == VK_NULL_HANDLE)
		return fnl_fail(FNL_ERR_SYSTEM,
						"the scene's render passes did not build");

	/* LINEAR is the filter every upscaled pixel goes through; NEAREST here
	 * would give a hard pixel grid. CLAMP_TO_EDGE so the outermost half-texel
	 * does not fetch past the image, and so MLAA's search does not wrap. */
	VkSamplerCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
	};
	VK_TRY(vkCreateSampler(VK(a)->device, &sci, NULL, &VK(a)->scene_sampler));

	VkDescriptorSetLayoutBinding b = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	VkDescriptorSetLayoutCreateInfo dlci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &b,
	};
	VK_TRY(vkCreateDescriptorSetLayout(VK(a)->device, &dlci, NULL,
										 &VK(a)->blit_set_layout));

	/* Three sets for the life of the device, each pointed at its image
	 * when that image is built (see write_scene_set) and chosen between at
	 * record time, so toggling MLAA never rewrites a descriptor a command
	 * buffer in flight names. MLAA's own input is the first of them; the
	 * third is the cover's, the frame read back before a move. */
	VkDescriptorPoolSize ps = {
		.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 3 };
	VkDescriptorPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 3, .poolSizeCount = 1, .pPoolSizes = &ps,
	};
	VK_TRY(vkCreateDescriptorPool(VK(a)->device, &pci, NULL,
								  &VK(a)->blit_pool));

	VkDescriptorSetLayout set_layouts[3] = { VK(a)->blit_set_layout,
											 VK(a)->blit_set_layout,
											 VK(a)->blit_set_layout };
	VkDescriptorSet sets[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE,
								VK_NULL_HANDLE };
	VkDescriptorSetAllocateInfo dai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = VK(a)->blit_pool,
		.descriptorSetCount = 3, .pSetLayouts = set_layouts,
	};
	VK_TRY(vkAllocateDescriptorSets(VK(a)->device, &dai, sets));
	VK(a)->blit_set_scene = sets[0];
	VK(a)->blit_set_mlaa  = sets[1];
	VK(a)->blit_set_cover = sets[2];

	VkPipelineLayoutCreateInfo blci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &VK(a)->blit_set_layout };
	VK_TRY(vkCreatePipelineLayout(VK(a)->device, &blci, NULL,
								  &VK(a)->blit_layout));

	VkShaderModule vert = make_shader(a, fullscreen_vert_spv,
									  sizeof fullscreen_vert_spv);
	VkShaderModule blit = make_shader(a, blit_frag_spv, sizeof blit_frag_spv);
	VkShaderModule mlaa = make_shader(a, mlaa_frag_spv, sizeof mlaa_frag_spv);

	/* The fetch, and the filter twice: for the pass that writes its own
	 * image, and for the window's, where it runs when the scene is already
	 * the size it lands at. None of them blends. */
	VK(a)->blit_pipeline =
		make_overlay_pipeline(a, VK(a)->render_pass, vert, blit,
							  VK(a)->blit_layout, 0, NULL, 0,
							  OVERLAY_BLEND_NONE);
	VK(a)->mlaa_pipeline =
		make_overlay_pipeline(a, VK(a)->post_pass, vert, mlaa,
							  VK(a)->blit_layout, 0, NULL, 0,
							  OVERLAY_BLEND_NONE);
	VK(a)->mlaa_win_pipeline =
		make_overlay_pipeline(a, VK(a)->render_pass, vert, mlaa,
							  VK(a)->blit_layout, 0, NULL, 0,
							  OVERLAY_BLEND_NONE);
	/* The cover: the fetch's shaders once more, blended by a constant, for
	 * the frame read back before a move over the first frames after it
	 * (vk_overlay_cover()). */
	VK(a)->cover_pipeline =
		make_overlay_pipeline(a, VK(a)->render_pass, vert, blit,
							  VK(a)->blit_layout, 0, NULL, 0,
							  OVERLAY_BLEND_CONSTANT);

	vkDestroyShaderModule(VK(a)->device, vert, NULL);
	vkDestroyShaderModule(VK(a)->device, blit, NULL);
	vkDestroyShaderModule(VK(a)->device, mlaa, NULL);

	/* make_shader() and make_overlay_pipeline() report failure as
	 * VK_NULL_HANDLE, so these are tested: the frame gates on the *image*
	 * and would bind a null pipeline. */
	if (VK(a)->blit_pipeline == VK_NULL_HANDLE ||
		VK(a)->mlaa_pipeline == VK_NULL_HANDLE ||
		VK(a)->mlaa_win_pipeline == VK_NULL_HANDLE ||
		VK(a)->cover_pipeline == VK_NULL_HANDLE)
		return fnl_fail(FNL_ERR_SYSTEM,
						"the scene's fetch and filter pipelines did not build");
	return FNL_OK;
}

/* ---- the images ---------------------------------------------------------- */

/* Point one of the three sets at the image it reads. No drain of its own:
 * a set is bound only while its image exists, and an image is replaced only
 * after the queue has drained -- scene_set_size()'s drain for the scene's
 * and MLAA's, cover_upload()'s own for the cover's -- so no executing
 * command buffer names the set; recorded ones are re-recorded, since the
 * size or mode they were recorded for has changed. */
static void write_scene_set(App *a, VkDescriptorSet set, VkImageView view)
{
	VkDescriptorImageInfo ii = {
		.sampler = VK(a)->scene_sampler,
		.imageView = view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet wd = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = set, .dstBinding = 0, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &ii,
	};
	vkUpdateDescriptorSets(VK(a)->device, 1, &wd, 0, NULL);
}

static void destroy_scene_ms(App *a)
{
	vkDestroyFramebuffer(VK(a)->device, VK(a)->scene_ms_fb, NULL);
	VK(a)->scene_ms_fb = VK_NULL_HANDLE;
	destroy_target_image(a, &VK(a)->scene_ms_depth);
	destroy_target_image(a, &VK(a)->scene_ms);
}

/* The multisampled pair and the framebuffer that resolves it into the scene
 * image. Neither image is stored, so both ask for transient memory. False
 * leaves a part-built set for the caller to destroy. */
static bool create_scene_ms(App *a)
{
	const uint32_t w = (uint32_t)a->scene_extent.w;
	const uint32_t h = (uint32_t)a->scene_extent.h;
	const VkSampleCountFlagBits ms = scene_samples(a);

	if (FNL_FAULT("scene-ms") ||
		!create_target_image(a, w, h, VK(a)->surf_format.format, ms,
							 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, true,
							 &VK(a)->scene_ms) ||
		!create_target_image(a, w, h, VK(a)->depth_format, ms,
							 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, true,
							 &VK(a)->scene_ms_depth))
		return false;

	VkImageView att[3] = { VK(a)->scene_ms.view, VK(a)->scene_ms_depth.view,
						   VK(a)->scene.view };
	return make_framebuffer(a, VK(a)->scene_pass_ms, 3, att, w, h,
							&VK(a)->scene_ms_fb) == VK_SUCCESS;
}

static void destroy_scene_mlaa(App *a)
{
	vkDestroyFramebuffer(VK(a)->device, VK(a)->mlaa_fb, NULL);
	VK(a)->mlaa_fb = VK_NULL_HANDLE;
	destroy_target_image(a, &VK(a)->mlaa);
}

/* The image MLAA writes when the scene is filtered and then scaled, with its
 * framebuffer and the set the fetch reads it through. */
static bool create_scene_mlaa(App *a)
{
	const uint32_t w = (uint32_t)a->scene_extent.w;
	const uint32_t h = (uint32_t)a->scene_extent.h;

	if (FNL_FAULT("scene-mlaa") ||
		!create_target_image(a, w, h, VK(a)->surf_format.format,
							 VK_SAMPLE_COUNT_1_BIT,
							 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
							 VK_IMAGE_USAGE_SAMPLED_BIT, false, &VK(a)->mlaa))
		return false;
	if (make_framebuffer(a, VK(a)->post_pass, 1, &VK(a)->mlaa.view, w, h,
						 &VK(a)->mlaa_fb) != VK_SUCCESS)
		return false;
	write_scene_set(a, VK(a)->blit_set_mlaa, VK(a)->mlaa.view);
	return true;
}

/* Only what a mode asks for, and only once it does (DESIGN *Antialiasing* has
 * the sizes). Building needs no drain -- nothing recorded names an object that
 * did not exist -- so it runs in the frame that first wants one; destroying
 * does, so what is built stays until scene_set_size() replaces the target.
 *
 * A refusal costs the mode and nothing else, since the predicates in frame.c
 * ask the framebuffers. It is remembered, so a device with no room is asked
 * once per target rather than once per frame. */
bool scene_ensure_aa(App *a, bool want_ms, bool want_mlaa)
{
	bool built = false;

	if (VK(a)->scene.image == VK_NULL_HANDLE)
		return false;

	if (want_ms && VK(a)->scene_pass_ms != VK_NULL_HANDLE &&
		VK(a)->scene_ms_fb == VK_NULL_HANDLE && !VK(a)->scene_ms_refused) {
		if (create_scene_ms(a)) {
			built = true;
		} else {
			destroy_scene_ms(a);
			VK(a)->scene_ms_refused = true;
			fnl_log(FNL_LOG_WARN,
					"no room for a %dx%d %dx multisampled scene target; "
					"FSAA is off at this size", a->scene_extent.w,
					a->scene_extent.h, (int)scene_samples(a));
		}
	}
	if (want_mlaa && VK(a)->mlaa_fb == VK_NULL_HANDLE && !VK(a)->mlaa_refused) {
		if (create_scene_mlaa(a)) {
			built = true;
		} else {
			destroy_scene_mlaa(a);
			VK(a)->mlaa_refused = true;
			fnl_log(FNL_LOG_WARN,
					"no room for a %dx%d filtered scene image; "
					"MLAA is off at this size", a->scene_extent.w,
					a->scene_extent.h);
		}
	}
	return built;
}

/* What every offscreen frame needs whatever the mode. False leaves a
 * part-built set for the caller to destroy. */
static bool create_scene_target(App *a, uint32_t w, uint32_t h)
{
	if (FNL_FAULT("scene-target") ||
		!create_target_image(a, w, h, VK(a)->surf_format.format,
							 VK_SAMPLE_COUNT_1_BIT,
							 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
							 VK_IMAGE_USAGE_SAMPLED_BIT, false,
							 &VK(a)->scene) ||
		!create_target_image(a, w, h, VK(a)->depth_format,
							 VK_SAMPLE_COUNT_1_BIT,
							 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, true,
							 &VK(a)->scene_depth))
		return false;

	VkImageView att[2] = { VK(a)->scene.view, VK(a)->scene_depth.view };
	if (make_framebuffer(a, VK(a)->scene_pass, 2, att, w, h,
						 &VK(a)->scene_fb) != VK_SUCCESS)
		return false;

	write_scene_set(a, VK(a)->blit_set_scene, VK(a)->scene.view);
	a->scene_extent = (Extent){ (int)w, (int)h };
	return true;
}

/* Everything at the current size, built or not: destroying null does
 * nothing. The caller has idled the queue if any of it was ever drawn
 * into. */
static void destroy_scene_target(App *a)
{
	destroy_scene_ms(a);
	destroy_scene_mlaa(a);
	vkDestroyFramebuffer(VK(a)->device, VK(a)->scene_fb, NULL);
	VK(a)->scene_fb = VK_NULL_HANDLE;
	destroy_target_image(a, &VK(a)->scene_depth);
	destroy_target_image(a, &VK(a)->scene);

	/* A new target is a new question for the device. */
	VK(a)->scene_ms_refused = false;
	VK(a)->mlaa_refused     = false;
	a->scene_extent     = (Extent){ 0, 0 };
}

void scene_set_size(App *a, int w, int h)
{
	if (!VK(a)->scene_pass)
		return;
	if ((w > 0 ? w : 0) == a->scene_extent.w &&
		(h > 0 ? h : 0) == a->scene_extent.h)
		return;

	/* As in text_set_scale(): command buffers that may still be executing name
	 * the images and their descriptors, and this runs from the rebuild path,
	 * which recovers from a lost device rather than failing on one. */
	if (VK(a)->scene.image) {
		VkResult r = vkQueueWaitIdle(VK(a)->queue);
		if (vk_lost(a, r))
			return;
		/* Not VK_TRY: this returns nothing, so a drain that failed for another
		 * reason is recorded and abandoned. Leaving the old target in place is
		 * the safe half -- an image a command buffer still names must not be
		 * destroyed. */
		if (VKR(r) < 0)
			return;
		destroy_scene_target(a);
	}
	/* A refusal leaves no target and says so; the loop reads scene_extent,
	 * still zero, and draws the scene straight into the window. The request
	 * stands. Not rate-limited: the loop calls this again only once the size
	 * or the window changed (see draw_scene_w in core/frame.c). */
	if (w > 0 && h > 0 && !create_scene_target(a, (uint32_t)w, (uint32_t)h)) {
		destroy_scene_target(a);
		fnl_log(FNL_LOG_WARN,
				"no room for a %dx%d scene target; drawing the scene at the "
				"window's own resolution", w, h);
	}
}

/* ---- the cover ----------------------------------------------------------- */

/* App::snap into an image of this device: BGRA, the snapshot's order
 * (app_snapshot_set()), as the image's format, and _SRGB or UNORM as the
 * surface is, so the bytes round-trip through the sampler and the
 * attachment unchanged. One staging copy, waited for, as the atlas's is;
 * an image before it, from an earlier move on this device, drained first,
 * since a frame in flight may still sample it. */
static bool cover_upload(App *a)
{
	if (VK(a)->cover.image != VK_NULL_HANDLE) {
		const VkResult wr = vkQueueWaitIdle(VK(a)->queue);
		if (vk_lost(a, wr) || wr != VK_SUCCESS)
			return false;
		destroy_target_image(a, &VK(a)->cover);
	}
	const VkFormat fmt = a->srgb_surface ? VK_FORMAT_B8G8R8A8_SRGB
										 : VK_FORMAT_B8G8R8A8_UNORM;
	const uint32_t w = (uint32_t)a->snap_w, h = (uint32_t)a->snap_h;
	if (!create_target_image(a, w, h, fmt, VK_SAMPLE_COUNT_1_BIT,
							 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
								 VK_IMAGE_USAGE_SAMPLED_BIT,
							 false, &VK(a)->cover))
		return false;

	VkBuffer       stage[1];
	VkDeviceMemory smem;
	void          *smap[1];
	const VkDeviceSize bytes = (VkDeviceSize)w * 4u * (VkDeviceSize)h;
	if (create_instance_buffers(a, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
								0, 1, stage, &smem, smap) < 0) {
		destroy_target_image(a, &VK(a)->cover);
		return false;
	}
	memcpy(smap[0], a->snap_px, (size_t)bytes);

	bool ok = false;
	VkCommandBuffer cb = begin_oneshot(a);
	if (cb != VK_NULL_HANDLE) {
		VkImageMemoryBarrier to_dst = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = VK(a)->cover.image,
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		};
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
							 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
							 NULL, 1, &to_dst);
		const VkBufferImageCopy copy = {
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { w, h, 1 },
		};
		vkCmdCopyBufferToImage(cb, stage[0], VK(a)->cover.image,
							   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
							   &copy);
		VkImageMemoryBarrier to_read = to_dst;
		to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
							 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
							 NULL, 0, NULL, 1, &to_read);
		ok = end_oneshot(a, cb) >= 0;
	}
	destroy_instance_buffers(a, 1, stage, &smem, smap);
	if (!ok) {
		destroy_target_image(a, &VK(a)->cover);
		return false;
	}
	write_scene_set(a, VK(a)->blit_set_cover, VK(a)->cover.view);
	return true;
}

/* The cover over the window at `alpha`, inside the overlay's recording:
 * uploaded once per snapshot (App::snap_gen), and tried once -- a device
 * that will not take it draws black for the rest of this transition. The
 * fetch's layout has no push range, so what the overlay pushed is undefined
 * from this bind on. */
bool vk_overlay_cover(App *a, float alpha)
{
	VkCommandBuffer cb = VK(a)->overlay_cb;
	if (cb == VK_NULL_HANDLE || !a->snap_valid ||
		VK(a)->cover_pipeline == VK_NULL_HANDLE)
		return false;
	if (VK(a)->cover_gen != a->snap_gen) {
		VK(a)->cover_gen = a->snap_gen;
		VK(a)->cover_live = cover_upload(a);
		if (!VK(a)->cover_live)
			fnl_log(FNL_LOG_WARN, "the frame read back before the move "
					"would not upload; the move dissolves from black");
	}
	if (!VK(a)->cover_live || VK(a)->cover.image == VK_NULL_HANDLE)
		return false;
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
					  VK(a)->cover_pipeline);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
							VK(a)->blit_layout, 0, 1, &VK(a)->blit_set_cover,
							0, NULL);
	const float k[4] = { 0.0f, 0.0f, 0.0f, alpha };
	vkCmdSetBlendConstants(cb, k);
	vkCmdDraw(cb, 3, 1, 0, 0);
	VK(a)->overlay_pushed = false;
	return true;
}

/* The seam's cover_release: after a drain, as cover_upload() replaces the
 * cover. A drain refused (a lost device) keeps both, for the teardown that
 * follows. The cover's set is left naming the view let go: no recording
 * that binds it is submitted again, each having been made for a veil the
 * draw state no longer has (App::record_gen), and cover_upload() writes
 * it before the next. */
void vk_cover_release(App *a)
{
	if (VK(a)->cover.image == VK_NULL_HANDLE && VK(a)->snap_bytes == 0)
		return;
	const VkResult wr = vkQueueWaitIdle(VK(a)->queue);
	if (vk_lost(a, wr) || wr != VK_SUCCESS)
		return;
	destroy_target_image(a, &VK(a)->cover);
	VK(a)->cover_live = false;
	destroy_instance_buffers(a, 1, VK(a)->snap_buf, &VK(a)->snap_mem,
							 VK(a)->snap_map);
	VK(a)->snap_bytes = 0;
}

void destroy_scene_objects(App *a)
{
	destroy_scene_target(a);
	destroy_target_image(a, &VK(a)->cover);
	vkDestroyPipeline(VK(a)->device, VK(a)->cover_pipeline, NULL);
	VK(a)->cover_pipeline = VK_NULL_HANDLE;
	VK(a)->blit_set_cover = VK_NULL_HANDLE;   /* freed with the pool */
	VK(a)->cover_gen = 0;
	VK(a)->cover_live = false;
	vkDestroyPipeline(VK(a)->device, VK(a)->mlaa_win_pipeline, NULL);
	vkDestroyPipeline(VK(a)->device, VK(a)->mlaa_pipeline, NULL);
	vkDestroyPipeline(VK(a)->device, VK(a)->blit_pipeline, NULL);
	vkDestroyPipelineLayout(VK(a)->device, VK(a)->blit_layout, NULL);
	vkDestroyDescriptorPool(VK(a)->device, VK(a)->blit_pool, NULL);
	vkDestroyDescriptorSetLayout(VK(a)->device, VK(a)->blit_set_layout, NULL);
	vkDestroySampler(VK(a)->device, VK(a)->scene_sampler, NULL);
	vkDestroyRenderPass(VK(a)->device, VK(a)->post_pass, NULL);
	vkDestroyRenderPass(VK(a)->device, VK(a)->scene_pass_ms, NULL);
	vkDestroyRenderPass(VK(a)->device, VK(a)->scene_pass, NULL);
	VK(a)->mlaa_win_pipeline = VK_NULL_HANDLE;
	VK(a)->mlaa_pipeline = VK_NULL_HANDLE;
	VK(a)->blit_pipeline = VK_NULL_HANDLE;
	VK(a)->blit_layout = VK_NULL_HANDLE;
	VK(a)->blit_pool = VK_NULL_HANDLE;
	VK(a)->blit_set_scene = VK_NULL_HANDLE;   /* freed with the pool */
	VK(a)->blit_set_mlaa = VK_NULL_HANDLE;
	VK(a)->blit_set_layout = VK_NULL_HANDLE;
	VK(a)->scene_sampler = VK_NULL_HANDLE;
	VK(a)->post_pass = VK_NULL_HANDLE;
	VK(a)->scene_pass_ms = VK_NULL_HANDLE;
	VK(a)->scene_pass = VK_NULL_HANDLE;
}

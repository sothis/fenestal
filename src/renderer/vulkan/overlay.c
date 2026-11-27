/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The two overlay primitives that are not text: solid rectangles and
 * antialiased strokes.
 *
 * One file because they are one pipeline with different shaders: four
 * procedural vertices, one instance per primitive, no descriptor set, one push
 * constant. That is what lets them share a layout and one instance buffer per
 * swapchain image.
 *
 * Rectangles do not blend. They stand in for vkCmdClearAttachments, which
 * writes rather than blends, so blending them would composite each against the
 * load-op clear instead of replacing it.
 *
 * Strokes exist because a rectangle is axis-aligned and hard-edged, and the
 * close button's "X" is a diagonal: shaders/shape.frag turns exact distance to
 * the segment into coverage, smooth at any angle and any surface scale.
 */

#include "core/app.h"

#include "renderer/vulkan/vk_internal.h"

#include "shape.vert.h"
#include "shape.frag.h"
#include "rect.vert.h"
#include "rect.frag.h"
#include "image.vert.h"
#include "image.frag.h"

/* How many caller images may have a descriptor set live at once: the pool's
 * maxSets, and so the images that may be drawn at once rather than the images
 * ever made -- vk_image_free() returns the set to the pool. A pool at its
 * limit is one image that does not appear, said once. */
#define IMAGE_SETS_MAX 64

/* The arena layout -- OVERLAY_SHAPE_OFF and the two byte counts -- is in
 * app.h, because src/core/overlay.c appends against the same offsets. */

/* ---- pipelines ---------------------------------------------------------- */

FnlResult create_overlay_pipelines(App *a)
{
	/* Neither pipeline reads a descriptor and both take the same block, in the
	 * vertex stage only, so one layout serves them. */
	VkPushConstantRange pcr        = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
									   .offset     = 0,
									   .size       = sizeof(OverlayPush) };
	VkPipelineLayoutCreateInfo lci = {
		.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges    = &pcr
	};
	VK_TRY(vkCreatePipelineLayout(VK(a)->device, &lci, NULL,
								  &VK(a)->overlay_layout));

	VkShaderModule rv = make_shader(a, rect_vert_spv, sizeof rect_vert_spv);
	VkShaderModule rf = make_shader(a, rect_frag_spv, sizeof rect_frag_spv);
	const VkVertexInputAttributeDescription rect_va[2] = {
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RectInstance, rect) },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RectInstance, color) },
	};
	VK(a)->rect_pipeline = make_overlay_pipeline(
		a, VK(a)->render_pass, rv, rf, VK(a)->overlay_layout,
		sizeof(RectInstance), rect_va, 2, OVERLAY_BLEND_NONE);
	vkDestroyShaderModule(VK(a)->device, rv, NULL);
	vkDestroyShaderModule(VK(a)->device, rf, NULL);

	VkShaderModule sv = make_shader(a, shape_vert_spv, sizeof shape_vert_spv);
	VkShaderModule sf = make_shader(a, shape_frag_spv, sizeof shape_frag_spv);
	/* Packed as vec4s: fewer attributes is fewer fetches, at no extra cost. */
	const VkVertexInputAttributeDescription shape_va[5] = {
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ShapeInstance, p0) },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ShapeInstance, p2) },
		{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ShapeInstance, color) },
		{ 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
		  offsetof(ShapeInstance, color2) },
		{ 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
		  offsetof(ShapeInstance, shape_kind) },
	};
	VK(a)->shape_pipeline = make_overlay_pipeline(
		a, VK(a)->render_pass, sv, sf, VK(a)->overlay_layout,
		sizeof(ShapeInstance), shape_va, 5, OVERLAY_BLEND_ALPHA);
	vkDestroyShaderModule(VK(a)->device, sv, NULL);
	vkDestroyShaderModule(VK(a)->device, sf, NULL);

	/* make_shader() and make_overlay_pipeline() report failure as
	 * VK_NULL_HANDLE, so a pipeline that did not build is a device that did
	 * not build -- not a first frame that binds nothing. */
	if (VK(a)->rect_pipeline == VK_NULL_HANDLE ||
		VK(a)->shape_pipeline == VK_NULL_HANDLE)
		return fnl_fail(FNL_ERR_SYSTEM,
						"the overlay's rectangle and stroke pipelines did not "
						"build");

	/* The image: one sampler and one rectangle, so its own layout -- its push
	 * block carries the destination rectangle, where the instanced pipelines'
	 * carries the extent alone. One sampler for every image, and a set a
	 * image (vk_image_free()). */
	VkDescriptorSetLayoutBinding ib = {
		.binding         = 0,
		.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	VkDescriptorSetLayoutCreateInfo islci = {
		.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings    = &ib
	};
	VK_TRY(vkCreateDescriptorSetLayout(VK(a)->device, &islci, NULL,
									   &VK(a)->image_set_layout));

	VkPushConstantRange ipcr = { .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
								 .offset     = 0,
								 .size       = sizeof(ImagePush) };
	VkPipelineLayoutCreateInfo ilci = {
		.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount         = 1,
		.pSetLayouts            = &VK(a)->image_set_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges    = &ipcr
	};
	VK_TRY(vkCreatePipelineLayout(VK(a)->device, &ilci, NULL,
								  &VK(a)->image_layout));

	/* LINEAR and CLAMP_TO_EDGE, as the scene's fetch: an image scaled into a
	 * rectangle larger than it is filtered, and one smaller does not wrap. */
	VkSamplerCreateInfo isci = {
		.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter    = VK_FILTER_LINEAR,
		.minFilter    = VK_FILTER_LINEAR,
		.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
	};
	VK_TRY(vkCreateSampler(VK(a)->device, &isci, NULL, &VK(a)->image_sampler));

	/* Freeable, because an image may be destroyed while others live. */
	VkDescriptorPoolSize ips        = { .type =
											VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
										.descriptorCount = IMAGE_SETS_MAX };
	VkDescriptorPoolCreateInfo ipci = {
		.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
		.maxSets       = IMAGE_SETS_MAX,
		.poolSizeCount = 1,
		.pPoolSizes    = &ips
	};
	VK_TRY(
		vkCreateDescriptorPool(VK(a)->device, &ipci, NULL, &VK(a)->image_pool));

	VkShaderModule iv  = make_shader(a, image_vert_spv, sizeof image_vert_spv);
	VkShaderModule ifr = make_shader(a, image_frag_spv, sizeof image_frag_spv);
	VK(a)->image_pipeline = make_overlay_pipeline(a, VK(a)->render_pass, iv,
												  ifr, VK(a)->image_layout, 0,
												  NULL, 0, OVERLAY_BLEND_ALPHA);
	vkDestroyShaderModule(VK(a)->device, iv, NULL);
	vkDestroyShaderModule(VK(a)->device, ifr, NULL);
	if (VK(a)->image_pipeline == VK_NULL_HANDLE)
		return fnl_fail(FNL_ERR_SYSTEM,
						"the overlay's image pipeline did not build");

	FNL_TRY(create_instance_buffers(a, OVERLAY_ARENA_BYTES,
									VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0,
									OVERLAY_MAX_IMAGES, VK(a)->ovl_vb,
									&VK(a)->ovl_vb_mem, (void **)a->ovl_map));
	return FNL_OK;
}

void destroy_overlay_pipelines(App *a)
{
	destroy_instance_buffers(a, OVERLAY_MAX_IMAGES, VK(a)->ovl_vb,
							 &VK(a)->ovl_vb_mem, (void **)a->ovl_map);

	vkDestroyPipeline(VK(a)->device, VK(a)->rect_pipeline, NULL);
	vkDestroyPipeline(VK(a)->device, VK(a)->shape_pipeline, NULL);
	vkDestroyPipelineLayout(VK(a)->device, VK(a)->overlay_layout, NULL);

	/* The images' own objects are theirs (vk_image_free()), and were let go
	 * before this ran (device.c's vk_images_device_gone()); the pool takes
	 * their sets with it. */
	vkDestroyPipeline(VK(a)->device, VK(a)->image_pipeline, NULL);
	vkDestroyPipelineLayout(VK(a)->device, VK(a)->image_layout, NULL);
	vkDestroyDescriptorPool(VK(a)->device, VK(a)->image_pool, NULL);
	vkDestroyDescriptorSetLayout(VK(a)->device, VK(a)->image_set_layout, NULL);
	vkDestroySampler(VK(a)->device, VK(a)->image_sampler, NULL);

	/* Every handle is cleared, which is what makes a second call safe. */
	VK(a)->rect_pipeline    = VK_NULL_HANDLE;
	VK(a)->shape_pipeline   = VK_NULL_HANDLE;
	VK(a)->overlay_layout   = VK_NULL_HANDLE;
	VK(a)->image_pipeline   = VK_NULL_HANDLE;
	VK(a)->image_layout     = VK_NULL_HANDLE;
	VK(a)->image_pool       = VK_NULL_HANDLE;
	VK(a)->image_set_layout = VK_NULL_HANDLE;
	VK(a)->image_sampler    = VK_NULL_HANDLE;
}

/* ---- recording ---------------------------------------------------------- */

/* Draw the instances appended since this kind was last flushed. Instance-rate
 * attributes are fetched from firstInstance, so a second flush reads on from
 * where the first stopped -- nothing is copied or rebound. */
static void flush(App *a, VkPipeline pipe, VkDeviceSize offset,
				  const uint32_t *count, uint32_t *drawn)
{
	VkCommandBuffer cb = VK(a)->overlay_cb;
	if (*count == *drawn || cb == VK_NULL_HANDLE || pipe == VK_NULL_HANDLE)
		return;

	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindVertexBuffers(cb, 0, 1, &VK(a)->ovl_vb[a->ovl_img], &offset);

	/* Both pipelines here read it through one layout and the extent cannot
	 * change inside a recording, so one push serves every flush after it. */
	if (!VK(a)->overlay_pushed) {
		OverlayPush push = { .screen = { (float)a->extent.w,
										 (float)a->extent.h } };
		vkCmdPushConstants(cb, VK(a)->overlay_layout,
						   VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, &push);
		VK(a)->overlay_pushed = true;
	}

	vkCmdDraw(cb, 4, *count - *drawn, 0, *drawn);
	*drawn = *count;
}

void vk_overlay_flush_rects(App *a)
{
	flush(a, VK(a)->rect_pipeline, 0, &a->rect_count, &a->rect_drawn);
}

/* NULL restores the whole framebuffer, which is what everything but a reticle
 * is drawn under. */
void vk_overlay_scissor(App *a, const FnlRect *r)
{
	if (VK(a)->overlay_cb == VK_NULL_HANDLE)
		return;
	VkRect2D sc = { { 0, 0 },
					{ (uint32_t)a->extent.w, (uint32_t)a->extent.h } };
	if (r) {
		sc.offset.x      = r->x;
		sc.offset.y      = r->y;
		sc.extent.width  = (uint32_t)r->w;
		sc.extent.height = (uint32_t)r->h;
	}
	vkCmdSetScissor(VK(a)->overlay_cb, 0, 1, &sc);
}

void vk_overlay_flush_shapes(App *a)
{
	flush(a, VK(a)->shape_pipeline, OVERLAY_SHAPE_OFF, &a->shape_count,
		  &a->shape_drawn);
}

/* ---- images ---------------------------------------------------------------
 *
 * A caller's picture (fnl_image_create()): the library's copy of its pixels
 * (FnlImage::pixels) and, here, the device object made from them. The upload
 * is lazy -- the first draw after the image is made, and the first after a
 * device rebuild -- and runs on the render thread, inside the overlay's own
 * recording, which is what lets it take no lock (fenestal.h says so where the
 * calls are declared). The copy is what a rebuild uploads again from. */

/* The device object behind FnlImage::gpu: what the draw binds, freed by
 * vk_image_free(). `retire_next` threads the ones destroyed while a recording
 * is open -- the command buffer being recorded still names them -- until the
 * frame that recorded it is submitted (vk_images_retire()). */
typedef struct VkImageObj {
	VkImage image;
	VkDeviceMemory mem;
	VkImageView view;
	VkDescriptorSet set;
	struct VkImageObj *retire_next;
	int retire_left; /* frames still to be submitted before it is let go */
} VkImageObj;

/* A destroyed image is let go once this many frames have been submitted: the
 * frame being recorded may name it, and every submit waits the fence of the
 * one MAX_FRAMES_IN_FLIGHT before it, so by then no list that named it is
 * still on the queue. One more than the swapchain's count, since the frame
 * that was open when it was destroyed has not been submitted yet. */
#define IMAGE_RETIRE_FRAMES (MAX_FRAMES_IN_FLIGHT + 1)
/* More than this waiting is an app that churns images, and the next destroy
 * outside a recording drains instead of letting the pool's sets and the
 * device memory pile up. */
#define IMAGE_RETIRE_MAX 8

bool vk_image_supported(const App *a)
{
	return a->renderer_state != NULL && VK(a)->image_pipeline != VK_NULL_HANDLE;
}

static void upload_free(App *a, struct VkUpload *u)
{
	vkFreeCommandBuffers(VK(a)->device, VK(a)->cmd_pool, 1, &u->cb);
	destroy_instance_buffers(a, 1, &u->stage, &u->mem, &u->map);
}

/* Take over a submitted upload's command buffer and staging pair. A full
 * table waits the queue out first, which is the old cost, once a burst. */
static void upload_keep(App *a, VkCommandBuffer cb, VkBuffer stage,
						VkDeviceMemory mem, void *map)
{
	if (VK(a)->uploads_n == IMAGE_UPLOADS_MAX)
		vk_uploads_drain(a);
	VK(a)->uploads[VK(a)->uploads_n++] = (struct VkUpload){
		.cb = cb, .stage = stage, .mem = mem, .map = map,
		.left = IMAGE_RETIRE_FRAMES,
	};
}

/* Every upload's buffers freed, the queue waited out first: with the device
 * about to be destroyed, or the table full. A lost device answers at once. */
void vk_uploads_drain(App *a)
{
	if (VK(a)->uploads_n == 0)
		return;
	(void)vk_lost(a, vk_queue_wait_idle(a));
	for (int i = 0; i < VK(a)->uploads_n; i++)
		upload_free(a, &VK(a)->uploads[i]);
	VK(a)->uploads_n = 0;
}

/* One image's device object, from the copy the library kept. False where
 * anything failed, with *out left null: the draw is skipped and the caller's
 * picture does not appear, which is what fnl_image_supported() promised. */
static bool image_upload(App *a, FnlImage *img, VkImageObj **out)
{
	*out          = NULL;
	VkImageObj *o = calloc(1, sizeof *o);
	if (!o)
		return false;

	/* _SRGB where the window is, as the cover: the sampler decodes each texel
	 * to linear before the blend and the framebuffer re-encodes on write. */
	const VkFormat fmt =
		a->linear_target ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
	const uint32_t w = (uint32_t)img->w, h = (uint32_t)img->h;
	const VkDeviceSize bytes = (VkDeviceSize)w * (VkDeviceSize)h * 4u;

	VkImageCreateInfo ici = {
		.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType   = VK_IMAGE_TYPE_2D,
		.format      = fmt,
		.extent      = { w, h, 1 },
		.mipLevels   = 1,
		.arrayLayers = 1,
		.samples     = VK_SAMPLE_COUNT_1_BIT,
		.tiling      = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (VKR(vkCreateImage(VK(a)->device, &ici, NULL, &o->image)) < 0)
		goto fail;

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(VK(a)->device, o->image, &req);
	uint32_t type;
	if (find_memory_type(a, req.memoryTypeBits,
						 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) < 0)
		goto fail;
	VkMemoryAllocateInfo mai = {
		.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize  = req.size,
		.memoryTypeIndex = type,
	};
	if (VKR(vkAllocateMemory(VK(a)->device, &mai, NULL, &o->mem)) < 0)
		goto fail;
	if (VKR(vkBindImageMemory(VK(a)->device, o->image, o->mem, 0)) < 0)
		goto fail;

	/* The staging pair through the same helper the arenas use, freed before
	 * this returns: the upload is once an image, not once a frame. */
	VkBuffer stage      = VK_NULL_HANDLE;
	VkDeviceMemory smem = VK_NULL_HANDLE;
	void *smap          = NULL;
	if (create_instance_buffers(a, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0,
								1, &stage, &smem, &smap) < 0)
		goto fail;
	memcpy(smap, img->pixels, (size_t)bytes);

	VkCommandBuffer cb = begin_oneshot(a);
	if (cb == VK_NULL_HANDLE) {
		destroy_instance_buffers(a, 1, &stage, &smem, &smap);
		goto fail;
	}
	VkImageMemoryBarrier to_dst = {
		.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image               = o->image,
		.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		.srcAccessMask       = 0,
		.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
						 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
						 &to_dst);
	VkBufferImageCopy copy = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent      = { w, h, 1 },
	};
	vkCmdCopyBufferToImage(cb, stage, o->image,
						   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	VkImageMemoryBarrier to_read = to_dst;
	to_read.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_read.newLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	to_read.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_read.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
						 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
						 NULL, 1, &to_read);
	/* Submitted and not waited on: the frame that draws the image is
	 * submitted after this on the same queue, and the barrier above makes the
	 * copy visible to everything behind it, so nothing here needs the GPU to
	 * be done. The command buffer and the staging pair are kept until the
	 * queue is past them (upload_keep()). A wait here was a drain of the
	 * whole queue, with the frame being recorded behind it, once an image. */
	const FnlResult rc = end_oneshot_nowait(a, cb);
	if (rc < 0) {
		vkFreeCommandBuffers(VK(a)->device, VK(a)->cmd_pool, 1, &cb);
		destroy_instance_buffers(a, 1, &stage, &smem, &smap);
		goto fail;
	}
	upload_keep(a, cb, stage, smem, smap);

	if (make_image_view(a, o->image, fmt, VK_IMAGE_ASPECT_COLOR_BIT,
						&o->view) != VK_SUCCESS)
		goto fail;

	/* The set the draw reads it through, from the pool the images share. A
	 * pool at its limit is a picture that does not appear; freeing a set
	 * (vk_image_free()) is what recycles it. */
	VkDescriptorSetAllocateInfo dai = {
		.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool     = VK(a)->image_pool,
		.descriptorSetCount = 1,
		.pSetLayouts        = &VK(a)->image_set_layout,
	};
	if (VKR(vkAllocateDescriptorSets(VK(a)->device, &dai, &o->set)) < 0)
		goto fail;
	VkDescriptorImageInfo ii = {
		.sampler     = VK(a)->image_sampler,
		.imageView   = o->view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet wr = {
		.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet          = o->set,
		.dstBinding      = 0,
		.descriptorCount = 1,
		.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo      = &ii,
	};
	vkUpdateDescriptorSets(VK(a)->device, 1, &wr, 0, NULL);

	*out = o;
	return true;

fail:
	/* The handles a failed call wrote are undefined, and every one was nulled
	 * by calloc(): destroying them is safe on any prefix. */
	vkDestroyImageView(VK(a)->device, o->view, NULL);
	vkDestroyImage(VK(a)->device, o->image, NULL);
	vkFreeMemory(VK(a)->device, o->mem, NULL);
	free(o);
	return false;
}

void vk_overlay_image(App *a, FnlImage *img, int x, int y, int w, int h)
{
	VkCommandBuffer cb = VK(a)->overlay_cb;
	if (cb == VK_NULL_HANDLE || VK(a)->image_pipeline == VK_NULL_HANDLE)
		return;

	if (!img->gpu) {
		VkImageObj *o = NULL;
		if (!image_upload(a, img, &o)) {
			fnl_log(FNL_LOG_WARN,
					"the %dx%d image would not upload; it is "
					"not drawn",
					img->w, img->h);
			return;
		}
		img->gpu = o;
	}

	/* What was appended before it is behind it: the core flushed the
	 * rectangles and strokes before this call, so the image lands between
	 * them in the command buffer as it did in call order. */
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
					  VK(a)->image_pipeline);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
							VK(a)->image_layout, 0, 1,
							&((VkImageObj *)img->gpu)->set, 0, NULL);
	const ImagePush push = {
		.rect   = { (float)x, (float)y, (float)w, (float)h },
		.screen = { (float)a->extent.w, (float)a->extent.h },
	};
	vkCmdPushConstants(cb, VK(a)->image_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
					   sizeof push, &push);
	vkCmdDraw(cb, 6, 1, 0, 0);
	/* image_layout's range is not overlay_layout's, so what flush() pushed is
	 * undefined from here: the next rect or stroke would read this image's
	 * rectangle as the framebuffer's extent. text.c and scene_target.c clear
	 * it for the same reason. */
	VK(a)->overlay_pushed = false;
}

static void image_obj_destroy(App *a, VkImageObj *o)
{
	vkDestroyImageView(VK(a)->device, o->view, NULL);
	vkDestroyImage(VK(a)->device, o->image, NULL);
	vkFreeMemory(VK(a)->device, o->mem, NULL);
	if (o->set)
		vkFreeDescriptorSets(VK(a)->device, VK(a)->image_pool, 1, &o->set);
	free(o);
}

/* Let go the images destroyed while a recording was open, and the ones
 * destroyed since, with a wait: their draws are in command buffers that may
 * still be on the queue. For a device about to go, and for a churn past
 * IMAGE_RETIRE_MAX; the steady path is vk_images_tick(). One wait covers the
 * lot. A lost device answers at once and is recorded, and every destroy is
 * legal there. */
void vk_images_retire(App *a)
{
	VkImageObj *o = VK(a)->img_retire;
	if (!o)
		return;
	VK(a)->img_retire   = NULL;
	VK(a)->img_retire_n = 0;
	(void)vk_lost(a, vk_device_wait_idle(a));
	while (o) {
		VkImageObj *next = o->retire_next;
		image_obj_destroy(a, o);
		o = next;
	}
}

/* After a frame is submitted: each destroyed image and each upload is one
 * frame closer to being let go, with no wait. Every submit waits the fence
 * of the frame MAX_FRAMES_IN_FLIGHT before it, which the queue order makes
 * the end of everything submitted earlier -- the argument vk_retire_tick()
 * makes for a swapchain. */
void vk_images_tick(App *a)
{
	VkImageObj **pp = (VkImageObj **)&VK(a)->img_retire;
	while (*pp) {
		VkImageObj *o = *pp;
		if (--o->retire_left > 0) {
			pp = &o->retire_next;
			continue;
		}
		*pp = o->retire_next;
		VK(a)->img_retire_n--;
		image_obj_destroy(a, o);
	}
	struct VkUpload *u = VK(a)->uploads;
	int kept           = 0;
	for (int i = 0; i < VK(a)->uploads_n; i++) {
		if (--u[i].left > 0)
			u[kept++] = u[i];
		else
			upload_free(a, &u[i]);
	}
	VK(a)->uploads_n = kept;
}

void vk_image_free(App *a, FnlImage *img)
{
	VkImageObj *o = img->gpu;
	if (!o)
		return;
	img->gpu = NULL;
	/* The command buffer being recorded, or the other frame slot's list
	 * still on the queue, may name the view and the set, which the slot
	 * gives back for a later picture to write. A wait here was the caller's
	 * toggle cost, and a drain of the frame behind it; the object is let go
	 * on the frame countdown instead (vk_images_tick()). Outside a recording
	 * a churn past IMAGE_RETIRE_MAX is drained as it was: nothing open
	 * names the others, and the pool has sets to give back. Inside one the
	 * list cannot be drained, since this frame's draws of them are not
	 * submitted yet. */
	if (!a->overlay_open && VK(a)->img_retire_n >= IMAGE_RETIRE_MAX) {
		vk_images_retire(a);
		(void)vk_lost(a, vk_device_wait_idle(a));
		image_obj_destroy(a, o);
		return;
	}
	o->retire_left    = IMAGE_RETIRE_FRAMES;
	o->retire_next    = VK(a)->img_retire;
	VK(a)->img_retire = o;
	VK(a)->img_retire_n++;
}

/* Every image's device object let go, before the device it names is destroyed
 * (device.c's destroy_device_objects()), leaving each FnlImage with a null
 * gpu -- which is what makes the next device upload its copy again. Destroying
 * on a lost device is legal, and is what the driver's own bookkeeping for the
 * image, its memory and its view needs: vkDestroyDevice() is not a substitute,
 * and leaving them to it leaks those allocations (found by the ASan round).
 * The descriptor set is not freed: the pool it came from is destroyed right
 * after (destroy_overlay_pipelines()), which takes every set with it. */
void vk_images_device_gone(App *a)
{
	for (FnlImage *img = a->images; img; img = img->next) {
		VkImageObj *o = img->gpu;
		if (!o)
			continue;
		img->gpu = NULL;
		vkDestroyImageView(VK(a)->device, o->view, NULL);
		vkDestroyImage(VK(a)->device, o->image, NULL);
		vkFreeMemory(VK(a)->device, o->mem, NULL);
		free(o);
	}
}

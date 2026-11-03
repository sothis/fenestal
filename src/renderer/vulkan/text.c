/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The overlay's text: Ubuntu Sans Mono and Ubuntu Sans, baked at build time
 * into coverage atlases, one per surface scale, and drawn as a single
 * instanced call. Nothing is rasterised here: this file picks the atlas for
 * the scale in force, decodes it (tools/font_codec.h), uploads it and draws.
 * DESIGN *Text* has the reasoning.
 *
 * Each texel is one byte of raw coverage; shaders/text.frag turns it into the
 * blend weight. Sampling is NEAREST and the quads land on whole pixels.
 *
 * There are *three texels to the pixel* across: mkfont rasterises for the three
 * emitters of an LCD pixel, so the atlas is at triple horizontal resolution and
 * a cell's texel width is three times the width its quad is drawn at. Cells
 * carry x in texels and w in pixels for that reason. Which unit a field is in
 * is the easy thing to get wrong here; GlyphCell in app.h names it per field.
 *
 * Coordinates are in logical points.
 */

#include "core/app.h"

#include "renderer/vulkan/vk_internal.h"

#include "text.vert.h"
#include "text.frag.h"
#include "text.frag.lcd.h"
#include "font_data.h"
#include "font_codec.h"

/* The metrics -- text_advance(), text_height() -- are derived at the bake and
 * inlined from font_metrics.h by app.h. Placement is whole-pixel integer
 * arithmetic. In a monospaced face the advance is a *measurement* of the
 * glyph, never a step chosen for it to sit inside. */

static void destroy_atlas_image(App *a)
{
	vkDestroyImageView(VK(a)->device, VK(a)->font_view, NULL);
	vkDestroyImage(VK(a)->device, VK(a)->font_image, NULL);
	vkFreeMemory(VK(a)->device, VK(a)->font_mem, NULL);
	VK(a)->font_view  = VK_NULL_HANDLE;
	VK(a)->font_image = VK_NULL_HANDLE;
	VK(a)->font_mem   = VK_NULL_HANDLE;
	/* And the index text_draw() tests before appending a glyph, so a scale
	 * change whose upload fails appends nothing against a destroyed view. */
	a->font_si = -1;
}

/* The memory type the atlas goes in: device-local, except on a device whose
 * VRAM does not keep a tiled image across its runtime suspend, where it is
 * the first type the image allows outside VRAM -- system memory, which
 * nothing powers off. The atlas is uploaded once and only read, so reading
 * it over the bus costs nothing measured; uploading it again after a
 * resume would need the resume told, and nothing in Vulkan tells it. A
 * device that allows the image no such type keeps it in VRAM, with one
 * line. DESIGN *The startup inventory*. */
static FnlResult atlas_memory_type(App *a, uint32_t bits, uint32_t *out)
{
	if (VK(a)->phys_vram_unkept || FNL_FAULT("atlas-out-of-vram")) {
		const VkPhysicalDeviceMemoryProperties *mp = &VK(a)->mem_props;
		for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
			if ((bits & (1u << i)) &&
				!(mp->memoryTypes[i].propertyFlags &
				  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
				fnl_log(FNL_LOG_INFO, "the glyph atlas is in system memory: "
						"this device's VRAM is not kept across its runtime "
						"suspend");
				*out = i;
				return FNL_OK;
			}
		fnl_log(FNL_LOG_WARN, "the glyph atlas is in VRAM, the only memory "
				"this device allows it, which its runtime suspend does not "
				"keep: text may garble after the GPU has been idle");
	}
	return find_memory_type(a, bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out);
}

/* Upload the atlas baked for scale index `si`. The images differ in height
 * from one scale to the next, so a scale change replaces this one. */
static FnlResult build_atlas(App *a, int si)
{
	const uint32_t aw = FONT_ATLAS_W[si];
	const uint32_t ah = FONT_ATLAS_H[si];
	const VkDeviceSize bytes = (VkDeviceSize)aw * (VkDeviceSize)ah;
	const unsigned char *src = font_atlas_blob + FONT_ATLAS_OFF[si];

	/* Cleared before anything can fail: Vulkan leaves an output handle
	 * undefined on failure, and destroy_atlas_image() is what runs next. */
	VK(a)->font_image = VK_NULL_HANDLE;
	VK(a)->font_mem   = VK_NULL_HANDLE;
	VK(a)->font_view  = VK_NULL_HANDLE;

	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8_UNORM,
		.extent = { aw, ah, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vkCreateImage(VK(a)->device, &ici, NULL, &VK(a)->font_image) !=
			VK_SUCCESS) {
		VK(a)->font_image = VK_NULL_HANDLE;
		return fnl_fail(FNL_ERR_SYSTEM,
						"could not create the %ux%u glyph atlas", aw, ah);
	}

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(VK(a)->device, VK(a)->font_image, &req);
	uint32_t type;
	FNL_TRY(atlas_memory_type(a, req.memoryTypeBits, &type));
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = type,
	};
	/* Nulled again on a failure: the call leaves it undefined, and the
	 * clearing above came before it. */
	const FnlResult mrc =
		VKR(vkAllocateMemory(VK(a)->device, &mai, NULL, &VK(a)->font_mem));
	if (mrc < 0) {
		VK(a)->font_mem = VK_NULL_HANDLE;
		return mrc;
	}
	VK_TRY(vkBindImageMemory(VK(a)->device, VK(a)->font_image,
							 VK(a)->font_mem, 0));

	/* Staging copy, freed before this returns: the atlas only changes when the
	 * scale does, and that comes back through here. */
	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = bytes,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	/* From here this function owns the staging pair, so it unwinds through
	 * one label instead of FNL_TRY. */
	VkBuffer       stage = VK_NULL_HANDLE;
	VkDeviceMemory smem  = VK_NULL_HANDLE;
	FnlResult      rc;

	/* A handle a failed call wrote is undefined: nulled before the label
	 * destroys it. */
	if ((rc = VKR(vkCreateBuffer(VK(a)->device, &bci, NULL, &stage))) < 0) {
		stage = VK_NULL_HANDLE;
		goto fail;
	}
	vkGetBufferMemoryRequirements(VK(a)->device, stage, &req);
	uint32_t stype;
	if ((rc = find_memory_type(a, req.memoryTypeBits,
							   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
							   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
							   &stype)) < 0)
		goto fail;
	VkMemoryAllocateInfo smai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = stype,
	};
	if ((rc = VKR(vkAllocateMemory(VK(a)->device, &smai, NULL, &smem))) < 0) {
		smem = VK_NULL_HANDLE;
		goto fail;
	}
	if ((rc = VKR(vkBindBufferMemory(VK(a)->device, stage, smem, 0))) < 0)
		goto fail;

	/* Decoded into a scratch block and copied in, never straight into the
	 * mapping: the codec reads back what it has written, and a
	 * HOST_VISIBLE|HOST_COHERENT type is uncached (19.9 ms against 5.5 at
	 * scale 1.0, on the render thread at every scale change). HOST_CACHED is
	 * not guaranteed to exist. DESIGN *The blob*.
	 *
	 * Zeroed: a decode that succeeds has written every byte, which
	 * -fanalyzer cannot follow through the range decoder. */
	unsigned char *scratch = calloc(1, (size_t)bytes);
	if (!scratch) {
		rc = fnl_fail(FNL_ERR_OUT_OF_MEMORY,
					  "out of memory expanding the %ux%u glyph atlas", aw, ah);
		goto fail;
	}
	/* A stream that does not decode to exactly this atlas is a corrupt
	 * executable, but a library does not end the host process over it.
	 * Checked before the mapping, so a corrupt blob never maps. */
	if (!font_decode(src, FONT_ATLAS_LEN[si], scratch, (size_t)bytes)) {
		free(scratch);
		rc = fnl_fail(FNL_ERR_SYSTEM,
					  "the baked atlas for scale %d does not decode to %ux%u",
					  FONT_SCALE[si], aw, ah);
		goto fail;
	}
	void *p;
	if ((rc = VKR(vkMapMemory(VK(a)->device, smem, 0, bytes, 0, &p))) < 0) {
		free(scratch);
		goto fail;
	}
	memcpy(p, scratch, (size_t)bytes);
	free(scratch);
	vkUnmapMemory(VK(a)->device, smem);

	VkCommandBuffer cb = begin_oneshot(a);
	if (cb == VK_NULL_HANDLE) {
		rc = FNL_ERR_SYSTEM;
		goto fail;
	}
	VkImageMemoryBarrier to_dst = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = VK(a)->font_image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
						 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
						 1, &to_dst);

	VkBufferImageCopy copy = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { aw, ah, 1 },
	};
	vkCmdCopyBufferToImage(cb, stage, VK(a)->font_image,
						   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

	VkImageMemoryBarrier to_read = to_dst;
	to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
						 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
						 0, NULL, 1, &to_read);
	rc = end_oneshot(a, cb);
	if (rc < 0)
		goto fail;   /* the staging pair is released there, not before */

	vkDestroyBuffer(VK(a)->device, stage, NULL);
	vkFreeMemory(VK(a)->device, smem, NULL);

	VK_TRY(make_image_view(a, VK(a)->font_image, VK_FORMAT_R8_UNORM,
						   VK_IMAGE_ASPECT_COLOR_BIT, &VK(a)->font_view));

	a->font_si      = si;
	a->font_atlas_w = (int)aw;
	a->font_atlas_h = (int)ah;
	return FNL_OK;

fail:
	vkDestroyBuffer(VK(a)->device, stage, NULL);
	vkFreeMemory(VK(a)->device, smem, NULL);
	return rc;
}

/* Point the descriptor at whatever image build_atlas() just made. */
static void write_descriptor(App *a)
{
	VkDescriptorImageInfo ii = {
		.sampler = VK(a)->font_sampler,
		.imageView = VK(a)->font_view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet w = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = VK(a)->text_set, .dstBinding = 0, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &ii,
	};
	vkUpdateDescriptorSets(VK(a)->device, 1, &w, 0, NULL);
}

/* Render thread, when it latches a new scale. Everything submitted must have
 * finished reading the image the descriptor points at before it is swapped,
 * hence the drain.
 *
 * Compared by atlas index rather than by scale: two fractional scales that
 * round to the same baked atlas have nothing to rebuild. */
FnlResult text_set_scale(App *a, int s120)
{
	if (!VK(a)->text_set)
		return FNL_OK;
	int si = scale_index(s120);
	if (si == a->font_si)
		return FNL_OK;

	/* This drain is the first thing to touch the device after the previous
	 * frame, so it is where a loss usually surfaces. Flag it and leave the
	 * atlas alone: the caller's create_swapchain sees the same loss, and the
	 * rebuild uploads the atlas for whatever scale is current by then. */
	VkResult r = vkQueueWaitIdle(VK(a)->queue);
	if (vk_lost(a, r))
		return FNL_OK;      /* flagged; the caller's rebuild answers it */
	VK_TRY(r);

	destroy_atlas_image(a);
	/* The descriptor is written only if there is a view to point it at; a null
	 * one in the set is invalid however little is drawn afterwards. */
	FnlResult rc = build_atlas(a, si);
	if (rc < 0)
		return rc;
	write_descriptor(a);
	return FNL_OK;
}

static FnlResult create_descriptor(App *a)
{
	VkDescriptorSetLayoutBinding b = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	VkDescriptorSetLayoutCreateInfo lci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &b,
	};
	VK_TRY(vkCreateDescriptorSetLayout(VK(a)->device, &lci, NULL,
										 &VK(a)->text_set_layout));

	VkDescriptorPoolSize ps = {
		.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1 };
	VkDescriptorPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps,
	};
	VK_TRY(vkCreateDescriptorPool(VK(a)->device, &pci, NULL,
								  &VK(a)->text_pool));

	VkDescriptorSetAllocateInfo dai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = VK(a)->text_pool,
		.descriptorSetCount = 1, .pSetLayouts = &VK(a)->text_set_layout,
	};
	VK_TRY(vkAllocateDescriptorSets(VK(a)->device, &dai, &VK(a)->text_set));

	/* Nearest: the atlas was baked for exactly the pixel grid these quads land
	 * on, so filtering would only blur the rasteriser's answer. */
	VkSamplerCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
	};
	VK_TRY(vkCreateSampler(VK(a)->device, &sci, NULL, &VK(a)->font_sampler));
	return FNL_OK;
}

FnlResult create_text_pipeline(App *a)
{
	FNL_TRY(create_descriptor(a));
	/* The scale is 120 until the render thread latches another;
	 * text_set_scale() then swaps the atlas. */
	a->font_si = -1;
	FNL_TRY(build_atlas(a, scale_index(scale120(a))));
	write_descriptor(a);

	FNL_TRY(create_instance_buffers(a, TEXT_ARENA_BYTES,
									VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0,
									OVERLAY_MAX_IMAGES, VK(a)->text_vb,
									&VK(a)->text_vb_mem, (void **)a->text_map));

	/* The subpixel variant needs a second fragment output, and so
	 * dualSrcBlend; the grayscale one reads the middle of each pixel's three
	 * texels and blends on one alpha. */
	VkShaderModule vert = make_shader(a, text_vert_spv, sizeof text_vert_spv);
	VkShaderModule frag = VK(a)->dual_src_blend
		? make_shader(a, text_frag_lcd_spv, sizeof text_frag_lcd_spv)
		: make_shader(a, text_frag_spv, sizeof text_frag_spv);

	/* Both stages: the vertex shader reads the framebuffer size, the fragment
	 * shader the blend space and the texel step. */
	VkPushConstantRange pcr = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0, .size = sizeof(TextPush) };
	VkPipelineLayoutCreateInfo lci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &VK(a)->text_set_layout,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
	/* The two modules are this function's, so the layout is not built through
	 * VK_TRY: every child of a device must be destroyed before it, and a
	 * module leaked here is one destroy_device_objects() cannot reach. */
	FnlResult rc = VKR(vkCreatePipelineLayout(VK(a)->device, &lci, NULL,
											  &VK(a)->text_layout));
	if (rc >= 0) {
		const VkVertexInputAttributeDescription va[3] = {
			{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
			  offsetof(GlyphInstance, rect) },
			{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
			  offsetof(GlyphInstance, uv) },
			{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
			  offsetof(GlyphInstance, color) },
		};
		VK(a)->text_pipeline =
			make_overlay_pipeline(a, VK(a)->render_pass, vert, frag,
								  VK(a)->text_layout,
								  sizeof(GlyphInstance), va, 3,
								  VK(a)->dual_src_blend ? OVERLAY_BLEND_DUAL_SRC
													: OVERLAY_BLEND_ALPHA);
	}

	vkDestroyShaderModule(VK(a)->device, vert, NULL);
	vkDestroyShaderModule(VK(a)->device, frag, NULL);

	if (rc < 0)
		return rc;
	if (VK(a)->text_pipeline == VK_NULL_HANDLE)
		return fnl_fail(FNL_ERR_SYSTEM, "the glyph pipeline did not build");
	return FNL_OK;
}

void destroy_text_pipeline(App *a)
{
	destroy_instance_buffers(a, OVERLAY_MAX_IMAGES, VK(a)->text_vb,
							 &VK(a)->text_vb_mem, (void **)a->text_map);
	vkDestroyPipeline(VK(a)->device, VK(a)->text_pipeline, NULL);
	vkDestroyPipelineLayout(VK(a)->device, VK(a)->text_layout, NULL);
	vkDestroyDescriptorPool(VK(a)->device, VK(a)->text_pool, NULL);
	vkDestroyDescriptorSetLayout(VK(a)->device, VK(a)->text_set_layout, NULL);
	vkDestroySampler(VK(a)->device, VK(a)->font_sampler, NULL);
	destroy_atlas_image(a);

	/* Every handle is cleared, which is what makes a second call safe. */
	VK(a)->text_pipeline   = VK_NULL_HANDLE;
	VK(a)->text_layout     = VK_NULL_HANDLE;
	VK(a)->text_pool       = VK_NULL_HANDLE;
	VK(a)->text_set_layout = VK_NULL_HANDLE;
	VK(a)->text_set        = VK_NULL_HANDLE;   /* freed with the pool */
	VK(a)->font_sampler    = VK_NULL_HANDLE;
	a->font_si         = -1;
}

/* ---- recording --------------------------------------------------------- */

FnlResult vk_text_end(App *a)
{
	VkCommandBuffer cb = VK(a)->overlay_cb;
	if (a->text_count == a->text_drawn || cb == VK_NULL_HANDLE ||
		VK(a)->text_pipeline == VK_NULL_HANDLE)
		return FNL_OK;

	/* text_layout's push range is not the rectangles' and strokes', so what
	 * they pushed is undefined from this bind on. */
	VK(a)->overlay_pushed = false;
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
					  VK(a)->text_pipeline);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
							VK(a)->text_layout, 0, 1, &VK(a)->text_set,
							0, NULL);
	VkDeviceSize zero = 0;
	vkCmdBindVertexBuffers(cb, 0, 1, &VK(a)->text_vb[a->text_img], &zero);

	/* The blend space the weight is wanted in, and the width of one atlas
	 * texel -- the step the subpixel path takes to the red and blue emitters.
	 * A step of zero switches that path off for a resampled scale (see
	 * text_resampled): all three samples then fall on the middle texel, which
	 * is the grayscale answer. */
	TextPush push = { .screen = { (float)a->extent.w,
								  (float)a->extent.h },
					  .srgb = a->srgb_surface ? 1u : 0u,
					  /* No glyph is appended without an atlas, but a
					   * divide by its width should not rest on that. */
					  .texel_u = text_resampled(a) || a->font_atlas_w <= 0
									 ? 0.0f
									 : 1.0f / (float)a->font_atlas_w };
	vkCmdPushConstants(cb, VK(a)->text_layout,
					   VK_SHADER_STAGE_VERTEX_BIT |
					   VK_SHADER_STAGE_FRAGMENT_BIT,
					   0, sizeof push, &push);

	/* Every glyph appended since the last of these. */
	vkCmdDraw(cb, 4, a->text_count - a->text_drawn, 0, a->text_drawn);
	a->text_drawn = a->text_count;
	return FNL_OK;
}

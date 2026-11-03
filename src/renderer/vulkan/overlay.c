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

/* The arena layout -- OVERLAY_SHAPE_OFF and the two byte counts -- is in
 * app.h, because src/core/overlay.c appends against the same offsets. */

/* ---- pipelines ---------------------------------------------------------- */

FnlResult create_overlay_pipelines(App *a)
{
	/* Neither pipeline reads a descriptor and both take the same block, in the
	 * vertex stage only, so one layout serves them. */
	VkPushConstantRange pcr = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		.offset = 0, .size = sizeof(OverlayPush) };
	VkPipelineLayoutCreateInfo lci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
	VK_TRY(vkCreatePipelineLayout(VK(a)->device, &lci, NULL,
								  &VK(a)->overlay_layout));

	VkShaderModule rv = make_shader(a, rect_vert_spv, sizeof rect_vert_spv);
	VkShaderModule rf = make_shader(a, rect_frag_spv, sizeof rect_frag_spv);
	const VkVertexInputAttributeDescription rect_va[2] = {
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RectInstance, rect) },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(RectInstance, color) },
	};
	VK(a)->rect_pipeline =
		make_overlay_pipeline(a, VK(a)->render_pass, rv, rf,
							  VK(a)->overlay_layout,
							  sizeof(RectInstance), rect_va, 2,
							  OVERLAY_BLEND_NONE);
	vkDestroyShaderModule(VK(a)->device, rv, NULL);
	vkDestroyShaderModule(VK(a)->device, rf, NULL);

	VkShaderModule sv = make_shader(a, shape_vert_spv, sizeof shape_vert_spv);
	VkShaderModule sf = make_shader(a, shape_frag_spv, sizeof shape_frag_spv);
	/* Packed as vec4s: fewer attributes is fewer fetches, at no extra cost. */
	const VkVertexInputAttributeDescription shape_va[3] = {
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
		  offsetof(ShapeInstance, p0) },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
		  offsetof(ShapeInstance, p2) },
		{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
		  offsetof(ShapeInstance, color) },
	};
	VK(a)->shape_pipeline =
		make_overlay_pipeline(a, VK(a)->render_pass, sv, sf,
							  VK(a)->overlay_layout,
							  sizeof(ShapeInstance), shape_va, 3,
							  OVERLAY_BLEND_ALPHA);
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

	/* Every handle is cleared, which is what makes a second call safe. */
	VK(a)->rect_pipeline  = VK_NULL_HANDLE;
	VK(a)->shape_pipeline = VK_NULL_HANDLE;
	VK(a)->overlay_layout = VK_NULL_HANDLE;
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
	VkRect2D sc = r ? (VkRect2D){ { r->x, r->y },
								  { (uint32_t)r->w, (uint32_t)r->h } }
					: (VkRect2D){ { 0, 0 },
								  { (uint32_t)a->extent.w,
									(uint32_t)a->extent.h } };
	vkCmdSetScissor(VK(a)->overlay_cb, 0, 1, &sc);
}

void vk_overlay_flush_shapes(App *a)
{
	flush(a, VK(a)->shape_pipeline, OVERLAY_SHAPE_OFF,
		  &a->shape_count, &a->shape_drawn);
}

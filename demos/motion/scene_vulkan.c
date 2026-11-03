/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * The cube as Vulkan draws it: one pipeline per scene target and nothing
 * else on the device. cube.vert makes the cube's 36 vertices from
 * gl_VertexIndex, so there is no buffer, image or descriptor to build, and
 * the matrix and the light travel as push constants inside the recording.
 *
 * Every entry point is asked of the library's loader
 * (FnlVulkanGpu::get_instance_proc_addr), never linked: a program that
 * imports the loader does not start on a machine without one.
 */

/* No Vulkan prototypes: a call to an entry SCENE_VK_FNS does not name is a
 * compile error, rather than a link to the loader. */
#define VK_NO_PROTOTYPES

#include "motion.h"

#include <fenestal/fnl_vulkan.h>

#include <stdio.h>
#include <string.h>

#include "cube.vert.h"
#include "cube.frag.h"

#define CUBE_VERTEX_COUNT 36

/* Every Vulkan entry this file needs, each a pointer of the function's own
 * name, so the calls below read as Vulkan's. */
#define SCENE_VK_FNS(X)                                                       \
	X(vkCmdBindPipeline)                                                      \
	X(vkCmdDraw)                                                              \
	X(vkCmdPushConstants)                                                     \
	X(vkCreateGraphicsPipelines)                                              \
	X(vkCreatePipelineLayout)                                                 \
	X(vkCreateShaderModule)                                                   \
	X(vkDestroyPipeline)                                                      \
	X(vkDestroyPipelineLayout)                                                \
	X(vkDestroyShaderModule)

#define X_DEFINE(f) static PFN_##f f;
SCENE_VK_FNS(X_DEFINE)
#undef X_DEFINE

/* Asked of the instance after every on_gpu_created: an entry is the
 * instance's, and the instance is new after every loss. */
static bool load_vulkan(const FnlVulkanGpu *gpu)
{
	if (!gpu->get_instance_proc_addr)
		return false;
#define X_LOAD(f)                                                             \
	f = (PFN_##f)gpu->get_instance_proc_addr(gpu->instance, #f);              \
	if (!f)                                                                   \
		return false;
	SCENE_VK_FNS(X_LOAD)
#undef X_LOAD
	return true;
}

/* One device generation: built in on_gpu_created, dropped in on_gpu_lost.
 * No lock: every callback runs on a thread the library owns, never two at
 * once. */
static struct {
	VkDevice         device;
	VkPipelineLayout layout;
	VkPipeline       pipeline[FNL_TARGET_COUNT];
	bool             srgb[FNL_TARGET_COUNT];
} g_dev;

/* The three _SRGB formats the library picks a surface from. A write to one
 * is encoded by the hardware, which is why the shader has to know. */
static bool format_is_srgb(VkFormat f)
{
	switch (f) {
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
		return true;
	default:
		return false;
	}
}

static VkShaderModule make_module(VkDevice dev, const uint32_t *code,
								  size_t bytes)
{
	VkShaderModuleCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = bytes, .pCode = code };
	VkShaderModule m = VK_NULL_HANDLE;
	if (vkCreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return m;
}

/* The same pipeline once per target, differing only in the pass it is for
 * and that pass's sample count -- what fnl_vulkan_scene_layout() reports.
 * Viewport and scissor are dynamic: the library sets both before on_record
 * is called. */
static VkPipeline make_pipeline(const FnlVulkanGpu *gpu,
								const FnlVulkanSceneLayout *layout,
								VkShaderModule vert, VkShaderModule frag)
{
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert,
		  .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag,
		  .pName = "main" },
	};
	/* No vertex input: the shader makes the cube. */
	VkPipelineVertexInputStateCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkPipelineViewportStateCreateInfo vp = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .scissorCount = 1 };
	VkPipelineRasterizationStateCreateInfo rs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		/* Counter-clockwise, as cube.vert winds for a y-up world: the
		 * projection's negated y and Vulkan's y-down framebuffer cancel. */
		.cullMode = VK_CULL_MODE_BACK_BIT,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f };
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = layout->samples };
	/* The scene's pass carries a depth attachment on every target, cleared
	 * to 1.0 as the pass begins, so only the test has to be switched on. */
	VkPipelineDepthStencilStateCreateInfo ds = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS };
	VkPipelineColorBlendAttachmentState cba = {
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
						  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
	VkPipelineColorBlendStateCreateInfo cb = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT,
							  VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dsi = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2, .pDynamicStates = dyn };

	VkGraphicsPipelineCreateInfo gci = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = stages,
		.pVertexInputState = &vi,
		.pInputAssemblyState = &ia,
		.pViewportState = &vp,
		.pRasterizationState = &rs,
		.pMultisampleState = &ms,
		.pDepthStencilState = &ds,
		.pColorBlendState = &cb,
		.pDynamicState = &dsi,
		.layout = g_dev.layout,
		.renderPass = layout->render_pass,
		.subpass = layout->subpass,
	};
	VkPipeline p = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(gpu->device, gpu->pipeline_cache, 1, &gci,
								  NULL, &p) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return p;
}

/* Everything the device owns, dropped. Every destroy takes a null handle
 * and does nothing, so one function serves an ordinary loss and a build
 * that stopped halfway. */
void scene_vulkan_on_gpu_lost(void *ud, FnlApp *app)
{
	(void)ud;
	(void)app;
	if (!g_dev.device)
		return;
	for (int i = 0; i < FNL_TARGET_COUNT; i++)
		vkDestroyPipeline(g_dev.device, g_dev.pipeline[i], NULL);
	vkDestroyPipelineLayout(g_dev.device, g_dev.layout, NULL);
	memset(&g_dev, 0, sizeof g_dev);
}

/* Called once at startup and again after every device loss, with a device
 * that shares nothing with the last one. A failure leaves the window open
 * and the scene black. */
void scene_vulkan_on_gpu_created(void *ud, FnlApp *app)
{
	(void)ud;
	FnlVulkanGpu gpu;
	if (!fnl_vulkan_gpu(app, &gpu)) {
		fprintf(stderr, "the running renderer is not Vulkan\n");
		return;
	}
	/* Before g_dev.device: on_gpu_lost destroys nothing without it, so a
	 * loader that answered only some of the entries is never called. */
	if (!load_vulkan(&gpu)) {
		fprintf(stderr, "the Vulkan loader is missing an entry the scene "
						"calls\n");
		return;
	}
	g_dev.device = gpu.device;

	VkPushConstantRange pcr = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		.offset = 0, .size = sizeof(CubePush) };
	VkPipelineLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
	if (vkCreatePipelineLayout(gpu.device, &plci, NULL, &g_dev.layout)
		!= VK_SUCCESS) {
		g_dev.layout = VK_NULL_HANDLE;
		fprintf(stderr, "could not build the scene's pipeline layout\n");
		return;
	}

	VkShaderModule vert = make_module(gpu.device, cube_vert_spv,
									  sizeof cube_vert_spv);
	VkShaderModule frag = make_module(gpu.device, cube_frag_spv,
									  sizeof cube_frag_spv);
	if (vert != VK_NULL_HANDLE && frag != VK_NULL_HANDLE)
		for (int i = 0; i < FNL_TARGET_COUNT; i++) {
			FnlVulkanSceneLayout layout;
			if (!fnl_vulkan_scene_layout(app, (FnlSceneTarget)i, &layout))
				continue;          /* this window cannot use that target */
			g_dev.srgb[i] = format_is_srgb(layout.color_format);
			g_dev.pipeline[i] = make_pipeline(&gpu, &layout, vert, frag);
		}
	vkDestroyShaderModule(gpu.device, vert, NULL);
	vkDestroyShaderModule(gpu.device, frag, NULL);
}

/* The cube, into the command buffer the library is recording -- inside the
 * scene's pass, with the viewport and scissor already set. */
void scene_vulkan_draw(FnlApp *app, const FnlFrame *f, CubePush *push)
{
	VkCommandBuffer cb;
	if (!fnl_vulkan_scene_cmd(app, &cb))
		return;
	if (f->target < 0 || f->target >= FNL_TARGET_COUNT ||
		g_dev.pipeline[f->target] == VK_NULL_HANDLE)
		return;

	push->srgb = g_dev.srgb[f->target] ? 1u : 0u;
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
					  g_dev.pipeline[f->target]);
	vkCmdPushConstants(cb, g_dev.layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
					   sizeof *push, push);
	vkCmdDraw(cb, CUBE_VERTEX_COUNT, 1, 0, 0);
}

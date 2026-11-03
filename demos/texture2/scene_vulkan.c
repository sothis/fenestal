/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * The textured cube as Vulkan draws it: the texture as an image, one
 * descriptor set that names it with a sampler, and a pipeline per scene
 * target. cube.vert makes the cube's 36 vertices from gl_VertexIndex, so
 * there is no vertex buffer; the matrix and the light travel as push
 * constants inside the recording.
 *
 * Every entry point is asked of the library's loader
 * (FnlVulkanGpu::get_instance_proc_addr), never linked: a program that
 * imports the loader does not start on a machine without one, where the
 * library would have drawn with Direct3D 12 instead, or not at all.
 */

/* No Vulkan prototypes: a call to an entry SCENE_VK_FNS does not name is a
 * compile error, rather than a link to the loader. */
#define VK_NO_PROTOTYPES

#include "texture2.h"

#include <fenestal/fnl_vulkan.h>

#include <stdio.h>
#include <string.h>

#include "cube.vert.h"
#include "cube.frag.h"

#define CUBE_VERTEX_COUNT 36

/* Every Vulkan entry this file calls, each a pointer of the function's own
 * name, so the calls below read as Vulkan's. */
#define SCENE_VK_FNS(X)                                                       \
	X(vkAllocateCommandBuffers)                                               \
	X(vkAllocateDescriptorSets)                                               \
	X(vkAllocateMemory)                                                       \
	X(vkBeginCommandBuffer)                                                   \
	X(vkBindBufferMemory)                                                     \
	X(vkBindImageMemory)                                                      \
	X(vkCmdBindDescriptorSets)                                                \
	X(vkCmdBindPipeline)                                                      \
	X(vkCmdCopyBufferToImage)                                                 \
	X(vkCmdDraw)                                                              \
	X(vkCmdPipelineBarrier)                                                   \
	X(vkCmdPushConstants)                                                     \
	X(vkCreateBuffer)                                                         \
	X(vkCreateCommandPool)                                                    \
	X(vkCreateDescriptorPool)                                                 \
	X(vkCreateDescriptorSetLayout)                                            \
	X(vkCreateGraphicsPipelines)                                              \
	X(vkCreateImage)                                                          \
	X(vkCreateImageView)                                                      \
	X(vkCreatePipelineLayout)                                                 \
	X(vkCreateSampler)                                                        \
	X(vkCreateShaderModule)                                                   \
	X(vkDestroyBuffer)                                                        \
	X(vkDestroyCommandPool)                                                   \
	X(vkDestroyDescriptorPool)                                                \
	X(vkDestroyDescriptorSetLayout)                                           \
	X(vkDestroyImage)                                                         \
	X(vkDestroyImageView)                                                     \
	X(vkDestroyPipeline)                                                      \
	X(vkDestroyPipelineLayout)                                                \
	X(vkDestroySampler)                                                       \
	X(vkDestroyShaderModule)                                                  \
	X(vkEndCommandBuffer)                                                     \
	X(vkFreeMemory)                                                           \
	X(vkGetBufferMemoryRequirements)                                          \
	X(vkGetImageMemoryRequirements)                                           \
	X(vkGetPhysicalDeviceMemoryProperties)                                    \
	X(vkMapMemory)                                                            \
	X(vkQueueSubmit)                                                          \
	X(vkQueueWaitIdle)                                                        \
	X(vkUnmapMemory)                                                          \
	X(vkUpdateDescriptorSets)

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
 * One struct so a loss clears it in one statement. No lock: every callback
 * runs on a thread the library owns, never two at once. */
static struct {
	VkDevice              device;
	VkPipelineLayout      layout;
	VkDescriptorSetLayout set_layout;
	VkDescriptorPool      pool;
	VkDescriptorSet       set;
	VkPipeline            pipeline[FNL_TARGET_COUNT];
	bool                  srgb[FNL_TARGET_COUNT];
	VkImage               tex;
	VkDeviceMemory        tex_mem;
	VkImageView           tex_view;
	VkSampler             sampler;
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

static uint32_t memory_type(const FnlVulkanGpu *gpu, uint32_t bits,
							VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(gpu->physical_device, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) &&
			(mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return UINT32_MAX;
}

/* A host-visible buffer holding `bytes` of `src`: the texture's way to the
 * device. Both outputs are left null or valid, never undefined, so the
 * caller can destroy whatever it finds whether or not this succeeded. */
static bool make_staging(const FnlVulkanGpu *gpu, const void *src,
						 VkDeviceSize bytes, VkBuffer *buf,
						 VkDeviceMemory *mem)
{
	*buf = VK_NULL_HANDLE;
	*mem = VK_NULL_HANDLE;

	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE };
	if (vkCreateBuffer(gpu->device, &bci, NULL, buf) != VK_SUCCESS) {
		*buf = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(gpu->device, *buf, &req);
	const uint32_t type = memory_type(gpu, req.memoryTypeBits,
									  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
									  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (type == UINT32_MAX)
		return false;
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size, .memoryTypeIndex = type };
	if (vkAllocateMemory(gpu->device, &mai, NULL, mem) != VK_SUCCESS) {
		*mem = VK_NULL_HANDLE;
		return false;
	}
	void *dst;
	if (vkBindBufferMemory(gpu->device, *buf, *mem, 0) != VK_SUCCESS ||
		vkMapMemory(gpu->device, *mem, 0, bytes, 0, &dst) != VK_SUCCESS)
		return false;
	memcpy(dst, src, (size_t)bytes);
	vkUnmapMemory(gpu->device, *mem);
	return true;
}

static void transition(VkCommandBuffer cb, VkImage img, VkImageLayout from,
					   VkImageLayout to, VkAccessFlags src_access,
					   VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
					   VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = src_access, .dstAccessMask = dst_access,
		.oldLayout = from, .newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
							  .levelCount = 1, .layerCount = 1 },
	};
	vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* The texture, uploaded the way a real one is: into a staging buffer, then
 * copied by the device into an image of its own.
 *
 * The copy is submitted on the library's queue, and that is safe here and
 * only here: on_gpu_created runs either before the render thread exists or
 * on it, so nothing else is submitting. From any other thread the queue is
 * shared and wants a lock of the caller's own (fnl_vulkan.h).
 *
 * _SRGB, because the bytes are colours as a picker gives them: sampling
 * then returns linear values, which is what the shader lights. */
static bool make_texture(const FnlVulkanGpu *gpu)
{
	static unsigned char px[TEX_SIZE * TEX_SIZE * 4];
	texture_pixels(px);

	VkBuffer        staging = VK_NULL_HANDLE;
	VkDeviceMemory  staging_mem = VK_NULL_HANDLE;
	VkCommandPool   cmd_pool = VK_NULL_HANDLE;
	VkCommandBuffer cb = VK_NULL_HANDLE;
	bool            ok = false;

	if (!make_staging(gpu, px, sizeof px, &staging, &staging_mem))
		goto done;

	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_SRGB,
		.extent = { TEX_SIZE, TEX_SIZE, 1 },
		.mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vkCreateImage(gpu->device, &ici, NULL, &g_dev.tex) != VK_SUCCESS) {
		g_dev.tex = VK_NULL_HANDLE;
		goto done;
	}
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(gpu->device, g_dev.tex, &req);
	const uint32_t type = memory_type(gpu, req.memoryTypeBits,
									  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type == UINT32_MAX)
		goto done;
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size, .memoryTypeIndex = type };
	if (vkAllocateMemory(gpu->device, &mai, NULL, &g_dev.tex_mem)
		!= VK_SUCCESS) {
		g_dev.tex_mem = VK_NULL_HANDLE;
		goto done;
	}
	if (vkBindImageMemory(gpu->device, g_dev.tex, g_dev.tex_mem, 0)
		!= VK_SUCCESS)
		goto done;

	VkCommandPoolCreateInfo cpi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
		.queueFamilyIndex = gpu->queue_family };
	if (vkCreateCommandPool(gpu->device, &cpi, NULL, &cmd_pool)
		!= VK_SUCCESS) {
		cmd_pool = VK_NULL_HANDLE;
		goto done;
	}
	VkCommandBufferAllocateInfo cai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1 };
	if (vkAllocateCommandBuffers(gpu->device, &cai, &cb) != VK_SUCCESS)
		goto done;

	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS)
		goto done;
	transition(cb, g_dev.tex, VK_IMAGE_LAYOUT_UNDEFINED,
			   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			   0, VK_ACCESS_TRANSFER_WRITE_BIT,
			   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			   VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkBufferImageCopy region = {
		.imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
							  .layerCount = 1 },
		.imageExtent = { TEX_SIZE, TEX_SIZE, 1 },
	};
	vkCmdCopyBufferToImage(cb, staging, g_dev.tex,
						   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	transition(cb, g_dev.tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			   VK_PIPELINE_STAGE_TRANSFER_BIT,
			   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	if (vkEndCommandBuffer(cb) != VK_SUCCESS)
		goto done;

	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1, .pCommandBuffers = &cb };
	/* Waited out, so the staging buffer can go as soon as this returns. */
	if (vkQueueSubmit(gpu->queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS ||
		vkQueueWaitIdle(gpu->queue) != VK_SUCCESS)
		goto done;

	VkImageViewCreateInfo vci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = g_dev.tex,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_SRGB,
		.subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
							  .levelCount = 1, .layerCount = 1 },
	};
	if (vkCreateImageView(gpu->device, &vci, NULL, &g_dev.tex_view)
		!= VK_SUCCESS) {
		g_dev.tex_view = VK_NULL_HANDLE;
		goto done;
	}

	/* Linear both ways, clamped at the edges: the frame round the
	 * checkerboard is the face's edge and is not to bleed in from the
	 * opposite side. */
	VkSamplerCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.maxLod = 0.0f,
	};
	if (vkCreateSampler(gpu->device, &sci, NULL, &g_dev.sampler)
		!= VK_SUCCESS) {
		g_dev.sampler = VK_NULL_HANDLE;
		goto done;
	}
	ok = true;

done:
	/* The pool takes its command buffer with it. */
	vkDestroyCommandPool(gpu->device, cmd_pool, NULL);
	vkDestroyBuffer(gpu->device, staging, NULL);
	vkFreeMemory(gpu->device, staging_mem, NULL);
	return ok;
}

/* One set, one binding: the texture with its sampler, for the fragment
 * stage; and the pipeline layout around it, with CubePush for both
 * stages -- cube.frag reads its srgb. */
static bool make_descriptors(const FnlVulkanGpu *gpu)
{
	VkDescriptorSetLayoutBinding binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
	VkDescriptorSetLayoutCreateInfo lci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &binding };
	if (vkCreateDescriptorSetLayout(gpu->device, &lci, NULL,
									&g_dev.set_layout) != VK_SUCCESS) {
		g_dev.set_layout = VK_NULL_HANDLE;
		return false;
	}

	VkDescriptorPoolSize size = {
		.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1 };
	VkDescriptorPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size };
	if (vkCreateDescriptorPool(gpu->device, &pci, NULL, &g_dev.pool)
		!= VK_SUCCESS) {
		g_dev.pool = VK_NULL_HANDLE;
		return false;
	}

	VkDescriptorSetAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = g_dev.pool,
		.descriptorSetCount = 1, .pSetLayouts = &g_dev.set_layout };
	if (vkAllocateDescriptorSets(gpu->device, &ai, &g_dev.set)
		!= VK_SUCCESS) {
		g_dev.set = VK_NULL_HANDLE;
		return false;
	}

	VkDescriptorImageInfo info = {
		.sampler = g_dev.sampler, .imageView = g_dev.tex_view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet w = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = g_dev.set, .dstBinding = 0, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &info };
	vkUpdateDescriptorSets(gpu->device, 1, &w, 0, NULL);

	VkPushConstantRange pcr = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
					  VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0, .size = sizeof(CubePush) };
	VkPipelineLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &g_dev.set_layout,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
	if (vkCreatePipelineLayout(gpu->device, &plci, NULL, &g_dev.layout)
		!= VK_SUCCESS) {
		g_dev.layout = VK_NULL_HANDLE;
		return false;
	}
	return true;
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
	/* The pool takes its set with it. */
	vkDestroyDescriptorPool(g_dev.device, g_dev.pool, NULL);
	vkDestroyDescriptorSetLayout(g_dev.device, g_dev.set_layout, NULL);
	vkDestroySampler(g_dev.device, g_dev.sampler, NULL);
	vkDestroyImageView(g_dev.device, g_dev.tex_view, NULL);
	vkDestroyImage(g_dev.device, g_dev.tex, NULL);
	vkFreeMemory(g_dev.device, g_dev.tex_mem, NULL);
	memset(&g_dev, 0, sizeof g_dev);
}

/* Called once at startup and again after every device loss, with a device
 * that shares nothing with the last one. A failure leaves the window open
 * and the scene black. */
void scene_vulkan_on_gpu_created(void *ud, FnlApp *app)
{
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

	if (!make_texture(&gpu) || !make_descriptors(&gpu)) {
		fprintf(stderr, "could not build the scene's texture\n");
		scene_vulkan_on_gpu_lost(ud, app);
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
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dev.layout,
							0, 1, &g_dev.set, 0, NULL);
	vkCmdPushConstants(cb, g_dev.layout,
					   VK_SHADER_STAGE_VERTEX_BIT |
					   VK_SHADER_STAGE_FRAGMENT_BIT,
					   0, sizeof *push, push);
	vkCmdDraw(cb, CUBE_VERTEX_COUNT, 1, 0, 0);
}

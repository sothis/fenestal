/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* The render scale's stretch alone, on whatever device is first: a
 * SRC_W x SRC_H image onto a DST_W x DST_H one, as the library draws it (a
 * covering triangle sampling with a LINEAR sampler) and as vkCmdBlitImage
 * with VK_FILTER_LINEAR would, in _SRGB and in _UNORM.
 *
 *   stretch [iterations]
 *
 * Each case is one command buffer, submitted and waited for `iterations`
 * times; the figure is the median wall time of a submit-and-wait, less the
 * median of an empty one. It is DESIGN *X11*'s stretch figure, measured on
 * the thinkpad, where there is no compiler: built here and copied over.
 *
 *   glslangValidator -V --vn stretch_vert tools/probe/stretch.vert \
 *       -o build/stretch_vert.h
 *   glslangValidator -V --vn stretch_frag tools/probe/stretch.frag \
 *       -o build/stretch_frag.h
 *   gcc -O2 -Wall -Wextra -Ibuild -o build/stretch tools/probe/stretch.c \
 *       -lvulkan */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "stretch_vert.h"
#include "stretch_frag.h"

#define SRC_W 640
#define SRC_H 480
#define DST_W 1280
#define DST_H 720

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s: %d\n", #x, r_); exit(1); } } while (0)

static VkDevice dev;
static VkPhysicalDevice pd;
static VkQueue q;
static VkCommandPool pool;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) &&
			(mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type\n");
	exit(1);
}

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } Img;

static Img make_image(VkFormat f, int w, int h, VkImageUsageFlags usage)
{
	Img r;
	VkImageCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = f,
		.extent = { (uint32_t)w, (uint32_t)h, 1 }, .mipLevels = 1,
		.arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
	CK(vkCreateImage(dev, &ci, NULL, &r.img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, r.img, &mr);
	VkMemoryAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits,
									VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	CK(vkAllocateMemory(dev, &ai, NULL, &r.mem));
	CK(vkBindImageMemory(dev, r.img, r.mem, 0));
	VkImageViewCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = r.img,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	CK(vkCreateImageView(dev, &vi, NULL, &r.view));
	return r;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from,
					VkImageLayout to)
{
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
		.oldLayout = from, .newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
						 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
						 NULL, 1, &b);
}

static VkCommandBuffer begin(void)
{
	VkCommandBuffer cb;
	VkCommandBufferAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1 };
	CK(vkAllocateCommandBuffers(dev, &ai, &cb));
	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT };
	CK(vkBeginCommandBuffer(cb, &bi));
	return cb;
}

static void submit_wait(VkCommandBuffer cb)
{
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
						.commandBufferCount = 1, .pCommandBuffers = &cb };
	CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(q));
}

static double time_cb(VkCommandBuffer cb, int n)
{
	double *t = malloc(sizeof *t * (size_t)n);
	submit_wait(cb);                          /* first-use costs out */
	for (int i = 0; i < n; i++) {
		double t0 = now();
		submit_wait(cb);
		t[i] = now() - t0;
	}
	qsort(t, (size_t)n, sizeof *t, cmp);
	double m = t[n / 2];
	free(t);
	return m * 1e3;
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
	VkShaderModuleCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size, .pCode = code };
	VkShaderModule m;
	CK(vkCreateShaderModule(dev, &ci, NULL, &m));
	return m;
}

/* The two stretches over one pair of images in format `f`. */
static void run(VkFormat f, const char *name, int n, double empty)
{
	VkFormatProperties fp;
	vkGetPhysicalDeviceFormatProperties(pd, f, &fp);
	const VkFormatFeatureFlags of = fp.optimalTilingFeatures;

	Img src = make_image(f, SRC_W, SRC_H, VK_IMAGE_USAGE_SAMPLED_BIT |
						 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
						 VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	Img dst = make_image(f, DST_W, DST_H,
						 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
						 VK_IMAGE_USAGE_TRANSFER_DST_BIT);

	/* Something other than one colour in the source, so that no path can
	 * take a shortcut over a uniform image: four quadrants. */
	VkCommandBuffer cb = begin();
	barrier(cb, src.img, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	const VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT,
										  0, 1, 0, 1 };
	VkClearColorValue c0 = { .float32 = { 0.2f, 0.5f, 0.8f, 1.0f } };
	vkCmdClearColorImage(cb, src.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						 &c0, 1, &all);
	barrier(cb, src.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	barrier(cb, dst.img, VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	CK(vkEndCommandBuffer(cb));
	submit_wait(cb);

	/* ---- the draw, as blit.frag does it ---- */
	VkAttachmentDescription att = {
		.format = f, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sp = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &ref };
	VkRenderPassCreateInfo rpci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &att, .subpassCount = 1,
		.pSubpasses = &sp };
	VkRenderPass rp;
	CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
	VkFramebufferCreateInfo fci = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
		.attachmentCount = 1, .pAttachments = &dst.view, .width = DST_W,
		.height = DST_H, .layers = 1 };
	VkFramebuffer fb;
	CK(vkCreateFramebuffer(dev, &fci, NULL, &fb));

	VkSamplerCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
	VkSampler smp;
	CK(vkCreateSampler(dev, &sci, NULL, &smp));
	VkDescriptorSetLayoutBinding lb = {
		0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
		VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo dlci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &lb };
	VkDescriptorSetLayout dsl;
	CK(vkCreateDescriptorSetLayout(dev, &dlci, NULL, &dsl));
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
	VkDescriptorPoolCreateInfo dpci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
		.poolSizeCount = 1, .pPoolSizes = &ps };
	VkDescriptorPool dp;
	CK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
	VkDescriptorSetAllocateInfo dsai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
	VkDescriptorSet set;
	CK(vkAllocateDescriptorSets(dev, &dsai, &set));
	VkDescriptorImageInfo dii = { smp, src.view,
								  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet w = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &dii };
	vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
	VkPipelineLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl };
	VkPipelineLayout pl;
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));

	VkShaderModule vs = shader(stretch_vert, sizeof stretch_vert);
	VkShaderModule fs = shader(stretch_frag, sizeof stretch_frag);
	VkPipelineShaderStageCreateInfo st[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs,
		  .pName = "main" } };
	VkPipelineVertexInputStateCreateInfo vin = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { 0, 0, DST_W, DST_H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { DST_W, DST_H } };
	VkPipelineViewportStateCreateInfo vps = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .pViewports = &vp, .scissorCount = 1,
		.pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
		.lineWidth = 1.0f };
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cb_s = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkGraphicsPipelineCreateInfo gp = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = st, .pVertexInputState = &vin,
		.pInputAssemblyState = &ia, .pViewportState = &vps,
		.pRasterizationState = &rs, .pMultisampleState = &ms,
		.pColorBlendState = &cb_s, .layout = pl, .renderPass = rp };
	VkPipeline pipe;
	CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));

	VkCommandBuffer draw = begin();
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
		.framebuffer = fb, .renderArea.extent = { DST_W, DST_H } };
	vkCmdBeginRenderPass(draw, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(draw, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets(draw, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1,
							&set, 0, NULL);
	vkCmdDraw(draw, 3, 1, 0, 0);
	vkCmdEndRenderPass(draw);
	CK(vkEndCommandBuffer(draw));
	printf("%-6s draw, LINEAR sampler        %7.2f ms\n", name,
		   time_cb(draw, n) - empty);

	/* ---- the blit, both filters, with the layouts it needs ---- */
	const bool can = (of & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
					 (of & VK_FORMAT_FEATURE_BLIT_DST_BIT);
	const bool lin = of & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	for (int k = 0; k < 2; k++) {
		const VkFilter filt = k ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
		if (!can || (!k && !lin)) {
			printf("%-6s blit, %s: not offered\n", name,
				   k ? "NEAREST" : "LINEAR");
			continue;
		}
		VkCommandBuffer bl = begin();
		barrier(bl, src.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		barrier(bl, dst.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkImageBlit r = {
			.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.srcOffsets = { { 0, 0, 0 }, { SRC_W, SRC_H, 1 } },
			.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.dstOffsets = { { 0, 0, 0 }, { DST_W, DST_H, 1 } } };
		vkCmdBlitImage(bl, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					   dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
					   filt);
		barrier(bl, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		barrier(bl, dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		CK(vkEndCommandBuffer(bl));
		printf("%-6s blit, %-7s                %7.2f ms\n", name,
			   k ? "NEAREST" : "LINEAR", time_cb(bl, n) - empty);
	}

	/* And a clear of the destination, for scale: one full-size write. */
	VkCommandBuffer cl = begin();
	barrier(cl, dst.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdClearColorImage(cl, dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						 &c0, 1, &all);
	barrier(cl, dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	CK(vkEndCommandBuffer(cl));
	printf("%-6s clear of the destination    %7.2f ms\n", name,
		   time_cb(cl, n) - empty);
}

int main(int argc, char **argv)
{
	const int n = argc > 1 ? atoi(argv[1]) : 100;
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
							  .apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app };
	VkInstance inst;
	CK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t np = 1;
	vkEnumeratePhysicalDevices(inst, &np, &pd);
	VkPhysicalDeviceProperties pp;
	vkGetPhysicalDeviceProperties(pd, &pp);
	printf("%s, %dx%d onto %dx%d, median of %d\n", pp.deviceName, SRC_W,
		   SRC_H, DST_W, DST_H, n);
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
							   .queueCreateInfoCount = 1,
							   .pQueueCreateInfos = &qci };
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &q);
	VkCommandPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = 0 };
	CK(vkCreateCommandPool(dev, &pci, NULL, &pool));

	VkCommandBuffer empty = begin();
	CK(vkEndCommandBuffer(empty));
	const double e = time_cb(empty, n);
	printf("an empty submit and wait     %7.2f ms (taken off below)\n", e);

	run(VK_FORMAT_R8G8B8A8_SRGB,  "SRGB",  n, e);
	run(VK_FORMAT_R8G8B8A8_UNORM, "UNORM", n, e);
	return 0;
}

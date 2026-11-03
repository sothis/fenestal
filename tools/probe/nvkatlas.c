/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 */

/* A tiled image in VRAM read back after the GPU has been left idle long
 * enough to runtime-suspend, with nothing of fenestal in it:
 * docs/mesa-nvk-pascal.md's reproducer. On schenker's GTX 1050 Ti (NVK,
 * nouveau, kernel 7.2.9) the image comes back laid out wrong after the GPU
 * powers off and on under a live device -- half its texels garbled -- and
 * never with runtime PM off, under the autosuspend delay, or on the Intel
 * GPU beside it.
 *
 *   NVKATLAS_IDLE=12 [NVKATLAS_HOST=1] nvkatlas [device-name-part [rounds]]
 *   nvkatlas wayland [device-name-part]      (built with NVKATLAS_WAYLAND)
 *
 * The image is ATLAS_W x ATLAS_H R8_UNORM, OPTIMAL tiling, a pseudo-random
 * pattern uploaded once by vkCmdCopyBufferToImage; a check samples it with
 * texture() through a NEAREST sampler at every texel's centre into an
 * R8G8B8A8_UNORM target of its own size, reads that back and compares it
 * texel by texel. NVKATLAS_IDLE=s checks once, sleeps s seconds and checks
 * again; nouveau's autosuspend delay is 5 s. The rest is how the cause was
 * found and is kept for that: `rounds` of render targets made and freed at
 * the sizes the window went through, and with the window, the
 * compositor's maximize, fullscreen, back and restore with the swapchain
 * remade at each -- none of which does it without the idle. Exit status 0
 * is a match every time, 1 a mismatch.
 *
 *   glslangValidator -V --vn stretch_vert tools/probe/stretch.vert \
 *       -o build/stretch_vert.h
 *   glslangValidator -V --vn nvkatlas_frag tools/probe/nvkatlas.frag \
 *       -o build/nvkatlas_frag.h
 *   gcc -O2 -Wall -Wextra -Ibuild -o build/nvkatlas \
 *       tools/probe/nvkatlas.c -lvulkan
 *
 * and with the window, xdg-shell's bindings from a linux tree:
 *
 *   G=build/linux/release/generated
 *   gcc -O2 -Wall -Wextra -DNVKATLAS_WAYLAND -Ibuild -I$G \
 *       -o build/nvkatlas tools/probe/nvkatlas.c $G/xdg-shell-protocol.c \
 *       -lvulkan -lwayland-client */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef NVKATLAS_WAYLAND
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include <vulkan/vulkan.h>
#include "stretch_vert.h"
#include "nvkatlas_frag.h"

#define ATLAS_W 1024
#define ATLAS_H 512

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s: %d\n", #x, r_); exit(2); } } while (0)

static VkDevice dev;
static VkPhysicalDevice pd;
static VkQueue q;
static VkCommandPool pool;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) &&
			(mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no memory type\n");
	exit(2);
}

typedef struct {
	VkImage img;
	VkDeviceMemory mem;
	VkImageView view;
} Img;

/* NVKATLAS_HOST=1 puts the atlas in the first memory type the image
 * allows that is not DEVICE_LOCAL -- system memory, out of the GPU's
 * reach when it powers off -- which is what the library does on this
 * driver (text.c). */
static bool place_host;

static Img make_image(VkFormat f, int w, int h, VkImageUsageFlags usage,
					  VkImageAspectFlags aspect)
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
	uint32_t type = mem_type(mr.memoryTypeBits,
							 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (place_host) {
		VkPhysicalDeviceMemoryProperties mp;
		vkGetPhysicalDeviceMemoryProperties(pd, &mp);
		printf("image memoryTypeBits 0x%x\n", mr.memoryTypeBits);
		for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
			printf("  type %u: heap %u, flags 0x%x\n", i,
				   mp.memoryTypes[i].heapIndex,
				   mp.memoryTypes[i].propertyFlags);
		uint32_t i = 0;
		while (i < mp.memoryTypeCount &&
			   (!(mr.memoryTypeBits & (1u << i)) ||
				(mp.memoryTypes[i].propertyFlags &
				 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
			i++;
		if (i == mp.memoryTypeCount) {
			fprintf(stderr, "no memory type outside VRAM for the image\n");
			exit(2);
		}
		printf("atlas in type %u\n", i);
		type = i;
	}
	VkMemoryAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = type };
	CK(vkAllocateMemory(dev, &ai, NULL, &r.mem));
	CK(vkBindImageMemory(dev, r.img, r.mem, 0));
	VkImageViewCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = r.img,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
		.subresourceRange = { aspect, 0, 1, 0, 1 } };
	CK(vkCreateImageView(dev, &vi, NULL, &r.view));
	return r;
}

static void free_image(Img *r)
{
	vkDestroyImageView(dev, r->view, NULL);
	vkDestroyImage(dev, r->img, NULL);
	vkFreeMemory(dev, r->mem, NULL);
}

typedef struct {
	VkBuffer buf;
	VkDeviceMemory mem;
	void *map;
} Buf;

static Buf make_buffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
	Buf b;
	VkBufferCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		.usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
	CK(vkCreateBuffer(dev, &ci, NULL, &b.buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, b.buf, &mr);
	VkMemoryAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = mem_type(mr.memoryTypeBits,
									VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
									VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	CK(vkAllocateMemory(dev, &ai, NULL, &b.mem));
	CK(vkBindBufferMemory(dev, b.buf, b.mem, 0));
	CK(vkMapMemory(dev, b.mem, 0, size, 0, &b.map));
	return b;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
					VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
		.oldLayout = from, .newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
		.subresourceRange = { aspect, 0, 1, 0, 1 } };
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
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	CK(vkBeginCommandBuffer(cb, &bi));
	return cb;
}

static void submit_wait(VkCommandBuffer cb)
{
	CK(vkEndCommandBuffer(cb));
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
						.commandBufferCount = 1, .pCommandBuffers = &cb };
	CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(q));
	vkFreeCommandBuffers(dev, pool, 1, &cb);
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

/* ---- the check: the atlas sampled into a target of its size ---------- */

static Img atlas, target;
/* The scene's: a second set whose one combined image sampler is pointed at
 * each new colour target, as the library's scene descriptor is when its
 * offscreen image is remade at a new size. */
static VkDescriptorSet scene_set;
static VkSampler scene_smp;
static Buf readback;
static VkRenderPass check_rp;
static VkFramebuffer check_fb;
static VkPipeline check_pipe;
static VkPipelineLayout check_pl;
static VkDescriptorSet check_set;
static uint8_t *pattern;

static void check_objects(void)
{
	VkAttachmentDescription att = {
		.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sp = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &ref };
	VkRenderPassCreateInfo rpci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &att, .subpassCount = 1,
		.pSubpasses = &sp };
	CK(vkCreateRenderPass(dev, &rpci, NULL, &check_rp));
	VkFramebufferCreateInfo fci = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = check_rp, .attachmentCount = 1,
		.pAttachments = &target.view, .width = ATLAS_W, .height = ATLAS_H,
		.layers = 1 };
	CK(vkCreateFramebuffer(dev, &fci, NULL, &check_fb));

	VkSamplerCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
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
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
	VkDescriptorPoolCreateInfo dpci = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2,
		.poolSizeCount = 1, .pPoolSizes = &ps };
	VkDescriptorPool dp;
	CK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
	VkDescriptorSetAllocateInfo dsai = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
	CK(vkAllocateDescriptorSets(dev, &dsai, &check_set));
	CK(vkAllocateDescriptorSets(dev, &dsai, &scene_set));
	VkSamplerCreateInfo lin = sci;
	lin.magFilter = lin.minFilter = VK_FILTER_LINEAR;
	CK(vkCreateSampler(dev, &lin, NULL, &scene_smp));
	VkDescriptorImageInfo dii = { smp, atlas.view,
								  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet w = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = check_set,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &dii };
	vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
	VkPipelineLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl };
	CK(vkCreatePipelineLayout(dev, &plci, NULL, &check_pl));

	VkShaderModule vs = shader(stretch_vert, sizeof stretch_vert);
	VkShaderModule fs = shader(nvkatlas_frag, sizeof nvkatlas_frag);
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
	VkViewport vp = { 0, 0, ATLAS_W, ATLAS_H, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { ATLAS_W, ATLAS_H } };
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
	VkPipelineColorBlendStateCreateInfo cbs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkGraphicsPipelineCreateInfo gp = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = st, .pVertexInputState = &vin,
		.pInputAssemblyState = &ia, .pViewportState = &vps,
		.pRasterizationState = &rs, .pMultisampleState = &ms,
		.pColorBlendState = &cbs, .layout = check_pl,
		.renderPass = check_rp };
	CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL,
								 &check_pipe));
}

/* How many texels of the atlas sample to something other than what was
 * uploaded. */
static long check(void)
{
	VkCommandBuffer cb = begin();
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = check_rp, .framebuffer = check_fb,
		.renderArea.extent = { ATLAS_W, ATLAS_H } };
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, check_pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, check_pl, 0,
							1, &check_set, 0, NULL);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	VkBufferImageCopy copy = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { ATLAS_W, ATLAS_H, 1 } };
	vkCmdCopyImageToBuffer(cb, target.img,
						   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						   readback.buf, 1, &copy);
	submit_wait(cb);
	const uint8_t *px = readback.map;
	long bad = 0;
	for (long i = 0; i < (long)ATLAS_W * ATLAS_H; i++)
		bad += px[4 * i] != pattern[i];
	return bad;
}

/* ---- what a resize did: targets at a size, cleared, the old ones gone -- */

typedef struct {
	Img colour, depth;
	VkRenderPass rp;
	VkFramebuffer fb;
	bool live;
} Targets;

static void make_targets(Targets *t, int w, int h)
{
	t->colour = make_image(VK_FORMAT_B8G8R8A8_UNORM, w, h,
						   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
						   VK_IMAGE_USAGE_SAMPLED_BIT |
						   VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
						   VK_IMAGE_ASPECT_COLOR_BIT);
	t->depth = make_image(VK_FORMAT_D32_SFLOAT, w, h,
						  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
						  VK_IMAGE_ASPECT_DEPTH_BIT);
	VkAttachmentDescription att[2] = {
		{ .format = VK_FORMAT_B8G8R8A8_UNORM,
		  .samples = VK_SAMPLE_COUNT_1_BIT,
		  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		  .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		  .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ .format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT,
		  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		  .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		  .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } };
	VkAttachmentReference cref = {
		0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference dref = {
		1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sp = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &cref,
		.pDepthStencilAttachment = &dref };
	VkRenderPassCreateInfo rpci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 2, .pAttachments = att, .subpassCount = 1,
		.pSubpasses = &sp };
	CK(vkCreateRenderPass(dev, &rpci, NULL, &t->rp));
	VkImageView views[2] = { t->colour.view, t->depth.view };
	VkFramebufferCreateInfo fci = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = t->rp, .attachmentCount = 2, .pAttachments = views,
		.width = (uint32_t)w, .height = (uint32_t)h, .layers = 1 };
	CK(vkCreateFramebuffer(dev, &fci, NULL, &t->fb));

	VkClearValue cv[2] = { { .color = { .float32 = { 0.1f, 0.2f, 0.3f, 1 } } },
						   { .depthStencil = { 1.0f, 0 } } };
	VkCommandBuffer cb = begin();
	VkRenderPassBeginInfo rbi = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = t->rp,
		.framebuffer = t->fb,
		.renderArea.extent = { (uint32_t)w, (uint32_t)h },
		.clearValueCount = 2, .pClearValues = cv };
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(cb);
	submit_wait(cb);
	t->live = true;

	if (getenv("NVKATLAS_NO_SCENE_SET"))
		return;
	VkDescriptorImageInfo dii = { scene_smp, t->colour.view,
								  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet wr = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = scene_set,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &dii };
	vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);
}

static void free_targets(Targets *t)
{
	if (!t->live)
		return;
	vkDestroyFramebuffer(dev, t->fb, NULL);
	vkDestroyRenderPass(dev, t->rp, NULL);
	free_image(&t->colour);
	free_image(&t->depth);
	t->live = false;
}

#ifdef NVKATLAS_WAYLAND
/* ---- the window: the compositor's states, a swapchain for each ------- */

static struct wl_display *wd;
static struct wl_compositor *wcomp;
static struct xdg_wm_base *wm;
static struct wl_surface *surf;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *top;
static int cfg_w, cfg_h, cfg_serial, configured;
static VkInstance inst;
static VkSurfaceKHR vsurf;
static VkSwapchainKHR chain;

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
	(void)d;
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_l = { .ping = wm_ping };

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
					   const char *iface, uint32_t ver)
{
	(void)d; (void)ver;
	if (!strcmp(iface, wl_compositor_interface.name))
		wcomp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm, &wm_l, NULL);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
	(void)d; (void)r; (void)name;
}
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w,
						  int32_t h, struct wl_array *states)
{
	(void)d; (void)t; (void)states;
	cfg_w = w;
	cfg_h = h;
}
static void top_close(void *d, struct xdg_toplevel *t)
{
	(void)d; (void)t;
}
static const struct xdg_toplevel_listener top_l = {
	.configure = top_configure, .close = top_close };

static void xs_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
	(void)d; (void)s;
	cfg_serial = (int)serial;
	configured = 1;
}
static const struct xdg_surface_listener xs_l = { .configure = xs_configure };

/* The next configure, answered with a swapchain of its size and a few
 * frames. Where the compositor leaves the size to the client it is the
 * library's own: 1280x720 and a 6-pixel grab ring, 1292x766. The ack goes
 * just before the first present at that size, as the library's does: a
 * frame of the old size still queued would otherwise be committed under
 * the new state. */
static void answer_configure(void)
{
	configured = 0;
	while (!configured)
		if (wl_display_dispatch(wd) < 0) {
			fprintf(stderr, "the compositor went away\n");
			exit(2);
		}
	const uint32_t w = cfg_w > 0 ? (uint32_t)cfg_w : 1292;
	const uint32_t h = cfg_h > 0 ? (uint32_t)cfg_h : 766;

	VkSurfaceCapabilitiesKHR caps;
	CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, vsurf, &caps));
	VkSwapchainCreateInfoKHR ci = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = vsurf, .minImageCount = caps.minImageCount < 2 ? 2
												: caps.minImageCount,
		.imageFormat = VK_FORMAT_B8G8R8A8_SRGB,
		.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
		.imageExtent = { w, h }, .imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
					  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
		.oldSwapchain = chain };
	VkSwapchainKHR fresh;
	CK(vkCreateSwapchainKHR(dev, &ci, NULL, &fresh));
	if (chain) {
		CK(vkQueueWaitIdle(q));
		vkDestroySwapchainKHR(dev, chain, NULL);
	}
	chain = fresh;

	uint32_t n = 0;
	CK(vkGetSwapchainImagesKHR(dev, chain, &n, NULL));
	VkImage imgs[16];
	if (n > 16)
		n = 16;
	CK(vkGetSwapchainImagesKHR(dev, chain, &n, imgs));
	VkSemaphoreCreateInfo sci = { .sType =
									  VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	for (int f = 0; f < 4; f++) {
		VkSemaphore got, done;
		VkFence fence;
		CK(vkCreateSemaphore(dev, &sci, NULL, &got));
		CK(vkCreateSemaphore(dev, &sci, NULL, &done));
		CK(vkCreateFence(dev, &fci, NULL, &fence));
		uint32_t i;
		VkResult r = vkAcquireNextImageKHR(dev, chain, UINT64_MAX, got,
										   VK_NULL_HANDLE, &i);
		if (r < 0) {
			fprintf(stderr, "acquire: %d\n", r);
			exit(2);
		}
		VkCommandBuffer cb = begin();
		barrier(cb, imgs[i], VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkClearColorValue c = { .float32 = { 0.1f * (float)f, 0.3f, 0.5f,
											 1.0f } };
		const VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT,
											  0, 1, 0, 1 };
		vkCmdClearColorImage(cb, imgs[i],
							 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1,
							 &all);
		barrier(cb, imgs[i], VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		CK(vkEndCommandBuffer(cb));
		VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo si = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
			.pWaitSemaphores = &got, .pWaitDstStageMask = &ws,
			.commandBufferCount = 1, .pCommandBuffers = &cb,
			.signalSemaphoreCount = 1, .pSignalSemaphores = &done };
		CK(vkQueueSubmit(q, 1, &si, fence));
		VkPresentInfoKHR pi = {
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.waitSemaphoreCount = 1, .pWaitSemaphores = &done,
			.swapchainCount = 1, .pSwapchains = &chain, .pImageIndices = &i };
		if (f == 0)
			xdg_surface_ack_configure(xsurf, (uint32_t)cfg_serial);
		r = vkQueuePresentKHR(q, &pi);
		if (r < 0) {
			fprintf(stderr, "present: %d\n", r);
			exit(2);
		}
		CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
		CK(vkQueueWaitIdle(q));
		vkFreeCommandBuffers(dev, pool, 1, &cb);
		vkDestroyFence(dev, fence, NULL);
		vkDestroySemaphore(dev, got, NULL);
		vkDestroySemaphore(dev, done, NULL);
		wl_display_roundtrip(wd);
	}
	printf("  %ux%u: %u images\n", w, h, n);
}

static int run_wayland(void)
{
	wd = wl_display_connect(NULL);
	if (!wd) {
		fprintf(stderr, "no Wayland display\n");
		return 2;
	}
	struct wl_registry *reg = wl_display_get_registry(wd);
	wl_registry_add_listener(reg, &reg_l, NULL);
	wl_display_roundtrip(wd);
	if (!wcomp || !wm) {
		fprintf(stderr, "no wl_compositor or xdg_wm_base\n");
		return 2;
	}
	surf = wl_compositor_create_surface(wcomp);
	xsurf = xdg_wm_base_get_xdg_surface(wm, surf);
	xdg_surface_add_listener(xsurf, &xs_l, NULL);
	top = xdg_surface_get_toplevel(xsurf);
	xdg_toplevel_add_listener(top, &top_l, NULL);
	xdg_toplevel_set_title(top, "nvkatlas");
	wl_surface_commit(surf);
	VkWaylandSurfaceCreateInfoKHR wci = {
		.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
		.display = wd, .surface = surf };
	CK(vkCreateWaylandSurfaceKHR(inst, &wci, NULL, &vsurf));

	answer_configure();
	long bad = check();
	printf("windowed: %ld of %d texels wrong\n", bad, ATLAS_W * ATLAS_H);
	int status = bad != 0;
	static const char *const STEP[] = {
		"maximize", "fullscreen", "leave fullscreen", "restore" };
	for (int i = 0; i < 4; i++) {
		switch (i) {
		case 0: xdg_toplevel_set_maximized(top); break;
		case 1: xdg_toplevel_set_fullscreen(top, NULL); break;
		case 2: xdg_toplevel_unset_fullscreen(top); break;
		case 3: xdg_toplevel_unset_maximized(top); break;
		}
		answer_configure();
		bad = check();
		printf("after %s: %ld wrong\n", STEP[i], bad);
		status |= bad != 0;
	}
	return status;
}
#endif

int main(int argc, char **argv)
{
	const bool window = argc > 1 && !strcmp(argv[1], "wayland");
	if (window) {
		argv++;
		argc--;
	}
	const char *want = argc > 1 ? argv[1] : "NVK";
	const int rounds = argc > 2 ? atoi(argv[2]) : 3;
#ifndef NVKATLAS_WAYLAND
	if (window) {
		fprintf(stderr, "built without NVKATLAS_WAYLAND\n");
		return 2;
	}
#endif

	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
							  .apiVersion = VK_API_VERSION_1_1 };
#ifdef NVKATLAS_WAYLAND
	static const char *const iext[] = { "VK_KHR_surface",
										"VK_KHR_wayland_surface" };
	static const char *const dext[] = { "VK_KHR_swapchain" };
#endif
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app };
#ifdef NVKATLAS_WAYLAND
	if (window) {
		ici.enabledExtensionCount = 2;
		ici.ppEnabledExtensionNames = iext;
	}
#else
	VkInstance inst;
#endif
	CK(vkCreateInstance(&ici, NULL, &inst));
	uint32_t np = 8;
	VkPhysicalDevice pds[8];
	vkEnumeratePhysicalDevices(inst, &np, pds);
	VkPhysicalDeviceProperties pp;
	for (uint32_t i = 0; i < np && !pd; i++) {
		vkGetPhysicalDeviceProperties(pds[i], &pp);
		if (strstr(pp.deviceName, want))
			pd = pds[i];
	}
	if (!pd) {
		fprintf(stderr, "no device named \"%s\"\n", want);
		return 2;
	}
	printf("%s\n", pp.deviceName);
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
							   .queueCreateInfoCount = 1,
							   .pQueueCreateInfos = &qci };
#ifdef NVKATLAS_WAYLAND
	if (window) {
		dci.enabledExtensionCount = 1;
		dci.ppEnabledExtensionNames = dext;
	}
#endif
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &q);
	VkCommandPoolCreateInfo pci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = 0 };
	CK(vkCreateCommandPool(dev, &pci, NULL, &pool));

	/* The atlas: a pattern no layout mistake keeps, uploaded once. */
	pattern = malloc((size_t)ATLAS_W * ATLAS_H);
	uint32_t s = 0x9e3779b9u;
	for (long i = 0; i < (long)ATLAS_W * ATLAS_H; i++) {
		s ^= s << 13; s ^= s >> 17; s ^= s << 5;
		pattern[i] = (uint8_t)(s >> 24);
	}
	place_host = getenv("NVKATLAS_HOST") != NULL;
	atlas = make_image(VK_FORMAT_R8_UNORM, ATLAS_W, ATLAS_H,
					   VK_IMAGE_USAGE_SAMPLED_BIT |
					   VK_IMAGE_USAGE_TRANSFER_DST_BIT,
					   VK_IMAGE_ASPECT_COLOR_BIT);
	place_host = false;
	Buf stage = make_buffer((VkDeviceSize)ATLAS_W * ATLAS_H,
							VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	memcpy(stage.map, pattern, (size_t)ATLAS_W * ATLAS_H);
	VkCommandBuffer cb = begin();
	barrier(cb, atlas.img, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy up = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { ATLAS_W, ATLAS_H, 1 } };
	vkCmdCopyBufferToImage(cb, stage.buf, atlas.img,
						   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
	barrier(cb, atlas.img, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	submit_wait(cb);

	target = make_image(VK_FORMAT_R8G8B8A8_UNORM, ATLAS_W, ATLAS_H,
						VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
						VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
						VK_IMAGE_ASPECT_COLOR_BIT);
	readback = make_buffer((VkDeviceSize)ATLAS_W * ATLAS_H * 4,
						   VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	check_objects();

#ifdef NVKATLAS_WAYLAND
	if (window)
		return run_wayland();
#endif
	long bad = check();
	printf("before: %ld of %d texels wrong\n", bad, ATLAS_W * ATLAS_H);
	int status = bad != 0;

	/* NVKATLAS_IDLE=s: the device left alone that long, then checked
	 * again -- long enough for nouveau's runtime PM to power the GPU off
	 * and on, which is what turned out to matter. */
	const char *idle = getenv("NVKATLAS_IDLE");
	if (idle && atoi(idle) > 0) {
		sleep((unsigned)atoi(idle));
		bad = check();
		printf("after %s s idle: %ld wrong\n", idle, bad);
		status |= bad != 0;
	}

	/* The window's sizes, in the order the round sends them. */
	static const int SIZES[][2] = {
		{ 1920, 1046 }, { 1920, 1080 }, { 1920, 1046 }, { 1280, 720 } };
	Targets cur = { 0 }, next = { 0 };
	make_targets(&cur, 1280, 720);
	for (int r = 0; r < rounds; r++) {
		for (size_t i = 0; i < sizeof SIZES / sizeof SIZES[0]; i++) {
			make_targets(&next, SIZES[i][0], SIZES[i][1]);
			free_targets(&cur);
			cur = next;
			next.live = false;
			bad = check();
			printf("round %d, after %dx%d: %ld wrong\n", r + 1, SIZES[i][0],
				   SIZES[i][1], bad);
			status |= bad != 0;
		}
	}
	free_targets(&cur);
	return status;
}

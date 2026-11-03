/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What the rest of the renderer builds with: the persisted pipeline cache,
 * shader modules, the one pipeline shape and the one render-pass shape the
 * library uses, memory types, attachment images, the per-image instance
 * buffers and the one-shot transfer.
 */

#include "core/app.h"

#include "renderer/vulkan/vk_internal.h"

/* The cache file's two host calls, as each C library names them. */
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define CACHE_MKDIR(path) _mkdir(path)
#define CACHE_GETPID()    _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define CACHE_MKDIR(path) mkdir((path), 0700)
#define CACHE_GETPID()    getpid()
#endif

/* ---- the cache (persisted so shader compiles are paid once) ------------ */

/* One file a device, named for the library rather than the caller and for
 * the device's own cache UUID: a driver keys its entries by shader hash,
 * so two Fenestal programs warm the same cache, and a caller's own
 * pipelines land in it through fnl_vulkan_gpu(). A driver takes only a
 * blob of its own UUID's, so one file for every device would hold the
 * last device's alone, and a window moved between two -- an NVIDIA GPU
 * between NVK and NVIDIA's driver -- would compile afresh at every move.
 *
 * `make_dir` creates the leaf directory, for the writer. Its result is
 * ignored: "already there" is the usual answer, and any other failure shows
 * at the fopen. */
static bool pipeline_cache_path(App *a, char *buf, size_t n, bool make_dir)
{
	const char *dir, *sub = "";
#ifdef _WIN32
	const char sep = '\\';
	dir = getenv("LOCALAPPDATA");
#else
	const char sep = '/';
	dir = getenv("XDG_CACHE_HOME");
	if (!dir || !dir[0]) {
		dir = getenv("HOME");
		sub = "/.cache";
	}
#endif
	if (!dir || !dir[0])
		return false;

	const int len = snprintf(buf, n, "%s%s", dir, sub);
	if (len < 0 || (size_t)len >= n)
		return false;
	if (make_dir)
		(void)CACHE_MKDIR(buf);
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(VK(a)->phys, &props);
	char uuid[2 * VK_UUID_SIZE + 1];
	for (size_t i = 0; i < VK_UUID_SIZE; i++)
		snprintf(uuid + 2 * i, sizeof uuid - 2 * i, "%02x",
				 props.pipelineCacheUUID[i]);
	const int more = snprintf(buf + len, n - (size_t)len,
							  "%cfenestal.pipeline_cache-%s", sep, uuid);
	return more >= 0 && (size_t)more < n - (size_t)len;
}

/* The blob read at startup, as a size and an FNV-1a hash rather than a kept
 * copy, so a run that compiled nothing new writes nothing back. File-scope:
 * the files are one a device, but one device at a time is live, and its
 * create_pipeline_cache() sets these for the file it read before anything
 * saves to it. */
static size_t   cache_loaded_size;
static uint64_t cache_loaded_hash;

static uint64_t blob_hash(const void *data, size_t n)
{
	const unsigned char *p = data;
	uint64_t h = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 0x100000001b3ull;
	return h;
}

FnlResult create_pipeline_cache(App *a)
{
	void *initial = NULL;
	size_t initial_size = 0;
	char path[512];
	if (pipeline_cache_path(a, path, sizeof path, false)) {
		FILE *f = fopen(path, "rb");
		if (f) {
			/* fseek and not rewind back to the start: rewind cannot say that
			 * it failed, and a read from wherever the stream was left is a
			 * cache of the wrong bytes. */
			long sz = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
			/* Bounded: the file is only semi-trusted, and the malloc happens
			 * before the driver validates the header. A real cache is a few
			 * MB. */
			if (sz > 0 && sz <= 64l * 1024 * 1024 &&
				fseek(f, 0, SEEK_SET) == 0) {
				initial = malloc((size_t)sz);
				if (initial && fread(initial, 1, (size_t)sz, f) == (size_t)sz)
					initial_size = (size_t)sz;
				else { free(initial); initial = NULL; }
			}
			fclose(f);
		}
	}
	cache_loaded_size = initial_size;
	cache_loaded_hash = blob_hash(initial, initial_size);

	VkPipelineCacheCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
		.initialDataSize = initial_size,
		.pInitialData = initial,
	};
	/* The read-back block is this function's, so no VK_TRY. */
	FnlResult rc = VKR(vkCreatePipelineCache(VK(a)->device, &ci, NULL,
											 &VK(a)->pipeline_cache));
	free(initial);
	return rc;
}

void save_pipeline_cache(App *a)
{
	if (VK(a)->pipeline_cache == VK_NULL_HANDLE)
		return;
	size_t sz = 0;
	if (vkGetPipelineCacheData(VK(a)->device, VK(a)->pipeline_cache, &sz,
							   NULL) != VK_SUCCESS || sz == 0)
		return;
	void *data = malloc(sz);
	if (!data)
		return;
	if (vkGetPipelineCacheData(VK(a)->device, VK(a)->pipeline_cache, &sz,
							   data) == VK_SUCCESS &&
		(sz != cache_loaded_size ||
		 blob_hash(data, sz) != cache_loaded_hash)) {
		/* Written beside the target and renamed over it: another Fenestal
		 * program shares the file, and a reader must find the old cache or
		 * the new one, never part of either. The pid keeps two writers from
		 * sharing the temporary as well. */
		char path[512], tmp[560];
		FILE *f = NULL;
		if (pipeline_cache_path(a, path, sizeof path, true) &&
			snprintf(tmp, sizeof tmp, "%s.%ld.tmp", path,
					 (long)CACHE_GETPID()) < (int)sizeof tmp)
			f = fopen(tmp, "wb");
		if (f) {
			/* Both calls are checked because fwrite can succeed into a buffer
			 * that fclose then fails to flush. */
			bool ok = fwrite(data, 1, sz, f) == sz;
			if (fclose(f) != 0)
				ok = false;
#ifdef _WIN32
			/* rename() does not replace an existing file there. */
			if (ok)
				remove(path);
#endif
			if (!ok || rename(tmp, path) != 0)
				remove(tmp);
		}
	}
	free(data);
}

/* ---- shaders and pipelines --------------------------------------------- */

VkShaderModule make_shader(App *a, const uint32_t *code, size_t bytes)
{
	VkShaderModuleCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = bytes,
		.pCode = code,
	};
	VkShaderModule mod;
	VK_TRY_NULL(vkCreateShaderModule(VK(a)->device, &ci, NULL, &mod));
	return mod;
}

/* All three overlay pipelines (solid rectangles, antialiased strokes, text)
 * want exactly the same fixed-function state and differ only in their shaders,
 * their instance stream and how they blend, so the state lives here once. */
VkPipeline make_overlay_pipeline(App *a, VkRenderPass pass,
								 VkShaderModule vert, VkShaderModule frag,
								 VkPipelineLayout layout, uint32_t stride,
								 const VkVertexInputAttributeDescription *va,
								 uint32_t n_va, OverlayBlend blend)
{
	/* make_shader() reports failure as VK_NULL_HANDLE, and passing one on is
	 * invalid usage (VUID-VkPipelineShaderStageCreateInfo-module-parameter)
	 * rather than an error returned. */
	if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_VERTEX_BIT,
		  .module = vert, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		  .module = frag, .pName = "main" },
	};

	/* One instance per primitive through this binding; the fetch and the
	 * filter pass n_va = 0 and nothing is bound. */
	VkVertexInputBindingDescription vb = {
		.binding = 0, .stride = stride,
		.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE };
	VkPipelineVertexInputStateCreateInfo vin = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = n_va ? 1u : 0u,
		.pVertexBindingDescriptions = n_va ? &vb : NULL,
		.vertexAttributeDescriptionCount = n_va,
		.pVertexAttributeDescriptions = n_va ? va : NULL };
	/* A strip for the quads -- the corners are the low two bits of the index
	 * -- and a list for the covering triangle. */
	VkPipelineInputAssemblyStateCreateInfo ia = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = n_va ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
						 : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkPipelineViewportStateCreateInfo vp = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .scissorCount = 1 };
	VkPipelineRasterizationStateCreateInfo rs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f };
	/* Every pass these run in is single-sampled. */
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };

	/* The pass carries a depth attachment, so the state is mandatory; the
	 * overlay must neither test nor write it. */
	VkPipelineDepthStencilStateCreateInfo ds = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_FALSE,
		.depthWriteEnable = VK_FALSE,
		.depthCompareOp = VK_COMPARE_OP_ALWAYS };

	/* Straight alpha for colour; on an _SRGB surface the hardware blends in
	 * linear, which the linearised palette (app_palette_init) expects. NONE is
	 * the solid rectangles: they stand in for vkCmdClearAttachments, so they
	 * write rather than blend. DUAL_SRC takes the weight per channel from the
	 * fragment shader's second output.
	 *
	 * Alpha is "over" as well -- ONE / ONE_MINUS_SRC_ALPHA, never ONE / ZERO.
	 * The surface is pre-multiplied while windowed, so the alpha channel is
	 * the window's transparency, and replacing it would stamp each fragment's
	 * coverage into it as a hole. The dual-source path takes it from the
	 * second output's alpha for the same reason: its first output is alpha 1.
	 * DESIGN *Composite alpha*. */
	const bool dual_src = (blend == OVERLAY_BLEND_DUAL_SRC);
	/* CONSTANT is the cover (vk_overlay_cover()): the whole frame read
	 * back before a move, weighted by one blend constant a draw and
	 * leaving the alpha channel as it is, which is opaque inside the
	 * window and the ring's zero outside it. */
	const bool constant = (blend == OVERLAY_BLEND_CONSTANT);
	VkPipelineColorBlendAttachmentState cba = {
		.blendEnable = blend != OVERLAY_BLEND_NONE,
		.srcColorBlendFactor = constant ? VK_BLEND_FACTOR_CONSTANT_ALPHA
							 : dual_src ? VK_BLEND_FACTOR_SRC1_COLOR
										: VK_BLEND_FACTOR_SRC_ALPHA,
		.dstColorBlendFactor = constant
								   ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA
							 : dual_src ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR
										: VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = constant ? VK_BLEND_FACTOR_ZERO
							 : dual_src ? VK_BLEND_FACTOR_SRC1_ALPHA
										: VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = constant ? VK_BLEND_FACTOR_ONE
							 : dual_src ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA
										: VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
						  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
	VkPipelineColorBlendStateCreateInfo cb = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };

	VkDynamicState dyn_states[3] = { VK_DYNAMIC_STATE_VIEWPORT,
									 VK_DYNAMIC_STATE_SCISSOR,
									 VK_DYNAMIC_STATE_BLEND_CONSTANTS };
	VkPipelineDynamicStateCreateInfo dyn = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = constant ? 3u : 2u,
		.pDynamicStates = dyn_states };

	VkGraphicsPipelineCreateInfo gci = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = stages,
		.pVertexInputState = &vin,
		.pInputAssemblyState = &ia,
		.pViewportState = &vp,
		.pRasterizationState = &rs,
		.pMultisampleState = &ms,
		.pDepthStencilState = &ds,
		.pColorBlendState = &cb,
		.pDynamicState = &dyn,
		.layout = layout,
		.renderPass = pass,
		.subpass = 0 };

	VkPipeline pipeline;
	VK_TRY_NULL(vkCreateGraphicsPipelines(VK(a)->device, VK(a)->pipeline_cache,
										  1, &gci, NULL, &pipeline));
	return pipeline;
}

/* ---- memory ------------------------------------------------------------- */

/* The index comes back through `out` because every uint32_t is a legal index.
 * The failure returns FNL_ERR_NO_DEVICE by name rather than fnl_fail()'s
 * result: only a constant lets the compiler pair FNL_OK with the write to
 * *out, and the callers otherwise warn -Wmaybe-uninitialized. fnl_fail() is
 * warn_unused_result, which a cast to void does not satisfy in GCC, hence the
 * bound `rc`. */
FnlResult find_memory_type(App *a, uint32_t bits, VkMemoryPropertyFlags props,
						   uint32_t *out)
{
	const VkPhysicalDeviceMemoryProperties *mp = &VK(a)->mem_props;
	for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
		if ((bits & (1u << i)) &&
			(mp->memoryTypes[i].propertyFlags & props) == props) {
			*out = i;
			return FNL_OK;
		}
	const FnlResult rc =
		fnl_fail(FNL_ERR_NO_DEVICE,
				 "no memory type matches bits 0x%x with properties 0x%x",
				 bits, (unsigned)props);
	(void)rc;
	return FNL_ERR_NO_DEVICE;
}

/* The first lazily allocated memory type among `bits`. A desktop GPU has
 * none; a tiler keeps a never-stored attachment in it. */
static bool lazy_memory_type(const App *a, uint32_t bits, uint32_t *type_out)
{
	const VkPhysicalDeviceMemoryProperties *mp = &VK(a)->mem_props;
	for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
		if ((bits & (1u << i)) &&
			(mp->memoryTypes[i].propertyFlags &
			 VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) {
			*type_out = i;
			return true;
		}
	return false;
}

/* ---- images, views, framebuffers ---------------------------------------- */

VkResult make_image_view(App *a, VkImage image, VkFormat fmt,
						 VkImageAspectFlags aspect, VkImageView *out)
{
	VkImageViewCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = fmt,
		.subresourceRange = {
			.aspectMask = aspect,
			.levelCount = 1,
			.layerCount = 1,
		},
	};
	VkResult r = vkCreateImageView(VK(a)->device, &ci, NULL, out);
	if (r != VK_SUCCESS)
		*out = VK_NULL_HANDLE;     /* undefined on failure, so say it */
	return r;
}

VkResult make_framebuffer(App *a, VkRenderPass pass, uint32_t n_views,
						  const VkImageView *views, uint32_t w, uint32_t h,
						  VkFramebuffer *out)
{
	VkFramebufferCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = pass,
		.attachmentCount = n_views,
		.pAttachments = views,
		.width = w, .height = h, .layers = 1,
	};
	VkResult r = vkCreateFramebuffer(VK(a)->device, &ci, NULL, out);
	if (r != VK_SUCCESS)
		*out = VK_NULL_HANDLE;
	return r;
}

void destroy_target_image(App *a, TargetImage *t)
{
	vkDestroyImageView(VK(a)->device, t->view, NULL);
	vkDestroyImage(VK(a)->device, t->image, NULL);
	vkFreeMemory(VK(a)->device, t->mem, NULL);
	t->view  = VK_NULL_HANDLE;
	t->image = VK_NULL_HANDLE;
	t->mem   = VK_NULL_HANDLE;
}

/* The device is asked twice for lazy memory: with every type allowed, to
 * decide whether the transient usage bit is worth adding, and again against
 * the memoryTypeBits the image came back with.
 *
 * Quiet on failure: this is the allocation a user gesture can make arbitrarily
 * large, and a refusal is answered by drawing without the target. */
bool create_target_image(App *a, uint32_t w, uint32_t h, VkFormat fmt,
						 VkSampleCountFlagBits samples,
						 VkImageUsageFlags usage, bool lazy, TargetImage *t)
{
	const bool depth =
		(usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
	uint32_t type = 0;
	const bool want_lazy = lazy && lazy_memory_type(a, ~0u, &type);

	t->image = VK_NULL_HANDLE;
	t->mem   = VK_NULL_HANDLE;
	t->view  = VK_NULL_HANDLE;

	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = fmt,
		.extent = { w, h, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = samples,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage | (want_lazy ? VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT
									: 0u),
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vkCreateImage(VK(a)->device, &ici, NULL, &t->image) != VK_SUCCESS) {
		t->image = VK_NULL_HANDLE;   /* undefined on failure, so say it */
		return false;
	}

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(VK(a)->device, t->image, &req);
	/* No matching memory type at all is a device that cannot run this, not
	 * one out of room -- but every caller already copes with a refusal, so it
	 * joins that path. */
	bool ok = (want_lazy && lazy_memory_type(a, req.memoryTypeBits, &type)) ||
			  find_memory_type(a, req.memoryTypeBits,
							   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) >= 0;
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = type,
	};
	if (ok) {
		ok = vkAllocateMemory(VK(a)->device, &mai, NULL, &t->mem) == VK_SUCCESS;
		if (!ok)
			t->mem = VK_NULL_HANDLE;
	}
	if (ok)
		ok = vkBindImageMemory(VK(a)->device, t->image, t->mem, 0) ==
			 VK_SUCCESS;
	if (ok)
		ok = make_image_view(a, t->image, fmt,
							 depth ? VK_IMAGE_ASPECT_DEPTH_BIT
								   : VK_IMAGE_ASPECT_COLOR_BIT,
							 &t->view) == VK_SUCCESS;
	if (!ok)
		destroy_target_image(a, t);
	return ok;
}

/* ---- render passes ------------------------------------------------------- */

/* The renderer's three passes as one shape with four switches: the window's
 * (depth, cleared, presented), the scene's (depth, cleared, sampled, at one
 * sample or several) and MLAA's (colour only, overwritten, sampled). A
 * pipeline is only compatible with the pass these produce, dependencies
 * included, so the masks below are not to be tidied. A multisampled colour is
 * never stored: the resolve is what leaves the pass.
 *
 * The dependency in orders this frame's first writes after what the last
 * frame did with the same attachments:
 *
 *   the fetch that read a sampled result      FRAGMENT_SHADER / SHADER_READ
 *   the last pass's colour writes, if cleared COLOR_ATTACHMENT_OUTPUT / WRITE
 *   the last pass's depth writes              the two FRAGMENT_TESTS / WRITE
 *
 * The depth line is what lets one depth image serve every frame in flight;
 * without it sync validation reports WRITE_AFTER_WRITE on the clear. The
 * dependency out, for a sampled result, puts this frame's colour writes --
 * the resolve included -- before whatever fetches them. */
VkRenderPass make_render_pass(App *a, const PassDesc *d)
{
	const bool ms = d->samples != VK_SAMPLE_COUNT_1_BIT;
	const VkImageLayout result =
		d->sampled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				   : vk_window_layout(a);

	VkAttachmentDescription attachments[3];
	uint32_t n = 0;

	attachments[n++] = (VkAttachmentDescription){
		.format = VK(a)->surf_format.format,
		.samples = d->samples,
		.loadOp = d->clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
						   : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.storeOp = ms ? VK_ATTACHMENT_STORE_OP_DONT_CARE
					  : VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = ms ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : result,
	};
	VkAttachmentReference color_ref = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	VkAttachmentReference depth_ref = {
		.attachment = n,
		.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	};
	if (d->depth)
		attachments[n++] = (VkAttachmentDescription){
			.format = VK(a)->depth_format,
			.samples = d->samples,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		};

	VkAttachmentReference resolve_ref = {
		.attachment = n,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	if (ms)
		attachments[n++] = (VkAttachmentDescription){
			.format = VK(a)->surf_format.format,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,   /* fully written */
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = result,
		};

	VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color_ref,
		.pResolveAttachments = ms ? &resolve_ref : NULL,
		.pDepthStencilAttachment = d->depth ? &depth_ref : NULL,
	};

	const VkPipelineStageFlags depth_stages =
		d->depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
				   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
				 : 0u;
	const VkAccessFlags depth_write =
		d->depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0u;
	VkSubpassDependency deps[2] = {
		{
			.srcSubpass = VK_SUBPASS_EXTERNAL,
			.dstSubpass = 0,
			.srcStageMask =
				(d->sampled ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0u) |
				(d->clear ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
						  : 0u) |
				depth_stages,
			.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
							depth_stages,
			.srcAccessMask =
				(d->sampled ? VK_ACCESS_SHADER_READ_BIT : 0u) |
				(d->clear ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0u) |
				depth_write,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
							 depth_write,
		},
		{
			.srcSubpass = 0,
			.dstSubpass = VK_SUBPASS_EXTERNAL,
			.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		},
	};
	VkRenderPassCreateInfo ci = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = n,
		.pAttachments = attachments,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = d->sampled ? 2u : 1u,
		.pDependencies = deps,
	};
	VkRenderPass pass;
	VK_TRY_NULL(vkCreateRenderPass(VK(a)->device, &ci, NULL, &pass));
	return pass;
}

/* ---- instance buffers --------------------------------------------------- */

/* One per swapchain image for each overlay stream: host-visible, coherent and
 * mapped for the life of the device, so recording writes instances straight in
 * and the submit publishes them. Device-scoped, not swapchain-scoped: a
 * rebuild's retired command buffers still name them.
 *
 * One allocation and one mapping for the set, strided by the buffers'
 * alignment. Unwinds through one label, since each call leaves the ones
 * before it owned by nobody. */
FnlResult create_instance_buffers(App *a, VkDeviceSize bytes,
								  VkBufferUsageFlags usage,
								  VkMemoryPropertyFlags prefer, uint32_t count,
								  VkBuffer *buf, VkDeviceMemory *mem,
								  void **map)
{
	FnlResult rc;
	uint32_t  type;

	/* Cleared first so the failure path below can be the ordinary destroy: it
	 * reads all three to decide what there is to release. */
	*mem = VK_NULL_HANDLE;
	for (uint32_t i = 0; i < count; i++) {
		buf[i] = VK_NULL_HANDLE;
		map[i] = NULL;
	}
	/* The requirements below are read off buf[0]. */
	if (count == 0)
		return FNL_OK;

	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = bytes,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	/* A handle a failed call wrote is undefined, so it is nulled before the
	 * destroy reads it. */
	for (uint32_t i = 0; i < count; i++)
		if ((rc = VKR(vkCreateBuffer(VK(a)->device, &bci, NULL,
									 &buf[i]))) < 0) {
			buf[i] = VK_NULL_HANDLE;
			goto fail;
		}

	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(VK(a)->device, buf[0], &req);
	const VkDeviceSize stride =
		(req.size + req.alignment - 1) / req.alignment * req.alignment;

	/* The first host-visible, coherent type is the one a CPU writes best,
	 * and on a discrete GPU often the window onto its memory, which the CPU
	 * reads uncached: on NVK 541 ms for a 1920x1080 frame against 0.7 from
	 * its cached system memory (DESIGN *KMS: no window server*). So a
	 * buffer read back asks for that first, and takes the other where the
	 * device has none. Not a non-coherent cached type: nothing here
	 * invalidates. Asked by hand, since find_memory_type() reports a miss
	 * as an error and this one is not. */
	const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
									   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	/* memoryTypeCount is "none preferred": the state lives in type itself,
	 * so every path to its use visibly assigns it. */
	const uint32_t none = VK(a)->mem_props.memoryTypeCount;
	type = none;
	for (uint32_t i = 0; prefer && type == none && i < none; i++)
		if ((req.memoryTypeBits & (1u << i)) &&
			(VK(a)->mem_props.memoryTypes[i].propertyFlags & (host | prefer)) ==
				(host | prefer))
			type = i;
	if (type == none &&
		(rc = find_memory_type(a, req.memoryTypeBits, host, &type)) < 0)
		goto fail;

	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = stride * count,
		.memoryTypeIndex = type,
	};
	if ((rc = VKR(vkAllocateMemory(VK(a)->device, &mai, NULL, mem))) < 0) {
		*mem = VK_NULL_HANDLE;
		goto fail;
	}
	for (uint32_t i = 0; i < count; i++)
		if ((rc = VKR(vkBindBufferMemory(VK(a)->device, buf[i], *mem,
										 stride * i))) < 0)
			goto fail;

	/* Sliced by the stride the binds used, so map[i] is buf[i] from its own
	 * offset zero. Aligned for the instance structs: the mapped base carries
	 * minMemoryMapAlignment and the stride is a multiple of the buffers'
	 * alignment, both multiples of a float's four bytes. */
	void *base;
	if ((rc = VKR(vkMapMemory(VK(a)->device, *mem, 0, stride * count, 0,
							  &base))) < 0)
		goto fail;
	for (uint32_t i = 0; i < count; i++)
		map[i] = (unsigned char *)base + stride * i;
	return FNL_OK;

fail:
	destroy_instance_buffers(a, count, buf, mem, map);
	return rc;
}

void destroy_instance_buffers(App *a, uint32_t count, VkBuffer *buf,
							  VkDeviceMemory *mem, void **map)
{
	/* Gated on the mapping, not the memory, so this doubles as the create's
	 * failure path: an allocation never mapped must not be unmapped. map[0]
	 * answers for the set because there is one mapping. */
	if (count && map[0]) {
		vkUnmapMemory(VK(a)->device, *mem);
		for (uint32_t i = 0; i < count; i++)
			map[i] = NULL;
	}
	if (*mem != VK_NULL_HANDLE) {
		vkFreeMemory(VK(a)->device, *mem, NULL);
		*mem = VK_NULL_HANDLE;
	}
	for (uint32_t i = 0; i < count; i++) {
		vkDestroyBuffer(VK(a)->device, buf[i], NULL);
		buf[i] = VK_NULL_HANDLE;
	}
}

/* ---- one-shot transfers ------------------------------------------------ */

/* Synchronous: allocate, record, submit, wait, free. Only the atlas upload
 * uses it, so the queue stall is the simplest correct thing. */
VkCommandBuffer begin_oneshot(App *a)
{
	VkCommandBufferAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = VK(a)->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer cb = VK_NULL_HANDLE;
	VK_TRY_NULL(vkAllocateCommandBuffers(VK(a)->device, &ai, &cb));
	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	if (VKR(vkBeginCommandBuffer(cb, &bi)) < 0) {
		/* Back to the pool, which outlives this failure and reclaims nothing
		 * on its own. */
		vkFreeCommandBuffers(VK(a)->device, VK(a)->cmd_pool, 1, &cb);
		return VK_NULL_HANDLE;
	}
	return cb;
}

/* Frees the buffer on every path, which is why this is not written with
 * VK_TRY: the pool lives as long as the device. */
FnlResult end_oneshot(App *a, VkCommandBuffer cb)
{
	FnlResult rc = VKR(vkEndCommandBuffer(cb));

	if (rc >= 0) {
		VkSubmitInfo si = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers = &cb,
		};
		rc = VKR(vkQueueSubmit(VK(a)->queue, 1, &si, VK_NULL_HANDLE));
		if (rc >= 0)
			rc = VKR(vkQueueWaitIdle(VK(a)->queue));
	}
	vkFreeCommandBuffers(VK(a)->device, VK(a)->cmd_pool, 1, &cb);
	return rc;
}

/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The window's scene, as a Fenestal client draws it: two pipelines and two
 * draws -- the test pattern, or the other scene a second window draws, which
 * are the fragment shader's two pictures chosen by the push block's content.
 *
 * This file does not know whether the scene is drawn straight into the window
 * or into a smaller image scaled back up, whether it is multisampled, whether
 * a morphological filter runs afterwards, or how the frame is paced. It builds
 * one pipeline per target the library says it might see, and records two draws
 * into whatever it is handed.
 *
 * The one thing it must get right is compatibility: a Vulkan pipeline belongs
 * to the render pass it was built against and to that pass's sample count, and
 * the three targets agree on neither. fnl_vulkan_scene_layout() says which,
 * per target.
 */

/* No Vulkan prototypes: every entry is asked of the library's loader below,
 * and one called without being asked is a compile error, not a link. */
#define VK_NO_PROTOTYPES

#include "testbed.h"

#include <fenestal/fnl_vulkan.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scene.vert.h"
#include "scene.frag.h"
#include "star.vert.h"
#include "star.frag.h"

/* Every Vulkan entry this file calls, each a pointer of the function's own
 * name, asked of FnlVulkanGpu::get_instance_proc_addr in load_vulkan(). Not
 * linked: a program that imports the loader does not start where there is
 * none, and the library would have drawn with OpenGL or Direct3D 12 there. */
#define SCENE_VK_FNS(X)                                                       \
	X(vkCmdBindPipeline)                                                      \
	X(vkCmdDraw)                                                              \
	X(vkCmdPushConstants)                                                     \
	X(vkCreateGraphicsPipelines)                                              \
	X(vkCreatePipelineLayout)                                                 \
	X(vkCreateShaderModule)                                                   \
	X(vkDestroyPipeline)                                                      \
	X(vkDestroyPipelineLayout)                                                \
	X(vkDestroyShaderModule)                                                  \
	X(vkEnumerateDeviceExtensionProperties)

#define X_DEFINE(f) static PFN_##f f;
SCENE_VK_FNS(X_DEFINE)
#undef X_DEFINE

/* Whether every pointer above is this instance's. */
static bool g_loaded;

/* Asked of the instance after every on_gpu_created, since the instance is
 * new after every loss. The device's entries come back as the loader's
 * trampolines, which find the device's driver through its handle. */
static bool load_vulkan(const FnlVulkanGpu *gpu)
{
	g_loaded = false;
	if (!gpu->get_instance_proc_addr)
		return false;
#define X_LOAD(f)                                                             \
	f = (PFN_##f)gpu->get_instance_proc_addr(gpu->instance, #f);              \
	if (!f)                                                                   \
		return false;
	SCENE_VK_FNS(X_LOAD)
#undef X_LOAD
	g_loaded = true;
	return true;
}

/* The star's draw: 36 lit wedges, two triangles each, all computed from
 * gl_VertexIndex in testbed/shaders/star.vert. */
#define STAR_VERTICES (36 * 2 * 3)

/* This window's view of the pattern: the part of the canvas it shows (origin
 * and canvas, the same for both being a window showing the whole of it), the
 * colour its own scene is, and its phase. The sRGB flag tells the fragment
 * stage which space this surface's blend works in; jitter is the temporal
 * accumulation's subpixel offset, applied in the vertex stages. The asserts
 * pin this struct to the block scene.frag declares. */
typedef struct {
	float    size[2];
	float    jitter[2];
	float    canvas[2];
	float    origin[2];
	float    tint[4];
	float    time;
	float    time_bias;
	uint32_t srgb;
	uint32_t content;
	/* x: what light authored relative to SDR white is in the target's unit
	 * (FnlOutputColor::sdr_scale); y: how far above SDR white the output
	 * goes (its peak); zw unused. A vec4 so the block's size stays a
	 * multiple of 16 for std140's and HLSL's sake. */
	float    colour[4];
} ScenePush;

_Static_assert(sizeof(ScenePush) == 80, "ScenePush must match scene.frag");
_Static_assert(offsetof(ScenePush, jitter) == 8, "ScenePush layout drifted");
_Static_assert(offsetof(ScenePush, canvas) == 16, "ScenePush layout drifted");
_Static_assert(offsetof(ScenePush, origin) == 24, "ScenePush layout drifted");
_Static_assert(offsetof(ScenePush, tint) == 32, "ScenePush layout drifted");
_Static_assert(offsetof(ScenePush, time) == 48, "ScenePush layout drifted");
_Static_assert(offsetof(ScenePush, time_bias) == 52,
			   "ScenePush layout drifted");
_Static_assert(offsetof(ScenePush, srgb) == 56, "ScenePush layout drifted");
_Static_assert(offsetof(ScenePush, content) == 60, "ScenePush layout drifted");
_Static_assert(offsetof(ScenePush, colour) == 64, "ScenePush layout drifted");

typedef struct {
	VkDevice         device;
	VkPipelineLayout layout;
	/* One pair per target. A null entry is a target this window cannot use --
	 * the multisampled one on a device with no sample count worth having --
	 * and the frame will never ask for it. */
	VkPipeline       pattern[FNL_TARGET_COUNT];
	VkPipeline       star[FNL_TARGET_COUNT];
	bool             srgb;
} Scene;

/* The scene is one per window, so that a second window on the host -- with its
 * own device and pipelines -- draws its own rather than overwriting the
 * first's. Keyed by the App; the library hands on_gpu_created and on_gpu_lost
 * each a window's own, so the slot is taken and given with them. */
#define SCENE_WINDOWS 8
static struct {
	FnlApp *app;
	Scene s;
} g_scenes[SCENE_WINDOWS];

/* The table is written on the main thread -- scene_open() and scene_close()
 * run from on_gpu_created and on_gpu_lost -- and read on each window's render
 * thread by scene_slot() (on_record), so one spinlock covers every scan and
 * write, as the testbed's picture table has. */
static atomic_flag g_scenes_lock;
static void scenes_lock(void)
{
	while (atomic_flag_test_and_set(&g_scenes_lock))
		;
}
static void scenes_unlock(void)
{
	atomic_flag_clear(&g_scenes_lock);
}

static Scene *scene_slot(FnlApp *app)
{
	Scene *s = NULL;
	scenes_lock();
	for (int i = 0; i < SCENE_WINDOWS && !s; i++)
		if (g_scenes[i].app == app)
			s = &g_scenes[i].s;
	scenes_unlock();
	return s;
}

/* The window's slot, emptied; a new one when it had none. NULL when every
 * slot is taken, which is more windows than the testbed draws a scene in. */
static Scene *scene_open(FnlApp *app)
{
	Scene *s = NULL;
	scenes_lock();
	for (int i = 0; i < SCENE_WINDOWS && !s; i++) {
		if (g_scenes[i].app == app || !g_scenes[i].app) {
			g_scenes[i].app = app;
			memset(&g_scenes[i].s, 0, sizeof g_scenes[i].s);
			s = &g_scenes[i].s;
		}
	}
	scenes_unlock();
	return s;
}

static void scene_close(FnlApp *app)
{
	int at = -1;
	scenes_lock();
	for (int i = 0; i < SCENE_WINDOWS; i++)
		if (g_scenes[i].app == app) {
			at = i;
			break;
		}
	scenes_unlock();
	if (at < 0)
		return;
	Scene *s = &g_scenes[at].s;
	if (s->device) {
		for (int i = 0; i < FNL_TARGET_COUNT; i++) {
			vkDestroyPipeline(s->device, s->pattern[i], NULL);
			vkDestroyPipeline(s->device, s->star[i], NULL);
		}
		vkDestroyPipelineLayout(s->device, s->layout, NULL);
	}
	memset(s, 0, sizeof *s);
	scenes_lock();
	g_scenes[at].app = NULL;
	scenes_unlock();
}

/* Whether the target holds linear light: the three _SRGB formats the library
 * picks a surface from, whose writes the hardware encodes, and the float one
 * HDR's target is, which holds it as written. Either way the shader writes
 * linear values and encodes nothing itself. */
static bool format_is_linear(VkFormat f)
{
	switch (f) {
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
	case VK_FORMAT_R16G16B16A16_SFLOAT:
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
		.codeSize = bytes,
		.pCode = code,
	};
	VkShaderModule m = VK_NULL_HANDLE;
	if (vkCreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return m;
}

/* Everything about these two pipelines is fixed except which pass they are for
 * and at what sample count. No vertex input at all -- both shaders build their
 * geometry from gl_VertexIndex -- no depth test, no blending, and a dynamic
 * viewport and scissor, because the library sets both before handing over. */
static VkPipeline make_pipeline(const FnlVulkanGpu *gpu,
								const FnlVulkanSceneLayout *layout,
								VkPipelineLayout pl, VkShaderModule vert,
								VkShaderModule frag)
{
	/* make_module() reports failure as a null module, and handing one to
	 * vkCreateGraphicsPipelines is invalid usage rather than an error it
	 * returns. Turned into the null pipeline every caller already copes
	 * with. */
	if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert,
		  .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		  .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag,
		  .pName = "main" },
	};
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
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f };
	VkPipelineMultisampleStateCreateInfo ms = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = layout->samples };
	VkPipelineDepthStencilStateCreateInfo ds = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	/* Writes rather than blends: the pattern fills what it is given and the
	 * star covers it. Blending either against the load-op clear would only
	 * darken it. */
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
		.layout = pl,
		.renderPass = layout->render_pass,
		.subpass = layout->subpass,
	};
	VkPipeline p = VK_NULL_HANDLE;
	/* The library's cache, shared deliberately: a caller that made its own
	 * would pay the first compile twice over across runs. */
	if (vkCreateGraphicsPipelines(gpu->device, gpu->pipeline_cache, 1, &gci,
								  NULL, &p) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return p;
}

void scene_vulkan_on_gpu_created(void *ud, FnlApp *app)
{
	(void)ud;
	Scene *s = scene_open(app);
	if (!s) {
		fprintf(stderr, "more scene windows than the testbed draws; "
						"no scene will be drawn\n");
		return;
	}

	FnlVulkanGpu gpu;
	if (!fnl_vulkan_gpu(app, &gpu)) {
		fprintf(stderr, "the running renderer is not Vulkan; "
						"no scene will be drawn\n");
		scene_close(app);
		return;
	}
	if (!load_vulkan(&gpu)) {
		fprintf(stderr, "the Vulkan loader is missing an entry the scene "
						"calls; no scene will be drawn\n");
		scene_close(app);
		return;
	}
	s->device = gpu.device;

	VkPushConstantRange pcr = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
					  VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0, .size = sizeof(ScenePush) };
	VkPipelineLayoutCreateInfo plci = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
	if (vkCreatePipelineLayout(s->device, &plci, NULL, &s->layout)
		!= VK_SUCCESS) {
		/* Vulkan leaves an output handle undefined on failure, and
		 * scene_vulkan_on_gpu_lost() runs next: it is gated on s->device, so
		 * clearing that is what stops it destroying a layout nobody wrote. */
		s->layout = VK_NULL_HANDLE;
		s->device = VK_NULL_HANDLE;
		fprintf(stderr, "could not create the scene's pipeline layout\n");
		scene_close(app);
		return;
	}

	VkShaderModule sv = make_module(s->device, scene_vert_spv,
									sizeof scene_vert_spv);
	VkShaderModule sf = make_module(s->device, scene_frag_spv,
									sizeof scene_frag_spv);
	VkShaderModule tv = make_module(s->device, star_vert_spv,
									sizeof star_vert_spv);
	VkShaderModule tf = make_module(s->device, star_frag_spv,
									sizeof star_frag_spv);

	for (int i = 0; i < FNL_TARGET_COUNT; i++) {
		FnlVulkanSceneLayout layout;
		if (!fnl_vulkan_scene_layout(app, (FnlSceneTarget)i, &layout))
			continue;          /* this window cannot use that target */
		/* Whether the target holds linear light -- hardware-encoded _SRGB,
		 * or a float format that keeps it -- which decides whether the shader
		 * must encode on output. Read off the format the pipeline needs
		 * anyway; there is no accessor for it. */
		if (i == FNL_TARGET_DIRECT)
			s->srgb = format_is_linear(layout.color_format);
		s->pattern[i] = make_pipeline(&gpu, &layout, s->layout, sv, sf);
		s->star[i]    = make_pipeline(&gpu, &layout, s->layout, tv, tf);
	}

	vkDestroyShaderModule(s->device, sv, NULL);
	vkDestroyShaderModule(s->device, sf, NULL);
	vkDestroyShaderModule(s->device, tv, NULL);
	vkDestroyShaderModule(s->device, tf, NULL);
}

void scene_vulkan_on_gpu_lost(void *ud, FnlApp *app)
{
	(void)ud;
	scene_close(app);
}

/* The scene itself: the pattern's background over one covering triangle, then
 * the star's wedges as actual geometry on top of it. Two draws rather than one
 * because the wedges have to be *primitives* for FSAA to have any coverage to
 * sample -- see testbed/shaders/star.vert. No vertex buffer is
 * bound. */
void scene_vulkan_on_record(void *ud, FnlApp *app, const FnlFrame *f)
{
	(void)ud;
	Scene *s = scene_slot(app);
	if (!s)
		return;

	VkCommandBuffer cb;
	if (!fnl_vulkan_scene_cmd(app, &cb))
		return;
	/* Both pipelines: they are built as a pair and either can fail on its
	 * own, and binding a null one is invalid usage rather than a draw that
	 * does nothing. */
	if (f->target < 0 || f->target >= FNL_TARGET_COUNT ||
		s->pattern[f->target] == VK_NULL_HANDLE ||
		s->star[f->target] == VK_NULL_HANDLE)
		return;

	TestbedViewSpec view;
	testbed_scene_view(app, f, &view);
	ScenePush pc = {
		.size = { view.size[0], view.size[1] },
		.jitter = { f->jitter_x, f->jitter_y },
		.canvas = { view.canvas[0], view.canvas[1] },
		.origin = { view.origin[0], view.origin[1] },
		.tint = { view.tint[0], view.tint[1], view.tint[2], view.tint[3] },
		.time = testbed_scene_time(f),
		.time_bias = view.time_bias,
		.srgb = s->srgb ? 1u : 0u,
		.content = view.content,
		.colour = { f->color.sdr_scale, f->color.peak, 0.0f, 0.0f },
	};
	vkCmdPushConstants(cb, s->layout,
					   VK_SHADER_STAGE_VERTEX_BIT |
					   VK_SHADER_STAGE_FRAGMENT_BIT,
					   0, sizeof pc, &pc);

	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
					  s->pattern[f->target]);
	vkCmdDraw(cb, 3, 1, 0, 0);

	/* The other scene is one picture and has no star; the star's wedges are
	 * the pattern's alone. */
	if (view.content == 0u) {
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
						  s->star[f->target]);
		vkCmdDraw(cb, STAR_VERTICES, 1, 0, 0);
	}
}

/* ---- the GPU memory in use ---------------------------------------------- */

/* Whether the device lists the extension `name`, the list read whole at the
 * count it reports: a driver lists over two hundred, and one cut at a fixed
 * length would lose the ones that fell past it. */
static bool device_lists(VkPhysicalDevice pd, const char *name)
{
	uint32_t n = 0;
	if (vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS
			|| n == 0)
		return false;
	VkExtensionProperties *ext = calloc(n, sizeof *ext);
	if (!ext)
		return false;
	bool has = false;
	if (vkEnumerateDeviceExtensionProperties(pd, NULL, &n, ext) >= 0)
		for (uint32_t i = 0; i < n && !has; i++)
			has = strcmp(ext[i].extensionName, name) == 0;
	free(ext);
	return has;
}

/* VK_EXT_memory_budget's heap usage, this process's, over the device-local
 * heaps -- on an integrated GPU the system's memory, which is what it draws
 * from. Read through VK_KHR_get_physical_device_properties2: the library
 * makes a 1.0 instance and enables that extension where it is offered, so
 * the 1.1 core entry point may not be called. Whether the device has the
 * budget is asked once a device. */
bool scene_vulkan_vram(FnlApp *app, double *mib)
{
	FnlVulkanGpu g;
	if (!fnl_vulkan_gpu(app, &g) || !g_loaded)
		return false;
	/* The device's answer is kept, keyed on the instance as well: a
	 * window opened after another's closed makes a new instance, whose
	 * handles may repeat the old one's values. The entry point is asked
	 * for every time, which is cheap, so none outlives its instance. */
	static VkPhysicalDevice asked;
	static VkInstance asked_in;
	static bool budget;
	if (g.physical_device != asked || g.instance != asked_in) {
		asked = g.physical_device;
		asked_in = g.instance;
		budget = device_lists(asked, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
	}
	const PFN_vkGetPhysicalDeviceMemoryProperties2KHR get =
		(PFN_vkGetPhysicalDeviceMemoryProperties2KHR)
		g.get_instance_proc_addr(g.instance,
								 "vkGetPhysicalDeviceMemoryProperties2KHR");
	if (!budget || !get)
		return false;
	VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
	};
	VkPhysicalDeviceMemoryProperties2KHR p = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2_KHR,
		.pNext = &b,
	};
	get(g.physical_device, &p);
	VkDeviceSize used = 0;
	for (uint32_t i = 0; i < p.memoryProperties.memoryHeapCount; i++)
		if (p.memoryProperties.memoryHeaps[i].flags &
				VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
			used += b.heapUsage[i];
	*mib = (double)used / (1024.0 * 1024.0);
	return true;
}

/* ---- the driver --------------------------------------------------------- */

/* VK_KHR_driver_properties' name and info where the device offers it --
 * "NVIDIA" "580.95.05", "Intel open-source Mesa driver" "Mesa 26.2.5" --
 * and the core properties' API version and PCI ids everywhere. Chained
 * only where the device lists the extension: a struct of one it does not
 * have is invalid in the chain. */
bool scene_vulkan_driver(FnlApp *app, TestbedDriver *d)
{
	memset(d, 0, sizeof *d);
	FnlVulkanGpu g;
	if (!fnl_vulkan_gpu(app, &g) || !g_loaded)
		return false;
	PFN_vkGetPhysicalDeviceProperties props =
		(PFN_vkGetPhysicalDeviceProperties)g.get_instance_proc_addr(
			g.instance, "vkGetPhysicalDeviceProperties");
	PFN_vkGetPhysicalDeviceProperties2KHR props2 =
		(PFN_vkGetPhysicalDeviceProperties2KHR)g.get_instance_proc_addr(
			g.instance, "vkGetPhysicalDeviceProperties2KHR");
	if (!props)
		return false;
	VkPhysicalDeviceProperties p;
	props(g.physical_device, &p);
	d->vendor_id = p.vendorID;
	d->device_id = p.deviceID;
	snprintf(d->api, sizeof d->api, "Vulkan %u.%u.%u",
			 VK_API_VERSION_MAJOR(p.apiVersion),
			 VK_API_VERSION_MINOR(p.apiVersion),
			 VK_API_VERSION_PATCH(p.apiVersion));

	if (props2 && device_lists(g.physical_device,
							   VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) {
		VkPhysicalDeviceDriverPropertiesKHR dp = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR,
		};
		VkPhysicalDeviceProperties2KHR p2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR,
			.pNext = &dp,
		};
		props2(g.physical_device, &p2);
		snprintf(d->name, sizeof d->name, "%s", dp.driverName);
		snprintf(d->info, sizeof d->info, "%s", dp.driverInfo);
	} else {
		snprintf(d->info, sizeof d->info, "driver version %u",
				 p.driverVersion);
	}
	return true;
}

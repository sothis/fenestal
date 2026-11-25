/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The window's scene, as a Fenestal client draws it with Metal: two pipeline
 * states per target and two draws, from the same shaders scene_vulkan.c builds
 * its Vulkan pipelines from -- compiled to SPIR-V and translated to MSL by
 * spirv-cross at build time, then compiled by the device when it is made. The
 * pattern and the other scene a second window draws are one fragment shader's
 * two pictures, chosen by the push block's content.
 *
 * As in scene_vulkan.c, this file does not know whether the scene is drawn
 * straight into the window or into a smaller image, whether it is multisampled,
 * or how the frame is paced. As there, a pipeline state belongs to a sample
 * count and a pair of formats, so it builds one per target the library says it
 * might see. And as under Direct3D 12, the encoder's state is shared with the
 * library's own drawing, so everything the draws rely on is set on every call.
 *
 * Objective-C under ARC: Metal has no C interface, and this is the one file
 * of the example that needs one.
 */

#include "testbed.h"

#include <fenestal/fnl_metal.h>

#include <stdio.h>
#include <string.h>

#include "scene.vert.msl.h"
#include "scene.frag.msl.h"
#include "star.vert.msl.h"
#include "star.frag.msl.h"

/* As in scene_vulkan.c: 36 lit wedges, two triangles each. */
#define STAR_VERTICES (36 * 2 * 3)

/* What the shaders' `Push` block holds, at buffer 0 in both stages: the same
 * bytes as scene_vulkan.c's push constant, and pinned the same way. */
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

/* One per window, as scene_vulkan.c's Scene is: a second window has a device
 * of its own, and a pipeline state is the device's that made it. Keyed by
 * the App, given with on_gpu_created and taken with on_gpu_lost, so neither
 * window's device loss drops the other's states. A nil entry is a target
 * this window cannot use -- the multisampled one on a device with no sample
 * count worth having -- and the frame will never ask for it. Strong
 * references, the example's own: the library hands none over. */
#define SCENE_WINDOWS 8
typedef struct {
	FnlApp *app;
	id<MTLRenderPipelineState> pattern[FNL_TARGET_COUNT];
	id<MTLRenderPipelineState> star[FNL_TARGET_COUNT];
	id<MTLDepthStencilState>   no_depth;
	bool                       srgb;
} Scene;
static Scene g_scenes[SCENE_WINDOWS];

static Scene *scene_slot(FnlApp *app)
{
	for (int i = 0; i < SCENE_WINDOWS; i++)
		if (g_scenes[i].app == app)
			return &g_scenes[i];
	return NULL;
}

/* Let go of what the window's device made. Nil assignments, which ARC
 * releases; a memset would drop the references on the floor. */
static void scene_clear(Scene *s)
{
	for (int t = 0; t < FNL_TARGET_COUNT; t++) {
		s->pattern[t] = nil;
		s->star[t] = nil;
	}
	s->no_depth = nil;
	s->srgb = false;
}

/* The window's slot, emptied; a new one when it had none. NULL when every
 * slot is taken, which is more windows than the testbed draws a scene in. */
static Scene *scene_open(FnlApp *app)
{
	for (int i = 0; i < SCENE_WINDOWS; i++) {
		if (g_scenes[i].app == app || !g_scenes[i].app) {
			g_scenes[i].app = app;
			scene_clear(&g_scenes[i]);
			return &g_scenes[i];
		}
	}
	return NULL;
}

/* One stage from MSL text; spirv-cross names every entry main0. */
static id<MTLFunction> stage(id<MTLDevice> dev, const char *msl)
{
	NSError *err = nil;
	id<MTLLibrary> lib = [dev
		newLibraryWithSource:[NSString stringWithUTF8String:msl]
					 options:nil
					   error:&err];
	if (!lib) {
		fprintf(stderr, "a scene shader did not compile: %s\n",
				err.localizedDescription.UTF8String);
		return nil;
	}
	return [lib newFunctionWithName:@"main0"];
}

/* No depth test, no blending, no culling -- what scene_vulkan.c's
 * pipelines fix -- for the target `layout` describes. The depth format is
 * named although nothing tests it: the library attaches a depth, and a
 * pipeline state must say what is attached. */
static id<MTLRenderPipelineState> make_pso(id<MTLDevice> dev,
										   const FnlMetalSceneLayout *layout,
										   id<MTLFunction> vs,
										   id<MTLFunction> fs)
{
	if (!vs || !fs)
		return nil;
	MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc]
										  init];
	d.vertexFunction = vs;
	d.fragmentFunction = fs;
	d.rasterSampleCount = (NSUInteger)layout->samples;
	d.colorAttachments[0].pixelFormat = (MTLPixelFormat)layout->color_format;
	d.depthAttachmentPixelFormat = (MTLPixelFormat)layout->depth_format;
	NSError *err = nil;
	id<MTLRenderPipelineState> p =
		[dev newRenderPipelineStateWithDescriptor:d error:&err];
	if (!p)
		fprintf(stderr, "a scene pipeline state was refused: %s\n",
				err.localizedDescription.UTF8String);
	return p;
}

void scene_metal_on_gpu_created(void *ud, FnlApp *app)
{
	(void)ud;
	/* This window's states alone: another window's device is another
	 * device's pipelines, and dropping them would stop its scene. */
	Scene *s = scene_open(app);
	if (!s)
		return;

	FnlMetalGpu gpu;
	if (fnl_renderer(app) != FNL_RENDERER_METAL || !fnl_metal_gpu(app, &gpu)) {
		fprintf(stderr, "the running renderer is not Metal; "
						"no scene will be drawn\n");
		return;
	}
	id<MTLFunction> sv = stage(gpu.device, scene_vert_msl);
	id<MTLFunction> sf = stage(gpu.device, scene_frag_msl);
	id<MTLFunction> tv = stage(gpu.device, star_vert_msl);
	id<MTLFunction> tf = stage(gpu.device, star_frag_msl);
	MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
	dd.depthCompareFunction = MTLCompareFunctionAlways;
	dd.depthWriteEnabled = NO;
	s->no_depth = [gpu.device newDepthStencilStateWithDescriptor:dd];
	for (int t = 0; t < FNL_TARGET_COUNT; t++) {
		FnlMetalSceneLayout layout;
		if (!fnl_metal_scene_layout(app, (FnlSceneTarget)t, &layout))
			continue;          /* this window cannot use that target */
		/* Whether the target holds linear light: hardware-encoded _sRGB, or
		 * HDR's float format that keeps it as written. */
		if (t == FNL_TARGET_DIRECT)
			s->srgb = layout.color_format == MTLPixelFormatBGRA8Unorm_sRGB ||
					  layout.color_format == MTLPixelFormatRGBA16Float;
		s->pattern[t] = make_pso(gpu.device, &layout, sv, sf);
		s->star[t] = make_pso(gpu.device, &layout, tv, tf);
	}
}

/* Let go: a command buffer still drawing with them keeps its own
 * references, and the slot is freed so the next window may take it. */
void scene_metal_on_gpu_lost(void *ud, FnlApp *app)
{
	(void)ud;
	Scene *s = scene_slot(app);
	if (!s)
		return;
	scene_clear(s);
	s->app = NULL;
}

/* The scene itself, as in scene_vulkan.c: the pattern over one covering
 * triangle, then the star's wedges as primitives FSAA can sample. Into what the
 * library bound, with the viewport and scissor set. */
void scene_metal_on_record(void *ud, FnlApp *app, const FnlFrame *f)
{
	(void)ud;
	Scene *s = scene_slot(app);
	if (!s)
		return;
	FnlMetalScene sc;
	if (!fnl_metal_scene(app, &sc))
		return;
	if (f->target < 0 || f->target >= FNL_TARGET_COUNT ||
		!s->pattern[f->target] || !s->star[f->target])
		return;

	TestbedViewSpec view;
	testbed_scene_view(app, f, &view);
	const ScenePush pc = {
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
	id<MTLRenderCommandEncoder> enc = sc.encoder;
	[enc setDepthStencilState:s->no_depth];
	[enc setCullMode:MTLCullModeNone];
	[enc setVertexBytes:&pc length:sizeof pc atIndex:0];
	[enc setFragmentBytes:&pc length:sizeof pc atIndex:0];

	[enc setRenderPipelineState:s->pattern[f->target]];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
			vertexCount:3];
	/* The other scene is one picture and has no star; the star's wedges are
	 * the pattern's alone. */
	if (view.content == 0u) {
		[enc setRenderPipelineState:s->star[f->target]];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
				vertexCount:STAR_VERTICES];
	}
}

/* ---- the GPU memory in use ---------------------------------------------- */

/* The device's currentAllocatedSize: what this process has allocated on
 * it, which on Apple silicon is the unified memory it draws from. */
bool scene_metal_vram(FnlApp *app, double *mib)
{
	FnlMetalGpu g;
	if (!fnl_metal_gpu(app, &g))
		return false;
	*mib = (double)g.device.currentAllocatedSize / (1024.0 * 1024.0);
	return true;
}

/* ---- the driver --------------------------------------------------------- */

/* Metal names the device, and its driver is the system's -- it has no
 * version of its own -- so the info is macOS's version and build. The API
 * is the newest GPU family the device says it is in. No PCI ids: Metal
 * gives none. */
bool scene_metal_driver(FnlApp *app, TestbedDriver *d)
{
	memset(d, 0, sizeof *d);
	FnlMetalGpu g;
	if (!fnl_metal_gpu(app, &g))
		return false;
	snprintf(d->name, sizeof d->name, "%s", g.device.name.UTF8String);
	snprintf(d->info, sizeof d->info, "macOS %s",
			 NSProcessInfo.processInfo.operatingSystemVersionString
				 .UTF8String);
	const char *fam = "Metal";
	if (@available(macOS 13.0, *)) {
		if ([g.device supportsFamily:MTLGPUFamilyMetal3])
			fam = "Metal 3";
	}
	if (strcmp(fam, "Metal") == 0 &&
		[g.device supportsFamily:MTLGPUFamilyCommon3])
		fam = "Metal, common family 3";
	snprintf(d->api, sizeof d->api, "%s", fam);
	return true;
}

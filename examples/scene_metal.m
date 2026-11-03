/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * A scene drawn by the caller with Metal, the library choosing where it is
 * drawn: straight into the window, into a smaller image it scales up, or
 * multisampled. The scene here is one triangle over the whole of it in one
 * colour, so that the seam is all there is to read -- Metal has no clear
 * inside a pass, so even a fill is a pipeline. testbed/
 * scene_metal.m is a real one, from translated shaders.
 *
 * Objective-C under ARC, which Metal needs.
 *
 * Shows: fnl_metal_gpu fnl_metal_scene_layout fnl_metal_scene
 *        fnl_renderer fnl_gpu_name
 */

#include <fenestal/fnl_metal.h>

#include <stdio.h>
#include <string.h>

/* One covering triangle from the vertex index, and the colour handed in at
 * buffer 0. */
static const char FILL_MSL[] =
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"vertex float4 fill_vs(uint i [[vertex_id]]) {\n"
	"    float2 p = float2((i << 1) & 2, i & 2);\n"
	"    return float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n"
	"fragment float4 fill_fs(constant float4 &c [[buffer(0)]]) {\n"
	"    return c;\n"
	"}\n";

typedef struct {
	id<MTLRenderPipelineState> pso[FNL_TARGET_COUNT];
	id<MTLDepthStencilState>   no_depth;
	FnlMetalSceneLayout        layout[FNL_TARGET_COUNT];
} Scene;

/* The device and the layouts belong to one device generation: asked again
 * after every loss, which may land on another GPU. A pipeline state per
 * target that has a layout. */
static void on_gpu_created(void *ud, FnlApp *app)
{
	Scene *s = (Scene *)ud;
	FnlMetalGpu gpu;
	if (!fnl_metal_gpu(app, &gpu))
		return;
	NSError *err = nil;
	id<MTLLibrary> lib = [gpu.device
		newLibraryWithSource:@(FILL_MSL) options:nil error:&err];
	if (!lib) {
		fprintf(stderr, "%s\n", err.localizedDescription.UTF8String);
		return;
	}
	MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
	s->no_depth = [gpu.device newDepthStencilStateWithDescriptor:dd];
	for (int t = 0; t < FNL_TARGET_COUNT; t++) {
		if (!fnl_metal_scene_layout(app, (FnlSceneTarget)t, &s->layout[t]))
			continue;
		MTLRenderPipelineDescriptor *d =
			[[MTLRenderPipelineDescriptor alloc] init];
		d.vertexFunction = [lib newFunctionWithName:@"fill_vs"];
		d.fragmentFunction = [lib newFunctionWithName:@"fill_fs"];
		d.rasterSampleCount = (NSUInteger)s->layout[t].samples;
		d.colorAttachments[0].pixelFormat =
			(MTLPixelFormat)s->layout[t].color_format;
		d.depthAttachmentPixelFormat =
			(MTLPixelFormat)s->layout[t].depth_format;
		s->pso[t] = [gpu.device newRenderPipelineStateWithDescriptor:d
															   error:&err];
	}
}

static void on_gpu_lost(void *ud, FnlApp *app)
{
	(void)app;
	Scene *s = (Scene *)ud;
	for (int t = 0; t < FNL_TARGET_COUNT; t++)
		s->pso[t] = nil;
	s->no_depth = nil;
}

static void on_record(void *ud, FnlApp *app, const FnlFrame *frame)
{
	Scene *s = (Scene *)ud;
	FnlMetalScene sc;
	if (!fnl_metal_scene(app, &sc) || !s->pso[frame->target])
		return;
	/* Into what the library bound, with the viewport and scissor on the
	 * scene's rectangle. Recorded every frame shown; the colour still
	 * follows scene_time, the clock that moves only while the scene
	 * does. */
	const float t = frame->scene_time;
	const float colour[4] = { 0.1f + 0.1f * (t - (float)(int)t),
							  0.12f, 0.16f, 1.0f };
	[sc.encoder setRenderPipelineState:s->pso[frame->target]];
	[sc.encoder setDepthStencilState:s->no_depth];
	[sc.encoder setFragmentBytes:colour length:sizeof colour atIndex:0];
	[sc.encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
				   vertexCount:3];
}

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	Scene *s = (Scene *)ud;
	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
	}
	char line[160];
	snprintf(line, sizeof line, "%s (%s)  target %d, %d sample%s",
			 fnl_gpu_name(app),
			 fnl_renderer(app) == FNL_RENDERER_METAL ? "Metal" : "?",
			 (int)frame->target, s->layout[frame->target].samples,
			 s->layout[frame->target].samples == 1 ? "" : "s");
	const FnlColor ink = { 0.9f, 0.9f, 0.9f, 1.0f };
	fnl_overlay_text(app, 16, 48, FNL_TEXT_BODY, ink, line);
}

int main(void)
{
	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title = "scene";
	/* The renderer the scene is written for, which a build of several
	 * renderers must not trade for another. */
	desc.renderer = FNL_RENDERER_METAL;

	FnlApp *app;
	if (fnl_init(&desc, &app) < 0)
		return 1;
	/* Static, as the objects in it are ARC's to keep: a stack struct of
	 * strong references would be let go at the return, after the shutdown
	 * that drained the queue -- which is the right order, but a heap
	 * struct would need its fields cleared by hand. */
	static Scene s;
	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size = sizeof cb;
	cb.ud = &s;
	cb.on_gpu_created = on_gpu_created;
	cb.on_gpu_lost = on_gpu_lost;
	cb.on_record = on_record;
	cb.on_overlay = on_overlay;
	const FnlResult rc = fnl_run(app, &cb);
	fnl_shutdown(app);
	return rc < 0;
}

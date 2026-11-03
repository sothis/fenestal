/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * The textured cube as Metal draws it: the texture as a private texture of
 * its own, a sampler, a depth state and a pipeline state per scene target --
 * scene_vulkan.c in Metal's terms. The shaders are the Vulkan scene's GLSL,
 * compiled to SPIR-V and translated to MSL by spirv-cross at build time
 * with Y flipped (the library's FNL_MSL_FLAGS), then compiled by the device
 * when it is made: only Xcode has Metal's offline compiler. cube.vert makes
 * the cube's 36 vertices from [[vertex_id]], so there is no vertex buffer
 * and no vertex descriptor.
 *
 * Three answers here are Metal's, where scene_vulkan.c has Vulkan's:
 *
 *   per-draw data is bytes   CubePush is handed to setVertexBytes and
 *   on the encoder           setFragmentBytes at buffer 0, padded to the 96
 *                            bytes MSL makes the block; the encoder copies
 *                            it, so nothing of this file's has to outlive
 *                            the frame.
 *   the texture is uploaded  from a shared buffer into a private texture by
 *   by a blit of its own     a blit on the library's queue, waited out
 *                            here: nothing is drawn with it until the copy
 *                            is done.
 *   state is said each time  the encoder is shared with the library's own
 *                            drawing, so the pipeline state, the depth
 *                            state, the culling, the texture and the
 *                            sampler are set on every call. Metal records
 *                            every frame afresh (fnl_metal.h).
 *
 * The texture and its sampler are texture(0) and sampler(0), where
 * spirv-cross splits the GLSL's combined sampler2D at binding 0.
 *
 * The one file of the demo that names Metal, and the one that is
 * Objective-C, which Metal needs. Under ARC: the objects here are this
 * file's own references, let go in on_gpu_lost; a command buffer still
 * drawing with them keeps its own.
 */

#include "texture3.h"

#include <fenestal/fnl_metal.h>

#include <stdio.h>
#include <string.h>

#include "cube.vert.msl.h"
#include "cube.frag.msl.h"

#define CUBE_VERTEX_COUNT 36

/* CubePush as MSL lays out the shaders' `Push`: the float4x4 aligns the
 * struct to sixteen, so it is 96 bytes there where C makes it 84, and Metal
 * checks a binding against the larger. Each draw's is copied into this
 * before it is handed over. */
typedef struct {
	CubePush push;
	unsigned char pad[96 - sizeof(CubePush)];
} CubePushMsl;

_Static_assert(sizeof(CubePushMsl) == 96, "Push is 96 bytes in MSL");

/* Where the shaders' resources are: `Push` at buffer 0 in both stages, the
 * texture and its sampler at 0 in the fragment stage. */
enum { BUF_PUSH = 0, TEX_SLOT = 0 };

/* One device generation: built in on_gpu_created, let go in on_gpu_lost,
 * and not valid between. No lock: every callback runs on a thread the
 * library owns, never two at once. Strong references at file scope, as
 * testbed/scene_metal.m keeps its own: each is let go when on_gpu_lost
 * sets it to nil. */
static id<MTLRenderPipelineState> g_pso[FNL_TARGET_COUNT];
static bool                       g_srgb[FNL_TARGET_COUNT];
static id<MTLTexture>             g_tex;
static id<MTLSamplerState>        g_sampler;
static id<MTLDepthStencilState>   g_depth;

static id<MTLFunction> stage(id<MTLDevice> dev, const char *msl)
{
	NSError *err = nil;
	id<MTLLibrary> lib = [dev
		newLibraryWithSource:[NSString stringWithUTF8String:msl]
					 options:nil
					   error:&err];
	if (!lib) {
		fprintf(stderr, "a shader did not compile: %s\n",
				err.localizedDescription.UTF8String);
		return nil;
	}
	/* spirv-cross names every entry main0. */
	return [lib newFunctionWithName:@"main0"];
}

/* The same pipeline state once per target, differing only in the formats
 * and sample count fnl_metal_scene_layout() reports. No vertex descriptor:
 * the shader makes the cube. */
static id<MTLRenderPipelineState> make_pso(id<MTLDevice> dev,
										   const FnlMetalSceneLayout *layout,
										   id<MTLFunction> vs,
										   id<MTLFunction> fs)
{
	MTLRenderPipelineDescriptor *d =
		[[MTLRenderPipelineDescriptor alloc] init];
	d.vertexFunction = vs;
	d.fragmentFunction = fs;
	d.rasterSampleCount = (NSUInteger)layout->samples;
	d.colorAttachments[0].pixelFormat = (MTLPixelFormat)layout->color_format;
	d.depthAttachmentPixelFormat = (MTLPixelFormat)layout->depth_format;
	NSError *err = nil;
	id<MTLRenderPipelineState> p =
		[dev newRenderPipelineStateWithDescriptor:d error:&err];
	if (!p)
		fprintf(stderr, "a pipeline state was refused: %s\n",
				err.localizedDescription.UTF8String);
	return p;
}

/* The texture, uploaded the way a real one is: into a shared buffer, then
 * copied by the device into a private texture of its own, which the GPU
 * reads at its own speed and the CPU cannot touch.
 *
 * The blit is committed on the library's queue, which is shared and orders
 * it against the frames (fnl_metal.h), and waited out, so the staging
 * buffer can go as soon as this returns. on_gpu_created runs before the
 * first frame of this device, so the wait holds nothing up.
 *
 * MTLPixelFormatRGBA8Unorm_sRGB, because the bytes are colours as a picker
 * gives them: sampling then returns linear values, which is what the shader
 * lights. */
static bool make_texture(id<MTLDevice> dev, id<MTLCommandQueue> q)
{
	static unsigned char px[TEX_SIZE * TEX_SIZE * 4];
	texture_pixels(px);

	MTLTextureDescriptor *td = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
									 width:TEX_SIZE
									height:TEX_SIZE
								 mipmapped:NO];
	td.storageMode = MTLStorageModePrivate;
	td.usage = MTLTextureUsageShaderRead;
	g_tex = [dev newTextureWithDescriptor:td];
	if (!g_tex)
		return false;

	/* The command buffer and the encoder are autoreleased; a pool of this
	 * file's own lets them go here, whatever the calling thread drains. */
	@autoreleasepool {
		id<MTLBuffer> staging =
			[dev newBufferWithBytes:px
							 length:sizeof px
							options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cb = [q commandBuffer];
		if (!staging || !cb)
			return false;
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		if (!blit)
			return false;
		[blit copyFromBuffer:staging
				 sourceOffset:0
			sourceBytesPerRow:TEX_SIZE * 4
		  sourceBytesPerImage:sizeof px
				   sourceSize:MTLSizeMake(TEX_SIZE, TEX_SIZE, 1)
					toTexture:g_tex
			 destinationSlice:0
			 destinationLevel:0
			destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		if (cb.status != MTLCommandBufferStatusCompleted) {
			fprintf(stderr, "the texture's upload failed: %s\n",
					cb.error ? cb.error.localizedDescription.UTF8String
							 : "no error given");
			return false;
		}
	}

	/* Linear both ways, clamped at the edges, as scene_vulkan.c's is: the
	 * frame round the checkerboard is the face's edge and is not to bleed
	 * in from the opposite side. */
	MTLSamplerDescriptor *s = [[MTLSamplerDescriptor alloc] init];
	s.minFilter = MTLSamplerMinMagFilterLinear;
	s.magFilter = MTLSamplerMinMagFilterLinear;
	s.mipFilter = MTLSamplerMipFilterNotMipmapped;
	s.sAddressMode = MTLSamplerAddressModeClampToEdge;
	s.tAddressMode = MTLSamplerAddressModeClampToEdge;
	g_sampler = [dev newSamplerStateWithDescriptor:s];
	return g_sampler != nil;
}

/* Everything the device owns, let go. Only what was made was kept, so one
 * function serves an ordinary loss and a build that stopped halfway. */
void scene_metal_on_gpu_lost(void *ud, FnlApp *app)
{
	(void)ud;
	(void)app;
	for (int i = 0; i < FNL_TARGET_COUNT; i++) {
		g_pso[i] = nil;
		g_srgb[i] = false;
	}
	g_tex = nil;
	g_sampler = nil;
	g_depth = nil;
}

/* Called once at startup and again after every device loss, with a device
 * that shares nothing with the last one. A failure leaves the window open
 * and the scene black. */
void scene_metal_on_gpu_created(void *ud, FnlApp *app)
{
	scene_metal_on_gpu_lost(ud, app);
	FnlMetalGpu gpu;
	if (!fnl_metal_gpu(app, &gpu)) {
		fprintf(stderr, "the running renderer is not Metal\n");
		return;
	}
	id<MTLFunction> vs = stage(gpu.device, cube_vert_msl);
	id<MTLFunction> fs = stage(gpu.device, cube_frag_msl);
	/* Depth tested and written: the target carries a depth attachment the
	 * library clears to 1 before on_record. */
	MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
	dd.depthCompareFunction = MTLCompareFunctionLess;
	dd.depthWriteEnabled = YES;
	g_depth = [gpu.device newDepthStencilStateWithDescriptor:dd];
	if (!vs || !fs || !g_depth || !make_texture(gpu.device, gpu.queue)) {
		fprintf(stderr, "could not build the scene's shaders or texture\n");
		scene_metal_on_gpu_lost(ud, app);
		return;
	}
	for (int i = 0; i < FNL_TARGET_COUNT; i++) {
		FnlMetalSceneLayout layout;
		if (!fnl_metal_scene_layout(app, (FnlSceneTarget)i, &layout))
			continue;          /* this window cannot use that target */
		/* Whether the target encodes linear to sRGB as it is written,
		 * which decides whether the shader must. */
		g_srgb[i] = layout.color_format == MTLPixelFormatBGRA8Unorm_sRGB;
		g_pso[i] = make_pso(gpu.device, &layout, vs, fs);
	}
}

/* The cube, into the encoder the library bound -- cleared, with the
 * viewport and scissor set. Back faces culled; counter-clockwise is the
 * front, as scene_vulkan.c says for the same shader: the MSL flips Y, so
 * the picture is Vulkan's and so is its winding (fnl_metal.h). */
void scene_metal_draw(FnlApp *app, const FnlFrame *f, CubePush *push)
{
	FnlMetalScene sc;
	if (!fnl_metal_scene(app, &sc))
		return;
	if (f->target < 0 || f->target >= FNL_TARGET_COUNT ||
		!g_pso[f->target] || !g_tex)
		return;

	push->srgb = g_srgb[f->target] ? 1u : 0u;
	CubePushMsl p;
	memset(&p, 0, sizeof p);
	p.push = *push;

	id<MTLRenderCommandEncoder> enc = sc.encoder;
	[enc setRenderPipelineState:g_pso[f->target]];
	[enc setDepthStencilState:g_depth];
	[enc setCullMode:MTLCullModeBack];
	[enc setFrontFacingWinding:MTLWindingCounterClockwise];
	[enc setVertexBytes:&p length:sizeof p atIndex:BUF_PUSH];
	[enc setFragmentBytes:&p length:sizeof p atIndex:BUF_PUSH];
	[enc setFragmentTexture:g_tex atIndex:TEX_SLOT];
	[enc setFragmentSamplerState:g_sampler atIndex:TEX_SLOT];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle
			vertexStart:0
			vertexCount:CUBE_VERTEX_COUNT];
}

/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The overlay under Metal: rectangles, strokes and text, each one instanced
 * draw per flush, from the frame slot's arenas; the pipeline states that
 * draw them and the scene's fetch; and the glyph atlas. What is drawn, and
 * in which order, is the core's (src/core/overlay.c, draw.c); this file is
 * how.
 *
 * Every shader is spirv-cross's MSL of the library's GLSL (DESIGN *One
 * shader source*), compiled when the device is made: `Push` at buffer 0 in
 * both stages, fed by setVertexBytes and setFragmentBytes; the instance
 * stream at buffer 1, a vertex attribute at location N being attribute N;
 * the image and its sampler at 0.
 *
 * Every pipeline here is made for the window's pass -- the drawable's
 * format, the window's depth, one sample -- and MLAA's own pass is given a
 * depth of the same format for the same reason, so that one set serves
 * both. None tests or writes the depth.
 */

#include "core/app.h"

#include "renderer/metal/metal_internal.h"

#include "msl/rect.vert.h"
#include "msl/rect.frag.h"
#include "msl/shape.vert.h"
#include "msl/shape.frag.h"
#include "msl/text.vert.h"
#include "msl/text_lcd.frag.h"
#include "msl/fullscreen.vert.h"
#include "msl/blit.frag.h"
#include "msl/mlaa.frag.h"
#include "font_data.h"
#include "font_codec.h"

/* The two `Push` blocks: the same bytes as the Vulkan renderer's OverlayPush
 * and TextPush. */
typedef struct {
	float screen[2];
} MtOverlayPush;

typedef struct {
	float    screen[2];
	uint32_t srgb;
	float    texel_u;
} MtTextPush;

_Static_assert(sizeof(MtOverlayPush) == 8,
			   "MtOverlayPush must match shaders/*.vert");
_Static_assert(sizeof(MtTextPush) == 16,
			   "MtTextPush must match shaders/text.*");
_Static_assert(offsetof(MtTextPush, srgb) == 8,
			   "MtTextPush layout drifted");
_Static_assert(offsetof(MtTextPush, texel_u) == 12,
			   "MtTextPush layout drifted");

/* ---- pipeline states ----------------------------------------------------- */

/* The blend each kind is drawn with: rectangles replace what is under them,
 * strokes blend on their alpha, text on a weight per channel. The same
 * factors as the Vulkan renderer's pipelines (make_overlay_pipeline()). */
typedef enum { BLEND_NONE, BLEND_ALPHA, BLEND_DUAL_SRC, BLEND_CONSTANT } Blend;

typedef struct {
	const char   *name;
	const char   *vs;
	const char   *fs;
	const size_t *offsets;      /* per-instance vec4s, or NULL */
	int           n_inputs;
	size_t        stride;
	Blend         blend;
} PsoDesc;

static id<MTLRenderPipelineState> make_pso(App *a, const PsoDesc *s)
{
	id<MTLFunction> vs = mt_function(a, s->name, s->vs);
	id<MTLFunction> fs = mt_function(a, s->name, s->fs);
	if (!vs || !fs)
		return nil;

	MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc]
										  init];
	d.label = [NSString stringWithUTF8String:s->name];
	d.vertexFunction = vs;
	d.fragmentFunction = fs;
	d.rasterSampleCount = 1;
	d.depthAttachmentPixelFormat = MT_DEPTH_FORMAT;

	if (s->n_inputs) {
		MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
		for (int i = 0; i < s->n_inputs; i++) {
			vd.attributes[(NSUInteger)i].format = MTLVertexFormatFloat4;
			vd.attributes[(NSUInteger)i].offset = s->offsets[i];
			vd.attributes[(NSUInteger)i].bufferIndex = MT_BUF_INSTANCE;
		}
		vd.layouts[MT_BUF_INSTANCE].stride = s->stride;
		vd.layouts[MT_BUF_INSTANCE].stepFunction =
			MTLVertexStepFunctionPerInstance;
		vd.layouts[MT_BUF_INSTANCE].stepRate = 1;
		d.vertexDescriptor = vd;
	}

	MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
	c.pixelFormat = MT_COLOR_FORMAT;
	c.blendingEnabled = s->blend != BLEND_NONE;
	c.rgbBlendOperation = MTLBlendOperationAdd;
	c.alphaBlendOperation = MTLBlendOperationAdd;
	if (s->blend == BLEND_ALPHA) {
		c.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
		c.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		c.sourceAlphaBlendFactor = MTLBlendFactorOne;
		c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	} else if (s->blend == BLEND_DUAL_SRC) {
		c.sourceRGBBlendFactor = MTLBlendFactorSource1Color;
		c.destinationRGBBlendFactor = MTLBlendFactorOneMinusSource1Color;
		c.sourceAlphaBlendFactor = MTLBlendFactorSource1Alpha;
		c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSource1Alpha;
	} else if (s->blend == BLEND_CONSTANT) {
		/* The cover: one weight a draw (setBlendColor), the alpha channel
		 * left as it is. */
		c.sourceRGBBlendFactor = MTLBlendFactorBlendAlpha;
		c.destinationRGBBlendFactor = MTLBlendFactorOneMinusBlendAlpha;
		c.sourceAlphaBlendFactor = MTLBlendFactorZero;
		c.destinationAlphaBlendFactor = MTLBlendFactorOne;
	}

	NSError *err = nil;
	id<MTLRenderPipelineState> p =
		[MT(a)->device newRenderPipelineStateWithDescriptor:d error:&err];
	if (!p)
		fnl_log(FNL_LOG_ERROR, "the %s pipeline state was refused: %s",
				s->name, err.localizedDescription.UTF8String ?: "(no log)");
	return p;
}

/* ---- the atlas ----------------------------------------------------------- */

static void destroy_atlas(App *a)
{
	MT(a)->font_tex = nil;
	/* And the index text_draw() tests before appending a glyph, so a scale
	 * change whose upload fails appends nothing. */
	a->font_si = -1;
}

/* Decoded and written into a managed texture, which replaceRegion keeps in
 * step with the GPU's copy: one byte of coverage a texel, row 0 the first
 * row of the bake, as on Vulkan. Managed and not shared, which an Intel
 * Mac's GPU will not sample. */
static FnlResult build_atlas(App *a, int si)
{
	const int aw = (int)FONT_ATLAS_W[si];
	const int ah = (int)FONT_ATLAS_H[si];
	const size_t bytes = (size_t)aw * (size_t)ah;

	unsigned char *scratch = calloc(1, bytes);
	if (!scratch)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
						"out of memory expanding the %dx%d glyph atlas",
						aw, ah);
	if (!font_decode(font_atlas_blob + FONT_ATLAS_OFF[si],
					 FONT_ATLAS_LEN[si], scratch, bytes)) {
		free(scratch);
		return fnl_fail(FNL_ERR_SYSTEM,
						"the baked atlas for scale %d does not decode to "
						"%dx%d", FONT_SCALE[si], aw, ah);
	}

	MTLTextureDescriptor *d = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
									 width:(NSUInteger)aw
									height:(NSUInteger)ah
								 mipmapped:NO];
	d.storageMode = MTLStorageModeManaged;
	d.usage = MTLTextureUsageShaderRead;
	id<MTLTexture> t = [MT(a)->device newTextureWithDescriptor:d];
	if (!t) {
		free(scratch);
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
						"no room for the %dx%d glyph atlas", aw, ah);
	}
	[t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)aw, (NSUInteger)ah)
		 mipmapLevel:0
		   withBytes:scratch
		 bytesPerRow:(NSUInteger)aw];
	free(scratch);

	MT(a)->font_tex = t;
	a->font_si      = si;
	a->font_atlas_w = aw;
	a->font_atlas_h = ah;
	return FNL_OK;
}

/* Render thread, when it latches a new scale. The old texture may be read
 * by a frame in flight, whose command buffer keeps it alive; it is simply
 * let go. Compared by atlas index, as on Vulkan. */
FnlResult mt_text_set_scale(App *a, int s120)
{
	if (!MT(a)->pso_text)
		return FNL_OK;
	const int si = scale_index(s120);
	if (si == a->font_si)
		return FNL_OK;
	destroy_atlas(a);
	/* The texture's descriptor is autoreleased, outside the frame's pool. */
	FnlResult rc;
	@autoreleasepool {
		rc = build_atlas(a, si);
	}
	return rc;
}

/* ---- building and tearing down ------------------------------------------- */

/* One arena of `bytes` per frame slot, shared between the CPU and the GPU
 * and write-combined: the core appends straight into it, and the slot's wait
 * (frame.m) says when the GPU is done reading it. */
static FnlResult make_arenas(App *a, __strong id<MTLBuffer> *bufs,
							 size_t bytes, void **maps)
{
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		bufs[i] = [MT(a)->device
			newBufferWithLength:bytes
						options:MTLResourceStorageModeShared |
								MTLResourceCPUCacheModeWriteCombined];
		if (!bufs[i]) {
			maps[i] = NULL;
			return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
							"could not map a %zu-byte overlay arena", bytes);
		}
		maps[i] = bufs[i].contents;
	}
	return FNL_OK;
}

static void destroy_arenas(__strong id<MTLBuffer> *bufs, void **maps)
{
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		bufs[i] = nil;
		maps[i] = NULL;
	}
}

static id<MTLSamplerState> make_sampler(App *a, MTLSamplerMinMagFilter f)
{
	MTLSamplerDescriptor *d = [[MTLSamplerDescriptor alloc] init];
	d.minFilter = f;
	d.magFilter = f;
	d.sAddressMode = MTLSamplerAddressModeClampToEdge;
	d.tAddressMode = MTLSamplerAddressModeClampToEdge;
	return [MT(a)->device newSamplerStateWithDescriptor:d];
}

FnlResult mt_create_overlay(App *a)
{
	struct RendererState *g = MT(a);
	const size_t rect[2] = { offsetof(RectInstance, rect),
							 offsetof(RectInstance, color) };
	const size_t shape[3] = { offsetof(ShapeInstance, p0),
							  offsetof(ShapeInstance, p2),
							  offsetof(ShapeInstance, color) };
	const size_t text[3] = { offsetof(GlyphInstance, rect),
							 offsetof(GlyphInstance, uv),
							 offsetof(GlyphInstance, color) };
	const PsoDesc descs[6] = {
		{ "rectangle", rect_vert_msl, rect_frag_msl, rect, 2,
		  sizeof(RectInstance), BLEND_NONE },
		{ "stroke", shape_vert_msl, shape_frag_msl, shape, 3,
		  sizeof(ShapeInstance), BLEND_ALPHA },
		/* The subpixel variant always: dual-source blending is in every
		 * Mac's Metal, where Vulkan has it as an optional feature. */
		{ "text", text_vert_msl, text_lcd_frag_msl, text, 3,
		  sizeof(GlyphInstance), BLEND_DUAL_SRC },
		{ "fetch", fullscreen_vert_msl, blit_frag_msl, NULL, 0, 0,
		  BLEND_NONE },
		{ "MLAA", fullscreen_vert_msl, mlaa_frag_msl, NULL, 0, 0,
		  BLEND_NONE },
		/* The cover: the fetch once more, blended by a constant, for the
		 * frame read back before a move (mt_overlay_cover()). */
		{ "cover", fullscreen_vert_msl, blit_frag_msl, NULL, 0, 0,
		  BLEND_CONSTANT },
	};
	__strong id<MTLRenderPipelineState> *out[6] = {
		&g->pso_rect, &g->pso_shape, &g->pso_text, &g->pso_blit,
		&g->pso_mlaa, &g->pso_cover,
	};
	for (int i = 0; i < 6; i++) {
		*out[i] = make_pso(a, &descs[i]);
		if (!*out[i])
			return fnl_fail(FNL_ERR_SYSTEM,
							"the overlay's pipeline states did not build");
	}

	MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
	dd.depthCompareFunction = MTLCompareFunctionAlways;
	dd.depthWriteEnabled = NO;
	g->ds_off = [g->device newDepthStencilStateWithDescriptor:dd];
	g->smp_nearest = make_sampler(a, MTLSamplerMinMagFilterNearest);
	g->smp_linear = make_sampler(a, MTLSamplerMinMagFilterLinear);
	if (!g->ds_off || !g->smp_nearest || !g->smp_linear)
		return fnl_fail(FNL_ERR_SYSTEM, "the overlay's samplers or depth "
						"state did not build");

	FNL_TRY(make_arenas(a, g->text_buf, TEXT_ARENA_BYTES,
						(void **)a->text_map));
	FNL_TRY(make_arenas(a, g->ovl_buf, OVERLAY_ARENA_BYTES,
						(void **)a->ovl_map));

	/* The scale is 120 until the render thread latches another, as on
	 * Vulkan. */
	a->font_si = -1;
	return build_atlas(a, scale_index(scale120(a)));
}

void mt_destroy_overlay(App *a)
{
	struct RendererState *g = MT(a);
	destroy_atlas(a);
	destroy_arenas(g->text_buf, (void **)a->text_map);
	destroy_arenas(g->ovl_buf, (void **)a->ovl_map);
	g->pso_rect = g->pso_shape = g->pso_text = nil;
	g->pso_blit = g->pso_mlaa = g->pso_cover = nil;
	g->cover_tex = nil;
	g->cover_gen = 0;
	g->snap_buf = nil;
	g->ds_off = nil;
	g->smp_nearest = g->smp_linear = nil;
}

/* ---- drawing ------------------------------------------------------------- */

void mt_own_state(App *a, id<MTLRenderCommandEncoder> enc)
{
	[enc setDepthStencilState:MT(a)->ds_off];
	[enc setCullMode:MTLCullModeNone];
	[enc setTriangleFillMode:MTLTriangleFillModeFill];
	[enc setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
}

void mt_fetch(App *a, id<MTLRenderCommandEncoder> enc,
			  id<MTLRenderPipelineState> pso, id<MTLTexture> tex)
{
	mt_own_state(a, enc);
	[enc setRenderPipelineState:pso];
	[enc setFragmentTexture:tex atIndex:0];
	[enc setFragmentSamplerState:MT(a)->smp_linear atIndex:0];
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
			vertexCount:3];
}

/* The instances appended since this kind was last flushed: the arena from
 * the first of them, so a second flush reads on with nothing copied, as on
 * Vulkan. */
static void flush(App *a, id<MTLRenderPipelineState> pso, id<MTLBuffer> buf,
				  size_t offset, size_t stride, const uint32_t *count,
				  uint32_t *drawn)
{
	id<MTLRenderCommandEncoder> enc = MT(a)->enc;
	if (*count == *drawn || !enc || !pso || !buf)
		return;
	[enc setRenderPipelineState:pso];
	[enc setVertexBuffer:buf offset:offset + (size_t)*drawn * stride
				 atIndex:MT_BUF_INSTANCE];
	const MtOverlayPush push = { .screen = { (float)a->extent.w,
											 (float)a->extent.h } };
	[enc setVertexBytes:&push length:sizeof push atIndex:MT_BUF_PUSH];
	[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0
			vertexCount:4 instanceCount:*count - *drawn];
	*drawn = *count;
}

void mt_overlay_flush_rects(App *a)
{
	flush(a, MT(a)->pso_rect, MT(a)->ovl_buf[a->ovl_img], 0,
		  sizeof(RectInstance), &a->rect_count, &a->rect_drawn);
}

void mt_overlay_flush_shapes(App *a)
{
	flush(a, MT(a)->pso_shape, MT(a)->ovl_buf[a->ovl_img],
		  OVERLAY_SHAPE_OFF, sizeof(ShapeInstance), &a->shape_count,
		  &a->shape_drawn);
}

/* NULL restores the whole image. Row 0 is the top, as on Vulkan. A scissor
 * past the target's edge is refused by Metal, so it is cut to fit. */
void mt_overlay_scissor(App *a, const FnlRect *r)
{
	id<MTLRenderCommandEncoder> enc = MT(a)->enc;
	if (!enc)
		return;
	FnlRect s = r ? *r : (FnlRect){ 0, 0, a->extent.w, a->extent.h };
	if (s.x < 0) { s.w += s.x; s.x = 0; }
	if (s.y < 0) { s.h += s.y; s.y = 0; }
	if (s.x + s.w > a->extent.w) s.w = a->extent.w - s.x;
	if (s.y + s.h > a->extent.h) s.h = a->extent.h - s.y;
	if (s.w < 0) s.w = 0;
	if (s.h < 0) s.h = 0;
	[enc setScissorRect:(MTLScissorRect){ (NSUInteger)s.x, (NSUInteger)s.y,
										  (NSUInteger)s.w,
										  (NSUInteger)s.h }];
}

/* ---- the cover ----------------------------------------------------------- */

/* App::snap into a managed texture, as the atlas is: BGRA, the
 * snapshot's order (app_snapshot_set()), as the texture's format, and
 * _sRGB as every colour image is, so the bytes round-trip through the
 * sampler and the drawable unchanged. The one before is simply let go. */
static bool cover_upload(App *a)
{
	const NSUInteger w = (NSUInteger)a->snap_w, h = (NSUInteger)a->snap_h;
	MTLTextureDescriptor *d = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB
									 width:w
									height:h
								 mipmapped:NO];
	d.storageMode = MTLStorageModeManaged;
	d.usage = MTLTextureUsageShaderRead;
	id<MTLTexture> t = [MT(a)->device newTextureWithDescriptor:d];
	if (!t)
		return false;
	[t replaceRegion:MTLRegionMake2D(0, 0, w, h)
		 mipmapLevel:0
		   withBytes:a->snap_px
		 bytesPerRow:w * 4];
	MT(a)->cover_tex = t;
	return true;
}

/* The cover over the window at `alpha`: the fetch at the cover's pipeline
 * state, its weight the blend colour's alpha. Uploaded once per snapshot
 * (App::snap_gen) and tried once. */
bool mt_overlay_cover(App *a, float alpha)
{
	struct RendererState *g = MT(a);
	id<MTLRenderCommandEncoder> enc = g->enc;
	if (!enc || !a->snap_valid || !g->pso_cover)
		return false;
	if (g->cover_gen != a->snap_gen) {
		g->cover_gen = a->snap_gen;
		g->cover_tex = nil;
		if (!cover_upload(a))
			fnl_log(FNL_LOG_WARN, "the frame read back before the move "
					"would not upload; the move dissolves from black");
	}
	if (!g->cover_tex)
		return false;
	[enc setBlendColorRed:0.0f green:0.0f blue:0.0f alpha:alpha];
	mt_fetch(a, enc, g->pso_cover, g->cover_tex);
	return true;
}

/* The seam's cover_release: simply let go, as cover_upload() lets the one
 * before go -- a command buffer in flight retains what it reads. */
void mt_cover_release(App *a)
{
	MT(a)->cover_tex = nil;
	MT(a)->snap_buf = nil;
}

FnlResult mt_text_end(App *a)
{
	struct RendererState *g = MT(a);
	id<MTLRenderCommandEncoder> enc = g->enc;
	if (a->text_count == a->text_drawn || !enc || !g->pso_text ||
		!g->font_tex)
		return FNL_OK;
	[enc setRenderPipelineState:g->pso_text];
	[enc setVertexBuffer:g->text_buf[a->text_img]
				  offset:(size_t)a->text_drawn * sizeof(GlyphInstance)
				 atIndex:MT_BUF_INSTANCE];
	[enc setFragmentTexture:g->font_tex atIndex:0];
	[enc setFragmentSamplerState:g->smp_nearest atIndex:0];

	/* As on Vulkan: the blend space, and the width of one atlas texel, which
	 * is zero -- the grayscale answer -- for a resampled scale. Both stages
	 * read the block. */
	const MtTextPush push = {
		.screen  = { (float)a->extent.w, (float)a->extent.h },
		.srgb    = a->srgb_surface ? 1u : 0u,
		.texel_u = text_resampled(a) || a->font_atlas_w <= 0
					   ? 0.0f
					   : 1.0f / (float)a->font_atlas_w,
	};
	[enc setVertexBytes:&push length:sizeof push atIndex:MT_BUF_PUSH];
	[enc setFragmentBytes:&push length:sizeof push atIndex:MT_BUF_PUSH];
	[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0
			vertexCount:4 instanceCount:a->text_count - a->text_drawn];
	a->text_drawn = a->text_count;
	return FNL_OK;
}

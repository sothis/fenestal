/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The overlay under Direct3D 12: rectangles, strokes and text, each one
 * instanced draw per flush, from the frame slot's arenas; the pipeline states
 * that draw them and the scene's fetch; and the glyph atlas. What is drawn,
 * and in which order, is the core's (src/core/overlay.c, draw.c); this file
 * is how.
 *
 * Every shader is dxc's DXIL of spirv-cross's HLSL of the library's GLSL
 * (DESIGN *One shader source*): `cbuffer Push` at b0, fed as root
 * constants; a sampled image at t0 with its sampler at s0; a vertex input
 * at location N as TEXCOORDN.
 */

#include "core/app.h"

#include "renderer/d3d12/d3d12_internal.h"

#include "dxil/rect.vert.h"
#include "dxil/rect.frag.h"
#include "dxil/shape.vert.h"
#include "dxil/shape.frag.h"
#include "dxil/text.vert.h"
#include "dxil/text_lcd.frag.h"
#include "dxil/fullscreen.vert.h"
#include "dxil/blit.frag.h"
#include "dxil/mlaa.frag.h"
#include "font_data.h"
#include "font_codec.h"

/* The two `Push` blocks as root constants: the same bytes as the Vulkan
 * renderer's OverlayPush and TextPush, in 32-bit words. */
typedef struct {
	float screen[2];
} D3dOverlayPush;

typedef struct {
	float    screen[2];
	uint32_t srgb;
	float    texel_u;
} D3dTextPush;

_Static_assert(sizeof(D3dOverlayPush) == 8,
			   "D3dOverlayPush must match shaders/*.vert");
_Static_assert(sizeof(D3dTextPush) == 16,
			   "D3dTextPush must match shaders/text.*");
_Static_assert(offsetof(D3dTextPush, srgb) == 8,
			   "D3dTextPush layout drifted");
_Static_assert(offsetof(D3dTextPush, texel_u) == 12,
			   "D3dTextPush layout drifted");

/* The root signature's parameters, in order. */
enum { ROOT_PUSH = 0, ROOT_SRV, ROOT_SAMPLER, ROOT_COUNT };
#define PUSH_WORDS 4

/* ---- the root signature -------------------------------------------------- */

/* One for everything the library draws: the push words visible to both
 * stages, and one image and one sampler for the pixel stage. Serialised at
 * run time rather than embedded in the DXIL, so the shaders stay what
 * spirv-cross wrote. Of `dev`, which may be no renderer's yet
 * (d3d_draws_on()). */
static HRESULT root_make(ID3D12Device *dev, ID3D12RootSignature **out)
{
	const D3D12_DESCRIPTOR_RANGE srv = {
		.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		.NumDescriptors = 1,
		.OffsetInDescriptorsFromTableStart =
			D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
	};
	const D3D12_DESCRIPTOR_RANGE smp = {
		.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,
		.NumDescriptors = 1,
		.OffsetInDescriptorsFromTableStart =
			D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
	};
	D3D12_ROOT_PARAMETER p[ROOT_COUNT] = {
		[ROOT_PUSH] = {
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
			.Constants = { .ShaderRegister = 0, .Num32BitValues = PUSH_WORDS },
			.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
		},
		[ROOT_SRV] = {
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
			.DescriptorTable = { 1, &srv },
			.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
		},
		[ROOT_SAMPLER] = {
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
			.DescriptorTable = { 1, &smp },
			.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
		},
	};
	const D3D12_ROOT_SIGNATURE_DESC d = {
		.NumParameters = ROOT_COUNT,
		.pParameters = p,
		.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
	};
	ID3DBlob *blob = NULL, *err = NULL;
	HRESULT hr = D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1,
											 &blob, &err);
	if (FAILED(hr)) {
		if (err) {
			fnl_log(FNL_LOG_ERROR, "the root signature: %.*s",
					(int)ID3D10Blob_GetBufferSize(err),
					(const char *)ID3D10Blob_GetBufferPointer(err));
			ID3D10Blob_Release(err);
		}
		return hr;
	}
	hr = ID3D12Device_CreateRootSignature(
		dev, 0, ID3D10Blob_GetBufferPointer(blob),
		ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
		(void **)out);
	ID3D10Blob_Release(blob);
	return hr;
}

static FnlResult make_root(App *a)
{
	const HRESULT hr = root_make(D3D(a)->device, &D3D(a)->root);
	if (FAILED(hr))
		return d3d_fail(FNL_ERR_SYSTEM, "the root signature", hr);
	return FNL_OK;
}

/* ---- pipeline states ----------------------------------------------------- */

/* The blend each kind is drawn with: rectangles replace what is under them,
 * strokes blend on their alpha, text on a weight per channel. The same
 * factors as the Vulkan renderer's pipelines (make_overlay_pipeline()). */
typedef enum { BLEND_NONE, BLEND_ALPHA, BLEND_DUAL_SRC, BLEND_CONSTANT } Blend;

typedef struct {
	const char          *name;
	const unsigned char *vs;
	size_t               vs_len;
	const unsigned char *ps;
	size_t               ps_len;
	const size_t        *offsets;      /* per-instance vec4s, or NULL */
	int                  n_inputs;
	Blend                blend;
} PsoDesc;

#define BYTES(x) (x), sizeof(x)

/* `s` on `dev` under `root`; NULL where it was refused, said in a line
 * unless `quiet`. */
static ID3D12PipelineState *pso_make(ID3D12Device *dev,
									 ID3D12RootSignature *root,
									 const PsoDesc *s, bool quiet)
{
	static const char *const SEMANTIC = "TEXCOORD";
	D3D12_INPUT_ELEMENT_DESC in[3];
	FNL_ASSERT(s->n_inputs <= 3);
	for (int i = 0; i < s->n_inputs; i++)
		in[i] = (D3D12_INPUT_ELEMENT_DESC){
			.SemanticName = SEMANTIC,
			.SemanticIndex = (UINT)i,
			.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
			.InputSlot = 0,
			.AlignedByteOffset = (UINT)s->offsets[i],
			.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,
			.InstanceDataStepRate = 1,
		};

	D3D12_RENDER_TARGET_BLEND_DESC b = {
		.BlendEnable = s->blend != BLEND_NONE,
		.SrcBlend = D3D12_BLEND_ONE,
		.DestBlend = D3D12_BLEND_ZERO,
		.BlendOp = D3D12_BLEND_OP_ADD,
		.SrcBlendAlpha = D3D12_BLEND_ONE,
		.DestBlendAlpha = D3D12_BLEND_ZERO,
		.BlendOpAlpha = D3D12_BLEND_OP_ADD,
		.LogicOp = D3D12_LOGIC_OP_NOOP,
		.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
	};
	if (s->blend == BLEND_ALPHA) {
		b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
		b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		b.SrcBlendAlpha = D3D12_BLEND_ONE;
		b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	} else if (s->blend == BLEND_DUAL_SRC) {
		b.SrcBlend = D3D12_BLEND_SRC1_COLOR;
		b.DestBlend = D3D12_BLEND_INV_SRC1_COLOR;
		b.SrcBlendAlpha = D3D12_BLEND_SRC1_ALPHA;
		b.DestBlendAlpha = D3D12_BLEND_INV_SRC1_ALPHA;
	} else if (s->blend == BLEND_CONSTANT) {
		/* The cover: one weight a draw (OMSetBlendFactor), the alpha
		 * channel left as it is. BLEND_FACTOR takes the factor's colour
		 * channels one each, and Direct3D 12 has no constant-alpha factor
		 * every Windows 10 runs, so d3d_overlay_cover() sets all four to
		 * the weight. */
		b.SrcBlend = D3D12_BLEND_BLEND_FACTOR;
		b.DestBlend = D3D12_BLEND_INV_BLEND_FACTOR;
		b.SrcBlendAlpha = D3D12_BLEND_ZERO;
		b.DestBlendAlpha = D3D12_BLEND_ONE;
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {
		.pRootSignature = root,
		.VS = { s->vs, s->vs_len },
		.PS = { s->ps, s->ps_len },
		.SampleMask = UINT32_MAX,
		.RasterizerState = {
			.FillMode = D3D12_FILL_MODE_SOLID,
			.CullMode = D3D12_CULL_MODE_NONE,
			.DepthClipEnable = TRUE,
		},
		.InputLayout = { s->n_inputs ? in : NULL, (UINT)s->n_inputs },
		.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
		.NumRenderTargets = 1,
		.RTVFormats = { D3D_COLOR_FORMAT },
		.DSVFormat = DXGI_FORMAT_UNKNOWN,
		.SampleDesc = { 1, 0 },
	};
	d.BlendState.RenderTarget[0] = b;
	ID3D12PipelineState *p = NULL;
	const HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(
		dev, &d, &IID_ID3D12PipelineState, (void **)&p);
	if (FAILED(hr)) {
		if (!quiet)
			fnl_log(FNL_LOG_ERROR, "the %s pipeline state was refused "
					"(0x%08lx)", s->name, (unsigned long)hr);
		return NULL;
	}
	return p;
}

static ID3D12PipelineState *make_pso(App *a, const PsoDesc *s)
{
	return pso_make(D3D(a)->device, D3D(a)->root, s, false);
}

/* Whether `dev` builds the library's pipeline states from its DXIL: the
 * rectangle's, the smallest, built and let go. A device can be made where
 * none will build -- wine's own vkd3d makes one and runs no DXIL -- and a
 * GPU listed there is a move that comes back and an entry the ranking
 * measures as nothing, so the list leaves it out instead. */
bool d3d_draws_on(ID3D12Device *dev)
{
	ID3D12RootSignature *root = NULL;
	if (FAILED(root_make(dev, &root)))
		return false;
	const size_t rect[2] = { offsetof(RectInstance, rect),
							 offsetof(RectInstance, color) };
	const PsoDesc d = { "rectangle", BYTES(rect_vert_dxil),
						BYTES(rect_frag_dxil), rect, 2, BLEND_NONE };
	ID3D12PipelineState *p = pso_make(dev, root, &d, true);
	const bool ok = p != NULL;
	D3D_RELEASE(p);
	D3D_RELEASE(root);
	return ok;
}

/* ---- the atlas ----------------------------------------------------------- */

static void destroy_atlas(App *a)
{
	D3D_RELEASE(D3D(a)->font_tex);
	/* And the index text_draw() tests before appending a glyph, so a scale
	 * change whose upload fails appends nothing. */
	a->font_si = -1;
}

/* Decoded into an upload buffer laid out as the copy wants it -- rows on
 * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT -- and copied into the texture by a
 * list of its own, waited for: the frame's allocators may be in flight.
 * One byte of coverage a texel, row 0 the first row of the bake, as on
 * Vulkan. */
static FnlResult build_atlas(App *a, int si)
{
	struct RendererState *g = D3D(a);
	const int aw = (int)FONT_ATLAS_W[si];
	const int ah = (int)FONT_ATLAS_H[si];
	const size_t bytes = (size_t)aw * (size_t)ah;
	const size_t pitch = ((size_t)aw + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
					   & ~(size_t)(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

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

	FnlResult rc = FNL_OK;
	ID3D12Resource *up = NULL;
	ID3D12CommandAllocator *alloc = NULL;
	ID3D12GraphicsCommandList *l = NULL;

	const D3D12_HEAP_PROPERTIES dflt = { .Type = D3D12_HEAP_TYPE_DEFAULT };
	const D3D12_HEAP_PROPERTIES upl = { .Type = D3D12_HEAP_TYPE_UPLOAD };
	const D3D12_RESOURCE_DESC td = {
		.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
		.Width = (UINT64)aw,
		.Height = (UINT)ah,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_R8_UNORM,
		.SampleDesc = { 1, 0 },
		.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
	};
	const D3D12_RESOURCE_DESC bd = {
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Width = (UINT64)(pitch * (size_t)ah),
		.Height = 1,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_UNKNOWN,
		.SampleDesc = { 1, 0 },
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
	};
	if (FAILED(ID3D12Device_CreateCommittedResource(
			g->device, &dflt, D3D12_HEAP_FLAG_NONE, &td,
			D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
			(void **)&g->font_tex)) ||
		FAILED(ID3D12Device_CreateCommittedResource(
			g->device, &upl, D3D12_HEAP_FLAG_NONE, &bd,
			D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
			(void **)&up))) {
		rc = fnl_fail(FNL_ERR_OUT_OF_MEMORY,
					  "no room for the %dx%d glyph atlas", aw, ah);
		goto out;
	}
	unsigned char *map = NULL;
	const D3D12_RANGE none = { 0, 0 };
	if (FAILED(ID3D12Resource_Map(up, 0, &none, (void **)&map))) {
		rc = fnl_fail(FNL_ERR_SYSTEM, "could not map the atlas's upload");
		goto out;
	}
	for (int y = 0; y < ah; y++)
		memcpy(map + (size_t)y * pitch, scratch + (size_t)y * (size_t)aw,
			   (size_t)aw);
	ID3D12Resource_Unmap(up, 0, NULL);

	HRESULT hr = ID3D12Device_CreateCommandAllocator(
		g->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
		&IID_ID3D12CommandAllocator, (void **)&alloc);
	if (SUCCEEDED(hr))
		hr = ID3D12Device_CreateCommandList(
			g->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
			&IID_ID3D12GraphicsCommandList, (void **)&l);
	if (FAILED(hr)) {
		rc = d3d_fail(FNL_ERR_SYSTEM, "the atlas upload's list", hr);
		goto out;
	}
	const D3D12_TEXTURE_COPY_LOCATION dst = {
		.pResource = g->font_tex,
		.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
		.SubresourceIndex = 0,
	};
	const D3D12_TEXTURE_COPY_LOCATION src = {
		.pResource = up,
		.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
		.PlacedFootprint = {
			.Offset = 0,
			.Footprint = {
				.Format = DXGI_FORMAT_R8_UNORM,
				.Width = (UINT)aw,
				.Height = (UINT)ah,
				.Depth = 1,
				.RowPitch = (UINT)pitch,
			},
		},
	};
	ID3D12GraphicsCommandList_CopyTextureRegion(l, &dst, 0, 0, 0, &src,
												NULL);
	const D3D12_RESOURCE_BARRIER to_read = {
		.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
		.Transition = {
			.pResource   = g->font_tex,
			.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
			.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
			.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		},
	};
	ID3D12GraphicsCommandList_ResourceBarrier(l, 1, &to_read);
	rc = d3d_run_once(a, l);
	if (rc < 0)
		goto out;

	const D3D12_SHADER_RESOURCE_VIEW_DESC v = {
		.Format = DXGI_FORMAT_R8_UNORM,
		.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
		.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
		.Texture2D = { .MipLevels = 1 },
	};
	ID3D12Device_CreateShaderResourceView(g->device, g->font_tex, &v,
										  d3d_srv_cpu(a, SRV_ATLAS));
	a->font_si      = si;
	a->font_atlas_w = aw;
	a->font_atlas_h = ah;
out:
	free(scratch);
	D3D_RELEASE(l);
	D3D_RELEASE(alloc);
	D3D_RELEASE(up);
	if (rc < 0)
		destroy_atlas(a);
	return rc;
}

/* Render thread, when it latches a new scale. The old texture may be read
 * by a frame in flight, and Direct3D 12 keeps nothing alive for it, so the
 * queue is drained first. Compared by atlas index, as on Vulkan. */
FnlResult d3d_text_set_scale(App *a, int s120)
{
	if (!D3D(a)->pso_text)
		return FNL_OK;
	const int si = scale_index(s120);
	if (si == a->font_si)
		return FNL_OK;
	/* A loss here is flagged (App::device_lost) and the atlas left alone,
	 * as on Vulkan: the rebuild that follows answers it and uploads the
	 * atlas for the scale then current. Not a failure, which the core
	 * would log as a scale with no atlas. */
	if (!d3d_wait_idle(a))
		return FNL_OK;
	destroy_atlas(a);
	return build_atlas(a, si);
}

/* ---- building and tearing down ------------------------------------------- */

/* One arena of `bytes` per frame slot, in the upload heap and mapped for
 * good: the core appends straight into it, and the slot's fence (frame.c)
 * says when the GPU is done reading it. The upload heap is write-combined
 * and coherent, so nothing is flushed between the append and the draw. */
static FnlResult make_arenas(App *a, ID3D12Resource **bufs, size_t bytes,
							 void **maps)
{
	const D3D12_HEAP_PROPERTIES upl = { .Type = D3D12_HEAP_TYPE_UPLOAD };
	const D3D12_RESOURCE_DESC d = {
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Width = (UINT64)bytes,
		.Height = 1,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_UNKNOWN,
		.SampleDesc = { 1, 0 },
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
	};
	const D3D12_RANGE none = { 0, 0 };
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (FAILED(ID3D12Device_CreateCommittedResource(
				D3D(a)->device, &upl, D3D12_HEAP_FLAG_NONE, &d,
				D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
				(void **)&bufs[i])) ||
			FAILED(ID3D12Resource_Map(bufs[i], 0, &none, &maps[i]))) {
			maps[i] = NULL;
			return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
							"could not map a %zu-byte overlay arena", bytes);
		}
	}
	return FNL_OK;
}

static void destroy_arenas(ID3D12Resource **bufs, void **maps)
{
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		D3D_RELEASE(bufs[i]);           /* unmaps it too */
		maps[i] = NULL;
	}
}

FnlResult d3d_create_overlay(App *a)
{
	struct RendererState *g = D3D(a);
	FNL_TRY(make_root(a));

	const size_t rect[2] = { offsetof(RectInstance, rect),
							 offsetof(RectInstance, color) };
	const size_t shape[3] = { offsetof(ShapeInstance, p0),
							  offsetof(ShapeInstance, p2),
							  offsetof(ShapeInstance, color) };
	const size_t text[3] = { offsetof(GlyphInstance, rect),
							 offsetof(GlyphInstance, uv),
							 offsetof(GlyphInstance, color) };
	const PsoDesc descs[6] = {
		{ "rectangle", BYTES(rect_vert_dxil), BYTES(rect_frag_dxil),
		  rect, 2, BLEND_NONE },
		{ "stroke", BYTES(shape_vert_dxil), BYTES(shape_frag_dxil),
		  shape, 3, BLEND_ALPHA },
		/* The subpixel variant always: dual-source blending is core in
		 * Direct3D 12, where Vulkan has it as an optional feature. */
		{ "text", BYTES(text_vert_dxil), BYTES(text_lcd_frag_dxil),
		  text, 3, BLEND_DUAL_SRC },
		{ "fetch", BYTES(fullscreen_vert_dxil), BYTES(blit_frag_dxil),
		  NULL, 0, BLEND_NONE },
		{ "MLAA", BYTES(fullscreen_vert_dxil), BYTES(mlaa_frag_dxil),
		  NULL, 0, BLEND_NONE },
		/* The cover: the fetch once more, blended by a constant, for the
		 * frame read back before a move (d3d_overlay_cover()). */
		{ "cover", BYTES(fullscreen_vert_dxil), BYTES(blit_frag_dxil),
		  NULL, 0, BLEND_CONSTANT },
	};
	ID3D12PipelineState **out[6] = { &g->pso_rect, &g->pso_shape,
									 &g->pso_text, &g->pso_blit,
									 &g->pso_mlaa, &g->pso_cover };
	for (int i = 0; i < 6; i++) {
		*out[i] = make_pso(a, &descs[i]);
		if (!*out[i])
			return fnl_fail(FNL_ERR_SYSTEM,
							"the overlay's pipeline states did not build");
	}

	FNL_TRY(make_arenas(a, g->text_buf, TEXT_ARENA_BYTES,
						(void **)a->text_map));
	FNL_TRY(make_arenas(a, g->ovl_buf, OVERLAY_ARENA_BYTES,
						(void **)a->ovl_map));

	/* The scale is 120 until the render thread latches another, as on
	 * Vulkan. */
	a->font_si = -1;
	return build_atlas(a, scale_index(scale120(a)));
}

void d3d_destroy_overlay(App *a)
{
	struct RendererState *g = D3D(a);
	destroy_atlas(a);
	destroy_arenas(g->text_buf, (void **)a->text_map);
	destroy_arenas(g->ovl_buf, (void **)a->ovl_map);
	D3D_RELEASE(g->cover_tex);
	g->cover_gen = 0;
	g->cover_live = false;
	D3D_RELEASE(g->snap_read);
	g->snap_read_bytes = 0;
	D3D_RELEASE(g->pso_cover);
	D3D_RELEASE(g->pso_rect);
	D3D_RELEASE(g->pso_shape);
	D3D_RELEASE(g->pso_text);
	D3D_RELEASE(g->pso_blit);
	D3D_RELEASE(g->pso_mlaa);
	D3D_RELEASE(g->root);
}

/* ---- drawing ------------------------------------------------------------- */

void d3d_own_state(App *a)
{
	struct RendererState *g = D3D(a);
	ID3D12GraphicsCommandList *l = g->list;
	ID3D12DescriptorHeap *heaps[2] = { g->srv_heap, g->smp_heap };
	ID3D12GraphicsCommandList_SetDescriptorHeaps(l, 2, heaps);
	ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, g->root);
	ID3D12GraphicsCommandList_IASetPrimitiveTopology(
		l, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
}

void d3d_fetch(App *a, ID3D12PipelineState *pso, int srv)
{
	ID3D12GraphicsCommandList *l = D3D(a)->list;
	ID3D12GraphicsCommandList_SetPipelineState(l, pso);
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
		l, ROOT_SRV, d3d_srv_gpu(a, srv));
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
		l, ROOT_SAMPLER, d3d_smp_gpu(a, SMP_LINEAR));
	ID3D12GraphicsCommandList_IASetPrimitiveTopology(
		l, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
	ID3D12GraphicsCommandList_IASetPrimitiveTopology(
		l, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
}

/* The instances appended since this kind was last flushed: the arena from
 * the kind's offset as the vertex buffer and the first instance where the
 * last flush stopped, so a second flush reads on with nothing copied, as on
 * Vulkan. */
static void flush(App *a, ID3D12PipelineState *pso, ID3D12Resource *buf,
				  size_t offset, size_t bytes, size_t stride,
				  const uint32_t *count, uint32_t *drawn)
{
	if (*count == *drawn || !D3D(a)->overlay_on || !pso || !buf)
		return;
	ID3D12GraphicsCommandList *l = D3D(a)->list;
	const D3D12_VERTEX_BUFFER_VIEW v = {
		.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buf) + offset,
		.SizeInBytes = (UINT)(bytes - offset),
		.StrideInBytes = (UINT)stride,
	};
	ID3D12GraphicsCommandList_SetPipelineState(l, pso);
	ID3D12GraphicsCommandList_IASetVertexBuffers(l, 0, 1, &v);
	const D3dOverlayPush push = { .screen = { (float)a->extent.w,
											  (float)a->extent.h } };
	ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(
		l, ROOT_PUSH, sizeof push / 4, &push, 0);
	ID3D12GraphicsCommandList_DrawInstanced(l, 4, *count - *drawn, 0,
											*drawn);
	*drawn = *count;
}

void d3d_overlay_flush_rects(App *a)
{
	flush(a, D3D(a)->pso_rect, D3D(a)->ovl_buf[a->ovl_img], 0,
		  OVERLAY_ARENA_BYTES, sizeof(RectInstance), &a->rect_count,
		  &a->rect_drawn);
}

void d3d_overlay_flush_shapes(App *a)
{
	flush(a, D3D(a)->pso_shape, D3D(a)->ovl_buf[a->ovl_img],
		  OVERLAY_SHAPE_OFF, OVERLAY_ARENA_BYTES, sizeof(ShapeInstance),
		  &a->shape_count, &a->shape_drawn);
}

/* NULL restores the whole image. Row 0 is the top, as on Vulkan. */
void d3d_overlay_scissor(App *a, const FnlRect *r)
{
	if (!D3D(a)->overlay_on)
		return;
	const D3D12_RECT s = r ? (D3D12_RECT){ r->x, r->y, r->x + r->w,
										   r->y + r->h }
						   : (D3D12_RECT){ 0, 0, a->extent.w, a->extent.h };
	ID3D12GraphicsCommandList_RSSetScissorRects(D3D(a)->list, 1, &s);
}

/* ---- the cover ----------------------------------------------------------- */

/* App::snap into a texture of this device, as build_atlas() uploads the
 * atlas: BGRA, the snapshot's order (app_snapshot_set()), as the
 * texture's format, and _SRGB as every colour view is, so the bytes
 * round-trip through the sampler and the target unchanged. Rows on the
 * pitch alignment in the upload buffer; the list waited for. The one before,
 * from an earlier move on this device, goes after a drain: Direct3D 12
 * keeps nothing alive for a list in flight. */
static bool cover_upload(App *a)
{
	struct RendererState *g = D3D(a);
	if (g->cover_tex) {
		if (!d3d_wait_idle(a))
			return false;
		D3D_RELEASE(g->cover_tex);
	}
	const DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	const int w = a->snap_w, h = a->snap_h;
	const size_t row = (size_t)w * 4u;
	const size_t pitch = (row + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
					   & ~(size_t)(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

	bool ok = false;
	ID3D12Resource *up = NULL;
	ID3D12CommandAllocator *alloc = NULL;
	ID3D12GraphicsCommandList *l = NULL;

	const D3D12_HEAP_PROPERTIES dflt = { .Type = D3D12_HEAP_TYPE_DEFAULT };
	const D3D12_HEAP_PROPERTIES upl = { .Type = D3D12_HEAP_TYPE_UPLOAD };
	const D3D12_RESOURCE_DESC td = {
		.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
		.Width = (UINT64)w,
		.Height = (UINT)h,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = fmt,
		.SampleDesc = { 1, 0 },
		.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
	};
	const D3D12_RESOURCE_DESC bd = {
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Width = (UINT64)(pitch * (size_t)h),
		.Height = 1,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_UNKNOWN,
		.SampleDesc = { 1, 0 },
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
	};
	if (FAILED(ID3D12Device_CreateCommittedResource(
			g->device, &dflt, D3D12_HEAP_FLAG_NONE, &td,
			D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
			(void **)&g->cover_tex)) ||
		FAILED(ID3D12Device_CreateCommittedResource(
			g->device, &upl, D3D12_HEAP_FLAG_NONE, &bd,
			D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
			(void **)&up)))
		goto out;
	unsigned char *map = NULL;
	const D3D12_RANGE none = { 0, 0 };
	if (FAILED(ID3D12Resource_Map(up, 0, &none, (void **)&map)))
		goto out;
	for (int y = 0; y < h; y++)
		memcpy(map + (size_t)y * pitch, a->snap_px + (size_t)y * row, row);
	ID3D12Resource_Unmap(up, 0, NULL);

	HRESULT hr = ID3D12Device_CreateCommandAllocator(
		g->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
		&IID_ID3D12CommandAllocator, (void **)&alloc);
	if (SUCCEEDED(hr))
		hr = ID3D12Device_CreateCommandList(
			g->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
			&IID_ID3D12GraphicsCommandList, (void **)&l);
	if (FAILED(hr))
		goto out;
	const D3D12_TEXTURE_COPY_LOCATION dst = {
		.pResource = g->cover_tex,
		.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
		.SubresourceIndex = 0,
	};
	const D3D12_TEXTURE_COPY_LOCATION src = {
		.pResource = up,
		.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
		.PlacedFootprint = {
			.Offset = 0,
			.Footprint = {
				.Format = fmt,
				.Width = (UINT)w,
				.Height = (UINT)h,
				.Depth = 1,
				.RowPitch = (UINT)pitch,
			},
		},
	};
	ID3D12GraphicsCommandList_CopyTextureRegion(l, &dst, 0, 0, 0, &src,
												NULL);
	const D3D12_RESOURCE_BARRIER to_read = {
		.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
		.Transition = {
			.pResource   = g->cover_tex,
			.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
			.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
			.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		},
	};
	ID3D12GraphicsCommandList_ResourceBarrier(l, 1, &to_read);
	if (d3d_run_once(a, l) < 0)
		goto out;

	const D3D12_SHADER_RESOURCE_VIEW_DESC v = {
		.Format = fmt,
		.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
		.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
		.Texture2D = { .MipLevels = 1 },
	};
	ID3D12Device_CreateShaderResourceView(g->device, g->cover_tex, &v,
										  d3d_srv_cpu(a, SRV_COVER));
	ok = true;
out:
	D3D_RELEASE(l);
	D3D_RELEASE(alloc);
	D3D_RELEASE(up);
	if (!ok)
		D3D_RELEASE(g->cover_tex);
	return ok;
}

/* The cover over the window at `alpha`: the fetch at the cover's pipeline
 * state, its weight the blend factor. Uploaded once per snapshot
 * (App::snap_gen) and tried once. */
bool d3d_overlay_cover(App *a, float alpha)
{
	struct RendererState *g = D3D(a);
	if (!g->overlay_on || !a->snap_valid || !g->pso_cover)
		return false;
	if (g->cover_gen != a->snap_gen) {
		g->cover_gen = a->snap_gen;
		g->cover_live = cover_upload(a);
		if (!g->cover_live)
			fnl_log(FNL_LOG_WARN, "the frame read back before the move "
					"would not upload; the move dissolves from black");
	}
	if (!g->cover_live || !g->cover_tex)
		return false;
	/* All four: the colour blend reads the factor's colour channels, not
	 * its alpha (make_pso()). With the weight in the alpha alone the cover
	 * drew nothing, on both laptops and on WARP, and every move cut. */
	const FLOAT k[4] = { alpha, alpha, alpha, alpha };
	ID3D12GraphicsCommandList_OMSetBlendFactor(g->list, k);
	d3d_fetch(a, g->pso_cover, SRV_COVER);
	return true;
}

/* The seam's cover_release: after a drain, since Direct3D 12 keeps nothing
 * alive for a list in flight. A drain refused (a lost device) keeps both,
 * for the teardown that follows. SRV_COVER is left naming the texture let
 * go, and nothing reads it until cover_upload() writes it again: the
 * texture is null and the cover not live. */
void d3d_cover_release(App *a)
{
	struct RendererState *g = D3D(a);
	if ((!g->cover_tex && !g->snap_read) || !d3d_wait_idle(a))
		return;
	D3D_RELEASE(g->cover_tex);
	g->cover_live = false;
	D3D_RELEASE(g->snap_read);
	g->snap_read_bytes = 0;
}

FnlResult d3d_text_end(App *a)
{
	struct RendererState *g = D3D(a);
	if (a->text_count == a->text_drawn || !g->overlay_on || !g->pso_text ||
		!g->font_tex)
		return FNL_OK;
	ID3D12GraphicsCommandList *l = g->list;
	const D3D12_VERTEX_BUFFER_VIEW v = {
		.BufferLocation =
			ID3D12Resource_GetGPUVirtualAddress(g->text_buf[a->text_img]),
		.SizeInBytes = (UINT)TEXT_ARENA_BYTES,
		.StrideInBytes = (UINT)sizeof(GlyphInstance),
	};
	ID3D12GraphicsCommandList_SetPipelineState(l, g->pso_text);
	ID3D12GraphicsCommandList_IASetVertexBuffers(l, 0, 1, &v);
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
		l, ROOT_SRV, d3d_srv_gpu(a, SRV_ATLAS));
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(
		l, ROOT_SAMPLER, d3d_smp_gpu(a, SMP_NEAREST));

	/* As on Vulkan: the blend space, and the width of one atlas texel, which
	 * is zero -- the grayscale answer -- for a resampled scale. */
	const D3dTextPush push = {
		.screen  = { (float)a->extent.w, (float)a->extent.h },
		.srgb    = a->srgb_surface ? 1u : 0u,
		.texel_u = text_resampled(a) || a->font_atlas_w <= 0
					   ? 0.0f
					   : 1.0f / (float)a->font_atlas_w,
	};
	ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(
		l, ROOT_PUSH, sizeof push / 4, &push, 0);
	ID3D12GraphicsCommandList_DrawInstanced(l, 4,
											a->text_count - a->text_drawn,
											0, a->text_drawn);
	a->text_drawn = a->text_count;
	return FNL_OK;
}

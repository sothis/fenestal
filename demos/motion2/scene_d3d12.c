/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * The cube as Direct3D 12 draws it: one root signature and a pipeline state
 * per scene target, and nothing else on the device -- scene_vulkan.c in
 * Direct3D 12's terms. The shaders are the Vulkan scene's GLSL, compiled
 * to SPIR-V, translated to HLSL by spirv-cross with Y flipped and compiled
 * to DXIL by dxc at build time; cube.vert makes the cube's 36 vertices
 * from SV_VertexID, so there is no buffer, no input layout and no
 * descriptor heap.
 *
 * Two answers here are Direct3D 12's, where scene_vulkan.c has Vulkan's:
 *
 *   per-draw data is root    CubePush is 21 32-bit root constants at b0,
 *   constants                set before the draw: the push block's cbuffer,
 *                            which dxc places at b0.
 *   state is said each time  the command list is shared with the library's
 *                            own drawing, so the root signature, the
 *                            pipeline state and the topology are set on
 *                            every call. Direct3D 12 records every frame
 *                            afresh (fnl_d3d12.h).
 *
 * From C, through COM's C macros (COBJMACROS).
 */

#define COBJMACROS

#include "motion2.h"

#include <fenestal/fnl_d3d12.h>

#include <stdio.h>
#include <string.h>

#include "cube.vert.dxil.h"
#include "cube.frag.dxil.h"

#define CUBE_VERTEX_COUNT 36

#define RELEASE(p)                                                            \
	do {                                                                      \
		if (p) {                                                              \
			IUnknown *u_ = (IUnknown *)(void *)(p);                           \
			u_->lpVtbl->Release(u_);                                          \
			(p) = NULL;                                                       \
		}                                                                     \
	} while (0)

/* One device generation: built in on_gpu_created, released in on_gpu_lost.
 * No lock: every callback runs on a thread the library owns, never two at
 * once. */
static struct {
	ID3D12RootSignature *root;
	ID3D12PipelineState *pso[FNL_TARGET_COUNT];
	bool                 srgb[FNL_TARGET_COUNT];
} g_dev;

/* CubePush as root constants at b0, for the vertex stage alone, which is
 * the only one that reads it. */
static ID3D12RootSignature *make_root(ID3D12Device *dev)
{
	const D3D12_ROOT_PARAMETER p = {
		.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
		.Constants = { .ShaderRegister = 0,
					   .Num32BitValues = sizeof(CubePush) / 4 },
		.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX,
	};
	const D3D12_ROOT_SIGNATURE_DESC d = {
		.NumParameters = 1,
		.pParameters = &p,
	};
	ID3DBlob *blob = NULL;
	if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1,
										   &blob, NULL)))
		return NULL;
	ID3D12RootSignature *r = NULL;
	if (FAILED(ID3D12Device_CreateRootSignature(
			dev, 0, ID3D10Blob_GetBufferPointer(blob),
			ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
			(void **)&r)))
		r = NULL;
	ID3D10Blob_Release(blob);
	return r;
}

/* Depth tested and written, back faces culled. Counter-clockwise is the
 * front, as scene_vulkan.c says for the same shader: the HLSL flips Y, so
 * the picture is Vulkan's and so is its winding (fnl_d3d12.h). */
static ID3D12PipelineState *make_pso(ID3D12Device *dev,
									 const FnlD3D12SceneLayout *layout)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {
		.pRootSignature = g_dev.root,
		.VS = { cube_vert_dxil, sizeof cube_vert_dxil },
		.PS = { cube_frag_dxil, sizeof cube_frag_dxil },
		.SampleMask = UINT32_MAX,
		.RasterizerState = {
			.FillMode = D3D12_FILL_MODE_SOLID,
			.CullMode = D3D12_CULL_MODE_BACK,
			.FrontCounterClockwise = TRUE,
			.DepthClipEnable = TRUE,
		},
		.DepthStencilState = {
			.DepthEnable = TRUE,
			.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL,
			.DepthFunc = D3D12_COMPARISON_FUNC_LESS,
		},
		.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
		.NumRenderTargets = 1,
		.RTVFormats = { (DXGI_FORMAT)layout->color_format },
		.DSVFormat = (DXGI_FORMAT)layout->depth_format,
		.SampleDesc = { (UINT)layout->samples, 0 },
	};
	d.BlendState.RenderTarget[0].RenderTargetWriteMask =
		D3D12_COLOR_WRITE_ENABLE_ALL;
	ID3D12PipelineState *p = NULL;
	if (FAILED(ID3D12Device_CreateGraphicsPipelineState(
			dev, &d, &IID_ID3D12PipelineState, (void **)&p)))
		return NULL;
	return p;
}

/* Everything the device owns, released; safe on a build that stopped
 * halfway. The library has drained the queue before calling. */
void scene_d3d12_on_gpu_lost(void *ud, FnlApp *app)
{
	(void)ud;
	(void)app;
	for (int i = 0; i < FNL_TARGET_COUNT; i++)
		RELEASE(g_dev.pso[i]);
	RELEASE(g_dev.root);
	memset(&g_dev, 0, sizeof g_dev);
}

/* Called once at startup and again after every device loss, with a device
 * that shares nothing with the last one. A failure leaves the window open
 * and the scene black. */
void scene_d3d12_on_gpu_created(void *ud, FnlApp *app)
{
	memset(&g_dev, 0, sizeof g_dev);
	FnlD3D12Gpu gpu;
	if (!fnl_d3d12_gpu(app, &gpu)) {
		fprintf(stderr, "the running renderer is not Direct3D 12\n");
		return;
	}
	g_dev.root = make_root(gpu.device);
	if (!g_dev.root) {
		fprintf(stderr, "could not build the scene's root signature\n");
		scene_d3d12_on_gpu_lost(ud, app);
		return;
	}
	for (int i = 0; i < FNL_TARGET_COUNT; i++) {
		FnlD3D12SceneLayout layout;
		if (!fnl_d3d12_scene_layout(app, (FnlSceneTarget)i, &layout))
			continue;          /* this window cannot use that target */
		/* Whether the target encodes linear to sRGB as it is written,
		 * which decides whether the shader must. */
		g_dev.srgb[i] =
			layout.color_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		g_dev.pso[i] = make_pso(gpu.device, &layout);
	}
}

/* The cube, into the target the library bound -- cleared, with the
 * viewport and scissor set. The rest of the list's state is whatever the
 * library left, so all of it the draw needs is set here. */
void scene_d3d12_draw(FnlApp *app, const FnlFrame *f, CubePush *push)
{
	FnlD3D12Scene sc;
	if (!fnl_d3d12_scene(app, &sc))
		return;
	if (f->target < 0 || f->target >= FNL_TARGET_COUNT ||
		!g_dev.pso[f->target])
		return;

	push->srgb = g_dev.srgb[f->target] ? 1u : 0u;
	ID3D12GraphicsCommandList *l = sc.list;
	ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, g_dev.root);
	ID3D12GraphicsCommandList_SetPipelineState(l, g_dev.pso[f->target]);
	ID3D12GraphicsCommandList_IASetPrimitiveTopology(
		l, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(
		l, 0, sizeof *push / 4, push, 0);
	ID3D12GraphicsCommandList_DrawInstanced(l, CUBE_VERTEX_COUNT, 1, 0, 0);
}

/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * The textured cube as Direct3D 12 draws it: the texture as a resource of
 * its own with a view in a descriptor heap of this file's, one root
 * signature, and a pipeline state per scene target -- scene_vulkan.c in
 * Direct3D 12's terms. The shaders are the Vulkan scene's GLSL, compiled to
 * SPIR-V, translated to HLSL by spirv-cross with Y flipped and compiled to
 * DXIL by dxc at build time; cube.vert makes the cube's 36 vertices from
 * SV_VertexID, so there is no vertex buffer and no input layout.
 *
 * Three answers here are Direct3D 12's, where scene_vulkan.c has Vulkan's:
 *
 *   per-draw data is root    CubePush is 21 32-bit root constants at b0,
 *   constants                set before the draw: the push block's cbuffer,
 *                            which dxc places at b0.
 *   the texture is uploaded  through a buffer in the upload heap and a copy
 *   by a list of its own     on the library's queue, waited for here with a
 *                            fence of this file's: nothing is drawn with it
 *                            until the copy is done.
 *   state is said each time  the command list is shared with the library's
 *                            own drawing, so the heap, the root signature,
 *                            the pipeline state and the topology are set on
 *                            every call. Direct3D 12 records every frame
 *                            afresh (fnl_d3d12.h).
 *
 * The texture and its sampler are t0 and s0, where spirv-cross puts the
 * GLSL's combined sampler2D at binding 0. From C, through COM's C macros
 * (COBJMACROS).
 */

#define COBJMACROS

#include "texture2.h"

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
	ID3D12RootSignature  *root;
	ID3D12PipelineState  *pso[FNL_TARGET_COUNT];
	ID3D12Resource       *tex;
	ID3D12DescriptorHeap *heap;          /* the texture's view */
	bool                  srgb[FNL_TARGET_COUNT];
} g_dev;

/* The root signature's parameters, in order. */
enum { ROOT_PUSH = 0, ROOT_TEX, ROOT_COUNT };

/* CubePush as root constants at b0, for both stages -- cube.frag reads its
 * srgb -- the texture's table at t0, and the sampler at s0 fixed in the
 * signature: linear both ways and clamped at the edges, as scene_vulkan.c's
 * is, since the frame round the checkerboard is the face's edge. */
static ID3D12RootSignature *make_root(ID3D12Device *dev)
{
	const D3D12_DESCRIPTOR_RANGE srv = {
		.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		.NumDescriptors = 1,
	};
	D3D12_ROOT_PARAMETER p[ROOT_COUNT] = {
		[ROOT_PUSH] = {
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
			.Constants = { .ShaderRegister = 0,
						   .Num32BitValues = sizeof(CubePush) / 4 },
			.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
		},
		[ROOT_TEX] = {
			.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
			.DescriptorTable = { 1, &srv },
			.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
		},
	};
	const D3D12_STATIC_SAMPLER_DESC smp = {
		.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
		.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
		.MaxLOD = D3D12_FLOAT32_MAX,
		.ShaderRegister = 0,
		.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
	};
	const D3D12_ROOT_SIGNATURE_DESC d = {
		.NumParameters = ROOT_COUNT,
		.pParameters = p,
		.NumStaticSamplers = 1,
		.pStaticSamplers = &smp,
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

/* A buffer in the upload heap, which the CPU writes and the GPU reads: the
 * texture's way to the device. */
static ID3D12Resource *make_upload(ID3D12Device *dev, size_t bytes)
{
	const D3D12_HEAP_PROPERTIES heap = { .Type = D3D12_HEAP_TYPE_UPLOAD };
	const D3D12_RESOURCE_DESC d = {
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Width = bytes,
		.Height = 1,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.SampleDesc = { 1, 0 },
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
	};
	ID3D12Resource *r = NULL;
	if (FAILED(ID3D12Device_CreateCommittedResource(
			dev, &heap, D3D12_HEAP_FLAG_NONE, &d,
			D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
			(void **)&r)))
		return NULL;
	return r;
}

/* Submit `l` on the library's queue and wait for it with a fence of our
 * own. The queue is shared, so this is ordered against the frames; and it
 * is called from on_gpu_created alone, when nothing else is submitting.
 * The fence and its event are made before the list is submitted: a list
 * that ran with nothing to wait on would still be reading the upload
 * buffer when the caller released it. */
static bool run_and_wait(ID3D12Device *dev, ID3D12CommandQueue *q,
						 ID3D12GraphicsCommandList *l)
{
	ID3D12Fence *f = NULL;
	HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
	bool ok = ev && SUCCEEDED(ID3D12Device_CreateFence(
						dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
						(void **)&f)) &&
			  SUCCEEDED(ID3D12GraphicsCommandList_Close(l));
	if (ok) {
		ID3D12CommandList *lists[1] = { (ID3D12CommandList *)l };
		ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);
		/* Once the copy is queued, a failure here is a device that is
		 * gone, which runs nothing more. */
		if (FAILED(ID3D12CommandQueue_Signal(q, f, 1)) ||
			FAILED(ID3D12Fence_SetEventOnCompletion(f, 1, ev)))
			ok = false;
		else
			WaitForSingleObject(ev, INFINITE);
	}
	RELEASE(f);
	if (ev)
		CloseHandle(ev);
	return ok;
}

/* DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, because the bytes are colours as a
 * picker gives them: sampling then returns linear values, which is what the
 * shader lights. A TEX_SIZE row of four-byte pixels is a whole multiple of
 * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, so the upload is the image as it
 * stands. */
static bool make_texture(ID3D12Device *dev, ID3D12CommandQueue *q)
{
	_Static_assert(TEX_SIZE * 4 % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0,
				   "a texture row must be a whole copy pitch");
	static unsigned char px[TEX_SIZE * TEX_SIZE * 4];
	texture_pixels(px);

	const D3D12_HEAP_PROPERTIES heap = { .Type = D3D12_HEAP_TYPE_DEFAULT };
	const D3D12_RESOURCE_DESC td = {
		.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
		.Width = TEX_SIZE,
		.Height = TEX_SIZE,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
		.SampleDesc = { 1, 0 },
	};
	if (FAILED(ID3D12Device_CreateCommittedResource(
			dev, &heap, D3D12_HEAP_FLAG_NONE, &td,
			D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
			(void **)&g_dev.tex)))
		return false;

	bool ok = false;
	ID3D12Resource *up = make_upload(dev, sizeof px);
	ID3D12CommandAllocator *alloc = NULL;
	ID3D12GraphicsCommandList *l = NULL;
	unsigned char *map = NULL;
	const D3D12_RANGE none = { 0, 0 };
	if (!up || FAILED(ID3D12Resource_Map(up, 0, &none, (void **)&map)))
		goto out;
	memcpy(map, px, sizeof px);
	ID3D12Resource_Unmap(up, 0, NULL);

	if (FAILED(ID3D12Device_CreateCommandAllocator(
			dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
			(void **)&alloc)) ||
		FAILED(ID3D12Device_CreateCommandList(
			dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
			&IID_ID3D12GraphicsCommandList, (void **)&l)))
		goto out;
	const D3D12_TEXTURE_COPY_LOCATION dst = {
		.pResource = g_dev.tex,
		.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
	};
	const D3D12_TEXTURE_COPY_LOCATION src = {
		.pResource = up,
		.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
		.PlacedFootprint.Footprint = {
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			.Width = TEX_SIZE,
			.Height = TEX_SIZE,
			.Depth = 1,
			.RowPitch = TEX_SIZE * 4,
		},
	};
	ID3D12GraphicsCommandList_CopyTextureRegion(l, &dst, 0, 0, 0, &src,
												NULL);
	const D3D12_RESOURCE_BARRIER b = {
		.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
		.Transition = {
			.pResource = g_dev.tex,
			.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
			.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
			.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		},
	};
	ID3D12GraphicsCommandList_ResourceBarrier(l, 1, &b);
	ok = run_and_wait(dev, q, l);
out:
	RELEASE(l);
	RELEASE(alloc);
	RELEASE(up);
	if (!ok)
		return false;

	/* The view, in a heap of this file's own: one shader-visible heap of a
	 * kind is bound at a time, and the library binds its own again after
	 * on_record returns. */
	const D3D12_DESCRIPTOR_HEAP_DESC hd = {
		.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		.NumDescriptors = 1,
		.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
	};
	if (FAILED(ID3D12Device_CreateDescriptorHeap(
			dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&g_dev.heap)))
		return false;
	D3D12_CPU_DESCRIPTOR_HANDLE h;
	g_dev.heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(g_dev.heap, &h);
	ID3D12Device_CreateShaderResourceView(dev, g_dev.tex, NULL, h);
	return true;
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
	RELEASE(g_dev.tex);
	RELEASE(g_dev.heap);
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
	if (!g_dev.root || !make_texture(gpu.device, gpu.queue)) {
		fprintf(stderr, "could not build the scene's root signature or "
						"texture\n");
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
		!g_dev.pso[f->target] || !g_dev.heap)
		return;

	push->srgb = g_dev.srgb[f->target] ? 1u : 0u;
	D3D12_GPU_DESCRIPTOR_HANDLE tex;
	g_dev.heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(g_dev.heap, &tex);

	ID3D12GraphicsCommandList *l = sc.list;
	ID3D12GraphicsCommandList_SetDescriptorHeaps(l, 1, &g_dev.heap);
	ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, g_dev.root);
	ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(l, ROOT_TEX,
															 tex);
	ID3D12GraphicsCommandList_SetPipelineState(l, g_dev.pso[f->target]);
	ID3D12GraphicsCommandList_IASetPrimitiveTopology(
		l, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(
		l, ROOT_PUSH, sizeof *push / 4, push, 0);
	ID3D12GraphicsCommandList_DrawInstanced(l, CUBE_VERTEX_COUNT, 1, 0, 0);
}

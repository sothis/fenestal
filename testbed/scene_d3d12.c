/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The window's scene, as a Fenestal client draws it with Direct3D 12: one root
 * signature, two pipeline states per target and two draws, from the same
 * shaders scene_vulkan.c builds its Vulkan pipelines from -- compiled to
 * SPIR-V, translated to HLSL by spirv-cross and compiled to DXIL by dxc at
 * build time. The pattern and the other scene a second window draws are one
 * fragment shader's two pictures, chosen by the push block's content.
 *
 * As in scene_vulkan.c, this file does not know whether the scene is drawn
 * straight into the window or into a smaller image, whether it is multisampled,
 * or how the frame is paced. As there, a pipeline state belongs to a sample
 * count and a pair of formats, so it builds one per target the library says it
 * might see. And as under GL, the list's state is shared with the library's own
 * drawing, so everything the draws rely on is set on every call.
 */

#define COBJMACROS

#include "testbed.h"

#include <fenestal/fnl_d3d12.h>

#include <dxgi1_4.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "scene.vert.dxil.h"
#include "scene.frag.dxil.h"
#include "star.vert.dxil.h"
#include "star.frag.dxil.h"

/* As in scene_vulkan.c: 36 lit wedges, two triangles each. */
#define STAR_VERTICES (36 * 2 * 3)

/* What the shaders' `Push` block holds, fed as sixteen root constants: the
 * same bytes as scene_vulkan.c's push constant, and pinned the same way. */
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
	ID3D12RootSignature *root;
	/* One pair per target. A null entry is a target this window cannot use
	 * -- the multisampled one on a device with no sample count worth having
	 * -- and the frame will never ask for it. */
	ID3D12PipelineState *pattern[FNL_TARGET_COUNT];
	ID3D12PipelineState *star[FNL_TARGET_COUNT];
	bool srgb;
	/* The adapter DXGI found for the device, for the memory its queries
	 * read; let go with the device. A window's own, since two windows draw on
	 * two devices from two render threads and one shared adapter would be
	 * released by whichever asked last. */
	IDXGIAdapter3 *vram_adapter;
	ID3D12Device  *vram_device;
} Scene;

/* The scene is one per window, as in scene_vulkan.c: a host's second window has
 * its own device and objects, and draws its own rather than overwriting the
 * first's. Keyed by the App; on_gpu_created and on_gpu_lost take and give the
 * slot. */
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

#define RELEASE(p)                                                             \
	do {                                                                       \
		if (p) {                                                               \
			IUnknown *u_ = (IUnknown *)(void *)(p);                            \
			u_->lpVtbl->Release(u_);                                           \
			(p) = NULL;                                                        \
		}                                                                      \
	} while (0)

/* The window's objects, where it made any, and its slot back. */
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
	for (int t = 0; t < FNL_TARGET_COUNT; t++) {
		RELEASE(s->pattern[t]);
		RELEASE(s->star[t]);
	}
	RELEASE(s->root);
	if (s->vram_adapter)
		IDXGIAdapter3_Release(s->vram_adapter);
	memset(s, 0, sizeof *s);
	scenes_lock();
	g_scenes[at].app = NULL;
	scenes_unlock();
}

/* The block as 32-bit root constants at b0, the register spirv-cross gives
 * `cbuffer Push`, and nothing else: both shaders build their geometry from
 * the vertex index. */
static ID3D12RootSignature *make_root(ID3D12Device *dev)
{
	D3D12_ROOT_PARAMETER p = {
		.ParameterType    = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
		.Constants        = { .ShaderRegister = 0,
							  .Num32BitValues = sizeof(ScenePush) / 4 },
		.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
	};
	const D3D12_ROOT_SIGNATURE_DESC d = { .NumParameters = 1,
										  .pParameters   = &p };
	ID3DBlob *blob                    = NULL;
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

/* No depth test, no blending, no culling -- what scene_vulkan.c's
 * pipelines fix -- for the target `layout` describes. The depth format is
 * named although nothing tests it: the library binds a depth buffer, and a
 * pipeline state must say what is bound. */
static ID3D12PipelineState *make_pso(ID3D12Device *dev,
									 ID3D12RootSignature *root,
									 const FnlD3D12SceneLayout *layout,
									 const void *vs, size_t vs_len,
									 const void *ps, size_t ps_len)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {
		.pRootSignature = root,
		.VS = { vs, vs_len },
		.PS = { ps, ps_len },
		.SampleMask = UINT32_MAX,
		.RasterizerState = {
			.FillMode = D3D12_FILL_MODE_SOLID,
			.CullMode = D3D12_CULL_MODE_NONE,
			.DepthClipEnable = TRUE,
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

void scene_d3d12_on_gpu_created(void *ud, FnlApp *app)
{
	(void)ud;
	Scene *s = scene_open(app);
	if (!s) {
		fprintf(stderr, "more scene windows than the testbed draws; "
						"no scene will be drawn\n");
		return;
	}

	FnlD3D12Gpu gpu;
	if (fnl_renderer(app) != FNL_RENDERER_D3D12 || !fnl_d3d12_gpu(app, &gpu)) {
		fprintf(stderr, "the running renderer is not Direct3D 12; "
						"no scene will be drawn\n");
		return;
	}
	s->root = make_root(gpu.device);
	if (!s->root) {
		fprintf(stderr, "the scene's root signature was refused\n");
		return;
	}
	for (int t = 0; t < FNL_TARGET_COUNT; t++) {
		FnlD3D12SceneLayout layout;
		if (!fnl_d3d12_scene_layout(app, (FnlSceneTarget)t, &layout))
			continue; /* this window cannot use that target */
		/* Whether the target holds linear light: hardware-encoded _SRGB, or
		 * HDR's float format that keeps it as written. */
		if (t == FNL_TARGET_DIRECT)
			s->srgb = layout.color_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
					  layout.color_format == DXGI_FORMAT_R16G16B16A16_FLOAT;
		s->pattern[t] = make_pso(gpu.device, s->root, &layout, scene_vert_dxil,
								 sizeof scene_vert_dxil, scene_frag_dxil,
								 sizeof scene_frag_dxil);
		s->star[t]    = make_pso(gpu.device, s->root, &layout, star_vert_dxil,
								 sizeof star_vert_dxil, star_frag_dxil,
								 sizeof star_frag_dxil);
		if (!s->pattern[t] || !s->star[t])
			fprintf(stderr, "a scene pipeline state was refused\n");
	}
}

/* The adapter scene_d3d12_vram() asks, found once per device and kept in the
 * window's scene. Through the vtable for the LUID: mingw-w64's C headers
 * return that struct by an out-pointer, as their inline wrapper shows. NULL
 * where DXGI has none. */
static IDXGIAdapter3 *adapter_of(Scene *s, const FnlD3D12Gpu *g)
{
	ID3D12Device *dev = g->device;
	if (dev != s->vram_device) {
		if (s->vram_adapter)
			IDXGIAdapter3_Release(s->vram_adapter);
		s->vram_adapter = NULL;
		s->vram_device  = dev;
		LUID luid;
		dev->lpVtbl->GetAdapterLuid(dev, &luid);
		IDXGIFactory4 *f = NULL;
		if (SUCCEEDED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&f))) {
			if (FAILED(IDXGIFactory4_EnumAdapterByLuid(
					f, luid, &IID_IDXGIAdapter3, (void **)&s->vram_adapter)))
				s->vram_adapter = NULL;
			IDXGIFactory4_Release(f);
		}
	}
	return s->vram_adapter;
}

/* DXGI's QueryVideoMemoryInfo: this process's use of the adapter's local
 * memory. */
bool scene_d3d12_vram(FnlApp *app, double *mib)
{
	Scene *s = scene_slot(app);
	FnlD3D12Gpu g;
	if (!s || !fnl_d3d12_gpu(app, &g) || !adapter_of(s, &g))
		return false;
	DXGI_QUERY_VIDEO_MEMORY_INFO info;
	if (FAILED(IDXGIAdapter3_QueryVideoMemoryInfo(
			s->vram_adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
		return false;
	*mib = (double)info.CurrentUsage / (1024.0 * 1024.0);
	return true;
}

/* The library has waited for the queue before calling this, so nothing
 * released here is still being drawn with. */
void scene_d3d12_on_gpu_lost(void *ud, FnlApp *app)
{
	(void)ud;
	scene_close(app);
}

/* The scene itself, as in scene_vulkan.c: the pattern over one covering
 * triangle, then the star's wedges as primitives FSAA can sample. Into what the
 * library bound, with the viewport and scissor set. */
void scene_d3d12_on_record(void *ud, FnlApp *app, const FnlFrame *f)
{
	(void)ud;
	Scene *s = scene_slot(app);
	if (!s)
		return;
	FnlD3D12Scene sc;
	if (!fnl_d3d12_scene(app, &sc))
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
	ID3D12GraphicsCommandList *l = sc.list;
	ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, s->root);
	ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, 0, sizeof pc / 4,
															&pc, 0);
	ID3D12GraphicsCommandList_IASetPrimitiveTopology(
		l, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	ID3D12GraphicsCommandList_SetPipelineState(l, s->pattern[f->target]);
	ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
	/* The other scene is one picture and has no star; the star's wedges are
	 * the pattern's alone. */
	if (view.content == 0u) {
		ID3D12GraphicsCommandList_SetPipelineState(l, s->star[f->target]);
		ID3D12GraphicsCommandList_DrawInstanced(l, STAR_VERTICES, 1, 0, 0);
	}
}


/* DXGI names the adapter and not the driver, so the name is the adapter's
 * description -- "NVIDIA GeForce GTX 1050 Ti", "Microsoft Basic Render
 * Driver" for WARP -- and the info the user-mode driver's version, which
 * CheckInterfaceSupport answers for IDXGIDevice alone (the one interface
 * it was defined for, Direct3D 10's); the API is the highest feature level
 * the device reaches. */
bool scene_d3d12_driver(FnlApp *app, TestbedDriver *d)
{
	memset(d, 0, sizeof *d);
	Scene *s = scene_slot(app);
	FnlD3D12Gpu g;
	IDXGIAdapter3 *ad;
	if (!s || !fnl_d3d12_gpu(app, &g) || !(ad = adapter_of(s, &g)))
		return false;
	DXGI_ADAPTER_DESC1 desc;
	if (SUCCEEDED(IDXGIAdapter3_GetDesc1(ad, &desc))) {
		if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, d->name,
								 (int)sizeof d->name, NULL, NULL))
			d->name[0] = '\0';
		d->vendor_id = desc.VendorId;
		d->device_id = desc.DeviceId;
	}
	LARGE_INTEGER umd;
	if (SUCCEEDED(
			IDXGIAdapter3_CheckInterfaceSupport(ad, &IID_IDXGIDevice, &umd)))
		snprintf(d->info, sizeof d->info, "driver %u.%u.%u.%u",
				 (unsigned)(umd.HighPart >> 16 & 0xffff),
				 (unsigned)(umd.HighPart & 0xffff),
				 (unsigned)(umd.LowPart >> 16 & 0xffff),
				 (unsigned)(umd.LowPart & 0xffff));
	static const D3D_FEATURE_LEVEL LEVELS[] = {
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_12_0,
		D3D_FEATURE_LEVEL_12_1,
	};
	D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {
		.NumFeatureLevels        = (UINT)(sizeof LEVELS / sizeof LEVELS[0]),
		.pFeatureLevelsRequested = LEVELS,
	};
	if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(
			g.device, D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl)))
		snprintf(d->api, sizeof d->api, "Direct3D 12, feature level %u_%u",
				 (unsigned)fl.MaxSupportedFeatureLevel >> 12 & 0xf,
				 (unsigned)fl.MaxSupportedFeatureLevel >> 8 & 0xf);
	else
		snprintf(d->api, sizeof d->api, "Direct3D 12");
	return true;
}

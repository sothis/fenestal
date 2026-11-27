/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What the frame is drawn into: the swapchain's buffers and the window's
 * depth at App::extent, and the scene's targets at the scene's own size;
 * the descriptor heaps that name them, at fixed slots (d3d12_state.h).
 *
 * Direct3D 12 releases nothing for you: a resource a frame in flight still
 * uses must outlive that frame. So everything here that is replaced waits
 * for the queue first -- the drain Vulkan's scene_set_size() makes.
 */

#include "core/app.h"

#include "renderer/d3d12/d3d12_internal.h"

/* The largest texture Direct3D 12 promises at feature level 11_0, which is
 * the ceiling the scale is resolved against. */
#define D3D_MAX_EXTENT D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION

static const float BLACK[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

/* A heap's first handle, as these two are the one pair whose C spelling the
 * two header sets disagree on: mingw-w64 inlines functions that return the
 * handle, and the Windows SDK defines macros that fill an out-parameter.
 * The vtable call is the shape both have. */
static D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu(ID3D12DescriptorHeap *heap)
{
	D3D12_CPU_DESCRIPTOR_HANDLE h;
	heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &h);
	return h;
}

static D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu(ID3D12DescriptorHeap *heap)
{
	D3D12_GPU_DESCRIPTOR_HANDLE h;
	heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &h);
	return h;
}

/* ---- descriptors --------------------------------------------------------- */

D3D12_CPU_DESCRIPTOR_HANDLE d3d_rtv(App *a, int slot)
{
	D3D12_CPU_DESCRIPTOR_HANDLE h = heap_cpu(D3D(a)->rtv_heap);
	h.ptr += (SIZE_T)slot * D3D(a)->rtv_inc;
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE d3d_dsv(App *a, int slot)
{
	D3D12_CPU_DESCRIPTOR_HANDLE h = heap_cpu(D3D(a)->dsv_heap);
	h.ptr += (SIZE_T)slot * D3D(a)->dsv_inc;
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE d3d_srv_cpu(App *a, int slot)
{
	D3D12_CPU_DESCRIPTOR_HANDLE h = heap_cpu(D3D(a)->srv_heap);
	h.ptr += (SIZE_T)slot * D3D(a)->srv_inc;
	return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE d3d_srv_gpu(App *a, int slot)
{
	D3D12_GPU_DESCRIPTOR_HANDLE h = heap_gpu(D3D(a)->srv_heap);
	h.ptr += (UINT64)slot * D3D(a)->srv_inc;
	return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE d3d_smp_gpu(App *a, int slot)
{
	D3D12_GPU_DESCRIPTOR_HANDLE h = heap_gpu(D3D(a)->smp_heap);
	h.ptr += (UINT64)slot * D3D(a)->smp_inc;
	return h;
}

FnlResult d3d_make_heap(App *a, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT n,
						bool visible, ID3D12DescriptorHeap **out, UINT *inc)
{
	const D3D12_DESCRIPTOR_HEAP_DESC d = {
		.Type           = type,
		.NumDescriptors = n,
		.Flags          = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
								  : D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
	};
	const HRESULT hr = ID3D12Device_CreateDescriptorHeap(
		D3D(a)->device, &d, &IID_ID3D12DescriptorHeap, (void **)out);
	if (FAILED(hr))
		return d3d_fail(FNL_ERR_OUT_OF_MEMORY, "CreateDescriptorHeap", hr);
	*inc = ID3D12Device_GetDescriptorHandleIncrementSize(D3D(a)->device, type);
	return FNL_OK;
}

/* The four heaps, and the two samplers, which never change: nearest for
 * the atlas, baked for the pixel grid; linear for the scene, the whole
 * filter of the scale. */
FnlResult d3d_make_heaps(App *a)
{
	struct RendererState *g = D3D(a);
	FNL_TRY(d3d_make_heap(a, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_COUNT, false,
						  &g->rtv_heap, &g->rtv_inc));
	FNL_TRY(d3d_make_heap(a, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_COUNT, false,
						  &g->dsv_heap, &g->dsv_inc));
	/* The caller's images take a descriptor each after the library's own
	 * (d3d12_state.h, SRV_HEAP_COUNT), so the one CBV_SRV_UAV heap is wider
	 * than the fixed slots: a second heap of that type may not be bound
	 * beside it. */
	FNL_TRY(d3d_make_heap(a, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
						  SRV_HEAP_COUNT, true, &g->srv_heap, &g->srv_inc));
	FNL_TRY(d3d_make_heap(a, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SMP_COUNT,
						  true, &g->smp_heap, &g->smp_inc));
	const D3D12_FILTER filters[SMP_COUNT] = {
		[SMP_NEAREST] = D3D12_FILTER_MIN_MAG_MIP_POINT,
		[SMP_LINEAR]  = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
		[SMP_LINEAR2] = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
		[SMP_LINEAR3] = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
	};
	for (int i = 0; i < SMP_COUNT; i++) {
		const D3D12_SAMPLER_DESC s = {
			.Filter         = filters[i],
			.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			.MaxLOD         = D3D12_FLOAT32_MAX,
			.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
		};
		D3D12_CPU_DESCRIPTOR_HANDLE h = heap_cpu(g->smp_heap);
		h.ptr += (SIZE_T)i * g->smp_inc;
		ID3D12Device_CreateSampler(g->device, &s, h);
	}
	return FNL_OK;
}

/* ---- resources ----------------------------------------------------------- */

void d3d_barrier(App *a, ID3D12Resource *r, D3D12_RESOURCE_STATES from,
				 D3D12_RESOURCE_STATES to)
{
	if (from == to)
		return;
	const D3D12_RESOURCE_BARRIER b = {
		.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
		.Transition = {
			.pResource   = r,
			.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
			.StateBefore = from,
			.StateAfter  = to,
		},
	};
	ID3D12GraphicsCommandList_ResourceBarrier(D3D(a)->list, 1, &b);
}

/* A 2D image in the GPU's own memory, made in `state`, with the clear value
 * its kind is cleared to so the clear is the fast one. NULL on a refusal. */
static ID3D12Resource *make_image(App *a, DXGI_FORMAT fmt, int w, int h,
								  int samples, bool depth,
								  D3D12_RESOURCE_STATES state)
{
	const D3D12_HEAP_PROPERTIES heap = { .Type = D3D12_HEAP_TYPE_DEFAULT };
	const D3D12_RESOURCE_DESC d      = {
			 .Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
			 .Width            = (UINT64)w,
			 .Height           = (UINT)h,
			 .DepthOrArraySize = 1,
			 .MipLevels        = 1,
			 .Format           = fmt,
			 .SampleDesc       = { (UINT)samples, 0 },
			 .Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN,
			 .Flags            = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
									   : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
	};
	D3D12_CLEAR_VALUE clear = { .Format = fmt };
	if (depth) {
		clear.DepthStencil.Depth = 1.0f;
	} else {
		for (int i = 0; i < 4; i++)
			clear.Color[i] = BLACK[i];
	}
	ID3D12Resource *r = NULL;
	if (FAILED(ID3D12Device_CreateCommittedResource(
			D3D(a)->device, &heap, D3D12_HEAP_FLAG_NONE, &d, state, &clear,
			&IID_ID3D12Resource, (void **)&r)))
		return NULL;
	return r;
}

static void depth_view(App *a, ID3D12Resource *r, int slot, int samples)
{
	const D3D12_DEPTH_STENCIL_VIEW_DESC v = {
		.Format        = D3D_DEPTH_FORMAT,
		.ViewDimension = samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS
									 : D3D12_DSV_DIMENSION_TEXTURE2D,
	};
	ID3D12Device_CreateDepthStencilView(D3D(a)->device, r, &v,
										d3d_dsv(a, slot));
}

static void color_view(App *a, ID3D12Resource *r, int slot, int samples)
{
	const D3D12_RENDER_TARGET_VIEW_DESC v = {
		.Format        = D3D(a)->color_format,
		.ViewDimension = samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS
									 : D3D12_RTV_DIMENSION_TEXTURE2D,
	};
	ID3D12Device_CreateRenderTargetView(D3D(a)->device, r, &v,
										d3d_rtv(a, slot));
}

static void sampled_view(App *a, ID3D12Resource *r, int slot)
{
	const D3D12_SHADER_RESOURCE_VIEW_DESC v = {
		.Format                  = D3D(a)->color_format,
		.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D,
		.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
		.Texture2D               = { .MipLevels = 1 },
	};
	ID3D12Device_CreateShaderResourceView(D3D(a)->device, r, &v,
										  d3d_srv_cpu(a, slot));
}

/* A depth image as a shader resource: the SRV's format is the *resource*
 * format of the D32_FLOAT attachment, which is R32_FLOAT, and the shader
 * reads the depth itself out of .r -- there is no compare here. */
static void depth_sampled_view(App *a, ID3D12Resource *r, int slot)
{
	const D3D12_SHADER_RESOURCE_VIEW_DESC v = {
		.Format                  = DXGI_FORMAT_R32_FLOAT,
		.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D,
		.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
		.Texture2D               = { .MipLevels = 1, .ResourceMinLODClamp = 0 },
	};
	ID3D12Device_CreateShaderResourceView(D3D(a)->device, r, &v,
										 d3d_srv_cpu(a, slot));
}

/* ---- the window's images ------------------------------------------------- */

void d3d_release_window_images(App *a)
{
	for (int i = 0; i < D3D_BUFFERS; i++)
		D3D_RELEASE(D3D(a)->back[i]);
	d3d_via11_release_images(a);
	D3D_RELEASE(D3D(a)->window_depth);
}

/* The swapchain's buffers for the size the frame policy settled, their
 * views, and the window's depth; and the interval vsync asks for, taken
 * here because this is the rebuild the loop asks for when it changes, as
 * Vulkan's swapchain is. ResizeBuffers wants every reference to a buffer
 * gone and the queue done with them, so it waits -- and on Intel's driver
 * it is 18 ms a size (DESIGN *Measured on Windows itself*). So the buffers
 * are made larger than the window (app_buffer_room()), and a size that fits
 * them is drawn into them as they are: with DXGI_SCALING_NONE the window
 * shows a buffer from its corner, one to one, and clips the rest. Not
 * SetSourceSize, which on a window's swapchain stretches the part it names
 * over the whole buffer -- the frame magnified and cut off at the edges
 * (DESIGN *A swapchain larger than the window*). */
/* The size the buffers are to be, and the window's own in `window`: what
 * d3d_window_images() asks, so a swapchain made at it is the one it wants. */
Extent d3d_window_buffer(App *a, Extent *window)
{
	const Extent one  = { 1, 1 };
	const Extent most = { D3D_MAX_EXTENT, D3D_MAX_EXTENT };
	*window           = app_buffer_extent(a, one, most);
	return app_buffer_room(a, *window, most);
}

FnlResult d3d_window_images(App *a)
{
	struct RendererState *g = D3D(a);
	fnl_atomic_store_u32(&a->max_extent_w, D3D_MAX_EXTENT);
	fnl_atomic_store_u32(&a->max_extent_h, D3D_MAX_EXTENT);
	const Extent have = { g->buf_w, g->buf_h };
	Extent e;
	const Extent b     = d3d_window_buffer(a, &e);
	const bool resized = !g->back[0] || !app_buffer_room_fits(a, e, have);
	/* A swapchain born at this size is not resized to it again: on Intel's
	 * driver that is 18 ms at every open. Only the first look at it counts,
	 * and not the Direct3D 11 bridge's, whose buffers are its own. */
	const bool born = g->sc_fresh && !g->back[0] && !g->via11 &&
					  b.w == g->sc_w && b.h == g->sc_h;
	g->sc_fresh     = false;
	if (resized) {
		if (!d3d_wait_idle(a))
			return fnl_fail(FNL_ERR_DEVICE_LOST, "the device was lost "
												 "before a resize");
		d3d_release_window_images(a);
		d3d_shown_idle(a);
		const HRESULT hr = born ? S_OK
								: IDXGISwapChain3_ResizeBuffers(
									  g->swapchain, D3D_BUFFERS, (UINT)b.w,
									  (UINT)b.h, g->back_format, g->sc_flags);
		if (FAILED(hr)) {
			if (d3d_hr_lost(a, hr))
				return fnl_fail(FNL_ERR_DEVICE_LOST, "the device was lost "
													 "in ResizeBuffers");
			/* The buffers are gone already, so as with the parts below:
			 * the loss is raised and recovery builds it all again. */
			if (!d3d_device_lost(a))
				a->device_lost = true;
			return d3d_fail(FNL_ERR_SYSTEM, "ResizeBuffers", hr);
		}
		/* All of them or none: a rebuild that stopped halfway would leave
		 * back[0] and the new size in place, the next one would see
		 * nothing to do, and a frame would clear a depth that is not
		 * there. So a part that is refused takes the rest with it and
		 * raises the loss, and recovery builds the device again. */
		if (g->via11) {
			const FnlResult r = d3d_via11_buffers(a, b.w, b.h);
			if (r != FNL_OK) {
				d3d_release_window_images(a);
				if (!d3d_device_lost(a))
					a->device_lost = true;
				return r;
			}
		}
		for (int i = 0; i < D3D_BUFFERS; i++) {
			if (g->via11) {
				color_view(a, g->back[i], RTV_BACK0 + i, 1);
				continue;
			}
			const HRESULT gb = IDXGISwapChain3_GetBuffer(g->swapchain, (UINT)i,
														 &IID_ID3D12Resource,
														 (void **)&g->back[i]);
			if (FAILED(gb)) {
				g->back[i] = NULL;
				d3d_release_window_images(a);
				if (!d3d_hr_lost(a, gb) && !d3d_device_lost(a))
					a->device_lost = true;
				return d3d_fail(FNL_ERR_SYSTEM, "the swapchain's GetBuffer",
								gb);
			}
			color_view(a, g->back[i], RTV_BACK0 + i, 1);
		}
		g->window_depth = make_image(a, D3D_DEPTH_FORMAT, b.w, b.h, 1, true,
									 D3D12_RESOURCE_STATE_DEPTH_WRITE);
		if (!g->window_depth) {
			d3d_release_window_images(a);
			/* An allocation that failed is not a device gone: raising the
			 * loss would rebuild the device and ask for the same depth for
			 * ever (device-lost@N). A real removal d3d_device_lost() still
			 * sets; a plain shortage leaves the build to fail and the loop
			 * to pace its retry, so a transient one recovers. */
			(void)d3d_device_lost(a);
			return fnl_fail(FNL_ERR_OUT_OF_MEMORY,
							"could not make the "
							"window's %dx%d depth",
							b.w, b.h);
		}
		depth_view(a, g->window_depth, DSV_WINDOW, 1);
		g->buf_w = b.w;
		g->buf_h = b.h;
	}
	a->extent = e;
	/* Larger than the window, for the loop's settle (app_buffer_room()). */
	const Extent got  = { g->buf_w, g->buf_h };
	const Extent none = { 0, 0 };
	a->buffer = g->buf_w != e.w || g->buf_h != e.h ? got : none;
	g->interval = fnl_atomic_load_bool(&a->vsync) ? 1 : 0;
	fnl_atomic_store_u32(&a->present_mode, (uint32_t)g->interval);
	return FNL_OK;
}

/* ---- the scene's targets ------------------------------------------------- */

static void destroy_target(D3dTarget *t)
{
	D3D_RELEASE(t->color);
	D3D_RELEASE(t->depth);
	t->state = D3D12_RESOURCE_STATE_COMMON;
}

/* A colour image -- sampled afterwards where `srv` is a slot, resolved from
 * where it is multisampled -- and its depth where `dsv` is a slot. False
 * leaves a part-built target for destroy_target(). Made as shader resources
 * where they are sampled, which is the state a frame leaves them in. */
static bool make_target(App *a, D3dTarget *t, int w, int h, int samples,
						int rtv, int dsv, int srv)
{
	t->state = (rtv >= 0 && srv < 0)
		? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
		: D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	t->color = make_image(a, D3D(a)->color_format, w, h, samples, false,
						  t->state);
	if (!t->color)
		return false;
	if (rtv >= 0)
	color_view(a, t->color, rtv, samples);
	if (srv >= 0)
		sampled_view(a, t->color, srv);
	if (dsv >= 0) {
		t->depth = make_image(a, D3D_DEPTH_FORMAT, w, h, samples, true,
							  D3D12_RESOURCE_STATE_DEPTH_WRITE);
		if (!t->depth)
			return false;
		depth_view(a, t->depth, dsv, samples);
	}
	return true;
}

static void destroy_scene_target(App *a)
{
	destroy_target(&D3D(a)->scene);
	destroy_target(&D3D(a)->scene_ms);
	destroy_target(&D3D(a)->mlaa);
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		destroy_target(&D3D(a)->taau_hist[i]);
		D3D(a)->taau_hist_ok[i] = false;
	}
	D3D(a)->scene_ms_refused = false;
	D3D(a)->mlaa_refused     = false;
	D3D(a)->taau_refused     = false;
	const Extent none        = { 0, 0 };
	a->scene_extent          = none;
}

/* As Vulkan's scene_ensure_aa(): only what a mode asks for, once it does,
 * and a refusal remembered. Nothing to drain: these are new. */
bool d3d_scene_ensure_aa(App *a, bool want_ms, bool want_mlaa, bool want_taau)
{
	struct RendererState *g = D3D(a);
	if (!g->scene.color)
		return false;
	const int w = a->scene_extent.w, h = a->scene_extent.h;
	bool built = false;
	if (want_ms && g->samples > 1 && !g->scene_ms.color &&
		!g->scene_ms_refused) {
		if (!FNL_FAULT("scene-ms") &&
			make_target(a, &g->scene_ms, w, h, g->samples, RTV_SCENE_MS,
						DSV_SCENE_MS, -1)) {
			built = true;
		} else {
			destroy_target(&g->scene_ms);
			g->scene_ms_refused = true;
			fnl_log(FNL_LOG_WARN,
					"no room for a %dx%d %dx multisampled scene target; "
					"FSAA is off at this size",
					w, h, g->samples);
		}
	}
	if (want_mlaa && !g->mlaa.color && !g->mlaa_refused) {
		if (!FNL_FAULT("scene-mlaa") &&
			make_target(a, &g->mlaa, w, h, 1, RTV_MLAA, -1, SRV_MLAA)) {
			built = true;
		} else {
			destroy_target(&g->mlaa);
			g->mlaa_refused = true;
			fnl_log(FNL_LOG_WARN,
					"no room for a %dx%d filtered scene image; "
					"MLAA is off at this size",
					w, h);
		}
	}
	if (want_taau && !g->taau_hist[0].color && !g->taau_refused) {
		bool all = false;
		if (!FNL_FAULT("scene-taau")) {
			all = true;
			for (int i = 0; all && i < MAX_FRAMES_IN_FLIGHT; i++)
				all = make_target(a, &g->taau_hist[i], w, h, 1, RTV_TAAU + i, -1,
								  SRV_TAAU + i);
			if (all) {
				/* The resolve's two-descriptor table reads consecutive
				 * descriptors, so the pairs are: {scene, the other slot's
				 * image} for each slot. */
				sampled_view(a, g->scene.color, SRV_PAIR0);
				sampled_view(a, g->taau_hist[1].color, SRV_PAIR0_H);
				sampled_view(a, g->scene.color, SRV_PAIR1);
				sampled_view(a, g->taau_hist[0].color, SRV_PAIR1_H);
			}
		}
		if (all) {
			built = true;
		} else {
			for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
				destroy_target(&g->taau_hist[i]);
			g->taau_refused = true;
			fnl_log(FNL_LOG_WARN,
					"no room for the resolved scene images; TAAU is off at "
					"this size");
		}
	}
	return built;
}

/* As Vulkan's scene_set_size(), and with its drain: a refusal leaves no
 * target and says so, and the loop draws the scene at the window's own
 * resolution. */
void d3d_scene_set_size(App *a, int w, int h)
{
	if (!D3D(a)->device)
		return;
	if ((w > 0 ? w : 0) == a->scene_extent.w &&
		(h > 0 ? h : 0) == a->scene_extent.h)
		return;
	if (!d3d_wait_idle(a))
		return;
	destroy_scene_target(a);
	if (w <= 0 || h <= 0)
		return;
	if (FNL_FAULT("scene-target") ||
		!make_target(a, &D3D(a)->scene, w, h, 1, RTV_SCENE, DSV_SCENE,
					 SRV_SCENE)) {
		destroy_scene_target(a);
		fnl_log(FNL_LOG_WARN,
				"no room for a %dx%d scene target; drawing the scene at the "
				"window's own resolution",
				w, h);
		return;
	}
	/* The pairs the resolve's three-descriptor table reads: the scene's view
	 * in each, made wherever the image is, and the depth after it -- the same
	 * depth in both, since the image is one and the slots are per frame. */
	sampled_view(a, D3D(a)->scene.color, SRV_PAIR0);
	sampled_view(a, D3D(a)->scene.color, SRV_PAIR1);
	if (D3D(a)->scene.depth) {
		depth_sampled_view(a, D3D(a)->scene.depth, SRV_PAIR0_D);
		depth_sampled_view(a, D3D(a)->scene.depth, SRV_PAIR1_D);
	}
	const Extent extent = { w, h };
	a->scene_extent = extent;
}

void d3d_destroy_targets(App *a)
{
	destroy_scene_target(a);
}

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
 * for the queue first -- the drain Vulkan's scene_set_size() makes, and
 * which GL's deletes did not need.
 */

#include "core/app.h"

#include "renderer/d3d12/d3d12_internal.h"

/* The largest texture Direct3D 12 promises at feature level 11_0, which is
 * the ceiling the scale is resolved against. */
#define D3D_MAX_EXTENT D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION

static const float BLACK[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

/* ---- descriptors --------------------------------------------------------- */

D3D12_CPU_DESCRIPTOR_HANDLE d3d_rtv(App *a, int slot)
{
	D3D12_CPU_DESCRIPTOR_HANDLE h =
		ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
			D3D(a)->rtv_heap);
	h.ptr += (SIZE_T)slot * D3D(a)->rtv_inc;
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE d3d_dsv(App *a, int slot)
{
	D3D12_CPU_DESCRIPTOR_HANDLE h =
		ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
			D3D(a)->dsv_heap);
	h.ptr += (SIZE_T)slot * D3D(a)->dsv_inc;
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE d3d_srv_cpu(App *a, int slot)
{
	D3D12_CPU_DESCRIPTOR_HANDLE h =
		ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
			D3D(a)->srv_heap);
	h.ptr += (SIZE_T)slot * D3D(a)->srv_inc;
	return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE d3d_srv_gpu(App *a, int slot)
{
	D3D12_GPU_DESCRIPTOR_HANDLE h =
		ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(
			D3D(a)->srv_heap);
	h.ptr += (UINT64)slot * D3D(a)->srv_inc;
	return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE d3d_smp_gpu(App *a, int slot)
{
	D3D12_GPU_DESCRIPTOR_HANDLE h =
		ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(
			D3D(a)->smp_heap);
	h.ptr += (UINT64)slot * D3D(a)->smp_inc;
	return h;
}

static FnlResult make_heap(App *a, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT n,
						   bool visible, ID3D12DescriptorHeap **out,
						   UINT *inc)
{
	const D3D12_DESCRIPTOR_HEAP_DESC d = {
		.Type = type,
		.NumDescriptors = n,
		.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
						 : D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
	};
	const HRESULT hr = ID3D12Device_CreateDescriptorHeap(
		D3D(a)->device, &d, &IID_ID3D12DescriptorHeap, (void **)out);
	if (FAILED(hr))
		return d3d_fail(FNL_ERR_OUT_OF_MEMORY, "CreateDescriptorHeap", hr);
	*inc = ID3D12Device_GetDescriptorHandleIncrementSize(D3D(a)->device,
														 type);
	return FNL_OK;
}

/* The four heaps, and the two samplers, which never change: nearest for
 * the atlas, baked for the pixel grid; linear for the scene, the whole
 * filter of the scale. */
FnlResult d3d_make_heaps(App *a)
{
	struct RendererState *g = D3D(a);
	FNL_TRY(make_heap(a, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_COUNT, false,
					  &g->rtv_heap, &g->rtv_inc));
	FNL_TRY(make_heap(a, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_COUNT, false,
					  &g->dsv_heap, &g->dsv_inc));
	FNL_TRY(make_heap(a, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRV_COUNT,
					  true, &g->srv_heap, &g->srv_inc));
	FNL_TRY(make_heap(a, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SMP_COUNT, true,
					  &g->smp_heap, &g->smp_inc));
	const D3D12_FILTER filters[SMP_COUNT] = {
		[SMP_NEAREST] = D3D12_FILTER_MIN_MAG_MIP_POINT,
		[SMP_LINEAR]  = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
	};
	for (int i = 0; i < SMP_COUNT; i++) {
		const D3D12_SAMPLER_DESC s = {
			.Filter = filters[i],
			.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			.MaxLOD = D3D12_FLOAT32_MAX,
			.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
		};
		D3D12_CPU_DESCRIPTOR_HANDLE h =
			ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
				g->smp_heap);
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
	const D3D12_RESOURCE_DESC d = {
		.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
		.Width = (UINT64)w,
		.Height = (UINT)h,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = fmt,
		.SampleDesc = { (UINT)samples, 0 },
		.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
		.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
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
		.Format = D3D_DEPTH_FORMAT,
		.ViewDimension = samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS
									 : D3D12_DSV_DIMENSION_TEXTURE2D,
	};
	ID3D12Device_CreateDepthStencilView(D3D(a)->device, r, &v,
										d3d_dsv(a, slot));
}

static void color_view(App *a, ID3D12Resource *r, int slot, int samples)
{
	const D3D12_RENDER_TARGET_VIEW_DESC v = {
		.Format = D3D_COLOR_FORMAT,
		.ViewDimension = samples > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS
									 : D3D12_RTV_DIMENSION_TEXTURE2D,
	};
	ID3D12Device_CreateRenderTargetView(D3D(a)->device, r, &v,
										d3d_rtv(a, slot));
}

static void sampled_view(App *a, ID3D12Resource *r, int slot)
{
	const D3D12_SHADER_RESOURCE_VIEW_DESC v = {
		.Format = D3D_COLOR_FORMAT,
		.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
		.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
		.Texture2D = { .MipLevels = 1 },
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
FnlResult d3d_window_images(App *a)
{
	struct RendererState *g = D3D(a);
	fnl_atomic_store_u32(&a->max_extent_w, D3D_MAX_EXTENT);
	fnl_atomic_store_u32(&a->max_extent_h, D3D_MAX_EXTENT);
	const Extent e = app_buffer_extent(a, (Extent){ 1, 1 },
									   (Extent){ D3D_MAX_EXTENT,
												 D3D_MAX_EXTENT });
	const bool resized = !g->back[0] ||
		!app_buffer_room_fits(a, e, (Extent){ g->buf_w, g->buf_h });
	const Extent b = app_buffer_room(a, e, (Extent){ D3D_MAX_EXTENT,
													 D3D_MAX_EXTENT });
	if (resized) {
		if (!d3d_wait_idle(a))
			return fnl_fail(FNL_ERR_DEVICE_LOST, "the device was lost "
							"before a resize");
		d3d_release_window_images(a);
		d3d_shown_idle(a);
		const HRESULT hr = IDXGISwapChain3_ResizeBuffers(
			g->swapchain, D3D_BUFFERS, (UINT)b.w, (UINT)b.h,
			D3D_BACK_FORMAT, g->sc_flags);
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
			const HRESULT gb = IDXGISwapChain3_GetBuffer(
				g->swapchain, (UINT)i, &IID_ID3D12Resource,
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
			if (!d3d_device_lost(a))
				a->device_lost = true;
			return fnl_fail(FNL_ERR_OUT_OF_MEMORY, "could not make the "
							"window's %dx%d depth", b.w, b.h);
		}
		depth_view(a, g->window_depth, DSV_WINDOW, 1);
		g->buf_w = b.w;
		g->buf_h = b.h;
	}
	a->extent = e;
	/* Larger than the window, for the loop's settle (app_buffer_room()). */
	a->buffer = g->buf_w != e.w || g->buf_h != e.h
		? (Extent){ g->buf_w, g->buf_h } : (Extent){ 0, 0 };
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
	t->state = srv >= 0 ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
						: D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
	t->color = make_image(a, D3D_COLOR_FORMAT, w, h, samples, false,
						  t->state);
	if (!t->color)
		return false;
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
	D3D(a)->scene_ms_refused = false;
	D3D(a)->mlaa_refused = false;
	a->scene_extent = (Extent){ 0, 0 };
}

/* As Vulkan's scene_ensure_aa(): only what a mode asks for, once it does,
 * and a refusal remembered. Nothing to drain: these are new. */
bool d3d_scene_ensure_aa(App *a, bool want_ms, bool want_mlaa)
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
					"FSAA is off at this size", w, h, g->samples);
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
					"MLAA is off at this size", w, h);
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
				"window's own resolution", w, h);
		return;
	}
	a->scene_extent = (Extent){ w, h };
}

void d3d_destroy_targets(App *a)
{
	destroy_scene_target(a);
}

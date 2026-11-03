/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Direct3D 12's frames presented through a swapchain of a Direct3D 11
 * device on the same adapter, where DWM composes Direct3D 12's own.
 *
 * A window's frames reach the screen a refresh sooner flipped onto a
 * plane than composed. Where its buffers are the window's size Windows
 * flips a Direct3D 12 swapchain on every GPU measured but one: the HD 530,
 * whose driver is WDDM 2.1, composes it in a window and flips a Direct3D 11
 * swapchain there (DESIGN *Benchmarking*). Nothing says so before the
 * first frames, so the window is measured at rest (frame.c,
 * via11_measure()), and where it is composed the swapchain is made from
 * the Direct3D 11 device instead: DXGI's own interface, the same to every
 * caller of it here -- present, statistics, the latency object, the
 * resize -- the back buffers textures of this device shared with that
 * one, the window's part copied into the swapchain's buffer before each
 * present, as Vulkan's frames are (present_dxgi.c). Measured again there,
 * it stays where that is flipped and goes back where it is not.
 *
 * The two queues are ordered by two shared fences and neither thread
 * waits: the copy waits on the GPU for the frame drawn, and a frame
 * drawn into an image waits on the GPU for the copy that last read it.
 * The images are _SRGB, the format of the views the back buffers are
 * drawn through: a typeless image, which this device could view so as
 * well, Direct3D 11 refuses to open (E_INVALIDARG). The copy moves their
 * bits into the swapchain's UNORM, the same group, unconverted.
 * d3d11.dll is loaded at run time, the link line being the system's own
 * libraries (README).
 */

#include "core/app.h"

#include "renderer/d3d12/d3d12_internal.h"

#include <d3d11_4.h>

#include <stdlib.h>
#include <string.h>

struct D3dVia11 {
	HMODULE               dll;
	ID3D11Device5        *dev;
	ID3D11DeviceContext4 *ctx;
	/* "drawn": signalled by this queue after a frame, waited on by the
	 * copy; "copied": signalled after the copy, waited on by the next
	 * frame drawn into the same image. One object each, both sides. */
	ID3D12Fence          *drawn;
	ID3D11Fence          *drawn11;
	ID3D11Fence          *copied11;
	ID3D12Fence          *copied;
	HANDLE                event;
	uint64_t              drawn_v, copied_v;
	uint64_t              copied_at[D3D_BUFFERS];
	ID3D11Texture2D      *tex[D3D_BUFFERS];
	UINT                  next;
};

int d3d_via11_forced(void)
{
	const char *v = getenv("FNL_D3D12_DXGI11");
	return v && (v[0] == '0' || v[0] == '1') && !v[1] ? v[0] - '0' : -1;
}

static void release_images(struct D3dVia11 *v)
{
	for (int i = 0; i < D3D_BUFFERS; i++) {
		D3D_RELEASE(v->tex[i]);
		v->copied_at[i] = 0;
	}
	v->next = 0;
}

void d3d_via11_end(App *a)
{
	struct RendererState *g = D3D(a);
	struct D3dVia11 *v = g->via11;
	if (!v)
		return;
	release_images(v);
	if (v->ctx) {
		ID3D11DeviceContext4_ClearState(v->ctx);
		ID3D11DeviceContext4_Flush(v->ctx);
	}
	D3D_RELEASE(v->drawn11);
	D3D_RELEASE(v->copied11);
	D3D_RELEASE(v->drawn);
	D3D_RELEASE(v->copied);
	D3D_RELEASE(v->ctx);
	D3D_RELEASE(v->dev);
	if (v->event)
		CloseHandle(v->event);
	if (v->dll)
		FreeLibrary(v->dll);
	free(v);
	g->via11 = NULL;
}

/* The device, its context and the two fences, each made on one side and
 * opened on the other by an NT handle. A failure leaves what was made in
 * g->via11, for d3d_via11_end(). */
static FnlResult via11_open(App *a)
{
	struct RendererState *g = D3D(a);
	struct D3dVia11 *v = calloc(1, sizeof *v);
	if (!v)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY, "the D3D11 present");
	g->via11 = v;
	v->dll = LoadLibraryW(L"d3d11.dll");
	PFN_D3D11_CREATE_DEVICE make = v->dll
		? (PFN_D3D11_CREATE_DEVICE)(void (*)(void))GetProcAddress(
			  v->dll, "D3D11CreateDevice")
		: NULL;
	ID3D11Device *dev = NULL;
	ID3D11DeviceContext *ctx = NULL;
	HRESULT hr = make ? make((IDXGIAdapter *)g->adapter,
							 D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
							 D3D11_SDK_VERSION, &dev, NULL, &ctx)
					  : E_NOINTERFACE;
	if (FAILED(hr))
		return d3d_fail(FNL_ERR_NO_DEVICE, "D3D11CreateDevice", hr);
	hr = ID3D11Device_QueryInterface(dev, &IID_ID3D11Device5,
									 (void **)&v->dev);
	if (SUCCEEDED(hr))
		hr = ID3D11DeviceContext_QueryInterface(
			ctx, &IID_ID3D11DeviceContext4, (void **)&v->ctx);
	ID3D11DeviceContext_Release(ctx);
	ID3D11Device_Release(dev);
	if (FAILED(hr))
		return d3d_fail(FNL_ERR_NO_DEVICE, "no ID3D11Device5", hr);

	HANDLE h = NULL;
	hr = ID3D12Device_CreateFence(g->device, 0, D3D12_FENCE_FLAG_SHARED,
								  &IID_ID3D12Fence, (void **)&v->drawn);
	if (SUCCEEDED(hr))
		hr = ID3D12Device_CreateSharedHandle(
			g->device, (ID3D12DeviceChild *)v->drawn, NULL, GENERIC_ALL,
			NULL, &h);
	if (SUCCEEDED(hr)) {
		hr = ID3D11Device5_OpenSharedFence(v->dev, h, &IID_ID3D11Fence,
										   (void **)&v->drawn11);
		CloseHandle(h);
	}
	if (SUCCEEDED(hr))
		hr = ID3D11Device5_CreateFence(v->dev, 0, D3D11_FENCE_FLAG_SHARED,
									   &IID_ID3D11Fence,
									   (void **)&v->copied11);
	if (SUCCEEDED(hr))
		hr = ID3D11Fence_CreateSharedHandle(v->copied11, NULL, GENERIC_ALL,
											NULL, &h);
	if (SUCCEEDED(hr)) {
		hr = ID3D12Device_OpenSharedHandle(g->device, h, &IID_ID3D12Fence,
										   (void **)&v->copied);
		CloseHandle(h);
	}
	if (FAILED(hr))
		return d3d_fail(FNL_ERR_SYSTEM, "the D3D11 present's fences", hr);
	v->event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!v->event)
		return fnl_fail(FNL_ERR_SYSTEM, "the D3D11 present's event");
	return FNL_OK;
}

FnlResult d3d_via11_open(App *a, IUnknown **by)
{
	struct RendererState *g = D3D(a);
	if (FNL_FAULT("d3d11-present-refused"))
		return fnl_fail(FNL_ERR_NO_DEVICE, "the D3D11 present (forced)");
	if (!g->via11) {
		/* Not kept half-made: the next open would take it for whole. */
		const FnlResult rc = via11_open(a);
		if (rc < 0) {
			d3d_via11_end(a);
			return rc;
		}
		fnl_log(FNL_LOG_INFO, "the frames go through a Direct3D 11 "
				"swapchain, copied there");
	}
	*by = (IUnknown *)g->via11->dev;
	return FNL_OK;
}

void d3d_via11_idle(App *a)
{
	struct D3dVia11 *v = D3D(a)->via11;
	if (!v || !v->ctx || !v->copied)
		return;
	ID3D11DeviceContext4_Flush(v->ctx);
	if (ID3D12Fence_GetCompletedValue(v->copied) >= v->copied_v)
		return;
	ID3D12Fence_SetEventOnCompletion(v->copied, v->copied_v, v->event);
	WaitForSingleObject(v->event, 1000);
}

void d3d_via11_release_images(App *a)
{
	struct D3dVia11 *v = D3D(a)->via11;
	if (v)
		release_images(v);
}

/* The back buffers at w x h: this device's, shared, each opened as a
 * texture there. Ahead of it the swapchain's own buffers were resized,
 * whose size the copy's destination is. */
FnlResult d3d_via11_buffers(App *a, int w, int h)
{
	struct RendererState *g = D3D(a);
	struct D3dVia11 *v = g->via11;
	release_images(v);
	const D3D12_HEAP_PROPERTIES hp = { .Type = D3D12_HEAP_TYPE_DEFAULT };
	const D3D12_RESOURCE_DESC rd = {
		.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
		.Width            = (UINT64)w,
		.Height           = (UINT)h,
		.DepthOrArraySize = 1,
		.MipLevels        = 1,
		.Format           = D3D_COLOR_FORMAT,
		.SampleDesc       = { 1, 0 },
		.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN,
		.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
	};
	/* What a frame clears them to (frame.c, begin_target()): a swapchain's
	 * own buffers are exempt from the debug layer's word on a clear with
	 * no value given at creation, and these are not. */
	const D3D12_CLEAR_VALUE black = {
		.Format = D3D_COLOR_FORMAT,
		.Color  = { 0.0f, 0.0f, 0.0f, 1.0f },
	};
	for (int i = 0; i < D3D_BUFFERS; i++) {
		const char *step = "a shared back buffer";
		HRESULT hr = ID3D12Device_CreateCommittedResource(
			g->device, &hp, D3D12_HEAP_FLAG_SHARED, &rd,
			D3D12_RESOURCE_STATE_COMMON, &black, &IID_ID3D12Resource,
			(void **)&g->back[i]);
		HANDLE sh = NULL;
		if (SUCCEEDED(hr)) {
			step = "a shared back buffer's handle";
			hr = ID3D12Device_CreateSharedHandle(
				g->device, (ID3D12DeviceChild *)g->back[i], NULL,
				GENERIC_ALL, NULL, &sh);
		}
		if (SUCCEEDED(hr)) {
			step = "a shared back buffer opened by Direct3D 11";
			hr = ID3D11Device5_OpenSharedResource1(
				v->dev, sh, &IID_ID3D11Texture2D, (void **)&v->tex[i]);
			CloseHandle(sh);
		}
		if (FAILED(hr))
			return d3d_fail(FNL_ERR_OUT_OF_MEMORY, step, hr);
	}
	return FNL_OK;
}

UINT d3d_via11_next(App *a)
{
	struct D3dVia11 *v = D3D(a)->via11;
	const UINT img = v->next;
	v->next = (v->next + 1) % D3D_BUFFERS;
	return img;
}

void d3d_via11_wait(App *a, UINT img)
{
	struct RendererState *g = D3D(a);
	struct D3dVia11 *v = g->via11;
	if (v->copied_at[img])
		ID3D12CommandQueue_Wait(g->queue, v->copied, v->copied_at[img]);
}

/* After the frame's list: the frame drawn signalled, the copy into the
 * swapchain's buffer waiting for it on the GPU, and the copy's own
 * signal for the frame that next draws into `img`. */
bool d3d_via11_copy(App *a, UINT img)
{
	struct RendererState *g = D3D(a);
	struct D3dVia11 *v = g->via11;
	const uint64_t drawn = ++v->drawn_v;
	if (FAILED(ID3D12CommandQueue_Signal(g->queue, v->drawn, drawn)))
		return false;
	ID3D11DeviceContext4_Wait(v->ctx, v->drawn11, drawn);
	ID3D11Texture2D *back = NULL;
	if (FAILED(IDXGISwapChain3_GetBuffer(g->swapchain, 0,
										 &IID_ID3D11Texture2D,
										 (void **)&back)))
		return false;
	const D3D11_BOX box = { 0, 0, 0, (UINT)a->extent.w, (UINT)a->extent.h,
							1 };
	ID3D11DeviceContext4_CopySubresourceRegion(
		v->ctx, (ID3D11Resource *)back, 0, 0, 0, 0,
		(ID3D11Resource *)v->tex[img], 0, &box);
	ID3D11Texture2D_Release(back);
	const uint64_t copied = ++v->copied_v;
	ID3D11DeviceContext4_Signal(v->ctx, v->copied11, copied);
	/* Sent now, not at the present: a frame that stops short of its
	 * present would leave the copy unsent, and the next frame drawn into
	 * `img` waiting on the GPU for a signal that never comes. */
	ID3D11DeviceContext4_Flush(v->ctx);
	v->copied_at[img] = copied;
	return true;
}

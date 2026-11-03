/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Win32-to-Direct3D 12 bridge: the one file of this renderer that sees the
 * window, and the sibling of src/renderer/vulkan/surface_win32.c. A flip-model
 * swapchain is made from the device's queue and the window, so it belongs
 * to the device and is made again with it.
 */

#include "core/app.h"

#include "renderer/d3d12/d3d12_internal.h"

#include "backend/win32/win32_state.h"

/* Flip discard, the only model Direct3D 12 presents with; the window's own
 * size (0 x 0 asks the window); no scaling, so a resize shows the old
 * frame's pixels where they were rather than stretched; and the
 * frame-latency object, which the frame waits on before it is drawn --
 * one frame queued, the latch as late as the swapchain allows. Tearing is
 * allowed on the swapchain where the factory offers it and vsync is off,
 * which is what lets a present without vsync tear rather than wait; with
 * vsync on the flag costs a refresh on aspire's GTX 960M, 53 ms from latch
 * to screen against 36 without it (schenker's 1050 Ti, behind the Intel
 * panel, 53 either way). DXGI fixes the flag when the swapchain is made,
 * which is enough: vsync is the window's for its life (fnl_init()). */
FnlResult d3d_bridge_swapchain(App *a)
{
	struct RendererState *g = D3D(a);
	const bool tear = g->tearing && !fnl_atomic_load_bool(&a->vsync);
	g->sc_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
				  (tear ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
	DXGI_SWAP_CHAIN_DESC1 d = {
		.Format      = D3D_BACK_FORMAT,
		.SampleDesc  = { 1, 0 },
		.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
		.BufferCount = D3D_BUFFERS,
		.Scaling     = DXGI_SCALING_NONE,
		.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD,
		.AlphaMode   = DXGI_ALPHA_MODE_IGNORE,
		.Flags       = g->sc_flags,
	};
	/* What presents: this device's queue, or a Direct3D 11 device's
	 * (present_d3d11.c). */
	IUnknown *by = (IUnknown *)g->queue;
	if (g->via11_on)
		FNL_TRY(d3d_via11_open(a, &by));
	IDXGISwapChain1 *sc1 = NULL;
	HRESULT hr = FNL_FAULT("d3d12-no-scaling-none")
					 ? E_INVALIDARG
					 : IDXGIFactory4_CreateSwapChainForHwnd(
						   g->factory, by,
						   win32_present_hwnd(a->backend_state), &d, NULL,
						   NULL, &sc1);
	/* A DXGI that will not scale "none" -- older wine -- stretches, and
	 * a resize then shows the old frame stretched until the new one. */
	if (FAILED(hr)) {
		fnl_log(FNL_LOG_WARN, "the swapchain would not leave its frames "
				"unscaled (0x%08lx); they are stretched while the window "
				"resizes", (unsigned long)hr);
		d.Scaling = DXGI_SCALING_STRETCH;
		hr = IDXGIFactory4_CreateSwapChainForHwnd(
			g->factory, by, win32_present_hwnd(a->backend_state), &d, NULL,
			NULL, &sc1);
	}
	if (FAILED(hr))
		return d3d_fail(FNL_ERR_SYSTEM, "CreateSwapChainForHwnd", hr);
	hr = IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3,
										(void **)&g->swapchain);
	IDXGISwapChain1_Release(sc1);
	if (FAILED(hr))
		return d3d_fail(FNL_ERR_SYSTEM, "IDXGISwapChain3", hr);

	/* Fullscreen is the library's (fnl_set_fullscreen()), never DXGI's
	 * exclusive mode behind Alt+Enter. */
	IDXGIFactory4_MakeWindowAssociation(g->factory, a->backend_state->hwnd,
										DXGI_MWA_NO_ALT_ENTER);
	IDXGISwapChain3_SetMaximumFrameLatency(g->swapchain, 1);
	g->latency = 1;
	g->frame_wait = IDXGISwapChain3_GetFrameLatencyWaitableObject(
		g->swapchain);
	g->interval = -1;             /* the next rebuild sets one */
	g->frame_waited = false;
	/* A new swapchain is asked for statistics again (frame.c). */
	g->no_stats = false;
	g->stats_unseen = 0;
	/* Wine: ntdll exports wine_get_version under wine and nowhere else. */
	const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
	g->blank_estimated = nt && GetProcAddress(nt, "wine_get_version");
	g->frame_owed = 0;
	return FNL_OK;
}

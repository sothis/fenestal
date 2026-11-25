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

#include <wchar.h>

/* ---- the output's colour ---------------------------------------------------
 *
 * scRGB where the display is in an advanced colour mode and the swapchain
 * takes it: R16G16B16A16_FLOAT, linear light, whose own 1.0 is 80 nits. The
 * white the desktop shows SDR content at is the user's slider, read from
 * DisplayConfig, and the library's unit is SDR white -- so sdr_scale carries
 * the difference and the library's chrome keeps the brightness of every other
 * window. The peak is the output's own (DXGI_OUTPUT_DESC1::MaxLuminance).
 * Both are read here and nowhere else, before anything is built for them.
 *
 * The Direct3D 11 swapchain (present_d3d11.c) has 8-bit textures and no way to
 * say otherwise, so HDR turns it off rather than going through it. */

/* The three DisplayConfig entry points mingw-w64 does not declare: wingdi.h
 * has the structures, and user32 exports the calls. Fetched by name so a
 * system without them answers "no" rather than failing to load. */
typedef LONG(WINAPI *GetBufferSizesFn)(UINT32, UINT32 *, UINT32 *);
typedef LONG(WINAPI *QueryConfigFn)(UINT32, UINT32 *, DISPLAYCONFIG_PATH_INFO *,
									UINT32 *, DISPLAYCONFIG_MODE_INFO *,
									DISPLAYCONFIG_TOPOLOGY_ID *);
typedef LONG(WINAPI *GetDeviceInfoFn)(DISPLAYCONFIG_DEVICE_INFO_HEADER *);

/* The desktop's SDR white in units of scRGB's (80 nits), by the output's GDI
 * name: DISPLAYCONFIG_SDR_WHITE_LEVEL is in thousandths of 80 nits. Zero
 * where the system will not say, which is "no HDR": a dim picture is worse
 * than an SDR one. */
static float sdr_white_scale(const WCHAR *gdi)
{
	const HMODULE u = GetModuleHandleW(L"user32.dll");
	if (!u)
		return 0.0f;
	const GetBufferSizesFn sizes = (GetBufferSizesFn)(void (*)(void))
		GetProcAddress(u, "GetDisplayConfigBufferSizes");
	const QueryConfigFn query = (QueryConfigFn)(void (*)(void))
		GetProcAddress(u, "QueryDisplayConfig");
	const GetDeviceInfoFn info = (GetDeviceInfoFn)(void (*)(void))
		GetProcAddress(u, "DisplayConfigGetDeviceInfo");
	if (!sizes || !query || !info)
		return 0.0f;

	UINT32 np = 0, nm = 0;
	if (sizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != 0 || np == 0)
		return 0.0f;
	DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof *paths);
	DISPLAYCONFIG_MODE_INFO *modes = calloc(nm ? nm : 1, sizeof *modes);
	float scale                    = 0.0f;
	if (paths && modes &&
		query(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == 0) {
		for (UINT32 i = 0; i < np && scale == 0.0f; i++) {
			DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
			memset(&src, 0, sizeof src);
			src.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
			src.header.size      = sizeof src;
			src.header.adapterId = paths[i].sourceInfo.adapterId;
			src.header.id        = paths[i].sourceInfo.id;
			if (info(&src.header) != 0 ||
				wcscmp(src.viewGdiDeviceName, gdi) != 0)
				continue;
			DISPLAYCONFIG_SDR_WHITE_LEVEL wl;
			memset(&wl, 0, sizeof wl);
			wl.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
			wl.header.size      = sizeof wl;
			wl.header.adapterId = paths[i].targetInfo.adapterId;
			wl.header.id        = paths[i].targetInfo.id;
			if (info(&wl.header) == 0 && wl.SDRWhiteLevel > 0)
				scale = (float)wl.SDRWhiteLevel / 1000.0f;
		}
	}
	free(paths);
	free(modes);
	return scale;
}

/* Whether the window's output is in an advanced colour mode, with the figures
 * read on the way: `true` leaves the output in `*out` for the caller to
 * release, the SDR white scale in `*scale` and the peak in `*peak`, both in
 * scRGB's units. False leaves nothing held. */
static bool colour_probe(App *a, IDXGIOutput **out, float *scale, float *peak)
{
	struct RendererState *g = D3D(a);
	*out                    = NULL;
	const HMONITOR mon =
		MonitorFromWindow(win32_present_hwnd(a->backend_state),
						  MONITOR_DEFAULTTOPRIMARY);
	IDXGIAdapter *ad = (IDXGIAdapter *)(void *)g->adapter;
	IDXGIOutput *o   = NULL;
	for (UINT32 i = 0;; i++) {
		IDXGIOutput *cand = NULL;
		if (FAILED(IDXGIAdapter_EnumOutputs(ad, i, &cand)) || !cand)
			return false;
		DXGI_OUTPUT_DESC d = { 0 };
		const bool mine = SUCCEEDED(IDXGIOutput_GetDesc(cand, &d)) &&
						  d.Monitor == mon;
		if (!mine) {
			IDXGIOutput_Release(cand);
			continue;
		}
		o = cand;
		break;
	}

	IDXGIOutput6 *o6 = NULL;
	DXGI_OUTPUT_DESC1 d1 = { 0 };
	bool advanced = false;
	if (SUCCEEDED(IDXGIOutput_QueryInterface(o, &IID_IDXGIOutput6,
											 (void **)&o6)) &&
		o6 && SUCCEEDED(IDXGIOutput6_GetDesc1(o6, &d1))) {
		switch (d1.ColorSpace) {
		case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
		case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
		case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
		case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020:
			advanced = true;
			break;
		default: break;
		}
	}
	D3D_RELEASE(o6);
	if (!advanced) {
		IDXGIOutput_Release(o);
		return false;
	}

	DXGI_OUTPUT_DESC d0 = { 0 };
	const float s = SUCCEEDED(IDXGIOutput_GetDesc(o, &d0))
						? sdr_white_scale(d0.DeviceName)
						: 0.0f;
	if (!(s > 0.0f)) {
		IDXGIOutput_Release(o);
		return false;
	}
	const float white = 80.0f * s;
	*scale            = s;
	*peak = d1.MaxLuminance > white ? d1.MaxLuminance / white : 1.0f;
	*out              = o;
	return true;
}

/* The swapchain itself, in the format `g` was resolved to. Flip discard, the
 * only model Direct3D 12 presents with; the window's own size (0 x 0 asks the
 * window); no scaling, so a resize shows the old frame's pixels where they
 * were rather than stretched; and the frame-latency object, which the frame
 * waits on before it is drawn -- one frame queued, the latch as late as the
 * swapchain allows. Tearing is allowed on the swapchain where the factory
 * offers it and vsync is off, which is what lets a present without vsync tear
 * rather than wait; with vsync on the flag costs a refresh on aspire's GTX
 * 960M, 53 ms from latch to screen against 36 without it (schenker's 1050 Ti,
 * behind the Intel panel, 53 either way). DXGI fixes the flag when the
 * swapchain is made, which is enough: vsync is the window's for its life
 * (fnl_init()). */
static FnlResult swapchain_create(App *a)
{
	struct RendererState *g = D3D(a);
	DXGI_SWAP_CHAIN_DESC1 d = {
		.Format      = g->back_format,
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
	return FNL_OK;
}

FnlResult d3d_bridge_swapchain(App *a, bool recolour)
{
	struct RendererState *g = D3D(a);
	const bool tear = g->tearing && !fnl_atomic_load_bool(&a->vsync);
	g->sc_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
				  (tear ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);

	IDXGIOutput *out = NULL;
	if (recolour) {
		/* The SDR answer until the output says otherwise. */
		g->back_format  = D3D_BACK_FORMAT;
		g->color_format = D3D_COLOR_FORMAT;
		g->hdr          = false;
		g->sdr_scale    = 1.0f;
		g->peak         = 1.0f;

		const bool asked = app_color_is_hdr(a->want_color);
		if (asked && g->via11_on && a->color_named)
			return fnl_fail(FNL_ERR_UNSUPPORTED,
							"FNL_COLOR=%s was named, and the Direct3D 11 "
							"swapchain presents 8-bit only",
							app_color_key(a->want_color));
		if (asked && !g->via11_on) {
			g->hdr = colour_probe(a, &out, &g->sdr_scale, &g->peak);
			/* The probe's output is its own reference, and nothing below
			 * uses it: released here so the swapchain's failure paths
			 * cannot leak it. */
			D3D_RELEASE(out);
		}
		if (asked && !g->via11_on && !g->hdr)
			fnl_log(FNL_LOG_INFO,
					"this output is not in an advanced colour mode; "
					"presenting SDR");
		if (g->hdr) {
			g->back_format = g->color_format =
				DXGI_FORMAT_R16G16B16A16_FLOAT;
			/* A swapchain is measured at rest as one device's own or as
			 * the Direct3D 11 bridge's; HDR has only the first, and the
			 * choice is the device's for its life. */
			g->via11_on      = false;
			g->via11_forced  = true;
			g->via11_settled = true;
			g->via11_switch  = false;
		}
	}

	FNL_TRY(swapchain_create(a));
	if (g->hdr) {
		UINT support = 0;
		const bool ok =
			SUCCEEDED(IDXGISwapChain3_CheckColorSpaceSupport(
				g->swapchain, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
				&support)) &&
			(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) &&
			SUCCEEDED(IDXGISwapChain3_SetColorSpace1(
				g->swapchain, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
		if (!ok) {
			/* The output is in an advanced mode and the surface will still
			 * not present scRGB: SDR, and the swapchain made again in its
			 * format. Nothing has been drawn into it yet. */
			fnl_log(FNL_LOG_WARN,
					"the swapchain will not present in scRGB; SDR instead");
			D3D_RELEASE(g->swapchain);
			g->hdr         = false;
			g->back_format = D3D_BACK_FORMAT;
			g->color_format = D3D_COLOR_FORMAT;
			g->sdr_scale   = 1.0f;
			g->peak        = 1.0f;
			FNL_TRY(swapchain_create(a));
		}
	}

	/* A named space is taken or refused, as FNL_RENDERER's name is. */
	if (recolour && a->color_named &&
		(FnlColorSpace)(g->hdr ? FNL_COLOR_EXTENDED_LINEAR : FNL_COLOR_SRGB) !=
			a->want_color)
		return fnl_fail(FNL_ERR_UNSUPPORTED,
						"FNL_COLOR=%s was named, and this output presents %s",
						app_color_key(a->want_color),
						app_color_key(g->hdr ? FNL_COLOR_EXTENDED_LINEAR
											 : FNL_COLOR_SRGB));
	if (recolour) {
		app_publish_peak(a, g->hdr ? g->peak : 1.0f);
		app_publish_color(a,
						  g->hdr ? FNL_COLOR_EXTENDED_LINEAR : FNL_COLOR_SRGB,
						  g->sdr_scale);
	}
	if (g->hdr)
		fnl_log(FNL_LOG_INFO,
				"scRGB: sdr white %.2f x 80 nits, peak %.1f nits",
				(double)g->sdr_scale, (double)(g->peak * 80.0f * g->sdr_scale));

	/* Fullscreen is the library's (fnl_set_fullscreen()), never DXGI's
	 * exclusive mode behind Alt+Enter. The window associated with the
	 * swapchain is the one it was made for -- after a move that is the child
	 * filling the client area, not the parent (win32_present_hwnd()). */
	IDXGIFactory4_MakeWindowAssociation(g->factory,
									   win32_present_hwnd(a->backend_state),
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

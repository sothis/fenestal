/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Direct3D 12 device: the factory and the adapter, the device, its queue
 * and fence, the one pair of functions that builds and tears down everything
 * below the factory for startup and loss recovery alike, the seam's table
 * and the public interop.
 *
 * Where the Vulkan renderer has an instance, a device and a swapchain, this
 * one has a DXGI factory, a device and a flip-model swapchain (target.c,
 * surface_win32.c). DESIGN *One shader source* has the conventions the
 * shaders keep; the DXIL is dxc's, compiled at build time.
 */

#include "core/app.h"

#include "renderer/d3d12/d3d12_internal.h"

FnlResult d3d_fail(FnlResult code, const char *what, HRESULT hr)
{
	return fnl_fail(code, "%s failed (0x%08lx)", what, (unsigned long)hr);
}

/* ---- loss ---------------------------------------------------------------- */

bool d3d_hr_lost(App *a, HRESULT hr)
{
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
		hr == DXGI_ERROR_DEVICE_HUNG ||
		hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
		a->device_lost = true;
		return true;
	}
	return false;
}

bool d3d_device_lost(App *a)
{
	if (!D3D(a)->device)
		return false;
	const HRESULT hr = ID3D12Device_GetDeviceRemovedReason(D3D(a)->device);
	if (hr == S_OK)
		return false;
	a->device_lost = true;
	return true;
}

bool d3d_wait_idle(App *a)
{
	struct RendererState *g = D3D(a);
	if (!g->queue || !g->fence)
		return true;
	uint64_t v = g->fenced + 1;
	if (SUCCEEDED(ID3D12CommandQueue_Signal(g->queue, g->fence, v)))
		g->fenced = v;
	else if (d3d_device_lost(a))
		return false;
	else
		v = g->fenced;
	while (ID3D12Fence_GetCompletedValue(g->fence) < v) {
		if (d3d_device_lost(a))
			return false;
		ID3D12Fence_SetEventOnCompletion(g->fence, v, g->fence_event);
		WaitForSingleObject(g->fence_event, FENCE_WAIT_MS);
	}
	d3d_via11_idle(a);
	return true;
}

FnlResult d3d_run_once(App *a, ID3D12GraphicsCommandList *l)
{
	HRESULT hr = ID3D12GraphicsCommandList_Close(l);
	if (FAILED(hr))
		return d3d_fail(FNL_ERR_SYSTEM, "closing an upload's list", hr);
	ID3D12CommandList *lists[1] = { (ID3D12CommandList *)l };
	ID3D12CommandQueue_ExecuteCommandLists(D3D(a)->queue, 1, lists);
	if (!d3d_wait_idle(a))
		return fnl_fail(FNL_ERR_DEVICE_LOST, "the device was lost during "
						"an upload");
	return FNL_OK;
}

/* What the debug layer has said, at warning and above, through the log --
 * as a validation layer's messages reach it on Vulkan. */
void d3d_drain_messages(App *a)
{
	ID3D12InfoQueue *q = D3D(a)->info;
	if (!q)
		return;
	const UINT64 n = ID3D12InfoQueue_GetNumStoredMessages(q);
	for (UINT64 i = 0; i < n; i++) {
		SIZE_T len = 0;
		if (FAILED(ID3D12InfoQueue_GetMessage(q, i, NULL, &len)) || !len)
			continue;
		D3D12_MESSAGE *m = malloc(len);
		if (!m)
			break;
		if (SUCCEEDED(ID3D12InfoQueue_GetMessage(q, i, m, &len)) &&
			m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
			fnl_log(m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR
						? FNL_LOG_ERROR : FNL_LOG_WARN,
					"D3D12: %.*s", (int)m->DescriptionByteLength,
					m->pDescription);
		free(m);
	}
	ID3D12InfoQueue_ClearStoredMessages(q);
}

/* ---- the adapter --------------------------------------------------------- */

static void adapter_name(const DXGI_ADAPTER_DESC1 *d, char *out, size_t n)
{
	if (!WideCharToMultiByte(CP_UTF8, 0, d->Description, -1, out, (int)n,
							 NULL, NULL))
		snprintf(out, n, "(unnamed)");
}

/* The hardware adapter Windows ranks first for performance -- the discrete
 * GPU of a laptop with two, or the one the user chose for this program in
 * Windows' graphics settings -- as its LUID. False where DXGI is older than
 * 1.6 (Windows 10 1803) or ranks none that makes a device. EnumAdapters1's
 * order is the display's, the panel's GPU first, which on a hybrid laptop
 * is the integrated one. */
static bool high_performance_luid(struct RendererState *g, LUID *out)
{
	IDXGIFactory6 *f6 = NULL;
	if (FAILED(IDXGIFactory4_QueryInterface(g->factory, &IID_IDXGIFactory6,
											(void **)&f6)))
		return false;
	bool found = false;
	for (UINT i = 0; !found; i++) {
		IDXGIAdapter1 *ad = NULL;
		if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(
				f6, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
				&IID_IDXGIAdapter1, (void **)&ad)))
			break;
		DXGI_ADAPTER_DESC1 d;
		IDXGIAdapter1_GetDesc1(ad, &d);
		if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
			SUCCEEDED(D3D12CreateDevice((IUnknown *)ad,
										D3D_FEATURE_LEVEL_11_0,
										&IID_ID3D12Device, NULL))) {
			*out = d.AdapterLuid;
			found = true;
		}
		IDXGIAdapter1_Release(ad);
	}
	IDXGIFactory6_Release(f6);
	return found;
}

/* Whether `ad` makes a feature level 11_0 device that builds the library's
 * pipeline states (d3d_draws_on()): a device made for the question and let
 * go, which on Windows is the adapter's one device, the renderer's own
 * where it draws there. The pipeline state is asked under wine alone,
 * whose own vkd3d before 1.16 makes a device and runs no DXIL: on Windows
 * a Direct3D 12 driver runs DXIL to be one at all, and the build cost every
 * list some 100 ms (schenker, three adapters, 2026-10-31). Wine: ntdll
 * exports wine_get_version there and nowhere else. */
static bool adapter_draws(IDXGIAdapter1 *ad)
{
	const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
	const bool wine = nt && GetProcAddress(nt, "wine_get_version");
	ID3D12Device *dev = NULL;
	if (FAILED(D3D12CreateDevice((IUnknown *)ad, D3D_FEATURE_LEVEL_11_0,
								 &IID_ID3D12Device,
								 wine ? (void **)&dev : NULL)))
		return false;
	if (!wine)
		return true;
	const bool ok = d3d_draws_on(dev);
	ID3D12Device_Release(dev);
	return ok;
}

/* The seam's list_gpus: choose_adapter()'s list, from a factory of its own
 * -- another renderer draws, and App::renderer_state is its. */
static int d3d_list_gpus(App *a, GpuEntry *out, int max)
{
	(void)a;
	IDXGIFactory4 *f = NULL;
	if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&f)))
		return 0;
	int n = 0;
	for (UINT i = 0; n < max; i++) {
		IDXGIAdapter1 *ad = NULL;
		if (IDXGIFactory4_EnumAdapters1(f, i, &ad) == DXGI_ERROR_NOT_FOUND)
			break;
		DXGI_ADAPTER_DESC1 d;
		IDXGIAdapter1_GetDesc1(ad, &d);
		if (adapter_draws(ad)) {
			adapter_name(&d, out[n].name, sizeof out[n].name);
			out[n].kind = d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE
							  ? FNL_GPU_KIND_SOFTWARE : FNL_GPU_KIND_UNKNOWN;
			out[n].renderer = FNL_RENDERER_D3D12;
			n++;
		}
		IDXGIAdapter1_Release(ad);
	}
	IDXGIFactory4_Release(f);
	return n;
}

/* Every adapter that makes a feature level 11_0 device and builds the
 * library's pipeline states on it, listed as the Vulkan renderer lists its
 * devices, in EnumAdapters1's order (which FnlInitDesc::
 * gpu_index counts), and the one to use: the name or the index the caller
 * asked for (FnlInitDesc::gpu_index, FNL_GPU), else the most capable
 * hardware adapter, as Vulkan's ranking takes the discrete GPU first, else
 * a software one -- WARP, which every Windows has, is what the guests draw
 * on. */
static FnlResult choose_adapter(App *a)
{
	struct RendererState *g = D3D(a);
	IDXGIAdapter1 *pick = NULL, *first_hw = NULL, *first_sw = NULL;
	LUID best;
	const bool ranked = high_performance_luid(g, &best);
	int n = 0;
	/* The same list, for fnl_gpu_count() and the titlebar: entry i is
	 * gpu_index i + 1. DXGI does not say which hardware is integrated, so
	 * that is left unknown rather than guessed from the memory it reports.
	 * And where in it each candidate is, for the one in use. */
	static FNL_THREAD_LOCAL GpuEntry list[GPU_LIST_MAX];
	int pick_i = -1, hw_i = -1, sw_i = -1, left_out = 0;
	for (UINT i = 0;; i++) {
		IDXGIAdapter1 *ad = NULL;
		if (IDXGIFactory4_EnumAdapters1(g->factory, i, &ad) ==
			DXGI_ERROR_NOT_FOUND)
			break;
		DXGI_ADAPTER_DESC1 d;
		IDXGIAdapter1_GetDesc1(ad, &d);
		if (FAILED(D3D12CreateDevice((IUnknown *)ad, D3D_FEATURE_LEVEL_11_0,
									 &IID_ID3D12Device, NULL))) {
			IDXGIAdapter1_Release(ad);
			continue;
		}
		const bool soft = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
		char name[GPU_NAME_MAX];
		adapter_name(&d, name, sizeof name);
		if (!adapter_draws(ad)) {
			fnl_log(FNL_LOG_INFO, "GPU left out: %s, whose Direct3D 12 "
					"builds none of the library's pipeline states", name);
			left_out++;
			IDXGIAdapter1_Release(ad);
			continue;
		}
		app_gpu_inventory(a, n++, name, soft ? "software" : "hardware");
		const bool asked = (a->gpu_want[0] && app_name_has(name, a->gpu_want))
						|| (a->gpu_index > 0 && a->gpu_index == n);
		if (n <= GPU_LIST_MAX) {
			GpuEntry *e = &list[n - 1];
			snprintf(e->name, sizeof e->name, "%s", name);
			e->kind = soft ? FNL_GPU_KIND_SOFTWARE : FNL_GPU_KIND_UNKNOWN;
			e->renderer = FNL_RENDERER_D3D12;
		}
		if (asked && !pick) {
			IDXGIAdapter1_AddRef(ad);
			pick = ad;
			pick_i = n - 1;
		}
		/* The ranked adapter replaces the first hardware one wherever it
		 * comes in the list; without a ranking the first stays. */
		const bool is_best = ranked && !soft &&
							 d.AdapterLuid.LowPart == best.LowPart &&
							 d.AdapterLuid.HighPart == best.HighPart;
		if (!soft && (!first_hw || is_best)) {
			D3D_RELEASE(first_hw);
			IDXGIAdapter1_AddRef(ad);
			first_hw = ad;
			hw_i = n - 1;
		} else if (soft && !first_sw) {
			IDXGIAdapter1_AddRef(ad);
			first_sw = ad;
			sw_i = n - 1;
		}
		IDXGIAdapter1_Release(ad);
	}
	if (!pick && (a->gpu_want[0] || a->gpu_index > 0))
		fnl_log(FNL_LOG_WARN, "no adapter is the GPU asked for; the first "
				"is used");
	/* The candidate's reference handed to pick; an adapter asked for holds
	 * one of its own, and its candidate's goes below. */
	if (!pick) {
		pick = first_hw ? first_hw : first_sw;
		pick_i = first_hw ? hw_i : sw_i;
		if (pick == first_hw)
			first_hw = NULL;
		if (pick == first_sw)
			first_sw = NULL;
	}
	D3D_RELEASE(first_hw);
	D3D_RELEASE(first_sw);
	if (!pick) {
		/* WARP, where EnumAdapters1 listed none that would do. */
		if (FAILED(IDXGIFactory4_EnumWarpAdapter(g->factory,
												 &IID_IDXGIAdapter1,
												 (void **)&pick)))
			return fnl_fail(FNL_ERR_NO_DEVICE, left_out
							? "no adapter builds the library's Direct3D 12 "
							  "pipeline states, and there is no WARP"
							: "no adapter makes a Direct3D 12 device, WARP "
							  "included");
		if (!adapter_draws(pick)) {
			D3D_RELEASE(pick);
			return fnl_fail(FNL_ERR_NO_DEVICE, "no adapter builds the "
							"library's Direct3D 12 pipeline states, WARP "
							"included");
		}
		/* Not one EnumAdapters1 listed, so not one gpu_index can name:
		 * the list says no GPU of its own is in use. */
		pick_i = -1;
	}
	g->adapter = pick;
	app_gpu_list_publish(a, list, n, pick_i);
	return FNL_OK;
}

/* What FSAA multisamples at: the best of 4x and 2x the device offers for
 * both the colour and the depth format; 1 when it offers neither. */
static void choose_samples(App *a)
{
	int best = 1;
	for (int s = 4; s >= 2 && best == 1; s /= 2) {
		bool both = true;
		const DXGI_FORMAT f[2] = { D3D_COLOR_FORMAT, D3D_DEPTH_FORMAT };
		for (int i = 0; i < 2; i++) {
			D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q = {
				.Format = f[i], .SampleCount = (UINT)s,
			};
			if (FAILED(ID3D12Device_CheckFeatureSupport(
					D3D(a)->device, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
					&q, sizeof q)) || q.NumQualityLevels == 0)
				both = false;
		}
		if (both)
			best = s;
	}
	if (FNL_FAULT("no-msaa"))
		best = 1;
	D3D(a)->samples = best;
	fnl_atomic_store_u32(&a->msaa_samples, (uint32_t)best);
}

/* ---- building and tearing down ------------------------------------------- */

/* The timestamps fnl_frame_gpu_ms() is read from: a heap of two per slot
 * and a readback buffer they are resolved into. Without them the figure is
 * said to be unknown, and nothing else changes. */
static void make_timestamps(App *a)
{
	struct RendererState *g = D3D(a);
	const D3D12_QUERY_HEAP_DESC qd = {
		.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
		.Count = 2 * MAX_FRAMES_IN_FLIGHT,
	};
	const D3D12_HEAP_PROPERTIES rb = { .Type = D3D12_HEAP_TYPE_READBACK };
	const D3D12_RESOURCE_DESC bd = {
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
		.Width = 2 * MAX_FRAMES_IN_FLIGHT * sizeof(UINT64),
		.Height = 1,
		.DepthOrArraySize = 1,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_UNKNOWN,
		.SampleDesc = { 1, 0 },
		.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
	};
	if (FAILED(ID3D12CommandQueue_GetTimestampFrequency(g->queue,
														&g->ts_freq)) ||
		g->ts_freq == 0 ||
		FAILED(ID3D12Device_CreateQueryHeap(g->device, &qd,
											&IID_ID3D12QueryHeap,
											(void **)&g->ts_heap)) ||
		FAILED(ID3D12Device_CreateCommittedResource(
			g->device, &rb, D3D12_HEAP_FLAG_NONE, &bd,
			D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
			(void **)&g->ts_read))) {
		D3D_RELEASE(g->ts_heap);
		D3D_RELEASE(g->ts_read);
	}
	memset(g->ts_unread, 0, sizeof g->ts_unread);
}

/* Everything below the factory, in dependency order. As on Vulkan: no
 * unwinding on failure, since destroy_device_objects() is safe on any
 * prefix -- every pointer is null until the call that made it succeeded. */
static FnlResult create_device_objects(App *a)
{
	struct RendererState *g = D3D(a);
	FNL_TRY(choose_adapter(a));
	HRESULT hr = D3D12CreateDevice((IUnknown *)g->adapter,
								   D3D_FEATURE_LEVEL_11_0,
								   &IID_ID3D12Device, (void **)&g->device);
	if (FAILED(hr))
		return d3d_fail(FNL_ERR_NO_DEVICE, "D3D12CreateDevice", hr);

	DXGI_ADAPTER_DESC1 d;
	IDXGIAdapter1_GetDesc1(g->adapter, &d);
	g->software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
	char name[GPU_NAME_MAX];
	adapter_name(&d, name, sizeof name);
	fnl_mutex_lock(&a->lock);
	snprintf(a->gpu_name, sizeof a->gpu_name, "%s", name);
	fnl_mutex_unlock(&a->lock);
	fnl_log(FNL_LOG_INFO, "GPU in use: %s (Direct3D 12)", name);

#ifndef NDEBUG
	/* Where the debug layer is installed (the Graphics Tools feature on
	 * Windows), its messages: none on a machine without it. */
	if (SUCCEEDED(ID3D12Device_QueryInterface(g->device, &IID_ID3D12InfoQueue,
											  (void **)&g->info)))
		ID3D12InfoQueue_SetMuteDebugOutput(g->info, TRUE);
#endif

	const D3D12_COMMAND_QUEUE_DESC qd = {
		.Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
	};
	hr = ID3D12Device_CreateCommandQueue(g->device, &qd,
										 &IID_ID3D12CommandQueue,
										 (void **)&g->queue);
	if (FAILED(hr))
		return d3d_fail(FNL_ERR_NO_DEVICE, "CreateCommandQueue", hr);
	hr = ID3D12Device_CreateFence(g->device, 0, D3D12_FENCE_FLAG_NONE,
								  &IID_ID3D12Fence, (void **)&g->fence);
	if (FAILED(hr))
		return d3d_fail(FNL_ERR_NO_DEVICE, "CreateFence", hr);
	g->fenced = 0;
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		g->slot_value[i] = 0;
		hr = ID3D12Device_CreateCommandAllocator(
			g->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
			&IID_ID3D12CommandAllocator, (void **)&g->alloc[i]);
		if (FAILED(hr))
			return d3d_fail(FNL_ERR_NO_DEVICE, "CreateCommandAllocator", hr);
	}
	hr = ID3D12Device_CreateCommandList(g->device, 0,
										D3D12_COMMAND_LIST_TYPE_DIRECT,
										g->alloc[0], NULL,
										&IID_ID3D12GraphicsCommandList,
										(void **)&g->list);
	if (FAILED(hr))
		return d3d_fail(FNL_ERR_NO_DEVICE, "CreateCommandList", hr);
	ID3D12GraphicsCommandList_Close(g->list);
	g->recording = false;

	choose_samples(a);
	a->srgb_surface = true;      /* every colour view is _SRGB */
	/* The palette is converted for that blend space; until then every entry
	 * is zero, and the titlebar draws as transparent black. */
	app_palette_init(a);

	make_timestamps(a);
	FNL_TRY(d3d_make_heaps(a));
	/* The device's own swapchain first, measured at rest (frame.c), unless
	 * FNL_D3D12_DXGI11 decides it. */
	const int forced = d3d_via11_forced();
	g->via11_on = forced == 1;
	g->via11_settled = g->via11_forced = forced >= 0;
	g->via11_switch = false;
	g->via11_seen = g->via11_late = 0;
	FNL_TRY(d3d_bridge_swapchain(a));
	/* Vulkan's "framebuffers" has its twin here: a build that fails with
	 * its swapchain on the window, so a window taking the next renderer's
	 * after Direct3D 12's flip model failed can be seen. */
	if (FNL_FAULT("d3d12-after-swapchain"))
		return fnl_fail(FNL_ERR_SYSTEM, "FNL_FAULT: after the swapchain");
	FNL_TRY(d3d_create_overlay(a));
	FNL_TRY(d3d_window_images(a));
	return FNL_OK;
}

/* The reverse, and safe on a lost device: releasing on one is harmless.
 * `save_cache` has nothing to save: no pipeline cache of ours is kept. */
static void destroy_device_objects(App *a, bool save_cache)
{
	(void)save_cache;
	struct RendererState *g = D3D(a);
	/* A loss included: d3d_seam_wait_idle() says why. */
	if (g->device)
		(void)d3d_wait_idle(a);
	d3d_destroy_overlay(a);
	d3d_destroy_targets(a);
	d3d_release_window_images(a);
	if (g->frame_wait) {
		CloseHandle(g->frame_wait);
		g->frame_wait = NULL;
	}
	d3d_shown_stop(a);
	D3D_RELEASE(g->swapchain);
	d3d_via11_end(a);
	D3D_RELEASE(g->rtv_heap);
	D3D_RELEASE(g->dsv_heap);
	D3D_RELEASE(g->srv_heap);
	D3D_RELEASE(g->smp_heap);
	D3D_RELEASE(g->list);
	D3D_RELEASE(g->ts_heap);
	D3D_RELEASE(g->ts_read);
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		D3D_RELEASE(g->alloc[i]);
	D3D_RELEASE(g->fence);
	D3D_RELEASE(g->info);
	D3D_RELEASE(g->queue);
	D3D_RELEASE(g->device);
	D3D_RELEASE(g->adapter);
	g->recording = false;

	/* Per-frame bookkeeping that named the objects just released. */
	a->frame   = 0;
	a->resized = false;
	/* The scene image went too, and a recovery onto the same window size
	 * rebuilds nothing else, so the loop must be told nothing is asked for. */
	a->draw_scene_w = a->draw_scene_h = SCENE_NATIVE;
}

/* Rebuild everything below the factory, as Vulkan's recover_from_loss()
 * does and with its rule: every way out leaves a loss flag raised unless
 * the device is up. The swapchain is below the device here, so a lost
 * surface is a lost device. DESIGN *Device and surface loss*. */
static void recover_from_loss(App *a)
{
	const bool remake = a->surface_remake;
	a->surface_remake = false;
	/* A move to another GPU was said by the loop (gpu_switch_owed()), and
	 * has no driver reset behind it to wait out. */
	const bool quiet = remake || a->gpu_switching;
	if (!quiet)
		fnl_log(FNL_LOG_ERROR, "%s lost -- rebuilding",
				a->surface_lost ? "surface" : "device");

	/* The gap's picture under the swapchain before it goes
	 * (FnlBackendVtbl::present_window_hold). */
	BACKEND(a)->present_window_hold(a);
	destroy_device_objects(a, false);
	a->surface_lost = false;
	a->device_lost  = true;
	/* The swapchain went with the device: a move to another GPU presents
	 * into a window of its own, which a driver before has not claimed
	 * (FnlBackendVtbl::present_window_renew). */
	if (a->gpu_switching)
		BACKEND(a)->present_window_renew(a);

	if (!quiet)
		fnl_sleep_seconds(0.2);

	if (FNL_FAULT("recover-objects") || create_device_objects(a) < 0)
		return;

	a->record_gen++;
	a->device_lost = false;
	fnl_log(FNL_LOG_INFO, "rebuilt on %s", a->gpu_name);
}

/* ---- the renderer seam ------------------------------------------------------
 *
 * This file's half of renderer.h. */

/* The seam's first entry, so the renderer's state is born here, zeroed. The
 * factory, the debug layer before any device, and whether the factory will
 * present with tearing. */
static FnlResult d3d_create_instance(App *a)
{
	FNL_ASSERT(!a->renderer_state);
	a->renderer_state = calloc(1, sizeof *a->renderer_state);
	if (!a->renderer_state)
		return fnl_fail(FNL_ERR_OUT_OF_MEMORY, "out of memory allocating the "
						"Direct3D 12 renderer's state");
	struct RendererState *g = D3D(a);
	UINT flags = 0;
#ifndef NDEBUG
	/* Before the device, or it is not layered. Absent where Windows' Graphics
	 * Tools are not installed, which is said once and is not a failure. */
	ID3D12Debug *dbg = NULL;
	if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&dbg))) {
		ID3D12Debug_EnableDebugLayer(dbg);
		ID3D12Debug_Release(dbg);
		flags = DXGI_CREATE_FACTORY_DEBUG;
	} else {
		fnl_log(FNL_LOG_WARN, "the Direct3D 12 debug layer is not installed");
	}
#endif
	HRESULT hr = CreateDXGIFactory2(flags, &IID_IDXGIFactory4,
									(void **)&g->factory);
	if (FAILED(hr) && flags)
		hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&g->factory);
	if (FAILED(hr)) {
		free(a->renderer_state);
		a->renderer_state = NULL;
		return d3d_fail(FNL_ERR_NO_DEVICE, "CreateDXGIFactory2", hr);
	}
	IDXGIFactory5 *f5 = NULL;
	if (SUCCEEDED(IDXGIFactory4_QueryInterface(g->factory, &IID_IDXGIFactory5,
											   (void **)&f5))) {
		BOOL allow = FALSE;
		if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(
				f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow,
				sizeof allow)))
			g->tearing = allow != FALSE;
		IDXGIFactory5_Release(f5);
	}
	return FNL_OK;
}

static void d3d_destroy_instance(App *a)
{
	if (!a->renderer_state)
		return;
	D3D_RELEASE(D3D(a)->factory);
	if (D3D(a)->fence_event)
		CloseHandle(D3D(a)->fence_event);
	free(a->renderer_state);
	a->renderer_state = NULL;
}

/* The swapchain is made with the device, from its queue, so the bridge's
 * entry has only the fence's event to make: the surface is the window, and
 * the window is the backend's. */
static FnlResult d3d_create_surface(App *a)
{
	if (!D3D(a)->fence_event)
		D3D(a)->fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!D3D(a)->fence_event)
		return fnl_fail(FNL_ERR_SYSTEM, "CreateEvent for the fence failed");
	return FNL_OK;
}

static void d3d_destroy_surface(App *a)
{
	(void)a;
}

/* The swapchain's buffers at the size the frame policy settled, and the
 * interval vsync asks for. False on a loss or a failed resize; the next
 * turn asks again (swapchain_adopt()). */
static bool d3d_recreate_swapchain(App *a)
{
	if (d3d_device_lost(a))
		return false;
	return d3d_window_images(a) >= 0;
}

static bool d3d_present_mode_matches(const App *a, bool vsync)
{
	return (int)fnl_atomic_load_u32(&a->present_mode) == (vsync ? 1 : 0);
}

/* A loss included, as Vulkan's vkDeviceWaitIdle() is: a removed device
 * answers at once (d3d_wait_idle() reads its removed reason), and one only
 * said lost -- the device-lost fault site, a present that failed on a live
 * device -- still has frames in flight, whose objects the caller's
 * on_gpu_lost and the teardown would free under the queue. The debug layer
 * calls that CORRUPTION and raises it, an exception nothing catches. */
static void d3d_seam_wait_idle(App *a)
{
	if (D3D(a)->device)
		(void)d3d_wait_idle(a);
}

static bool d3d_is_lost(const App *a)
{
	return a->device_lost || a->surface_lost;
}

/* No surface here names a display mode. */
static void d3d_remake_surface(App *a)
{
	(void)a;
}

/* DXGI says when a present went nowhere (DXGI_STATUS_OCCLUDED), and a
 * present with DXGI_PRESENT_TEST asks again without showing anything --
 * which is how the cover is found to have gone. */
static bool d3d_occluded(App *a)
{
	struct RendererState *g = D3D(a);
	if (!g->occluded || !g->swapchain)
		return false;
	const HRESULT hr = IDXGISwapChain3_Present(g->swapchain, 0,
											   DXGI_PRESENT_TEST);
	g->occluded = hr == DXGI_STATUS_OCCLUDED;
	return g->occluded;
}

static int d3d_max_samples(const App *a)
{
	return (int)fnl_atomic_load_u32(&a->msaa_samples);
}

/* DXGI's QueryVideoMemoryInfo: this process's use of the adapter's local
 * memory, which on an integrated GPU or WARP is the system's. Asked of
 * IDXGIAdapter3, which every Windows with Direct3D 12 has. */
static bool d3d_gpu_memory(App *a, double *mib)
{
	if (!D3D(a)->adapter)
		return false;
	IDXGIAdapter3 *ad3 = NULL;
	if (FAILED(IDXGIAdapter1_QueryInterface(D3D(a)->adapter,
											&IID_IDXGIAdapter3,
											(void **)&ad3)))
		return false;
	DXGI_QUERY_VIDEO_MEMORY_INFO info;
	const bool ok = SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(
		ad3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info));
	IDXGIAdapter3_Release(ad3);
	if (!ok)
		return false;
	*mib = (double)info.CurrentUsage / (1024.0 * 1024.0);
	return true;
}

static const char *d3d_device_name(const App *app)
{
	/* A per-thread snapshot under App::lock, as the other renderers' are. */
	static FNL_THREAD_LOCAL char snap[GPU_NAME_MAX];
	App *a = (App *)app;
	fnl_mutex_lock(&a->lock);
	memcpy(snap, a->gpu_name, sizeof snap);
	fnl_mutex_unlock(&a->lock);
	snap[sizeof snap - 1] = '\0';
	return snap[0] ? snap : "(no device)";
}

static const char *d3d_present_mode_name(const App *a)
{
	return fnl_atomic_load_u32(&a->present_mode) == 1u
			   ? "sync interval 1 (vsync)"
			   : "sync interval 0";
}

const FnlRendererVtbl fnl_renderer_d3d12 = {
	.name                  = "Direct3D 12",
	.api                   = FNL_RENDERER_D3D12,
	.key                   = "d3d12",
	.draws_overlay         = true,
	.offscreen_scene       = true,
	.create_instance       = d3d_create_instance,
	.destroy_instance      = d3d_destroy_instance,
	.create_surface        = d3d_create_surface,
	.destroy_surface       = d3d_destroy_surface,
	.list_gpus             = d3d_list_gpus,
	/* Any thread may use the device. */
	.thread_enter          = renderer_noop,
	.thread_leave          = renderer_noop,
	.create_device_objects = create_device_objects,
	.destroy_device_objects = destroy_device_objects,
	.recreate_swapchain    = d3d_recreate_swapchain,
	.present_mode_matches  = d3d_present_mode_matches,
	/* Nothing is retired: a resize waits for the queue first (target.c). */
	.retire_tick           = renderer_noop,
	.scale_changed         = d3d_text_set_scale,
	.set_scene_size        = d3d_scene_set_size,
	.draw_frame            = d3d_draw_frame,
	.wait_idle             = d3d_seam_wait_idle,
	.lost                  = d3d_is_lost,
	.recover               = recover_from_loss,
	.remake_surface        = d3d_remake_surface,
	.occluded              = d3d_occluded,
	.max_samples           = d3d_max_samples,
	.active_samples        = d3d_active_samples,
	.gpu_time_poll         = d3d_gpu_time_poll,
	.device_name           = d3d_device_name,
	.gpu_memory            = d3d_gpu_memory,
	.present_mode_name     = d3d_present_mode_name,
	.overlay_flush_rects   = d3d_overlay_flush_rects,
	.overlay_flush_shapes  = d3d_overlay_flush_shapes,
	.overlay_scissor       = d3d_overlay_scissor,
	.text_end              = d3d_text_end,
	.overlay_cover         = d3d_overlay_cover,
	.cover_release         = d3d_cover_release,
};

/* ---- the public Direct3D 12 interop ---------------------------------------
 *
 * include/fenestal/fnl_d3d12.h.
 *
 * Each refuses an App that another renderer draws: a build may have several,
 * and every one's interop is exported. fnl_d3d12_gpu() is the render
 * thread's, since it reads the renderer's state, and refuses any other
 * (app_renderer_state_ours()); fnl_d3d12_scene_layout() any thread's,
 * reading atomics alone. */

bool fnl_d3d12_gpu(App *a, FnlD3D12Gpu *out)
{
	if (!a || !out || !app_renderer_state_ours(a) ||
		renderer_any(a) != &fnl_renderer_d3d12 ||
		!a->renderer_state || !D3D(a)->device)
		return false;
	*out = (FnlD3D12Gpu){ .device = D3D(a)->device, .queue = D3D(a)->queue };
	return true;
}

bool fnl_d3d12_scene_layout(App *a, FnlSceneTarget t,
							FnlD3D12SceneLayout *out)
{
	if (!a || !out || renderer_any(a) != &fnl_renderer_d3d12 ||
		t < 0 || t >= FNL_TARGET_COUNT)
		return false;
	int samples = 1;
	if (t == FNL_TARGET_OFFSCREEN_MS) {
		samples = d3d_max_samples(a);
		if (samples <= 1)
			return false;
	}
	*out = (FnlD3D12SceneLayout){
		.samples      = samples,
		.color_format = D3D_COLOR_FORMAT,
		.depth_format = D3D_DEPTH_FORMAT,
	};
	return true;
}

bool fnl_d3d12_scene(App *a, FnlD3D12Scene *out)
{
	/* on_record's thread alone, tested first as fnl_vulkan_scene_cmd() is. */
	if (!a || !out || app_thread() != APP_THREAD_RENDER ||
		a->renderer != &fnl_renderer_d3d12 ||
		!a->renderer_state || !D3D(a)->scene_list_now)
		return false;
	*out = (FnlD3D12Scene){
		.list = D3D(a)->scene_list_now,
		.rtv  = D3D(a)->scene_rtv_now,
		.dsv  = D3D(a)->scene_dsv_now,
		.rect = D3D(a)->scene_rect_now,
	};
	return true;
}

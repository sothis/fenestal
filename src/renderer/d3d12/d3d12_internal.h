/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What the files of the Direct3D 12 renderer share with each other and with
 * nothing else. src/core/ and src/backend/ reach this directory through
 * FnlRendererVtbl (core/renderer.h) and the few overlay entry points
 * core/app.h declares. The renderer's state is in d3d12_state.h, behind
 * App::renderer_state.
 *
 * In C, through COM's C macros. WIDL_C_INLINE_WRAPPERS is what makes a
 * method returning a structure -- GetCPUDescriptorHandleForHeapStart --
 * callable from C with mingw-w64's headers: the C vtable entry takes the
 * result as a hidden first argument, and the wrapper passes it.
 *
 * Included after "core/app.h", which every .c names first. That has
 * included <windows.h> and with it <unknwn.h> before COBJMACROS is defined
 * here, so IUnknown's own macros are missing and D3D_RELEASE goes through
 * the vtable; the D3D12 and DXGI headers, included below, have theirs.
 */

#ifndef FNL_D3D12_INTERNAL_H
#define FNL_D3D12_INTERNAL_H

#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS

#include "core/app.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include "renderer/d3d12/d3d12_state.h"

#include <fenestal/fnl_d3d12.h>

/* Release a COM object and null the pointer; nothing for a null one. */
#define D3D_RELEASE(p)                                                        \
	do {                                                                      \
		if (p) {                                                              \
			IUnknown *u_ = (IUnknown *)(void *)(p);                           \
			u_->lpVtbl->Release(u_);                                          \
			(p) = NULL;                                                       \
		}                                                                     \
	} while (0)

/* A failed HRESULT as a recorded result, with what failed and the code. */
FNL_MUST_USE FnlResult d3d_fail(FnlResult code, const char *what, HRESULT hr);

/* The formats every image here is made in. The swapchain's buffers are
 * UNORM, as the flip model requires, and drawn through _SRGB views, so a
 * write is encoded and a blend is linear, as on Vulkan's _SRGB surface. */
#define D3D_BACK_FORMAT  DXGI_FORMAT_R8G8B8A8_UNORM
#define D3D_COLOR_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
#define D3D_DEPTH_FORMAT DXGI_FORMAT_D32_FLOAT

/* How long one wait on the queue's fence lasts before the loss is looked
 * for again. */
#define FENCE_WAIT_MS 100

/* ---- device.c ------------------------------------------------------------ */
/* Whether `hr` says the device is gone, which flags App::device_lost; and
 * whether the device says so of itself. */
bool d3d_hr_lost(App *a, HRESULT hr);
bool d3d_device_lost(App *a);
/* Signal the queue and wait until the device has reached it: every
 * submission before this is finished. Where the queue will not take the
 * signal on a live device, the last value it did take is waited for
 * instead: what was submitted after that is behind no fence, and nothing
 * can wait for it. False on a loss. */
bool d3d_wait_idle(App *a);
/* Until the shown waiter is done with the frame handed to it: ahead of
 * ResizeBuffers and of the swapchain's release. */
void d3d_shown_idle(App *a);
/* Ahead of the swapchain's release for good: an ack still held is
 * released and the thread is gone when this returns. */
void d3d_shown_stop(App *a);
/* Close, submit and wait for a list recorded once -- an upload. */
FNL_MUST_USE FnlResult d3d_run_once(App *a, ID3D12GraphicsCommandList *l);
/* A Debug tree's: what the debug layer has said since, into the log. */
void d3d_drain_messages(App *a);

/* ---- surface_win32.c: the bridge ----------------------------------------- */
/* The swapchain for the window, flip model, D3D_BUFFERS buffers at
 * App::extent, its frame-latency object set. */
FNL_MUST_USE FnlResult d3d_bridge_swapchain(App *a);

/* ---- present_d3d11.c: through a Direct3D 11 swapchain ------------------- */
/* FNL_D3D12_DXGI11: 1 always, 0 never, -1 unset, which measures. */
int  d3d_via11_forced(void);
/* The Direct3D 11 device on the adapter, made the first time, as what
 * the swapchain is made from. */
FNL_MUST_USE FnlResult d3d_via11_open(App *a, IUnknown **by);
/* RendererState::back at w x h, shared, after the swapchain's resize. */
FNL_MUST_USE FnlResult d3d_via11_buffers(App *a, int w, int h);
void d3d_via11_release_images(App *a);
/* The image a frame draws into, and the GPU's wait for its last copy,
 * ahead of the frame's list. */
UINT d3d_via11_next(App *a);
void d3d_via11_wait(App *a, UINT img);
/* After the frame's list: its copy into the swapchain's buffer. */
bool d3d_via11_copy(App *a, UINT img);
/* Until the copies made are done. */
void d3d_via11_idle(App *a);
void d3d_via11_end(App *a);

/* ---- target.c ------------------------------------------------------------ */
D3D12_CPU_DESCRIPTOR_HANDLE d3d_rtv(App *a, int slot);
D3D12_CPU_DESCRIPTOR_HANDLE d3d_dsv(App *a, int slot);
D3D12_GPU_DESCRIPTOR_HANDLE d3d_srv_gpu(App *a, int slot);
D3D12_CPU_DESCRIPTOR_HANDLE d3d_srv_cpu(App *a, int slot);
D3D12_GPU_DESCRIPTOR_HANDLE d3d_smp_gpu(App *a, int slot);
FNL_MUST_USE FnlResult d3d_make_heaps(App *a);
/* The swapchain's buffers and the window's depth at the size the frame
 * policy settled, resizing the swapchain when it has another: the
 * rebuild. */
FNL_MUST_USE FnlResult d3d_window_images(App *a);
void d3d_release_window_images(App *a);
void d3d_scene_set_size(App *a, int w, int h);
bool d3d_scene_ensure_aa(App *a, bool want_ms, bool want_mlaa);
void d3d_destroy_targets(App *a);
/* A resource-state transition on the open list. */
void d3d_barrier(App *a, ID3D12Resource *r, D3D12_RESOURCE_STATES from,
				 D3D12_RESOURCE_STATES to);

/* ---- overlay.c ----------------------------------------------------------- */
FNL_MUST_USE FnlResult d3d_create_overlay(App *a);
/* Whether `dev` builds the library's pipeline states, asked of a device
 * made for the question: the adapter lists (device.c) leave out one that
 * does not. Any thread. */
bool d3d_draws_on(ID3D12Device *dev);
void d3d_destroy_overlay(App *a);
FNL_MUST_USE FnlResult d3d_text_set_scale(App *a, int s120);
/* The library's own state on the open list: root signature, heaps,
 * topology. Set before every draw of the library's own, since on_record
 * may have left anything bound. */
void d3d_own_state(App *a);
/* The fetch or the filter, one covering triangle reading `srv` at
 * `pso`. */
void d3d_fetch(App *a, ID3D12PipelineState *pso, int srv);

/* ---- frame.c ------------------------------------------------------------- */
bool d3d_draw_frame(App *a, uint32_t frame, const FnlDrawState *ds,
					uint32_t cfg_serial, bool cfg_pending);
int  d3d_active_samples(const App *a, const FnlDrawState *ds);
void d3d_gpu_time_poll(App *a);

/* The overlay's draws, the seam's overlay entries before the cover. */
void d3d_overlay_flush_rects(App *a);
void d3d_overlay_flush_shapes(App *a);
void d3d_overlay_scissor(App *a, const FnlRect *r);
FNL_MUST_USE FnlResult d3d_text_end(App *a);
/* The seam's overlay_cover: App::snap over the window at `alpha`. */
bool d3d_overlay_cover(App *a, float alpha);
/* The seam's cover_release: the cover and the snapshot's readback let go. */
void d3d_cover_release(App *a);

#endif /* FNL_D3D12_INTERNAL_H */

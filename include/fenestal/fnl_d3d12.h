/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Fenestal's Direct3D 12 interop.
 *
 * Present only in a build configured with the Direct3D 12 renderer, and
 * included by nothing in fenestal.h, so the portable header does not drag
 * <d3d12.h> into every consumer. From C, define COBJMACROS before including
 * it, for the ID3D12*_Method() macros; the two heap methods that return a
 * structure are called through the vtable, with the result's address, in
 * mingw-w64's headers and the Windows SDK's alike.
 *
 * Fenestal owns the window, the device, the swapchain, the present schedule,
 * the render scale, both antialiasing modes and the text overlay; the scene
 * is yours. Everything up to "here is a command list, with the render target
 * bound and the viewport set" is portable, and what you record into it is
 * not.
 *
 * Vulkan's conventions hold: a viewport's and a scissor's y count down from
 * the top, as Direct3D's do, depth runs from 0 to 1, and a shader compiled
 * from the same GLSL through SPIR-V and spirv-cross's HLSL, with Y flipped at
 * the end of the vertex stage (--flip-vert-y), draws what it draws under
 * Vulkan. A triangle's winding is Direct3D's: counter-clockwise under Vulkan
 * is counter-clockwise here once Y is flipped, so FrontCounterClockwise =
 * TRUE culls as VK_FRONT_FACE_COUNTER_CLOCKWISE does.
 */

#ifndef FENESTAL_D3D12_H
#define FENESTAL_D3D12_H

#include <fenestal/fenestal.h>

#include <d3d12.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Everything needed to allocate, upload and submit alongside Fenestal. The
 * queue is shared, not handed over: work on it is ordered against the
 * frame's own. Not referenced for you: AddRef what you keep.
 *
 * Valid for one device generation -- from an on_gpu_created to the next
 * on_gpu_lost. Every pointer here changes across a loss. */
typedef struct {
	ID3D12Device       *device;
	ID3D12CommandQueue *queue;
} FnlD3D12Gpu;

/* Read from the renderer's state, so valid on the render thread alone:
 * call it from a callback -- on_gpu_created, on_record or on_overlay -- as
 * the examples do. The thread that called fnl_init() counts as that thread
 * while there is no other: before fnl_run() starts it, where the first
 * on_gpu_created runs, and once fnl_run() has joined it, where the last
 * on_gpu_lost runs. Anywhere else a loss or a move to another renderer may
 * free what it reads, so anywhere else it returns false and writes
 * nothing. False too where Direct3D 12 does not draw this window. */
FNL_API bool fnl_d3d12_gpu(FnlApp *app, FnlD3D12Gpu *out);

/* What a pipeline state for each target must be built with: the sample
 * count, and the render target's and depth's formats (DXGI_FORMAT values).
 * One pipeline state per target you might see.
 *
 * False when this window cannot use that target: FNL_TARGET_OFFSCREEN_MS on
 * a device that offers no sample count worth having, for instance. */
typedef struct {
	int      samples;
	uint32_t color_format;     /* DXGI_FORMAT_R8G8B8A8_UNORM_SRGB */
	uint32_t depth_format;     /* DXGI_FORMAT_D32_FLOAT */
} FnlD3D12SceneLayout;

FNL_API bool fnl_d3d12_scene_layout(FnlApp *app, FnlSceneTarget target,
									FnlD3D12SceneLayout *out);    /* [any] */

/* This frame's scene: the command list it is recorded into, the render
 * target and depth it draws to, and the rectangle of them it is drawn
 * into. Call it only from inside on_record, on the thread that runs it;
 * anywhere else it returns false, and nothing here may be kept past the
 * callback.
 *
 * On return the render target and depth are bound and cleared (colour to
 * opaque black, depth to 1), and the viewport and scissor are set to the
 * rectangle the frame is being drawn into -- the presentable area under
 * FNL_TARGET_DIRECT, the offscreen image's own extent otherwise. Set the
 * root signature, the pipeline state, the descriptor heaps and the
 * topology your draws need; the library sets its own after you return.
 *
 * Do not close the list, change the bound target's resource state, or
 * execute anything on the queue from inside the callback.
 *
 * Direct3D 12 is recorded afresh for every frame shown here, as OpenGL is:
 * on_record runs for every frame, not once per change (fenestal.h
 * *on_record*). */
typedef struct {
	ID3D12GraphicsCommandList  *list;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv;
	D3D12_CPU_DESCRIPTOR_HANDLE dsv;
	/* The viewport's and the scissor's rectangle, in the target's pixels.
	 * A clear of your own wants it: ClearRenderTargetView ignores the
	 * scissor, and under FNL_TARGET_DIRECT the target is the whole window,
	 * titlebar included. */
	D3D12_RECT                  rect;
} FnlD3D12Scene;

FNL_API bool fnl_d3d12_scene(FnlApp *app,
							 FnlD3D12Scene *out);              /* on_record */

#ifdef __cplusplus
}
#endif

#endif /* FENESTAL_D3D12_H */

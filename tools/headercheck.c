/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The public headers, compiled on their own as C11 and as C++11 through
 * C++20 under -pedantic-errors -Wall -Wextra -Werror (DESIGN *Building*
 * says they do; this is the build saying it). Compiled and never linked:
 * what is checked is the headers. The same file serves both languages.
 */

#include <fenestal/fenestal.h>
#include <fenestal/fnl_gl.h>
/* fnl_gl.h includes no GL header, and fnl_metal.h none outside Objective-C,
 * so every tree checks both; fnl_vulkan.h includes Vulkan's, so only a tree
 * that has them does. A macOS tree compiles this as Objective-C as well,
 * where fnl_metal.h is Metal's types. */
#include <fenestal/fnl_metal.h>
#ifdef FNL_CHECK_VULKAN
#include <fenestal/fnl_vulkan.h>
#endif
/* fnl_d3d12.h includes <d3d12.h>, so only a tree with Direct3D 12 checks
 * it; from C with COBJMACROS, as its own comment asks of a caller. */
#ifdef FNL_CHECK_D3D12
#ifndef __cplusplus
#define COBJMACROS
#endif
#include <fenestal/fnl_d3d12.h>
#endif

FNL_STATIC_ASSERT(sizeof(FnlEvent) >= sizeof(double) * 2,
				  "FnlEvent holds at least its time and a coordinate");

/* A caller's use of each kind of thing the headers declare: a descriptor, a
 * callback table, an event drained, a colour, and the interop structs. */
static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	FnlEvent ev;
	const FnlColor white = { 1.0f, 1.0f, 1.0f, 1.0f };
	(void)ud;
	while (fnl_poll_event(app, &ev))
		if (ev.type == FNL_EVENT_KEY && ev.key.pressed)
			fnl_overlay_rect(app, 0, 0, frame->area_w_px, 1, white);
}

int headercheck(void);
int headercheck(void)
{
	FnlInitDesc d;
	FnlCallbacks cb;
#ifdef FNL_CHECK_VULKAN
	FnlVulkanGpu gpu;
	FnlVulkanSceneLayout layout;
#endif
#ifdef FNL_CHECK_D3D12
	FnlD3D12Gpu d3d_gpu;
	FnlD3D12SceneLayout d3d_layout;
	FnlD3D12Scene d3d_scene;
#endif
	FnlGlSceneLayout gl_layout;
	FnlGlProc proc = 0;
	FnlMetalGpu mtl_gpu;
	FnlMetalSceneLayout mtl_layout;
	FnlMetalScene mtl_scene;
	FnlApp *app = 0;
	d.size = sizeof d;
	d.header_version = FNL_VERSION;
	cb.size = sizeof cb;
	cb.on_overlay = on_overlay;
#ifdef FNL_CHECK_VULKAN
	(void)gpu;
	(void)layout;
#endif
#ifdef FNL_CHECK_D3D12
	(void)d3d_gpu;
	(void)d3d_layout;
	(void)d3d_scene;
#endif
	(void)gl_layout;
	(void)proc;
	(void)mtl_gpu;
	(void)mtl_layout;
	(void)mtl_scene;
	return fnl_init(&d, &app) == FNL_OK ? 0 : 1;
}

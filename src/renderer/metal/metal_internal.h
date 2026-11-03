/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What the files of the Metal renderer share with each other and with
 * nothing else. src/core/ and src/backend/ reach this directory through
 * FnlRendererVtbl (core/renderer.h) and the few overlay entry points core/app.h
 * declares. The renderer's state is in metal_state.h, behind
 * App::renderer_state.
 *
 * Objective-C under ARC, like src/backend/cocoa/ and for its reason: Metal
 * has no C interface. Every object a command buffer names is kept alive by
 * that command buffer until it completes, so -- unlike Direct3D 12 -- a
 * texture or a pipeline replaced while a frame is in flight is simply let
 * go, and nothing here waits for the queue to do it. What the CPU writes
 * into, the overlay's arenas, is the exception: a frame slot waits for the
 * last command buffer that read it (frame.m).
 */

#ifndef FNL_METAL_INTERNAL_H
#define FNL_METAL_INTERNAL_H

#include "core/app.h"

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "renderer/metal/metal_state.h"

#include <fenestal/fnl_metal.h>

/* The formats every image here is made in. The layer's drawables are
 * BGRA -- a CAMetalLayer offers no RGBA of eight bits -- and _sRGB, so a
 * write is encoded and a blend is linear, as on Vulkan's _SRGB surface; the
 * scene's images are the same format, so one pipeline state draws into
 * either. */
#define MT_COLOR_FORMAT MTLPixelFormatBGRA8Unorm_sRGB
#define MT_DEPTH_FORMAT MTLPixelFormatDepth32Float

/* Where the shaders' resources are bound (DESIGN *One shader source*):
 * `Push` at buffer 0 in both stages, the instance stream at buffer 1, and
 * the one image and its sampler at 0. */
#define MT_BUF_PUSH     0
#define MT_BUF_INSTANCE 1

/* ---- device.m ------------------------------------------------------------ */
/* Whether a command buffer that finished says the device has gone, which
 * raises App::device_lost; and whether the device says so of itself. */
bool mt_cb_lost(App *a, id<MTLCommandBuffer> cb);
bool mt_device_lost(App *a);
/* Every command buffer this renderer has committed, finished. */
void mt_wait_idle(App *a);
/* What a library of MSL text made, by entry name, or nil with the
 * compiler's log in ours. */
id<MTLFunction> mt_function(App *a, const char *what, const char *msl);

/* ---- surface_cocoa.m: the bridge ----------------------------------------- */
/* The window's layer, which the backend made; and whether the window is
 * covered, which only the backend can see. */
CAMetalLayer *mt_bridge_layer(App *a);
bool mt_bridge_occluded(const App *a);

/* ---- target.m ------------------------------------------------------------ */
/* The layer's drawables and the window's depth at the size the frame policy
 * settled, and the present mode vsync asks for: the rebuild. */
FNL_MUST_USE FnlResult mt_window_images(App *a);
void mt_release_window_images(App *a);
void mt_scene_set_size(App *a, int w, int h);
bool mt_scene_ensure_aa(App *a, bool want_ms, bool want_mlaa);
void mt_destroy_targets(App *a);

/* ---- overlay.m ----------------------------------------------------------- */
FNL_MUST_USE FnlResult mt_create_overlay(App *a);
void mt_destroy_overlay(App *a);
FNL_MUST_USE FnlResult mt_text_set_scale(App *a, int s120);
/* The library's own state on an encoder: no depth test, no culling, filled
 * triangles, before every draw of the library's own, since on_record may
 * have left anything set. */
void mt_own_state(App *a, id<MTLRenderCommandEncoder> enc);
/* The fetch or the filter, one covering triangle reading `tex` at `pso`. */
void mt_fetch(App *a, id<MTLRenderCommandEncoder> enc,
			  id<MTLRenderPipelineState> pso, id<MTLTexture> tex);

/* ---- frame.m ------------------------------------------------------------- */
bool mt_draw_frame(App *a, uint32_t frame, const FnlDrawState *ds,
				   uint32_t cfg_serial, bool cfg_pending);
int  mt_active_samples(const App *a, const FnlDrawState *ds);
void mt_gpu_time_poll(App *a);

/* The overlay's draws, the seam's overlay entries before the cover. */
void mt_overlay_flush_rects(App *a);
void mt_overlay_flush_shapes(App *a);
void mt_overlay_scissor(App *a, const FnlRect *r);
FNL_MUST_USE FnlResult mt_text_end(App *a);
/* The seam's overlay_cover: App::snap over the window at `alpha`. */
bool mt_overlay_cover(App *a, float alpha);
/* The seam's cover_release: the cover and the snapshot's buffer let go. */
void mt_cover_release(App *a);

#endif /* FNL_METAL_INTERNAL_H */

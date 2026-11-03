/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Fenestal's Metal interop.
 *
 * Present only in a build configured with the Metal renderer (macOS), and
 * included by nothing in fenestal.h, so the portable header does not drag
 * <Metal/Metal.h> into every consumer. Metal is an Objective-C API: from
 * Objective-C each object below is its protocol type; from C or C++ it is a
 * `void *` to the same object, for a caller that reaches Metal some other
 * way. No reference is handed over either way -- under ARC the fields are
 * __unsafe_unretained -- so retain what you keep.
 *
 * Fenestal owns the window, the device, the layer and its drawables, the
 * present schedule, the render scale, both antialiasing modes and the text
 * overlay; the scene is yours. Everything up to "here is a render command
 * encoder, with the target bound and the viewport set" is portable, and
 * what you encode into it is not.
 *
 * Vulkan's conventions hold: a viewport's and a scissor's y count down from
 * the top, as Metal's do, depth runs from 0 to 1, and a shader translated
 * from the same GLSL through SPIR-V and spirv-cross's MSL, with Y flipped at
 * the end of the vertex stage (--flip-vert-y), draws what it draws under
 * Vulkan. A triangle's winding is Metal's: counter-clockwise under Vulkan is
 * counter-clockwise here once Y is flipped, so
 * MTLWindingCounterClockwise culls as VK_FRONT_FACE_COUNTER_CLOCKWISE does.
 */

#ifndef FENESTAL_METAL_H
#define FENESTAL_METAL_H

#include <fenestal/fenestal.h>

#if defined(__OBJC__)
#import <Metal/Metal.h>
#  if __has_feature(objc_arc)
#    define FNL_MTL_BORROWED __unsafe_unretained
#  else
#    define FNL_MTL_BORROWED
#  endif
typedef id<MTLDevice>               FnlMtlDevice;
typedef id<MTLCommandQueue>         FnlMtlCommandQueue;
typedef id<MTLCommandBuffer>        FnlMtlCommandBuffer;
typedef id<MTLRenderCommandEncoder> FnlMtlRenderEncoder;
#else
#  define FNL_MTL_BORROWED
typedef void *FnlMtlDevice;
typedef void *FnlMtlCommandQueue;
typedef void *FnlMtlCommandBuffer;
typedef void *FnlMtlRenderEncoder;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Everything needed to make resources and submit alongside Fenestal. The
 * queue is shared, not handed over: work on it is ordered against the
 * frame's own.
 *
 * Valid for one device generation -- from an on_gpu_created to the next
 * on_gpu_lost. Both change across a loss. */
typedef struct {
	FNL_MTL_BORROWED FnlMtlDevice       device;
	FNL_MTL_BORROWED FnlMtlCommandQueue queue;
} FnlMetalGpu;

/* Read from the renderer's state, so valid on the render thread alone:
 * call it from a callback -- on_gpu_created, on_record or on_overlay -- as
 * the examples do. The thread that called fnl_init() counts as that thread
 * while there is no other: before fnl_run() starts it, where the first
 * on_gpu_created runs, and once fnl_run() has joined it, where the last
 * on_gpu_lost runs. Anywhere else a loss or a move to another renderer may
 * free what it reads, so anywhere else it returns false and writes
 * nothing. False too where Metal does not draw this window. */
FNL_API bool fnl_metal_gpu(FnlApp *app, FnlMetalGpu *out);

/* What a render pipeline state for each target must be built with: the
 * sample count, and the colour and depth attachments' formats
 * (MTLPixelFormat values). One pipeline state per target you might see.
 *
 * False when this window cannot use that target: FNL_TARGET_OFFSCREEN_MS on
 * a device that offers no sample count worth having, for instance. */
typedef struct {
	int      samples;
	uint32_t color_format;     /* MTLPixelFormatBGRA8Unorm_sRGB */
	uint32_t depth_format;     /* MTLPixelFormatDepth32Float */
} FnlMetalSceneLayout;

FNL_API bool fnl_metal_scene_layout(FnlApp *app, FnlSceneTarget target,
									FnlMetalSceneLayout *out);    /* [any] */

/* This frame's scene: the encoder it is recorded into, the command buffer
 * that encoder belongs to, and the rectangle of the target it is drawn
 * into. Call it only from inside on_record, on the thread that runs it;
 * anywhere else it returns false, and nothing here may be kept past the
 * callback.
 *
 * On return the colour and depth attachments are cleared (colour to opaque
 * black, depth to 1), and the viewport and scissor are set to the rectangle
 * the frame is being drawn into -- the presentable area under
 * FNL_TARGET_DIRECT, the offscreen image's own extent otherwise. Set the
 * pipeline state, the depth-stencil state and the buffers and textures your
 * draws need; the library sets its own after you return.
 *
 * Do not end the encoder, open another on the command buffer, or commit
 * anything from inside the callback. A buffer of your own written by the CPU
 * must outlive the frames that read it, as on any queue: the command buffer
 * keeps the objects it names alive, and does not keep their contents.
 *
 * Metal is recorded afresh for every frame shown, as OpenGL and Direct3D 12
 * are: on_record runs for every frame, not once per change (fenestal.h
 * *on_record*). */
typedef struct {
	FNL_MTL_BORROWED FnlMtlRenderEncoder encoder;
	FNL_MTL_BORROWED FnlMtlCommandBuffer command_buffer;
	/* The viewport's and the scissor's rectangle, in the target's pixels.
	 * Under FNL_TARGET_DIRECT the target is the whole window, titlebar
	 * included. */
	int x, y, w, h;
} FnlMetalScene;

FNL_API bool fnl_metal_scene(FnlApp *app,
							 FnlMetalScene *out);              /* on_record */

#ifdef __cplusplus
}
#endif

#endif /* FENESTAL_METAL_H */

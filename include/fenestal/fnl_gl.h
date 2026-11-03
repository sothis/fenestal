/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Fenestal's OpenGL interop.
 *
 * Present only in a build configured with the OpenGL renderer -- a Linux
 * one, where GL is reached through EGL, or a macOS one, through CGL, at 4.1
 * core -- and included by nothing in
 * fenestal.h. It includes no GL header either: GL is reached through a
 * loader, and which one is yours. The formats below are GL enum
 * values, as GLenum carries them.
 *
 * Fenestal owns the window, the context, the present schedule, the render
 * scale, both antialiasing modes and the text overlay; the scene is yours.
 * Everything up to "here is a framebuffer, bound, with the viewport set" is
 * portable, and what you draw into it is not.
 *
 * The context is OpenGL 4.5 core, or 4.1 core on a device that makes no 4.5
 * -- fnl_gl_version() says which -- and it is current on whichever thread
 * runs a callback, for the length of the callback: on_gpu_created and
 * on_gpu_lost as well as on_record and on_overlay. Outside a callback it may
 * be current on another thread, so GL is not to be called there.
 *
 * Vulkan's conventions, kept so that one projection and one set of shaders
 * serve both: every image the library draws into has row 0 at the top --
 * a viewport's y, a scissor's y and gl_FragCoord.y count down from the top,
 * and a texture rendered by the library is sampled with v = 0 at its top --
 * and, under 4.5, depth runs from 0 to 1 (glClipControl(GL_LOWER_LEFT,
 * GL_ZERO_TO_ONE), which the library sets once and you must not change).
 * Under 4.1, which has no glClipControl, depth runs from -1 to 1, GL's own,
 * and a vertex stage written for Vulkan maps it itself: spirv-cross does,
 * given --fixup-clipspace (z = 2z - w), as the library's own shaders and
 * the examples' are translated (the build's FNL_GL41_FLAGS).
 * The window is flipped onto the screen at the end of each frame. One
 * difference stays: GL measures a triangle's winding with y pointing up, so
 * the triangle Vulkan calls counter-clockwise GL calls clockwise, and
 * glFrontFace(GL_CW) culls as VK_FRONT_FACE_COUNTER_CLOCKWISE does.
 */

#ifndef FENESTAL_GL_H
#define FENESTAL_GL_H

#include <fenestal/fenestal.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Any GL entry point, by name -- core functions included -- for a loader to
 * cast. NULL for a name the driver does not have. Valid for one device
 * generation, from an on_gpu_created to the next on_gpu_lost: a loss may
 * land on another driver. */
typedef void (*FnlGlProc)(void);

FNL_API FnlGlProc fnl_gl_proc(FnlApp *app, const char *name);    /* [any] */

/* Which OpenGL the context is, as major * 10 + minor: 45, or 41 where the
 * device makes no 4.5 core context. Under 41 the scene draws with 4.1 core
 * alone -- no direct state access, immutable storage, base instance or
 * glClipControl -- in GLSL 4.10, and maps its depth itself (above). Asked
 * in on_gpu_created, since a loss or a move may land on another device; 0
 * where OpenGL does not draw this window. */
FNL_API int fnl_gl_version(FnlApp *app);                        /* [any] */

/* What the framebuffer of each target is made of, for building against it.
 *
 * GL has no render-pass compatibility, so unlike Vulkan one program serves
 * every target; what differs is the sample count, which decides whether a
 * draw is multisampled, and the formats, which a program that renders into
 * images of its own matches to blit or sample between them.
 *
 * False when this window cannot use that target: FNL_TARGET_OFFSCREEN_MS on
 * a device that offers no sample count worth having, for instance. */
typedef struct {
	int      samples;
	uint32_t color_format;     /* GL_SRGB8_ALPHA8 */
	uint32_t depth_format;     /* GL_DEPTH_COMPONENT32F */
} FnlGlSceneLayout;

FNL_API bool fnl_gl_scene_layout(FnlApp *app, FnlSceneTarget target,
								 FnlGlSceneLayout *out);         /* [any] */

/* The framebuffer this frame's scene is drawn into. Call it only from inside
 * on_record, on the thread that runs it; anywhere else it returns false, and
 * the name must not be kept past the callback -- it changes when the window
 * or the render scale does.
 *
 * On return it is bound for drawing and cleared (colour to opaque black,
 * depth to 1), the viewport and scissor are set to the rectangle the frame is
 * being drawn into -- the presentable area under FNL_TARGET_DIRECT, the
 * offscreen image's own extent otherwise -- with the scissor test on, and
 * GL_FRAMEBUFFER_SRGB is enabled, so a shader writes linear colour. Every
 * other piece of GL state is whatever the frame left it as: set what your
 * draws need. The library sets what its own need after you return.
 *
 * Bind it again after drawing into a framebuffer of your own. Do not delete
 * it, resize it, or change what it is attached to.
 *
 * OpenGL cannot keep a recording: on_record runs for every frame shown, not
 * once per change (fenestal.h *on_record*). */
FNL_API bool fnl_gl_scene_framebuffer(FnlApp *app,
									  uint32_t *out);           /* on_record */

#ifdef __cplusplus
}
#endif

#endif /* FENESTAL_GL_H */

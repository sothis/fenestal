/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What the files of the OpenGL renderer share with each other and with
 * nothing else. src/core/ and src/backend/ reach this directory through
 * FnlRendererVtbl (core/renderer.h) and through the few overlay entry points
 * core/app.h declares. The renderer's state is in gl_state.h, behind
 * App::renderer_state.
 *
 * Included after "core/app.h", which every .c names first.
 */

#ifndef FNL_GL_INTERNAL_H
#define FNL_GL_INTERNAL_H

#include "core/app.h"
#include "renderer/gl/gl_state.h"

#include <fenestal/fnl_gl.h>

#include <GL/glcorearb.h>

/* ---- the entry points -------------------------------------------------------
 * Every GL function the renderer calls, loaded once a context exists
 * (gl_load()) and called as gl.Name(...). Through a table, because nothing
 * links libGL: EGL is opened at run time (egl_dyn.h) and hands out every
 * entry point, core functions too (EGL 1.5). One table for the process: a
 * driver's entry points are the same for every context it makes, and it is
 * written before the render thread exists or by the render thread while it
 * alone draws.
 *
 * Three lists, in the table's order. FNL_GL_CORE is core in 4.1, and loaded
 * on every context: the calls both profiles make, and the bind-to-edit
 * calls the 4.1 profile's stand-ins are made of (gl_compat.c). FNL_GL_45 is
 * what 4.5 adds that the renderer is written against -- direct state
 * access, immutable storage, the base instance, glClipControl -- loaded on
 * a 4.5 context and, on a 4.1 one, filled with gl_compat.c's stand-ins, so
 * that every call site is the same in both. FNL_GL_OPT may be missing on
 * either: NULL where the context has neither the version nor the
 * extension, and every caller asks first. */
#define FNL_GL_CORE(X)                                                        \
	X(PFNGLGETSTRINGPROC,                     GetString)                      \
	X(PFNGLGETSTRINGIPROC,                    GetStringi)                     \
	X(PFNGLGETINTEGERVPROC,                   GetIntegerv)                    \
	X(PFNGLGETERRORPROC,                      GetError)                       \
	X(PFNGLENABLEPROC,                        Enable)                         \
	X(PFNGLDISABLEPROC,                       Disable)                        \
	X(PFNGLVIEWPORTPROC,                      Viewport)                       \
	X(PFNGLSCISSORPROC,                       Scissor)                        \
	X(PFNGLBLENDFUNCSEPARATEPROC,             BlendFuncSeparate)              \
	X(PFNGLBLENDCOLORPROC,                    BlendColor)                     \
	X(PFNGLBLENDEQUATIONPROC,                 BlendEquation)                  \
	X(PFNGLCOLORMASKPROC,                     ColorMask)                      \
	X(PFNGLDEPTHMASKPROC,                     DepthMask)                      \
	X(PFNGLPIXELSTOREIPROC,                   PixelStorei)                    \
	X(PFNGLREADPIXELSPROC,                    ReadPixels)                     \
	X(PFNGLFINISHPROC,                        Finish)                         \
	X(PFNGLFLUSHPROC,                         Flush)                          \
	X(PFNGLGENBUFFERSPROC,                    GenBuffers)                     \
	X(PFNGLBUFFERDATAPROC,                    BufferData)                     \
	X(PFNGLBUFFERSUBDATAPROC,                 BufferSubData)                  \
	X(PFNGLMAPBUFFERRANGEPROC,                MapBufferRange)                 \
	X(PFNGLUNMAPBUFFERPROC,                   UnmapBuffer)                    \
	X(PFNGLDELETEBUFFERSPROC,                 DeleteBuffers)                  \
	X(PFNGLBINDBUFFERBASEPROC,                BindBufferBase)                 \
	X(PFNGLBINDBUFFERPROC,                    BindBuffer)                     \
	X(PFNGLPOLYGONMODEPROC,                   PolygonMode)                    \
	X(PFNGLGENVERTEXARRAYSPROC,               GenVertexArrays)                \
	X(PFNGLDELETEVERTEXARRAYSPROC,            DeleteVertexArrays)             \
	X(PFNGLBINDVERTEXARRAYPROC,               BindVertexArray)                \
	X(PFNGLVERTEXATTRIBPOINTERPROC,           VertexAttribPointer)            \
	X(PFNGLENABLEVERTEXATTRIBARRAYPROC,       EnableVertexAttribArray)        \
	X(PFNGLVERTEXATTRIBDIVISORPROC,           VertexAttribDivisor)            \
	X(PFNGLGENTEXTURESPROC,                   GenTextures)                    \
	X(PFNGLBINDTEXTUREPROC,                   BindTexture)                    \
	X(PFNGLACTIVETEXTUREPROC,                 ActiveTexture)                  \
	X(PFNGLTEXIMAGE2DPROC,                    TexImage2D)                     \
	X(PFNGLTEXSUBIMAGE2DPROC,                 TexSubImage2D)                  \
	X(PFNGLTEXPARAMETERIPROC,                 TexParameteri)                  \
	X(PFNGLDELETETEXTURESPROC,                DeleteTextures)                 \
	X(PFNGLGENSAMPLERSPROC,                   GenSamplers)                    \
	X(PFNGLDELETESAMPLERSPROC,                DeleteSamplers)                 \
	X(PFNGLSAMPLERPARAMETERIPROC,             SamplerParameteri)              \
	X(PFNGLBINDSAMPLERPROC,                   BindSampler)                    \
	X(PFNGLGENRENDERBUFFERSPROC,              GenRenderbuffers)               \
	X(PFNGLBINDRENDERBUFFERPROC,              BindRenderbuffer)               \
	X(PFNGLRENDERBUFFERSTORAGEPROC,           RenderbufferStorage)            \
	X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC,                                \
									 RenderbufferStorageMultisample)          \
	X(PFNGLDELETERENDERBUFFERSPROC,           DeleteRenderbuffers)            \
	X(PFNGLGENFRAMEBUFFERSPROC,               GenFramebuffers)                \
	X(PFNGLDELETEFRAMEBUFFERSPROC,            DeleteFramebuffers)             \
	X(PFNGLBINDFRAMEBUFFERPROC,               BindFramebuffer)                \
	X(PFNGLFRAMEBUFFERTEXTURE2DPROC,          FramebufferTexture2D)           \
	X(PFNGLFRAMEBUFFERRENDERBUFFERPROC,       FramebufferRenderbuffer)        \
	X(PFNGLCHECKFRAMEBUFFERSTATUSPROC,        CheckFramebufferStatus)         \
	X(PFNGLBLITFRAMEBUFFERPROC,               BlitFramebuffer)                \
	X(PFNGLCLEARBUFFERFVPROC,                 ClearBufferfv)                  \
	X(PFNGLCREATESHADERPROC,                  CreateShader)                   \
	X(PFNGLSHADERSOURCEPROC,                  ShaderSource)                   \
	X(PFNGLCOMPILESHADERPROC,                 CompileShader)                  \
	X(PFNGLGETSHADERIVPROC,                   GetShaderiv)                    \
	X(PFNGLGETSHADERINFOLOGPROC,              GetShaderInfoLog)               \
	X(PFNGLDELETESHADERPROC,                  DeleteShader)                   \
	X(PFNGLCREATEPROGRAMPROC,                 CreateProgram)                  \
	X(PFNGLATTACHSHADERPROC,                  AttachShader)                   \
	X(PFNGLDETACHSHADERPROC,                  DetachShader)                   \
	X(PFNGLLINKPROGRAMPROC,                   LinkProgram)                    \
	X(PFNGLGETPROGRAMIVPROC,                  GetProgramiv)                   \
	X(PFNGLGETPROGRAMINFOLOGPROC,             GetProgramInfoLog)              \
	X(PFNGLDELETEPROGRAMPROC,                 DeleteProgram)                  \
	X(PFNGLUSEPROGRAMPROC,                    UseProgram)                     \
	X(PFNGLGETUNIFORMBLOCKINDEXPROC,          GetUniformBlockIndex)           \
	X(PFNGLUNIFORMBLOCKBINDINGPROC,           UniformBlockBinding)            \
	X(PFNGLDRAWARRAYSPROC,                    DrawArrays)                     \
	X(PFNGLDRAWARRAYSINSTANCEDPROC,           DrawArraysInstanced)            \
	X(PFNGLFENCESYNCPROC,                     FenceSync)                      \
	X(PFNGLCLIENTWAITSYNCPROC,                ClientWaitSync)                 \
	X(PFNGLGENQUERIESPROC,                    GenQueries)                     \
	X(PFNGLDELETEQUERIESPROC,                 DeleteQueries)                  \
	X(PFNGLQUERYCOUNTERPROC,                  QueryCounter)                   \
	X(PFNGLGETQUERYOBJECTIVPROC,              GetQueryObjectiv)               \
	X(PFNGLGETQUERYOBJECTUI64VPROC,           GetQueryObjectui64v)            \
	X(PFNGLDELETESYNCPROC,                    DeleteSync)

#define FNL_GL_45(X)                                                          \
	X(PFNGLCLIPCONTROLPROC,                   ClipControl)                    \
	X(PFNGLCREATEBUFFERSPROC,                 CreateBuffers)                  \
	X(PFNGLNAMEDBUFFERSTORAGEPROC,            NamedBufferStorage)             \
	X(PFNGLMAPNAMEDBUFFERRANGEPROC,           MapNamedBufferRange)            \
	X(PFNGLUNMAPNAMEDBUFFERPROC,              UnmapNamedBuffer)               \
	X(PFNGLNAMEDBUFFERSUBDATAPROC,            NamedBufferSubData)             \
	X(PFNGLCREATEVERTEXARRAYSPROC,            CreateVertexArrays)             \
	X(PFNGLVERTEXARRAYVERTEXBUFFERPROC,       VertexArrayVertexBuffer)        \
	X(PFNGLVERTEXARRAYATTRIBFORMATPROC,       VertexArrayAttribFormat)        \
	X(PFNGLVERTEXARRAYATTRIBBINDINGPROC,      VertexArrayAttribBinding)       \
	X(PFNGLENABLEVERTEXARRAYATTRIBPROC,       EnableVertexArrayAttrib)        \
	X(PFNGLVERTEXARRAYBINDINGDIVISORPROC,     VertexArrayBindingDivisor)      \
	X(PFNGLCREATETEXTURESPROC,                CreateTextures)                 \
	X(PFNGLTEXTURESTORAGE2DPROC,              TextureStorage2D)               \
	X(PFNGLTEXTURESUBIMAGE2DPROC,             TextureSubImage2D)              \
	X(PFNGLBINDTEXTUREUNITPROC,               BindTextureUnit)                \
	X(PFNGLCREATESAMPLERSPROC,                CreateSamplers)                 \
	X(PFNGLCREATERENDERBUFFERSPROC,           CreateRenderbuffers)            \
	X(PFNGLNAMEDRENDERBUFFERSTORAGEPROC,      NamedRenderbufferStorage)       \
	X(PFNGLNAMEDRENDERBUFFERSTORAGEMULTISAMPLEPROC,                           \
									 NamedRenderbufferStorageMultisample)     \
	X(PFNGLCREATEFRAMEBUFFERSPROC,            CreateFramebuffers)             \
	X(PFNGLNAMEDFRAMEBUFFERTEXTUREPROC,       NamedFramebufferTexture)        \
	X(PFNGLNAMEDFRAMEBUFFERRENDERBUFFERPROC,  NamedFramebufferRenderbuffer)   \
	X(PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC,   CheckNamedFramebufferStatus)    \
	X(PFNGLBLITNAMEDFRAMEBUFFERPROC,          BlitNamedFramebuffer)           \
	X(PFNGLCLEARNAMEDFRAMEBUFFERFVPROC,       ClearNamedFramebufferfv)        \
	X(PFNGLDRAWARRAYSINSTANCEDBASEINSTANCEPROC,                               \
									 DrawArraysInstancedBaseInstance)

#define FNL_GL_OPT(X)                                                         \
	X(PFNGLGETGRAPHICSRESETSTATUSPROC,        GetGraphicsResetStatus)         \
	X(PFNGLDEBUGMESSAGECALLBACKPROC,          DebugMessageCallback)

#define FNL_GL_FUNCS(X) FNL_GL_CORE(X) FNL_GL_45(X) FNL_GL_OPT(X)

typedef struct {
#define FNL_GL_MEMBER(type, name) type name;
	FNL_GL_FUNCS(FNL_GL_MEMBER)
#undef FNL_GL_MEMBER
} GlFuncs;

extern GlFuncs gl;                                           /* loader.c */

/* Which profile the table holds: false for 4.5 core, what everything here is
 * written against, DSA and glClipControl included; true for 4.1 core, where
 * FNL_GL_45's slots are gl_compat.c's and the shaders are the gl41
 * translation, which maps depth itself (DESIGN *OpenGL 4.1*). Written with
 * the table. */
extern bool gl_profile41;                                    /* loader.c */

/* Every entry point through the bridge, and the version checked: 4.5 core,
 * or 4.1 core with the stand-ins installed. Called with the context
 * current. */
FNL_MUST_USE FnlResult gl_load(App *a);

/* ---- gl_compat.c --------------------------------------------------------- */
/* FNL_GL_45's slots of `t` filled with stand-ins made of FNL_GL_CORE's
 * calls, and DeleteVertexArrays with one that keeps the stand-ins' account
 * of each vertex array; `t`'s core entries must be loaded. */
void gl_compat_install(GlFuncs *t);
/* The overlay's arenas under 4.1, where no buffer stays mapped: the core
 * appends into client memory, and a flush uploads what it is about to draw
 * -- `bytes` from `offset` of `map` into the same place of `buf`. */
void gl_arena_upload(unsigned buf, const void *map, size_t offset,
					 size_t bytes);

/* ---- the bridge -------------------------------------------------------------
 * The window system's half: EGL (egl.c, with the window server's few lines
 * in egl_<backend>.c). Each step is one entry of the seam's, so that a
 * loss can remake the surface or the context alone, as Vulkan remakes its
 * surface or its device. */

/* The EGL library held for the instance's life, taken before the first
 * open and given back after the last close; the last hold given back may
 * close it (egl.c). */
FNL_MUST_USE FnlResult gl_bridge_hold(App *a);
void gl_bridge_let_go(App *a);
/* A hold only on a library another hold has open, given back with
 * gl_bridge_let_go(): for fnl_gl_proc() on any thread, which must not be
 * the one that opens it. */
FNL_MUST_USE bool gl_bridge_hold_open(App *a);
/* The display and the configuration every surface and context is made with:
 * create_instance. */
FNL_MUST_USE FnlResult gl_bridge_open(App *a);
void gl_bridge_close(App *a);
/* The window's surface: create_surface. */
FNL_MUST_USE FnlResult gl_bridge_surface(App *a);
void gl_bridge_surface_close(App *a);
/* A 4.5 core context, or a 4.1 one where the device makes no 4.5, current
 * on the calling thread when this returns: create_device_objects' first
 * step. */
FNL_MUST_USE FnlResult gl_bridge_context(App *a);
void gl_bridge_context_close(App *a);
/* Make the context current on the calling thread, or nothing current.
 * False when the context could not be made current, logged. */
bool gl_bridge_current(App *a, bool on);
/* How many vertical blanks a swap waits for, 0 or 1, as given: frame.c
 * decides which (gl_choose_interval()). False where the driver refused,
 * logged, so the caller asks again. */
bool gl_bridge_interval(App *a, int n);
/* What paces a vsynced frame when the swap does not, said for the status
 * line ("frame callback (vsync)"); NULL where the swap is the throttle.
 * Any thread's (present_mode_name), so it reads no renderer state. */
const char *gl_bridge_pacer(const App *a);
/* Whether the window is covered, where the window system says (the
 * renderer's FnlRendererVtbl::occluded): Cocoa's, whose layer hands a
 * covered window no drawable for a second at a time; never under EGL. */
bool gl_bridge_occluded(const App *a);
/* No window surface: the frame is read back from the window's image and
 * handed to the backend (FnlBackendVtbl::present_pixels) in place of the
 * blit and the swap. KMS's. */
bool gl_bridge_surfaceless(const App *a);
/* A surfaceless bridge's frame onto the output's own buffers, in place of
 * the readback (EglBridge::scan_begin): true where it went that way, or
 * shows nothing this frame; then gl_bridge_scan_show() in the swap's place,
 * false where the bridge had nothing to hand over, which the frame counts
 * as no present. A flip the backend then declines (KMS's kms_scan_show())
 * is counted, as the copy path counts one.
 * gl_bridge_scan_close() before the context goes. */
bool gl_bridge_scan_begin(App *a, unsigned fbo, int w, int h);
bool gl_bridge_scan_show(App *a);
void gl_bridge_scan_close(App *a);
/* In gl_bridge_scan_close()'s place where the context could not be made
 * current: no GL call, every name taken for gone with the context, and
 * what the bridge holds outside it -- KMS's EGL images, the display's --
 * let go. */
void gl_bridge_scan_forget(App *a);
/* How long after the last size change the swap is still given 0: long
 * enough that a drag's steps, which arrive a frame or a few apart, are one
 * stretch. */
#define GL_RESIZE_HOLD 0.25
/* The window's surface at the rebuild's extent, and opaque or not as the
 * grab ring asks: `opaque` is a surface made again where it differs and the
 * window server can be told, with the context current on it once more.
 * Render thread, or the main thread before it exists, with the context
 * current. */
FNL_MUST_USE FnlResult gl_bridge_resize(App *a, int w, int h, bool opaque);

typedef enum {
	GL_SWAP_OK,
	GL_SWAP_DEVICE_LOST,       /* the context is gone                      */
	GL_SWAP_SURFACE_LOST,      /* the window's surface is gone             */
	GL_SWAP_FAILED,            /* nothing shown, for any other reason      */
} GlSwap;
GlSwap gl_bridge_swap(App *a);

/* Any GL entry point by name, core included; NULL where there is none. */
FnlGlProc gl_bridge_proc(const char *name);

/* The GPUs the window's display can be opened on, as `out` entries (their
 * EGL names; the caller puts GL's own on the one in use), and in `*cur`
 * the one it is on, or -1. 0 where EGL lists none, and the list is then
 * the one GPU in use. */
int gl_bridge_gpus(App *a, GpuEntry *out, int *cur);
/* The seam's list_gpus: the same list, found while another renderer draws,
 * with no state of this renderer's; where EGL lists none, the window
 * server's one GPU as GL names it, or none where it has no 4.5. */
int gl_bridge_list_gpus(App *a, GpuEntry *out, int max);
/* Whether GL_RENDERER's string is a software rasteriser's. */
bool gl_renderer_is_software(const char *r);

/* Clear the error flags, before a step that reads glGetError() as its own
 * result: on_record may have left one set. Bounded, since a lost context
 * may report the loss again on every call. */
static inline void gl_clear_errors(void)
{
	for (int i = 0; i < 16 && gl.GetError() != GL_NO_ERROR; i++) {
	}
}

/* ---- device.c ------------------------------------------------------------ */
/* Whether a reset has taken the context, which flags App::device_lost. */
bool gl_reset_lost(App *a);

/* ---- target.c ------------------------------------------------------------ */
/* The window's image at App::extent, remade on every rebuild. */
FNL_MUST_USE FnlResult gl_window_image(App *a);
void gl_destroy_targets(App *a);
void gl_scene_set_size(App *a, int w, int h);
/* Build what this frame's antialiasing wants of the scene target. True when
 * something was built. */
bool gl_scene_ensure_aa(App *a, bool want_ms, bool want_mlaa);
/* The formats every colour and depth image here is made in. sRGB, so a write
 * is encoded and a blend is linear, as on Vulkan's _SRGB surface. */
#define GL_COLOR_FORMAT GL_SRGB8_ALPHA8
#define GL_DEPTH_FORMAT GL_DEPTH_COMPONENT32F

/* ---- overlay.c ----------------------------------------------------------- */
FNL_MUST_USE FnlResult gl_create_overlay(App *a);
void gl_destroy_overlay(App *a);
FNL_MUST_USE FnlResult gl_text_set_scale(App *a, int s120);
/* Compile and link one program from two translated stages, its `Push` block
 * (if it has one) bound to point 0. 0 on failure, with the log logged. */
unsigned gl_program(const char *name, const char *vert, const char *frag);
/* The state every draw of the library's own relies on, set before them
 * rather than assumed: on_record may have left anything behind. */
void gl_own_state(App *a);
/* Load `bytes` of what a program's `Push` block holds. */
void gl_push(App *a, const void *data, size_t bytes);

/* ---- frame.c ------------------------------------------------------------- */
bool gl_draw_frame(App *a, uint32_t frame, const FnlDrawState *ds,
				   uint32_t cfg_serial, bool cfg_pending);
int  gl_active_samples(const App *a, const FnlDrawState *ds);
void gl_gpu_time_poll(App *a);

/* The overlay's draws, the seam's overlay entries before the cover. */
void gl_overlay_flush_rects(App *a);
void gl_overlay_flush_shapes(App *a);
void gl_overlay_scissor(App *a, const FnlRect *r);
FNL_MUST_USE FnlResult gl_text_end(App *a);
/* The seam's overlay_cover: App::snap over the window at `alpha`. */
bool gl_overlay_cover(App *a, float alpha);
/* The seam's cover_release: the cover let go. */
void gl_cover_release(App *a);

/* ---- memory.c -------------------------------------------------------------
 * The seam's gpu_memory, the system's figure where GL keeps none; and the
 * end of what it opened to ask, with the device. */
bool gl_gpu_memory(App *a, double *mib);
void gl_memory_release(void);

#endif /* FNL_GL_INTERNAL_H */

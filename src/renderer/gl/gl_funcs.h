/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The OpenGL entry points the renderer calls: every name, in three lists, and
 * the table they make. A table is the window's (RendererState::funcs), loaded
 * with its context (gl_load()): a host of two windows may draw through two
 * drivers' entry points -- a display opened on another GPU names another
 * vendor's EGL -- so there is no one table a process can hold, and a second
 * window's load must never write under the first window's draws.
 *
 * FNL_GL_CORE is core in 4.1, and loaded on every context: the calls both
 * profiles make, and the bind-to-edit calls the 4.1 profile's stand-ins are
 * made of (gl_compat.c). FNL_GL_45 is what 4.5 adds that the renderer is
 * written against -- direct state access, immutable storage, the base
 * instance, glClipControl -- loaded on a 4.5 context and, on a 4.1 one,
 * filled with gl_compat.c's stand-ins, so that every call site is the same in
 * both. FNL_GL_OPT may be missing on either: NULL where the context has
 * neither the version nor the extension, and every caller asks first.
 */

#ifndef FNL_GL_FUNCS_H
#define FNL_GL_FUNCS_H

#include <GL/glcorearb.h>

#define FNL_GL_CORE(X)                                                         \
	X(PFNGLGETSTRINGPROC, GetString)                                           \
	X(PFNGLGETSTRINGIPROC, GetStringi)                                         \
	X(PFNGLGETINTEGERVPROC, GetIntegerv)                                       \
	X(PFNGLGETERRORPROC, GetError)                                             \
	X(PFNGLENABLEPROC, Enable)                                                 \
	X(PFNGLDISABLEPROC, Disable)                                               \
	X(PFNGLVIEWPORTPROC, Viewport)                                             \
	X(PFNGLSCISSORPROC, Scissor)                                               \
	X(PFNGLBLENDFUNCSEPARATEPROC, BlendFuncSeparate)                           \
	X(PFNGLBLENDCOLORPROC, BlendColor)                                         \
	X(PFNGLBLENDEQUATIONPROC, BlendEquation)                                   \
	X(PFNGLCOLORMASKPROC, ColorMask)                                           \
	X(PFNGLDEPTHMASKPROC, DepthMask)                                           \
	X(PFNGLPIXELSTOREIPROC, PixelStorei)                                       \
	X(PFNGLREADPIXELSPROC, ReadPixels)                                         \
	X(PFNGLFINISHPROC, Finish)                                                 \
	X(PFNGLFLUSHPROC, Flush)                                                   \
	X(PFNGLGENBUFFERSPROC, GenBuffers)                                         \
	X(PFNGLBUFFERDATAPROC, BufferData)                                         \
	X(PFNGLBUFFERSUBDATAPROC, BufferSubData)                                   \
	X(PFNGLMAPBUFFERRANGEPROC, MapBufferRange)                                 \
	X(PFNGLUNMAPBUFFERPROC, UnmapBuffer)                                       \
	X(PFNGLDELETEBUFFERSPROC, DeleteBuffers)                                   \
	X(PFNGLBINDBUFFERBASEPROC, BindBufferBase)                                 \
	X(PFNGLBINDBUFFERPROC, BindBuffer)                                         \
	X(PFNGLPOLYGONMODEPROC, PolygonMode)                                       \
	X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays)                               \
	X(PFNGLDELETEVERTEXARRAYSPROC, DeleteVertexArrays)                         \
	X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray)                               \
	X(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer)                       \
	X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray)               \
	X(PFNGLVERTEXATTRIBDIVISORPROC, VertexAttribDivisor)                       \
	X(PFNGLGENTEXTURESPROC, GenTextures)                                       \
	X(PFNGLBINDTEXTUREPROC, BindTexture)                                       \
	X(PFNGLACTIVETEXTUREPROC, ActiveTexture)                                   \
	X(PFNGLTEXIMAGE2DPROC, TexImage2D)                                         \
	X(PFNGLTEXSUBIMAGE2DPROC, TexSubImage2D)                                   \
	X(PFNGLTEXPARAMETERIPROC, TexParameteri)                                   \
	X(PFNGLDELETETEXTURESPROC, DeleteTextures)                                 \
	X(PFNGLGENSAMPLERSPROC, GenSamplers)                                       \
	X(PFNGLDELETESAMPLERSPROC, DeleteSamplers)                                 \
	X(PFNGLSAMPLERPARAMETERIPROC, SamplerParameteri)                           \
	X(PFNGLBINDSAMPLERPROC, BindSampler)                                       \
	X(PFNGLGENRENDERBUFFERSPROC, GenRenderbuffers)                             \
	X(PFNGLBINDRENDERBUFFERPROC, BindRenderbuffer)                             \
	X(PFNGLRENDERBUFFERSTORAGEPROC, RenderbufferStorage)                       \
	X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, RenderbufferStorageMultisample) \
	X(PFNGLDELETERENDERBUFFERSPROC, DeleteRenderbuffers)                       \
	X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers)                               \
	X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers)                         \
	X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer)                               \
	X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D)                     \
	X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, FramebufferRenderbuffer)               \
	X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus)                 \
	X(PFNGLBLITFRAMEBUFFERPROC, BlitFramebuffer)                               \
	X(PFNGLCLEARBUFFERFVPROC, ClearBufferfv)                                   \
	X(PFNGLCREATESHADERPROC, CreateShader)                                     \
	X(PFNGLSHADERSOURCEPROC, ShaderSource)                                     \
	X(PFNGLCOMPILESHADERPROC, CompileShader)                                   \
	X(PFNGLGETSHADERIVPROC, GetShaderiv)                                       \
	X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog)                             \
	X(PFNGLDELETESHADERPROC, DeleteShader)                                     \
	X(PFNGLCREATEPROGRAMPROC, CreateProgram)                                   \
	X(PFNGLATTACHSHADERPROC, AttachShader)                                     \
	X(PFNGLDETACHSHADERPROC, DetachShader)                                     \
	X(PFNGLLINKPROGRAMPROC, LinkProgram)                                       \
	X(PFNGLGETPROGRAMIVPROC, GetProgramiv)                                     \
	X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog)                           \
	X(PFNGLDELETEPROGRAMPROC, DeleteProgram)                                   \
	X(PFNGLUSEPROGRAMPROC, UseProgram)                                         \
	X(PFNGLGETUNIFORMBLOCKINDEXPROC, GetUniformBlockIndex)                     \
	X(PFNGLUNIFORMBLOCKBINDINGPROC, UniformBlockBinding)                       \
	X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation)                         \
	X(PFNGLUNIFORM1IPROC, Uniform1i)                                           \
	X(PFNGLDRAWARRAYSPROC, DrawArrays)                                         \
	X(PFNGLDRAWARRAYSINSTANCEDPROC, DrawArraysInstanced)                       \
	X(PFNGLFENCESYNCPROC, FenceSync)                                           \
	X(PFNGLCLIENTWAITSYNCPROC, ClientWaitSync)                                 \
	X(PFNGLGENQUERIESPROC, GenQueries)                                         \
	X(PFNGLDELETEQUERIESPROC, DeleteQueries)                                   \
	X(PFNGLQUERYCOUNTERPROC, QueryCounter)                                     \
	X(PFNGLGETQUERYOBJECTIVPROC, GetQueryObjectiv)                             \
	X(PFNGLGETQUERYOBJECTUI64VPROC, GetQueryObjectui64v)                       \
	X(PFNGLDELETESYNCPROC, DeleteSync)

#define FNL_GL_45(X)                                                           \
	X(PFNGLCLIPCONTROLPROC, ClipControl)                                       \
	X(PFNGLCREATEBUFFERSPROC, CreateBuffers)                                   \
	X(PFNGLNAMEDBUFFERSTORAGEPROC, NamedBufferStorage)                         \
	X(PFNGLMAPNAMEDBUFFERRANGEPROC, MapNamedBufferRange)                       \
	X(PFNGLUNMAPNAMEDBUFFERPROC, UnmapNamedBuffer)                             \
	X(PFNGLNAMEDBUFFERSUBDATAPROC, NamedBufferSubData)                         \
	X(PFNGLCREATEVERTEXARRAYSPROC, CreateVertexArrays)                         \
	X(PFNGLVERTEXARRAYVERTEXBUFFERPROC, VertexArrayVertexBuffer)               \
	X(PFNGLVERTEXARRAYATTRIBFORMATPROC, VertexArrayAttribFormat)               \
	X(PFNGLVERTEXARRAYATTRIBBINDINGPROC, VertexArrayAttribBinding)             \
	X(PFNGLENABLEVERTEXARRAYATTRIBPROC, EnableVertexArrayAttrib)               \
	X(PFNGLVERTEXARRAYBINDINGDIVISORPROC, VertexArrayBindingDivisor)           \
	X(PFNGLCREATETEXTURESPROC, CreateTextures)                                 \
	X(PFNGLTEXTURESTORAGE2DPROC, TextureStorage2D)                             \
	X(PFNGLTEXTURESUBIMAGE2DPROC, TextureSubImage2D)                           \
	X(PFNGLBINDTEXTUREUNITPROC, BindTextureUnit)                               \
	X(PFNGLCREATESAMPLERSPROC, CreateSamplers)                                 \
	X(PFNGLCREATERENDERBUFFERSPROC, CreateRenderbuffers)                       \
	X(PFNGLNAMEDRENDERBUFFERSTORAGEPROC, NamedRenderbufferStorage)             \
	X(PFNGLNAMEDRENDERBUFFERSTORAGEMULTISAMPLEPROC,                            \
	  NamedRenderbufferStorageMultisample)                                     \
	X(PFNGLCREATEFRAMEBUFFERSPROC, CreateFramebuffers)                         \
	X(PFNGLNAMEDFRAMEBUFFERTEXTUREPROC, NamedFramebufferTexture)               \
	X(PFNGLNAMEDFRAMEBUFFERRENDERBUFFERPROC, NamedFramebufferRenderbuffer)     \
	X(PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC, CheckNamedFramebufferStatus)       \
	X(PFNGLBLITNAMEDFRAMEBUFFERPROC, BlitNamedFramebuffer)                     \
	X(PFNGLCLEARNAMEDFRAMEBUFFERFVPROC, ClearNamedFramebufferfv)               \
	X(PFNGLDRAWARRAYSINSTANCEDBASEINSTANCEPROC, DrawArraysInstancedBaseInstance)

#define FNL_GL_OPT(X)                                                          \
	X(PFNGLGETGRAPHICSRESETSTATUSPROC, GetGraphicsResetStatus)                 \
	X(PFNGLDEBUGMESSAGECALLBACKPROC, DebugMessageCallback)

#define FNL_GL_FUNCS(X) FNL_GL_CORE(X) FNL_GL_45(X) FNL_GL_OPT(X)

typedef struct {
#define FNL_GL_MEMBER(type, name) type name;
	FNL_GL_FUNCS(FNL_GL_MEMBER)
#undef FNL_GL_MEMBER
} GlFuncs;

#endif /* FNL_GL_FUNCS_H */

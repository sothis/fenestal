/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * The textured cube as OpenGL draws it: a texture, a sampler, one program
 * and a uniform block. The shaders are scene_vulkan.c's GLSL, compiled to
 * SPIR-V and translated back by spirv-cross at build time: to GLSL 4.50,
 * and to 4.10 for a context that is 4.1 core where the device makes no 4.5
 * (fnl_gl_version()), as a Mac's is, which maps depth into GL's range
 * itself. The calls are 4.1's alone, which a 4.5 context runs too.
 *
 * Three answers here are GL's, where scene_vulkan.c has Vulkan's:
 *
 *   one program for all      GL has no render-pass compatibility, so one
 *                            program draws into the window, a smaller image
 *                            or a multisampled one alike.
 *   per-draw data is a       the push-constant block becomes a std140
 *   uniform block            uniform block, written before the draw.
 *   state is said each time  the context has one state machine, shared with
 *                            the library's own drawing, so everything the
 *                            draw relies on is set on every call. GL keeps
 *                            no recording: this runs for every frame.
 *
 * cube.vert makes the cube from gl_VertexID, so the vertex array is empty --
 * but a core context draws nothing without one bound.
 */

#include "texture.h"

#include <fenestal/fnl_gl.h>

#include <GL/glcorearb.h>

#include <stdio.h>
#include <string.h>

#include "cube.vert.gl.h"
#include "cube.frag.gl.h"
#include "cube.vert.gl41.h"
#include "cube.frag.gl41.h"

#define CUBE_VERTEX_COUNT 36

/* The entry points this file calls, loaded through fnl_gl_proc() at every
 * on_gpu_created: a loss may land on another driver. */
#define SCENE_GL(X)                                                           \
	X(PFNGLCREATESHADERPROC,             CreateShader)                        \
	X(PFNGLSHADERSOURCEPROC,             ShaderSource)                        \
	X(PFNGLCOMPILESHADERPROC,            CompileShader)                       \
	X(PFNGLGETSHADERIVPROC,              GetShaderiv)                         \
	X(PFNGLGETSHADERINFOLOGPROC,         GetShaderInfoLog)                    \
	X(PFNGLDELETESHADERPROC,             DeleteShader)                        \
	X(PFNGLCREATEPROGRAMPROC,            CreateProgram)                       \
	X(PFNGLATTACHSHADERPROC,             AttachShader)                        \
	X(PFNGLLINKPROGRAMPROC,              LinkProgram)                         \
	X(PFNGLGETPROGRAMIVPROC,             GetProgramiv)                        \
	X(PFNGLGETPROGRAMINFOLOGPROC,        GetProgramInfoLog)                   \
	X(PFNGLDELETEPROGRAMPROC,            DeleteProgram)                       \
	X(PFNGLUSEPROGRAMPROC,               UseProgram)                          \
	X(PFNGLGETUNIFORMBLOCKINDEXPROC,     GetUniformBlockIndex)                \
	X(PFNGLUNIFORMBLOCKBINDINGPROC,      UniformBlockBinding)                 \
	X(PFNGLGENBUFFERSPROC,               GenBuffers)                          \
	X(PFNGLBINDBUFFERPROC,               BindBuffer)                          \
	X(PFNGLBUFFERDATAPROC,               BufferData)                          \
	X(PFNGLBUFFERSUBDATAPROC,            BufferSubData)                       \
	X(PFNGLBINDBUFFERBASEPROC,           BindBufferBase)                      \
	X(PFNGLDELETEBUFFERSPROC,            DeleteBuffers)                       \
	X(PFNGLGENVERTEXARRAYSPROC,          GenVertexArrays)                     \
	X(PFNGLBINDVERTEXARRAYPROC,          BindVertexArray)                     \
	X(PFNGLDELETEVERTEXARRAYSPROC,       DeleteVertexArrays)                  \
	X(PFNGLGENTEXTURESPROC,              GenTextures)                         \
	X(PFNGLACTIVETEXTUREPROC,            ActiveTexture)                       \
	X(PFNGLBINDTEXTUREPROC,              BindTexture)                         \
	X(PFNGLTEXIMAGE2DPROC,               TexImage2D)                          \
	X(PFNGLTEXPARAMETERIPROC,            TexParameteri)                       \
	X(PFNGLDELETETEXTURESPROC,           DeleteTextures)                      \
	X(PFNGLGENSAMPLERSPROC,              GenSamplers)                         \
	X(PFNGLSAMPLERPARAMETERIPROC,        SamplerParameteri)                   \
	X(PFNGLBINDSAMPLERPROC,              BindSampler)                         \
	X(PFNGLDELETESAMPLERSPROC,           DeleteSamplers)                      \
	X(PFNGLENABLEPROC,                   Enable)                              \
	X(PFNGLDISABLEPROC,                  Disable)                             \
	X(PFNGLDEPTHFUNCPROC,                DepthFunc)                           \
	X(PFNGLDEPTHMASKPROC,                DepthMask)                           \
	X(PFNGLCULLFACEPROC,                 CullFace)                            \
	X(PFNGLFRONTFACEPROC,                FrontFace)                           \
	X(PFNGLDRAWARRAYSPROC,               DrawArrays)

/* One device generation: built in on_gpu_created, deleted in on_gpu_lost.
 * One struct so a loss clears it in one statement. No lock: every callback
 * runs on a thread the library owns, never two at once, with the context
 * current. */
static struct {
#define SCENE_MEMBER(type, name) type name;
	SCENE_GL(SCENE_MEMBER)
#undef SCENE_MEMBER
	bool   loaded;      /* every entry point above is there */
	bool   v41;         /* a 4.1 context: the 4.10 translation */
	GLuint program;     /* 0 where it did not build: nothing is drawn */
	GLuint vao;         /* empty, and bound to draw */
	GLuint push;
	GLuint tex;
	GLuint sampler;
	bool   srgb[FNL_TARGET_COUNT];
} g_dev;

/* The point the `Push` block is bound to. The library binds its own buffer
 * to point 0 before every draw of its own, so any point will do; 1 keeps
 * the two apart for anyone reading a GL trace. */
#define PUSH_BINDING 1

/* The block's size as std140 lays it out: CubePush's 84 bytes rounded up to
 * a multiple of a vec4, which is what the driver checks the bound buffer
 * against. */
#define PUSH_BYTES   ((sizeof(CubePush) + 15) / 16 * 16)

/* The texture unit the sampler is declared at (binding = 0 in the shader);
 * and 0 too in the 4.10 translation, which has no binding qualifiers, since
 * a sampler uniform left unset reads unit 0. */
#define TEX_UNIT     0

static GLuint compile(GLenum stage, const char *src)
{
	GLuint sh = g_dev.CreateShader(stage);
	g_dev.ShaderSource(sh, 1, &src, NULL);
	g_dev.CompileShader(sh);
	GLint ok = GL_FALSE;
	g_dev.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		g_dev.GetShaderInfoLog(sh, (GLsizei)sizeof log, NULL, log);
		fprintf(stderr, "a shader did not compile: %s\n", log);
		g_dev.DeleteShader(sh);
		return 0;
	}
	return sh;
}

static GLuint make_program(void)
{
	GLuint vs = compile(GL_VERTEX_SHADER,
						g_dev.v41 ? cube_vert_glsl41 : cube_vert_glsl);
	GLuint fs = compile(GL_FRAGMENT_SHADER,
						g_dev.v41 ? cube_frag_glsl41 : cube_frag_glsl);
	GLuint p = 0;
	if (vs && fs) {
		p = g_dev.CreateProgram();
		g_dev.AttachShader(p, vs);
		g_dev.AttachShader(p, fs);
		g_dev.LinkProgram(p);
		GLint ok = GL_FALSE;
		g_dev.GetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok) {
			char log[512];
			g_dev.GetProgramInfoLog(p, (GLsizei)sizeof log, NULL, log);
			fprintf(stderr, "the program did not link: %s\n", log);
			g_dev.DeleteProgram(p);
			p = 0;
		}
	}
	/* Deleting a name of 0 does nothing, and an attached shader lives on
	 * inside the program. */
	g_dev.DeleteShader(vs);
	g_dev.DeleteShader(fs);
	if (p) {
		const GLuint block = g_dev.GetUniformBlockIndex(p, "Push");
		if (block != GL_INVALID_INDEX)
			g_dev.UniformBlockBinding(p, block, PUSH_BINDING);
	}
	return p;
}

/* GL_SRGB8_ALPHA8, because the bytes are colours as a picker gives them:
 * sampling then returns linear values, which is what the shader lights.
 * Linear both ways and clamped at the edges, as scene_vulkan.c's sampler
 * is: the frame round the checkerboard is the face's edge. */
static void make_texture(void)
{
	static unsigned char px[TEX_SIZE * TEX_SIZE * 4];
	texture_pixels(px);

	g_dev.GenTextures(1, &g_dev.tex);
	g_dev.ActiveTexture(GL_TEXTURE0 + TEX_UNIT);
	g_dev.BindTexture(GL_TEXTURE_2D, g_dev.tex);
	/* One level, said, so the texture is complete without mipmaps. Rows of
	 * four-byte pixels are packed under any unpack alignment, so none is
	 * set; an unpack buffer left bound would be read in the pixels'
	 * place. */
	g_dev.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	g_dev.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	g_dev.TexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, TEX_SIZE, TEX_SIZE, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, px);

	g_dev.GenSamplers(1, &g_dev.sampler);
	g_dev.SamplerParameteri(g_dev.sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	g_dev.SamplerParameteri(g_dev.sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	g_dev.SamplerParameteri(g_dev.sampler, GL_TEXTURE_WRAP_S,
							GL_CLAMP_TO_EDGE);
	g_dev.SamplerParameteri(g_dev.sampler, GL_TEXTURE_WRAP_T,
							GL_CLAMP_TO_EDGE);
}

/* Everything the device owns, deleted. Deleting a name of 0 does nothing,
 * which is what lets one function serve both an ordinary loss and a build
 * that stopped halfway. */
void scene_gl_on_gpu_lost(void *ud, FnlApp *app)
{
	(void)ud;
	(void)app;
	if (!g_dev.loaded)
		return;
	g_dev.DeleteProgram(g_dev.program);
	g_dev.DeleteVertexArrays(1, &g_dev.vao);
	g_dev.DeleteBuffers(1, &g_dev.push);
	g_dev.DeleteTextures(1, &g_dev.tex);
	g_dev.DeleteSamplers(1, &g_dev.sampler);
	memset(&g_dev, 0, sizeof g_dev);
}

/* Called once at startup and again after every device loss, with a context
 * that shares nothing with the last one. A failure leaves the window open
 * and the scene black. */
void scene_gl_on_gpu_created(void *ud, FnlApp *app)
{
	(void)ud;
	memset(&g_dev, 0, sizeof g_dev);
	if (fnl_renderer(app) != FNL_RENDERER_OPENGL) {
		fprintf(stderr, "the running renderer is not OpenGL\n");
		return;
	}
#define SCENE_LOAD(type, name)                                                \
	if (!(g_dev.name = (type)fnl_gl_proc(app, "gl" #name))) {                 \
		fprintf(stderr, "the driver has no gl" #name "\n");                   \
		return;                                                               \
	}
	SCENE_GL(SCENE_LOAD)
#undef SCENE_LOAD
	g_dev.loaded = true;
	g_dev.v41 = fnl_gl_version(app) == 41;

	/* Whether each target encodes linear to sRGB as fragments are written,
	 * which decides whether the shader must. Read off the format, as
	 * scene_vulkan.c reads it off Vulkan's. */
	for (int i = 0; i < FNL_TARGET_COUNT; i++) {
		FnlGlSceneLayout layout;
		if (fnl_gl_scene_layout(app, (FnlSceneTarget)i, &layout))
			g_dev.srgb[i] = layout.color_format == GL_SRGB8_ALPHA8;
	}

	g_dev.GenVertexArrays(1, &g_dev.vao);
	make_texture();
	g_dev.GenBuffers(1, &g_dev.push);
	g_dev.BindBuffer(GL_UNIFORM_BUFFER, g_dev.push);
	g_dev.BufferData(GL_UNIFORM_BUFFER, PUSH_BYTES, NULL, GL_DYNAMIC_DRAW);
	g_dev.program = make_program();
}

/* The cube, into the framebuffer the library bound -- cleared, with the
 * viewport and scissor set. Everything else is whatever the last frame
 * left, the library's drawing included, so it is all said here. */
void scene_gl_draw(FnlApp *app, const FnlFrame *f, CubePush *push)
{
	uint32_t fbo;
	if (!g_dev.program || !fnl_gl_scene_framebuffer(app, &fbo))
		return;
	if (f->target < 0 || f->target >= FNL_TARGET_COUNT)
		return;

	g_dev.Enable(GL_DEPTH_TEST);
	g_dev.DepthFunc(GL_LESS);
	g_dev.DepthMask(GL_TRUE);
	g_dev.Disable(GL_BLEND);
	/* Clockwise, where scene_vulkan.c says counter-clockwise for the same
	 * shader: the library keeps Vulkan's row order, so the projection's
	 * negated y is not undone by the framebuffer as it is under Vulkan, and
	 * GL measures the winding the other way round (fnl_gl.h). */
	g_dev.Enable(GL_CULL_FACE);
	g_dev.CullFace(GL_BACK);
	g_dev.FrontFace(GL_CW);

	g_dev.UseProgram(g_dev.program);
	g_dev.BindVertexArray(g_dev.vao);
	g_dev.ActiveTexture(GL_TEXTURE0 + TEX_UNIT);
	g_dev.BindTexture(GL_TEXTURE_2D, g_dev.tex);
	g_dev.BindSampler(TEX_UNIT, g_dev.sampler);
	/* Bound to its point, which binds the generic target too: the block is
	 * written through that. */
	g_dev.BindBufferBase(GL_UNIFORM_BUFFER, PUSH_BINDING, g_dev.push);

	push->srgb = g_dev.srgb[f->target] ? 1u : 0u;
	g_dev.BufferSubData(GL_UNIFORM_BUFFER, 0, sizeof *push, push);
	g_dev.DrawArrays(GL_TRIANGLES, 0, CUBE_VERTEX_COUNT);
}
